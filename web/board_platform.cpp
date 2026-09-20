// The board under the panel, in a browser.
//
// Everything the panel reads and writes -- the presets, the amp values, the
// model library, the dirty flag the save prompt asks about -- is the board's own
// code: src/native/ui/board_bridge.cpp, src/native/ui/controls.c and
// src/native/storage/{model_catalog.c,factory_presets.c,panel_presets.cpp},
// compiled unmodified. This file is what those sources stand on when the thing
// underneath them is a page rather than an ESP32.
//
// There are exactly four such things, and each is answered with the browser's
// own version rather than a model of the original:
//
//   the two factory images   the bytes the board is flashed with, fetched by
//                            the page and handed over here
//   the key-value store      the player's own presets, in localStorage, so they
//                            survive a reload the way they survive a power cycle
//   the clock                the page's monotonic clock
//   the SD card and radio    absent, and they say so
//
// Nothing here invents a preset, a profile or a name. If an image has not been
// installed the catalogue reads as empty, which is what an unprogrammed board
// looks like, and the panel says so in the words it already has.

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "flash_storage.h"
#include "remote_service.h"
#include "board_ui.h"
#include "factory_presets.h"
#include "model_catalog.h"
#include "pedal_audio.hpp"
#include "sd_models.h"

namespace {

// The two factory files the board is flashed with: the packed model library and
// the preset document, which is assets/presets.json itself. The page fetches
// them and hands them over before the panel initialises; until then both are
// empty.
std::vector<std::uint8_t> g_models;
std::vector<std::uint8_t> g_presets;
// The user-model window. The board fills it by importing a capture over the
// network; a page has no such path yet, so it stays empty and the catalogue
// simply finds nothing there.
std::vector<std::uint8_t> g_user_models;

bool window_read(const std::vector<std::uint8_t>& image, const std::uint32_t offset,
                 std::uint8_t* const out, const std::uint32_t count) {
    if (out == nullptr || offset + count > image.size()) {
        return false;
    }
    std::memcpy(out, image.data() + offset, count);
    return true;
}

} // namespace

// --- the flash store -------------------------------------------------------
//
// The same flat address space drivers/flash_storage.cpp carves, mapped onto the
// three images above instead of onto three ESP-IDF data partitions. The windows
// are the header's, not this file's: a reader that knew the offsets would be a
// second authority on the layout.

extern "C" bool coyopedal_flash_store_init(void) {
    return !g_models.empty() || !g_presets.empty();
}

extern "C" bool coyopedal_flash_store_read(const std::uint32_t address, std::uint8_t* const out,
                                           const std::uint32_t count) {
    if (address < COYOPEDAL_FLASH_PRESETS_BASE) {
        return window_read(g_models, address, out, count);
    }
    if (address >= COYOPEDAL_FLASH_USER_MODELS_BASE &&
        address < COYOPEDAL_FLASH_USER_MODELS_BASE + COYOPEDAL_FLASH_USER_MODELS_SIZE) {
        return window_read(g_user_models, address - COYOPEDAL_FLASH_USER_MODELS_BASE, out, count);
    }
    if (address >= COYOPEDAL_FLASH_PRESETS_BASE && address < COYOPEDAL_FLASH_PRESETS_END) {
        return window_read(g_presets, address - COYOPEDAL_FLASH_PRESETS_BASE, out, count);
    }
    return false;
}

// The preset document, which on the board is a partition read whole and here is
// simply the file the page fetched. No window and no address: it has no header
// to find a length in, which is the point of it being text.
extern "C" bool coyopedal_flash_presets_read(char* const out, const std::uint32_t capacity,
                                             std::uint32_t* const length) {
    if (out == nullptr || length == nullptr || g_presets.empty()) {
        return false;
    }
    const std::size_t count = std::min<std::size_t>(capacity, g_presets.size());
    std::memcpy(out, g_presets.data(), count);
    *length = static_cast<std::uint32_t>(count);
    return true;
}

// On the board this reads the copy embedded in the application image rather
// than the partition, so a stale partition can be repaired. Here there is one
// copy of the library and this is it.
extern "C" bool coyopedal_factory_models_read(const std::uint32_t offset, std::uint8_t* const out,
                                              const std::uint32_t count) {
    return window_read(g_models, offset, out, count);
}

// --- the heap --------------------------------------------------------------

extern "C" void* heap_caps_malloc(const std::size_t size, unsigned) {
    return std::malloc(size);
}

