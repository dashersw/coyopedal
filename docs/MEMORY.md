# Memory placement

Internal SRAM is the scarcest resource on this pedal. The amp model needs about
213 KB of it, and where each buffer lands decides both whether the model loads
and whether each block finishes on time. [ARCHITECTURE.md](ARCHITECTURE.md)
describes the system around it.

## The memory map

The ESP32-S3 has 512 KiB of internal SRAM, 8 KiB of RTC fast memory and, on this
board, 8 MB of octal PSRAM.

| Region          | Address range             | Notes                                                          |
| --------------- | ------------------------- | -------------------------------------------------------------- |
| IRAM-only SRAM  | `0x40374000`-`0x40378000` | Taken by the instruction cache if it is set to 32 KiB          |
| DIRAM           | `0x3FC88000`-`0x3FCF0000` | Shared by `.iram0.text`, `.data`, `.bss` and the main heap     |
| DRAM-only SRAM  | `0x3FCF0000`-`0x3FCF8000` | Heap                                                           |
| ROM data        | `0x3FCF8000`-`0x3FD00000` | Never available                                                |
| RTC fast memory | `0x600FE000`-`0x60100000` | Internal heap region, but on the peripheral bus                |
| PSRAM           | external                  | Application code and rodata (XIP), cold data, most task stacks |

The top of DIRAM, above `0x3FCE9710`, is the ROM's boot stack, which ESP-IDF
adds to the heap once the application runs. Two properties of this map drive most of the decisions below.

**IRAM is paid for in DRAM.** On the S3, code placed in IRAM occupies DIRAM,
and the linker reserves the same span on the data side as `.dram0.dummy`. Every
byte added to IRAM is a byte removed from the internal heap. IRAM is reserved
for code that runs every block or with the flash cache disabled.

**`MALLOC_CAP_INTERNAL` does not mean fast.** RTC fast memory reports
`MALLOC_CAP_INTERNAL` and sits in the allocator's low-priority column, so an
internal request that DRAM cannot satisfy silently lands there. It is clocked
from the APB bus, a third of the CPU's speed. It is the one internal region
without `MALLOC_CAP_DMA`, so every hot allocation asks for
`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`, which excludes it at no cost. RTC fast
memory is used on purpose only for cold statics: the mode worker's stack, the
BOOT button task's TCB and the Gea frame scheduler's state.

## Where the internal budget goes

In audio mode the internal heap is effectively full. The amp graph takes the
largest share:

| Allocation                                    | Bytes (approx.) |
| --------------------------------------------- | --------------: |
| 23 int16 history rings, one per layer         |         148,400 |
| Low-precision history ring                    |          30,344 |
| Stage B coefficient table (layers 8-22, head) |          19,392 |
| Stage A coefficient table (layers 0-7)        |          14,272 |
| Layer 0 int8 history ring                     |           1,064 |
| **A2-Full graph**                             |    **~213,500** |

The rings follow the network's dilations (three stacks from about 2 KB up to
21,168 bytes, plus two more), and every A2-Full capture has the same shape. The
rest holds the image's IRAM and static data, FreeRTOS, the DSP stage stacks,
the USB host's and display's DMA buffers, the effects arena and a few system
task stacks. In maintenance mode the radios use the space instead.

## The NAM bank arena

S3 data SRAM is organised in 32 KiB banks, and two cores streaming their hottest
tables from one bank contend for it. Each stage's coefficient table, and the
largest core 0 history, therefore starts on a bank boundary of its own. Asking
the heap for such blocks cuts it into pieces the other histories must fit
around, so the graph is placed in a fixed arena (`src/audio/nam/nam_a2_full_s3_native.cpp`):

- **Five 32 KiB banks at `0x3FCC0000`**, 163,840 bytes, reserved with
  `SOC_RESERVE_MEMORY_REGION` before the heap is initialised. The range
  extends past the linker's ceiling for static data, so it cannot be a `.bss`
  array, and it needs no alignment padding. If `.bss` ever grows into it, the
  heap reservation fails loudly at boot.
- **Banks 0-2 hold the aligned blocks**: the largest core 0 history, the stage
  B table and the stage A table. Each bank records the stage that claimed it.
- **Histories fill the bank tails**, best fit, and only from the same stage. A
  core 0 history behind a core 1 table would reintroduce the contention the
  alignment exists to avoid. Banks 3 and 4 take the remaining histories.
- **The reverb's core state** is carved from the top of bank 1 before the graph
  is placed, so it gets a fixed internal address instead of whatever contiguous
  space the heap has left after the graph.
- **Unused tails go back to the heap** once the graph is placed
  (`coyopedal_nam_bank_arena_release_unused`). The display and USB allocate after
  the graph and need them.
