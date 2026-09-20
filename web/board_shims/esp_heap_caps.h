#pragma once
#include <stddef.h>
// The board asks for SPIRAM because it has two heaps and the preset store is
// too big for the internal one. A wasm module has one heap, so the capability
// bits are accepted and ignored.
#define MALLOC_CAP_8BIT 0x0004
#define MALLOC_CAP_INTERNAL 0x0800
#define MALLOC_CAP_SPIRAM 0x0400
#define MALLOC_CAP_DEFAULT 0x0000
#ifdef __cplusplus
extern "C" {
#endif
void* heap_caps_malloc(size_t size, unsigned caps);
void* heap_caps_calloc(size_t count, size_t size, unsigned caps);
void heap_caps_free(void* pointer);
#ifdef __cplusplus
}
#endif
