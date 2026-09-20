#include "preset_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

// coyopedal_fx_block_t's order, which is the order a record stores its blocks in
// and the order they are written here. Not the order the chain runs in -- see
// the note on the enum in src/audio/effects.h, which explains why these numbers
// are frozen.
static const char* const block_names[COYOPEDAL_FX_BLOCK_COUNT] = {
    "gate", "compressor", "overdrive", "reverb", "modulation", "delay",
};

static void say(char* const error, const size_t capacity, const char* const message) {
    if (error != NULL && capacity != 0U) {
        snprintf(error, capacity, "%s", message);
    }
}

// The effects layer clamps every parameter to its own range anyway, so a number
// outside int16 is taken as far as it goes rather than refused: a hand-edited
// file with a delay of 99999 ms should give the longest delay there is, not an
// unreadable preset.
static int16_t clamp16(const double value) {
    if (value <= -32768.0) {
        return -32768;
    }
    if (value >= 32767.0) {
        return 32767;
    }
    return (int16_t)value;
}

static bool copy_text(char* const out, const size_t capacity, const cJSON* const item) {
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    const size_t length = strlen(item->valuestring);
    if (length == 0U || length >= capacity) {
        return false;
    }
    memcpy(out, item->valuestring, length + 1U);
    return true;
}

static bool read_block(const cJSON* const entry, coyopedal_preset_record_t* const preset,
                       char* const error, const size_t error_capacity) {
    const cJSON* const name = cJSON_GetObjectItemCaseSensitive(entry, "block");
    if (!cJSON_IsString(name) || name->valuestring == NULL) {
        say(error, error_capacity, "a block has no name");
        return false;
    }
    unsigned index = 0;
    for (; index < COYOPEDAL_FX_BLOCK_COUNT; ++index) {
        if (strcmp(block_names[index], name->valuestring) == 0) {
            break;
        }
    }
    if (index == COYOPEDAL_FX_BLOCK_COUNT) {
        // Named rather than swallowed: a typo in a hand-edited file is the most
        // likely way to get here, and a block that silently does nothing is the
        // worst possible answer to one.
        char message[64];
        snprintf(message, sizeof message, "no block called %s", name->valuestring);
        say(error, error_capacity, message);
        return false;
    }

    coyopedal_block_setting_t* const setting = &preset->blocks[index];
    setting->enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "enabled"));
    memset(setting->params, 0, sizeof setting->params);

    const cJSON* const params = cJSON_GetObjectItemCaseSensitive(entry, "params");
    if (params != NULL && !cJSON_IsArray(params)) {
        say(error, error_capacity, "params is not a list");
        return false;
    }
    unsigned slot = 0;
    const cJSON* value = NULL;
    cJSON_ArrayForEach(value, params) {
        if (slot >= COYOPEDAL_PRESET_PARAMS) {
            break;
        }
        if (!cJSON_IsNumber(value)) {
            say(error, error_capacity, "a parameter is not a number");
            return false;
        }
        setting->params[slot++] = clamp16(value->valuedouble);
    }
    return true;
}

static bool read_preset(const cJSON* const entry, coyopedal_preset_record_t* const preset,
                        char* const error, const size_t error_capacity) {
    memset(preset, 0, sizeof *preset);

    if (!copy_text(preset->name, COYOPEDAL_PRESET_NAME_MAX,
                   cJSON_GetObjectItemCaseSensitive(entry, "name"))) {
        say(error, error_capacity, "a preset has no usable name");
        return false;
    }
    if (!copy_text(preset->profile, COYOPEDAL_MODEL_ID_MAX,
                   cJSON_GetObjectItemCaseSensitive(entry, "profile"))) {
        say(error, error_capacity, "a preset names no profile");
        return false;
    }

    const cJSON* const amp = cJSON_GetObjectItemCaseSensitive(entry, "amp");
    if (!cJSON_IsArray(amp) || cJSON_GetArraySize(amp) != COYOPEDAL_PRESET_AMP) {
        say(error, error_capacity, "amp needs six numbers");
        return false;
    }
    unsigned slot = 0;
    const cJSON* value = NULL;
    cJSON_ArrayForEach(value, amp) {
        if (!cJSON_IsNumber(value)) {
            say(error, error_capacity, "an amp value is not a number");
            return false;
        }
        preset->amp[slot++] = clamp16(value->valuedouble);
    }

    // Absent means on. Every factory preset has its amp engaged, and a file
    // written by hand should not have to say so to make a sound.
    const cJSON* const amp_on = cJSON_GetObjectItemCaseSensitive(entry, "ampOn");
    preset->amp_on = amp_on == NULL || !cJSON_IsFalse(amp_on);

    const cJSON* const blocks = cJSON_GetObjectItemCaseSensitive(entry, "blocks");
    if (blocks != NULL && !cJSON_IsArray(blocks)) {
        say(error, error_capacity, "blocks is not a list");
        return false;
    }
    // A block that is not listed is off with everything at zero, which is what
    // memset above left behind.
    const cJSON* block = NULL;
    cJSON_ArrayForEach(block, blocks) {
        if (!read_block(block, preset, error, error_capacity)) {
            return false;
        }
    }
    return true;
}