- **Histories that do not fit the arena** are planned jointly from a snapshot of
  the heap (`coyopedal_nam_internal_batch` in `src/native/nam_banks/`). The
  planner searches for up to a million steps with its workspace in PSRAM, and
  plans against DMA-capable DRAM before it considers RTC fast memory. It logs
  which case applied.
- **On a maintenance boot** banks 3 and 4 are lent to the heap for Wi-Fi, HTTP
  and OTA. Banks 0-2 are never lent, so the aligned blocks always have a home.

`src/native/nam_banks/bank_allocator.c` also wraps TLSF's aligned allocation.
Stock TLSF satisfies a 32 KiB-aligned request by looking for a 64 KiB free
block; the wrapper picks the smallest free block with an aligned fit, keeping
larger regions for histories. It uses ESP-IDF's private TLSF interface, so
review it when upgrading ESP-IDF.

## The effects arena

The reverb's eight Q15 tank lines (35,512 bytes) live in `g_effects_arena`, a
35,520-byte static array in `src/native/main/main.cpp`. It is a bump allocator
that is reset when the graph is torn down.

A heap block, even one taken early, is a wall across the one large free
region, leaving the graph's planner two fragments. A static array sits below
the heap start, so the heap begins higher and stays in one piece. On a
maintenance boot the arena is handed to the heap. The whole tank is internal
because lines in PSRAM share the data cache with UI initialization and display
flushes, and the core running the reverb then overruns.

The reverb's predelay line is strictly sequential and is placed in PSRAM on
purpose. The two 16 KiB
modulation lines are promoted to internal SRAM only if space remains after the
UI starts; normally they stay in PSRAM.

## Allocation order

The order at boot (`gea_app_native_boot`) is part of the design:

1. The display's 4 KiB internal DMA reserve is taken, then released while the
   graph is placed. Holding it during placement pushed stage B histories into
   RTC fast memory.
2. The reverb's tank and core state are claimed from their fixed arenas.
3. The model is loaded and the graph placed.
4. The effects take their remaining buffers.
5. The display reserve is taken back, then the USB host starts and creates the
   DSP stages. The stages must not exist before a model does.
6. The UI starts, and the modulation lines are promoted if room remains.

Fixed placements come first, then the graph, then whatever can live with the
rest. `tests/test_sram_budget.py` checks that both load paths reserve the
reverb's memory before loading a model.

## What lives in PSRAM

`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` sends every default `malloc` to PSRAM,
so control-plane allocations from Wi-Fi, lwIP, HTTP, the UI and USB bookkeeping
cannot fragment internal SRAM. Anything that must be internal asks for it
explicitly. PSRAM also holds:

- application code and rodata (`CONFIG_SPIRAM_XIP_FROM_PSRAM`),
- the USB rings, the delay line and the model's float calibration copy,
- Gea's zero-initialised state and caches (`GEA_EMBEDDED_UI_STATE_EXTERNAL`),
- the Gea render, frame and touch task stacks (`GEA_EMBEDDED_*_STACK_EXTERNAL`
  in `gea.defines`), the USB task
  stacks, the tuning worker, the BOOT button task and the lwIP thread
  (`src/native/services/network_thread.c`),
- the 128 KiB reboot-surviving log ring.

A task whose stack is in PSRAM must never touch flash: ESP-IDF asserts when a
flash operation runs on an external stack. The mode worker, the OTA worker and
the NimBLE host therefore keep internal stacks.

## Pinned sdkconfig settings

These are in `src/native/sdkconfig.defaults`. Each one is silent when wrong:
the build succeeds and the cost appears elsewhere.

