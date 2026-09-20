#ifndef COYOPEDAL_PEDAL_MODEL_CATALOG_H_
#define COYOPEDAL_PEDAL_MODEL_CATALOG_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Catalogue of the embedded factory amp, persisted imports and SD models.
// tools/models_to_bin.py packs assets/models/factory.json for the firmware.
// The index lives in RAM; model data is read into staging only when selected.
// The active engine accepts compatible A2-Full profiles.

#define COYOPEDAL_MODEL_ID_MAX 24
#define COYOPEDAL_MODEL_NAME_MAX 32

// Shared catalogue capacity across all model sources.
#define COYOPEDAL_MODEL_MAX 256

typedef struct {
    char id[COYOPEDAL_MODEL_ID_MAX];
    char name[COYOPEDAL_MODEL_NAME_MAX];
    unsigned offset; // byte offset into the library image
    unsigned size;
    bool sd_model; // read from the SD card
    bool sd_json;
    // Path under the card's /nam directory, folders included: the browser
    // shows the card's own tree.
    char sd_filename[128];
    bool user_model; // resolves the separate user partition on S3
} coyopedal_model_t;

extern coyopedal_model_t coyopedal_models[COYOPEDAL_MODEL_MAX];
extern unsigned coyopedal_model_count;

// Reads the library index out of the library image. Leaves the count at zero if the
// flash holds no valid container, which is what an unprogrammed board looks like;
// the pedal then runs in pass-through and the panel can say so rather than
// pretending a profile is loaded.
bool coyopedal_models_init(void);

// Loads the profile at `index` into the DSP. Returns false and leaves the
// processor in pass-through if the index is out of range or the image fails
// header, size, checksum or architecture validation.
//
// Not real-time safe: it reads the capture out of storage, validates it and
// rewrites the whole weight set, so it must be called from the UI task rather
// than from audio.
//
// On failure, `error` receives a short reason suitable for the panel. Pass NULL
// to discard it.
bool coyopedal_load_model(unsigned index, char* error, size_t error_capacity);

// Re-reads the whole index after the storage underneath it changed - a card
// inserted or removed, a folder picked in the browser. The profile currently
// playing keeps playing: its weights are resident and nothing here touches the
// DSP. What moves is its INDEX, because the catalogue is rebuilt from scratch
// and entries before it may have come or gone, so the active entry is found
// again by id. A profile whose file is gone leaves the selection empty while
// the sound continues, which is what has actually happened.
void coyopedal_models_rescan(void);

// Index of the profile currently loaded, or COYOPEDAL_MODEL_NONE if the last
// load failed and the processor is passing through.
unsigned coyopedal_active_model(void);
const char* coyopedal_last_model_error(void);

#define COYOPEDAL_MODEL_NONE 0xFFFFFFFFU

#ifdef __cplusplus
}
#endif

#endif // COYOPEDAL_PEDAL_MODEL_CATALOG_H_
