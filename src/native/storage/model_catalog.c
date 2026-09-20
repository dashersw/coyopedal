#include "model_catalog.h"

#include <string.h>

#include "flash_storage.h"
#include "audio/usb_frame_processor.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "sd_models.h"

static unsigned active_model = COYOPEDAL_MODEL_NONE;

// The library index, read out of the library image at boot. Zero entries is what an
// unprogrammed board looks like.
EXT_RAM_BSS_ATTR
coyopedal_model_t coyopedal_models[COYOPEDAL_MODEL_MAX];
unsigned coyopedal_model_count = 0;

// One profile's worth of staging, so a load is one flash read followed by the normal
// bounded loader. Deliberately not in internal RAM: it is touched once per profile
// change, and internal RAM is full of weights. A2-Full profiles are 48,616 bytes;
// entries larger than this are left out of the library.
#define MODEL_STAGING_MAX 65536U
EXT_RAM_BSS_ATTR
static unsigned char staging[MODEL_STAGING_MAX];

// The factory image's shape, which has to match tools/models_to_bin.py.
#define CONTAINER_HEADER 16U
#define CONTAINER_ENTRY 64U
#define CONTAINER_VERSION 1U

static unsigned read_u32(const unsigned char* const bytes) {
    return (unsigned)bytes[0] | ((unsigned)bytes[1] << 8) | ((unsigned)bytes[2] << 16) |
           ((unsigned)bytes[3] << 24);
}

// Imported profiles live in the user_models partition; model_import.cpp reads
// them. They are appended to the same coyopedal_models[] array, after the factory
// ones, so nothing downstream - the panel, the control plane, a preset naming a
// profile by id - has to know which storage an entry came from.
void coyopedal_s3_read_imported_models(void);

bool coyopedal_models_init(void) {
    coyopedal_model_count = 0;

    unsigned char header[CONTAINER_HEADER];
    if (!coyopedal_factory_models_read(0U, header, sizeof header)) {
        return false;
    }
    if (header[0] != 'T' || header[1] != 'M' || header[2] != 'D' || header[3] != 'L') {
        // No container. An erased device reads 0xFF, so this is also what a board
        // that has never been given a library looks like.
        return false;
    }
    const unsigned version = (unsigned)header[4] | ((unsigned)header[5] << 8);
    unsigned count = (unsigned)header[6] | ((unsigned)header[7] << 8);
    if (version != CONTAINER_VERSION || count == 0U) {
        return false;
    }
    if (count > COYOPEDAL_MODEL_MAX) {
        // Read what fits rather than refusing the lot; the panel then shows a
        // shorter library instead of none.
        count = COYOPEDAL_MODEL_MAX;
    }

    for (unsigned index = 0; index < count; ++index) {
        unsigned char entry[CONTAINER_ENTRY];
        if (!coyopedal_factory_models_read(CONTAINER_HEADER + index * CONTAINER_ENTRY, entry,
                                           sizeof entry)) {
            return false;
        }
        coyopedal_model_t* const model = &coyopedal_models[coyopedal_model_count];
        model->user_model = false;
        model->sd_model = false;
        model->sd_json = false;
        model->sd_filename[0] = '\0';
        model->offset = read_u32(&entry[0]);
        model->size = read_u32(&entry[4]);
        if (model->size == 0U || model->size > MODEL_STAGING_MAX) {
            // Skip an entry this firmware cannot stage rather than failing the
            // whole library.
            continue;
        }
        memcpy(model->id, &entry[8], COYOPEDAL_MODEL_ID_MAX);
        model->id[COYOPEDAL_MODEL_ID_MAX - 1U] = '\0';
        memcpy(model->name, &entry[32], COYOPEDAL_MODEL_NAME_MAX);
        model->name[COYOPEDAL_MODEL_NAME_MAX - 1U] = '\0';
        ++coyopedal_model_count;
    }

    coyopedal_s3_read_imported_models();
    pedalboard_sd_models_scan();
    return coyopedal_model_count > 0U;
}

static void copy_error(char* out, size_t capacity, const char* message) {
    if (out == NULL || capacity == 0U) {
        return;
    }
    strncpy(out, message, capacity - 1U);
    out[capacity - 1U] = '\0';
}

static bool load_model_impl(const unsigned index, char* const error, const size_t error_capacity) {
    if (index >= coyopedal_model_count) {
        copy_error(error, error_capacity, "no such profile");
        return false;
    }

    const coyopedal_model_t* const model = &coyopedal_models[index];

    // Pull the profile out of storage first. A read failure here is a
    // storage fault rather than a bad profile, so it is reported separately.
    const bool read_ok =
        model->sd_model
            ? pedalboard_sd_model_read(model, staging, sizeof staging, error, error_capacity)
        : model->user_model ? coyopedal_flash_store_read(model->offset, staging, model->size)
                            : coyopedal_factory_models_read(model->offset, staging, model->size);
    if (!read_ok) {
        if (!model->sd_model)
            copy_error(error, error_capacity, "model storage read failed");
        return false;
    }

    // The DSP's own message is the useful one — it distinguishes a checksum
    // mismatch from a unsupported architecture — so it is passed through rather
    // than replaced with a generic failure.
    char reason[64] = {0};
    if (!coyopedal_pedal_dsp_load_namb(staging, (size_t)model->size, reason, sizeof reason)) {
        // A rejected profile leaves the processor in pass-through. Forgetting the
        // previous index matters: the panel must not keep showing a name whose
        // weights are no longer resident.
        // The S3 wrapper restores the embedded factory amp on engine failure.
        // Report the amp actually producing audio instead of an empty selection.
        active_model = coyopedal_pedal_dsp_model_loaded() && coyopedal_model_count > 0U
                           ? 0U
                           : COYOPEDAL_MODEL_NONE;
        copy_error(error, error_capacity, reason);
        ESP_LOGE("models", "profile %s rejected: %s", model->id, reason);
        return false;
    }

    active_model = index;
    ESP_LOGI("models", "profile %s loaded, %u bytes", model->id, model->size);
    return true;
}

void coyopedal_models_rescan(void) {
    char previous[COYOPEDAL_MODEL_ID_MAX] = {0};
    if (active_model < coyopedal_model_count) {
        memcpy(previous, coyopedal_models[active_model].id, sizeof previous);
    }
    coyopedal_models_init();
    if (previous[0] == '\0') {
        return;
    }
    // The index is meaningless across a rebuild; the id is not.
    active_model = COYOPEDAL_MODEL_NONE;
    for (unsigned index = 0; index < coyopedal_model_count; ++index) {
        if (strcmp(coyopedal_models[index].id, previous) == 0) {
            active_model = index;
            return;
        }
    }
}

unsigned coyopedal_active_model(void) {
    return active_model;
}

static char last_model_error[96];
const char* coyopedal_last_model_error(void) {
    return last_model_error;
}
bool coyopedal_load_model(unsigned index, char* error, size_t capacity) {
    last_model_error[0] = '\0';
    const bool ok = load_model_impl(index, last_model_error, sizeof last_model_error);
    copy_error(error, capacity, last_model_error);
    return ok;
}
