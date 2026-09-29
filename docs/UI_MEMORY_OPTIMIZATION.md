# UI memory optimization measurements

## Latest experiment: compact CSS snapshots

On top of the rule-plan batch, style snapshots drop wide color copies, unused
length caches and excess class-token capacity, and pseudo-element syncing reuses
the node's rule plan instead of building a second one.

| Record                      | Rule-plan candidate | Snapshot candidate |
| --------------------------- | ------------------: | -----------------: |
| `ParentStyleSnapshot`       |                36 B |               20 B |
| `NodeClassSnapshot`         |                24 B |               12 B |
| `CustomPropertyFingerprint` |                24 B |               12 B |
| `CustomPropertySnapshot`    |               112 B |               64 B |
| Firmware                    |         2,982,768 B |        2,982,544 B |

A new overflow fixture (six inherited variables, twelve class tokens, thirty
theme switches) first failed on the old engine as well: an inline whole-value
`var()` that names a static custom color resolved to nothing, because that
entry stores only native colors. Inline color `var()` now reads it the way a rule
does. Inline `var()` is still resolved once, when set, and does not follow later
ancestor changes; the pedal UI uses no inline `var()`. The native batch (pedal and
full records, pixel diffs, pseudo-element, first-line, animation, census and the
fixture) passes.

On the board, every window has zero DSP deadline misses, USB transfer errors and
watchdog events, and the worst core-0 block stays near 300,000 cycles against
the control's ~316,000. Padded / trimmed frames:

| Window           | Qualified control | Rule-plan candidate | Snapshot candidate |
| ---------------- | ----------------: | ------------------: | -----------------: |
| 20-second UI (3) |         0 / 65–66 |              0 / 79 |         29–33 / 80 |
| 90-second UI     |          99 / 145 |           241 / 320 |          298 / 317 |
| 45-second steady |             9 / 0 |               0 / 0 |             45 / 0 |

Heartbeats place the padding at audio restarts and in the interval after them;
steady playback pads 0–4 frames per five seconds in both builds. The UI
difference therefore belongs to the earlier batch, not to the snapshots, and
remains unresolved. Candidate `candidate-snapshot-batch.bin` has SHA-256
`7a044c1db9c5899146af80c5b479dc09ee8763cd5087ef3ddadbff684e355676`. Logs use the `snapshot-batch` prefix. The qualified
`candidate-animation-pruned` is restored in maintenance mode; nothing is
published.

## Previous experiment: smaller rule plans and direct style resets

The batch retains the direct 144-byte node and removes generated pseudo-element
support automatically when analysis proves it unused. Rule plans now reserve only
the enabled main, pseudo-element, first-line and animation buckets. CSS resets
copy an immutable default style, without constructing a temporary whole node.
No pointer indirection or bitfield decoding is added to normal field reads.

| Metric                               | Qualified 148-byte control |     Combined candidate |                Change |
| ------------------------------------ | -------------------------: | ---------------------: | --------------------: |
| Node                                 |                      148 B |                  144 B |                  −4 B |
| Fixed TreeState, 160 slots           |                   27,256 B |               26,616 B |                −640 B |
| Temporary active-rule plan           |                    1,012 B |                  228 B |                −784 B |
| Cached rule-plan entry               |                      612 B |                  598 B |                 −14 B |
| S3 style-recomputation stack frame   |                    2,320 B |                  592 B |              −1,728 B |
| S3 style-reset stack frame           |                      192 B |                   48 B |                −144 B |
| Firmware                             |                2,986,544 B |            2,982,768 B |              −3,776 B |
| Matched startup median free PSRAM    |                4,147,172 B |            4,214,168 B |             +66,996 B |
| Internal free / largest block        |          94,322 / 31,744 B |      94,334 / 31,744 B |             +12 / 0 B |
| Median first paint, three starts     |                 3.895987 s |             3.855802 s |   −40.185 ms / −1.03% |
| Worst core-0 cycles, those starts    |                    318,182 |                302,341 |        −15,841 cycles |
| Steady audio means, last six samples |     1,152.50 / 1,193.67 µs | 1,153.17 / 1,192.83 µs | Effectively unchanged |

The memory gain includes **65,536 B freed by crossing a firmware mapping page
boundary**; the remaining observed heap difference is 1,460 B. Static PSRAM BSS
falls 16 B. These figures overlap and must not be added together. The matched host
allocator census reports no additional allocated-byte savings across twelve
phases, despite the smaller records. Stack-frame reductions are not allocated
heap savings because task stack reservations have not changed.

All three short starts, a 90-second UI/audio run and a 45-second fixed audio run
have zero DSP deadline misses, USB transfer errors and watchdog events. The
90-second run has 50 taps, 40 mask changes and 33 painted tap responses, with
241 padded / 320 trimmed audio frames. The saved qualified control had 100 / 146,
and the fresh control repeat reproduced 99 / 145. The candidate is therefore
**not promoted**: the USB stress regression remains unresolved.
Steady audio has zero padding and trimming; the fresh control has 9 / 0 and
stage means of 1,152.50 / 1,193.67 µs. The fresh control's 90-second first paint
is 3.843928 s versus the candidate's 3.867644 s, so the short-run median alone
is not evidence of a repeatable startup speedup. Maintenance renders the expected
15 nodes and 35,699 non-black pixels.

All 22 geometry/pixel frames and twelve host phase hashes match. Copy/move/reset
coverage preserves neighboring fields. Positive pseudo-element, first-line and
animation tests pass independently of the other buckets. Analyzer 505, CLI 27,
pedal check/test and the clean firmware build pass. Default-style initialization
also compiles with inline rare styles for RGB565, RGBA8888 and grayscale targets.
Ten linked record layouts agree. The earlier fresh-control repeat itself reached
318,182 cycles, showing that the prior alpha experiment's timing difference was
not enough to isolate a cause; its original observations remain recorded below.

Candidate `candidate-reset-plan.bin` has SHA-256
`909274b555487207024b50a7279bb2f5017d02fe4c96e3d3017e88b94399330a`.
Raw logs, matched statuses, source/archive hashes, target layouts and reports are
under `build/css-feature-audit/` with the `reset-plan` prefix. The saved qualified
`candidate-animation-pruned` is restored on the board in maintenance mode. No package
is published or pushed. The lockfile has no local references or installed Gea
symlinks; this experimental firmware uses explicit development overrides.
Measurements are at 15 fps, with input near the noise floor, and do not include
a hardware audio null test. Registry-only qualification and the ~50-byte goal
remain open.

## Previous experiment: 144-byte nodes, timing regressions unresolved

The automatic alpha proof removes text/border alpha storage only when all reachable
colors stay opaque. It follows local store fields, getters and finite palettes;
unknown receivers/writes, escaped objects, reflection, native controls and older
analysis retain the fields. Direct style reads remain inline. Tag and node-kind
members reuse record tail padding, with style kept at offset zero.

| Metric                                          | Qualified 148-byte control | 144-byte style-first candidate |                        Change |
| ----------------------------------------------- | -------------------------: | -----------------------------: | ----------------------------: |
| Node                                            |                      148 B |                          144 B |                 −4 B / −2.70% |
| Fixed-capacity TreeState, 160 slots             |                   27,256 B |                       26,616 B |                        −640 B |
| Firmware                                        |                2,986,544 B |                    2,985,440 B |                      −1,104 B |
| Free PSRAM, matched startup medians             |                4,147,180 B |                    4,148,324 B |                      +1,144 B |
| Internal free / largest block                   |          94,322 / 31,744 B |              94,322 / 31,744 B |                     Unchanged |
| Median first paint, three starts                |                 3.862903 s |                     3.899955 s |       **+37.052 ms / +0.96%** |
| Fresh control / candidate maximum core-0 cycles |                    314,903 |                        318,446 | **+3,543 cycles / +14.76 µs** |
| Fixed audio means, last six samples             |     1,153.50 / 1,193.67 µs |         1,152.50 / 1,193.50 µs |         Effectively unchanged |

