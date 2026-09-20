#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Microseconds since the module started, from the page's monotonic clock.
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
