#include "factory_presets.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#endif

#include "model_catalog.h"
#include "flash_storage.h"
#include "preset_json.h"
#include "esp_heap_caps.h"
#include "audio/effects.h"
#include "audio/usb_frame_processor.h"

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#define COYOPEDAL_PRESET_LOG(...) ESP_LOGW("presets", __VA_ARGS__)
#else
#define COYOPEDAL_PRESET_LOG(...) ((void)0)
#endif

// Parameter order matches the tables in src/audio/effects.cpp. Keeping the numbers
// here as the real units - tenths of a decibel, milliseconds, percent, hertz -
// rather than as normalised fractions means a preset can be read and judged
// without running it.
//
//   gate:        threshold dB*10, release ms
//   comp:        threshold dB*10, ratio *10, attack ms, release ms, makeup dB*10
//   drive:       drive dB*10, tone Hz, level dB*10
//   reverb:      output (0 stereo, 1 mono), mix %, decay ms, damping %, predelay ms
//   modulation:  rate Hz*10, depth %, delay ms, mix %
//   delay:       time ms, feedback %, mix %

// The record is coyopedal_preset_record_t, declared in the header. Its fields:
//
//   profile    an id in assets/models/factory.json, not an index.
//   amp        input, drive, level, then the tone stack: bass, mid, treble, all in
//              tenths of a decibel. Six because the amp has an EQ, and a preset
//              that did not carry it would leave the previous preset's tone under
//              a new profile.
//   blocks     indexed by coyopedal_fx_block_t, which is why the order is that enum's
//              rather than the order the chain runs in.
typedef coyopedal_block_setting_t block_setting_t;
typedef coyopedal_preset_record_t preset_t;

// Presets are read out of their own container in flash at boot, separately from
// the profile library. Zero presets is what a board that has never been given one
// looks like, and the panel shows nothing rather than names of profiles that are
// not there.
#if defined(ESP_PLATFORM)
EXT_RAM_BSS_ATTR
#endif
static preset_t presets[COYOPEDAL_PRESET_COUNT];
static unsigned preset_count = 0;

static unsigned active = COYOPEDAL_PRESET_NONE;
static int16_t amp_gains[6] = {0, 0, 0, 0, 0, 0};

// The factory list is JSON -- assets/presets.json, flashed into the presets
// partition exactly as the repo keeps it. There is no container around it and no
// packer in front of it: what is in flash is the file, and `esptool read_flash`
// on the partition gives you back something you can read. preset_json.c is the
// reader, and it is the same one the player's own list goes through, so a file
// the board writes onto a card and the file this repo ships are the same shape.
//
// The partition is read whole. Everything past the closing brace is the 0xFF of
// an erased sector, which the parser stops at.
bool coyopedal_presets_init(void) {
    preset_count = 0;

    char* const text =
        heap_caps_malloc(COYOPEDAL_PRESET_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text == NULL) {
        return false;
    }
    uint32_t length = 0;
    char error[64] = {0};
    if (coyopedal_flash_presets_read(text, COYOPEDAL_PRESET_JSON_MAX, &length) && length != 0U) {
        preset_count = coyopedal_presets_from_json(text, length, presets, COYOPEDAL_PRESET_COUNT,
                                                   error, sizeof error);
    }
    heap_caps_free(text);

    // No presets is what an unprogrammed board looks like, and the panel says so
    // rather than showing names of profiles it does not have.
    if (preset_count == 0U && error[0] != '\0') {
        COYOPEDAL_PRESET_LOG("factory presets unreadable: %s", error);
    }
    return preset_count > 0U;
}

unsigned coyopedal_preset_count(void) {
    return preset_count;
}

const char* coyopedal_preset_profile(const unsigned index) {
    return (index < preset_count) ? presets[index].profile : "";
}

unsigned coyopedal_preset_active(void) {
    return active;
}