This is a verified storage reduction with a measured timing tradeoff, not an
accepted replacement for the qualified image. At 240 MHz the worst core-0 stage
is 1,326.86 µs against the 1,333.33 µs deadline, leaving 6.48 µs of observed
headroom; the fresh short control runs leave 21.24 µs. All three starts, the
90-second UI/audio run and the 45-second steady run have zero DSP misses, USB
errors and watchdog events. The steady means compare with the saved 45-second
window of the same control image; they are not a demonstrated speedup. The
previous qualified `candidate-animation-pruned` remains the accepted build.

Heap comparisons use status taken after the same three 20-second startup windows
for each image. Paired free-PSRAM deltas are 1,144, 1,128 and 1,160 B. The earlier
pre/post five-sample comparison showed +1,312 B but used different boot histories;
those raw samples remain in the report and are not the primary memory claim.
The host allocator reports unchanged total bytes and allocation counts across
all twelve matched phases despite the smaller tree request. Do not add sizeof
savings to measured free-heap deltas. Static SRAM/PSRAM sections and flash mapping
reservation remain unchanged.

All 22 rendered frames and twelve host phase hashes match. Copy/move/reset tests
preserve adjacent members when tail padding is reused. Independent text/border
alpha variants retain transparency. The S3 probes show unchanged instruction
sequences for normal field reads, eight grouped fields and passing the style
record to a helper; alpha becomes a constant and node-kind reads lose a sign
extension. All ten linked layouts agree. The analyzer passes 491 cases, CLI 26,
and pedal checks/tests plus prepared-audio bit equivalence pass. The clean build
passes. No hardware audio null test or 60 fps qualification was performed; the
board runs at **15 fps**, with input near the noise floor.

The UI run has 50 taps, 39 mask changes and 32 painted tap responses, with
112 padding / 156 trimmed frames; steady audio has 6 / 0. Maintenance paints
15 nodes and 35,699 non-black pixels. Earlier text-first candidates also passed
functional/deadline checks but regressed first paint by 1.89% and 4.90%; their
images and results remain as diagnostic artifacts. Neither was promoted.

Candidate: `build/css-feature-audit/candidate-alpha-front.bin`, SHA-256
`65d5bef3bc86af5fca923943ccd991631170ac1643664847b9b0351a229fcd8c`.
Measurements, source audit, linked-layout audit, matched allocation census and
field probes are saved alongside it with the `candidate-alpha-front` and
`alpha-front` prefixes. The source changes remain experimental. No package was
published or pushed; the lockfile and installed packages have no local links,
while the experimental build uses explicit development overrides. Registry-only
qualification and the approximately 50-byte goal remain open.

## Current checkpoint: compact supporting records and unused-animation removal

The combined build retains directly embedded style fields, compacts supporting
records, and automatically removes CSS/declarative animation support when the
source graph proves it unused. Application `requestAnimationFrame` callbacks
remain intact. Unknown styles, native UI access, reflection and animation aliases
retain support; there are no application opt-in flags.

| Metric                                       | Saved class-capacity control |         Combined build |                Change |
| -------------------------------------------- | ---------------------------: | ---------------------: | --------------------: |
| Node / computed style                        |                   148 / 80 B |             148 / 80 B |             Unchanged |
| Optional node data                           |                         68 B |                   64 B |                  −4 B |
| CSS rule / keyframe rule                     |                    32 / 16 B |              24 / 12 B |             −8 / −4 B |
| Selector plan                                |                        240 B |                  216 B |                 −24 B |
| Fixed TreeState, 160 slots                   |                     27,256 B |               27,256 B |             Unchanged |
| Firmware                                     |                  3,031,360 B |            2,986,544 B |         **−44,816 B** |
| Static PSRAM BSS                             |                    212,232 B |              210,616 B |              −1,616 B |
| Maintenance free PSRAM, five-sample median   |                  4,133,932 B |            4,147,180 B |         **+13,248 B** |
| Free internal SRAM / largest block           |            94,322 / 31,744 B |      94,334 / 31,744 B |             +12 / 0 B |
| Median first paint, three matched short runs |                   3.879942 s |             3.833312 s |   −46.630 ms / −1.20% |
| Fixed audio stage means, last six samples    |       1,153.50 / 1,193.50 µs | 1,153.50 / 1,193.67 µs | Effectively unchanged |

The first-paint control is the fresh three-run repeat performed after the failed
record-only experiment below. The audio means use the saved 45-second control
window. Heap samples were freshly collected from each image in maintenance;
internal sections and flash mapping are unchanged. The 12-byte internal-heap
variation is not a static SRAM saving. The 13,248-byte free-PSRAM delta already
includes static reservation and allocator effects: do not add the record or
static savings to it.

Three 20-second starts, a 90-second UI/audio soak and a 45-second fixed-load
window all pass with zero DSP deadline misses, USB errors and watchdog firings.
Short-run first paints are 3.833312, 3.824128 and 3.858847 seconds, all initialized
on core 1. The long UI run first paints at 3.880240 seconds; 50 taps produce
40 mask changes and 36 painted responses. Its maxima are 314,906 / 292,115
cycles, padding/trimmed 100 / 146. Fixed-load maxima are 312,764 / 291,956
cycles, padding/trimmed 8 / 0. The earlier record-only image remains failed;
passing this combined image does not retroactively qualify it.

The analyzer passes 438 cases and the CLI 25. Frame callbacks, including callbacks
queued for the next frame, match with animations enabled and removed. Enabled
CSS animations still pass priming and keyframe tests. All 22 saved pixel/geometry
frames and twelve allocation-fixture phase hashes match; selector spill,
reallocation, rare-record copying and reuse pass. All ten linked layouts agree.
The linked firmware has no CSS animation engine, parser, track builder or active
list, while its frame-callback queue remains. Pedal checks, tests, host prepared
audio equivalence and the clean S3 build pass. Maintenance still paints 15 nodes
and 35,699 non-black pixels.

The matched host allocator fixture saves 528 B in the populated phase against
the control (1,107,888 → 1,107,360 B); 64 B of that belongs to animation pruning.
Empty-tree allocation is unchanged, as are allocation counts. Initial independently
launched runs had different process baselines and are retained as raw artifacts;
matched launches and three repeats resolve that comparison. Host totals are
separate from S3 heap measurements.

This qualification is at the configured **15 fps**, not 60 fps. Input is near
the noise floor, and no hardware sample-by-sample audio null test was performed.
There is no established steady DSP speedup. The approximately 50-byte Node goal
remains open: Node itself is still 148 B. No extra normal style-field pointer
loads or packed-field decoding were introduced.

Image: `build/css-feature-audit/candidate-animation-pruned.bin`, SHA-256
`8635379bb9ec9c5d22ef963fb158da76993e3e22824badc8f54564276b359b16`.
The ELF, engine archive, source audit, linked-layout audit and measurements use
the same prefix; hardware logs and matched allocation reports use `animation-*`.
Post-build analyzer hardening emits exactly the same pedal flags as the build.
The board is left on this image in maintenance mode. Nothing was published or
pushed. Development inputs use explicit upstream overrides; installed packages
and the lockfile remain registry-only. Final registry-only qualification is pending.

