#ifndef COYOPEDAL_PEDAL_PRESET_JSON_H_
#define COYOPEDAL_PEDAL_PRESET_JSON_H_

#include <stdbool.h>
#include <stddef.h>

#include "factory_presets.h"

#ifdef __cplusplus
extern "C" {
#endif

// The preset document: the one shape a preset has anywhere it is written down.
//
//   assets/presets.json     the factory list, flashed into the presets partition
//                           exactly as it is written here and read back by
//                           coyopedal_presets_init()
//   coyopedal-presets.json  the player's own list, mirrored onto the SD card by
//                           the board and into the picked folder by the browser
//
// One format, three places, and a file from any of them can be dropped into any
// other. That is the whole reason it is text: a preset is six numbers and a name
// per block, a player should be able to read it, diff it, keep it in a repo and
// edit it in a text editor, and none of that is true of a packed struct. The
// version that used to be packed is gone; the parser here is what both the board
// and the page read presets with.
//
//   {
//     "version": 3,
//     "presets": [
//       {
//         "name": "Silver Lining",
//         "profile": "volum-herbert-1-v30",
//         "amp": [20, 40, -30, 10, -10, 20],
//         "ampOn": true,
//         "blocks": [
//           { "block": "gate", "enabled": true, "params": [-640, 150] },
//           ...
//         ]
//       }
//     ]
//   }
//
// `amp` is input, drive, level, bass, mid, treble, in tenths of a decibel.
// `params` are the block's own real units -- milliseconds, percent, hertz,
// tenths of a decibel -- in the order src/audio/effects.cpp declares them, and a
// block may give fewer than it has. `ampOn` defaults to true. A block that is
// not listed is off. Block names and their order are coyopedal_fx_block_t's, which
// is not the order the chain runs in.
#define COYOPEDAL_PRESET_JSON_VERSION 3

// Parses a document into records. Returns how many it read, which is zero when
// the text is not a preset document -- `error` then says why, in words meant for
// the panel. Trailing bytes after the closing brace are ignored, because this
// parses a whole flash partition whose tail is erased 0xFF.
unsigned coyopedal_presets_from_json(const char* text, size_t length,
                                     coyopedal_preset_record_t* out, unsigned capacity, char* error,
                                     size_t error_capacity);

// Writes a document. Returns its length, or zero when it does not fit. The
// layout matches assets/presets.json: two-space indent, one line per block, so
// what the board writes onto a card and what this repo keeps are the same file.
size_t coyopedal_presets_to_json(const coyopedal_preset_record_t* presets, unsigned count,
                                 char* out, size_t capacity);

// Room for a full library of them: 32 presets of about 560 bytes, and a margin
// for names and profile ids at their limits.
#define COYOPEDAL_PRESET_JSON_MAX 24576U

#ifdef __cplusplus
}
#endif

#endif // COYOPEDAL_PEDAL_PRESET_JSON_H_
