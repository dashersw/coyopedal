#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Controls for the real-time audio path, callable from C.

// Suspends the real-time path at a block boundary while a group of controls is
// changed, so preset and model replacement cannot race the DSP stages. Calls may
// be nested.
bool coyopedal_pedal_dsp_begin_update(void);
void coyopedal_pedal_dsp_end_update(void);

void coyopedal_pedal_dsp_set_input_gain(float gain);
void coyopedal_pedal_dsp_set_output_gain(float gain);

// The amp's tone stack, in decibels: bass, mid, treble, each plus or minus twelve. It
// runs after the profile - see Processor::set_tone for why an EQ in front of a full-rig
// capture is a change of gain structure rather than a tone control.
void coyopedal_pedal_dsp_set_tone(float bass_db, float mid_db, float treble_db);

// Loads an A2-Full .namb image. Not real-time safe. Returns false and copies a
// short error message when the header, size, checksum, or weights are invalid;
// the embedded fallback profile is then loaded in its place.
bool coyopedal_pedal_dsp_load_namb(const uint8_t* data, size_t size, char* error_message,
                                   size_t error_message_capacity);
bool coyopedal_pedal_dsp_model_loaded(void);

// The amp block's own bypass, and nothing else's: a true bypass of the profile
// and its gains, with the effects around it left running, like every other
// block's enable (coyopedal_fx_set_enabled). The profile stays resident, so
// engaging again is instant.
void coyopedal_pedal_dsp_set_bypass(bool bypass);
bool coyopedal_pedal_dsp_bypassed(void);

// The pedal's own footswitch: the whole thing steps out of the way, effects
// included.
//
// Skipped rather than run at a null setting: clearing a block's state is not
// real-time safe, so tails stay in their
// buffers and pick up where they left off - what a pedal with trails does.
void coyopedal_pedal_dsp_set_pedal_bypass(bool bypass);
bool coyopedal_pedal_dsp_pedal_bypassed(void);

// Engages the tuner. While it is on, the block is handed to the detector and the
// output is silence: the effects chain, the profile and the tone stack are all
// skipped, not merely bypassed.
//
// Skipping inference releases the audio budget while the tuner is active.
void coyopedal_pedal_dsp_set_tuner(bool active);
bool coyopedal_pedal_dsp_tuner_active(void);

#ifdef __cplusplus
}
#endif
