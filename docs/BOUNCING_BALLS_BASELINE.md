# AMOLED 2.06 Bouncing Balls baseline

Historical baseline restoration. For subsequent measured shared-style work and
current qualification status, see [Shared-style experiment](SHARED_STYLE_EXPERIMENT.md).

This is baseline restoration before the shared-style experiment. The device is
USB AMOLED 2.06 (`amoled`), serial `80:B5:4E:DA:73:88`. The app is the unchanged
`geastack/examples/apps/bouncing-balls-jsx`, with 64 balls, the original Inter
font, its 120 FPS request, vsync disabled, and 64-row/depth-2 display transfers.
The pedal board is outside this measurement scope.

The final deployed build measured **60.132 FPS**, with zero watchdog warnings
during the run. This restores average throughput to 60 FPS; it does **not**
establish that every frame meets 16.67 ms. Of the 3,600 completion intervals,
1,474 exceeded that budget; p99 was ≤18.500 ms and the maximum was 31.865 ms.
Shared-style performance remains untested and the experiment stays paused.

## Measurement

The target scheduler measures completed frames including display transfer. It
discards 300 warm-up frames, then collects 3,600 frames. FPS is calculated from
completion intervals; p99 is an upper bound from 250 µs histogram bins. These
numbers measure firmware completion, not physical panel scanout.

| Cumulative build                                                         |        FPS | Mean completion interval | p99 interval ≤ |
| ------------------------------------------------------------------------ | ---------: | -----------------------: | -------------: |
| Original inline baseline                                                 |     11.495 |                86.988 ms |      88.750 ms |
| Correct clipped replay binding                                           |     34.921 |                28.636 ms |      29.750 ms |
| Retain layout with unset minimum sizes                                   |     42.181 |                23.706 ms |      25.000 ms |
| Current Geastack compiler                                                |     44.761 |                22.340 ms |      23.500 ms |
| Direct parsing of small integer positions                                |     53.338 |                18.748 ms |      20.000 ms |
| Early position-name lookup and skip leaf clip scans                      |     58.851 |                16.991 ms |      32.500 ms |
| Resolve text-cache hash collisions (initial LRU implementation)          |     59.459 |                16.818 ms |      18.250 ms |
| Final: DMA tail, bounded catch-up, occlusion reads, read-only cache hits | **60.132** |            **16.629 ms** |  **18.500 ms** |

The early builds contained temporary phase prints. Final builds remove those
prints, retaining only the one summary after the measurement. Two intermediate
DMA-wait builds reached 60.142 and 59.917 FPS but triggered idle-task watchdog
warnings; neither is an acceptable final baseline.

## Changes

- The renderer feature analyzer retains circle caches when CSS border radius
  uses them.
- Clipped replay remains on the canvas bound to the current DMA chunk. Its
  previous worker split did not carry that buffer binding or clip.
- The absolute-leaf layout path accepts both unset and zero minimum dimensions.
- Complete signed decimal position strings within ±32767 avoid generic CSS
  expression parsing. Units, fractions, larger values and expressions retain
  the general parser. The existing style setter still handles invalidation.
- Position names are classified before unrelated CSS declarations.
- Translating a leaf skips scanning the entire display list for descendant
  clips, which the recorder never creates for leaves.
- The four existing text-cache slots are searched by full key. A four-byte
  atomic replacement cursor advances only on misses; hits do not mutate shared
  ordering metadata. The measured initial LRU implementation was simplified to
  this form before the final build. Cache capacity and glyph drawing are unchanged.
- Occlusion checks reject commands that cannot cover the region before reading
  node visibility state.
- The CS-held framebuffer tail uses a 4 ms spin budget, matching the existing
  slot wait. The previous 2 ms budget was shorter than two normal queued
  64-row chunks at 80 MHz QSPI and introduced scheduler wakeup delays.
- Sustained catch-up yields for two ticks every 32 frames, retaining watchdog
  protection and guaranteeing idle time. The one-tick attempt still starved idle.

## Reproduction and scope

`scripts/build-balls-206.mjs` builds the original app through the Geastack CLI. It
temporarily adds benchmark defines and restores the app manifest. Set
`GEA_COMPILER_DIR=/Users/dashersw/Projects/Code/github/geastack/compiler` so the
build uses the single current compiler in `compiler/dist`, rather than the older
pedal dependency. Engine and element staging packages preserve inline styles;
`GEA_EMBEDDED_SHARED_STYLES=0` throughout. On this app, `Node` is 168 bytes and
`ComputedStyle` is 100 bytes.

Firmware, ELF files, build/flash/device logs and result summaries are saved under
`build/css-feature-audit/balls-206-*`. The machine-readable comparison is
`build/css-feature-audit/balls-206-baseline-results.json`.

Final image: `build/css-feature-audit/balls-206-baseline60-idle.bin` (1,967,328
bytes), SHA-256
`f109a7d20a4397c18d36def897b9d2700097a490f1ecf47e8d5b3869639b3643`.

The original app and existing test files were not edited. Validation here is
successful firmware builds, source/diff checks, and actual device measurement;
no repeated unit suite was run. No packages were published or pushed. Shared
styles remain paused and have not been deployed.

## Regression protection (September 30)

Behavioral regression tests now live in Geastack core and targets, with focused
push/PR workflows. The engine tests check the 64-ball workload's retained layout,
integer-position fast path and CSS semantics, zero per-leaf clip scans, clipped
DMA replay binding, text-cache collision behavior, cache capacity and pixel
parity. Renderer analysis tests preserve circle caches for CSS radii without
allocating them for zero-radius/unrounded content. Target tests exercise the
actual catch-up and DMA-wait bodies with fake RTOS time and semaphore completion.

Reintroducing each of five old engine bugs and two old target bugs makes the
intended assertion fail. The existing retained-subtree pixel test also passes,
including moving parent overflow clips. Two new profiling work counters and their
storage compile out with UI profiling disabled; the firmware gets no new hot-path
test machinery.

The examples repository provides `npm run test:balls:device`: rebuild the original
app, flash only the registered USB AMOLED 2.06, collect 300 warm-up + 3,600 completed
frames, and fail below 60 FPS, above 19 ms p99 / 35 ms maximum, or on device faults.
The gate validates exact elapsed time and source fingerprints. Its 35 host tests
reject slow, malformed, missing, duplicate and faulting measurements. Host CI
cannot run a physical USB measurement; the hardware command remains a required
local acceptance check for performance work.

A fresh end-to-end gate run **passed at 60.071 FPS**, p99 ≤18.500 ms, maximum
31.961 ms, 1,595 intervals above 16.667 ms, and no device faults. This remains a
sustained-throughput gate, not an every-frame deadline guarantee. Firmware and
logs are saved as `build/css-feature-audit/balls-206-regression-gate*`, image SHA-256
`979127894a86e29977f6fbc382b547d2695900dfafbc928aa383590d51098703`.
The app source is unchanged and its temporary benchmark manifest was restored.

The fresh build exposed an existing compiler packaging omission: `gea_runtime.h`
included `gea_runtime_rtc.h`, but the CLI did not copy that split header. The CLI
now copies it alongside the other runtime headers. The single current compiler's
TypeScript build completed and the actual firmware then built/flashed successfully,
using matching current host headers. The broader compiler build command's
unrelated host-binding test chain was stopped after hanging; it is not reported
as a full compiler-suite pass. No compiler lowering/semantic changes were made
for this task. No packages were published or changes pushed.
