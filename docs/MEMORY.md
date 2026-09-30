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

The optional internal speaker uses a PSRAM worker stack and a 2 KiB PSRAM
sample queue. Its I2S DMA buffers must remain internal. The app selects
`GEA_AUDIO_DMA_DESCRIPTORS=3` and `GEA_AUDIO_DMA_FRAMES=128`; the codec's media
playback defaults of six 240-frame descriptors exhaust the remaining internal
heap when USB audio and the NAM graph are already running. Check both output
paths on hardware after changing these values.
The speaker worker runs on core 0 at priority 18, below USB (20) and DSP stage
A (19). Its clock-drift interpolation uses single precision so the ESP32-S3
can execute it in hardware. Speaker drop counters must be checked independently
of the USB and DSP counters.
On the LCD 3.5B-C, 64-frame DMA buffers dropped speaker samples despite clean
USB output. With 128-frame buffers and 16-bit initial codec slots, a 30-second
run sustained 1,874–1,876 speaker blocks per five seconds with zero steady-state
speaker drops/underruns or DSP deadline misses. Listening confirmed clean output.

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

### Keep the measurement harness out of the audio deadline

Do not call `uxTaskGetSystemState` during an audio window. ESP-IDF scans every
task's stack high-water mark while holding its SMP scheduler lock. With PSRAM
stacks, a measured census took 2,200 µs at startup and 1,792 µs at shutdown,
longer than a 1,333 µs audio block. The diagnostic can create the overruns it
purports to measure.

The remote window samples named persistent tasks with
`vTaskGetInfo(..., pdFALSE, ...)`, skipping the stack scan, and captures its DSP
counters before shutdown diagnostics. Transient workers are omitted from the
per-task CPU percentages. Two corrected runs measured 114/112 µs for this
collection and zero DSP misses. See [the UI memory checkpoint](UI_MEMORY_OPTIMIZATION.md)
for the paired hardware results and their scope.

### Keep UI diagnostics out of audio startup

The UI pump's one-time framebuffer census and render-stack high-water query run
only in maintenance mode. The pixel census reads the entire PSRAM framebuffer;
`xTaskGetHandle` walks FreeRTOS task lists under the kernel lock before the stack
scan. Those diagnostics add memory traffic and scheduling interference during
the tight startup window. Native pixel tests and maintenance census checks remain
available; audio stress runs use the display flush odometer without framebuffer
or task-stack scans. This removes avoidable diagnostic work, not the audio
window's deadline accounting. Its effect on an intermittent startup miss must
be measured, not inferred from a passing host build.

## Gea initialization CPU affinity

The compact-node development build selects `GEA_EMBEDDED_GEA_INIT_TASK_CORE=1`.
Gea initialization uses floating point; when created without affinity, ESP-IDF
can pin it to whichever CPU first executes those operations. A single diagnostic
image produced four core-0 starts at 4.984–5.019 seconds and a core-1 start at
3.975 seconds. Explicit core-1 placement produced three short starts at
3.944–3.959 seconds and a 90-second-soak first paint at 4.025 seconds. Both audio
stages and USB retained zero errors/misses in the measured windows. This does
not establish 60 fps behavior or identical startup to the older 3.887-second
control. See UI_MEMORY_OPTIMIZATION.md for the complete measurements.

The shared target defaults this new setting to -1 (unpinned). It validates the
selected CPU at compile time and applies it to either initialization stack
allocation path. This setting requires the unpublished target change currently
under qualification; the installed registry target does not implement it yet.
Do not attribute the observed approximately one-second startup variation solely
to node stride or pointer-load cost without recording initialization affinity.

The subsequent automatic class-list-capacity build keeps that same core-1 setting
and 148-byte Node. Class records shrink 12 → 8 B, reducing the fixed tree by
640 B to 27,256 B without additional field-read loads. Its three short starts
were 3.876–3.904 seconds; the UI/audio and steady-load windows retained zero
DSP misses, USB errors and watchdog firings. See UI_MEMORY_OPTIMIZATION.md for
the measured heap delta and the distinction between fixed storage and heap
variation. The approximately 50-byte node target remains open.

The supporting-record candidate (`candidate-record-padding`) records two startup
misses, despite 7,776 B more observed free PSRAM. Its maxima were 320,304 and
320,600 cycles. Three subsequent class-capacity control windows passed. Do not
qualify a storage change solely from field-load assembly or native tests; CSS
record strides and code layout can change execution cost without a style pointer.
The candidate is retained for investigation; see UI_MEMORY_OPTIMIZATION.md.

The combined `candidate-animation-pruned` follow-up automatically excludes
unreachable CSS/declarative animation code, tracking and per-frame polling.
Frame callbacks remain. Firmware is 44,816 B smaller than class-capacity,
static PSRAM BSS saves 1,616 B, and matched maintenance free PSRAM rises 13,248 B
(do not add those savings). Node stays 148 B with direct style fields. All five
startup/UI/audio windows pass at 15 fps; median first paint is 3.833312 s and
steady audio time is unchanged. This supersedes the failed record-only image,
not its recorded failure. Current artifacts and limits are in
UI_MEMORY_OPTIMIZATION.md. No package publication or registry-only requalification
has occurred for this batch.

### Opaque-alpha / 144-byte node experiment

`candidate-alpha-front` removes provably unused text/border alpha, reuses record
tail padding and keeps style at offset zero. Node 148 → 144 B; the 160-slot tree
27,256 → 26,616 B; matched device free PSRAM +1,144 B; firmware −1,104 B.
Ordinary field reads and style-reference passing retain their S3 instruction
sequences. Full/pruned pixels, copy/reset tests and transparent-color retention
pass. No pointer-based style storage is involved.

Do not promote this image merely because it is smaller. Three matched starts
regress median first paint 3.862903 → 3.899955 s (+0.96%). The worst observed
core-0 stage grows 314,903 → 318,446 cycles against a 320,000-cycle budget:
21.24 → 6.48 µs of measured headroom. All five windows have zero deadline,
USB or watchdog failures; steady means are effectively unchanged. Preliminary
text-first layouts also regress boot (+1.89% / +4.90%). The 148-byte animation-
pruned build remains qualified. The source/ABI experiment and all three images
are preserved for continued work; no release was made. See
[UI memory measurements](UI_MEMORY_OPTIMIZATION.md) for artifacts and limitations.

### 2026-09-29: rule-plan/reset batch remains experimental

The 144-byte direct Node plus automatic pseudo-element pruning reduces active
rule plans 1,012 → 228 B and the target style-recomputation stack frame
2,320 → 592 B. Copying a constant default style avoids temporary Node setup.
Firmware falls 3,776 B to 2,982,768 B. Matched maintenance PSRAM rises 66,996 B;
65,536 B comes from a mapping page boundary, not node allocation. Stack task
reservations are unchanged. All 22 native frames and twelve allocation-phase
hashes match; host allocated bytes do not decrease further.

Five device windows have zero DSP misses, USB transfer errors and watchdog
firings, but the UI stress window pads/trims 241 / 320 frames versus 99 / 145 in
a fresh control repeat. Do not qualify it merely from the lower peak DSP time
or the short-run first-paint median. The saved candidate-animation-pruned image
is restored in maintenance mode; candidate-reset-plan and its full logs remain
available for diagnosis. Native snapshots still have unneeded length/color/class
capacity storage to audit together; no field-read indirection is accepted.
