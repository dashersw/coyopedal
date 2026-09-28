# S3 continued optimization investigation — 2026-09-28

The completed sweep retains eight of eleven further changes. Fresh measurements
show **7.00% less processing time on the limiting stage than the original
firmware**, including **1.18% from this round**. The verified production image
was installed and the board was returned to normal audio mode.

All timing figures are microseconds per 64-frame block at 240 MHz / 48 kHz.
The per-stage deadline is 1333.333 µs. Factory model 1 is used for the fresh
round-four comparisons. Earlier model-0 figures are not subtracted from them.
The final original/round-three/final comparison uses fresh five-second model-1
runs with deterministic synthetic plucks.

## Final production before and after

The fresh five-second comparisons use the same factory model 1 (Ampete 4 V30),
preset 0, deterministic input, clock and block size. The limiting-stage mean
falls **7.00% versus the original firmware** and **1.18%
versus round three**. These are measured execution-time reductions, not an
end-to-end latency measurement. Percentages from earlier rounds are not added.

| Full six-effect chain | Core 0 µs | Core 1 µs | Limiting-stage µs | Mean deadline headroom µs |
| --------------------- | --------: | --------: | ----------------: | ------------------------: |
| Original              |   1297.08 |   1289.31 |           1297.08 |                     36.25 |
| Round-three reference |   1196.37 |   1220.74 |           1220.74 |                    112.59 |
| Final                 |   1163.38 |   1206.28 |           1206.28 |                    127.05 |

| Case               | Core 0 original | Core 0 final | Reduction | Core 1 original | Core 1 final | Reduction |
| ------------------ | --------------: | -----------: | --------: | --------------: | -----------: | --------: |
| NAM only           |         1129.78 |      1024.75 |     9.30% |         1273.08 |      1187.33 |     6.74% |
| Hard Gate only     |         1143.17 |      1041.01 |     8.94% |         1273.77 |      1188.01 |     6.73% |
| Studio VCA only    |         1146.22 |      1039.06 |     9.35% |         1273.57 |      1189.16 |     6.63% |
| Chorus only        |         1143.41 |      1043.12 |     8.77% |         1270.69 |      1186.70 |     6.61% |
| Klon only          |         1163.36 |      1055.03 |     9.31% |         1278.30 |      1194.03 |     6.59% |
| Digital Delay only |         1136.95 |      1034.94 |     8.97% |         1293.94 |      1198.71 |     7.36% |
| Spring Reverb only |         1205.84 |      1102.40 |     8.58% |         1264.86 |      1193.99 |     5.60% |
| All six effects    |         1297.08 |      1163.38 |    10.31% |         1289.31 |      1206.28 |     6.44% |

All three eight-case profiles have zero deadline misses, input/output drops,
USB transfer errors, trims and clips. They record **15 / 8 / 12 silent frames**
respectively across the eight cases; the final full-chain case contains four.
The short profile is therefore not represented as perfectly gap-free. Steady
60-second validation is reported separately below.

This round retains **eight of eleven tested changes**: six kernel changes,
one ordered output writer, and feedback batching. Mono packing, redundant
self-notification removal and QACC stack extraction are rejected. The tables
below give each isolated result and the independently measured combinations.

## Independent kernel trials

The controlled diagnostic reference is unchanged round-three code, with a
processor reset and temporary checksum. All rows below use model 1 and three
seconds per case. Positive savings mean faster. Reset changes gain/tone state;
these diagnostic numbers must not be compared with production timings.

Diagnostic all-effects reference: **1174.800 / 1196.550 µs** (core 0 / core 1).

| Candidate        |   Core 0 |   Core 1 | Core 0 saved | Core 1 saved | Decision                                                             |
| ---------------- | -------: | -------: | -----------: | -----------: | -------------------------------------------------------------------- |
| unsigned-low     | 1160.292 | 1189.562 |       14.508 |        6.987 | Keep: remove redundant sign comparison for 0–127 limbs               |
| high-ring-shift  | 1174.558 | 1193.821 |        0.242 |        2.729 | Keep: hoist the seven-bit high-ring offset                           |
| residual-shift   | 1173.921 | 1191.229 |        0.879 |        5.321 | Keep: hoist the combined residual shift                              |
| input-gain       | 1173.746 | 1196.592 |        1.054 |       -0.042 | Keep: fuse gain into Q15 conversion, preserving multiplication order |
| clean-live-low   | 1170.667 | 1194.600 |        4.133 |        1.950 | Keep: widen the live limb in its existing vector register            |
| clean-qacc-spill | 1222.867 | 1239.225 |      -48.067 |      -42.675 | Reject: exact accumulator extraction through SRAM is slower          |
| clean-mono-pack  | 1174.842 | 1196.467 |       -0.042 |        0.083 | Reject: no measured benefit, including NAM-only                      |

