#include <stdint.h>
#include "tlsf.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio/nam/runtime_allocator.h"

#include "audio/nam/runtime_allocation_plan.h"

static _Thread_local uintptr_t nam_target;

static bool snapshot_region(walker_heap_into_t heap, walker_block_info_t block, void* context) {
    (void)heap;
    nam_allocation_plan* plan = context;
    if (!block.used && block.size >= 512 && plan->region_count < NAM_PLAN_MAX_REGIONS) {
        nam_memory_region* region = &plan->regions[plan->region_count++];
        region->start = (uintptr_t)block.ptr;
        region->end = region->start + block.size;
    }
    return true;
}

static void planner_yield(void) {
    vTaskDelay(1);
}

bool coyopedal_nam_internal_batch(size_t count, const size_t* sizes, void** blocks) {
    if (!count || count > NAM_PLAN_MAX_BLOCKS)
        return false;
    for (size_t i = 0; i < count; ++i)
        blocks[i] = NULL;
    nam_allocation_plan* plan = heap_caps_calloc(1, sizeof(*plan), MALLOC_CAP_SPIRAM);
    if (!plan)
        return false;
    // RTC fast memory is an internal heap region on the peripheral bus, and
    // the one without MALLOC_CAP_DMA. A history planned into it runs its
    // convolution at APB speed and eats into the block budget. Plan against
    // the real DRAM regions first; the RTC-capable set is the last resort that
    // keeps the graph loadable when DRAM alone cannot hold it, and says so.
    static const uint32_t caps_sets[] = {MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT};
    bool complete = false;
    for (size_t set = 0; set < sizeof caps_sets / sizeof caps_sets[0] && !complete; ++set) {
        const uint32_t caps = caps_sets[set];
        for (unsigned attempt = 0; attempt < 3 && !complete; ++attempt) {
            plan->region_count = 0;
            plan->block_count = count;
            plan->header_size = sizeof(size_t);
            plan->step_limit = 1000000;
            plan->yield = planner_yield;
            for (size_t i = 0; i < count; ++i)
                plan->sizes[i] = sizes[i];
            heap_caps_walk(caps, snapshot_region, plan);
            if (!nam_plan_allocations(plan)) {
                if (set == 0) {
                    ESP_LOGW("nam",
                             "history plan does not fit DRAM alone: regions=%u steps=%u; "
                             "replanning with RTC fast memory",
                             (unsigned)plan->region_count, (unsigned)plan->steps);
                    break;
                }
                ESP_LOGE("nam", "history plan failed: regions=%u steps=%u",
                         (unsigned)plan->region_count, (unsigned)plan->steps);
                break;
            }
            ESP_LOGI("nam", "history plan ready: steps=%u%s", (unsigned)plan->steps,
                     set == 0 ? "" : " (RTC fast memory in play)");
            complete = true;
            for (size_t i = 0; i < count; ++i) {
                size_t index = plan->order[i];
                nam_target = plan->addresses[index];
                blocks[index] = heap_caps_aligned_calloc(16, 1, sizes[index], caps);
                nam_target = 0;
                if (!blocks[index]) {
                    ESP_LOGW("nam", "history plan changed before allocation %u", (unsigned)index);
                    complete = false;
                    break;
                }
            }
            if (!complete) {
                for (size_t i = 0; i < count; ++i) {
                    heap_caps_free(blocks[i]);
                    blocks[i] = NULL;
                }
            }
        }
    }
    heap_caps_free(plan);
    return complete;
}
#else
#define IRAM_ATTR
#endif

void* __real_tlsf_memalign_offs(tlsf_t heap, size_t align, size_t size, size_t offset);

typedef struct {
    size_t size;
    size_t alignment;
    size_t block_size;
    void* address;
} bank_search;

static bool IRAM_ATTR find_bank(void* ptr, size_t bytes, int used, void* context) {
    bank_search* search = context;
    if (used)
        return true;
    const uintptr_t start = (uintptr_t)ptr;
    const uintptr_t bank = search->alignment;
    uintptr_t aligned = (start + bank - 1) & ~(bank - 1);
    // Leave enough room for a free-block header when splitting the prefix.
    if (aligned != start && aligned - start < 4 * sizeof(void*))
        aligned = (start + 4 * sizeof(void*) + bank - 1) & ~(bank - 1);
    if (aligned - start <= bytes && search->size <= bytes - (aligned - start) &&
        bytes < search->block_size) {
        search->address = (void*)aligned;
        search->block_size = bytes;
    }
    return true;
}

// Called under the SDK multi-heap lock; tlsf_malloc_addr keeps normal block
// splitting and free/accounting semantics. Also exercised against SDK TLSF by
// the host regression tests.
void* IRAM_ATTR pedalboard_tlsf_best_fit(tlsf_t heap, size_t align, size_t size) {
    bank_search search = {.size = size, .alignment = align, .block_size = SIZE_MAX};
    tlsf_walk_pool(tlsf_get_pool(heap), find_bank, &search);
    if (!search.address)
        return NULL;
    return tlsf_malloc_addr(heap, size, search.address);
}

#ifdef ESP_PLATFORM
static bool IRAM_ATTR find_target(void* ptr, size_t bytes, int used, void* context) {
    bank_search* search = context;
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t target = (uintptr_t)search->address;
    if (!used && target >= start && target - start <= bytes &&
        search->size <= bytes - (target - start) &&
        (target == start || target - start >= 4 * sizeof(void*)))
        search->block_size = bytes;
    return true;
}
#endif

// Keep SDK accounting and freeing. The desired address is scoped to the NAM
// caller, then checked again under the heap lock so stale snapshots cannot
// allocate over another task's memory. Ordinary allocations are unchanged.
void* IRAM_ATTR __wrap_tlsf_memalign_offs(tlsf_t heap, size_t align, size_t size, size_t offset) {
#ifdef ESP_PLATFORM
    if (!xPortInIsrContext() && nam_target) {
        if (align != 16 || offset != 0)
            return NULL;
        bank_search search = {.size = size, .address = (void*)nam_target};
        tlsf_walk_pool(tlsf_get_pool(heap), find_target, &search);
        return search.block_size ? tlsf_malloc_addr(heap, size, search.address) : NULL;
    }
#endif
    // TLSF otherwise requests 64 KiB for sub-32 KiB bank-aligned coefficients.
    if (offset != 0 || size == 0 || align != 32768 || size > 32768)
        return __real_tlsf_memalign_offs(heap, align, size, offset);
    return pedalboard_tlsf_best_fit(heap, align, size);
}
