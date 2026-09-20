#include "audio/usb_frame_processor.h"

#include <cstddef>
#include <cstdint>

#include "audio/processor.hpp"
#include "usb_audio.h"

extern coyopedal::pedal::Processor g_engine;
extern "C" const std::uint8_t kModelStart[] asm("_binary_fallback_model_namb_start");
extern "C" const std::uint8_t kModelEnd[] asm("_binary_fallback_model_namb_end");

extern "C" bool coyopedal_pedal_dsp_begin_update(void) {
    return usb_audio_begin_update();
}

extern "C" void coyopedal_pedal_dsp_end_update(void) {
    usb_audio_end_update();
}

extern "C" void coyopedal_pedal_dsp_set_input_gain(const float gain) {
    g_engine.set_input_gain(gain);
}

extern "C" void coyopedal_pedal_dsp_set_output_gain(const float gain) {
    g_engine.set_output_gain(gain);
}

extern "C" void coyopedal_pedal_dsp_set_tone(const float bass_db, const float mid_db,
                                             const float treble_db) {
    g_engine.set_tone(bass_db, mid_db, treble_db);
}

extern "C" bool coyopedal_pedal_dsp_load_namb(const std::uint8_t* const data,
                                              const std::size_t size, char* const error,
                                              const std::size_t error_capacity) {
    if (usb_audio_load_model(data, size, error, error_capacity)) {
        return true;
    }
    // App-only OTA can leave an older model-library partition behind. An
    // incompatible selection must not unload the valid A2-Full embedded amp.
    char fallback_error[96]{};
    const std::size_t fallback_size = static_cast<std::size_t>(kModelEnd - kModelStart);
    (void)usb_audio_load_model(kModelStart, fallback_size, fallback_error, sizeof fallback_error);
    return false;
}

extern "C" bool coyopedal_pedal_dsp_model_loaded(void) {
    return g_engine.model_loaded();
}

extern "C" void coyopedal_pedal_dsp_set_bypass(const bool bypass) {
    g_engine.set_bypass(bypass);
}

extern "C" bool coyopedal_pedal_dsp_bypassed(void) {
    return g_engine.bypassed();
}

extern "C" void coyopedal_pedal_dsp_set_pedal_bypass(const bool bypass) {
    usb_audio_set_pedal_bypass(bypass);
}

extern "C" bool coyopedal_pedal_dsp_pedal_bypassed(void) {
    return usb_audio_pedal_bypassed();
}

extern "C" void coyopedal_pedal_dsp_set_tuner(const bool active) {
    usb_audio_set_tuner(active);
}

extern "C" bool coyopedal_pedal_dsp_tuner_active(void) {
    return usb_audio_tuner_active();
}
