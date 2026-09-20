#include "panel_presets.hpp"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "factory_presets.h"
#include "preset_json.h"
#include "control.h"
#include "audio/usb_frame_processor.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
namespace panel_presets {
namespace {
constexpr unsigned capacity = 32;
// Raised whenever the factory presets change in a way a stored list cannot
// survive. Version 1 lists were seeded from factory presets whose captures no
// longer ship (5150 Sneap named sneap-5150-od808), so they are seeded again.
// Version 2 lists name volum-deli120-4-v30, the Fryette the library carried
// before the Ampete One replaced it: a stored preset whose amp is not in the
// library cannot be recalled at all ("Preset amp missing from SD"), so those
// lists are seeded again too.
constexpr uint32_t store_version = 3;
struct Sound {
    char profile[24];
    int16_t amp[6];
    int16_t params[COYOPEDAL_FX_BLOCK_COUNT][5];
    uint8_t enabled[COYOPEDAL_FX_BLOCK_COUNT];
    uint8_t amp_on;
    uint8_t reserved;
};
struct Preset {
    char name[24];
    Sound sound;
};
struct PresetStore {
    uint32_t version;
    uint32_t count;
    Preset presets[capacity];
};
// The card, or the folder standing in for one: src/native/storage/sd_models.cpp
// on the board, web/sd_models_web.cpp in the browser. Every way of changing a
// preset ends in store(), so mirroring there covers save, rename, add, erase and
// the remembered selection without any of them knowing there is a card.
extern "C" void pedalboard_presets_mirror(const char* text, size_t length);

PresetStore* store_{};
unsigned current{};
bool writable{};
char last_error[64]{};
bool fail(const char* message) {
    std::snprintf(last_error, sizeof last_error, "%s", message);
    return false;
}
void clear() {
    last_error[0] = 0;
}
PresetStore* allocate() {
    return static_cast<PresetStore*>(
        heap_caps_calloc(1, sizeof(PresetStore), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
bool valid_name(const char* name) {
    const size_t n = strnlen(name, 24);
    if (!n || n >= 24)
        return false;
    for (size_t i = 0; i < n; i++)
        if (name[i] < 32 || name[i] > 126)
            return false;
    return true;
}
bool valid(const PresetStore& value) {
    if (value.version != store_version || value.count == 0 || value.count > capacity)
        return false;
    for (unsigned i = 0; i < value.count; i++) {
        const auto& p = value.presets[i];
        if (!valid_name(p.name) || !memchr(p.sound.profile, 0, sizeof p.sound.profile) ||
            p.sound.amp_on > 1)
            return false;
        for (unsigned j = 0; j < 6; j++)
            if (p.sound.enabled[j] > 1)
                return false;
    }
    return true;
}
Sound capture() {
    Sound s{};
    coyopedal_control_state_t state{};
    coyopedal_ui_control_state(&state);
    if (state.model < coyopedal_model_count)
        std::snprintf(s.profile, sizeof s.profile, "%s", coyopedal_models[state.model].id);
    for (unsigned i = 0; i < 6; i++)
        s.amp[i] = state.amp[i];
    s.amp_on = state.block[COYOPEDAL_CONTROL_AMP_BLOCK].enabled;
    for (unsigned i = 0; i < COYOPEDAL_FX_BLOCK_COUNT; i++) {
        const auto block = static_cast<coyopedal_fx_block_t>(i);
        s.enabled[i] = coyopedal_fx_enabled(block);
        for (unsigned j = 0; j < coyopedal_fx_param_count(block) && j < 5; j++)
            s.params[i][j] = coyopedal_fx_param(block, j);
    }
    return s;
}
bool apply(const Sound& s) {
    unsigned model = 0;
    for (; model < coyopedal_model_count; model++)
        if (!std::strcmp(s.profile, coyopedal_models[model].id))
            break;
    if (model == coyopedal_model_count)
        return fail("Preset amp missing from SD");
    if (model != coyopedal_active_model() && !coyopedal_ui_control_set_model(model))
        return fail("Could not load preset amp");
    for (unsigned i = 0; i < 6; i++)
        if (!coyopedal_ui_control_set_param(COYOPEDAL_CONTROL_AMP_BLOCK, i, s.amp[i]))
            return fail("Could not restore amp settings");
    for (unsigned i = 0; i < COYOPEDAL_FX_BLOCK_COUNT; i++) {
        const auto block = static_cast<coyopedal_fx_block_t>(i);
        for (unsigned j = 0; j < coyopedal_fx_param_count(block) && j < 5; j++)
            if (!coyopedal_ui_control_set_param(block, j, s.params[i][j]))
                return fail("Could not restore effect settings");
        if (!coyopedal_ui_control_set_enabled(block, s.enabled[i]))
            return fail("Could not restore effect state");
    }
    return coyopedal_ui_control_set_enabled(COYOPEDAL_CONTROL_AMP_BLOCK, s.amp_on);
}
// A stored preset and a preset document's record are the same preset in two
// shapes: the store is a fixed struct because it goes into NVS, where 24 KB of
// text would not fit twice during a write, and the document is text because that
// is what travels. These two functions are the whole of the difference.
coyopedal_preset_record_t record_of(const Preset& preset) {
    coyopedal_preset_record_t out{};
    std::snprintf(out.name, sizeof out.name, "%s", preset.name);
    std::snprintf(out.profile, sizeof out.profile, "%s", preset.sound.profile);
    std::copy_n(preset.sound.amp, COYOPEDAL_PRESET_AMP, out.amp);
    out.amp_on = preset.sound.amp_on != 0;
    for (unsigned i = 0; i < COYOPEDAL_FX_BLOCK_COUNT; i++) {
        out.blocks[i].enabled = preset.sound.enabled[i] != 0;
        std::copy_n(preset.sound.params[i], COYOPEDAL_PRESET_PARAMS, out.blocks[i].params);
    }
    return out;
}

Preset preset_of(const coyopedal_preset_record_t& record) {
    Preset out{};
    std::snprintf(out.name, sizeof out.name, "%s", record.name);
    std::snprintf(out.sound.profile, sizeof out.sound.profile, "%s", record.profile);
    std::copy_n(record.amp, COYOPEDAL_PRESET_AMP, out.sound.amp);
    out.sound.amp_on = record.amp_on ? 1 : 0;
    for (unsigned i = 0; i < COYOPEDAL_FX_BLOCK_COUNT; i++) {
        out.sound.enabled[i] = record.blocks[i].enabled ? 1 : 0;
        std::copy_n(record.blocks[i].params, COYOPEDAL_PRESET_PARAMS, out.sound.params[i]);
    }
    return out;
}

// Writes the list out to wherever a card is. Best effort and silent: a folder
// that will not take a write costs the mirror and nothing else -- the list
// itself is in NVS, which is what the pedal reads at boot.
void mirror() {
    if (store_ == nullptr || store_->count == 0U)
        return;
    auto* const records = static_cast<coyopedal_preset_record_t*>(heap_caps_calloc(
        capacity, sizeof(coyopedal_preset_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    char* const text = static_cast<char*>(
        heap_caps_malloc(COYOPEDAL_PRESET_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (records != nullptr && text != nullptr) {
        for (unsigned i = 0; i < store_->count; i++)
            records[i] = record_of(store_->presets[i]);
        const size_t length =
            coyopedal_presets_to_json(records, store_->count, text, COYOPEDAL_PRESET_JSON_MAX);
        if (length != 0U)
            pedalboard_presets_mirror(text, length);
    }
    heap_caps_free(records);
    heap_caps_free(text);
}

bool store(PresetStore* next) {
    if (!next)
        return fail("Not enough preset memory");
    if (!writable) {
        heap_caps_free(next);
        return fail("Preset storage unavailable");
    }
    if (!coyopedal_pedal_dsp_begin_update()) {
        heap_caps_free(next);
        return fail("Audio busy; try saving again");
    }
    nvs_handle_t handle{};
    esp_err_t result = nvs_open("panel_presets", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "presets", next, sizeof(PresetStore));
        if (result == ESP_OK)
            result = nvs_commit(handle);
        nvs_close(handle);
    }
    coyopedal_pedal_dsp_end_update();
    if (result == ESP_OK)
        *store_ = *next;
    heap_caps_free(next);
    if (result != ESP_OK)
        return fail("Preset save failed; no changes saved");
    mirror();
    return true;
}
PresetStore* copy() {
    if (!store_) {
        fail("Preset storage unavailable");
        return nullptr;
    }
    auto* next = allocate();
    if (next)
        *next = *store_;
    else
        fail("Not enough preset memory");
    return next;
}
} // namespace
void init() {
    if (store_)
        heap_caps_free(store_);
    current = 0;
    writable = false;
    clear();
    store_ = allocate();
    if (!store_) {
        fail("Not enough preset memory");
        return;
    }
    nvs_handle_t handle{};
    auto result = nvs_open("panel_presets", NVS_READONLY, &handle);
    if (result == ESP_OK) {
        uint32_t remembered = 0;
        nvs_get_u32(handle, "active", &remembered);
        current = remembered;
        size_t size = sizeof(PresetStore);
        result = nvs_get_blob(handle, "presets", store_, &size);
        nvs_close(handle);
        if (result == ESP_OK && size == sizeof(PresetStore) && valid(*store_)) {
            current = std::min(current, unsigned(store_->count - 1));
            writable = true;
            return;
        }
        const bool outdated =
            result == ESP_OK && size == sizeof(PresetStore) && store_->version < store_version;
        if (outdated) {
            current = 0;
        } else if (result != ESP_ERR_NVS_NOT_FOUND) {
            *store_ = {};
            fail("Preset storage needs recovery");
            return;
        }
    } else if (result != ESP_ERR_NVS_NOT_FOUND) {
        fail("Preset storage unavailable");
        return;
    }
    // First run, or a list from an older store version: seed from the factory
    // presets. The seed is written on the next save; the presets partition
    // itself is left untouched.
    *store_ = {};
    store_->version = store_version;
    store_->count = std::min(capacity, coyopedal_preset_count());
    for (unsigned i = 0; i < store_->count; i++) {
        coyopedal_preset_record_t old{};
        coyopedal_preset_read(i, &old);
        auto& p = store_->presets[i];
        std::snprintf(p.name, sizeof p.name, "%s", old.name);
        std::snprintf(p.sound.profile, sizeof p.sound.profile, "%s", old.profile);
        p.sound.amp_on = old.amp_on ? 1 : 0;
        std::copy_n(old.amp, 6, p.sound.amp);
        for (unsigned j = 0; j < COYOPEDAL_FX_BLOCK_COUNT; j++) {
            p.sound.enabled[j] = old.blocks[j].enabled;
            std::copy_n(old.blocks[j].params, 5, p.sound.params[j]);
        }
    }
    if (!store_->count) {
        store_->count = 1;
        std::snprintf(store_->presets[0].name, 24, "My preset");
        store_->presets[0].sound = capture();
    }
    current = std::min(current, unsigned(store_->count - 1));
    writable = true;
}
unsigned count() {
    return store_ ? store_->count : 0;
}
unsigned active() {
    return current;
}
const char* name(unsigned index) {
    return store_ && index < store_->count ? store_->presets[index].name : "My preset";
}
const char* error() {
    return last_error;
}
bool edited() {
    if (!store_ || current >= store_->count)
        return false;
    const auto s = capture();
    return std::memcmp(&s, &store_->presets[current].sound, sizeof s) != 0;
}
bool load(unsigned index) {
    clear();
    if (!store_ || index >= store_->count)
        return fail("Preset not found");
    const Sound previous = capture();
    const bool engaged = !coyopedal_pedal_dsp_pedal_bypassed();
    coyopedal_ui_control_set_engaged(false);
    const bool ok = apply(store_->presets[index].sound);
    if (!ok) {
        char reason[64];
        std::snprintf(reason, sizeof reason, "%s", last_error);
        apply(previous);
        std::snprintf(last_error, sizeof last_error, "%s", reason);
    }
    coyopedal_ui_control_set_engaged(engaged);
    if (ok)
        current = index;
    return ok;
}
bool remember_active() {
    clear();
    nvs_handle_t handle{};
    auto result = nvs_open("panel_presets", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u32(handle, "active", current);
        if (result == ESP_OK)
            result = nvs_commit(handle);
        nvs_close(handle);
    }
    return result == ESP_OK ? true : fail("Could not remember current preset");
}
bool save() {
    clear();
    auto* next = copy();
    if (!next)
        return false;
    next->presets[current].sound = capture();
    return store(next);
}
bool rename(unsigned index, const char* name) {
    clear();
    if (index >= count() || !valid_name(name))
        return fail("Enter a preset name");
    auto* next = copy();
    if (!next)
        return false;
    std::snprintf(next->presets[index].name, 24, "%s", name);
    return store(next);
}
bool add(const char* name) {
    clear();
    if (!valid_name(name))
        return fail("Enter a preset name");
    if (count() >= capacity)
        return fail("Preset library full (32)");
    auto* next = copy();
    if (!next)
        return false;
    const auto index = next->count++;
    next->presets[index] = {};
    std::snprintf(next->presets[index].name, 24, "%s", name);
    next->presets[index].sound = capture();
    if (!store(next))
        return false;
    current = index;
    return true;
}
bool erase(unsigned index) {
    clear();
    if (count() <= 1)
        return fail("Keep at least one preset");
    if (index >= count())
        return fail("Preset not found");
    const unsigned previous = current;
    const Sound sound = capture();
    if (index == current && !load(index + 1 < count() ? index + 1 : index - 1))
        return false;
    auto* next = copy();
    if (!next) {
        current = previous;
        apply(sound);
        return false;
    }
    for (unsigned i = index + 1; i < next->count; i++)
        next->presets[i - 1] = next->presets[i];
    next->presets[--next->count] = {};
    if (!store(next)) {
        current = previous;
        apply(sound);
        return false;
    }
    if (current > index)
        --current;
    return true;
}

size_t json_capacity() {
    return COYOPEDAL_PRESET_JSON_MAX;
}

size_t to_json(char* const out, const size_t out_capacity) {
    if (store_ == nullptr || store_->count == 0U)
        return 0;
    auto* const records = static_cast<coyopedal_preset_record_t*>(heap_caps_calloc(
        capacity, sizeof(coyopedal_preset_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (records == nullptr) {
        fail("Not enough preset memory");
        return 0;
    }
    for (unsigned i = 0; i < store_->count; i++)
        records[i] = record_of(store_->presets[i]);
    const size_t length = coyopedal_presets_to_json(records, store_->count, out, out_capacity);
    heap_caps_free(records);
    return length;
}

bool from_json(const char* const text, const size_t length) {
    auto* const records = static_cast<coyopedal_preset_record_t*>(heap_caps_calloc(
        capacity, sizeof(coyopedal_preset_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (records == nullptr)
        return fail("Not enough preset memory");
    char reason[64]{};
    const unsigned found =
        coyopedal_presets_from_json(text, length, records, capacity, reason, sizeof reason);
    if (found == 0U) {
        heap_caps_free(records);
        // The parser's words, which name the line rather than saying "bad file":
        // this is a document a player may have edited by hand.
        return fail(reason[0] != '\0' ? reason : "No presets in that file");
    }
    auto* const next = allocate();
    if (next == nullptr) {
        heap_caps_free(records);
        return fail("Not enough preset memory");
    }
    next->version = store_version;
    next->count = found;
    for (unsigned i = 0; i < found; i++)
        next->presets[i] = preset_of(records[i]);
    heap_caps_free(records);
    if (!store(next))
        return false;
    current = std::min(current, found - 1U);
    clear();
    return true;
}
} // namespace panel_presets
