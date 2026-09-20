// The SD card, in a browser.
//
// src/native/storage/sd_models.cpp mounts an SDMMC card, walks /sdcard/nam with
// dirent and reads what it finds with stdio. None of that exists here, and none
// of it needs to: the page asks the player for a folder, reads it, and hands the
// bytes over. What the firmware calls -- pedalboard_sd_models_scan() and
// pedalboard_sd_model_read() -- is answered from that snapshot, so the
// catalogue, the browse screen and the loader are the board's own code running
// unchanged. A folder of .nam files IS the card as far as the pedal is
// concerned, and a real card in a reader is simply one folder you might pick.
//
// The rules the device applies are mirrored deliberately, down to the accepted
// sizes, the shape of the id and the name of the cache file, because the point
// is that the two are the same card. A capture prepared here writes the same
// .s3cache the board would have written, next to the same file; drop that folder
// on a card and the board loads it without preparing it again. A preset naming a
// capture means the same capture on both.
//
// Writes leave through pedal_sd_write(): the page owns them, because the folder
// handle is the page's and because IndexedDB -- which is where they go when the
// browser will not give a page a writable folder -- cannot be reached from
// synchronous C++ at all. Nothing here waits for a write. A cache that fails to
// persist costs the next load its preparation time and nothing else.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

#include "audio/processor.hpp"
#include "audio/usb_frame_processor.h"
#include "model_catalog.h"
#include "nam_json.h"
#include "nvs.h"
#include "panel_presets.hpp"
#include "sd_models.h"

// board_bridge.cpp. The panel is repainted from the same place the board repaints
// it, rather than from a second path this file would have to keep in step.
extern "C" void coyopedal_ui_catalog_changed(bool reveal);
extern "C" void coyopedal_ui_touch(void);

// clang-format off
// The page's side of a write: the path relative to the picked folder and the
// bytes, copied out of the heap before this returns because the heap is not the
// page's to hold on to. Where it lands -- the folder, IndexedDB, both, neither
// -- is index.html's business.
EM_JS(void, pedal_sd_write, (const char* path, const std::uint8_t* data, int size), {
    if (typeof globalThis.pedalSdWrite !== 'function')
        return;
    globalThis.pedalSdWrite(UTF8ToString(path), HEAPU8.slice(data, data + size));
})

// The "SD card" row, tapped with nothing behind it. The page owns the picker --
// it is the page that has a folder handle, an IndexedDB and a browser whose
// rules about when a picker may open are not C++'s to know -- so this only asks,
// and the captures arrive later through pedal_sd_reset/add/mount the way they do
// on a board when a card is pushed in.
EM_JS(void, pedal_sd_pick, (), {
    if (typeof globalThis.pedalSdOpen === 'function')
        globalThis.pedalSdOpen();
})

EM_JS_DEPS(pedal_sd_models, "$UTF8ToString")

// clang-format on

