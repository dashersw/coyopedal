#include "sd_models.h"
#include "nam_json.h"
#include "audio/processor.hpp"
#include "usb_audio.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/sdmmc_host.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

extern coyopedal::pedal::Processor g_engine;
namespace {
using Model = coyopedal::pedal::Processor::Model;
sdmmc_card_t* card{};
bool attempted{};
constexpr const char* directory = "/sdcard/nam";
struct Free {
    void operator()(void* p) const {
        heap_caps_free(p);
    }
};
bool mount() {
    if (attempted)
        return card != nullptr;
    attempted = true;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_2;
    slot.cmd = GPIO_NUM_1;
    slot.d0 = GPIO_NUM_3;
    slot.d1 = slot.d2 = slot.d3 = slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
    slot.cd = slot.wp = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t config{};
    config.format_if_mount_failed = false;
    config.max_files = 2;
    const auto result = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &config, &card);
    if (result != ESP_OK) {
        card = nullptr;
        ESP_LOGW("sd_models", "SD unavailable: %s", esp_err_to_name(result));
    }
    return card != nullptr;
}
bool cached(const char* path, std::uint8_t* out) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    std::array<std::uint8_t, 512> chunk{};
    bool ok = true;
    // Compare the entire source model, not just its filename or modification time.
    for (size_t offset = 0; ok && offset < Model::kFileSize; offset += chunk.size()) {
        const auto n = std::min(chunk.size(), Model::kFileSize - offset);
        ok = fread(chunk.data(), 1, n, f) == n && std::memcmp(out + offset, chunk.data(), n) == 0;
    }
    if (ok)
        ok = fread(out + Model::kFileSize, 1, Model::kPreparedTrailerSize, f) ==
                 Model::kPreparedTrailerSize &&
             fgetc(f) == EOF && !ferror(f);
    fclose(f);
    return ok && Model::validate_prepared(out, Model::kPreparedFileSize);
}
void save_cache(const char* path, const std::uint8_t* data) {
    char temp[224];
    std::snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE* f = fopen(temp, "wb");
    if (!f)
        return;
    bool ok = fwrite(data, 1, Model::kPreparedFileSize, f) == Model::kPreparedFileSize;
    ok = fflush(f) == 0 && ok;
    if (ok)
        ok = fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(temp, path) != 0) {
        unlink(temp);
        ESP_LOGW("sd_models", "Could not persist cache; loaded model remains usable");
    }
}
// A capture on the card. `relative` is its path under /nam, which is what the
// catalogue keeps: the browser shows the card's own folders.
void add_model(const char* const relative, const std::size_t length, const std::size_t base,
               const bool json, const off_t size) {
    // The id names the capture for presets, so it has to be stable and fit its
    // field. A hash of the path is both; moving a file on the card makes it a
    // different capture as far as a saved preset is concerned.
    const auto hash = esp_crc32_le(0, reinterpret_cast<const uint8_t*>(relative), length);
    char id[COYOPEDAL_MODEL_ID_MAX];
    std::snprintf(id, sizeof id, "sd-%08lx", static_cast<unsigned long>(hash));
    for (unsigned i = 0; i < coyopedal_model_count; ++i) {
        if (!std::strcmp(id, coyopedal_models[i].id)) {
            ESP_LOGW("sd_models", "Duplicate SD ID skipped: %s", relative);
            return;
        }
    }
    auto& model = coyopedal_models[coyopedal_model_count++];
    model = {};
    std::snprintf(model.id, sizeof model.id, "%s", id);
    // The display name is the file name without its extension.
    const std::size_t stem = length - base - (json ? 4U : 5U);
    std::memcpy(model.name, relative + base, std::min(stem, sizeof model.name - 1));
    std::memcpy(model.sd_filename, relative, length + 1);
    model.size = json ? Model::kPreparedFileSize : size;
    model.sd_model = true;
    model.sd_json = json;
}

// Walks one directory of the card. `relative` is a shared buffer holding the
// directory's path under /nam, and each child is appended to it in place, so a
// deep tree costs one path buffer rather than one per level.
constexpr int kMaxDepth = 6;
void scan_directory(char* const relative, const std::size_t length, const int depth) {
    char path[192];
    std::snprintf(path, sizeof path, "%s%s%s", directory, length ? "/" : "", relative);
    DIR* const dir = opendir(path);
    if (!dir) {
        if (depth == 0)
            ESP_LOGI("sd_models", "Copy .nam files into /nam on the SD card");
        return;
    }
    const std::size_t base = length ? length + 1U : 0U;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr && coyopedal_model_count < COYOPEDAL_MODEL_MAX) {
        const std::size_t len = std::strlen(entry->d_name);
        if (entry->d_name[0] == '.' || base + len >= sizeof(coyopedal_models[0].sd_filename))
            continue;
        if (length)
            relative[length] = '/';
        std::memcpy(relative + base, entry->d_name, len + 1);
        const std::size_t child = base + len;
        if (entry->d_type == DT_DIR) {
            if (depth < kMaxDepth)
                scan_directory(relative, child, depth + 1);
        } else {
            const bool json = len > 4 && strcasecmp(entry->d_name + len - 4, ".nam") == 0;
            const bool binary = len > 5 && strcasecmp(entry->d_name + len - 5, ".namb") == 0;
            std::snprintf(path, sizeof path, "%s/%s", directory, relative);
            struct stat st{};
            if ((json || binary) && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
                (json ? st.st_size <= 2 * 1024 * 1024
                      : (st.st_size == 48616 || st.st_size == 49748)))
                add_model(relative, child, base, json, st.st_size);
        }
        relative[length] = '\0';
    }
    closedir(dir);
}
} // namespace
// board_bridge.cpp. The catalogue and the browse screen are rebuilt from the
// same place a card being picked in the browser build rebuilds them.
extern "C" void coyopedal_ui_catalog_changed(bool reveal);