## Supporting-record candidate: timing failure, not qualified

The next batch keeps the 148-byte Node and 80-byte style unchanged. It compacts
optional node data 68 → 64 B, CSS rules 32 → 26 B, keyframe rules 16 → 12 B,
and selector plans 240 → 216 B. There are no new style pointers or accessors.
Sampled direct field reads retain one load; this does not establish identical
array-index arithmetic or whole-engine execution cost.

Matched warm maintenance samples show 7,776 B more free PSRAM (4,133,940 →
4,141,716 B), unchanged internal SRAM and largest block. The host allocation
fixture independently saves 464 B when populated, with all twelve plain/rounded
phase hashes unchanged. Firmware grows 1,440 B to 3,032,800 B. Static sections
and the flash mapping reservation are unchanged. Do not add these measurements.

This image is **not timing-qualified**: two of three 20-second startup windows
each missed a core-0 deadline, reaching 320,304 and 320,600 cycles against a
320,000-cycle budget. The 90-second UI soak and 45-second fixed load had no
misses; all windows had zero USB errors and watchdog firings. First-paint median
was 3.946673 s versus 3.882804 s for the preceding control (+1.64%). After OTA
back to that saved control, three matched repeats also passed with zero misses
and median first paint 3.879942 s. The smaller records therefore remain an
unqualified experiment, not the current accepted checkpoint.

The long candidate UI soak painted first at 3.928059 s, with 50 taps, 40 mask
changes and 36 painted responses. Maxima were 318,753 / 292,129 cycles,
padding/trimmed 112 / 159. Fixed-load maxima were 319,733 / 292,300 cycles,
padding/trimmed 6 / 0; mean last-six stage times were 1,154.33 / 1,193.33 µs.
This is a RAM saving with unresolved timing cost, not a speed improvement.
All measurements use 15 fps, not 60 fps; there is no hardware audio null test.

Record copy/move/reuse and selector spill/reallocation tests pass against the
saved control and candidate; enabled CSS animation priming passes. The 22 pixel
frames match, all ten linked record layouts agree, and pedal checks, tests,
prepared-audio equivalence and clean S3 build pass. Maintenance rendering is
still 15 nodes and 35,699 non-black pixels.

Artifacts: `candidate-record-padding.bin` (SHA-256
`4ac0b9bb840a77290e9d490ad5b3b7df5e076d9d441cf93cf9b1a8d95d64f93f`), its
ELF, engine archive and source audit; `candidate-record-padding-measurements.json`
and `record-padding-*` logs under `build/css-feature-audit`. The saved
class-capacity image was restored for control repeats. The combined follow-up
above now supersedes this failed experiment. No publication or push occurred.

## Previous checkpoint: automatic class-list capacity

The build analyzer proves the maximum number of class tokens from the app's
source graph. Literals, conditional class maps and numeric template substitutions
are supported; unknown mutations, imports, assertions and native constructors
retain four inline slots. The CLI generates the capacity automatically and
ignores manual overrides. Overflow, copying, moving and reuse remain supported.
This analysis runs at build time. Normal style fields stay directly embedded.

| Metric                                     | Preceding core-1 build |          Current build |                Change |
| ------------------------------------------ | ---------------------: | ---------------------: | --------------------: |
| Node / computed style                      |             148 / 80 B |             148 / 80 B |             Unchanged |
| Class-list record                          |                   12 B |                    8 B |                  −4 B |
| Fixed TreeState, 160 slots                 |               27,896 B |               27,256 B |                −640 B |
| Firmware                                   |            3,031,824 B |            3,031,360 B |                −464 B |
| Maintenance free PSRAM, five-sample median |            4,132,856 B |            4,134,104 B |     +1,248 B observed |
| Free internal SRAM / largest block         |      94,334 / 31,744 B |      94,322 / 31,744 B |             −12 / 0 B |
| Median first paint, three short runs       |             3.951960 s |             3.882804 s |   −69.156 ms / −1.75% |
| Fixed audio stage means, last six samples  | 1,153.17 / 1,193.50 µs | 1,153.50 / 1,193.50 µs | Effectively unchanged |

Only 640 bytes of the observed free-PSRAM increase are directly accounted for by
fixed tree storage. The other 608 bytes remain unattributed heap variation.
Static internal/PSRAM sections and the flash mapping reservation are unchanged;
the 12-byte internal variation is not a static cost. Do not add the tree saving
to the heap delta. The before samples were freshly collected from the preceding
core-1 image, after its saved 90-second UI/audio and 45-second fixed workloads;
the candidate samples follow the same workloads.

All three 20-second startup windows, the 90-second UI/audio soak and the
45-second fixed load recorded zero DSP deadline misses, USB errors and watchdog
firings. Startup took 3.903950, 3.882804 and 3.876114 seconds. The long UI run
first painted in 3.895938 seconds, with 50 taps, 41 mask changes and 33 painted
responses. Its core maxima were 316,552 / 292,096 cycles; padding/trimmed frames
were 96 / 142. The fixed-load maxima were 316,718 / 292,411 cycles, with 9 / 0
padding/trimmed frames. All initializations completed on core 1. The board remains
configured for **15 fps**; these results do not qualify 60 fps. Input was near the
noise floor, and no hardware sample-by-sample audio null test was performed.

The analyzer passes 405 tests and the CLI passes 24. Native class storage tests
pass at capacities one through four, including overflow and independent copies.
All 22 saved geometry/pixel frames match, and all ten linked record layouts agree.
S3 class-read assembly retains the same instruction sequence; offsets and the
capacity constant change, with no additional loads. Ordinary Node fields and
their offsets are unchanged. Pedal checks, tests, prepared-audio bit equivalence
and the clean S3 build pass. Maintenance still renders 15 nodes and 35,699
non-black pixels. The merged-native fallback was tightened after the build;
all generated pedal flags were verified identical and the CLI tests rerun.

Image: `build/css-feature-audit/candidate-class-capacity.bin`, SHA-256
`7fe32a51b3fdb4fa985f7b4cba227b6a0d38bbb3e5107bcf43196328532dc4f5`.
The engine archive, source hashes, linked-layout audit and measurements are in
`candidate-class-capacity-*`; hardware logs are `class-capacity-*`.
The board is left in maintenance mode. No package was published or code pushed.
This is a development build using explicit upstream overrides; the lockfile and
installed packages remain registry-only. The final registry-only build and the
approximately 50-byte node goal are still outstanding.

## Previous checkpoint: compact positions and deterministic initialization

The current development build uses a **148-byte Node**, with directly embedded
style fields and no additional style-pointer load. Automatic analysis removes
four unused position values and the transform payload of cached style operations.
When floats are unused, layout also omits two descendant scans that cannot find
any floats. Mixed inline/block layout and height clamping remain supported.

