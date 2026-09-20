#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
// The board's key-value store, backed by the browser's. panel_presets.cpp keeps
// the player's presets here, so they have to outlive a reload the way they
// outlive a power cycle -- see web/board_platform.cpp.
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out, size_t* size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t size);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* out);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value);
#ifdef __cplusplus
}
#endif