The two low-limb trials overlap; their savings must not be added. The retained
five-kernel combination measures **1163.758 / 1181.279 µs** in the same diagnostic
full-chain test. Core 1 improves **1.276%**; core 0 improves **0.940%**.
The four-kernel combination without the live-register change measured
1194.863 / 1180.892 µs. Its slower core-0 result is another reason to measure
combinations rather than add independent gains.

All completed diagnostic images match **49,152 settled amp-output samples,
CRC `b4298a4d`** on model 1. The input-gain change also matches **114,944 host
output floats exactly**, including gain/tone/partial-block cases. The mono
packer matches the existing converter for **2080 finite float patterns and
lengths 0–64 on the device**, but NAM-only core-1 time is 1191.254 µs versus
1191.050 µs for its reference. It adds no useful measured speed.

## Production pipeline and feedback trials

These use the separate production reference: model 1, three seconds per case,
without the controlled reset or checksum instrumentation. All-six-effect means:

| Candidate                    |   Core 0 |   Core 1 | USB callback µs/block | Decision                                                      |
| ---------------------------- | -------: | -------: | --------------------: | ------------------------------------------------------------- |
| baseline-production          | 1193.125 | 1222.496 |                56.475 | Round-three reference                                         |
| single-writer-production     | 1192.971 | 1222.717 |                56.617 | Keep for ordered output ownership                             |
| clean-notify-production      | 1192.992 | 1222.875 |                56.362 | Reject: no measurable saving versus single-writer             |
| clean-feedback-production    | 1182.408 | 1223.333 |                55.550 | Keep: halve feedback completions                              |
| combined-production          | 1168.196 | 1208.908 |                56.612 | Five kernels + single writer                                  |
| combined-feedback-production | 1164.946 | 1208.967 |                55.425 | Five kernels + single writer + feedback batching              |
| combined-extra-production    | 1164.571 | 1209.029 |                55.667 | Mono/notification combination; qualification observation only |

Feedback batching changes only the feedback URB to eight USB frames. Capture
and playback retain one-frame URBs. The isolated full-chain run reduces feedback
completions from **752 to 376**, saves **10.717 µs on core 0**, and has no deadline
misses, silence, trims, drops or USB errors. On top of the kernel/single-writer
combination it saves **3.250 µs on core 0**, with core 1 effectively unchanged.

The single-writer change fixes a real ownership violation. At split 8, an older
wet block could be published on core 0 while a following dry block was published
on core 1 during a reverb toggle. The output ring permits one producer. A
reproduction using its actual template shows two 64-frame reservations sharing
a write cursor: only 64 of 128 frames are published and the first block is
overwritten. With one ordered writer, all 128 frames arrive in order. The
reproduction passes ASan/UBSan. This does not establish how much observed UI
silence came from that race.

Every block now returns to stage A for output. This adds a return hop to the dry
path; no extra ring capacity, sample rate or block-size change was made.
End-to-end analog latency has not been measured. Removing stage A's redundant
self-notification produces **no measurable speed gain** and is not retained.

## UI interaction

Each window lasts 45 seconds and loads model 1, starts the actual display/touch
UI and sends taps. Counts below are after window configuration, excluding
startup. Tap timing and effect masks vary between runs; input was near the noise
floor, so this is a control/transport check, not a guitar-signal comparison.

| Image                        | Taps | Silent frames | Trimmed frames | Core 0 misses | Core 1 misses |
| ---------------------------- | ---: | ------------: | -------------: | ------------: | ------------: |
| baseline-production          |   16 |           339 |             80 |             0 |             0 |
| single-writer-production     |   17 |           256 |             76 |             0 |             0 |
| combined-production          |   17 |           268 |             92 |             0 |             0 |
| combined-feedback-production |   17 |           284 |            108 |             0 |             0 |
| combined-extra-production    |   17 |           315 |            187 |             0 |             0 |

UI transition silence remains. It must not be described as eliminated or as an
acoustic listening result. The tests also check display startup: an audio stream
alone is insufficient, because a prior profiling image starved the display's
DMA allocation despite processing audio successfully.

## Build audit and evidence quality

Snapshot restores initially used `copy2`, preserving old modification times.
Ninja consequently reused some previous NAM objects. The original `live-low`,
`single-writer-notify-production`, `feedback-eight-production`, `qacc-spill` and
`mono-pack` results are excluded from independent comparisons above. Their raw
files are retained as exploratory evidence, not silently overwritten. The mono
exploratory image actually contained the four-kernel combination as well.

Every affected image was rebuilt after refreshing all changed source timestamps.
Build logs confirm all six changed objects recompiled. ELF inspection confirms
baseline input-converter signatures in isolated repeats and the gain-aware
signature in combined images. Independent repeated results use the `clean-`
labels. The original baseline, unsigned-low, high-ring-shift, residual-shift and
input-gain builds were unaffected. Four/five-kernel combination images contain
the intended objects. Final selection requires a freshly verified production
build, matching source snapshot and running ELF hash.

