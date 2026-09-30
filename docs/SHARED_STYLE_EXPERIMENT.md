# Shared-style experiment

The user's acceptance condition is measured S3 frame latency with Bouncing Balls
JSX, including worst frames and missed 16.67 ms budgets. The experimental engine
must also reduce total live storage, including style records and ownership data.
A smaller `sizeof(Node)` alone is insufficient. No package publication is planned.

## USB AMOLED 1.8 comparison (2026-09-30)

The user connected the smaller 1.8 board and authorized switching the comparison
to it. Its USB serial is `30:ED:A0:AC:90:DC`, registered as `amoled-18` in the
examples project. The unrelated 2.06 serial `80:B5:4E:DA:73:88` remains untouched.
The original JSX/CSS/font and 64 balls remain unchanged. Wi-Fi is disabled for
both variants, with USB capture and the same existing 300 + 3,600-frame gate.

The initial `balls-18-inline-baseline` image was built and its linked layout
verified, but it produced no performance result: runtime initialization stopped
at FT3168 touch setup. Device control reported zero UI nodes and the framebuffer
was black. I2C scanning found the expander at `0x20`, but no touch controller.
The target had omitted the reset sequence present in
[Waveshare's original example](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/main/examples/arduino/examples/04_GFX_FT3168_Image/04_GFX_FT3168_Image.ino).
The board hook now pulses EXIO0..2 while preserving other expander bits, reusing
the existing shared I2C bus. A focused host test covers every initial output and
direction value plus transaction failures. The failed capture and startup log
are preserved; neither counts as an FPS measurement.

The 1.8 also enables the established Bouncing Balls JSX profile. Its first
compile exposed a pacing block referring to `streamFlush` when the held-CS
mode had excluded that variable; matching the pacing guard to the declaration
fixes that combination. Both variants below include these board bring-up fixes.

| Measurement                                 | Inline baseline | Shared styles |
| ------------------------------------------- | --------------: | ------------: |
| Completed-frame FPS                         |       68.234802 |     67.532429 |
| Mean interval                               |       14.655 ms |     14.807 ms |
| p99 interval <=                             |       16.500 ms |     16.500 ms |
| Maximum interval                            |       29.048 ms |     29.041 ms |
| Intervals over 16.667 ms / 3,600            |               6 |            22 |
| Node bytes                                  |             100 |            52 |
| ComputedStyle bytes                         |              40 |            40 |
| TreeState bytes / 512 slots                 |          60,472 |        35,896 |
| Allocated node-owned payload + static bytes |          68,220 |        47,460 |
| Free PSRAM                                  |       7,366,956 |     7,386,484 |
| Free internal RAM                           |          84,784 |        85,216 |
| Firmware bytes                              |       1,767,872 |     1,766,768 |

Both first completed captures pass the unchanged FPS/p99/max/fault gate, with
no faults reported. The measured throughput cost of sharing is 1.03%; p99 is
unchanged, although the number of individual over-budget intervals increased.
This is a measured pass, not a guarantee that every frame meets 16.667 ms.
The Node remains 52 bytes; associated allocations are included in the table.
Node-owned totals include the actual allocated payload size, pool slack,
style records, persistent layout pages and static owners, but not the allocator's
private headers. The whole-heap observations show 19,528 more PSRAM bytes and
432 more internal bytes free. There are 68 live nodes in both variants; the
512-slot capacity is unchanged.

All 12 generated native files match after normalizing only the storage define;
all 556 maintained framework/target source fingerprints also match. Artifacts:
`balls-18-inline-reset-*` and `balls-18-shared52-*`. Firmware SHA-256 values:

- Inline: `77d4ca1d56863d709f3554baf7f49ea788c70ae2ffecacd858c9ff9060f269ed`.
- Shared: `d6dfc9134cba824a9acf5fe64d069486c06ae6c3a1af45e0bd725275492fad4c`.

The retained allocation candidate removes the backward link in each shared style
record and grows override blocks through capacities 2, 3, 4 before doubling.
Style-field reads and writes are unchanged; record unlinking uses a cold list
scan. Focused host ownership/rendering checks pass, including middle/tail/head
reclamation, complete Property growth, and the unchanged framebuffer hash
`18157712955437833091`. Its first completed hardware capture also passes the
unchanged gate, with no faults reported:

| Measurement                                 | Shared styles | Compact ownership |
| ------------------------------------------- | ------------: | ----------------: |
| Completed-frame FPS                         |     67.532429 |         67.354977 |
| p99 interval <=                             |     16.500 ms |         16.750 ms |
| Maximum interval                            |     29.041 ms |         30.206 ms |
| Intervals over 16.667 ms / 3,600            |            22 |                27 |
| Node bytes                                  |            52 |                52 |
| SharedStyleRecord bytes                     |            52 |                48 |
| Allocated node-owned payload + static bytes |        47,460 |            46,652 |
| Free PSRAM                                  |     7,386,484 |         7,386,632 |
| Free internal RAM                           |        85,216 |            85,232 |
| Firmware bytes                              |     1,766,768 |         1,766,688 |

The ownership census falls another 808 bytes, but the whole-heap observations
improve by only 148 PSRAM bytes and 16 internal bytes. These are different
measurements; 808 bytes is not a measured free-heap gain. Relative to the inline
baseline, the final ownership census is 21,568 bytes (31.62%) smaller, with
19,676 more PSRAM bytes and 448 more internal bytes free. Its observed FPS is
1.29% below inline. Single captures do not establish the significance of the
smaller 0.26% difference between the two shared builds.

The final image is `balls-18-shared52-compact-records.bin`, SHA-256
`d96b1e315c295adb7e41d14511e13148763090245ea458cf34e5715b0093fb83`,
and remains flashed on the 1.8. Linked DWARF confirms the 48-byte style ownership
record consistently across all 34 translation units that describe it. The
generated app fingerprints remain identical. Framework fingerprint changes are
the three intended ownership implementation files and their focused regression
test. An unrelated 2.06 target CMake change is recorded separately and verified
absent from the 1.8 build graph. The report and raw capture are preserved under
`balls-18-shared52-compact-records-*`.

These timings measure completed scheduler frame cadence, including transfers
in the steady frame cycle; they are not physical panel scanout measurements.
The final p99 is slightly above 16.667 ms, and 27 individual intervals exceeded
that budget. The result supports sustained throughput above 60 FPS under this
workload, not a promise that every frame arrives within one 60 Hz interval.

Reproduce with `scripts/build-balls-18.py`, the shared layout checker wrapper
`scripts/qualify-balls-241.mjs`, and `scripts/measure-balls-18.mjs`. The latter
verifies the registered USB identity and qualified firmware hash before flashing.
Generated native hashes and maintained framework-source hashes accompany each
image so comparison variants can be checked for unexpected source differences.

## Original Bouncing Balls on Wi-Fi 2.41 (2026-09-30)

The user subsequently authorized replacing the pedal app on the same 2.41 with
`geastack/examples/apps/bouncing-balls-jsx`. The 2.06 remains untouched. This is
the original 64-ball JSX app, original Inter font, native 450 x 600 portrait panel
at DPR 1.5, 120 FPS request, vsync off, and requested 64-row/depth-2 transfers.
Source hashes are checked against the existing workload gate. Wi-Fi OTA and TCP
diagnostics remain enabled because this board has no USB connection to this Mac.
The app's animation sources and permanent manifest remain unchanged.

The inline baseline exposed a display-resource issue unrelated to style sharing:
the synchronous Wi-Fi startup loan held display staging at its two-row minimum
throughout the run despite more than 100 KiB of free internal RAM. Narrow dirty
windows also used the full-width row count, wasting their available DMA capacity.
The corrected startup releases the loan after app and task allocations, and
partial windows use the available capacity without exceeding the requested row
limit or the panel's even-row rule. The allocator now selects 56 rows x 2.

| Inline checkpoint                                                 |       FPS | p99 interval <= | Maximum interval |
| ----------------------------------------------------------------- | --------: | --------------: | ---------------: |
| Existing 2.41 defaults, two-row Wi-Fi floor                       |  8.354141 |      149.750 ms |       174.454 ms |
| Released startup loan, packed transfers, JSX CPU/renderer profile | 50.415741 |       21.500 ms |        27.217 ms |
| Tight dirty windows on this larger panel                          | 52.156458 |       22.000 ms |        27.575 ms |
| Raster into the other buffer before draining the previous DMA     | 56.191948 |       21.750 ms |        23.919 ms |

Each row uses 300 warm-up frames followed by 3,600 frame intervals, including
transfer work in the steady frame cycle. All four still fail the existing
60 FPS gate. These are intermediate results, not a successful qualification.
Node remains 100 B and ComputedStyle 40 B in every row. Per-band panel address
commands remain enabled; the RM69080's unsafe RAMWRC continuation was not enabled.
A bounded DMA queue-drain candidate measured 52.098305 FPS, p99 <=22.000 ms
and maximum 23.341 ms: no useful throughput improvement. Its production change
was removed; the patch and run remain archived under `balls-241-no-gain-*` and
`balls-241-inline-dma-poll-*`. A separate phase-profile build showed about
13.8 ms in flushing and 3.3 ms in animation updates. The flush entered a DMA
drain before rasterizing the next window, serializing work that the two buffers
could overlap. The retained fix reserves the alternate buffer, rasterizes into
it, and only then drains the preceding transfer before issuing panel commands.
The blocking fallback accounts for the slot held by the prepared buffer.
Focused cadence tests cover successful drains, partial timeout, and reserved
slots. The 56.19 FPS measurement has profiling disabled and still does not pass
the gate. A subsequent 64-dirty-window candidate measured 53.579 FPS,
p99 <=21.500 ms and max 29.438 ms; its capacity override was removed. The display
window array now follows `DirtyRegions::kMaxRects` rather than assuming 32.
Artifacts for the rejected capacity experiment remain under
`balls-241-inline-regions64-*`. The 56.19 FPS image was restored to 2.41 when
the user connected and authorized the smaller USB 1.8 board for the comparison.

The initial slow run exceeded the logger's original 420-second deadline. The
logger reconnected to the same uninterrupted device run and retrieved its first
RESULT from the diagnostics ring; it was not rebooted or rerun to pass. Subsequent
captures have a 600-second ceiling. Upload-phase logs are separate from benchmark
logs: flash erasure can stall the old app, and its upload-time watchdog diagnostics
are not attributed to the new firmware. The qualification scripts now wait for
the changed running OTA slot before starting the new capture.

The explicit deployment declaration `gea.ota.wifi: true` retains networking for
an app with no network bindings. This required adding the missing `spi_flash`
component dependency in the shared AMOLED target. The experiment's app-local
sdkconfig enables IPv6 because the target's network bundle includes esp_peer's
IPv6 transport. The build wrapper restores the original manifest, SDK defaults,
and ignored Wi-Fi settings after each build; credentials are not saved in reports.

Reproduce with `scripts/build-balls-241.py`, `scripts/qualify-balls-241.mjs`,
`scripts/ota-balls-241.py`, and `scripts/capture-balls-241.mjs`. The first transition
from pedal firmware uses `amoled_remote.py ... ota <explicit-image>` instead of
the native Gea OTA endpoint. These are development-checkout experiments. The
pedal lockfile is unchanged; no release or push is authorized by this work.
Artifacts are under `build/css-feature-audit/balls-241-*`.

The earlier pedal audio results below apply to the runtime before these new
display changes. They do not qualify the new display changes for pedal audio.
The saved passing pedal image remains available for restoration.

## Pedal 2.41 hardware comparison (2026-09-30)

The user authorized Wi-Fi OTA of the maintenance-mode 2.41 pedal, MAC
`30:ed:a0:29:59:14`, at `192.168.178.159`. The 2.06 running Knight was not
touched. These builds run the existing pedal JSX at its configured 15 FPS;
they do not qualify the original Bouncing Balls application's 60 FPS requirement.

Both current variants use the same Geastack development source and compiler,
the same sdkconfig and the same pedal application. The only compile-command
difference is `GEA_EMBEDDED_SHARED_STYLES=0` versus `1`. All checked compiled
sources and engine/target headers remained unchanged between builds. Both linked
ELFs pass the complete record-layout consistency check. The temporary app-manifest
change was restored; the lockfile and registry dependencies were not changed.
These are unpublished development experiments, not registry-qualified releases.

| Measurement                            | Previously installed | Current inline | Current shared |
| -------------------------------------- | -------------------: | -------------: | -------------: |
| Firmware bytes                         |            2,986,544 |      2,877,216 |      2,877,472 |
| Node bytes                             |                  148 |            140 |             52 |
| ComputedStyle bytes                    |                   80 |             84 |             84 |
| TreeState bytes, 160 slots             |               27,256 |         25,656 |         11,576 |
| Free PSRAM after matched 45-second run |            4,147,000 |      4,288,668 |      4,301,316 |
| Free internal RAM at that sample       |               94,322 |         96,714 |         96,714 |
| First paint in 20-second UI stress run |         3,859,904 us |   3,860,097 us |   3,772,103 us |
| Maintenance non-black pixels / nodes   |          35,699 / 15 |    35,699 / 15 |    35,699 / 15 |

The current shared build frees **12,648 additional PSRAM bytes** versus the
matched inline build after live ownership/allocation overhead. An earlier boot
sample differed by 12 B, so this is approximately 12.4 KiB, not a claim of
byte-invariant heap telemetry. The TreeState payload reduction alone is 14,080 B;
do not report that as the net saving. Relative to the previously installed
firmware, the shared build frees 154,316 B and removes 109,072 firmware bytes.
Most of the total PSRAM gain predates style sharing: the current code/rodata
reservation (`.ext_ram.dummy`) is 196,608 B smaller, while other static and heap
allocations offset part of that reduction. These quantities must not be added.

The amp and all six effects remain enabled throughout each fixed-load run.
The following are the last six complete five-second heartbeat intervals of
the matched 45-second windows, excluding startup. Means are weighted by block
count; maxima are measured interval maxima, not percentiles or averages.

| Stable audio, 1,333 us block budget        | Previously installed |      Current inline |      Current shared |
| ------------------------------------------ | -------------------: | ------------------: | ------------------: |
| Core 0 mean / maximum                      |  1,153.50 / 1,230 us | 1,153.50 / 1,253 us | 1,152.00 / 1,255 us |
| Core 1 mean / maximum                      |  1,193.67 / 1,215 us | 1,192.50 / 1,215 us | 1,192.67 / 1,215 us |
| Deadline misses, complete 45-second window |                0 / 0 |               0 / 0 |               0 / 0 |
| USB transfer errors                        |                    0 |                   0 |                   0 |

The shared 20-second screen-switching stress run did record one core-0 miss:
323,870 cycles (about 1,349 us), 8,962 ms after audio configuration. This was
during UI churn; neither the previous nor inline UI stress run missed. The user
clarified that acceptance concerns the stable all-effects deadline budget.
Preserve this event as a separate interaction/startup limitation, not evidence
of a steady-audio failure or proof of an identified root cause. The fixed-load
shared run has no misses even when including startup. The small mean differences
do not establish a DSP speedup. Input was near the noise floor; these runs are
timing measurements, not a listening or sample-by-sample audio equivalence test.

The subsequent **120-second shared-build all-effects window also passes**, with
zero deadline misses over the entire window, including startup. Stable analysis
excludes the first 30 seconds and includes only complete heartbeat intervals
whose preceding sample was already beyond that boundary. Observed stable maxima
are **1,257 us / 1,215 us**, leaving **76 us / 118 us** within the 1,333 us budget.
The final state confirms engaged pedal, amp enabled and effect mask 63. USB
transfer errors, input/output drops and watchdog firings are zero. The separate
stream-padding counter records 57 frames since configuration; passing the DSP
deadline check is not a claim of zero padding or audible equivalence.

At the end of this audio qualification the shared image was installed and the
board returned to maintenance mode. The subsequent Bouncing Balls experiment
above replaces that app on the board.
Its SHA-256 is
`7b2a5f64b64d59f0f6142cc7ccea7fd22f607af8e99df131b942fe515bc6a6d6`.
The preserved inline image is the comparison/recovery alternative; no packages
were published and nothing was pushed. Further compaction and the original
2.06 Bouncing Balls qualification remain open.

Evidence is under `build/css-feature-audit/pedal-241-current-*`, including
`pedal-241-current-comparison.json`, the linked-layout reports, matching compile
command diff, firmware/ELF archives and raw status/log captures. The fresh old
firmware controls use the prefix `pedal-241-baseline-*`.

## Pass-local layout memo validity (2026-09-30)

The layout memo already allocates and clears scratch for each pass. Its two
generation tags therefore duplicate the scratch lifetime. Both cache-hit validity
bytes now live in that existing scratch; the persistent record retains only
whether its last available box is valid for scoped layout. No new allocation,
lookup or packed-bit accessor is introduced. The global generation counter and
its periodic whole-tree invalidation scan are removed.

| S3 compiler measurement         | Before | After |
| ------------------------------- | -----: | ----: |
| Shared persistent layout record |   10 B |   8 B |
| Sixteen-record page             |  160 B | 128 B |
| Per-node pass scratch           |   10 B |  10 B |
| Global generation counter       |    4 B |   0 B |
| Shared Node                     |   52 B |  52 B |
| Inline Node                     |  100 B | 100 B |

For the last measured 68-node tree, five pages reserve 80 records. The page
payload falls 800 to 640 B; including the unchanged 32-byte pointer-table capacity
gives **832 to 672 B**. This **160 B payload reduction is a model using the
compiler-verified layout and previous occupancy**, not a new live-heap result.
At full 512-node occupancy the page payload saving is 1,024 B. Scratch size
does not increase, and the four-byte global saving is independently verified.

The zero-size regression deliberately leaves previous geometry at zero, changes
the authored width, and starts a fresh pass. Zeroed scratch must not count as a
cached result just because its dimensions match the old empty box. Replacing the
new pass-local validity check with persistent available-box validity makes that
test fail. Both MRU slots, explicit invalidation, node growth/reuse, scoped-layout
acceptance/rejection, repeated pass allocation and scratch release still pass.
Shared/pruned, inline, and shared dynamic-init/profiling-off controls preserve
pixel hash `18157712955437833091`. All **48** device-gate tests pass. The linked
gate now requires the eight-byte shared layout record, complete allocation
accounting and zero remaining generation-counter storage.

An additional control reproduced a promotion bug in the first candidate: an
out-of-range available box disabled the primary memo, and a later secondary hit
failed to restore persistent validity for scoped layout. Promotion now restores
that validity. The new case passes in both storage modes and with profiling off;
the original failure remains in
`balls-pass-local-memo-promotion-first-control.log`.

These S3 objects compile from current source with the saved original app flags.
They are **not a full firmware link or frame-rate qualification**. Actual heap,
firmware size and latency remain pending access to the AMOLED 2.06. Current and
previous geometry remain inline: their per-frame consumers make them a different
tradeoff from this pass-local memo bookkeeping.

Artifacts: `balls-pass-local-memo-s3-final-{commands,layouts}.json`,
`balls-pass-local-memo-{shared,shared-dynamic-init,inline}-final.log`,
`balls-pass-local-memo-inline-mutation-final.log`,
`balls-pass-local-memo-rejected-mutant-final.log`,
`balls-pass-local-memo-subtree-final.log`, `balls-pass-local-memo-gate.log` and
`balls-pass-local-memo-v18-linked-check.json`.

## S3 object audit and smaller private caches (2026-09-30)

Standalone S3 compilation now verifies the earlier host predictions without
touching Knight's shared build or USB device. These are compiler record/symbol
measurements, **not a linked firmware, live allocator census or FPS result**.
The saved original Bouncing Balls commands use DPR 1.5 and current v19 feature
proofs. Both UI state initialization modes compile successfully.

| S3 storage                         |    Saved v18 | Current object |        Saved |
| ---------------------------------- | -----------: | -------------: | -----------: |
| Circle tables (`CanvasMath`)       |     21,480 B |          468 B |     21,012 B |
| Triangle occlusion scratch         |      3,072 B |            0 B |      3,072 B |
| CSS rule index                     |      3,000 B |        2,872 B |        128 B |
| Active-rule plan cache             |      4,800 B |        4,704 B |         96 B |
| Text line-break cache              |      6,720 B |        6,656 B |         64 B |
| Two dense class-update mark arrays |      2,052 B |        1,026 B |      1,026 B |
| Dynamic length-expression cache    |      1,280 B |        1,024 B |        256 B |
| **These fixed objects combined**   | **42,404 B** |   **16,750 B** | **25,654 B** |

The latest two rows save another **1,282 B**. Length-cache bookkeeping occupies
existing tail padding with `[[no_unique_address]]`; direct member reads and
numeric precision are unchanged. Static-expression entries similarly shrink
12 to 8 B, but vector capacity and allocator savings need a device census.

Class-update marks now store an eight-bit batch epoch instead of sixteen bits.
Node IDs retain their full range; all 512 nodes remain supported. Before an epoch
is reused, the existing rollover path clears the marks. That reset now happens
every 255 batches rather than every 65,535, so its worst-frame cost still needs
hardware qualification. This is not a claim of zero runtime cost.

The S3 shared node remains **52 B**. ComputedStyle is now **40 B**, its shared
allocation record **52 B** (was 56), and the inline node **100 B** (was 104).
Shared TreeState remains 35,896 B; inline TreeState is 60,472 B. The default
shared-style owner record and counters total 60 B rather than 64 B. Per-record
payload changes must not be multiplied into claimed live heap savings before
the allocator census. Gradient storage, class-tracking globals, heap and stack
reductions are excluded from the fixed-object table to keep its scope explicit.

Focused native checks cover 70 dynamic length-cache entries, both axes, parent
resizing, static expressions and variable invalidation. Eight hundred batched
class updates exercise repeated epoch reuse and stale ancestor marks. Removing
the rollover reset deliberately makes the behavioral test fail. Default and
dynamic-initialization/pruned controls pass with the unchanged pixel hash
`18157712955437833091`. All **47** device-gate tests pass; the gate now checks
these private cache symbols as well as Node/TreeState/CanvasMath layouts. Its
symbol census was checked against the saved linked v18 ELF, including exclusion
of separate C++ initialization guards.

Artifacts: `balls-v19-s3-object-{commands,compile-results,layouts}.json`,
`balls-v18-s3-cache-baseline-layouts.json`,
`balls-length-and-epoch-s3-{commands,layouts}.json`,
`balls-length-and-epoch-storage-{default-final,dynamic-init}.log`,
`balls-epoch-rollover-mutation.log`,
`balls-epoch-rollover-rejected-mutant.log`,
`balls-length-and-epoch-gate-tests.log` and
`balls-cache-census-v18-linked-check.json`.

## Earlier host checkpoint: automatic percentage-size storage (v19)

The unchanged Bouncing Balls source now proves that width and height never use
percentage values. Both two-byte percentage fields become compile-time unset
constants. Pixel, viewport and font-relative dimensions keep their existing
paths; no field accessor or runtime feature check is added. Unknown values,
functions, variables, logical-axis ambiguity and native controls retain the
necessary fields. Missing, older, mixed or future proof versions cannot remove
them. The application requires no opt-in.

| Compiler-measured host layout    |   Before |    After |   Saved |
| -------------------------------- | -------: | -------: | ------: |
| ComputedStyle                    |     44 B |     40 B |     4 B |
| Inline Node                      |    104 B |    100 B |     4 B |
| Inline TreeState, 512 slots      | 62,520 B | 60,472 B | 2,048 B |
| Shared Node on 64-bit host       |     56 B |     56 B |     0 B |
| SharedStyleRecord on 64-bit host |     64 B |     64 B |     0 B |

The host's eight-byte pointer alignment absorbs the style reduction inside the
shared allocation. Do not claim a shared heap saving from these host numbers.
The last S3 Node remains 52 bytes; this batch has not been linked or measured
there. S3 shared-record size, allocator rounding, firmware size and frame latency
remain pending the shared board/build reservation.

The enabled-percentage control exposed stale absolute geometry: changing a
30-pixel width to 50% correctly updated the style but retained the old layout.
The retained-layout classifier now honors explicit structural invalidation before
taking its position-only path. Pixel-only ball movement still uses retained
layout. The failed control and diagnostic logs are preserved.

Validation: 686 analyzer-source cases, 37 CLI feature cases and 46 pre-flash
gate cases pass. Shared enabled-percentage, shared percentage-free and inline
percentage-free native controls pass, including viewport/em-to-pixel transitions,
percentage-to-pixel transitions and cached class changes. The retained-subtree
control also exits successfully. The Bouncing Balls pixel hash remains
`18157712955437833091`. CI covers v19 proof and both retained/pruned native
representations. These checks do not establish a new 60 FPS result.

Artifacts: `balls-v19-percent-original-app-source-proof.json`,
`balls-v19-percent-host-layouts.json`,
`balls-v19-percent-shared-record-host-layouts.json`,
`balls-v19-percent-{default,shared-pruned,inline-pruned}-fixed-regressions.log`,
`balls-v19-percent-subtree-regressions.log`, `balls-v19-percent-gate-tests.log`.
Initial failures remain in `balls-v19-percent-default-regressions.log` and
`balls-v19-percent-default-diagnostic.log`.

The private line-break cache also drops its unread `scaleQ8` member and its
redundant zero assignment. The saved S3 ELF has 16 entries of 420 bytes, totaling
6,720 bytes. With that four-byte member removed, the expected S3 total is
6,656 bytes (64 fewer); a new S3 link has not verified this prediction. Host
alignment keeps the entry at 424 bytes. All 40 renderer fingerprints match the
preceding cache-packing checkpoint across full, bounded and uncached builds.
No cache slots or supported lines were removed. Evidence is saved in
`balls-line-cache-s3-baseline-layout.json`,
`balls-line-cache-cleanup-comparison.json` and
`balls-line-cache-cleanup-renderer.log`.

## Prior linked candidate: unused variable storage and allocation census (v18)

This batch is **not yet qualified on hardware**. The AMOLED 2.06 and its shared
target dependency directory are reserved by the concurrent Knight task.
No pedal board was touched and no package was published.

The unchanged Bouncing Balls app has no CSS custom properties. Analysis v18
automatically removes their two per-rare-node vectors, dependency records and
lookup cache. Authored definitions, `var()`, unknown names/styles and opaque
native code retain support; older analyzers cannot authorize removal.

An S3 build completed before the reservation was extended. Its twelve emitted
app/runtime files match the saved ownership-baseline build exactly, and the only
feature-definition change is `GEA_CSS_CUSTOM_PROPERTIES=0`.

| Linked measurement          | Ownership baseline | First v18 build |
| --------------------------- | -----------------: | --------------: |
| Node                        |               52 B |            52 B |
| ComputedStyle               |               44 B |            44 B |
| NodeRareData                |               64 B |            40 B |
| Override allocation header  |               12 B |             8 B |
| Variable dependency globals |            7,364 B |           512 B |
| Firmware                    |        1,945,840 B |     1,931,648 B |

This is **6,852 B less fixed dependency storage**, **24 B less per allocated
rare record**, **4 B less per override allocation payload**, and **14,192 B
less firmware**. Allocator rounding and live record counts still require the
device census; these are not fabricated total-heap or FPS measurements.
The baseline includes the newly added allocation diagnostics, so its firmware
size differs from the earlier 1,944,880-byte v17 performance checkpoint.

The subsequent source revision reuses an existing Node alignment byte for the
remaining class-tracking flag in shared mode, removing the separate 512-byte
array. Reads remain direct. Compile-time offsets and node-slot reuse are tested.
The inline control keeps its one-byte-per-node array. Unused custom-property
methods are inline no-ops, avoiding new calls from their empty containers.
**This padding revision has host coverage but has not yet been linked for S3.**

The linked-memory audit also found two unused transformed-gradient SIMD buffers,
2,048 B each. They now follow the existing renderer feature proof; the scalar
fallback remains. A direct native-command comparison covers eight opaque,
translucent, varying-alpha and three-stop gradients: all pixel hashes match
with/without caches and pruned cache symbols are absent. This change also awaits
an S3 build and device qualification. The older broad comparison initially
failed because it expected disabled gradient CSS fields to accept authored
gradients. That contract is now corrected: authored gradients are verified in
the enabled build, and full/bounded/pruned builds compare identical native
commands, shapes, transformed text and geometry. All comparisons pass. The
original failure is retained in `balls-unused-gradient-scratch-regressions.log`;
the corrected checks are in `balls-bounded-circle-regressions.log` and
`renderer-features-css-enabled.log`. Gradient coverage was not removed.

The new read-only, post-frame census covers tree ownership, text pool/pages and
character buffers, rare records/attributes, override blocks and CSS-pixel unit
metadata, and dependency globals. It includes spare capacity and flags opaque
shared/callback owners instead of hiding them. Hardware qualification requires
all five groups and no untracked owners. This expands the previous accounting
scope; old 40,956-byte totals must not be directly subtracted from the new census.

Validation so far: 616 analyzer tests, 31 CLI feature tests and 43 gate tests
pass. Focused native shared-pruned, inline-pruned and enabled-variable cases
pass, including growth through every Property, copy/move/erase, variable
inheritance/mutation/removal/fallback, ordinary class changes, and node reuse.
The Bouncing Balls native pixel hash remains `18157712955437833091`.
These are behavior checks, not new 60 FPS results.

The earlier ownership-baseline device capture timed out in ROM download mode
without a RESULT/OWNED frame sample. Its binary and logs are retained; it has no
valid FPS result. Hardware qualification remains pending, including the full
allocation comparison. Last qualified performance remains the v17 matched pair
below, and the board currently belongs to Knight rather than that restored image.

Artifacts: `balls-206-style-shared52-node-owned-baseline*`,
`balls-206-style-shared52-v18*`, `balls-v18-linked-auxiliary-storage.json`.
First v18 SHA-256:
`411f9afbeb2c3af1a7bebb305813d2dd58ac52054352438db6222fc63c74dc92`.

The circle-cache optimization is now implemented and host-verified. The current
original app source proves radius 8. Its build derives maximum radius 8, even
circle span 16, square-box span 19, and four box-cache slots automatically.
The square bound includes the renderer's `floor(side / 2) - 1` tolerance;
sizes 16 through 19 all retain cache capacity. No application switch is added.
DPR is included, and imperative canvas calls, native drawing, unknown styles,
transforms, effects and unsupported proof versions retain full caches.

| Circle cache layout      |   Before | Candidate |            Saved |
| ------------------------ | -------: | --------: | ---------------: |
| Host `CanvasMath` object | 21,480 B |     768 B | 20,712 B (96.4%) |

The existing v18 S3 ELF also confirms a 21,480-byte baseline object. The new
768-byte value is a compiler-measured **host layout**, not a new S3 RAM/FPS
result. All lookup tables remain static; this saves storage rather than moving
it to heap. Native tests compare identical pixels with full, bounded and absent
caches, including cache-edge sizes, larger fallback shapes, alpha and clipping.
The post-link device gate now rejects a CanvasMath layout larger than 768 B.

Circle-cache checkpoint: 634 analyzer-source tests, 34 CLI feature tests, 44 device-
gate tests, and full/bounded/uncached native renderer comparisons pass. Analyzer
source tests use Node's type stripping without rebuilding shared plugin output
while Knight is active. Normal plugin build and S3 qualification remain pending.
A virtual-file fixture initially missed the CLI's `accessSync` check; its failed
log is retained and the corrected native-source test passes. Pedal lint/frontend
checks pass; the initial formatting failure is retained separately.

Artifacts: `balls-circle-cache-host-layouts.json`,
`balls-circle-cache-original-app-source-proof.json`,
`balls-circle-cache-source-analyzer-tests.log`, `balls-circle-cache-cli-tests.log`,
`balls-circle-cache-gate-tests.log`, `balls-bounded-circle-regressions.log`.

Triangle-occlusion scratch is now independently specialized with a positive
`renderer-occlusion-v1` proof. The original app produces
`GEA_EMBEDDED_RENDERER_TRIANGLE_OCCLUSION=0`. Its saved S3 binary contains exactly
**3,072 B** in `s_occlBits`; that symbol is absent from the new disabled host
build. Source-visible triangle methods, aliases, computed dispatch, opaque
imports/evaluation and all custom native producers retain the accelerator.
Old or unsupported analyzers retain it as well; app flags cannot bypass proof.

The existing painter fallback remains available. Eight native pixel comparisons
cover overlap, clipped edges, degenerate triangles, and both sides of the
48-row/256-column scratch limits. Accelerated and fallback pixels are identical;
the full/bounded/pruned renderer comparison passes. The pre-flash gate requires
a linked symbol census and zero triangle scratch for this app. The original
app's computed flags differ from the first v18 link only in the four circle
bounds and the new triangle-scratch flag.

Latest host validation: **652 analyzer-source tests, 36 CLI feature tests,
45 device-gate tests**, plugin type checking and renderer pixel comparisons pass.
These checks neither build shared plugin output nor flash the reserved board.
The circle and triangle changes remove **23,784 B of host static storage** in
total. The new S3 link, usable-heap census, firmware size and 60 FPS run remain
pending; no new hardware-performance result is claimed.

Image-slot audit: linked S3 `ImageSlot` is 72 B and `ImageStore` is 7,012 B.
Regrouping its existing scalar, pointer and boolean fields is calculated to
make each slot 64 B and the store 6,244 B: **768 B less for 96 slots**, with
all named fields still inline. `image-slot-padding-proposal.diff` is prepared
but **unapplied**, because it changes a shared native ABI while Knight is
building/flashing. It needs linked-layout and image-lifetime/decode validation
after the reservation ends. These calculated bytes are not counted as savings.

Latest artifacts: `balls-triangle-scratch-{source-analyzer,cli,gate}-tests.log`,
`balls-triangle-scratch-plugin-typecheck.log`,
`balls-triangle-scratch-original-app-source-proof.json`,
`balls-triangle-scratch-regressions.log`,
`balls-206-style-shared52-v18-with-render-storage.json`,
`balls-image-slot-padding-audit.json`.

## Latest host reduction: cache keys, absent buckets and ready masks

The same automatic one-class proof now narrows both private class-rule cache
keys from six atoms to one. Programs with class overflow retain the six-atom
fast key and their existing fallback beyond six classes. No field indirection
or runtime capacity check is added.

With custom properties absent, the active-rule plan also omits its empty custom
buckets and their cached counts/offsets. Pseudo-element ordinary buckets remain
available. Circle-cache readiness masks now use the integer width required by
the proven radius/span, and the four-slot cursor uses one byte.

| Host record              | Before this batch |   After |            Reduction |
| ------------------------ | ----------------: | ------: | -------------------: |
| RuleIndex                |           3,680 B | 3,552 B |          128 B fixed |
| ActiveRulePlanCacheStore |           4,816 B | 4,720 B |           96 B fixed |
| Bounded CanvasMath       |             768 B |   756 B |           12 B fixed |
| ActiveRulePlan           |             264 B |   136 B | 128 B stack per plan |

The fixed-storage reduction is **236 B**. Each spilled candidate cache record
also shrinks 184 to 176 B and each spilled active-plan cache record 598 to 586 B;
actual allocated savings depend on entry counts and allocator rounding. The
full/default circle cache remains 21,480 B. Relative to the original full circle
cache, the bounded object now saves 20,724 B. Together with triangle scratch and
this batch's two style caches, these host static-storage changes total
**24,020 B**; stack savings are not added to that persistent total.

Shared native tests pass with variables enabled/default class storage and with
variables disabled/one-class storage. They cover 32 distinct class keys,
repeated mutation/reuse, candidate and plan spill, six-class cache hits and
seven-class fallback in the default build, and before/after pseudo-elements.
The original Bouncing Balls pixel hash remains `18157712955437833091` in both.
Full/bounded/uncached renderer comparisons also pass. All 45 device-gate tests
pass; the pre-flash circle ceiling is now 756 B. No new FPS result is claimed:
these layouts are compiler-measured on the host, and S3 linking, heap census
and hardware qualification remain pending the board/build reservation.

Artifacts: `balls-style-cache-layouts.json`,
`balls-circle-bookkeeping-host-layouts.json`,
`balls-cache-bookkeeping-{pruned,default,renderer}-regressions.log`,
`balls-cache-bookkeeping-gate-tests.log`.

## Packed circle tables: all cached sizes retained

The circle table rows are now concatenated at their actual heights. Previously,
every radius reserved the maximum height: the default radius table occupied
64 × 127 × 2 bytes, even though radius `r` only reads `2r + 1` rows. Concatenation
places that radius at row `r²`, reducing the table from 16,256 to 8,192 B. The
even-diameter table likewise stores only `2s` rows for half-diameter `s`, starting
at row `s(s - 1)`. Every previous cached size remains cached; no rows move to heap.

| Compiler-measured host object                   | Before this batch |    After |   Saved |
| ----------------------------------------------- | ----------------: | -------: | ------: |
| Full/default CanvasMath                         |          21,480 B | 12,872 B | 8,608 B |
| Automatically bounded Bouncing Balls CanvasMath |             756 B |    468 B |   288 B |

The bounded object is now 21,012 B smaller than the original full cache.
Together with triangle scratch and the earlier rule-key/bucket reductions,
the recent host static-storage reductions total **24,308 B**. These figures
remain separate from node-owned heap and temporary layout/stack measurements.

The per-row renderer and returned span shape are unchanged. Table-start
addressing uses the formulas above instead of a constant row stride. Therefore
unchanged device latency is **not assumed**: S3 link, census and 60 FPS
qualification remain pending the shared board/build reservation.

A new public-API test traverses all radii 1–64 (including the radius-64 fallback)
and all even diameters 2–32. Ascending fills followed by descending warm hits
exercise neighboring packed tables, with opaque/translucent and clipped/full
painting and guarded output buffers. Its hash is `a8fe09c99cdac523`.
All **40 fingerprints match before and after**, and the full/bounded/uncached
renderer comparisons pass. All 45 pre-flash gate tests pass; the circle ceiling
is now 468 B. The first two baseline compilation attempts used unavailable
private/member APIs; both failed logs are retained. The corrected test uses
`fillRoundedRect` and `Display::setAA`; the engine was changed only after that
baseline passed.

The rule-plan capacity audit found 31 captured app registration calls plus
14 separately injected compiler default rules. Some declarations expand into
multiple registration calls, and native/dynamic producers can add others.
The 96-rule cache capacity is unchanged: authored-CSS counting alone is not a
sufficient proof. Image-slot packing remains unapplied while the shared native
ABI is reserved.

Artifacts: `balls-packed-circle-tables-host-layouts.json`,
`balls-packed-circle-tables-baseline-public-api.log`,
`balls-packed-circle-tables-after.log`, `balls-packed-circle-tables-gate-tests.log`,
`balls-rule-plan-capacity-audit.json`. Initial compile failures are retained in
`balls-packed-circle-tables-before.log` and `balls-packed-circle-tables-baseline.log`.

## Latest result: 44-byte styles with automatic default-field pruning

The v17 source analysis removes another ten unused families from the unchanged
Bouncing Balls app: numeric margins, padding, flex factors, gap, border widths,
border colors, font weight, text alignment, white space and text overflow.
Their exact initial values become compile-time constants. This removes **24 B of
fields**, and regrouping the remaining byte fields removes **4 B of padding**:
ComputedStyle is **72 → 44 B (-38.9%)**. No application opt-in or extra field-load
indirection is introduced by this batch. Authored, unknown and native styles
retain the families conservatively.

The final guarded inline/shared pair uses identical twelve emitted app/runtime
fingerprints and identical native feature defines except for the storage switch.
Both pass the unchanged 300-warmup + 3,600-completed-frame gate on USB AMOLED 2.06,
including DMA, with no device faults.

| Metric                                | Pruned inline control | Pruned shared candidate |
| ------------------------------------- | --------------------: | ----------------------: |
| Node                                  |                 104 B |                    52 B |
| ComputedStyle                         |                  44 B |                    44 B |
| Counted persistent tree/style storage |              62,532 B |                40,956 B |
| Counted layout scratch peak           |              63,224 B |                41,648 B |
| Completed FPS                         |  **60.946940 — pass** |    **61.053440 — pass** |
| p99 completion bound                  |              18.25 ms |                18.25 ms |
| Maximum completion interval           |             32.795 ms |               32.737 ms |
| Intervals over 16.667 ms              |           224 / 3,600 |             187 / 3,600 |
| Firmware                              |           1,945,600 B |             1,944,880 B |
| Free PSRAM                            |           7,254,436 B |             7,276,804 B |
| Free internal heap                    |              66,728 B |                66,664 B |

Shared storage saves **21,576 B (34.5%)** against the matched inline control.
Observed throughput is 0.175% higher in this pair; that is no observed loss,
not proof of a statistically stable speedup or a guarantee that every frame
finishes within 16.67 ms. Shared firmware is 720 B smaller.

Compared with the previous v16 shared checkpoint, counted persistent storage
falls **42,868 → 40,956 B (-1,912 B, -4.46%)** and firmware falls
**1,951,584 → 1,944,880 B (-6,704 B)**. The base Node remains **52 B**.
The 68 allocated style records now request 3,808 B and occupy 3,828 usable heap
bytes, plus allocation headers; static style bookkeeping/default storage is
64 B. Removing 28 B from 68 records plus the default saves 1,932 payload/static
bytes, offset by 20 B of measured allocator slack. The inline Node shrinks
132 → 104 B, saving 14,336 B across its 512 slots.

The counted budget retains the previous scope: TreeState, style blocks and
their usable heap/header sizes, cold-layout pages/tables, layout scratch and
bookkeeping. It **does not include unchanged text/rare-data allocations**, the
common tree allocation header, or runtime/render allocations. It is not the
complete UI heap. All 68 live styles remain unique in a 512-slot tree; spare-slot
savings dominate, and dense unique-style trees can cost more. Shared mode stays
experimental/default-off.

Validation: 604 analyzer tests, 30 CLI feature tests and 42 benchmark-gate tests
pass. Focused native tests cover enabled fields/inheritance, pruned storage,
wide and narrowed representations, ownership and retained rendering. The pixel
hash remains `18157712955437833091`. Pruned shared/profiling-off CI now includes
the single-byte-radius layout. Warning checks remain enabled.

A rejected build is retained as
`balls-206-style-shared52-pruned-defaults-current-runtime*`: **59.933806 FPS**,
56-byte Node, 100-byte ComputedStyle and 50,952 counted persistent bytes.
Its configure command lost the v16/v17 field definitions, range bounds and
class/image/input pruning; it is not the 44-byte-style candidate and was not
rounded up or retried unchanged. The corrected configure flags are archived.
The exact reason that analysis output changed during those builds is not
established. A new pre-flash check of the linked layout rejects style, node or
tree sizes above this app's measured ceilings, even when all translation units
agree. Smaller future layouts remain eligible. The regression test includes
the actual rejected 100/56-byte case. This check runs with the linked-layout
validator used for these qualifications.

Earlier build failures (constant-array comparisons, fixed in the engine; and a
compiler runtime string-view mismatch, corrected in the shared runtime without
a compiler edit from this experiment) are also retained. Initial passing runs
used different runtime headers and are not presented as the matched A/B.
The final pair's raw fingerprints and feature defines agree. Cross-checkpoint
FPS changes still cannot be attributed solely to pruning because compiler
inputs differ from v16.

Artifacts are `balls-206-style-{shared52,inline}-pruned-defaults-guarded*`.
Shared SHA-256:
`46843980fe69c3c65199bf6d347d70757cbc320a93adf8396be2fd7f88bb8f3c`.
Inline SHA-256:
`54fbe05fe24a8b46899299181924e1487b48edeb72c3d01d08663e35a689f936`.
The shared image is restored; USB summary reports 68 nodes at 410×502, and
`balls-206-style-shared52-pruned-defaults-guarded-restored.png` shows the original
balls with FPS: 60. Restoration checks are not another qualification sample.
No package publication, push or pedal-board flash was performed.

Work continues beyond the 50-byte milestone. The next audit must include text,
rare-data and override allocations. Remaining candidates include proving unused
percentage representations absent and removing empty rare-data containers.
Narrower text/tag handles could reduce the base node without adding field
indirection, but their range and lifetime bounds are not yet proven or implemented.

## Previous checkpoint: v16 unused common style fields

The v16 whole-source analysis removes nine unused common-style families from
the unchanged Bouncing Balls app: flex direction, justify-content, align-items,
box sizing, auto margins, unitless line height, min height, max width and active
background. Deferred width remains enabled because the app uses `100vw`.
Opaque/native styles retain the fields conservatively. There is no app opt-in,
runtime feature branch, or added field-load indirection from this pruning.

Both fresh USB AMOLED 2.06 builds passed the unchanged 300-warmup +
3,600-completed-frame gate, including DMA. All twelve emitted fingerprints
match between these two builds.

| Metric                                | Pruned inline control | Pruned shared candidate |
| ------------------------------------- | --------------------: | ----------------------: |
| Node                                  |                 132 B |                    52 B |
| ComputedStyle                         |                  72 B |                    72 B |
| Counted persistent tree/style storage |              76,868 B |                42,868 B |
| Counted layout scratch peak           |              77,560 B |                43,560 B |
| Completed FPS                         |  **61.401557 — pass** |    **60.610815 — pass** |
| p99 completion bound                  |              18.25 ms |                18.25 ms |
| Maximum completion interval           |             32.650 ms |               33.814 ms |
| Firmware                              |           1,951,104 B |             1,951,584 B |
| Free PSRAM                            |           7,238,388 B |             7,272,680 B |
| Free internal heap                    |              66,728 B |                66,632 B |

Shared storage saves **34,000 B (44.2%)** against this matched inline control.
Its observed throughput is **1.288% lower** in this pair. Both exceed sustained
60 FPS and satisfy the existing tail limits; neither establishes that every
frame meets 16.67 ms or proves a statistically stable speed difference.

Against the previous shared checkpoint below, ComputedStyle shrinks 88 to 72 B,
counted persistent storage shrinks **43,972 to 42,868 B (-1,104 B)**, and firmware
shrinks **1,954,976 to 1,951,584 B (-3,392 B)**. The storage saving is exactly
16 B across each of 68 live records plus the static default record. The base
Node remains 52 B; external storage savings must not be advertised as a smaller
base node. The inline node shrinks 144 to 132 B, saving 6,144 B in its 512-slot
tree. The latest inline control passes; the older failure below remains recorded.

The shared compiler emitted different type numbering and an added host-audio
runtime helper compared with the preceding checkpoint. Their raw diffs are
saved; no compiler or generated-source edits were made for this batch. The
new inline/shared pair uses matching current output. Before/after FPS changes
across checkpoints are observations, not isolated attribution to CSS pruning.

The budget counts TreeState, style allocations and usable sizes, four-byte
allocation headers, cold-layout pages/tables, and style/layout bookkeeping.
It excludes other unchanged node text/rare-data allocations, common tree
allocation metadata, and runtime/render allocations; free heap is reported
alongside it. It is not the complete application's UI heap. The app still has
68 live nodes in 512 reserved slots and all 68 final styles are unique. Shared
storage remains experimental/default-off because dense unique-style trees can
cost more.

Validation: 568 analyzer tests, 29 CLI feature tests, enabled-field native
behavior, pruned shared/profiling-off behavior, and pruned inline behavior pass.
The retained pixel hash remains `18157712955437833091`. Focused CI now includes
v16 proof cases and both pruned representations. A stale native audio test stub
was adapted to the existing shared-state header without changing production
audio behavior. Linked UI record layouts are consistent in both firmware builds.

Artifacts are `balls-206-style-{shared52,inline}-pruned-base*` in the audit
directory. Shared SHA-256:
`72447bad4b2ea195655e89fc04b20fa712293add5342dac4ae7d300420a774a8`.
Inline SHA-256:
`52eb84f0124177e31a91c3d43a85b21ef118d62865291f5e17d17cb379eb0525`.
The smaller qualified shared image is restored. USB summary confirms 68 nodes
at 410×502, and the saved screenshot shows the original balls and FPS: 60.
See `restore-shared52-pruned-base.log`, its summary log, and
`balls-206-style-shared52-pruned-base-restored.png`. This is restoration
verification, not a second qualification sample. No package publication, push,
or pedal-board flash was performed.

The goal remains to minimize the complete associated allocation beyond a
50-byte base-node milestone. The linked layout exposes four avoidable padding
bytes in each 72-byte style: offsets 2–3, 43 and 71. Reordering existing byte
fields is the next zero-indirection candidate, alongside automatic elimination
of unused numeric margin/padding, flex, gap and border defaults. These next
reductions are not implemented or claimed as measured savings yet.

## Previous checkpoint: smaller shared build passes, inline control fails

The latest original-app measurements use matching emitted app/runtime inputs
on USB AMOLED 2.06, with 300 warmup and 3,600 completed frames including DMA.
The passing shared image is `balls-206-style-shared52-no-profiling-writes.bin`;
restoration is recorded in `restore-shared52-no-profiling-writes.log`.
Shared storage is still experimental and disabled by default.

| Metric                                | Current inline control |     Shared candidate |
| ------------------------------------- | ---------------------: | -------------------: |
| Node                                  |                  144 B |                 52 B |
| Counted persistent tree/style storage |               83,012 B |             43,972 B |
| Counted layout scratch peak           |               83,704 B |             44,664 B |
| Completed FPS                         |   **59.693220 — fail** | **60.218928 — pass** |
| p99 completion bound                  |               18.50 ms |             18.50 ms |
| Maximum completion interval           |              33.362 ms |            32.286 ms |
| Firmware                              |            1,954,400 B |          1,954,976 B |
| Free PSRAM                            |            7,230,852 B |          7,270,040 B |
| Free internal heap                    |               66,728 B |             66,616 B |

Shared storage saves **39,040 bytes (47.0%)** against this matched control,
including external styles, cold pages, pointer tables, static bookkeeping and
nominal allocator headers. Both exclude the common TreeState header and
unrelated runtime allocations. The base record is not the full per-live-node
cost: only 68 of 512 reserved slots are live, and all 68 final styles are unique.
A dense tree of unique styles can reverse the benefit.

All twelve emitted fingerprints match. The shared run meets sustained 60 FPS
and the unchanged tail limits; the inline run fails and is preserved as a
failure. Neither is an every-frame 16.67 ms guarantee, and this pair does not
establish a statistically reliable speedup. The default inline performance
failure remains unresolved; this is not a release-ready result for both modes.
The earlier passing pair is retained below as historical evidence.

Both profiling modes pass focused host pixels, geometry, ownership and class
checks. The captured framebuffer hash remains `18157712955437833091`. With
profiling off, the actual diagnostic backing store remains untouched. S3
assembly confirms counter references are absent, and the linked discarded
744-byte stats object has disappeared. Relative to the prior shared passing
image, counted persistent storage is 1,024 bytes smaller and firmware is
6,320 bytes smaller; generated runtime inputs changed across that older pair,
so its FPS difference is not isolated attribution.

No release, publish, push or pedal-board flash was performed. The smaller
shared image is the one restored to the USB 2.06. Further automatic elimination
of unused base-style fields and the inline failure remain to be investigated.

## Previous passing pair: 52 bytes on USB AMOLED 2.06

The matching current inline/shared builds use identical emitted app/runtime
fingerprints and the original JSX/CSS/font workload. Both passed the unchanged
300-warmup + 3,600-completed-frame gate. The optimized image is saved as
`balls-206-style-shared52-parent-area.bin`; deployment restoration is logged in
`restore-shared52-qualified.log`. Shared storage remains an experiment, disabled
by default. No package publication, push or pedal-board flash was performed.

| Metric                                | Current inline control | Shared candidate |
| ------------------------------------- | ---------------------: | ---------------: |
| Node bytes                            |                    144 |               52 |
| Counted persistent tree/style storage |               84,036 B |         44,996 B |
| Completed FPS                         |              60.205064 |        60.109305 |
| p99 completion bound                  |               18.50 ms |         18.50 ms |
| Maximum completion interval           |              32.174 ms |        32.064 ms |
| Firmware                              |            1,960,944 B |      1,961,296 B |
| Free PSRAM                            |            7,228,840 B |      7,269,028 B |
| Free internal heap                    |               65,928 B |         65,784 B |

The persistent saving is **39,040 bytes (46.5%)**, after shared-style records,
cold layout pages, pointer-table capacity, static bookkeeping and allocation
headers. Both exclude the common TreeState allocation header and unrelated
runtime/render storage. The 52-byte base record alone is 63.9% smaller; it is
not the complete per-live-node cost. The app uses only 68 of 512 reserved slots
and its 68 final styles are unique. Most of the benefit comes from removing
inline storage from unused slots, not deduplicating those live styles. A dense
tree with unique styles can reverse that benefit; these numbers do not justify
turning shared storage on globally.

The measured FPS difference is -0.159%, not proof of zero overhead or a
statistically established slowdown. Both runs meet sustained 60 FPS and the
same tail limits; neither guarantees every frame completes within 16.67 ms.
Firmware grows by 352 bytes. The host retained pixel golden is identical in
both representations, and moving parent/child, overflow, percentage-anchor,
layout-memo and ownership regressions pass.

The smaller candidate's final structure consists of a four-byte style pointer,
four-byte text owner, 14 bytes of links/tag/rare handle, 18 bytes of current and
previous geometry/inline bookkeeping, one byte of kind, ten bytes of render
state and one byte of padding. Its nine cold layout bytes are counted separately
in stable pages rather than silently omitted from the budget.

## Initial implementation scope

The later measured layout-storage reductions are recorded chronologically below.
The initial style-only experiment kept links, layout memo, previous geometry, render state, tag IDs and text handles
as they were. It replaced only the embedded computed-style record with a shared record
pointer in an experimental build. Keep the inline representation for a matched
control build; both builds compile the same app, engine code and instrumentation.
The A/B switch is a development tool, not an application opt-in requirement.

Native `Node.style` reads become a const `Node.computedStyle()` reference. Group repeated
reads in each operation so the compiler can reuse the loaded pointer. Explicit
`mutableStyle()` calls detach only when the record has another owner. Do not
return mutable references from read APIs. Intern equal stable computed styles at
style-recomputation boundaries, not on every frame or every field read. Initially
exclude records with separately owned rare-style handles from interning. Retain
correct rare-style cloning and destruction. The integrated implementation compares semantic fields, not struct padding.
The original contained prototype used byte equality; it is not the engine implementation.

Migration scope is the native engine/elements call sites, plus target diagnostics
that inspect Node.style. There was no broad test migration. The existing native test host was adapted
for the focused regression harness after the user explicitly requested regression
tests. No layout accessor migration is included. The earlier 63-file
style-and-layout proposal remains withdrawn.

Const compilation must identify every write and reference escape. Then test
copy/move/reuse, CSS inheritance, inline overrides, class changes, rare-style
ownership, pixel/geometry equivalence, allocation counts, and original JSX
bouncing-ball behavior. Only a passing candidate is eligible for the device A/B.
Use only the USB AMOLED 2.06 for the experiment; leave the pedal board alone.

## Completed contained prototype

`tests/ui_shared_style_prototype.cpp` leaves the framework unchanged. ASan/UBSan
passes copy-on-write isolation, moves, self-assignment, 128,000 dynamic writes and
complete record reclamation. With the pedal's 80-byte ComputedStyle on the host:

| Workload                              | Handle bytes | Record bytes | Inline style bytes |
| ------------------------------------- | -----------: | -----------: | -----------------: |
| 160 nodes, 10 equal style groups      |        1,280 |        1,040 |             12,800 |
| Same nodes, 64 independently modified |        1,280 |        7,696 |             12,800 |

These are requested payload bytes on a 64-bit host, excluding allocator overhead
and the common default record, **not measured S3 heap savings or frame timings**.
Whole-style sharing does not combine nodes with different positions. The final
engine census must include every supporting allocation and the default record.

## Device baseline setup

Use the unchanged `geastack/examples/apps/bouncing-balls-jsx` app on the USB AMOLED
2.06, serial `80:B5:4E:DA:73:88`, with the Geastack targets and compiler. The app
requests 120 FPS without vsync, 64-row/depth-2 flushing and its original Inter
font. The examples repository's `npm run test:balls:device` temporarily adds measurement
defines and then restores the app manifest; `-- --shared-styles` selects the experiment. Do not substitute an adapted pedal benchmark.

Build against `/Users/dashersw/Projects/Code/github/geastack/compiler/dist` through
`GEA_COMPILER_DIR`; the pedal's installed compiler is older. Both variants now build the current engine source directly, including the
shared-style migration, with matching core headers. The default keeps inline
storage; the experimental define changes ownership and layout only.

A target hook records scheduler work including display flush and completion
cadence, skips 300 warm-up frames and collects 3,600 frames. Histograms give 250 us
upper bounds for p99; maximum and over-budget counts are exact. A single summary
prints after collection. Temporary phase logs are diagnostic only and must be
removed for the final comparison. Scheduler completion is not a photodiode
measurement of panel scanout.

The original-app inline baseline initially measured 11.495 FPS. Restoring valid
clipped replay and retained layout, using the current compiler, and removing
repeated position parsing and display-list scans brought it to 58.851 FPS.
Results and firmware images are saved under `build/css-feature-audit/balls-206-*`.
The final deployed inline baseline now measures 60.132 FPS over 3,600 frames,
with zero watchdog warnings. Its p99 completion interval is ≤18.500 ms and its
maximum is 31.865 ms: average throughput reaches 60 FPS, but this is not a strict
16.67 ms guarantee for every frame. See `docs/BOUNCING_BALLS_BASELINE.md` for the
full comparison. Host ownership checks alone do not establish frame performance. The subsequent
shared-style device measurements are recorded below.

## Integrated experiment: 2026-09-30

Pre-experiment work is checkpointed in pedal `422a404`, core `3acb4b5`, CLI
`ef6eccb`, and targets `77bdfc3`. Nothing has been published or pushed.

The integrated inline/shared host builds pass the retained 64-ball behavior
checks and produce the same framebuffer hash, `18157712955437833091`. Shared
ownership checks cover copy-on-write, move/self-assignment, semantic interning,
2,000 detach/write/reclaim cycles, class sharing, inline override isolation,
rare-style cloning and inherited updates. This is pixel parity for these host
scenes, not a claim that every possible app is bit-exact.

All following runs use 68 live nodes, capacity 512, 300 warm-up frames and 3,600
measured completed frames on the USB AMOLED 2.06. Firmware, ELF, logs and JSON
are preserved in `build/css-feature-audit/balls-206-style-*`.

| Variant                                  |  Node |     Tree | Style record payload |    FPS | p99 upper bound |   Maximum | Gate |
| ---------------------------------------- | ----: | -------: | -------------------: | -----: | --------------: | --------: | ---- |
| Initial inline control                   | 168 B | 99,392 B |                    0 | 60.203 |        18.50 ms | 32.117 ms | Pass |
| Initial shared candidate                 |  76 B | 52,288 B |              7,616 B | 60.239 |        18.50 ms | 32.134 ms | Pass |
| Shared, alignment padding removed        |  72 B | 50,240 B |              7,616 B | 59.800 |        18.75 ms | 32.996 ms | Fail |
| Fresh inline control                     | 168 B | 99,392 B |                    0 | 60.195 |        18.25 ms | 32.116 ms | Pass |
| 72 B with matching emitted runtime       |  72 B | 50,240 B |              7,616 B | 59.778 |        18.50 ms | 33.822 ms | Fail |
| Final shared candidate, packing reverted |  76 B | 52,288 B |              7,616 B | 60.273 |        18.50 ms | 19.477 ms | Pass |

The initial shared run had 68 style records at 112 bytes each, plus 120 bytes of
static style bookkeeping. Free PSRAM increased by 39,228 bytes against the initial
control, while free internal RAM decreased by 128 bytes. The tiny FPS difference
is not evidence of a speedup. Most of the memory gain comes from no longer
embedding styles in 444 unused node slots; independently moving balls do not
share their final style records. This is not yet a reason to enable whole-style
sharing for every workload, especially a dense tree of unique styles.

The 72-byte layout grouped its two four-byte owners before the 16-bit fields.
It introduced no extra indirection, but failed the strict FPS gate. Because
compiler/plugin inputs changed during the experiment, a fresh inline control
was built. All 12 emitted C++/runtime-header fingerprints matched that control
for the final 72-byte run. It still measured 59.778 FPS versus 60.195 FPS inline
(-0.69%). The packing change was therefore removed from source. Both failures
are preserved, not rounded up or discarded.

The final 76-byte build is deployed on USB AMOLED 2.06 and passed at 60.273 FPS.
Its firmware SHA256 is
`0a329a04e2e33286c2cf733f309ee7cbcead401cde111d2f4edb00be95069d01`.
It occupies 1,971,104 bytes, just 656 bytes more than the fresh inline image.
The final runtime header changed again while preparing this build, so its tiny
FPS difference from the control is not a causal speedup claim. The device gate
independently establishes sustained throughput above 60 FPS for this run.

The final tree plus style payload/static bookkeeping is 60,024 bytes versus
99,392 bytes inline: 39,368 bytes less (-39.6%), excluding heap allocator headers.
The 68 style allocations each have a 112-byte usable block. The S3 IDF TLSF
allocator charges another four bytes per allocation with heap poisoning off:
272 additional bytes. Observed internal RAM use grew by 128 bytes (including
the 120-byte declared static storage). Accounting for those costs gives a
39,088-byte reduction attributable to tree/style storage.

The final observed free PSRAM is 7,256,872 bytes, versus 7,214,508 in the control
(+42,364); free internal RAM is 65,192 versus 65,320 (-128). Free-heap deltas
also include differences in runtime caches, so the entire PSRAM delta must not
be attributed to shared node storage. The first 76-byte run had a smaller
39,228-byte free-PSRAM gain, despite identical node/tree/style sizes.

Both representations passed the focused host behavior suite. The shared suite
now runs in CI against the captured inline framebuffer golden. The gate records
emitted native source hashes, verifies the linked UI ABI before device tests
when configured, and removes a stale passing report before a new attempt.
The original JSX/CSS/font workload and app manifest are unchanged after the run.
No pedal-board flash, release, or push was performed.

The roughly 50-byte goal remains unfinished. Layout memo fields cannot all be
moved to disposable per-pass scratch: `layoutNodeScoped` reuses the first slot's
available width/height and validity across frames. That behavior must survive
any later memo-storage redesign.

## Pass-local layout memo: 64-byte node (2026-09-30)

Moved ten bytes of memo results/secondary keys per node into scratch allocated
only for a layout pass, kept through absolute-coordinate resolution and freed
before retained rendering. The available box and its validity remain inline for
cross-frame scoped relayout. Failed allocation falls back to uncached layout.
Tree reset and node reuse clear ownership and stale entries.

The unchanged original Bouncing Balls JSX app on USB AMOLED 2.06 passed the
300-warmup + 3,600-completed-frame gate. All twelve generated-source hashes match
the saved 76-byte control.

| Metric                          | Shared 76-byte control | Shared 64-byte memo |
| ------------------------------- | ---------------------: | ------------------: |
| Completed FPS                   |              60.272532 |           60.221463 |
| p99 completion bound            |               18.50 ms |            18.50 ms |
| Maximum interval                |              19.477 ms |           33.038 ms |
| Tree allocation                 |               52,288 B |            46,144 B |
| Style usable allocations        |                7,616 B |             7,616 B |
| New persistent memo bookkeeping |                    0 B |                 8 B |
| Pass-local memo payload peak    |                    0 B |             5,120 B |
| Pass-local memo at measurement  |                    0 B |                 0 B |
| Free PSRAM                      |            7,256,872 B |         7,260,140 B |
| Free internal heap              |               65,192 B |            65,192 B |
| Firmware                        |            1,971,104 B |         1,969,600 B |

Persistent modeled savings: 6,144 minus 8 = **6,136 bytes**. Including the
5,120-byte scratch allocation and the target allocator's four-byte block
header, the layout-phase modeled saving is **1,012 bytes**. Other runtime
allocations explain why free-heap movement differs from this structural budget.
This preserves the strict sustained-60 gate; it does not promise every frame
finishes within 16.67 ms or claim a speedup.

The focused shared and inline behavioral suites pass the unchanged framebuffer
hash `18157712955437833091`. New cases protect memo lifetime, two-slot MRU,
external size invalidation, scoped success/rejection cleanup, node-slot reuse,
no-scratch correctness, serial wrap and tree-clear cleanup. The 84-row nested
flex geometry/work trace exactly matches `repaint-layout_memo_work-full.txt`.
The gate's 37 tests pass; it now rejects missing scratch accounting or a cache
left allocated during retained frames. Linked ABI layouts are consistent.

Artifacts: `build/css-feature-audit/balls-206-style-shared64-memo-*` and saved
`.bin` / `.elf`. Firmware SHA-256:
`9a5797ee8e24a63a5fb861016ad1f4bee80efabb41fece4e281115ea1638e84e`.

## Display-import reachability correction (2026-09-30)

The whole-program analyzer treated `import { Display }` as an opaque native
factory. This unnecessarily retained image/input payloads and disabled all CSS
range narrowing in the unchanged Bouncing Balls app. Recognizing this specific
panel/canvas host fixes both false positives. Calls that change DPR, including
aliased/destructured/bracket calls, and unknown factories remain conservative.
The plugin build and 61 focused analyzer cases pass.

The full-capacity memo intermediate passed at **60.154260 FPS** (p99 <=18.50 ms,
max 32.263 ms), with every generated C++/header fingerprint still identical to the
previous run. Node remains 64 bytes; ComputedStyle falls 100→88 bytes. TreeState
falls 46,144→43,064 bytes; live style allocations 7,616→6,800 bytes; style static
bookkeeping 120→108 bytes. This adds **3,908 bytes** of modeled persistent savings.
Firmware falls 1,969,600→1,959,136 bytes, a 10,464-byte reduction.
Free PSRAM is 7,263,752 bytes; free internal heap 65,832 bytes.

The scratch cache in this intermediate still peaks at 5,120 bytes. A subsequent
candidate sizes it to allocated node slots at pass entry (68 slots: 680 bytes).
New nodes created mid-pass take the uncached path until the next pass, preserving
addresses held by recursive layout calls. Its extra capacity counter costs 4
persistent bytes. The focused shared regression passes, including growth during
layout and unchanged retained pixels. Hardware qualification of that subsequent
candidate is tracked separately.

Intermediate artifacts: `balls-206-style-shared64-display-analysis-*`.
Firmware SHA-256:
`6b21e5cb7982d10f94e0695fe566e2b046a530a4e5efad8dc1cda6edd0c2edaa`.

### Bounded memo result

The bounded-cache candidate passed at **60.125537 FPS**, p99 <=18.50 ms,
maximum 32.046 ms, with no device faults. The hardware census confirms 680 bytes
of peak scratch payload and zero bytes retained. Node 64; style 88; TreeState 43,064;
style usable allocations 6,800; style static 108; memo static 12 bytes. Free PSRAM
7,263,752 and internal heap65,816 bytes. All twelve emitted fingerprints match
the 76-byte control. Firmware 1,958,192 bytes; SHA-256
`9e6c730780ec3fe291478f88c0eb137010238997d23ac54789d47de807294861`.
Artifacts: `balls-206-style-shared64-active-memo-*`.

Compared with the 76-byte checkpoint, this removes **10,040 persistent bytes**
including all declared bookkeeping and 68 style allocation headers. During a
layout pass, including the 680-byte scratch plus 4-byte allocator header, the
modeled net saving is **9,356 bytes**. Shared and inline behavioral suites and
the 37-case hardware-gate parser suite pass.

## Padding-only 60-byte node (2026-09-30)

Grouping the shared-style pointer and text handle ahead of two-byte members
removes the remaining four padding bytes. No new accessor, indirection or
bitfield was introduced. The default inline representation keeps its member
order. The shared native suite passes the unchanged retained framebuffer hash
and all ownership/memo cases. Linked ABI records are consistent at 60 bytes.

The original JSX app passed on USB AMOLED 2.06 at **60.110233 FPS** over 3,600
completed frames after 300 warmup, p99 <=18.50 ms, max 32.576 ms, no faults. All
12 emitted fingerprints match the 76-byte control. TreeState 41,016 bytes;
node array 30,720; 68 style allocations 6,800; style static 108; memo static 12;
memo peak 680 and current 0 bytes. Free PSRAM 7,268,240; internal 65,816 bytes.
Firmware 1,961,184 bytes, SHA-256
`124ac9c7dbc1cba12c425083fe3394cfe03a71e6a19e0c64d03304fff20dfb80`.
Artifacts: `balls-206-style-shared60-packed-*`.

The counted persistent tree/style budget, including 68 four-byte style allocation
headers and memo bookkeeping, is **48,208 bytes**, down from 60,296 for the 76-byte
checkpoint: **12,088 bytes saved (20.0%)**. During layout, include the 680-byte
scratch plus 4-byte allocation header: 48,892 bytes, still 11,404 below the old
persistent budget. The common tree allocation header is excluded from both.
This does not count unrelated runtime/text/display allocations; do not equate
the free-heap delta with structural savings. Versus the earlier inline control's
99,392-byte tree, the counted saving is 51,184 bytes (51.5%).

The deployed node's remaining 60 bytes contain 4 bytes of style ownership,
4 of text ownership, 14 of links/tag/rare-data handle, 27 of layout, 1 of kind,
and 10 of render state. There is no padding left to remove in this build.
Getting closer to 50 requires removing or changing live state, such as previous
geometry; this checkpoint does not claim the 50-byte objective is finished.
No releases or pushes were performed.

## Cold layout pages: initial 52-byte experiment (2026-09-30)

The shared representation moves nine cold layout bytes (available box, memo
validity and static-position anchor) into stable pages of 16 allocated slots.
Geometry and render state remain inline. Default inline builds retain the old
fields. Pages survive recursive node creation and are reclaimed with the tree;
all pages, pointer-table capacity, headers and growth overlap are counted.

The initial candidate **failed** the unchanged hardware gate at **59.914222 FPS**
(3,600 completed frames after 300 warmup), p99 <=18.50 ms, max 32.970 ms. Node 52,
style 88, TreeState 36,920 bytes, style heap 6,800, cold layout payload 832/usable 840
bytes in six allocations, cold-layout static 20, style static 108, memo static 12.
Peak scratch 680; active scratch 0. Free PSRAM 7,271,460 and internal 65,800 bytes.
Firmware 1,961,248 bytes; SHA-256
`0050915d583388bc7544412376cae5c6e935b4db4931810e160015e013b5151d`.
Artifacts: `balls-206-style-shared52-cold-layout-*`. The initial page-growth
peak 848 was requested payload, not allocator usable size; a subsequent change
corrects that census and strengthens the gate to reject understated usable peaks.

The original JSX source/CSS hashes passed. Eleven emitted hashes match the
60-byte control, but `index.cpp` differs, so the timing delta cannot be attributed
solely to storage. A fresh inline control is required before drawing that
conclusion. The saved passing 60 image was restored successfully while the next
candidate was prepared.

Focused shared/inline behavior suites pass, including the retained pixel golden
`18157712955437833091`, stable page references across growth, reclamation, scoped
layout and static absolute anchors. The 84-row nested-flex work/geometry trace
matches the saved inline trace; the retained-subtree case also passes. Host
correctness and memory reduction do not override the failed hardware result.

### Qualified 52-byte candidate with sibling bounds reuse

The retained position loop now resolves the parent padding box once for each
consecutive group of absolute siblings. Grid areas and fixed-position cases
remain on their child-specific path. Automatic offsets reuse the already
resolved area too. The cache is stack-local and discarded each frame. New
behavior assertions require one containing-area calculation and 63 hits for
64 moving siblings, and compare alternating parent groups, borders and
percentage anchors with full layout. Shared and inline suites pass the same
retained pixel golden. The existing performance thresholds are unchanged.

On USB AMOLED 2.06 this candidate passed **60.109305 FPS**, p99 <=18.50 ms,
maximum 32.064 ms,3,600 samples/300 warmup, no faults. Corrected cold-layout peak
is 856 usable bytes plus 7 four-byte allocation headers during pointer-table
growth; steady state is 840 usable bytes plus 6 headers. Tree/style/cold-layout
bookkeeping and headers total **44,996 persistent bytes**. Adding the 680-byte
scratch payload and its nominal four-byte header gives a **45,680-byte modeled
layout budget**; this scratch peak does not independently measure allocator slack. This is 3,212 bytes
(6.66%) below the previous 60-byte checkpoint, and 15,300 bytes (25.4%) below 76.
The common TreeState allocation header and unrelated runtime/render allocations
are excluded consistently. Page growth precedes the initial layout and is lower
than the measured layout peak in this app.

Firmware 1,961,296 bytes, SHA-256
`9b7dfc30556f720e6f2dad6196f32fccd984c13777a23b02bca83571466b8555`.
Free PSRAM 7,269,028 and internal 65,784 bytes. Artifacts
`balls-206-style-shared52-parent-area-*` include ABI census and emitted sources
for review. Compiler/runtime fingerprints differ from earlier checkpoints;
a matching current inline control is being measured. These historical deltas
do not establish that the sibling optimization alone caused the FPS change.

### Matched current inline control

The fresh inline control passed 60.205064 FPS with 144-byte Node and 88-byte
ComputedStyle. TreeState 84,024 bytes plus 12 bytes of memo bookkeeping gives
84,036 counted persistent bytes. All 12 normalized emitted fingerprints match
the qualified 52-byte candidate. Maximum 32.174 ms, p99 <=18.50 ms,3,600 completed
frames/300 warmup, no faults. Firmware 1,960,944 bytes; SHA-256
`922a3d7d76d456c7490f69b198432469d94fa4f0ca54ea539327d80b784228ca`.
Artifacts: `balls-206-style-inline-current-control-*`. The 52-byte candidate
uses 39,040 fewer persistent bytes and 352 more firmware bytes. Its observed
throughput is 0.159% lower; no claim of zero cost is warranted from one pair.
The qualified 52-byte image is restored after this comparison.

## Completion audit: scratch allocation size and occupancy (2026-09-30)

The earlier 680-byte scratch peak was payload, not the allocator's usable block.
The peak counter now queries the ESP allocator at allocation time and is exposed
as `memo_peak_heap`; the same static counter is reused, so persistent storage
and retained-frame work do not grow. The revised gate rejects the old
payload-only field. All 39 gate tests and focused shared behavior checks pass.

The resulting measurement image failed the strict throughput gate at
**59.903203 FPS** (p99 <=18.50 ms, max 32.834 ms), with all 12 emitted-source hashes
identical to the preceding qualified candidate. This is a failed result, not
a retry opportunity. It confirms **688 bytes** of peak scratch usable storage
plus the nominal 4-byte header. The persistent budget remains 44,996 bytes and
the counted layout-phase budget is 45,688 bytes. Firmware 1,961,296 bytes; SHA-256
`4e090948fe9ed9f5e1f6fe1a8fba8c446c6e6ff732f39c740478da52e1a74db0`.
Artifacts: `balls-206-style-shared52-heap-census-*`. The qualified 52-byte image
was restored before preparing a different optimization.

An occupancy model derived from the qualified S3 layouts is saved as
`balls-206-shared-storage-occupancy-model.json`. At 512 live nodes with 512 unique
styles, shared storage costs at least 95,688 bytes versus 84,036 inline: at least
11,652 bytes more. At 512 live nodes but 8 distinct styles, its lower-bound cost
is 43,272 bytes. These are projections excluding extra allocator slack, not
measurements of those workloads. They explain why shared mode remains an
experiment and is not automatically enabled for every application.

A further candidate removes the per-slot class overflow handle only when the
whole-source class bound is proven. Bouncing Balls uses one class per node, so
its `NodeClassList` can shrink 6 to 4 bytes: 1,024 bytes across 512 reserved slots.
Older analyzers, unknown mutation, opaque native UI sources, future proof
versions and incomplete merged proofs retain full overflow storage. No app
opt-in is needed, and no Node field read gains a new load or branch. Hardware
qualification is tracked separately.

### Bounded class-storage result: memory gain, throughput failure

The linked ABI confirms Node 52, NodeClassList 4 and TreeState 35,896 bytes.
The original app passed host behavior/pixel/ownership cases and automatically
selected `GEA_UI_CLASS_INLINE_TOKENS=1;GEA_UI_CLASS_OVERFLOW=0`; its manifest
was not modified to opt in. Analyzer 46 scoped tests, CLI 28 tests and the
39-case device-gate parser pass. Default overflow behavior also passes.

Hardware **failed** at **59.916242 FPS**, p99 <=18.75 ms, max 32.994 ms, 3,600
completed frames after 300 warmup. Counted persistent storage 43,972 bytes;
actual scratch usable peak 688 plus 4-byte header gives 44,664 during layout.
Firmware 1,958,352 bytes, 1,024 fewer persistent bytes and 2,944 fewer firmware
bytes than the preceding 52-byte layout. Free PSRAM 7,272,472; internal 65,864.
SHA-256 `4515557b75bf36fd61b015c737da10e1c7006013518db34a675e9641bf0eaad8`.
Artifacts: `balls-206-style-shared52-bounded-classes-*`. This candidate is not
qualified; the previous passing 52-byte image is restored. No unchanged retry.

Rebuilding the plugin also incorporated existing RTC source changes into
generated runtime headers and renumbered emitted record types; review diffs
are saved as `balls-bounded-classes-*.diff`. No isolated performance attribution
is made across that compiler-output change. The source memory optimization
remains under evaluation while frame-time margin is investigated.

### Profiling-off counter removal: shared candidate passes

The supposedly disabled refresh profiler still incremented counters and wrote
region samples into a discarded 744-byte object. Clock reads had been removed,
but these writes remained in style updates, dirty scans and command replay.
Diagnostic statements now use a compile-time guard; diagnostic-only region and
text-cache census loops are omitted. Profiling-enabled behavior is preserved.
No new per-field lookup, runtime flag check or rendering shortcut is introduced.

The focused shared/compact-class suite passes with profiling enabled and off.
Both produce the established pixel hash `18157712955437833091`. The off build
also fills the actual stats backing store with a sentinel and verifies that
layout, style updates, rendering and reset leave every byte untouched. Macro
arguments are not evaluated. S3 assembly for all eight changed engine files has
no live counter-data references, and the linked ELF no longer contains the
744-byte discard object. Assembly and host logs are saved with the artifacts.

The candidate passes at **60.218928 FPS**, p99 <=18.50 ms, maximum 32.286 ms,
3,600 completed frames after 300 warmup, no device faults. Node remains 52 bytes;
counted persistent tree/style storage is **43,972 bytes**, with **44,664 bytes**
at the measured layout scratch peak. Firmware is **1,954,976 bytes**, 3,376 bytes
smaller than the failed bounded-class candidate. Free internal heap is 66,616
bytes and free PSRAM 7,270,040 bytes. These heap totals include unrelated runtime
allocations; the complete delta is not attributed to the counter removal.

Artifacts: `balls-206-style-shared52-no-profiling-writes-*`; SHA-256
`97f0238d273af6a8e2cc74815f36199679cb0b4be374a1704437a04f40877efc`.
Existing RTC and compiler runtime changes also affected three generated files
relative to the failed candidate, so the observed FPS increase from 59.916242
is not an isolated causal measurement of counter removal. Saved diffs document
that change. The fresh inline control uses all twelve matching emitted fingerprints but
fails at **59.693220 FPS**, p99 <=18.50 ms, maximum 33.362 ms. Its 144-byte node
and 83,012-byte persistent budget confirm a 39,040-byte shared saving. Firmware
is 1,954,400 bytes; SHA-256
`0f7f34a35bcec5f9f1926ba515c5c2a28f4f9b4380f7636c5fb85dd504ccefa6`.
The failure, firmware and emitted inputs are archived as
`balls-206-style-inline-no-profiling-writes-*`. No unchanged retry was run.

Restoration verification: esptool verified the written image but reported a
serial disconnect after reset. The CLI recorded flash completion. A later USB
`devctl summary` answered with 68 nodes on the 410x502 display, and a captured
framebuffer shows the original balls app with the FPS badge. Artifacts:
`restore-shared52-no-profiling-writes-summary.log` and
`balls-206-style-shared52-no-profiling-writes-restored.png`. The short passive
boot-log attempt received no startup text; application state and framebuffer
readback establish that it is running. This was not another qualification run.

## September 30: combined packed storage on AMOLED 1.8

The original Bouncing Balls JSX app now runs on the registered USB AMOLED 1.8
(`amoled-18`, serial `30:ED:A0:AC:90:DC`) with a 48-byte Node. The combined
candidate is left running on that board. The other boards were not flashed.
No app source, CSS, font, ball count, frame-rate request or flush settings changed.

| Measurement                                | Saved before | Combined after |
| ------------------------------------------ | -----------: | -------------: |
| Base Node                                  |         52 B |           48 B |
| ComputedStyle                              |         40 B |           32 B |
| SharedStyleRecord, including ComputedStyle |         48 B |           40 B |
| NodeRareData                               |         40 B |           12 B |
| RenderState, included in Node              |         10 B |            6 B |
| TreeState, 512 slots                       |     35,896 B |       33,848 B |
| Node-owned heap                            |     46,436 B |       42,244 B |
| Node-owned static storage                  |        216 B |          172 B |
| Node-owned total                           |     46,652 B |       42,416 B |
| Free PSRAM                                 |  7,386,632 B |    7,390,348 B |
| Free internal RAM                          |     85,232 B |       85,248 B |
| Firmware                                   |  1,766,688 B |    1,761,872 B |
| Completed FPS                              |    67.354977 |      67.868017 |
| Mean frame interval                        |    14.846 ms |      14.734 ms |
| p99 interval upper bound                   |    16.750 ms |      16.500 ms |
| Maximum interval                           |    30.206 ms |      30.119 ms |
| Intervals above 16.667 ms                  |   27 / 3,600 |     21 / 3,600 |

Each timing result uses 300 warm-up frames followed by the first complete
3,600-frame capture. The candidate passes the existing 60 FPS, 19 ms p99 and
35 ms maximum gate without faults. The observed FPS difference is +0.76%; this
single comparison establishes no throughput regression in the measured workload,
not a repeatable speedup or an every-frame 16.667 ms guarantee.

Node-owned memory falls 4,236 B (9.08%), and firmware falls 4,816 B. The ownership
sum includes the tree allocation, text/rare/override/dependency allocations,
shared styles, persistent layout pages, and their reported static owners. It
excludes unrelated renderer/CSS caches and released layout scratch. ComputedStyle
is already inside SharedStyleRecord; do not add those two table rows together.
Allocator rounding explains why payload savings and usable-heap savings differ.
Free-heap deltas are separate observations, not additional savings to add.

### Implementation

Small display, position, background and render flags use bitfields, preserving
all supported values, including overflow's signed inheritance sentinel. Color
and coordinate precision are unchanged. The Node's existing shared-style pointer
remains unchanged; this batch adds no additional style-pointer indirection.

The compiler plugin proves usage across discovered sources and imported CSS.
The CLI automatically removes unused listener, attribute and default-style owners,
explicit-display bookkeeping, line-height storage, and percentage-position fields.
Typed numeric position values cannot contain CSS percentages; non-percentage
viewport and font-relative positions also need no percentage slot. Relative
margins still retain their separate deferred-expression storage. Unknown imports,
opaque calls, dynamic styles, native mutation and applicable built-in controls
retain the affected support. No application opt-in or override is needed.

The first device candidate aborted because Document's implicit mount root wrote
an internal ID despite the app having no attributes. That rejected image and log
remain under `balls-18-packed-pruned*`. The corrected engine uses the existing
mounted-root handle for the implicit app root when attributes are proven unused.
Authored ID/attribute selectors and DOM lookups retain normal attribute identity.
A host regression covers root creation, repeated lookup, clear and remount with
attribute owners physically absent. The measurement script now stops on startup
aborts instead of waiting through reboot loops.

### Final byte layout

| ComputedStyle byte offsets | Contents                                                                      |
| -------------------------- | ----------------------------------------------------------------------------- |
| 0                          | display 2 bits, signed overflow 3 bits, position 2 bits, has-background 1 bit |
| 1                          | z-index-auto 1 bit; 7 unused bits                                             |
| 2                          | common border radius, uint8                                                   |
| 3                          | alignment padding                                                             |
| 4–7, 8–11                  | width and height expression handles, int32 each                               |
| 12–13, 14–15               | width and height, int16 each                                                  |
| 16–17, 18–19               | top and left, int16 each                                                      |
| 20–21                      | z-index, int16                                                                |
| 22, 23                     | background and text alpha, uint8 each                                         |
| 24–25, 26–27               | background and text color, full RGB565 each                                   |
| 28–29, 30–31               | font ID and font size, int16 each                                             |

SharedStyleRecord is that 32-byte value followed by a 4-byte reference count and
4-byte next pointer. NodeRareData is a 6-byte inline-position record (two int16
coordinates and two flags), two bytes of alignment/empty-member identity space,
and the 4-byte inline-style owner. Listener, attribute and default-style owners
have no allocated fields in this app. Those owners remain available in builds
whose source requires them.

### Validation and provenance

- All 714 analyzer cases and 39 CLI feature tests pass.
- Enabled-owner and removed-owner native fixtures pass. Packed-field combinations,
  adjacent-bit preservation, signed sentinels and copy-on-write isolation pass.
- The retained-rendering framebuffer hash stays `18157712955437833091`.
- The original application source fingerprint passes. All linked translation units
  agree on sizes, member offsets and newly checked bit offsets/widths.
- The hardware gate now rejects a return to 52-byte Nodes, 40-byte computed styles,
  48-byte shared records, 40-byte rare records or 10-byte render state.
- Pedal lint, type checking and formatting pass. The captured screen shows the
  original balls and FPS label at 68 FPS.

The saved baseline uses generated runtime-header hash
`ebb51c35ff31fe8fb5a7dbcdb1ba2d1da161a86ffe7e7673d71e9d5f76fda4fb`.
The current shared compiler header differs in native property-enumeration helpers;
11 of 12 generated native files, including all application C++, are identical.
Those helper symbols are absent in the compared linked images. Other preserved
checkout changes include media/RTC, 2.06-only code and shared FT3168 diagnostic
counters. The audio-only touch priority change is inactive here. The source
comparison records these differences: this is an observed whole-firmware timing
comparison, not perfect isolation of every FPS delta to packing alone. The compiler
was neither modified nor rebuilt for this batch.

Artifacts are in `build/css-feature-audit/`:
`balls-18-packed-pruned-v2-{comparison,result,acceptance,linked-layout,source-comparison}.json`,
the corresponding `.bin`, `.elf`, `.map`, logs and `-screen.png`.
Candidate SHA-256:
`aec1994bb51706daf4a62b41824e7802e114edbd692aa3e37871fc28013efec3`.
No packages were published and no changes were pushed.

## September 30: packed per-node tree flags on AMOLED 1.8

**Reverted at the user's request:** this experiment produced no throughput or
SRAM benefit. The five separate flag arrays and their previous storage gate are
restored; all earlier optimizations remain. The measurements below are retained
as a record of the rejected experiment.

The original Bouncing Balls JSX was tested on the USB AMOLED 1.8 with the five
per-node flag arrays packed into one byte per slot. Capacity remained 512. This
reduced the accessed flag footprint and the instructions/stores used to clear
frame-local flags, but did not improve completed-frame throughput in the measured
runs. No internal-heap saving was observed.

A fresh current-source control was built before editing, because the earlier
saved firmware preceded unrelated framework changes. Both images used the same
shared compiler, app, CSS, fonts, board configuration and benchmark settings.
All 12 generated native files match exactly; maintained framework differences
are confined to this flag migration and its tests. Neither the compiler nor the
application was changed. The saved earlier image was also re-measured at
67.976609 FPS, but is not used for this isolated comparison.

The measurement order was control, candidate, control, candidate. Each boot had
300 warm-up frames followed by 3,600 measured completed frames, including display
flush. The aggregate FPS below uses 7,200 frames divided by the sum of their
intervals; it is not an average of rounded on-screen FPS values.

| Measurement                             |      Control | Packed flags |
| --------------------------------------- | -----------: | -----------: |
| FPS, run 1                              |    67.836907 |    67.699144 |
| FPS, run 2                              |    67.798515 |    67.725424 |
| Aggregate completed FPS                 |    67.817706 |    67.712282 |
| Mean frame interval                     | 14.745412 ms | 14.768370 ms |
| p99 interval upper bound, both runs     |    16.500 ms |    16.500 ms |
| Worst measured interval                 |    29.882 ms |    29.718 ms |
| Intervals above 16.667 ms, out of 7,200 |           40 |           32 |
| Free internal RAM, both runs            |     85,120 B |     85,120 B |
| Flag payload for 68 live nodes          |        340 B |         68 B |
| Reserved flag payload for 512 slots     |      2,560 B |        512 B |
| TreeState payload                       |     33,848 B |     31,800 B |
| Tree allocation usable size             |     34,816 B |     32,256 B |
| Complete node-owned storage             |     42,416 B |     39,856 B |
| Firmware                                |  1,765,536 B |  1,765,664 B |

The measured throughput change is **-0.155%**, approximately 23 microseconds
more per frame. These runs show no speedup; they are not sufficient to establish
whether that small difference is repeatable across broader conditions. All four
runs pass the unchanged 60 FPS / 19 ms p99 / 35 ms maximum gate without faults.
This is sustained throughput, not a guarantee that every frame meets 16.667 ms.

### Implementation and compiled work

`TreeState::nodeFlags` contains active, command-dirty, dirty-bounds-valid,
can-overpaint and in-backdrop bits. Frame capture clears the three transient
command bits together, preserving membership and backdrop state. The bounds
path reads its related flags once; that read remains after the moving-leaf fast
path, so moving balls do not acquire an unnecessary load. Node creation, removal,
reuse, input, text invalidation, virtual lists and backdrop rendering use the
same masks. The backdrop cooldown is still a separate counter.

The actual linked S3 instructions for the three frame-local flag clears change
from two address adjustments plus three byte stores to one byte load, one AND
and one byte store: five instructions become three per node. The complete
LayoutSnapshot::capture function shrinks from 124 to 118 bytes. These are static
assembly observations, not measured cache-miss or CPU-cycle reductions. Other
flag writes now use read-modify-write operations; the total firmware grows
128 bytes. No new accessor calls or pointer indirection were introduced.

The base Node stays 48 bytes, ComputedStyle 32, SharedStyleRecord 40, NodeRareData
12 and RenderState 6. The linked static-storage census is unchanged. Allocator
rounding makes the 2,048-byte payload reduction release 2,560 bytes of owned heap.
Unused reserved slots remain in PSRAM as requested.

### Validation and artifacts

- The focused native fixture passes with app-like owner pruning and with normal
  CSS/owners enabled. It exercises every flag combination through actual dirty
  APIs and frame capture, and checks removal, interior-slot reuse, neighbouring
  nodes and full reset, including spare slots.
- Both configurations retain framebuffer hash `18157712955437833091`; the existing
  retained rendering, shared ownership and invalidation regressions pass.
- At experiment completion, all 50 hardware-gate tests passed. Its additional
  packed-array storage assertion has been removed with the revert. The previous
  49 gate tests remain. All linked translation units agreed on layout.
- The first compile caught an unmigrated virtual-list active-node reference; it
  was fixed before either candidate device run. Its failure log is preserved.
- The experiment screenshot shows the original balls and FPS label at 68 FPS.
  The candidate has since been rejected in favor of the saved control on
  `amoled-18`, serial `30:ED:A0:AC:90:DC`.

Artifacts under `build/css-feature-audit/` use the prefixes
`balls-18-flags-control`, `balls-18-flags-control-repeat`,
`balls-18-packed-flags` and `balls-18-packed-flags-repeat`.
The comparison, source comparison, assembly comparison, linked layouts, firmware,
logs, exact incremental patch and screenshot are preserved. Primary report:
`balls-18-packed-flags-comparison.json`.

Candidate SHA-256:
`ed9fadc0d5023787567b46054e7ca20d7b0807eabc757121e5090381f181b25c`.
No packages were published and no changes were pushed.

### Reversion verification

Only the flag-packing batch was reversed, using its preserved incremental patch.
All 11 affected framework/test files exactly match their pre-experiment control
hashes. The previous 49 storage/performance-gate tests pass, and the control's
saved linked layouts pass the restored storage ceilings. Later unrelated RTC
source edits were left untouched. The earlier 48-byte Node, 32-byte computed
style, 40-byte shared record, 12-byte rare record and automatic pruning remain.

The exact qualified control image was restored to the USB AMOLED 1.8 without
rebuilding the shared compiler or firmware. Its fresh verification, saved as
`balls-18-flags-reverted-result.json`, passes with 300 warm-up frames and 3,600
measured frames: **67.829196 FPS**, mean 14.742 ms, p99 at most 16.500 ms, maximum
29.724 ms, and 18 intervals above 16.667 ms. No device faults were reported.
TreeState is again 33,848 bytes, with 512 slots and 85,120 bytes free internal RAM.
The rejected experiment's images and evidence remain archived in the build audit.

Restored firmware SHA-256:
`c44bf325699c966f235542a0893aa84c998dc017b39e49d70665f1b2e091751e`.
Source verification: `balls-18-flags-reverted-source-check.json`.

## September 30: package-only release qualification

The reviewed package artifacts use core 0.1.30, engine 0.1.8, elements 0.1.3,
geatsc-plugin-gea 0.1.11, targets 0.1.84 and CLI 0.1.87. Qualification uses the
already-published compiler 1.0.19, host 0.1.9 and simulator 0.1.11. Unrelated
uncommitted RTC, media and compiler development is excluded from these artifacts.
Every source file in each tarball is taken from the reviewed Git commit; the
analyzer's JavaScript and declarations are compiled from that same committed
TypeScript. Artifact hashes and package contents are preserved in
`build/css-feature-audit/release-packages.json`.

All six artifacts are published. Each registry `dist.integrity` matches the
original tarball's SHA-512. Clean `npm ci` installations in the pedal and examples
repositories reproduce every file of those artifacts, with registry lockfile
URLs and no framework symlinks or local-package dependencies. The pedal retains
its qualified TypeScript 5.9.3 and other existing tool versions; an unrelated
TypeScript `latest`/linter peer conflict was resolved without forcing npm.

Package-only checks found and fixed a release defect: the native build looked
only in the app's own `node_modules` and missed typed Gea sources hoisted to the
workspace root. Native source resolution now follows Node's package search chain,
respects nearest-package shadowing, and does not require a public `package.json`
export. Regression coverage includes root apps, workspace children, web
subdirectories, missing typed sources and nested dependencies. No compiler
checkout or local package override is used by either ESP32 qualification build.

### Original Bouncing Balls JSX, AMOLED 1.8

The original source, CSS, font, 64-ball count and display settings pass the
existing workload fingerprints. The registered 1.8 board is the only board
flashed. Linked DWARF inspection checks all translation units before flashing.
The framebuffer readback shows the expected balls and FPS badge.

| Measurement                         | Retained development control | Packaged release candidate |
| ----------------------------------- | ---------------------------: | -------------------------: |
| Completed FPS                       |                    67.829196 |                  67.808032 |
| Mean frame interval                 |                    14.742 ms |                  14.747 ms |
| p99 interval upper bound            |                    16.500 ms |                  16.500 ms |
| Maximum interval                    |                    29.724 ms |                  30.909 ms |
| Intervals above 16.667 ms           |                   18 / 3,600 |                 14 / 3,600 |
| Base Node                           |                         48 B |                       48 B |
| ComputedStyle                       |                         32 B |                       32 B |
| SharedStyleRecord, including style  |                         40 B |                       40 B |
| NodeRareData                        |                         12 B |                       12 B |
| TreeState, 512 slots                |                     33,848 B |                   33,848 B |
| Node-owned heap plus static storage |                     42,416 B |                   42,216 B |
| Free internal RAM                   |                     85,120 B |                   85,984 B |
| Free PSRAM                          |                  7,390,348 B |                7,343,256 B |
| Firmware                            |                  1,765,536 B |                1,798,208 B |

Both samples contain 300 warm-up frames and 3,600 measured completed frames.
The packaged build passes the unchanged 60 FPS / 19 ms p99 / 35 ms maximum gate
without device faults. The observed throughput difference is -0.031%; this is
one qualification comparison, not proof that every frame takes less than
16.667 ms. Node-owned accounting includes shared records and persistent layout
storage; the computed-style row is already included in its shared record.

Replacing the development toolchain with the published compiler and host changes
whole-program size and free heap: the image is 32,672 B larger, free PSRAM is
47,092 B lower, and free internal RAM is 864 B higher. Those whole-program deltas
must not be presented as an additional style-packing improvement or added to the
200 B reduction in measured node-owned storage. The rejected flag-array packing
remains reverted.

Artifacts are named `balls-18-release-resolver-*`. The exact qualified image is
1,798,208 B with SHA-256
`c20d2c0c89ea1c3e89978faed60093e766c09b7bd36756a630123c5fb214cd51`.

### Pedal and web validation

The clean pedal candidate builds with the normal Gea CLI and the installed
packages, with every development override removed. Its binary is 2,958,864 B;
all linked translation units agree on a 48 B Node, 80 B ComputedStyle, 88 B
SharedStyleRecord and 64 B NodeRareData. These style and rare-record sizes belong
to the pedal's richer feature set; the balls app's narrower sizes are not promised
for every application. Shared styles are enabled in the pedal manifest.

Compile inputs contain no Geastack checkout paths, and sdkconfig retains the
required 32 KiB S3 data cache. The pedal's lint/type/format checks, host tests,
browser build and WASM build pass. Targeted release checks pass: 714 analyzer
cases, 58 CLI cases, 49 frame-gate cases, eight target checks and the analyzer
architecture ratchet. The package resolver regression also passes.

Authenticated maintenance discovery received no pedal response, so this release
qualification does not supply a fresh all-effects audio deadline measurement.
The display benchmark is not an audio timing measurement.

The subsequent firmware rebuild after clean registry `npm ci` also passes and
is byte-identical to the qualified pedal candidate: 2,958,864 B, SHA-256
`b8989227939c4070b512fde081eecb9d9ef05c89669b1050562c064bdea12d52`.
Its linked-layout census agrees with the sizes above. Registry-installed browser
and WASM rebuilds pass as well. Final provenance and inputs are retained as
`release-registry-pedal-*`; the firmware for deployment is `build/pedalboard.bin`.
