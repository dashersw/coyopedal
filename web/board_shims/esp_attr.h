#pragma once
// Placement attributes. Every one of them answers "which memory does this live
// in", which a wasm module does not get to decide.
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define RTC_DATA_ATTR
#define NOINIT_ATTR
#define FORCE_INLINE_ATTR inline