| Setting                                                   | Value  | Why                                                                                                                 |
| --------------------------------------------------------- | ------ | ------------------------------------------------------------------------------------------------------------------- |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`                   | `0`    | A reserve pool is carved out of the middle of the span a history bank needs; the graph then fails to load           |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`                     | `0`    | Default mallocs go to PSRAM                                                                                         |
| `CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB`                   | `y`    | A 32 KiB cache claims the IRAM-only region, moving 16,640 bytes of IRAM into DIRAM; the graph no longer fits        |
| `CONFIG_ESP32S3_DATA_CACHE_32KB`                          | `y`    | The gea CLI defaults the data cache to 64 KiB for PSRAM framebuffers; the extra 32 KiB is the DRAM-only heap region |
| `CONFIG_SPIRAM_XIP_FROM_PSRAM` and friends                | `y`    | Code misses are served from octal PSRAM instead of QIO flash; flash writes no longer disable the cache              |
| `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH`                   | `y`    | Safe only because of XIP; returns heap code from IRAM to the heap                                                   |
| `CONFIG_LIBC_LOCKS_PLACE_IN_IRAM`                         | `n`    | Same reasoning                                                                                                      |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE`                         | `4096` | The main task only brings up drivers and the first UI mount; the render loop has its own PSRAM stack                |
| `CONFIG_ESP_IPC_TASK_STACK_SIZE`                          | `1280` | Per core. Smaller values overflow in the flash cache-disable callback                                               |
| `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE`               | `1024` | Static DRAM buffer; the XTONE Pro's configuration descriptor is 375 bytes                                           |
| `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`, no Wi-Fi IRAM options | `y`    | Keeps the radio code out of IRAM                                                                                    |
| `CONFIG_HEAP_TRACING_OFF`                                 | `y`    | Unused; costs IRAM and a hook on the allocator path                                                                 |
| Unfitted flash chip drivers (ISSI, MXIC, ...)             | off    | IRAM for parts the board cannot have. Winbond's stays: the GD driver reuses its functions                           |
| SPI master/slave and I2C master ISRs in IRAM              | off    | IRAM saved; the worst case is a display or touch interrupt deferred during a flash write                            |

A new `sdkconfig.defaults` value only applies to a fresh build directory.
`tests/test_sram_budget.py` pins the first three rows.

## Things that were tried and did not help

- **Global IRAM-trimming options while code ran from flash.** The ROM flash
  driver (`CONFIG_SPI_FLASH_ROM_IMPL`) cannot address a 32 MB part and broke OTA
  validation; ESP-IDF now rejects it at compile time. Heap code in flash
  without XIP reset the board on the first flash write after entering
  maintenance. Place individual functions instead.
- **Running without XIP from PSRAM.** Stage B slowed by about 10%: the
  dispatcher and kernels did not stay in the 16 KiB instruction cache and every
  miss was fetched from QIO flash.
- **RTC fast memory for hot buffers.** History rings placed there made the amp
  miss deadlines on its own; stage B rose from about 94% to 98% of its budget.
- **Placing the whole graph from the heap.** Without the fixed arena, the
  aligned blocks fragmented the heap and the low-precision ring could not be
  placed.
- **Reserving the reverb from the heap before the graph, or putting its core
  state in the effects arena.** Both left the history planner too little
  contiguous room; it gave up, and the retry made boots take about 20 seconds.
- **Split layer 9.** With the USB client on core 0, split 9 overloaded core 0.
  Split 8 balances the two cores.
- **Stage A above the USB client.** Isochronous transfers were resubmitted
  late, which produced transfer errors and inserted silence.
- **In-place mode switches.** Loading the graph into a heap the radios had
  fragmented, or starting BLE in a heap the graph had shaped, fails. Both
  directions reboot.
- **Moving an amp history to PSRAM to make room for effects.** It works, but
  the model runs from SRAM by design.
- **Options that change nothing here:** `CONFIG_ESP_PHY_IRAM_OPT` (libphy's
  IRAM code is the blob's own section), and turning off `RTC_CLK_FUNC_IN_IRAM`
  or `RTC_TIME_FUNC_IN_IRAM` (selected unconditionally unless flash
  auto-suspend is on, which the GD25Q256 does not support).

## How to measure

The radios are off in audio mode, so measurements are taken in a bounded audio
window and read back from maintenance with `tools/esp32/amoled_remote.py`
(`logs [--follow]`, `command <TEXT>`; `command HELP` lists the commands).

- **Boot census.** Every audio boot logs `heap_census: before NAM load`, the
  owner of each large internal block, followed by the graph placement
  (`history plan ready`, bank arena and reverb lines). Read it with `logs`.
- **`AUDIO TRY <5..120> [ENGAGE] [MASK <0..63>] [MODEL <index>] [SWAP]`**
  reboots into a real audio boot for the given time and returns to
  maintenance, logging `audio window` summaries (DSP, USB, per-core tasks).
  `SWAP` exchanges the stages' cores. `NOUI` suspends nothing: the UI tasks do
  not exist yet when the window is armed.
- **The `audio:` heartbeat** is logged every five seconds in every audio boot.
  `C0` and `C1` give each stage's mean and maximum time against the 1,333 us
  budget, and `miss=a/b` counts overruns per stage. A non-zero miss count is a
  click. `cap`, `play`, `silence` and `err` describe the transport.
- **`HEAP MAP`** captures and prints a block-by-block census of the internal
  heap in maintenance. The output is a `slot=` header, one `H <heap>` line per
  heap, then `<address>,<size>,u|f` per block. The audio-boot census is the
  `logs` output described above.

After changing placement, check that the graph loads, that `miss=0/0` holds
with a demanding preset, and that a maintenance round trip and an OTA work.
