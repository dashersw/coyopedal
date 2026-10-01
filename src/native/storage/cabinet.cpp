#include "audio/cabinet.h"
#include "audio/cabinet_ir.hpp"
#include "audio/ir_wav.hpp"
#include "audio/usb_frame_processor.h"
#include "sd_models.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <iterator>
#include <sys/stat.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

extern "C" bool pedalboard_nam_residual_check();

#ifdef __EMSCRIPTEN__
// The web manifest mounts these same WAVs in Emscripten's filesystem.
#define FACTORY_CABINET(path, name) {path, nullptr, nullptr}
#else
extern "C" const unsigned char kFactoryV30Start[] asm("_binary_factory_cab_v30_wav_start");
extern "C" const unsigned char kFactoryV30End[] asm("_binary_factory_cab_v30_wav_end");
extern "C" const unsigned char kFactoryDV77Start[] asm("_binary_factory_cab_dv77_wav_start");
extern "C" const unsigned char kFactoryDV77End[] asm("_binary_factory_cab_dv77_wav_end");
extern "C" const unsigned char
    kFactoryRockdriverStart[] asm("_binary_factory_cab_rockdriver_wav_start");
extern "C" const unsigned char
    kFactoryRockdriverEnd[] asm("_binary_factory_cab_rockdriver_wav_end");

#define FACTORY_CABINET(path, name) {path, kFactory##name##Start, kFactory##name##End}
#endif

namespace {
struct FactoryCabinet {
    const char* path;
    const unsigned char* start;
    const unsigned char* end;
};
constexpr FactoryCabinet factory[] = {
    FACTORY_CABINET("Factory/V30 SM57.wav", V30),
    FACTORY_CABINET("Factory/DV77 SM57.wav", DV77),
    FACTORY_CABINET("Factory/Rockdriver e606.wav", Rockdriver),
};
#undef FACTORY_CABINET
const FactoryCabinet* factory_cabinet(const char* path) {
    for (const auto& entry : factory)
        if (std::strcmp(path, entry.path) == 0)
            return &entry;
    return nullptr;
}
coyopedal::pedal::CabinetIR cabinet;
EXT_RAM_BSS_ATTR coyopedal_cabinet_setting_t current;
std::atomic<bool> enabled{};
bool capture_includes_cabinet{};
float gain = 1.0F;
constexpr unsigned kCapacity = 64;
struct IRPath {
    char text[COYOPEDAL_IR_PATH_MAX];
};
IRPath* paths{};
unsigned count{};
bool fail(char* error, size_t capacity, const char* reason) {
    if (error && capacity)
        std::snprintf(error, capacity, "%s", reason);
    return false;
}
bool valid_path(const char* path) {
    const size_t n = strnlen(path, COYOPEDAL_IR_PATH_MAX);
    if (!n || n == COYOPEDAL_IR_PATH_MAX || path[0] == '/' || std::strchr(path, '\\'))
        return false;
    const char* component = path;
    for (const char* p = path;; ++p) {
        if (*p == '/' || !*p) {
            const size_t length = p - component;
            if (!length || (length == 1 && component[0] == '.') ||
                (length == 2 && component[0] == '.' && component[1] == '.'))
                return false;
            if (!*p)
                break;
            component = p + 1;
        } else if (static_cast<unsigned char>(*p) < 32)
            return false;
    }
    return true;
}
void scan_directory(char* relative, size_t length, unsigned depth) {
    char path[COYOPEDAL_IR_PATH_MAX + 16];
    std::snprintf(path, sizeof path, "/sdcard/ir%s%s", length ? "/" : "", relative);
    DIR* dir = opendir(path);
    if (!dir)
        return;
    const size_t base = length ? length + 1 : 0;
    while (auto* entry = readdir(dir)) {
        const size_t n = std::strlen(entry->d_name);
        if (entry->d_name[0] == '.' || base + n >= COYOPEDAL_IR_PATH_MAX || count == kCapacity)
            continue;
        if (length)
            relative[length] = '/';
        std::memcpy(relative + base, entry->d_name, n + 1);
        std::snprintf(path, sizeof path, "/sdcard/ir/%s", relative);
        struct stat st{};
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode) && depth < 6)
                scan_directory(relative, base + n, depth + 1);
            else if (S_ISREG(st.st_mode) && n > 4 && !strcasecmp(entry->d_name + n - 4, ".wav") &&
                     !factory_cabinet(relative))
                std::memcpy(paths[count++].text, relative, base + n + 1);
        }
        relative[length] = 0;
    }
    closedir(dir);
}
struct Resume {
    ~Resume() {
        coyopedal_pedal_dsp_end_update();
    }
};
bool load_cabinet(const float* taps, unsigned frames) {
    const bool ok = cabinet.load(taps, frames);
#ifdef __EMSCRIPTEN__
    if (!ok)
        ESP_LOGE("cabinet", "IR allocation failed");
#else
    if (!ok)
        ESP_LOGE("cabinet", "IR allocation failed: PSRAM free=%u largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#endif
    return ok;
}
} // namespace