| Metric                                     | Saved CSS-pool control |          Current build |                  Change |
| ------------------------------------------ | ---------------------: | ---------------------: | ----------------------: |
| Node / computed style                      |             156 / 88 B |             148 / 80 B |               −8 / −8 B |
| Fixed TreeState, 160 slots                 |               29,176 B |               27,896 B |                −1,280 B |
| Cached style operation                     |                   48 B |                   24 B |                   −24 B |
| Static PSRAM BSS                           |              212,616 B |              212,232 B |                  −384 B |
| Firmware                                   |            3,034,464 B |            3,031,824 B |                −2,640 B |
| Maintenance free PSRAM, five-sample median |            4,125,288 B |            4,133,020 B |            **+7,732 B** |
| Free internal SRAM / largest block         |      94,322 / 31,744 B |      94,334 / 31,744 B |               +12 / 0 B |
| Median first paint, three short runs       |             3.886914 s |             3.951960 s | **+65.046 ms / +1.67%** |
| Fixed audio stage means, last six samples  | 1,152.83 / 1,193.17 µs | 1,153.17 / 1,193.50 µs |   Effectively unchanged |

Heap values follow matched 90-second UI/audio and 45-second fixed-load workloads.
Internal sections and flash mapping are unchanged; the 12-byte internal heap
variation is not a claimed static saving. Do not add tree/static savings to the
measured heap delta. The host allocation census from the preceding position
experiment remains separate from these device measurements.

The startup investigation found an uncontrolled scheduling effect. In one
unchanged diagnostic image, initialization began without affinity and ended
pinned to core 0 in four runs: first paint took 4.984396–5.018937 s. A fifth run
ended on core 1 and first painted at 3.974782 s. ESP-IDF implicitly pins tasks
that use floating point. Core 0 also handles USB and audio stage A.
The target now accepts `GEA_EMBEDDED_GEA_INIT_TASK_CORE`, defaulting to the same
unpinned behavior as before; this pedal selects core 1. Both PSRAM and internal
stack creation paths honor that selection, with a compile-time valid-core check.
Existing performance logs now report executing CPU and affinity.

With explicit core-1 initialization, three short runs first painted at 3.951960,
3.958865 and 3.943967 s; the 90-second soak took 4.025445 s. Every initialization
completed on core 1. The short-run median is 20.83% below the traced core-0
median, but **1.67% above the older saved control**. No startup equivalence,
steady DSP speedup or 60 fps qualification is claimed. This addresses the large
CPU-placement variation; it does not prove that layout/code-generation effects
contribute nothing to the smaller remaining difference.

All three 20-second runs, the 90-second UI/audio soak and the 45-second fixed
load recorded zero DSP misses, USB errors and watchdog events. The long UI soak
recorded 50 taps, 40 mask changes and 32 painted responses; padding/trimmed
frames were 116/146. Its maximum core cycles were 301,325/292,406. The fixed
window recorded 9/0 padding/trimmed frames and maxima of 315,846/292,729 cycles.
The board is configured for 15 fps, and is left in maintenance mode. Input is
near the noise floor; no hardware sample-by-sample audio null test was performed.

All nine linked UI layouts agree. Eighteen new nested mixed-flow frames match
both the preceding traversal and float-enabled engine. The 22 pedal pixel frames
match the saved control; all 84 nested memo-work cases retain 4,544 layout calls.
The prior 130 full position cases and partial-storage checks remain relevant:
the position representation is unchanged in this follow-up. Pedal checks,
formatting, tests, host prepared-audio bit equivalence and the clean S3 build pass.
Maintenance rendering matches 15 nodes and 35,699 non-black pixels.

Image: `build/css-feature-audit/candidate-init-core1.bin`, SHA-256
`d807ad2ce1b9d06a5e99787782a5cd37492edf91fd75135e44b4ddf559453320`.
The engine archive is `candidate-float-walk-engine.tar.gz`; source hashes and
hardware results are in `candidate-init-core1-*.json` and `init-core1-*` logs.
The target's initialization code/configuration now joins its position diagnostic
compatibility change in the eventual dependency update. Unrelated target/audio
and JPEG work remains excluded. No packages were published or changes pushed.
The lockfile and installed packages remain registry-only; this qualification
uses explicit development inputs. Final registry-only qualification and the
approximately 50-byte goal remain outstanding.

## Historical qualified fallback: cache payload only

The qualified development checkpoint retains a **156-byte Node** and direct,
embedded style fields. The 148-byte position-storage experiment is not qualified:
a matched three-run comparison reproduced a 25.96% median first-paint regression.
The cache-only diagnostic removes that regression while retaining most of the
associated-allocation saving. No package has been published.

| Metric                                     | Saved CSS pool control |       Cache-only build |                Change |
| ------------------------------------------ | ---------------------: | ---------------------: | --------------------: |
| Node / computed style                      |             156 / 88 B |             156 / 88 B |                     — |
| Fixed TreeState, 160 slots                 |               29,176 B |               29,176 B |                     — |
| Cached style operation                     |                   48 B |                   24 B |                 −24 B |
| Static PSRAM BSS                           |              212,616 B |              212,232 B |                −384 B |
| Firmware                                   |            3,034,464 B |            3,033,408 B |              −1,056 B |
| Maintenance free PSRAM, five-sample median |            4,125,288 B |            4,132,064 B |          **+6,776 B** |
| Free internal SRAM / largest block         |      94,322 / 31,744 B |      94,322 / 31,744 B |                     — |
| Median first paint, three 20-second runs   |             3.886914 s |             3.874752 s | Effectively unchanged |
| Fixed audio stage means, last six samples  | 1,152.83 / 1,193.17 µs | 1,153.17 / 1,193.33 µs | Effectively unchanged |

All three short runs, the 90-second UI/audio soak and 45-second fixed audio
window recorded zero DSP deadline misses, USB transfer errors and watchdog events.
The long UI soak painted first at 3.835929 s, with 50 taps, 40 mask changes and
34 painted responses; padding/trimmed frames were 96/142. The fixed window
recorded 6/0 padding/trimmed frames. Its maximum core cycles were 312,327/292,180;
the UI soak's were 297,904/292,261. These measurements use the 600 × 450 board
configured for 15 fps; they are not 60 fps qualification or a hardware audio
sample null test. Audio input remains near the noise floor.

The cache omits the eleven transform values only when automatic analysis proves
transforms unused. It adds no style pointer load or packed-field decoding.
All nine linked UI layouts agree, and 22 native geometry/pixel frames match the
control. Cached declaration and color records remain 72 B and 6 B. Internal
sections and flash mapping are unchanged. Do not add static savings to the
measured heap saving. The prior host fixture's 4,096-byte saving belongs to the
unqualified position experiment, not this cache-only build.

Image: `build/css-feature-audit/candidate-edge-cache-only.bin`, SHA-256
`ce3bd4abe941a5eeed47d969e3f7a1a2d26a214dcc9986da858d03bed39bc4bd`.
The saved engine archive and `edge-cache-only-*` logs preserve this exact
checkpoint. The local upstream engine still carries the unqualified position
experiment; it must be repaired or retired before release. Diagnostic sources
use explicit development overrides; installed packages and the lockfile remain
registry-only. Final registry-only qualification and the ~50-byte goal are open.

## Float-scan follow-up: startup still unqualified

Automatic float pruning now removes two unnecessary descendant scans while
retaining mixed inline/block flow and final height clamping. The 148-byte image
passes 18 new nested mixed-flow frames against the original traversal and
float-enabled engine, 22 pedal pixel frames, and all 84 memo-work cases.
The firmware is 3,031,792 B (2,672 B below the CSS-pool control).

