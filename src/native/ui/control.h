#ifndef COYOPEDAL_PEDAL_UI_CONTROL_H_
#define COYOPEDAL_PEDAL_UI_CONTROL_H_

#include <stdbool.h>
#include <stdint.h>

#include "audio/control_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a host front end reaches the pedal through.
//
// Every write here goes through the panel rather than around it. A host that
// called coyopedal_fx_set_enabled() directly would change the sound and leave the
// screen showing the old chain, and the two would stay apart until something else
// happened to force a repaint - which is the bug where a player looks down mid-set
// and the pedal is lying to them. So these are the same functions the panel's own
// touch handlers call, and they mark the same regions dirty: a preset recall
// repaints everything, a block toggle repaints the strip.
//
// Reads go the same way for the same reason. The snapshot is built from the live
// engine and the live panel state, so there is no second copy of anything on the
// device that could disagree with the first.

// A counter that changes whenever anything a host can control changes, from any
// source - a touch on the panel, a host write, a preset recall.
//
// It is a hash of the controllable state rather than a count of edits, and that is
// deliberate: a counter has to be incremented at every site that changes
// something, so it drifts the first time a new edit path forgets to, and the
// symptom is a host that silently stops updating. A hash cannot forget. It costs a
// few hundred instructions per poll, which is the cheaper side of that trade by a
// wide margin.
//
// It deliberately excludes the meters - the gate's state, the compressor's
// reduction, the tuner's reading - because those change continuously and would
// make the counter useless as an idle poll. A host that wants them reads the
// snapshot on its own cadence.
uint32_t coyopedal_ui_revision(void);

// Fills the whole host-visible state. Never fails; a block that is not present
// reads as absent rather than as zero.
void coyopedal_ui_control_state(coyopedal_control_state_t* out);

// Writes. Each returns false if the request names something that does not exist
// or the audio pipeline is busy. Block indices are coyopedal_fx_block_t values, with
// COYOPEDAL_CONTROL_AMP_BLOCK for the amp.
bool coyopedal_ui_control_set_param(uint8_t block, uint8_t index, int16_t value);
bool coyopedal_ui_control_set_enabled(uint8_t block, bool enabled);
bool coyopedal_ui_control_load_preset(unsigned index);
bool coyopedal_ui_control_set_model(unsigned index);
bool coyopedal_ui_control_set_engaged(bool engaged);
bool coyopedal_ui_control_set_tuner(bool on);

#ifdef __cplusplus
}
#endif

#endif // COYOPEDAL_PEDAL_UI_CONTROL_H_
