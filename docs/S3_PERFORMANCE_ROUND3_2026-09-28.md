# S3 output, control and profiling trials — 2026-09-28

The retained production image reduces full-chain processing time by **2.10%
on core 0 and 1.08% on core 1**, which remains the limiting stage. This
round adds about 1.1% at the bottleneck, below another 5–10%. The block-boundary
control change reduces observed UI-transition silence by **22.62%**.

Layer profiling now produces useful measurements. Its instrumented image failed
display DMA allocation during UI stress, so profiling is retained as an **opt-in
diagnostic build** and compiled out of the accepted production image. The
replacement production image passed real UI interaction tests.

## Each change measured separately

Microseconds per 64-frame block at 240 MHz / 48 kHz, preset 0 / model 0
(VoLum Herbert C1), deterministic synthetic plucks, five seconds per case.
The deadline is 1,333.33 µs. These are processing times, not end-to-end latency.

| Image                    | NAM core 0 | NAM core 1 | Six-effect core 0 | Six-effect core 1 | Free internal memory |
| ------------------------ | ---------: | ---------: | ----------------: | ----------------: | -------------------: |
| Baseline                 |   1,039.34 |   1,212.91 |          1,202.77 |          1,229.95 |             10,236 B |
| Output pass              |   1,039.31 |   1,212.44 |          1,202.65 |          1,229.46 |             10,236 B |
| Output + controls        |   1,037.40 |   1,209.78 |          1,177.77 |          1,216.66 |             11,212 B |
| Layer-instrumented trial |   1,037.67 |   1,209.95 |          1,178.13 |          1,216.75 |             11,008 B |
| Retained production      |   1,037.47 |   1,209.77 |          1,177.55 |          1,216.62 |             11,212 B |

- **Output pass:** full-chain core 1 falls 1,229.95 → 1,229.46 µs (**0.040%**).
  Its reduction across all eight cases is consistently 0.47–0.56 µs.
- **Control publication:** relative to output-only, core 0 falls 1,202.65 →
  1,177.77 µs (**2.07%**) and core 1 falls 1,229.46 → 1,216.66 µs (**1.04%**).
  This includes coefficient/state separation and the resulting compiled layout;
  the test does not isolate those contributions further.
- **Layer instrumentation:** relative to controls, the full-chain means rise
  **0.37 µs on core 0 and 0.09 µs on core 1**. The runtime overhead is small,
  but the memory-placement regression makes it unsuitable for normal operation.
  Production compiles out the per-block branches and counter storage.

Retained production mean headroom on the limiting stage rises from
**103.38 to 116.71 µs**. It includes the corrected delay-only deadline accounting.

## Full production before/after profile

| Case               | Core 0 before | Core 0 after | Core 1 before | Core 1 after |
| ------------------ | ------------: | -----------: | ------------: | -----------: |
| NAM only           |      1,039.34 |     1,037.47 |      1,212.91 |     1,209.77 |
| Hard Gate only     |      1,049.80 |     1,049.33 |      1,211.79 |     1,210.15 |
| Studio VCA only    |      1,049.02 |     1,041.12 |      1,211.46 |     1,210.91 |
| Chorus only        |      1,044.35 |     1,049.80 |      1,209.10 |     1,209.01 |
| Klon only          |      1,085.44 |     1,057.57 |      1,217.64 |     1,215.63 |
| Digital Delay only |      1,036.62 |     1,039.47 |      1,233.13 |     1,220.79 |
| Spring Reverb only |      1,124.82 |     1,112.58 |      1,204.79 |     1,203.45 |
| All six effects    |      1,202.77 |     1,177.55 |      1,229.95 |     1,216.62 |

All five profiles report zero USB errors, input/output drops, trims and clipped
samples. Inserted silence across the eight cases is 11 / 9 / 8 / 9 / 8 frames for
baseline / output / controls / instrumented / production. Production's full-chain
case has zero silence; its isolated gate and delay cases each have four frames.

Production records zero deadline misses in all cases. The largest measured block
is **1,194.90 µs on core 0 and 1,242.43 µs on core 1** across the suite. Earlier
images report zero misses too, but their delay-only core-0 maximum is incorrectly
zero because that path omitted stage-A accounting. Production fixes the condition:
delay runs on stage B; only reverb requires a return to stage A. Its delay-only
core-0 maximum is 1,066.05 µs. Earlier zeros in that field are not deadline proof.

## UI/model-load stress

Every run used `AUDIO TRY 45 ENGAGE MASK 63 MODEL 1 UISOAK`, loading the second
model and driving real UI taps. Input was at the noise floor.

| Metric after configuration    | Output-only reference | Control trial | Retained production |
| ----------------------------- | --------------------: | ------------: | ------------------: |
| Silent frames                 |                   367 |           284 |                 284 |
| Inserted silence              |               7.65 ms |       5.92 ms |             5.92 ms |
| Trimmed input frames          |                   174 |           104 |                 104 |
| Core 0 / core 1 misses        |                 0 / 0 |         0 / 0 |               0 / 0 |
| Maximum core 0 time           |           1,330.72 µs |   1,292.05 µs |         1,248.60 µs |
| Maximum core 1 time           |           1,252.08 µs |   1,241.61 µs |         1,241.62 µs |
| Taps / observed mask changes  |               20 / 13 |       20 / 15 |             19 / 13 |
| Total silence including setup |          1,134 frames |    811 frames |          827 frames |