Three 20-second runs first painted at 3.870749, 3.886760 and 3.876250 s,
but the subsequent 90-second soak took 4.721443 s. **This build remains
unqualified for startup performance.** All five short/long audio windows
recorded zero DSP misses, USB transfer errors and watchdog events. Detailed
cycles, padding, trimming and heap samples are in
`candidate-float-walk-measurements.json` under `build/css-feature-audit`.

A 156-byte diagnostic retaining the position code but restoring old member
offsets first painted at 3.874697, 3.874716 and 3.874736 s. That implicates layout
or generated-code effects but does not identify the cause. Initialization is
created without explicit CPU affinity; ESP-IDF can automatically pin a task
when it first uses the FPU. CPU/affinity logging is being added to test this
hypothesis before any scheduling change. The saved cache-only image remains
the qualified fallback. Nothing was published.

## Unqualified position-storage experiment

**Follow-up:** A matched three-run comparison reproduced slower startup: median
first paint was 3.886914 s for the saved CSS pool control and 4.896094 s for
this candidate (+25.96%). All six audio windows had zero DSP misses, USB errors
and watchdog events. The position-storage batch is **not performance-qualified**.
The cache-only diagnostic restored control startup timing; see
`build/css-feature-audit/edge-startup-ab.json` for the complete comparison.

Measured on 2026-09-29 on the 600 × 450 ESP32-S3 AMOLED 2.41, configured for
15 fps. Hot style fields remain embedded and directly loaded. Automatic source
analysis removes this app's unused bottom pixel offset and top/right/bottom
percentage offsets. Unknown styles, logical insets, opaque native use, inputs
and lists retain the support they require. No app opt-in was added.

| Metric                                 | Previous CSS pool build | Current position-storage build |       Change |
| -------------------------------------- | ----------------------: | -----------------------------: | -----------: |
| Node                                   |                   156 B |                          148 B |         −8 B |
| ComputedStyle                          |                    88 B |                           80 B |         −8 B |
| Fixed TreeState, 160 slots             |                29,176 B |                       27,896 B |     −1,280 B |
| Cached style operation                 |                    48 B |                           24 B |        −24 B |
| Static PSRAM BSS                       |               212,616 B |                      212,232 B |       −384 B |
| Firmware                               |             3,034,464 B |                    3,031,488 B |     −2,976 B |
| Maintenance free PSRAM, median of five |             4,125,288 B |                    4,133,024 B | **+7,736 B** |
| Maintenance free internal SRAM         |                94,322 B |                       94,322 B |            — |
| Largest free internal block            |                31,744 B |                       31,744 B |            — |

Heap samples compare maintenance after the same 90-second UI/audio and
45-second fixed-load workloads. Candidate boot samples are saved separately.
Flash mapping and internal sections are unchanged. The heap improvement already
includes the static and tree savings; do not add the rows together. The separate
host allocation fixture saves 4,096 B in all six phases with identical pixel
hashes; host and device savings must not be added together.

The unused transform payload disappears from cached style operations. Retained
transform builds keep it and pass mixed static/parsed transform replay. Every
retained position read still compiles to one S3 `l16si`; 24 existing field probes
retain their instruction sequences, with updated offsets where necessary.
Missing edges become compile-time constants, without a runtime edge lookup,
style pointer, packed decoding or per-read range check.

All nine linked UI layouts agree. All 130 full position cases match the saved
engine. Four partial-storage configurations and 66 exact-pedal cases pass,
as do 22 geometry/pixel frames, 26 repaint cases and the full CSS cascade.
Nested layout remains at exactly 4,544 calls across 84 cases. Analyzer tests
pass 366/366 and CLI feature tests 23/23. Pedal checks, tests, host prepared-audio
bit equivalence and the clean S3 build pass. Maintenance rendering matches
15 nodes and 35,699 non-black pixels.

| Hardware check                                       | Previous CSS pool build |                               Current build |
| ---------------------------------------------------- | ----------------------: | ------------------------------------------: |
| Fixed audio load                                     |     45 s, amp + mask 63 |                         45 s, amp + mask 63 |
| Mean last-six heartbeat stage times                  |  1,152.83 / 1,193.17 µs |                      1,154.00 / 1,192.33 µs |
| Fixed-load maximum core 0 / core 1 cycles            |       314,489 / 292,402 |                           301,924 / 292,061 |
| Fixed-load DSP misses / USB errors / watchdog events |               0 / 0 / 0 |                                   0 / 0 / 0 |
| Fixed-load padding / trimmed frames                  |                   9 / 0 |                                      22 / 0 |
| UI/audio soak                                        |                    90 s |                         90 s, repeated once |
| UI maximum core 0 / core 1 cycles                    |       314,269 / 292,252 | 299,209 / 292,207; repeat 300,749 / 292,703 |
| UI DSP misses / USB errors / watchdog events         |               0 / 0 / 0 |                      0 / 0 / 0 in both runs |
| UI first paint                                       |            3,879,993 µs |           4,897,475 µs; repeat 4,033,448 µs |
| UI padding / trimmed frames                          |                 84 / 79 |                 130 / 160; repeat 129 / 160 |
| UI taps / mask changes / painted responses           |            50 / 40 / 36 |           50 / 39 / 29; repeat 50 / 38 / 32 |

Fixed-load stage times are effectively unchanged. UI first paint varies and
both new samples exceed the saved baseline. Startup latency remains an open
comparison concern: no UI speedup or startup equivalence is claimed. These
soaks change the DSP load through UI taps and are not a 60 fps benchmark.
Earlier intermittent startup deadline misses also remain unresolved by these
passing windows. Input is near the noise floor; no hardware sample-by-sample
audio null test was performed. The board is left in maintenance mode.

The target compiler initially rejected a redundant comparison between disabled
transform arrays; it now compiles out with that payload. Five upstream target
diagnostic reads were updated for the selected edges, retaining compatibility
with older engines. The target package must accompany a later registry release.
Only that diagnostic file differs among the target sources compiled here;
unrelated ES8311 and other-board changes were excluded from this build.

Image: `build/css-feature-audit/candidate-edge-storage.bin`, SHA-256
`47c07ec6638fdb0fab55c2263ffe60fad69646f2b586e79c063a4dfb397bf4cb`.
Raw data, source hashes, assembly, saved engine sources and logs use
`edge-storage-*` and `candidate-edge-storage-*`. No packages were published or
changes committed/pushed. The lockfile remains registry-only and installed Gea
packages are not local symlinks; this unpublished measurement uses explicit
engine, element, plugin, CLI and target development inputs. Final registry-only
qualification and the approximately 50-byte node goal remain outstanding.

## Historical CSS pool checkpoint

Measured on 2026-09-29 on the 600 × 450 S3 AMOLED 2.41 board, configured for
15 fps. The current candidate retains the **156-byte Node** and embedded style
fields. This batch reduces associated CSS allocations, without a new field
accessor, pointer load, ownership scheme, or app opt-in.

| Metric                                 | Previous repaint build | Current CSS pool build |        Change |
| -------------------------------------- | ---------------------: | ---------------------: | ------------: |
| Node                                   |                  156 B |                  156 B |             — |
| Fixed TreeState, 160 slots             |               29,176 B |               29,176 B |             — |
| CSS variable entry                     |                   24 B |                   20 B |          −4 B |
| Compiled CSS declaration               |                   84 B |                   72 B |         −12 B |
| Parsed-color cache entry               |                   12 B |                    6 B |          −6 B |
| Firmware                               |            3,034,432 B |            3,034,464 B |         +32 B |
| Maintenance free PSRAM, median of five |            4,113,004 B |            4,125,492 B | **+12,488 B** |
| Maintenance free internal SRAM         |               94,322 B |               94,322 B |             — |
| Largest free internal block            |               31,744 B |               31,744 B |             — |