// The "SD card" row's three answers on a board with a real slot.
//
// The slot has no card-detect line wired, so a card put in after boot is
// invisible until something looks again -- and the one thing that knows a card
// was just pushed in is the player. Hence a row that asks: forget the failed
// mount, mount again, rescan. Unmounting first also makes this the way to swap
// cards without power-cycling the pedal, which is what the same row does in the
// browser build when it asks for a different folder.
extern "C" bool pedalboard_sd_open(void) {
    if (card != nullptr) {
        esp_vfs_fat_sdcard_unmount("/sdcard", card);
        card = nullptr;
    }
    attempted = false;
    const bool mounted = mount();
    coyopedal_ui_catalog_changed(mounted);
    return mounted;
}

// Short because a row is 211 points wide and the name has to fit beside it: the
// subtitle is the hint, not the instructions.
extern "C" const char* pedalboard_sd_hint(void) {
    return card == nullptr ? "no card" : "nothing in /nam";
}

extern "C" const char* pedalboard_sd_action(void) {
    return "Look again";
}

// The player's presets, mirrored onto the card beside the captures they name, as
// the same document assets/presets.json is. panel_presets.cpp calls this every
// time the list changes and does not look at the result: the list itself is in
// NVS, and a card that is absent, full or write-protected costs the copy and
// nothing else. What it is for is carrying presets and captures together -- pull
// the card, put it in another pedal or a folder in the browser, and both arrive.
extern "C" void pedalboard_presets_mirror(const char* const text, const size_t length) {
    if (text == nullptr || length == 0U || !mount()) {
        return;
    }
    char path[sizeof(coyopedal_models[0].sd_filename) + 32];
    std::snprintf(path, sizeof path, "%s/coyopedal-presets.json", directory);
    FILE* const f = fopen(path, "wb");
    if (f == nullptr) {
        return;
    }
    if (fwrite(text, 1, length, f) != length) {
        ESP_LOGW("sd_models", "preset mirror write failed");
    }
    fclose(f);
}

extern "C" void pedalboard_sd_models_scan() {
    // Boot scans factory presets before reserving the NAM history/coefficient
    // banks. Defer SD driver allocations to the later UI catalogue scan.
    if (!g_engine.model_loaded() || !mount())
        return;
    const unsigned first = coyopedal_model_count;
    char relative[sizeof(coyopedal_models[0].sd_filename)]{};
    scan_directory(relative, 0, 0);
    std::sort(
        coyopedal_models + first, coyopedal_models + coyopedal_model_count,
        [](const auto& a, const auto& b) { return std::strcmp(a.sd_filename, b.sd_filename) < 0; });
    ESP_LOGI("sd_models", "Found %u SD models", coyopedal_model_count - first);
}
extern "C" bool pedalboard_sd_model_read(const coyopedal_model_t* model, unsigned char* out,
                                         size_t capacity, char* error, size_t error_capacity) {
    auto fail = [&](const char* msg) {
        if (error && error_capacity)
            std::snprintf(error, error_capacity, "%s", msg);
        return false;
    };
    if (!model || !out || capacity < Model::kPreparedFileSize || !model->sd_filename[0] ||
        std::strstr(model->sd_filename, ".."))
        return fail("Invalid SD model entry");
    // Parsing, tuning and file IO are serialized with DSP replacement, outside
    // the audio callbacks. The live model survives parser/tuner failure.
    if (!usb_audio_begin_model_update())
        return fail("Audio pipeline busy");
    struct Resume {
        ~Resume() {
            usb_audio_end_model_update();
        }
    } resume;
    char path[192];
    std::snprintf(path, sizeof path, "%s/%s", directory, model->sd_filename);
    FILE* f = fopen(path, "rb");
    if (!f)
        return fail("SD card or model file unavailable");
    if (!model->sd_json) {
        const size_t n = fread(out, 1, model->size, f);
        const bool ok = n == model->size && fgetc(f) == EOF && !ferror(f);
        fclose(f);
        return ok || fail("SD model read failed");
    }
    struct stat st{};
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > 2 * 1024 * 1024) {
        fclose(f);
        return fail("NAM file exceeds 2 MiB limit");
    }
    std::unique_ptr<char, Free> json(
        static_cast<char*>(heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!json) {
        fclose(f);
        return fail("Not enough PSRAM for NAM JSON");
    }
    bool ok = fread(json.get(), 1, st.st_size, f) == static_cast<size_t>(st.st_size) &&
              fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!ok)
        return fail("SD NAM read failed");
    json.get()[st.st_size] = '\0';
    ok = pedalboard_parse_nam(json.get(), st.st_size, out, capacity, error, error_capacity);
    json.reset();
    if (!ok)
        return false;
    char cache[208];
    std::snprintf(cache, sizeof cache, "%s.s3cache", path);
    if (cached(cache, out))
        return true;
    ESP_LOGI("sd_models", "Preparing %s on the S3; audio paused", model->sd_filename);
    Model::TuningOptions options;
    options.max_trials = 96;
    options.probe_gain = 1.0F;
    options.target_peak = 0.001;
    Model::TuningReport report;
    if (!Model::prepare_tuned(out, Model::kFileSize, out + Model::kFileSize, options, report, error,
                              error_capacity) ||
        !Model::validate_prepared(out, Model::kPreparedFileSize))
        return false;
    save_cache(cache, out);
    return true;
}
