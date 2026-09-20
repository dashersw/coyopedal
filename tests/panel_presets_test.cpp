#include "panel_presets.hpp"
#include "factory_presets.h"
#include "control.h"
#include "audio/usb_frame_processor.h"
#include "nvs.h"
#include <cassert>
#include <vector>
#include <cstring>
#include <string>
#include "reverb_storage.hpp"
std::vector<unsigned char> saved;
// The card, which on a board is src/native/storage/sd_models.cpp and here is a
// string. Every successful store mirrors the list as the preset document, so
// this is also how the writer and the parser get exercised against each other.
std::string mirrored;
bool storage_failure{}, engaged = true, amp_on = true;
uint32_t remembered{};
unsigned model{};
int16_t gains[6]{};
extern "C" {
coyopedal_model_t coyopedal_models[COYOPEDAL_MODEL_MAX]{};
unsigned coyopedal_model_count = 2;
unsigned coyopedal_active_model() {
    return model;
}
unsigned coyopedal_preset_count() {
    return 0;
}
bool coyopedal_preset_read(unsigned, coyopedal_preset_record_t*) {
    return false;
}
void coyopedal_ui_control_state(coyopedal_control_state_t* out) {
    *out = {};
    out->model = model;
    for (int i = 0; i < 6; i++)
        out->amp[i] = gains[i];
    out->block[COYOPEDAL_CONTROL_AMP_BLOCK].enabled = amp_on;
}
bool coyopedal_ui_control_set_model(unsigned value) {
    if (value >= coyopedal_model_count)
        return false;
    model = value;
    amp_on = true;
    return true;
}
bool coyopedal_ui_control_set_param(uint8_t b, uint8_t p, int16_t value) {
    if (b == COYOPEDAL_CONTROL_AMP_BLOCK)
        gains[p] = value;
    else
        coyopedal_fx_set_param(static_cast<coyopedal_fx_block_t>(b), p, value);
    return true;
}
bool coyopedal_ui_control_set_enabled(uint8_t b, bool value) {
    if (b == COYOPEDAL_CONTROL_AMP_BLOCK)
        amp_on = value;
    else
        coyopedal_fx_set_enabled(static_cast<coyopedal_fx_block_t>(b), value);
    return true;
}
bool coyopedal_ui_control_set_engaged(bool value) {
    engaged = value;
    return true;
}
bool coyopedal_pedal_dsp_pedal_bypassed() {
    return !engaged;
}
bool coyopedal_pedal_dsp_begin_update() {
    return true;
}
void coyopedal_pedal_dsp_end_update() {}
}
esp_err_t nvs_open(const char*, int mode, nvs_handle_t* handle) {
    *handle = 1;
    return mode == NVS_READONLY && saved.empty() ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void* p, size_t* size) {
    if (saved.empty())
        return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= saved.size());
    std::memcpy(p, saved.data(), saved.size());
    *size = saved.size();
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void* p, size_t size) {
    if (storage_failure)
        return 2;
    const auto* bytes = static_cast<const unsigned char*>(p);
    saved.assign(bytes, bytes + size);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t* v) {
    *v = remembered;
    return ESP_OK;
}
extern "C" void pedalboard_presets_mirror(const char* text, size_t length) {
    mirrored.assign(text, length);
}
esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t v) {
    if (storage_failure)
        return 2;
    remembered = v;
    return ESP_OK;
}
int main() {
    std::strcpy(coyopedal_models[0].id, "factory");
    std::strcpy(coyopedal_models[1].id, "sd-model");
    attach_reverb_storage();
    coyopedal_fx_init();
    panel_presets::init();
    assert(panel_presets::count() == 1);
    assert(!panel_presets::edited());
    amp_on = false;
    model = 1;
    coyopedal_fx_set_enabled(COYOPEDAL_FX_DELAY, true);
    gains[0] = 12;
    assert(panel_presets::add("Lead"));
    assert(panel_presets::active() == 1);
    assert(!panel_presets::edited());
    assert(panel_presets::load(0));
    assert(amp_on && model == 0);
    assert(gains[0] == 0);
    assert(panel_presets::load(1));
    assert(!amp_on && model == 1);
    assert(gains[0] == 12);
    assert(coyopedal_fx_enabled(COYOPEDAL_FX_DELAY));
    assert(panel_presets::rename(1, "Lead 2"));
    assert(std::string(panel_presets::name(1)) == "Lead 2");
    assert(panel_presets::remember_active());
    panel_presets::init();
    assert(panel_presets::active() == 1);
    assert(panel_presets::load(panel_presets::active()));
    const auto prior = saved;
    storage_failure = true;
    amp_on = true;
    assert(!panel_presets::save());
    assert(panel_presets::edited());
    assert(saved == prior);
    assert(!panel_presets::remember_active());
    assert(!panel_presets::add("Failure"));
    assert(panel_presets::count() == 2);
    assert(!panel_presets::erase(1));
    assert(panel_presets::active() == 1);
    assert(amp_on && model == 1);
    storage_failure = false;
    assert(panel_presets::save());
    assert(!panel_presets::edited());
    coyopedal_model_count = 1;
    assert(!panel_presets::load(1));
    assert(model == 1 && amp_on && engaged);
    coyopedal_model_count = 2;
    assert(panel_presets::erase(0));
    assert(panel_presets::active() == 0);
    assert(panel_presets::count() == 1);
    assert(std::string(panel_presets::name(0)) == "Lead 2");
    assert(!panel_presets::erase(0));
    assert(!panel_presets::rename(0, ""));
    model = 0;
    amp_on = false;
    panel_presets::init();
    assert(panel_presets::count() == 1);
    assert(panel_presets::load(0));
    assert(model == 1 && amp_on);

    // The document. What the mirror wrote is what to_json() writes, and reading
    // it back gives the same list -- which is the whole claim of keeping presets
    // as text: a file off a card, or one edited by hand, is a preset list.
    assert(mirrored.find("\"name\": \"Lead 2\"") != std::string::npos);
    assert(mirrored.find("\"profile\": \"sd-model\"") != std::string::npos);
    std::string text(panel_presets::json_capacity(), '\0');
    const size_t length = panel_presets::to_json(text.data(), text.size());
    assert(length != 0);
    text.resize(length);
    assert(text == mirrored);

    assert(panel_presets::from_json(text.c_str(), text.size()));
    assert(panel_presets::count() == 1);
    assert(std::string(panel_presets::name(0)) == "Lead 2");

    // A hand-edited name comes through, and a hand-broken file is refused
    // without touching the list -- the two things a player can do to it.
    std::string edited = text;
    edited.replace(edited.find("Lead 2"), 6, "By Hand");
    assert(panel_presets::from_json(edited.c_str(), edited.size()));
    assert(std::string(panel_presets::name(0)) == "By Hand");
    assert(!panel_presets::from_json("{\"version\": 3, \"presets\": []}", 29));
    assert(std::string(panel_presets::name(0)) == "By Hand");
    assert(panel_presets::error()[0] != '\0');
}