These are fresh matched heap samples, not differences between historical
samples from separate runs. Firmware mapping, static PSRAM BSS and internal
sections are unchanged. The memory improvement is associated allocation
storage; it is not a further reduction in Node or fixed tree size.

Packed-color caches now use 16-bit unsigned fields on RGB565/gray targets.
RGBA/ARGB retain 32-bit signed carriers, including their high bits. Both cached
colors remain separate because the style-input and panel-native byte orders
can differ. Transform scale values now occupy previously unused scalar slots
in compiled declarations. Four-property groups use the existing auxiliary byte
for their count, so the scalar array shrinks from eleven to eight words.
No parser capability or numeric range is removed.

All 22 existing S3 field probes retain identical instructions. Each narrowed
color-cache probe uses one `l16ui`. Full and exact-pedal native configurations
match all 22 saved geometry/pixel frames. Four-property group replay, mixed
static/parsed 3D transforms and the CSS cascade suite pass. The host allocation
fixture saves 256 B in its populated UI phase and 2,048 B when all 32 copied
variable maps are independently modified; every value/pixel fingerprint
matches. Host savings must not be added to device heap savings.

The shared-map experiment was removed: its independently modified case used
512 B more host memory. Neither rejected custom allocator proposal was applied.
The retained stores remain ordinary vectors with the preceding string ownership.
A new regression initially exposed an existing generic-property-group font-weight
issue; the saved control and candidate both reset 700 to 400. That behavior is
recorded separately and was not changed to obtain these storage measurements.

The native display harness cannot compile as RGBA because its display API is
hardcoded to RGB565. Independent RGBA and ARGB store tests pass high-bit and copy
checks; this is not a claim of full-color display-harness coverage. Pedal checks,
tests, prepared-audio equivalence and the clean S3 build pass. All nine linked UI
record layouts agree. The maintenance census matches 15 nodes and 35,699
non-black pixels.

| Hardware check                                       | Previous repaint build | Current CSS pool build |
| ---------------------------------------------------- | ---------------------: | ---------------------: |
| Fixed-load window                                    |    45 s, amp + mask 63 |    45 s, amp + mask 63 |
| Mean last-six heartbeat stage times                  | 1,154.00 / 1,193.50 µs | 1,152.83 / 1,193.17 µs |
| Fixed-load maximum core 0 / core 1 cycles            |      313,497 / 292,816 |      314,489 / 292,402 |
| Fixed-load DSP misses / USB errors / watchdog events |              0 / 0 / 0 |              0 / 0 / 0 |
| Fixed-load padding / trimmed frames                  |                  9 / 0 |                  9 / 0 |
| UI soak                                              |                   90 s |                   90 s |
| UI maximum core 0 / core 1 cycles                    |      312,026 / 292,201 |      314,269 / 292,252 |
| UI DSP misses / USB errors / watchdog events         |              0 / 0 / 0 |              0 / 0 / 0 |
| UI first paint                                       |           3,843,896 µs |           3,879,993 µs |
| UI padding / trimmed frames                          |               96 / 142 |                84 / 79 |

Stage means are effectively unchanged; no DSP speedup is established. The UI
soak recorded 50 taps, 40 mask changes and 36 painted responses. Its changing
load is not a fixed-load or 60 fps benchmark. Earlier intermittent startup
misses remain unresolved by these passing windows. Input is at the noise floor;
no hardware sample-by-sample null test was performed. The board is running the
CSS pool image and was left in maintenance mode.

Image: `build/css-feature-audit/candidate-css-pool.bin`, SHA-256
`137dedf70abdeab456faef9ef3b9afe6fcb909c53ef29d5b4e3680a7bec6a716`.
Raw measurements, source hashes, assembly and device logs use `css-pool-*` and
`candidate-css-pool-*` in that directory. No packages were published or changes
committed/pushed. The pedal lockfile remains registry-only; this development
measurement uses explicit overrides. Final registry-only qualification and the
approximately 50-byte node goal remain outstanding.

## Historical repaint checkpoint

The following report describes the preceding repaint candidate. Its “current”
references refer to that saved image, not the CSS pool build above.

The current unpublished development candidate uses a **156-byte Node**, with
embedded direct fields and no additional style-pointer load. The approximately
50-byte goal remains open. Measured on 2026-09-29 on the connected 600 × 450
AMOLED 2.41 target, configured for **15 fps**.

| Metric                                 | Previous 160-byte build | Current 156-byte build |       Change |
| -------------------------------------- | ----------------------: | ---------------------: | -----------: |
| Firmware                               |             3,033,232 B |            3,034,432 B | **+1,200 B** |
| Node                                   |                   160 B |                  156 B |         −4 B |
| ComputedStyle                          |                    88 B |                   88 B |            — |
| LayoutBox                              |                    38 B |                   38 B |            — |
| RenderState                            |                    14 B |                   10 B |         −4 B |
| Fixed TreeState, 160 slots             |                29,816 B |               29,176 B |       −640 B |
| Maintenance free PSRAM, median of five |             4,111,960 B |            4,113,180 B |     +1,220 B |
| Maintenance free internal SRAM         |                94,318 B |               94,322 B |         +4 B |
| Largest free internal block            |                31,744 B |               31,744 B |            — |

These heap values are matched samples immediately before and after this OTA.
The tree accounts for 640 B of the PSRAM increase; the remaining 580 B is not
attributed to node storage. Static PSRAM BSS, the firmware mapping reservation,
and internal sections remain unchanged. Against the registry engine's 400-byte
Node, the record is 61% smaller. Firmware remains 190,016 B below that registry
baseline, despite this batch's 1,200 B increase.

## What changed

Repaint scratch now shares four bytes between the original background color
and partial-text bounds. Only one shortcut may own that space. The destination
color already exists in computed style, so its duplicate was removed. Ordinary
reads remain direct aligned fields: S3 probes for width, background, original
recolor color and partial-text bounds each contain one load. There are no
accessors, packed-field decoding, shared styles or new style-pointer loads.

Mutation handling now preserves the last painted state. Consecutive text
changes union their dirty extents; mixed text/style changes use ordinary
node repainting. This also fixes existing stale pixels: the previous engine
fails 16 of 24 matched coalesced-update scenes, while the candidate passes all
24 plus two additional raw native color-write cases. Both framebuffer and
presented pixels match a clean repaint exactly. This intentional correction
means the buggy prior output is not preserved in those cases.

The layout-result compaction experiment was rejected and removed. It saved
four bytes but increased nested-flex layout calls from 4,544 to 15,228 across
84 cases, despite identical geometry. The retained implementation keeps the
memo result snapshots and matches the prior 4,544 calls and all cache hits.
Its failed diff and measurements remain saved for future audits.

The matched host allocation census still saves 96,592 B against registry in
the populated 49-node fixture. Plain totals are 1,206,224 → 1,109,632 B;
rounded totals are 1,206,256 → 1,109,664 B. The registry totals shifted too
relative to the previous run, so no additional host allocation saving is
claimed for this batch. All six pixel phases match in both fixtures.

## Hardware timing

