#pragma once
#include <cstddef>
namespace panel_presets {
void init();
unsigned count();
unsigned active();
const char* name(unsigned index);
const char* error();
bool edited();
bool load(unsigned index);
bool save();
bool remember_active();
bool rename(unsigned index, const char* name);
bool add(const char* name);
bool erase(unsigned index);

// The player's list as the one preset document: the same text assets/presets.json
// is, and the same text a folder or a card carries in coyopedal-presets.json.
// Returns the length written, or zero when it does not fit.
size_t to_json(char* out, size_t capacity);

// Replaces the list with the one in `text`. Used by the card: a folder of
// captures carries the presets that name them, so the machine it is plugged into
// ends up with both. False leaves the list untouched and error() says why.
bool from_json(const char* text, size_t length);

// The buffer to_json() wants, which is preset_json.h's ceiling for a full list.
size_t json_capacity();
} // namespace panel_presets