## Audit conclusions

The retained head change shares sign and quantization shift settings between
independent vector halves, removing two `SSAI` instructions per frame. It saves
**0.621 µs on core 1** against the initial model-1 five-kernel combination
(1181.279 → 1180.658 µs), and **0.658 µs** in the matched model-0 comparison
(1174.163 → 1173.504 µs). The head function shrinks from 300 to 294 bytes.
Both factory models match their established 49,152-sample references:
**model 0 `d019effc`; model 1 `b4298a4d`**.

`AUDIO TRY` model selection is deliberately temporary. An initial attempt to
carry its model-0 selection across a reboot was rejected by the model guard;
that row still selected model 1 and is excluded from model-0 comparisons.
Model 0 was then qualified by the same temporary model-load override in both
diagnostic images. The override and CRC instrumentation are absent from the
production image. Production comparisons use the unchanged boot preset's
model 1, consistently across the original, round-three and final images.

Relative to the round-three image, production IRAM text falls from **101459 to
101139 bytes** (320 fewer). Internal data remains 53846 bytes and BSS 63536 bytes;
RTC FAST use is unchanged at 3824 bytes.

Previously tested split-9 scheduling, USB/DSP priorities, hot RTC allocations,
blind SRAM-bank changes, cached argument tables, broad guarded S16 extraction
and extra tap unrolling remain rejected. Broad guarded extraction lost 4.18%
in round two; cached tables consumed about 3 KiB without a useful gain. All
hot model history/weights already reside in internal SRAM. The P4 lesson that
transfers is reducing movement and repeated setup; its multiple-frame weight
reuse cannot be copied directly into the S3's one-result QACC/register schedule.

S16 QACC extraction saturates, so it cannot replace the exact S32 reconstruction
without guards. Instruction semantics were checked against the
[Espressif S3 technical reference](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf).
This sweep preserves model weights, arithmetic precision, CPU clock and the
64-frame audio block. Larger changes to model architecture, sample rate,
latency policy or hardware are separate design choices.

## Reproduction and artifacts

All trial images, matching source snapshots, hashes, raw measurements, build/test
logs and the ring reproduction are under `build/s3-performance-round4-20260928/`.
Checks use `npm run check`, `npm test`, and the Gea firmware build with the local
target checkout. Maintenance-mode updates use the Wi-Fi OTA tool. The measurement
sweep itself created no commits, branches or pull requests.

The measurements used the existing AMOLED 2.41 board/UI configuration and local
Gea target fixes for runtime stack placement and panel transfers. Those board/UI
changes are separate from the audio optimization commit; this report does not
qualify a fresh checkout's original AMOLED 2.06 configuration on hardware. Raw
artifacts under `build/` remain local and are not included in the commit.

## Final steady-state validation and deployment

The final production image passed two separate 60-second synthetic-input runs
with all six profiled effects enabled. Each captured **2,880,080 frames** and
recorded **zero silent frames, trims, input/output drops or USB transfer errors**.

The timed run recorded zero deadline misses on both cores. Its mean times were
1149.083 / 1208.529 µs, with maxima 1186.571 / 1219.275 µs (core 0 / core 1).
The production-equivalent run disabled cycle and peak telemetry; its zero timing
and peak fields are not measurements. Neither run is an acoustic listening test.

The final UI window completed 16 actual taps and 13 observed effect-mask changes,
with zero deadline misses and USB errors. After configuration it recorded 252
silent and 76 trimmed frames (5.25 ms and 1.58 ms respectively), so transition
gaps remain. Its observed core-0 maximum was 1314.213 µs, higher than the
round-four baseline UI observation but still within the 1333.333 µs deadline.
These wall-time-driven UI runs do not provide an identical per-action workload
or a worst-case latency bound.

`npm run check`, `npm test`, and the firmware build pass. All seven native
objects restored for the final build were recompiled, source hashes were saved,
and the installed ELF hash was verified after both soaks. The board acknowledged
`AUDIO MODE` with “OK switching to audio; radios will turn off”. Its boot preset
was left as configured. Temporary CRC/model overrides, the rejected mono packer
and notification change are absent from production.

| Final artifact  | SHA-256                                                            |
| --------------- | ------------------------------------------------------------------ |
| ELF             | `82960d5b20502aab5a9f52fc611135889289a3debc13fe85f98c2bd194899d3c` |
| Firmware binary | `6a1f1772ea5fc2ccfee37451fa9ceda4121b4934fa033e086dd00795ee2caf5a` |

The practical candidates in this sweep are resolved: eight retained, three
rejected, and the earlier scheduling, memory-placement and extraction dead ends
remain documented. This completes the sweep at the existing clock, precision
and block size. It does not claim that further optimization is impossible.
