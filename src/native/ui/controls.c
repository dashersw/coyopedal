// Audio/control state shared by the Gea UI and maintenance protocol.
// The drawing, navigation and touch implementation lives entirely in Gea.
#include "board_ui.h"
#include "control.h"
#include "model_catalog.h"
#include "factory_presets.h"
#include "controls.h"
#include "audio/effects.h"
#include "audio/tuner.h"
#include "audio/usb_frame_processor.h"
#include <string.h>

enum { AMP_COUNT = 6, AMP_STEP = 5 };
typedef struct {
    const char* label;
    int16_t minimum, maximum, initial, neutral;
} amp_param_t;

enum { SCREEN_HOME, SCREEN_TUNER };
static struct {
    bool engaged, amp_on, fx_present[COYOPEDAL_FX_BLOCK_COUNT];
    unsigned preset;
    int screen;
    coyopedal_ui_usb_t usb;
    int16_t amp_value[AMP_COUNT];
} ui;

static const amp_param_t amp_params[AMP_COUNT] = {
    {"INPUT", -120, 120, 20, 0}, {"DRIVE", 0, 180, 85, 0}, {"LEVEL", -180, 60, -30, 0},
    {"BASS", -120, 120, 0, 0},   {"MID", -120, 120, 0, 0}, {"TREBLE", -120, 120, 0, 0},
};

#define GAIN_STEP_RATIO 1.05925372F

static float gain_from_tenths(const int16_t tenths) {
    // Apply half-decibel steps without powf.
    int steps = tenths / AMP_STEP;
    float gain = 1.0F;
    for (; steps > 0; --steps) {
        gain *= GAIN_STEP_RATIO;
    }
    for (; steps < 0; ++steps) {
        gain /= GAIN_STEP_RATIO;
    }
    return gain;
}

static void apply_amp_gains(void) {
    if (!coyopedal_pedal_dsp_begin_update()) {
        return;
    }
    // Drive multiplies into the input stage.
    coyopedal_pedal_dsp_set_input_gain(
        gain_from_tenths((int16_t)(ui.amp_value[0] + ui.amp_value[1])));
    coyopedal_pedal_dsp_set_output_gain(gain_from_tenths(ui.amp_value[2]));
    // Tenths of a decibel on the panel, decibels in the DSP: the tone stack takes a gain
    // in dB rather than a linear factor, so this is a scale rather than a conversion.
    coyopedal_pedal_dsp_set_tone((float)ui.amp_value[3] * 0.1F, (float)ui.amp_value[4] * 0.1F,
                                 (float)ui.amp_value[5] * 0.1F);
    coyopedal_pedal_dsp_end_update();
}

static void apply_bypass(void) {
    coyopedal_pedal_dsp_set_bypass(!ui.amp_on);
    coyopedal_pedal_dsp_set_pedal_bypass(!ui.engaged);
}

static bool load_preset(void) {
    const unsigned index = ui.preset;
    char error[48] = {0};
    const bool ok = coyopedal_preset_load(index, error, sizeof error);

    const int16_t* const gains = coyopedal_preset_amp_gains();
    for (int i = 0; i < AMP_COUNT; ++i) {
        ui.amp_value[i] = gains[i];
    }
    ui.amp_on = ok;
    // Every preset sets every block, engaged or not, so a bypassed block is
    // still present: presence does not follow the enable flag.
    for (uint8_t block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
        ui.fx_present[block] = true;
    }
    apply_bypass();

    return ok;
}

static void set_engaged(const bool engaged) {
    ui.engaged = engaged;
    apply_bypass();
}

static void set_tuner(const bool on) {
    coyopedal_pedal_dsp_set_tuner(on);
    ui.screen = on ? SCREEN_TUNER : SCREEN_HOME;
}

void coyopedal_controls_init(void) {
    memset(&ui, 0, sizeof ui);
    // Start engaged. The bridge restores the remembered preset afterwards;
    // without one, the first factory preset is Silver Lining (clean).
    ui.engaged = true;
    ui.amp_on = true;
    for (int i = 0; i < COYOPEDAL_FX_BLOCK_COUNT; ++i)
        ui.fx_present[i] = true;
    ui.fx_present[COYOPEDAL_FX_DELAY] = coyopedal_fx_delay_ready();
    for (int i = 0; i < AMP_COUNT; ++i)
        ui.amp_value[i] = amp_params[i].initial;
    if (coyopedal_preset_count())
        (void)load_preset();
    else
        apply_amp_gains();
    apply_bypass();
}
void coyopedal_ui_set_usb(coyopedal_ui_usb_t state) {
    ui.usb = state;
}

static void mix(uint32_t* const hash, const void* const data, const size_t bytes) {
    const unsigned char* const octets = (const unsigned char*)data;
    for (size_t index = 0; index < bytes; ++index) {
        *hash ^= octets[index];
        *hash *= 16777619U;
    }
}

uint32_t coyopedal_ui_revision(void) {
    uint32_t hash = 2166136261U;
    const uint8_t flags[] = {
        (uint8_t)ui.screen,
        (uint8_t)ui.engaged,
        (uint8_t)ui.amp_on,
    };
    mix(&hash, flags, sizeof flags);
    mix(&hash, &ui.preset, sizeof ui.preset);
    mix(&hash, ui.amp_value, sizeof ui.amp_value);
    mix(&hash, ui.fx_present, sizeof ui.fx_present);

    const unsigned model = coyopedal_active_model();
    mix(&hash, &model, sizeof model);

    for (uint8_t block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
        const coyopedal_fx_block_t fx = (coyopedal_fx_block_t)block;
        const uint8_t enabled = coyopedal_fx_enabled(fx) ? 1U : 0U;
        mix(&hash, &enabled, sizeof enabled);
        const uint8_t count = coyopedal_fx_param_count(fx);
        for (uint8_t index = 0; index < count; ++index) {
            const int16_t value = coyopedal_fx_param(fx, index);
            mix(&hash, &value, sizeof value);
        }
    }
    return hash;
}

