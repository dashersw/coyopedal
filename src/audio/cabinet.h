#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COYOPEDAL_IR_PATH_MAX 96
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    char path[COYOPEDAL_IR_PATH_MAX]; // relative to /ir on the SD card
    int16_t level;                    // tenths of a dB, -180..60
    bool enabled;
} coyopedal_cabinet_setting_t;

void pedalboard_cabinet_setting(coyopedal_cabinet_setting_t* out);
bool pedalboard_cabinet_apply(const coyopedal_cabinet_setting_t* setting, char* error,
                              size_t capacity);
bool pedalboard_cabinet_enable(bool enabled, char* error, size_t capacity);
bool pedalboard_cabinet_enabled(void);
// Called during a paused model transaction. Keeps the selected IR remembered.
void pedalboard_cabinet_capture_changed(bool includes_cabinet);
void pedalboard_cabinet_timing(unsigned* cycles);
void pedalboard_cabinet_process(float* samples, size_t frames);
unsigned pedalboard_cabinet_begin(float* samples, float* imaginary);
void pedalboard_cabinet_finish(float* samples, float* imaginary, unsigned cursor);
void pedalboard_cabinet_scan(void);
unsigned pedalboard_cabinet_count(void);
const char* pedalboard_cabinet_path(unsigned index);
// Dense 1024-tap diagnostic kernel for the bounded AUDIO TRY window only.
bool pedalboard_cabinet_benchmark(char* error, size_t capacity);
#ifdef __cplusplus
}
#endif