extern "C" void* heap_caps_calloc(const std::size_t count, const std::size_t size, unsigned) {
    return std::calloc(count, size);
}

extern "C" void heap_caps_free(void* const pointer) {
    std::free(pointer);
}

// --- the clock and the scheduler -------------------------------------------

extern "C" std::int64_t esp_timer_get_time(void) {
    return static_cast<std::int64_t>(emscripten_get_now() * 1000.0);
}

extern "C" void vTaskDelay(TickType_t) {}

extern "C" int xPortGetCoreID() {
    return 0;
}

// The board wakes the render task so a change is drawn without waiting for the
// next tick. The page drives the panel from requestAnimationFrame, which is
// already the next tick.
void s3_v1_ui_wake() {}

// --- the key-value store ---------------------------------------------------
//
// One localStorage entry per key, hex-encoded, under the namespace the board
// opens ("panel_presets"). Hex rather than base64 because the store is a few
// kilobytes of struct and the encoder has to be exact: a blob that comes back
// one byte short fails panel_presets.cpp's own validation and the player's
// presets are silently replaced by the factory list.

// clang-format off
EM_JS(char*, board_kv_read, (const char* key), {
    const name = UTF8ToString(key);
    let text = null;
    try {
        text = globalThis.localStorage.getItem(name);
    } catch (error) {
        text = null;
    }
    if (text === null)
        return 0;
    const bytes = lengthBytesUTF8(text) + 1;
    const buffer = _malloc(bytes);
    stringToUTF8(text, buffer, bytes);
    return buffer;
})

EM_JS(void, board_kv_write, (const char* key, const char* value), {
    try {
        globalThis.localStorage.setItem(UTF8ToString(key), UTF8ToString(value));
    } catch (error) {
        // A private window refuses storage. The presets then live for as long
        // as the page does, which is the honest outcome and not a failure the
        // panel can do anything about.
    }
})
// clang-format on

EM_JS_DEPS(pedal_board_platform, "$lengthBytesUTF8,$stringToUTF8,$UTF8ToString")

