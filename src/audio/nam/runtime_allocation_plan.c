#include "runtime_allocation_plan.h"

static uintptr_t aligned_start(uintptr_t start, size_t header) {
    uintptr_t aligned = (start + 15) & ~(uintptr_t)15;
    if (aligned != start && aligned - start < 4 * header)
        aligned = (start + 4 * header + 15) & ~(uintptr_t)15;
    return aligned;
}

static bool place(nam_allocation_plan* plan, size_t depth) {
    if (depth == plan->block_count)
        return true;
    if (++plan->steps > plan->step_limit)
        return false;
    if (plan->yield && (plan->steps & 4095) == 0)
        plan->yield();
    size_t needed = 0;
    for (size_t i = depth; i < plan->block_count; ++i)
        needed += plan->sizes[plan->order[i]];
    const size_t smallest = plan->sizes[plan->order[plan->block_count - 1]];
    size_t available_total = 0;
    for (size_t i = 0; i < plan->region_count; ++i) {
        const nam_memory_region* region = &plan->regions[i];
        size_t available = region->end > region->start ? region->end - region->start : 0;
        if (available >= smallest)
            available_total += available;
    }
    if (available_total < needed)
        return false;
    size_t index = plan->order[depth];
    size_t size = plan->sizes[index];
    // Enumerate fitting regions in increasing remaining capacity. Equal
    // capacities are interchangeable for the remaining aligned requests.
    size_t previous = 0;
    for (;;) {
        size_t best = SIZE_MAX;
        size_t capacity = SIZE_MAX;
        for (size_t i = 0; i < plan->region_count; ++i) {
            nam_memory_region* region = &plan->regions[i];
            size_t available = region->end > region->start ? region->end - region->start : 0;
            if (available >= size && available > previous && available < capacity) {
                best = i;
                capacity = available;
            }
        }
        if (best == SIZE_MAX)
            return false;
        nam_memory_region* region = &plan->regions[best];
        uintptr_t address = region->start;
        plan->addresses[index] = address;
        size_t rounded = (size + plan->header_size - 1) & ~(plan->header_size - 1);
        region->start = aligned_start(address + rounded + plan->header_size, plan->header_size);
        if (place(plan, depth + 1))
            return true;
        region->start = address;
        previous = capacity;
        if (plan->steps > plan->step_limit)
            return false;
    }
}

bool nam_plan_allocations(nam_allocation_plan* plan) {
    if (!plan->block_count || plan->block_count > NAM_PLAN_MAX_BLOCKS ||
        plan->region_count > NAM_PLAN_MAX_REGIONS ||
        (plan->header_size != 4 && plan->header_size != 8))
        return false;
    plan->steps = 0;
    for (size_t i = 0; i < plan->region_count; ++i)
        plan->regions[i].start = aligned_start(plan->regions[i].start, plan->header_size);
    for (size_t i = 0; i < plan->block_count; ++i) {
        if (!plan->sizes[i] || plan->sizes[i] > SIZE_MAX / 2)
            return false;
        plan->order[i] = i;
        plan->addresses[i] = 0;
    }
    for (size_t i = 1; i < plan->block_count; ++i) {
        size_t index = plan->order[i];
        size_t j = i;
        while (j && plan->sizes[plan->order[j - 1]] < plan->sizes[index]) {
            plan->order[j] = plan->order[j - 1];
            --j;
        }
        plan->order[j] = index;
    }
    return place(plan, 0);
}