unsigned coyopedal_presets_from_json(const char* const text, const size_t length,
                                     coyopedal_preset_record_t* const out, const unsigned capacity,
                                     char* const error, const size_t error_capacity) {
    if (text == NULL || length == 0U || out == NULL || capacity == 0U) {
        say(error, error_capacity, "no preset file");
        return 0;
    }
    // require_null_terminated is off: this reads a whole flash partition, and
    // everything after the closing brace is the 0xFF an erased sector holds.
    cJSON* const root = cJSON_ParseWithLengthOpts(text, length, NULL, 0);
    if (root == NULL) {
        say(error, error_capacity, "presets file is not JSON");
        return 0;
    }

    unsigned count = 0;
    const cJSON* const presets = cJSON_GetObjectItemCaseSensitive(root, "presets");
    if (!cJSON_IsArray(presets) || cJSON_GetArraySize(presets) == 0) {
        say(error, error_capacity, "presets file lists no presets");
        cJSON_Delete(root);
        return 0;
    }
    const cJSON* entry = NULL;
    cJSON_ArrayForEach(entry, presets) {
        if (count == capacity) {
            break;
        }
        if (!read_preset(entry, &out[count], error, error_capacity)) {
            // One bad preset fails the document. Reading the rest would put a
            // list on the panel that is not the list in the file, and the player
            // would have no way to tell which one they were looking at.
            cJSON_Delete(root);
            return 0;
        }
        ++count;
    }
    cJSON_Delete(root);
    return count;
}

// --- writing -----------------------------------------------------------------
//
// By hand rather than through cJSON_Print, so the layout is exactly the one
// assets/presets.json is kept in: two-space indent, a block to a line. A file
// the board writes onto a card can then be pasted into this repo, and a preset
// copied out of this repo onto a card reads the same way.

typedef struct {
    char* out;
    size_t capacity;
    size_t length;
    bool full;
} writer_t;

static void put(writer_t* const w, const char* const format, ...) {
    if (w->full) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(w->out + w->length, w->capacity - w->length, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= w->capacity - w->length) {
        w->full = true;
        return;
    }
    w->length += (size_t)written;
}

// A preset name and a profile id are the player's, and a quote or a backslash in
// one would otherwise produce a file nothing can read back.
static void put_string(writer_t* const w, const char* const text) {
    put(w, "\"");
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') {
            put(w, "\\%c", *c);
        } else if ((unsigned char)*c < 0x20) {
            put(w, "\\u%04x", (unsigned)(unsigned char)*c);
        } else {
            put(w, "%c", *c);
        }
    }
    put(w, "\"");
}

size_t coyopedal_presets_to_json(const coyopedal_preset_record_t* const presets,
                                 const unsigned count, char* const out, const size_t capacity) {
    if (presets == NULL || out == NULL || capacity == 0U) {
        return 0;
    }
    writer_t w = {out, capacity, 0U, false};
    put(&w, "{\n  \"version\": %d,\n  \"presets\": [\n", COYOPEDAL_PRESET_JSON_VERSION);
    for (unsigned index = 0; index < count; ++index) {
        const coyopedal_preset_record_t* const preset = &presets[index];
        put(&w, "    {\n      \"name\": ");
        put_string(&w, preset->name);
        put(&w, ",\n      \"profile\": ");
        put_string(&w, preset->profile);
        put(&w, ",\n      \"amp\": [");
        for (unsigned value = 0; value < COYOPEDAL_PRESET_AMP; ++value) {
            put(&w, "%s%d", value ? ", " : "", (int)preset->amp[value]);
        }
        put(&w, "],\n      \"ampOn\": %s,\n      \"blocks\": [\n",
            preset->amp_on ? "true" : "false");
        for (unsigned block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
            const coyopedal_block_setting_t* const setting = &preset->blocks[block];
            put(&w, "        { \"block\": \"%s\", \"enabled\": %s, \"params\": [",
                block_names[block], setting->enabled ? "true" : "false");
            for (unsigned value = 0; value < COYOPEDAL_PRESET_PARAMS; ++value) {
                put(&w, "%s%d", value ? ", " : "", (int)setting->params[value]);
            }
            put(&w, "] }%s\n", block + 1U < COYOPEDAL_FX_BLOCK_COUNT ? "," : "");
        }
        put(&w, "      ]\n    }%s\n", index + 1U < count ? "," : "");
    }
    put(&w, "  ]\n}\n");
    return w.full ? 0U : w.length;
}