namespace {

std::string storage_key(const std::string& space, const char* const key) {
    return "pedalboard/" + space + "/" + key;
}

std::string to_hex(const void* const data, const std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    const auto* const octets = static_cast<const unsigned char*>(data);
    std::string text(size * 2, '0');
    for (std::size_t index = 0; index < size; ++index) {
        text[index * 2] = digits[octets[index] >> 4];
        text[index * 2 + 1] = digits[octets[index] & 0x0F];
    }
    return text;
}

bool from_hex(const std::string& text, void* const out, const std::size_t size) {
    if (text.size() != size * 2) {
        return false;
    }
    auto* const octets = static_cast<unsigned char*>(out);
    for (std::size_t index = 0; index < size; ++index) {
        const auto nibble = [](const char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        octets[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

// The namespace a handle was opened under. There is one store and no
// concurrency, so a handle is an index into this rather than anything richer.
std::vector<std::string> g_namespaces;

bool read_text(const std::string& key, std::string& out) {
    char* const value = board_kv_read(key.c_str());
    if (value == nullptr) {
        return false;
    }
    out.assign(value);
    std::free(value);
    return true;
}

} // namespace

extern "C" esp_err_t nvs_open(const char* const name, nvs_open_mode_t, nvs_handle_t* const out) {
    if (out == nullptr) {
        return ESP_FAIL;
    }
    g_namespaces.emplace_back(name == nullptr ? "" : name);
    *out = static_cast<nvs_handle_t>(g_namespaces.size());
    return ESP_OK;
}

extern "C" void nvs_close(const nvs_handle_t) {}

// Every write is committed as it is made: localStorage has no transaction to
// close, so there is nothing here to flush.
extern "C" esp_err_t nvs_commit(const nvs_handle_t) {
    return ESP_OK;
}

namespace {

const std::string* space_of(const nvs_handle_t handle) {
    return handle == 0 || handle > g_namespaces.size() ? nullptr : &g_namespaces[handle - 1];
}

} // namespace

extern "C" esp_err_t nvs_get_blob(const nvs_handle_t handle, const char* const key, void* const out,
                                  std::size_t* const size) {
    const std::string* const space = space_of(handle);
    if (space == nullptr || size == nullptr) {
        return ESP_FAIL;
    }
    std::string text;
    if (!read_text(storage_key(*space, key), text)) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    // The size-query call the board makes before it allocates.
    if (out == nullptr) {
        *size = text.size() / 2;
        return ESP_OK;
    }
    if (text.size() / 2 > *size) {
        return ESP_ERR_INVALID_SIZE;
    }
    *size = text.size() / 2;
    return from_hex(text, out, *size) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t nvs_set_blob(const nvs_handle_t handle, const char* const key,
                                  const void* const value, const std::size_t size) {
    const std::string* const space = space_of(handle);
    if (space == nullptr || value == nullptr) {
        return ESP_FAIL;
    }
    board_kv_write(storage_key(*space, key).c_str(), to_hex(value, size).c_str());
    return ESP_OK;
}

extern "C" esp_err_t nvs_get_u32(const nvs_handle_t handle, const char* const key,
                                 std::uint32_t* const out) {
    const std::string* const space = space_of(handle);
    if (space == nullptr || out == nullptr) {
        return ESP_FAIL;
    }
    std::string text;
    if (!read_text(storage_key(*space, key), text)) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return from_hex(text, out, sizeof *out) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t nvs_set_u32(const nvs_handle_t handle, const char* const key,
                                 const std::uint32_t value) {
    return nvs_set_blob(handle, key, &value, sizeof value);
}

// --- the card and the radio ------------------------------------------------
//
// The card is a folder the player picks: web/sd_models_web.cpp answers the
// catalogue's two card questions out of what the page read from it. Until one is
// picked the snapshot is empty, which the catalogue already understands -- that
// is a board with no card in the slot.

// Captures imported over the maintenance network, which live in the user-model
// window. Nothing has written to that window here, so the catalogue finds the
// factory library and stops -- the same as a board nobody has imported to.
extern "C" void coyopedal_s3_read_imported_models(void) {}

// The maintenance network. "Not busy, in audio mode" is what the panel needs to
// hear to behave as a pedal rather than as a board mid-transition, and a page is
// never anything else. C++ linkage, because remote_service.h declares these
// without an extern "C" and the panel calls them by the name that header gives.
void coyopedal_remote_capture_logs() {}
void coyopedal_remote_prepare_runtime() {}
bool coyopedal_remote_start() {
    return false;
}
void coyopedal_remote_sync_network_ui() {}
bool coyopedal_remote_maintenance_boot_requested() {
    return false;
}
void coyopedal_remote_arm_audio_window() {}
void coyopedal_remote_audio_window_configure() {}
bool coyopedal_remote_audio_boot_requested() {
    return true;
}
void coyopedal_remote_mark_audio_boot() {}
void coyopedal_remote_validate_running_ota() {}
void coyopedal_remote_prepare_requested_diagnostics() {}
void coyopedal_remote_start_requested_diagnostics() {}
void coyopedal_remote_mode_start() {}
bool coyopedal_remote_mode_busy() {
    return false;
}
void coyopedal_remote_toggle_mode() {}
bool coyopedal_remote_audio_mode() {
    return true;
}

// --- installing the images -------------------------------------------------

extern "C" {

// The board's power-on, given the two factory images the page fetched. Called
// once, before the UI mounts, with the same bytes the board is flashed with.
//
// The sequence is task.cpp's initialize_ui(), in its order and for its reasons:
// the store has to answer before the catalogue reads its index, the catalogue
// has to be there before the preset table names profiles in it, and the panel
// has to find both before it recalls anything. The engine's memory comes first
// of all -- on the board main.cpp has already allocated it by this point --
// because recalling a preset loads a profile into it.
//
// Returns the count of models and presets found, packed as models * 1000 +
// presets, so the page can say what the board came up with rather than only
// whether it came up.
EMSCRIPTEN_KEEPALIVE int pedal_board_boot(const std::uint8_t* const models, const int models_size,
                                          const std::uint8_t* const presets,
                                          const int presets_size) {
    g_models.assign(models, models + (models_size < 0 ? 0 : models_size));
    g_presets.assign(presets, presets + (presets_size < 0 ? 0 : presets_size));
    pedal_audio::init();
    (void)coyopedal_flash_store_init();
    (void)coyopedal_models_init();
    (void)coyopedal_presets_init();
    if (!coyopedal_ui_init()) {
        return -1;
    }
    // The board's USB is what tells the panel a host is connected. Here the
    // host is the page, and it is connected by definition.
    coyopedal_ui_set_usb(COYOPEDAL_UI_USB_STREAMING);
    return static_cast<int>(coyopedal_model_count) * 1000 +
           static_cast<int>(coyopedal_preset_count());
}

} // extern "C"