| Measurement                                       | Previous 160-byte candidate | Current 156-byte candidate |
| ------------------------------------------------- | --------------------------: | -------------------------: |
| Fixed-load window                                 |         45 s, amp + mask 63 |        45 s, amp + mask 63 |
| Mean last-six heartbeat stage times               |      1,153.83 / 1,193.50 µs |     1,154.00 / 1,193.50 µs |
| Maximum core 0 / core 1 cycles                    |           314,735 / 292,579 |          313,497 / 292,816 |
| DSP misses / USB errors / watchdog events         |                   0 / 0 / 0 |                  0 / 0 / 0 |
| Padding / trimmed frames                          |                       8 / 0 |                      9 / 0 |
| UI soak length                                    |                        90 s |                       90 s |
| UI soak maximum core 0 / core 1 cycles            |           314,735 / 292,163 |          312,026 / 292,201 |
| UI soak DSP misses / USB errors / watchdog events |                   0 / 0 / 0 |                  0 / 0 / 0 |
| UI soak first paint                               |                3,842,701 µs |               3,843,896 µs |
| UI soak padding / trimmed frames                  |                   112 / 157 |                   96 / 142 |

The steady times are effectively unchanged; no DSP speedup is established.
The UI soak recorded 50 taps, 40 mask changes and 33 painted responses. Its
legacy coordinates change processing load and do not constitute a fixed-load
or 60 fps benchmark. Maintenance again painted 35,699 non-black pixels with
15 mounted nodes. Expensive framebuffer/task scans remain disabled in audio
mode; no audio-mode pixel census was taken.

Earlier candidates intermittently exceeded the 320,000-cycle startup budget.
One passing steady window and one passing UI window do not resolve that history
or establish reliable startup. Input remains at the noise floor. Host prepared
audio equivalence passes; no hardware sample-by-sample null test was performed.

## Validation and artifacts

Full and pruned native configurations pass 22 geometry/pixel frames, the
rounded clipped-root regression, and the 84-case layout-work comparison.
The full and exact-pedal configurations pass all 26 repaint cases; the fully
pruned configuration passed the original 24 before the two native-color cases
were added. Pedal checks/tests, the clean S3 build, and full-CSS WASM compilation
pass. All nine linked UI record layouts are consistent. The layout checker now
also inspects anonymous-union alternatives and nested scratch offsets.

The saved image is `build/css-feature-audit/candidate-repaint.bin`, SHA-256
`4a9691282c274adb459225462ef6b619c355aa7bd5539f9aabe5a3d749c52e8f`.
Measurements, source hashes, field-load assembly, native results and device
logs use `candidate-repaint-*`, `repaint-*` and `device-repaint-*` in that directory.
The board is running this image in maintenance mode. No packages were published
and nothing was committed or pushed. The lockfile contains only registry
references and installed Gea packages are not symlinks; this measurement still
uses explicit development overrides. Final registry-only qualification remains
pending completion of the optimization work.

## Historical 160-byte checkpoint and earlier measurements

The remainder preserves the preceding report. References to its “current”
build mean the saved 160-byte checkpoint, not the 156-byte candidate above.

Measured on 2026-09-29 on the connected 600 × 450 AMOLED 2.41 target.
The current development candidate uses a **160-byte Node**, with embedded direct
fields and no additional style-pointer load. It is unpublished and not
release-qualified. The approximately 50-byte goal and the earlier intermittent
startup timing failures remain open.

| Metric                              | Registry engine 0.1.6 |    Previous 164-byte build |     Current 160-byte build |
| ----------------------------------- | --------------------: | -------------------------: | -------------------------: |
| Firmware image                      |           3,224,448 B |                3,044,944 B |                3,033,232 B |
| Node                                |                 400 B |                      164 B |                      160 B |
| ComputedStyle                       |                 172 B |                       88 B |                       88 B |
| LayoutBox                           |                  80 B |                       38 B |                       38 B |
| RenderState                         |                 100 B |                       16 B |                       14 B |
| Fixed TreeState, 160 slots          |              68,272 B |                   30,464 B |                   29,816 B |
| RareStyle                           |                 336 B | No instance fields or pool | No instance fields or pool |
| Optional NodeRareData               |               1,044 B |                       68 B |                       68 B |
| One custom-property entry           |                     — |                       24 B |                       24 B |
| Authored-style override store       |                     — |                        4 B |                        4 B |
| Maintenance free PSRAM, median of 5 |           3,830,224 B |                4,108,224 B |                4,112,132 B |
| Maintenance free internal SRAM      |              93,822 B |                   94,322 B |                   94,334 B |
| Largest free internal block         |              31,744 B |                   31,744 B |                   31,744 B |

The previous-column heap values are fresh samples before the native-payload OTA.
The earlier 164-byte report measured 4,108,404 B; both datasets remain saved.
This combined batch saves 11,712 B of firmware and frees a measured 3,908 B of
PSRAM. TreeState explains 648 B and static PSRAM BSS falls 624 B. The remaining
2,636 B includes other allocations and sampling variation; it is not attributed
to exact per-node storage. Internal free heap rises 12 B, while its largest
block, internal sections and the firmware mapping reservation are unchanged.

Against registry, Node is 60% smaller and firmware saves 191,216 B. Whole-system
free PSRAM rises 281,908 B, including 196,608 B less firmware mapping reservation;
that total must not be described as dynamic UI allocation savings.

## Combined automatic storage changes

The independent `node-analysis-v1` source proof removes unused native image and
text-input support. The pedal therefore has no image handle in each node, no
focus/caret fields, and no native input keyboard implementation. Its custom
preset keyboard consists of ordinary buttons and stays intact. The no-scroll
specialization also drops the second scroll-only dirty flag and its writes.
Unknown imports, markup, dynamic/aliased factories and native mutation retain
support. These decisions require no application switches or per-field lookups.

The UI pump now limits full framebuffer and render-stack diagnostics to
maintenance mode. Its task-name lookup traverses FreeRTOS lists under the kernel
lock; the framebuffer scan adds PSRAM traffic during startup. Removing this
unnecessary audio-mode work changes neither UI behavior nor deadline accounting.

The new `css-ranges-v1` analysis proves nonnegative bounds for padding, gap,
border width/radius, font size, resolved line height and flex grow/shrink.
The CLI combines those bounds with the actual layout pixel ratio and emits
byte-storage definitions only when all values fit 0–255. Unknown/dynamic styles,
relative lengths, native writes, animations and runtime scale changes retain
wide storage. There is no application opt-in or hand-maintained property list.

Source discovery now uses TypeScript syntax trees. The old import regex could
skip a leading stylesheet when another import/export followed it, understating
the pedal's root radius as 13px instead of 65px. Regression tests cover this
case, reexports, require/import-equals and computed module boundaries. The real
65px radius remains safe after 2x scaling; every stylesheet participates in the
proof. Inheritance and unitless line-height bounds are included too.

All newly compacted fields compile to one direct S3 `l8ui`, replacing one
`l16si` or `l16ui`. No extra style-pointer load, sign extension, decoding or
per-read range check is introduced. The previous family pruning and direct
field behavior remain. See the [record audit](UI_NODE_STORAGE_AUDIT.md) for
all 160 bytes and the associated allocations.

The host's populated 49-node fixture remains at 1,109,888 B allocated, the same
as the previous build and 96,592 B below registry. No additional host allocator
saving is claimed for this batch. Plain and rounded fixtures still match all
six pixel phases. Host allocator totals include spare capacity and allocation
rounding; they are not S3 heap bytes and must not be added to device savings.

## Rejected incremental image