const int16_t* coyopedal_preset_amp_gains(void) {
    return amp_gains;
}

// 10^(x/20) built from 0.5 dB steps, matching the panel's own gain arithmetic so a
// preset and a knob at the same number produce the same gain.
static float gain_from_tenths(const int16_t tenths) {
    int steps = tenths / 5;
    float gain = 1.0F;
    for (; steps > 0; --steps) {
        gain *= 1.059253725F;
    }
    for (; steps < 0; ++steps) {
        gain /= 1.059253725F;
    }
    return gain;
}

static void apply_block(const coyopedal_fx_block_t block, const block_setting_t* const setting) {
    const uint8_t count = coyopedal_fx_param_count(block);
    for (uint8_t index = 0; index < count && index < 5U; ++index) {
        coyopedal_fx_set_param(block, index, setting->params[index]);
    }
    coyopedal_fx_set_enabled(block, setting->enabled);
}

static void copy_error(char* const out, const size_t capacity, const char* const message) {
    if (out == NULL || capacity == 0U) {
        return;
    }
    strncpy(out, message, capacity - 1U);
    out[capacity - 1U] = '\0';
}

bool coyopedal_preset_load(const unsigned index, char* const error, const size_t error_capacity) {
    if (index >= preset_count) {
        copy_error(error, error_capacity, "no such preset");
        return false;
    }
    if (!coyopedal_pedal_dsp_begin_update()) {
        copy_error(error, error_capacity, "audio pipeline busy");
        return false;
    }
    const preset_t* const preset = &presets[index];

    // Effects first, and unconditionally. If the profile is missing the player
    // still lands on the right settings rather than on the previous preset's.
    //
    // The delay is applied whether or not its line came up: the block reports
    // itself unavailable and is skipped, so the preset still carries its setting for
    // a board that has the memory rather than the library needing to know what a
    // given board brought up.
    for (uint8_t block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
        apply_block((coyopedal_fx_block_t)block, &preset->blocks[block]);
    }

    memcpy(amp_gains, preset->amp, sizeof amp_gains);
    // Input and drive both feed the profile's input, as the panel's knob row does.
    coyopedal_pedal_dsp_set_input_gain(
        gain_from_tenths((int16_t)(preset->amp[0] + preset->amp[1])));
    coyopedal_pedal_dsp_set_output_gain(gain_from_tenths(preset->amp[2]));
    // Tenths on the way in, decibels on the way out: the tone stack takes a gain, not a
    // factor.
    coyopedal_pedal_dsp_set_tone((float)preset->amp[3] * 0.1F, (float)preset->amp[4] * 0.1F,
                                 (float)preset->amp[5] * 0.1F);

    active = index;

    // Profiles are named rather than numbered: the library is built from
    // assets/models/factory.json and its order is not a stable identifier, and a fresh
    // clone legitimately has only the required default.
    for (unsigned candidate = 0; candidate < coyopedal_model_count; ++candidate) {
        if (strcmp(coyopedal_models[candidate].id, preset->profile) == 0) {
            if (coyopedal_active_model() == candidate && coyopedal_pedal_dsp_model_loaded()) {
                coyopedal_pedal_dsp_end_update();
                return true;
            }
            char reason[64] = {0};
            if (coyopedal_load_model(candidate, reason, sizeof reason)) {
                coyopedal_pedal_dsp_end_update();
                return true;
            }
            copy_error(error, error_capacity, reason);
            coyopedal_pedal_dsp_end_update();
            return false;
        }
    }

    copy_error(error, error_capacity, "profile not in library");
    coyopedal_pedal_dsp_end_update();
    return false;
}

//--------------------------------------------------------------------+
// Reading one preset
//--------------------------------------------------------------------+

bool coyopedal_preset_read(const unsigned index, coyopedal_preset_record_t* const out) {
    if (index >= preset_count || out == NULL) {
        return false;
    }
    *out = presets[index];
    return true;
}
