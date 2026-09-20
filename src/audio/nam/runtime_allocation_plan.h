#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NAM_PLAN_MAX_BLOCKS 32
#define NAM_PLAN_MAX_REGIONS 128

typedef struct {
    uintptr_t start;
    uintptr_t end;
} nam_memory_region;

typedef struct {
    nam_memory_region regions[NAM_PLAN_MAX_REGIONS];
    size_t sizes[NAM_PLAN_MAX_BLOCKS];
    uintptr_t addresses[NAM_PLAN_MAX_BLOCKS];
    size_t order[NAM_PLAN_MAX_BLOCKS];
    size_t region_count;
    size_t block_count;
    size_t header_size;
    size_t steps;
    size_t step_limit;
    void (*yield)(void);
} nam_allocation_plan;

// Snapshot-only planner: never owns or reserves memory. The platform must
// revalidate each planned address under the heap lock before allocating it.
bool nam_plan_allocations(nam_allocation_plan* plan);