void coyopedal_ui_control_state(coyopedal_control_state_t* const out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->revision = coyopedal_ui_revision();
    out->protocol_version = (uint16_t)COYOPEDAL_CONTROL_PROTOCOL_VERSION;
    out->screen = (uint8_t)ui.screen;
    out->engaged = ui.engaged ? 1U : 0U;
    out->tuner = (ui.screen == SCREEN_TUNER) ? 1U : 0U;
    out->usb = (uint8_t)ui.usb;

    const unsigned active = coyopedal_preset_active();
    out->preset = (active == COYOPEDAL_PRESET_NONE) ? 0xFFFFU : (uint16_t)active;

    const unsigned model = coyopedal_active_model();
    out->model = (model < coyopedal_model_count) ? (uint16_t)model : 0xFFFFU;
    out->amp_present = (model < coyopedal_model_count) ? 1U : 0U;
    for (unsigned index = 0; index < COYOPEDAL_CONTROL_AMP_GAINS; ++index) {
        out->amp[index] = ui.amp_value[index];
    }

    for (uint8_t block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
        const coyopedal_fx_block_t fx = (coyopedal_fx_block_t)block;
        coyopedal_control_block_t* const slot = &out->block[block];
        slot->present = ui.fx_present[block] ? 1U : 0U;
        slot->enabled = coyopedal_fx_enabled(fx) ? 1U : 0U;
        const uint8_t count = coyopedal_fx_param_count(fx);
        slot->param_count =
            (count > COYOPEDAL_CONTROL_MAX_PARAMS) ? (uint8_t)COYOPEDAL_CONTROL_MAX_PARAMS : count;
        for (uint8_t index = 0; index < slot->param_count; ++index) {
            slot->param[index] = coyopedal_fx_param(fx, index);
        }
    }

    coyopedal_control_block_t* const amp = &out->block[COYOPEDAL_CONTROL_AMP_BLOCK];
    amp->present = out->amp_present;
    amp->enabled = ui.amp_on ? 1U : 0U;
    amp->param_count = 0U;

    out->gate_closed = coyopedal_fx_gate_closed() ? 1U : 0U;
    out->compressor_reduction = coyopedal_fx_compressor_reduction();

    coyopedal_tuner_reading_t reading;
    coyopedal_tuner_read(&reading);
    out->tuner_voiced = reading.voiced ? 1U : 0U;
    out->tuner_cents = (int16_t)reading.cents;
    out->tuner_note = reading.voiced ? (int16_t)reading.note : (int16_t)-1;
    out->tuner_octave = (int16_t)reading.octave;
    out->tuner_millihertz = (uint32_t)(reading.frequency * 1000.0F);
}

bool coyopedal_ui_control_set_param(const uint8_t block, const uint8_t index, const int16_t value) {
    if (block == COYOPEDAL_CONTROL_AMP_BLOCK) {
        if (index >= AMP_COUNT) {
            return false;
        }
        const amp_param_t* const info = &amp_params[index];
        int16_t clamped = value;
        if (clamped < info->minimum) {
            clamped = info->minimum;
        }
        if (clamped > info->maximum) {
            clamped = info->maximum;
        }
        ui.amp_value[index] = clamped;
        apply_amp_gains();

        return true;
    }
    if (block >= COYOPEDAL_FX_BLOCK_COUNT || !ui.fx_present[block]) {
        return false;
    }
    const coyopedal_fx_block_t fx = (coyopedal_fx_block_t)block;
    if (index >= coyopedal_fx_param_count(fx)) {
        return false;
    }
    if (!coyopedal_pedal_dsp_begin_update()) {
        return false;
    }

    coyopedal_fx_set_param(fx, index, value);
    coyopedal_pedal_dsp_end_update();

    return true;
}

bool coyopedal_ui_control_set_enabled(const uint8_t block, const bool enabled) {
    if (block == COYOPEDAL_CONTROL_AMP_BLOCK) {
        ui.amp_on = enabled;
        apply_bypass();

        return true;
    }
    if (block >= COYOPEDAL_FX_BLOCK_COUNT || !ui.fx_present[block]) {
        return false;
    }

    if (!coyopedal_pedal_dsp_begin_update()) {
        return false;
    }
    coyopedal_fx_set_enabled((coyopedal_fx_block_t)block, enabled);
    coyopedal_pedal_dsp_end_update();

    return true;
}

bool coyopedal_ui_control_load_preset(const unsigned index) {
    if (index >= coyopedal_preset_count()) {
        return false;
    }
    ui.preset = index;

    const bool loaded = load_preset();

    return loaded;
}

bool coyopedal_ui_control_set_model(const unsigned index) {
    if (index >= coyopedal_model_count) {
        return false;
    }
    char error[48] = {0};
    if (!coyopedal_load_model(index, error, sizeof error)) {

        return false;
    }
    ui.amp_on = true;
    apply_bypass();

    return true;
}

bool coyopedal_ui_control_set_engaged(const bool engaged) {
    set_engaged(engaged);
    return true;
}

bool coyopedal_ui_control_set_tuner(const bool on) {
    if ((ui.screen == SCREEN_TUNER) == on) {
        return true;
    }
    set_tuner(on);
    return true;
}
