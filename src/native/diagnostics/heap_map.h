// Byte-exact internal-SRAM census.
//
// `heap_caps_get_free_size` reports a total; it cannot say which allocation
// consumed the rest. This captures the complete block list of every internal
// heap into a PSRAM snapshot that the maintenance service sends as `HEAP MAP`.
// Audio boots, where the radios are down, log a census instead.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COYOPEDAL_HEAP_MAP_SLOT_LIVE 0U
#define COYOPEDAL_HEAP_MAP_SLOTS 1U

typedef struct {
    uint32_t address;
    uint32_t size;
    uint32_t heap_start;
    uint32_t used;
} coyopedal_heap_block_t;

// Records every block of every heap carrying MALLOC_CAP_INTERNAL, then every
// block of the RTC heaps that the first walk did not already cover. Storage is
// claimed from PSRAM before the walk begins, because the walk runs under the
// multi-heap lock and must not allocate. Returns the number of blocks stored.
size_t coyopedal_heap_map_capture(unsigned slot);

// Logs the internal heaps' block count and used and free totals, then the 24
// largest used blocks of 1 KiB or more, each with the task it belongs to (or its
// first and last words when no task owns it), then every task whose stack is in
// internal SRAM. When the model does not fit, the useful question is which
// allocations were already standing; `label` names the moment. Allocates
// nothing from internal SRAM.
void coyopedal_log_internal_heap_census(const char* label);

size_t coyopedal_heap_map_count(unsigned slot);
size_t coyopedal_heap_map_dropped(unsigned slot);
uint32_t coyopedal_heap_map_uptime_ms(unsigned slot);
const coyopedal_heap_block_t* coyopedal_heap_map_blocks(unsigned slot);

#ifdef __cplusplus
}
#endif
