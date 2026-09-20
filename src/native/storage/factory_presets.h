#ifndef COYOPEDAL_PEDAL_FACTORY_PRESETS_H_
#define COYOPEDAL_PEDAL_FACTORY_PRESETS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// For the block count and the model-id width a preset record is built from.
#include "model_catalog.h"
#include "audio/effects.h"

#ifdef __cplusplus
extern "C" {
#endif

// Factory presets for the multi-effects app.
//
// A preset is the whole signal chain at once: which profile the amp block loads,
// the amp's six gains, and for each effect whether it is engaged and what its
// parameters are. Recalling one sets the whole chain in one step.
//
// It is a flat list; the document carries them in order.
//
// The names are the contract with the player; the settings behind them are chosen
// against the real parameter ranges in src/audio/effects.h.
//
// They ship in their own partition, separate from the profile library: the library
// changes when the captures do, the presets are a couple of kilobytes and change
// whenever a setting is tweaked. The library is packed by tools/models_to_bin.py;
// the presets are not packed at all. What is in the `presets` partition is
// assets/presets.json itself, checked and copied by tools/presets_to_json.py, and
// read by preset_json.c -- see the document's description in preset_json.h. Both
// are embedded in the app image too, and drivers/flash_storage.cpp repairs the
// partition from that copy at boot so a board never runs on a stale one.
//
// The table is read into RAM at boot, like the profile index and for the same
// reason - a partition is reached through esp_partition_read(), not a pointer, so
// there is nothing to dereference in place.
//
// The counts below are the ceiling the RAM table is sized for, not what the board
// has: an unprogrammed board reports zero presets, and coyopedal_preset_count()
// is what a front end should ask.

#define COYOPEDAL_PRESET_NAME_MAX 24
#define COYOPEDAL_PRESET_COUNT 128

#define COYOPEDAL_PRESET_NONE 0xFFFFFFFFU

// Reads the preset table out of the document in flash. Leaves the count at zero
// if there is none -- an unprogrammed board, or a file that is not a preset
// document -- and the panel then shows nothing rather than names of profiles it
// does not have.
//
// Call after coyopedal_models_init(), which is what finds the partitions.
bool coyopedal_presets_init(void);

// Number of presets. Indices run 0 to count-1.
unsigned coyopedal_preset_count(void);
// Stable model id named by a preset, or an empty string when the slot does not
// exist. Boot uses this to load preset 1 before the S3's later allocations.
const char* coyopedal_preset_profile(unsigned index);

// Applies a preset: loads its profile, sets the amp gains, and sets every effect's
// enable state and parameters.
//
// Not real-time safe, because loading a profile is not: it reads the capture
// out of storage and rewrites the weight set. Call it from the UI task, the
// same place a profile change already happens.
//
// Returns false if the profile it names is not in the library - a preset built
// around a capture the board was never given. The effects are still applied in
// that case, so the pedal lands on the right settings with whatever profile was
// already loaded, and `error` says what was missing.
bool coyopedal_preset_load(unsigned index, char* error, size_t error_capacity);

// Index of the last preset applied, or COYOPEDAL_PRESET_NONE if none has been.
// Editing a parameter afterwards does not clear this: the panel wants to show
// which preset you started from.
unsigned coyopedal_preset_active(void);

// The amp gains a preset asked for, in tenths of a decibel, so the panel can show
// the same numbers the preset set. Order is input, drive, level, bass, mid, treble.
const int16_t* coyopedal_preset_amp_gains(void);

// How many parameters one block's entry carries, which is the widest any block has
// rather than what a given one uses.
#define COYOPEDAL_PRESET_PARAMS 5
#define COYOPEDAL_PRESET_AMP 6

// A whole preset, in RAM. This is the unit a front end edits and saves, and the
// unit preset_json.c reads and writes: the profile it names, the amp's six gains, and each block's
// engage state and parameters.
typedef struct {
    bool enabled;
    int16_t params[COYOPEDAL_PRESET_PARAMS];
} coyopedal_block_setting_t;

typedef struct {
    char name[COYOPEDAL_PRESET_NAME_MAX];
    char profile[COYOPEDAL_MODEL_ID_MAX];
    int16_t amp[COYOPEDAL_PRESET_AMP];
    // Whether the amp block itself is engaged. The factory list has it on
    // everywhere and said so by not having the field at all; a preset the player
    // saved with the amp switched out has to carry it, and now that a preset is
    // the same document wherever it is written down, both use this.
    bool amp_on;
    coyopedal_block_setting_t blocks[COYOPEDAL_FX_BLOCK_COUNT];
} coyopedal_preset_record_t;

// Reads one preset out of the RAM table. False for an index the board does not
// have, in which case nothing is written to out.
bool coyopedal_preset_read(unsigned index, coyopedal_preset_record_t* out);

#ifdef __cplusplus
}
#endif

#endif // COYOPEDAL_PEDAL_FACTORY_PRESETS_H_