Post-configuration silence decreases **22.62%** and trimming **40.23%** in both
control and retained production runs. USB errors and input/output drops remain
zero. Wall-clock actions produce different mask transitions, so this is an
observed stress-test result, not an identical-signal benchmark or a universal
per-action guarantee. Control changes preserve filter state, but gaps remain.

## What the layer profiler found

The instrumented image sampled **520 blocks**, one in 64, across all eight cases
and their warmups. Layer indices are zero-based. Means include the actual mix
of narrow and wide input paths.

| Region                | Mean time | Share of sampled core-1 NAM work |
| --------------------- | --------: | -------------------------------: |
| Layers 8–11 together  | 420.00 µs |                           35.67% |
| Layers 14–15 together | 175.39 µs |                           14.90% |
| Output head           |  30.37 µs |                            2.58% |

Layers 8–11 are the strongest measured target for a future kernel investigation.
The head is a small part of the budget, limiting further gains there. These
measurements identify a target; they do not establish another optimization.

## Why profiling is diagnostic-only

The instrumented image (`final.bin`, a historical trial filename) passed audio
profiling and a 60-second audio soak, but failed the UI test. Its largest DMA
block at display startup was **1,792 bytes**, versus **1,984 bytes** on the control
trial. The minimum two-row LCD buffer could not be allocated, Gea reported
bring-up failure, and **zero taps occurred**. Its lower silence count is therefore
not accepted as an interaction result.

Compiling out the profiler restores the control trial's static DRAM layout and
model object size. The accepted `production.bin` starts the display, processes
actual taps, and retains both optimizations plus the deadline-accounting fix.
The instrumented image added only **404 bytes** of IRAM/static DRAM relative to
the control image, illustrating why total free memory alone was insufficient.

For diagnostic builds, set `COYOPEDAL_S3_LAYER_PROFILE=1` globally in `gea.defines`
so every translation unit agrees on the model layout. Use exclusive effect
profiling and restore the default **0** before deploying normal firmware.
`layer_blocks` counts actual sampled blocks, `layer_cycles` holds total cycles,
and index 23 is the head. Divide cycles by `layer_blocks * 240` for microseconds.
`wide_mixin_blocks` counts wide-path selection among those samples. Production
reports `layer_sample_stride=0` and `layer_blocks=0`, explicitly indicating that
layer sampling is unavailable in that build.

## Retained implementation

The output scale is cached at model preparation. Flat tone combines output gain
with conversion; active tone combines gain with its EQ loop. The original
floating-point operation order is preserved.

Controls use three-buffer mailboxes with one serialized control writer and one
audio reader per mailbox. Coefficients are calculated off the audio thread and
adopted at processing boundaries without modifying filter history or clearing
delay lines. Intermediate edits may coalesce. Each amp block carries its full
settings across both stages; each effect adopts a complete coefficient update
before processing. One effect-enable mask follows the block through every stage.
Models and presets retain their full pipeline pause. Mailboxes remain in internal
SRAM so ESP-IDF's atomic implementation takes its hardware path.

## Host validation

- Output-only and control candidates each match the saved baseline for
  **114,944 amp-output float samples**, covering unity/non-unity gains, flat and
  active tone, and partial blocks.
- Effect coefficient publication matches **384,000 stereo-output float samples**
  against the baseline while parameters change between processing blocks.
- A concurrent 200,000-sequence mailbox test passes AddressSanitizer,
  UndefinedBehaviorSanitizer and ThreadSanitizer. It checks stable reader ownership,
  complete values, monotonic updates and eventual delivery of the final value.
- Pipeline tests verify that an edit between stages cannot change the current
  block's amp settings or effect routing. Effects/model lifecycle tests pass.
- `npm test`, `npm run check`, `git diff --check`, and the retained production
  firmware build pass. Host numerical comparisons do not establish the S3's timing or live audio
  behavior.

## On-device numerical qualification

A temporary diagnostic image resets the processor before deterministic injection.
Its **49,152 settled amp-output samples** exactly match the established S3 CRC
**`d019effc`**. Those diagnostic timings are excluded because the reset changes
amp gains/tone. Production contains no checksum or controlled-reset instrumentation.

## Production soak

The retained image completed a **60-second** all-effects synthetic-input soak:
**2,880,080 captured frames**, **0 silent frames**,
**0 trims**, **0/0 input/output drops**,
and **0 USB errors**. Cycle and peak telemetry were disabled, so zero
cycle/peak fields are not measured deadline or clipping results.

## Memory and evidence

Profiled internal free memory is **10,236 → 11,212 bytes**. Linked IRAM text is
**100,175 → 101,459 bytes** and static DRAM data + BSS is **115,782 → 117,382 bytes**.
Linked section sizes do not directly predict free heap because allocation and
alignment also matter. Both factory models loaded, and production UI startup
was verified under the loaded audio pipeline.

Raw measurements, images, matching source snapshots, hashes, tests and UI logs are
in `build/s3-performance-round3-20260928/`. The accepted image is `production.bin`;
`build/pedalboard.bin` contains that image too. `baseline.bin`, `output.bin`, and
`controls.bin` isolate the comparisons. `final.bin` is the rejected instrumented
trial; `diagnostic.bin` adds only the controlled checksum test. The preliminary
`profile.bin` was superseded before flashing.

Production ELF SHA-256:
`31f2340dca6b891c50bce32c79db28c0a54d25683e4b4680631ab22a441d9a8f`.

Production binary SHA-256:
`c51a1cb2d59ce2b887678a4870aa8e9c66890fed63e958a04aaf6a9a23641bf2`.

The installed ELF hash was verified after all measurements. The board acknowledged
`OK switching to audio; radios will turn off` and was returned to normal audio mode.
