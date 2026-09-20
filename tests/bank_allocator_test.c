#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tlsf.h"
#include "runtime_allocation_plan.h"

// The device supplies optional heap-poisoning hooks; this test uses no poison.
bool tlsf_check_hook(void* start, size_t size, bool is_free) {
    (void)start;
    (void)size;
    (void)is_free;
    return true;
}
void block_absorb_post_hook(void* start, size_t size, bool is_free) {
    (void)start;
    (void)size;
    (void)is_free;
}

void* __wrap_tlsf_memalign_offs(tlsf_t, size_t, size_t, size_t);
void* pedalboard_tlsf_best_fit(tlsf_t, size_t, size_t);
void* __real_tlsf_memalign_offs(tlsf_t heap, size_t align, size_t size, size_t offset) {
    return tlsf_memalign_offs(heap, align, size, offset);
}

static unsigned char storage[196608];

static bool collect_regions(void* ptr, size_t size, int used, void* context) {
    nam_allocation_plan* plan = context;
    if (!used && size >= 512) {
        assert(plan->region_count < NAM_PLAN_MAX_REGIONS);
        plan->regions[plan->region_count++] =
            (nam_memory_region){(uintptr_t)ptr, (uintptr_t)ptr + size};
    }
    return true;
}

int main(void) {
    unsigned char* arena = (unsigned char*)(((uintptr_t)storage + 32767) & ~(uintptr_t)32767);
    // A 48 KiB free region contains an entire aligned coefficient bank, but
    // cannot satisfy TLSF's pessimistic 64 KiB search request.
    // A 64 KiB maximum also keeps IDF's four-byte bitmap metadata aligned
    // for pointer-sized fields in this 64-bit host test. The pool is 48 KiB.
    tlsf_t heap = tlsf_create_with_pool(arena + 16384, 49152, 65536);
    assert(heap);
    assert(tlsf_check(heap) == 0);
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        assert(tlsf_memalign_offs(heap, 32768, 19392, 0) == NULL);
        void* table = __wrap_tlsf_memalign_offs(heap, 32768, 19392, 0);
        assert(table == arena + 32768);
        memset(table, 0x5a, 19392);
        assert(__wrap_tlsf_memalign_offs(heap, 32768, 14272, 0) == NULL);
        assert(__wrap_tlsf_memalign_offs(heap, 32768, 0, 0) == NULL);
        assert(__wrap_tlsf_memalign_offs(heap, 32768, 19392, 4) == NULL);
        void* small = __wrap_tlsf_memalign_offs(heap, 16, 1024, 0);
        assert(small && ((uintptr_t)small % 16) == 0);
        tlsf_free(heap, small);
        tlsf_free(heap, table);
        assert(tlsf_check(heap) == 0);
        assert(tlsf_check_pool(tlsf_get_pool(heap)) == 0);
    }
    puts("Bank allocator: 100 fragmented allocation/free cycles passed");

    // The SDK can satisfy this request from the large first free region, but
    // doing so needlessly splits space needed by a later history allocation.
    // Prefer the smaller region after the guard, despite its sub-64 KiB size.
    heap = tlsf_create_with_pool(arena + 16384, 147456, 262144);
    assert(heap && tlsf_check(heap) == 0);
    void* guard = tlsf_malloc_addr(heap, 4096, arena + 98304);
    assert(guard == arena + 98304);
    void* sdk_table = tlsf_memalign_offs(heap, 32768, 19392, 0);
    assert(sdk_table == arena + 32768);
    tlsf_free(heap, sdk_table);
    void* table = __wrap_tlsf_memalign_offs(heap, 32768, 19392, 0);
    assert(table == arena + 131072);
    void* history = tlsf_malloc(heap, 70000);
    assert(history);
    tlsf_free(heap, history);
    tlsf_free(heap, table);
    tlsf_free(heap, guard);
    assert(tlsf_check(heap) == 0);
    assert(tlsf_check_pool(tlsf_get_pool(heap)) == 0);
    puts("Bank allocator: preserves large history regions");

    // Two almost-equal holes share a TLSF bucket. The 30 KiB low history must
    // use the smaller one: only the larger hole fits a 21 KiB + 10 KiB pair.
    heap = tlsf_create_with_pool(arena, 131072, 262144);
    void* guard1 = tlsf_malloc_addr(heap, 1024, arena + 34000);
    void* guard2 = tlsf_malloc_addr(heap, 1024, arena + 66304);
    void* tail = tlsf_malloc_addr(heap, 60000, arena + 67344);
    assert(guard1 && guard2 && tail);
    void* low = pedalboard_tlsf_best_fit(heap, 16, 30344);
    assert(low && (uintptr_t)low > (uintptr_t)guard1);
    void* large = pedalboard_tlsf_best_fit(heap, 16, 21168);
    void* medium = pedalboard_tlsf_best_fit(heap, 16, 10128);
    assert(large && medium);
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        memset(low, 0x6a, 30344);
        memset(large, 0x7b, 21168);
        memset(medium, 0x8c, 10128);
        tlsf_free(heap, low);
        tlsf_free(heap, large);
        tlsf_free(heap, medium);
        assert(tlsf_check(heap) == 0);
        assert(tlsf_check_pool(tlsf_get_pool(heap)) == 0);
        low = pedalboard_tlsf_best_fit(heap, 16, 30344);
        large = pedalboard_tlsf_best_fit(heap, 16, 21168);
        medium = pedalboard_tlsf_best_fit(heap, 16, 10128);
        assert(low && large && medium);
    }
    puts("NAM allocator: 100 tightly packed history reloads passed");

    // Exact free-region snapshot from the AMOLED after radio shutdown and
    // coefficient allocation. Greedy history placement fails on these holes.
    nam_allocation_plan plan = {
        .regions = {{0x3fcc24ac, 0x3fcc9c44},
                    {0x600fe180, 0x600fffe4},
                    {0x3fce9f30, 0x3fcea2b8},
                    {0x3fcea448, 0x3fcecb00},
                    {0x3fcecb20, 0x3fcedff0},
                    {0x3fcee010, 0x3fceeb10},
                    {0x3fcf02e4, 0x3fcf7d60},
                    {0x3fcf7dcc, 0x3fcf7ffc},
                    {0x3fcbf45c, 0x3fcbf9dc},
                    {0x3fcc08a0, 0x3fcc0bec},
                    {0x3fcca1e0, 0x3fccfeb8},
                    {0x3fce4280, 0x3fce918c},
                    {0x3fce9298, 0x3fce9530},
                    {0x3fcd37c4, 0x3fcd7eb0},
                    {0x3fcdcbc4, 0x3fce4268}},
        .sizes = {30344, 1064, 21168, 21168, 21168, 10128, 10128, 10128, 5328,
                  5328,  5328, 4960,  3408,  3408,  3408,  2608,  2608,  2608,
                  2288,  2288, 2288,  2272,  2128,  2128,  2128},
        .region_count = 15,
        .block_count = 25,
        .header_size = 4,
        .step_limit = 1000000,
    };
    nam_allocation_plan before = plan;
    assert(nam_plan_allocations(&plan));
    for (size_t i = 0; i < plan.block_count; ++i) {
        uintptr_t start = plan.addresses[i];
        uintptr_t end = start + plan.sizes[i];
        assert(start % 16 == 0);
        bool contained = false;
        for (size_t j = 0; j < before.region_count; ++j)
            contained |= start >= before.regions[j].start && end <= before.regions[j].end;
        assert(contained);
        for (size_t j = 0; j < i; ++j)
            assert(end <= plan.addresses[j] || start >= plan.addresses[j] + plan.sizes[j]);
    }
    printf("NAM planner: all 25 histories fit the hardware snapshot in %zu steps\n", plan.steps);
    plan = before;
    plan.step_limit = 1;
    assert(!nam_plan_allocations(&plan));
    plan = before;
    plan.sizes[0] = 1000000;
    assert(!nam_plan_allocations(&plan));

    // Allocate/free the plan using real SDK TLSF, checking metadata integrity.
    for (unsigned cycle = 0; cycle < 10; ++cycle) {
        heap = tlsf_create_with_pool(arena, 131072, 262144);
        nam_allocation_plan actual = {
            .block_count = 25, .header_size = sizeof(size_t), .step_limit = 1000000};
        // Smaller sample sizes fit this single host pool with fragmentation.
        for (size_t i = 0; i < actual.block_count; ++i)
            actual.sizes[i] = 512 + (i % 5) * 1024;
        // Model-owned bank-aligned history and Core 1 coefficient storage
        // are excluded from the fallback plan and must survive it intact.
        void* pin1 = tlsf_malloc_addr(heap, 21168, arena + 32768);
        void* pin2 = tlsf_malloc_addr(heap, 19392, arena + 65536);
        assert(pin1 && pin2);
        memset(pin1, 0x33, 21168);
        memset(pin2, 0x77, 19392);
        tlsf_walk_pool(tlsf_get_pool(heap), collect_regions, &actual);
        assert(nam_plan_allocations(&actual));
        void* blocks[NAM_PLAN_MAX_BLOCKS] = {0};
        for (size_t i = 0; i < actual.block_count; ++i) {
            size_t index = actual.order[i];
            blocks[index] =
                tlsf_malloc_addr(heap, actual.sizes[index], (void*)actual.addresses[index]);
            assert(blocks[index] == (void*)actual.addresses[index]);
            memset(blocks[index], 0x9d, actual.sizes[index]);
        }
        for (size_t i = 0; i < actual.block_count; ++i)
            tlsf_free(heap, blocks[i]);
        for (size_t i = 0; i < 21168; ++i)
            assert(((unsigned char*)pin1)[i] == 0x33);
        for (size_t i = 0; i < 19392; ++i)
            assert(((unsigned char*)pin2)[i] == 0x77);
        tlsf_free(heap, pin1);
        tlsf_free(heap, pin2);
        assert(tlsf_check(heap) == 0);
        assert(tlsf_check_pool(tlsf_get_pool(heap)) == 0);
    }
    puts("NAM planner: real TLSF allocation/free integrity passed");
}