extern "C" void pedalboard_cabinet_setting(coyopedal_cabinet_setting_t* out) {
    *out = current;
}
extern "C" IRAM_ATTR bool pedalboard_cabinet_enabled() {
    return enabled.load(std::memory_order_relaxed);
}
extern "C" void pedalboard_cabinet_capture_changed(bool includes_cabinet) {
    capture_includes_cabinet = includes_cabinet;
    const bool on = current.enabled && !includes_cabinet;
    if (on != enabled.load(std::memory_order_relaxed)) {
        cabinet.reset();
        cabinet.cache_residency(on);
        enabled.store(on, std::memory_order_relaxed);
    }
}
extern "C" void pedalboard_cabinet_timing(unsigned* cycles) {
    cabinet.timing(cycles);
}
extern "C" IRAM_ATTR void pedalboard_cabinet_process(float* samples, size_t frames) {
    if (frames == coyopedal::pedal::CabinetIR::kFrames)
        cabinet.process(samples, gain);
}
extern "C" IRAM_ATTR unsigned pedalboard_cabinet_begin(float* samples, float* imaginary) {
    return cabinet.begin(samples, imaginary);
}
extern "C" IRAM_ATTR void pedalboard_cabinet_finish(float* samples, float* imaginary,
                                                    unsigned cursor) {
    cabinet.finish(samples, imaginary, cursor, gain);
}

extern "C" bool pedalboard_cabinet_apply(const coyopedal_cabinet_setting_t* setting, char* error,
                                         size_t capacity) {
    if (!setting || !std::memchr(setting->path, 0, sizeof setting->path) ||
        (setting->path[0] && !valid_path(setting->path)) ||
        (setting->enabled && !setting->path[0]) || setting->level < -180 || setting->level > 60)
        return fail(error, capacity, "Invalid cabinet setting");
    const bool on = setting->enabled && !capture_includes_cabinet;
    const bool replace = std::strcmp(current.path, setting->path) != 0;
    if (!replace && current.enabled == setting->enabled && current.level == setting->level &&
        enabled.load(std::memory_order_relaxed) == on)
        return true;
    float* taps = nullptr;
    unsigned frames = 0;
    if (replace && setting->path[0]) {
        FILE* file;
        if (const auto* entry = factory_cabinet(setting->path)) {
#ifdef __EMSCRIPTEN__
            file = std::fopen(entry->path, "rb");
#else
            file =
                fmemopen(const_cast<unsigned char*>(entry->start), entry->end - entry->start, "rb");
#endif
        } else {
            if (!pedalboard_sd_ready())
                return fail(error, capacity, "Insert an SD card with /ir WAVs");
            char path[COYOPEDAL_IR_PATH_MAX + 16];
            std::snprintf(path, sizeof path, "/sdcard/ir/%s", setting->path);
            file = std::fopen(path, "rb");
        }
        if (!file)
            return fail(error, capacity, "Cabinet IR unavailable");
        taps = static_cast<float*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        const bool ok = taps && pedalboard_ir_read_wav(file, taps, frames, error, capacity);
        std::fclose(file);
        if (!ok) {
            if (!taps)
                fail(error, capacity, "Not enough PSRAM for IR");
            heap_caps_free(taps);
            return false;
        }
    }
    if (!coyopedal_pedal_dsp_begin_update()) {
        heap_caps_free(taps);
        return fail(error, capacity, "Audio pipeline busy");
    }
    Resume resume;
    bool ok = true;
    if (replace) {
        if (setting->path[0])
            ok = load_cabinet(taps, frames);
        else
            cabinet.release();
    } else if (on != enabled.load(std::memory_order_relaxed))
        cabinet.reset();
    heap_caps_free(taps);
    if (!ok)
        return fail(error, capacity, "Not enough memory for cabinet IR");
    current = *setting;
    gain = std::pow(10.0F, current.level / 200.0F);
    cabinet.cache_residency(on);
    enabled.store(on, std::memory_order_relaxed);
    if (replace)
        ESP_LOGI("cabinet", "IR %s: %u taps, %u B PSRAM / 0 B DRAM",
                 current.path[0] ? current.path : "bypassed", frames,
                 current.path[0] ? (coyopedal::pedal::CabinetIR::kResidentBytes + 63) & ~63U : 0);
    return true;
}

