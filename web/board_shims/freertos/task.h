#pragma once
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
// The panel's own waits. A page cannot block its main thread, and every caller
// here is polling something that a later frame will see anyway, so this returns
// at once and the poll lands on the next frame instead.
void vTaskDelay(TickType_t ticks);
// The engine keeps one occlusion-coverage bank per core, and picks between them
// whenever a FreeRTOS header is in scope -- which, for the web build, is this
// shim. The page rasterizes on a single thread, so there is only ever core 0.
int xPortGetCoreID(void);
#ifdef __cplusplus
}
#endif
