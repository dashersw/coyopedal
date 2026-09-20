#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "model_catalog.h"
#ifdef __cplusplus
extern "C" {
#endif
void pedalboard_sd_models_scan(void);
bool pedalboard_sd_model_read(const coyopedal_model_t* model, unsigned char* out, size_t capacity,
                              char* error, size_t error_capacity);
#ifdef __cplusplus
}
#endif