namespace {

using Model = coyopedal::pedal::Processor::Model;

// One file out of the picked folder. `path` is relative to the folder the player
// chose, which is exactly what the device keeps relative to /nam -- so the
// browse screen shows their own directory tree, with their own folder names.
struct Entry {
    std::string path;
    std::vector<std::uint8_t> bytes;
};

std::vector<Entry> g_card;

Entry* find(const char* const path) {
    for (Entry& entry : g_card) {
        if (entry.path == path) {
            return &entry;
        }
    }
    return nullptr;
}

// Kept in the snapshot as well as sent to the page, so a profile loaded twice in
// one session is prepared once even if the folder is read-only.
void store_file(const std::string& path, const std::uint8_t* const data, const std::size_t size) {
    Entry* const existing = find(path.c_str());
    Entry& entry = existing ? *existing : (g_card.push_back(Entry{path, {}}), g_card.back());
    entry.bytes.assign(data, data + size);
    pedal_sd_write(path.c_str(), data, static_cast<int>(size));
}

// esp_crc32_le(0, ...) is the standard reflected CRC-32 -- polynomial 0xEDB88320,
// both ends inverted, zlib's. It is reproduced rather than approximated because
// the id it produces is what a saved preset stores to name its capture: a
// different hash here would mean a preset written in the browser could not find
// its profile on the board, which is the one thing this file exists to avoid.
std::uint32_t crc32(const void* const data, const std::size_t length) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
            }
            table[index] = value;
        }
        ready = true;
    }
    const auto* const bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < length; ++index) {
        crc = table[(crc ^ bytes[index]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

bool ends_with(const std::string& text, const char* const suffix) {
    const std::size_t length = std::strlen(suffix);
    return text.size() > length && strcasecmp(text.c_str() + text.size() - length, suffix) == 0;
}

// sd_models.cpp's add_model, to the letter: a hash of the path for an id that is
// stable and fits its field, the file name without its extension for a display
// name, and the PREPARED size for a JSON capture because that is what the reader
// below produces from one.
void add_model(const std::string& relative, const bool json, const std::size_t size) {
    if (coyopedal_model_count >= COYOPEDAL_MODEL_MAX) {
        return;
    }
    char id[COYOPEDAL_MODEL_ID_MAX];
    std::snprintf(id, sizeof id, "sd-%08lx",
                  static_cast<unsigned long>(crc32(relative.data(), relative.size())));
    for (unsigned index = 0; index < coyopedal_model_count; ++index) {
        if (std::strcmp(id, coyopedal_models[index].id) == 0) {
            return;
        }
    }
    coyopedal_model_t& model = coyopedal_models[coyopedal_model_count];
    model = {};
    std::snprintf(model.id, sizeof model.id, "%s", id);

    const std::size_t slash = relative.find_last_of('/');
    const std::size_t base = slash == std::string::npos ? 0U : slash + 1U;
    const std::size_t stem = relative.size() - base - (json ? 4U : 5U);
    std::memcpy(model.name, relative.data() + base, std::min(stem, sizeof model.name - 1U));
    std::snprintf(model.sd_filename, sizeof model.sd_filename, "%s", relative.c_str());
    model.size = static_cast<unsigned>(json ? Model::kPreparedFileSize : size);
    model.sd_model = true;
    model.sd_json = json;
    ++coyopedal_model_count;
}

// sd_models.cpp's cached(): a prepared image is accepted only if the capture it
// was prepared from is byte for byte the capture being loaded now. Not the file
// name, not a timestamp, not a hash -- the whole 48,616 bytes, because the cost
// of being wrong is an engine full of weights that are not the ones on screen.
bool cached(const std::string& path, std::uint8_t* const out) {
    const Entry* const entry = find(path.c_str());
    if (!entry || entry->bytes.size() != Model::kPreparedFileSize ||
        std::memcmp(entry->bytes.data(), out, Model::kFileSize) != 0) {
        return false;
    }
    std::memcpy(out + Model::kFileSize, entry->bytes.data() + Model::kFileSize,
                Model::kPreparedTrailerSize);
    return Model::validate_prepared(out, Model::kPreparedFileSize);
}

// The player's presets, mirrored into the picked folder so they travel with the
// captures they name. The same document assets/presets.json is, written by the
// same code the board writes its card with -- so a folder filled here drops onto
// a card, and a preset file off a card opens in a text editor.
constexpr char kPresetsFile[] = "coyopedal-presets.json";
bool importing_presets = false;

} // namespace

// --- what the page hands over ------------------------------------------------

extern "C" {

// Empties the card. The page calls this before reading a folder in, so picking a
// second folder replaces the first rather than merging into it -- one card at a
// time, the way a slot works.
EMSCRIPTEN_KEEPALIVE void pedal_sd_reset(void) {
    g_card.clear();
}

// One file off the folder. The page filters by name before calling, and the scan
// below filters again: this is also how a .s3cache and the preset file get in,
// and neither of those is a capture.
EMSCRIPTEN_KEEPALIVE void pedal_sd_add(const char* const path, const std::uint8_t* const bytes,
                                       const int size) {
    if (path == nullptr || path[0] == '\0' || size < 0 || std::strstr(path, "..") != nullptr) {
        return;
    }
    Entry entry;
    entry.path = path;
    entry.bytes.assign(bytes, bytes + size);
    g_card.push_back(std::move(entry));
}

// The card is in. Rebuilding the catalogue is coyopedal_models_init()'s job and it
// starts by zeroing the count, so this is a replacement rather than an append and
// can be done as often as the player picks a folder.
EMSCRIPTEN_KEEPALIVE int pedal_sd_mount(const int reveal) {
    coyopedal_ui_catalog_changed(reveal != 0);
    unsigned found = 0;
    for (unsigned index = 0; index < coyopedal_model_count; ++index) {
        found += coyopedal_models[index].sd_model ? 1U : 0U;
    }
    return static_cast<int>(found);
}

// Presets the page found in the folder. They are written through the same store
// the panel saves to, so what is restored is indistinguishable from what was
// saved here -- and the mirror below is held off while it happens, or importing
// would immediately write the same bytes back out again.
EMSCRIPTEN_KEEPALIVE int pedal_sd_presets_import(const std::uint8_t* const data, const int size) {
    if (data == nullptr || size <= 0) {
        return 0;
    }
    // Held off while it happens, or the import would immediately mirror the same
    // presets back out over the file they came from.
    importing_presets = true;
    const bool ok = panel_presets::from_json(reinterpret_cast<const char*>(data),
                                             static_cast<std::size_t>(size));
    importing_presets = false;
    coyopedal_ui_touch();
    return ok ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int pedal_sd_file_count(void) {
    return static_cast<int>(g_card.size());
}

// panel_presets.cpp calls this whenever the list changes -- save, rename, add,
// erase, the remembered selection -- without knowing there is a folder. Nothing
// happens before a folder is picked: there is nowhere to put it, and the list is
// in the browser's own storage regardless.
void pedalboard_presets_mirror(const char* const text, const std::size_t length) {
    if (importing_presets || g_card.empty()) {
        return;
    }
    store_file(kPresetsFile, reinterpret_cast<const std::uint8_t*>(text), length);
}

// The card row's three answers in a browser. The "slot" is a folder picker, so
// the row that would rescan a card asks for a folder instead -- and asks for one
// again from inside the card, which since the page around the panel stopped
// offering a button is the only way to change it.
bool pedalboard_sd_open(void) {
    pedal_sd_pick();
    // Nothing has happened yet and the panel has nothing new to draw; what the
    // player picks lands through pedal_sd_mount() whenever the picker settles.
    return true;
}

// Short because a row is 211 points wide and the name has to fit beside it: the
// subtitle is the hint, not the instructions.
const char* pedalboard_sd_hint(void) {
    return "pick a folder";
}

const char* pedalboard_sd_action(void) {
    return "Pick another folder";
}

// --- what the firmware calls -------------------------------------------------

void pedalboard_sd_models_scan(void) {
    const unsigned first = coyopedal_model_count;
    for (const Entry& entry : g_card) {
        const bool json = ends_with(entry.path, ".nam");
        const bool binary = ends_with(entry.path, ".namb");
        if (!json && !binary) {
            continue;
        }
        const std::size_t size = entry.bytes.size();
        // The device's own limits: a JSON capture up to 2 MiB, and a prepared one
        // at exactly one of the two sizes the engine writes.
        if (json ? (size == 0U || size > 2U * 1024U * 1024U)
                 : (size != Model::kFileSize && size != Model::kPreparedFileSize)) {
            continue;
        }
        if (entry.path.size() >= sizeof(coyopedal_models[0].sd_filename)) {
            continue;
        }
        add_model(entry.path, json, size);
    }
    std::sort(coyopedal_models + first, coyopedal_models + coyopedal_model_count,
              [](const coyopedal_model_t& a, const coyopedal_model_t& b) {
                  return std::strcmp(a.sd_filename, b.sd_filename) < 0;
              });
}

bool pedalboard_sd_model_read(const coyopedal_model_t* const model, unsigned char* const out,
                              const std::size_t capacity, char* const error,
                              const std::size_t error_capacity) {
    const auto fail = [&](const char* const message) {
        if (error != nullptr && error_capacity != 0U) {
            std::snprintf(error, error_capacity, "%s", message);
        }
        return false;
    };
    if (model == nullptr || out == nullptr || capacity < Model::kPreparedFileSize ||
        model->sd_filename[0] == '\0') {
        return fail("Invalid SD model entry");
    }
    const Entry* const entry = find(model->sd_filename);
    if (entry == nullptr) {
        return fail("That file is not in the folder any more");
    }

    if (!model->sd_json) {
        if (entry->bytes.size() != model->size) {
            return fail("Profile changed; pick the folder again");
        }
        std::memcpy(out, entry->bytes.data(), model->size);
        return true;
    }

    // Parsing and tuning rewrite the whole weight set, so they are serialised
    // against the audio thread exactly as the board serialises them -- outside
    // the callbacks, with the live model surviving a parser or tuner failure.
    if (!coyopedal_pedal_dsp_begin_update()) {
        return fail("Audio pipeline busy");
    }
    struct Resume {
        ~Resume() {
            coyopedal_pedal_dsp_end_update();
        }
    } resume;

    if (!pedalboard_parse_nam(reinterpret_cast<const char*>(entry->bytes.data()),
                              entry->bytes.size(), out, capacity, error, error_capacity)) {
        return false;
    }

    const std::string cache = std::string(model->sd_filename) + ".s3cache";
    if (cached(cache, out)) {
        return true;
    }

    // A minute of arithmetic on a phone, and the panel is frozen for all of it:
    // the tuner runs 96 trials over the whole network looking for the fixed-point
    // scaling the S3 will use. It is done here rather than skipped because the
    // browser has to produce the same bytes the board would, and it is cached
    // because nobody should pay for it twice.
    Model::TuningOptions options;
    options.max_trials = 96;
    options.probe_gain = 1.0F;
    options.target_peak = 0.001;
    Model::TuningReport report;
    if (!Model::prepare_tuned(out, Model::kFileSize, out + Model::kFileSize, options, report, error,
                              error_capacity) ||
        !Model::validate_prepared(out, Model::kPreparedFileSize)) {
        return false;
    }
    store_file(cache, out, Model::kPreparedFileSize);
    return true;
}

} // extern "C"
