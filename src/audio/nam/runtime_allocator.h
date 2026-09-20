#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
// Platform implementation: zeroed, aligned internal SRAM with best-fit
// placement for NAM. Returned memory is owned by NAM and freed normally.
bool coyopedal_nam_internal_batch(size_t count, const size_t* sizes, void** blocks);
#ifdef __cplusplus
}
#endif
