#include "heap_map.h"

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// One 32 KiB PSRAM page per slot. The internal heaps hold well under a thousand
// blocks even with the whole audio graph resident; the dropped counter makes an
// overflow visible instead of silently truncating the census.
#define COYOPEDAL_HEAP_MAP_CAPACITY 2048U
#define COYOPEDAL_HEAP_MAP_MAX_HEAPS 32U

typedef struct {
    coyopedal_heap_block_t* blocks;
    size_t count;
    size_t dropped;
    uint32_t uptime_ms;
    uint32_t seen_heaps[COYOPEDAL_HEAP_MAP_MAX_HEAPS];
    size_t seen_count;
} heap_map_slot_t;

static heap_map_slot_t g_slots[COYOPEDAL_HEAP_MAP_SLOTS];

static bool record_block(walker_heap_into_t heap, walker_block_info_t block, void* user_data) {
    heap_map_slot_t* const slot = (heap_map_slot_t*)user_data;
    if (slot->count >= COYOPEDAL_HEAP_MAP_CAPACITY) {
        ++slot->dropped;
        return true;
    }
    coyopedal_heap_block_t* const entry = &slot->blocks[slot->count++];
    entry->address = (uint32_t)(uintptr_t)block.ptr;
    entry->size = (uint32_t)block.size;
    entry->heap_start = (uint32_t)(uintptr_t)heap.start;
    entry->used = block.used ? 1U : 0U;
    return true;
}

// The RTC walk runs after the internal one and must not duplicate a heap that
// reports both capabilities. The heap identity is its start address.
static bool record_new_heap_block(walker_heap_into_t heap, walker_block_info_t block,
                                  void* user_data) {
    heap_map_slot_t* const slot = (heap_map_slot_t*)user_data;
    const uint32_t start = (uint32_t)(uintptr_t)heap.start;
    for (size_t index = 0; index < slot->seen_count; ++index) {
        if (slot->seen_heaps[index] == start)
            return false;
    }
    return record_block(heap, block, user_data);
}

static void note_heaps(heap_map_slot_t* const slot, const size_t from) {
    for (size_t index = from; index < slot->count; ++index) {
        const uint32_t start = slot->blocks[index].heap_start;
        bool known = false;
        for (size_t seen = 0; seen < slot->seen_count; ++seen)
            known = known || slot->seen_heaps[seen] == start;
        if (!known && slot->seen_count < COYOPEDAL_HEAP_MAP_MAX_HEAPS)
            slot->seen_heaps[slot->seen_count++] = start;
    }
}