The first v11 image, SHA-256
`b02f492af30581f1d5180645047bd4207655b0ec859cb5b0f41142575cf74fd6`,
was rejected. Debug information exposed both 176-byte and 180-byte Node layouts
in different linked translation units. It mounted the expected 15/48 nodes but
painted zero pixels in maintenance/audio. Its apparent 185,244 B free-heap gain
and clean audio counters are **invalid comparisons with a functioning UI**.
The image, ELF, measurements and logs remain saved under names containing
`mixed-layout-rejected`.

The control image was restored while diagnosing this. A complete native rebuild
with compiler-cache reuse disabled produced consistent record layouts and
restored the control's 35,699-pixel maintenance census and 29,678-pixel audio
census. `tools/esp32/check_ui_layout.py` now checks record sizes and member offsets
across every linked translation unit; it rejects the failed image and accepts
both the saved control and clean candidate. This identifies the linked mismatch;
the exact incremental invalidation failure has not been isolated.

## Behavior and timing

The payload-only 160-byte image passed a 45-second fixed-load audio window:
maximum cycles were 312,095 / 292,115, misses 0 / 0 and USB errors zero. Mean
last-six heartbeat times were 1,153.83 / 1,193.50 µs. This does not establish a
speedup over the previous 164-byte window (1,145.33 / 1,193.17 µs).

Its 90-second UI soak reproduced a startup miss at 9,132 ms: 321,092 cycles
against the 320,000-cycle budget, before the first injected tap. Core 1 peaked
at 292,727 cycles without misses; USB errors and watchdog firings were zero.
First paint was 3,839,973 µs; 50 taps produced 41 mask changes and 35 painted
taps. Padding/trimmed counts were 113 / 156. This failed result stays saved.

After restricting the pump diagnostics to maintenance, the first 90-second
repeat measured 312,571 / 292,838 cycles and 0 / 0 misses, with zero USB errors
or watchdog firings. First paint was 3,902,783 µs, with 49 taps, 38 mask changes,
37 painted taps and 99 / 145 padding/trimmed frames. This supports the
hypothesis that startup diagnostics contributed, but does not establish causality or reliable startup. A second identical
90-second window also passed: 314,735 / 292,163 cycles, 0 / 0 misses, zero USB
errors and watchdog firings, first paint 3,842,701 µs, 50 taps, 40 mask changes,
35 painted taps and 112 / 157 padding/trimmed frames. These are two observed
passing runs; the previous failed image is not retroactively qualified.

The final diagnostic-scoped 45-second fixed-load window also passed: maxima
314,735 / 292,579 cycles, 0 / 0 misses, zero USB errors and watchdog firings,
and 8 / 0 padding/trimmed frames. Mean last-six heartbeat stage times were
1153.83 / 1193.50 µs. This is not evidence of a steady DSP speedup; the concrete
improvements are memory/code removal and avoiding unnecessary startup diagnostic
work. No audio sample arithmetic was changed by this follow-up.

The analyzer passes 320 cases and the CLI passes 22. Seventeen native
configurations match 22 geometry/pixel frames, with an additional rounded,
clipped app-root regression painting 26,968 pixels. That scene also passes the
pedal's exact feature-flag combination. Pedal checks, tests and the clean S3
build pass. Full-CSS WASM compatibility passes using the installed compiler's
legacy Gea adapter; this does not verify automatic pruning or browser visuals.

The following tables preserve the earlier 176/164-byte timing measurements.
They are historical controls, not results from the current 160-byte image.

| Device measurement                  | Previous common-storage build | Bounded-style candidate |
| ----------------------------------- | ----------------------------: | ----------------------: |
| Steady window                       |           45 s, amp + mask 63 |     45 s, amp + mask 63 |
| Mean last-six heartbeat stage times |        1,152.50 / 1,193.67 µs |  1,145.33 / 1,193.17 µs |
| Maximum cycles since configure      |             314,647 / 292,174 |       292,172 / 292,575 |
| DSP deadline misses                 |                         0 / 0 |                   0 / 0 |
| USB transfer errors                 |                             0 |                       0 |
| Padding / trimmed frames            |                         9 / 0 |                   6 / 0 |
| UI soak first paint                 |                  3,862,759 µs |            3,879,976 µs |
| UI soak maximum cycles              |             316,713 / 292,111 |       322,370 / 291,951 |
| UI soak deadline misses             |                         0 / 0 |               **1 / 0** |
| UI soak USB transfer errors         |                             0 |                       0 |
| UI soak padding / trimmed frames    |                     112 / 159 |               112 / 159 |

The small steady-time difference is not an established speedup. The candidate's
UI soak recorded a core-0 startup deadline miss at 9,172 ms: 322,370 cycles
against a 320,000-cycle budget. It is not timing-qualified. Its first steady
boot also delayed UI initialization compared with the control; the following
soak returned to the prior first-paint range. The matched control and candidate repeat below both passed, so the miss did
not reproduce on that pair. It remains an unresolved intermittent failure.

| Follow-up UI stress window     | Saved 176-byte control | 164-byte candidate repeat |
| ------------------------------ | ---------------------: | ------------------------: |
| Core 0 / core 1 maximum cycles |      314,203 / 292,603 |         314,015 / 292,782 |
| Deadline misses                |                  0 / 0 |                     0 / 0 |
| USB transfer errors            |                      0 |                         0 |
| First paint                    |           3,907,897 µs |              3,890,736 µs |
| Padding / trimmed frames       |              114 / 160 |                 100 / 146 |

The candidate repeat used 51 taps, 41 mask changes and 32 painted taps.
The original failed window remains in the table and raw artifacts; these
passing follow-ups do not establish that startup is reliable.

The 90-second UI soak uses legacy panel tap coordinates and changes processing
load: this candidate recorded 50 taps, 41 mask changes and 35 painted taps. It
can miss controls or late paints and is not a fixed-load DSP or global UI
latency benchmark. The configured steady cadence is 15 fps (66,667 µs); this
is not a 60 fps qualification.

The deadline budget is 320,000 cycles. Device pixel counts establish that the
expected UI paints; they do not prove full-frame pixel equality. Native tests
supply the pixel/geometry comparisons. Input is at the noise floor. Host prepared
audio equivalence passes; a hardware sample-by-sample null test has not been
performed.

Earlier negative results remain relevant: the preceding scroll-pruned 184-byte
image recorded one startup miss at 320,120 cycles and first-paint variation from
3.98 to 5.07 seconds. An older 212-byte image recorded two startup misses.
Successful current windows do not explain away those intermittent results.

## Artifacts and release status

The current image is `build/css-feature-audit/candidate-node-payload-quiet.bin`,
SHA-256 `d0bc2e675bad3698e987f35de895f0637fb32f5131b48e58f5b48356b3707e5d`.
Its ELF, source audit, linked-layout report and measurement files are alongside
it. The payload-only candidate and its failed startup run are retained as
`candidate-node-payload.*` and `device-node-payload-uisoak.*`; previous
`bounded-style` and `common-storage` artifacts remain saved. Unrelated JPEG
changes remain excluded.

No packages were published or pushed. Development builds use explicit
engine/elements artifacts and analyzer/CLI overrides. The lockfile has no local,
file or link dependencies, and installed Gea packages are not symlinks. Final
registry-only build qualification remains outstanding. Complete optimization
and resolve startup qualification before publication and the dependency bump.

The board is on the 160-byte candidate with maintenance-only diagnostics and
has returned to maintenance mode.
The 176-byte control remains saved for comparison or rollback.