extern "C" bool pedalboard_cabinet_enable(bool on, char* error, size_t capacity) {
    auto setting = current;
    setting.enabled = on;
    return pedalboard_cabinet_apply(&setting, error, capacity);
}
extern "C" void pedalboard_cabinet_scan() {
    count = 0;
    if (!paths)
        paths = static_cast<IRPath*>(heap_caps_calloc(kCapacity, COYOPEDAL_IR_PATH_MAX,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!paths)
        return;
    for (const auto& entry : factory)
        std::snprintf(paths[count++].text, COYOPEDAL_IR_PATH_MAX, "%s", entry.path);
    if (!pedalboard_sd_ready())
        return;
    char relative[COYOPEDAL_IR_PATH_MAX]{};
    scan_directory(relative, 0, 0);
    std::sort(paths + std::size(factory), paths + count,
              [](const auto& a, const auto& b) { return std::strcmp(a.text, b.text) < 0; });
}
extern "C" unsigned pedalboard_cabinet_count() {
    return count;
}
extern "C" const char* pedalboard_cabinet_path(unsigned i) {
    return i < count ? paths[i].text : "";
}
extern "C" bool pedalboard_cabinet_benchmark(char* error, size_t capacity) {
    auto* taps = static_cast<float*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!taps)
        return fail(error, capacity, "No IR benchmark memory");
    for (unsigned i = 0; i < 1024; ++i)
        taps[i] =
            (i == 0 ? 0.5F
                    : 0.01F * std::sin(i * 0.73F) * std::exp(-static_cast<float>(i) / 200.0F));
    if (!coyopedal_pedal_dsp_begin_update()) {
        heap_caps_free(taps);
        return fail(error, capacity, "Audio pipeline busy");
    }
    Resume resume;
    if (!pedalboard_nam_residual_check()) {
        heap_caps_free(taps);
        return fail(error, capacity, "NAM residual integer check failed");
    }
    const bool ok = load_cabinet(taps, 1024);
    if (ok) {
        // Validate the actual S3 assembly, not just the host fallback. This
        // bounded diagnostic runs with DSP drained before the measured window.
        float maximum_error = 0;
        for (unsigned base = 0; base < 1024; base += 64) {
            float block[64]{};
            if (base == 0)
                block[0] = 1;
            cabinet.process(block, 1);
            for (unsigned i = 0; i < 64; ++i) {
                if (!std::isfinite(block[i]) || std::fabs(block[i] - taps[base + i]) > 2e-6F) {
                    heap_caps_free(taps);
                    cabinet.reset();
                    return fail(error, capacity, "IR convolution numerical check failed");
                }
                maximum_error = std::max(maximum_error, std::fabs(block[i] - taps[base + i]));
            }
        }
        cabinet.reset();
        ESP_LOGI("cabinet", "S3 1024-tap impulse check: max error=%g", double(maximum_error));
    }
    heap_caps_free(taps);
    if (!ok)
        return fail(error, capacity, "No IR benchmark memory");
    current = {};
    current.enabled = true;
    gain = 1.0F;
    enabled.store(true, std::memory_order_relaxed);
    ESP_LOGI("cabinet", "1024-tap dense IR benchmark enabled");
    return true;
}