size_t coyopedal_heap_map_capture(const unsigned slot_index) {
    if (slot_index >= COYOPEDAL_HEAP_MAP_SLOTS)
        return 0U;
    heap_map_slot_t* const slot = &g_slots[slot_index];
    if (slot->blocks == NULL) {
        slot->blocks = (coyopedal_heap_block_t*)heap_caps_calloc(
            COYOPEDAL_HEAP_MAP_CAPACITY, sizeof(coyopedal_heap_block_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (slot->blocks == NULL)
            return 0U;
    }
    slot->count = 0U;
    slot->dropped = 0U;
    slot->seen_count = 0U;
    slot->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    heap_caps_walk(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, record_block, slot);
    note_heaps(slot, 0U);
    heap_caps_walk(MALLOC_CAP_RTCRAM, record_new_heap_block, slot);
    return slot->count;
}

size_t coyopedal_heap_map_count(const unsigned slot) {
    return slot < COYOPEDAL_HEAP_MAP_SLOTS ? g_slots[slot].count : 0U;
}

size_t coyopedal_heap_map_dropped(const unsigned slot) {
    return slot < COYOPEDAL_HEAP_MAP_SLOTS ? g_slots[slot].dropped : 0U;
}

uint32_t coyopedal_heap_map_uptime_ms(const unsigned slot) {
    return slot < COYOPEDAL_HEAP_MAP_SLOTS ? g_slots[slot].uptime_ms : 0U;
}

const coyopedal_heap_block_t* coyopedal_heap_map_blocks(const unsigned slot) {
    return slot < COYOPEDAL_HEAP_MAP_SLOTS ? g_slots[slot].blocks : NULL;
}

// The census above needs PSRAM storage and a maintenance HTTP round trip. A
// boot-time arena failure has neither: it happens before the radios, and the
// answer it needs is small -- which blocks are already standing, and in which
// heap. This walks under the multi-heap lock and allocates nothing, keeping the
// top blocks in a fixed array on the caller's stack.
#define COYOPEDAL_CENSUS_MIN_BLOCK 1024U
#define COYOPEDAL_CENSUS_TOP 24U

typedef struct {
    uint32_t address;
    uint32_t size;
} census_entry_t;

typedef struct {
    census_entry_t top[COYOPEDAL_CENSUS_TOP];
    size_t count;
    size_t used_total;
    size_t free_total;
    size_t block_count;
} census_t;

static bool census_block(walker_heap_into_t heap, walker_block_info_t block, void* user_data) {
    (void)heap;
    census_t* const census = (census_t*)user_data;
    ++census->block_count;
    if (!block.used) {
        census->free_total += block.size;
        return true;
    }
    census->used_total += block.size;
    if (block.size < COYOPEDAL_CENSUS_MIN_BLOCK)
        return true;
    // Insertion sort into a fixed top-N, largest first: the tail is the part
    // nobody can act on anyway.
    size_t position =
        census->count < COYOPEDAL_CENSUS_TOP ? census->count : COYOPEDAL_CENSUS_TOP - 1;
    if (census->count == COYOPEDAL_CENSUS_TOP && block.size <= census->top[position].size)
        return true;
    while (position > 0 && census->top[position - 1].size < block.size) {
        census->top[position] = census->top[position - 1];
        --position;
    }
    census->top[position].address = (uint32_t)(uintptr_t)block.ptr;
    census->top[position].size = (uint32_t)block.size;
    if (census->count < COYOPEDAL_CENSUS_TOP)
        ++census->count;
    return true;
}

// Which task a block belongs to, when it is a task stack or TCB. The task list
// goes in PSRAM: this runs on the main task, whose internal stack is small.
#define COYOPEDAL_CENSUS_TASKS 40U

static const char* census_owner(const TaskStatus_t* const tasks, const UBaseType_t count,
                                const uint32_t address, const uint32_t size) {
    for (UBaseType_t i = 0; i < count; ++i) {
        const uint32_t stack = (uint32_t)(uintptr_t)tasks[i].pxStackBase;
        const uint32_t tcb = (uint32_t)(uintptr_t)tasks[i].xHandle;
        if ((stack >= address && stack < address + size) ||
            (tcb >= address && tcb < address + size))
            return tasks[i].pcTaskName;
    }
    return "-";
}

void coyopedal_log_internal_heap_census(const char* const label) {
    census_t census = {0};
    heap_caps_walk(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, census_block, &census);
    ESP_LOGI("heap_census", "%s: blocks=%u used=%u free=%u", label, (unsigned)census.block_count,
             (unsigned)census.used_total, (unsigned)census.free_total);
#if configUSE_TRACE_FACILITY
    TaskStatus_t* const tasks = (TaskStatus_t*)heap_caps_malloc(
        COYOPEDAL_CENSUS_TASKS * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const UBaseType_t task_count =
        tasks != NULL ? uxTaskGetSystemState(tasks, COYOPEDAL_CENSUS_TASKS, NULL) : 0U;
#else
    const TaskStatus_t* const tasks = NULL;
    const UBaseType_t task_count = 0U;
#endif
    for (size_t i = 0; i < census.count; ++i) {
        const uint32_t address = census.top[i].address;
        const uint32_t size = census.top[i].size;
        const char* const owner = census_owner(tasks, task_count, address, size);
        if (owner[0] != '-') {
            ESP_LOGI("heap_census", "%s: used %p %u %s", label, (void*)(uintptr_t)address,
                     (unsigned)size, owner);
            continue;
        }
        // No task owns it: print its first and last words, which usually
        // name the owner by the pointers and constants it holds.
        const uint32_t* const w = (const uint32_t*)(uintptr_t)address;
        const uint32_t* const e = (const uint32_t*)(uintptr_t)(address + size - 16U);
        ESP_LOGI("heap_census",
                 "%s: used %p %u - head %08lx %08lx %08lx %08lx %08lx %08lx tail %08lx %08lx "
                 "%08lx %08lx",
                 label, (void*)(uintptr_t)address, (unsigned)size, (unsigned long)w[0],
                 (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4],
                 (unsigned long)w[5], (unsigned long)e[0], (unsigned long)e[1], (unsigned long)e[2],
                 (unsigned long)e[3]);
    }
    for (UBaseType_t i = 0; i < task_count; ++i) {
        if (esp_ptr_internal(tasks[i].pxStackBase)) {
            ESP_LOGI("heap_census", "%s: internal stack %s at %p", label, tasks[i].pcTaskName,
                     (void*)tasks[i].pxStackBase);
        }
    }
#if configUSE_TRACE_FACILITY
    heap_caps_free(tasks);
#endif
}
