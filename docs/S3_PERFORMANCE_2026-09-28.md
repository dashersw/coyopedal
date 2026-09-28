# S3 performance comparison — 2026-09-28

The first-round optimized firmware was installed on the AMOLED 2.41 ESP32-S3 at `192.168.178.159` (MAC `30:ed:a0:29:59:14`). The full six-effect chain uses **3.85% less time on its limiting stage**. Core 0 improves by 5.4–6.6%; core 1 by 3.7–3.8%.

## Before and after

Average processing time per 64-frame block, in microseconds. Same 240 MHz CPU, 48 kHz sample rate, preset 0 / model 0 (**VoLum Herbert C1**), and deterministic synthetic plucks. The deadline is 1,333.33 µs. Each case measures three seconds after warm-up, with both cores and the attached USB audio interface running.

| Case               | Core 0 before | Core 0 after | Reduction | Core 1 before | Core 1 after | Reduction |
| ------------------ | ------------: | -----------: | --------: | ------------: | -----------: | --------: |
| NAM only           |      1,118.84 |     1,045.11 |     6.59% |      1,264.03 |     1,215.42 |     3.85% |
| Hard Gate only     |      1,135.02 |     1,067.28 |     5.97% |      1,263.05 |     1,216.15 |     3.71% |
| Studio VCA only    |      1,127.79 |     1,061.34 |     5.89% |      1,265.13 |     1,217.43 |     3.77% |
| Chorus only        |      1,131.66 |     1,062.56 |     6.11% |      1,262.14 |     1,215.06 |     3.73% |
| Klon only          |      1,142.07 |     1,082.31 |     5.23% |      1,271.51 |     1,223.98 |     3.74% |
| Digital Delay only |      1,127.60 |     1,059.55 |     6.04% |      1,273.80 |     1,226.38 |     3.72% |
| Spring Reverb only |      1,207.98 |     1,131.49 |     6.33% |      1,255.47 |     1,208.86 |     3.71% |
| All six effects    |      1,275.19 |     1,205.95 |     5.43% |      1,272.81 |     1,226.09 |     3.67% |

Both baseline and final profiles had **zero deadline misses, input/output drops, USB transfer errors and clipped samples across all eight cases**. The baseline recorded four silent frames in the Klon case; the final profile recorded none. The masks were 0, 1, 2, 16, 4, 32, 8 and 63; the final case means the profiler’s six effects, not the additional second modulation block.

With all six effects, the larger of the two stage averages fell from **1,275.19 to 1,226.09 µs**. Mean deadline headroom grew from 58.15 to 107.25 µs. The largest observed stage time fell from 1,328.20 to 1,302.34 µs. These are observed benchmark timings, not an end-to-end latency reduction or an all-model worst-case bound.

The exclusive effects profile does not run the normal UI. UI and model reload checks are separate from the timing comparison. One delay-case core-0 maximum was reported as zero by the existing profiler; it is unavailable, not zero execution time.

## Changes retained

The useful P4 lesson was to reduce movement of intermediate values. The S3 already has fused kernels and pipelined SIMD dot products, so simply importing the P4 six-tap unrolling or argument cache did not produce a worthwhile improvement.

- Keep rational activation residual limbs in vector registers, and keep the next high-ring cursor in a scalar register.
- Convert the first layer’s eight input lanes with vector shifts, A22 clamps and packing.
- Copy aligned history mirror rows with vector loads/stores and simplify the fixed graph’s wrap handling.

The retained changes preserve precision, model weights, arithmetic ordering, core split, clock speed and block size. They add no static model workspace. IRAM text shrank by 516 bytes (512 bytes after reservation alignment), and measured free internal memory increased from **9,224 to 9,736 bytes**.

## Numerical and build checks

- Original and optimized diagnostic builds matched CRC **`d019effc`** over 49,152 settled amp-output samples, starting from an identical processor reset and fixed input. This window exercises repeated ring wraps.
- The vector quantizer passed **81,920 on-device comparisons** against the scalar shift/clamp/split operation: random signed inputs, clamp endpoints, shifts 0–31 and 1–4-frame calls.
- `npm run check`, `npm test`, and the Gea firmware build passed.
- The final production image removes the temporary checksum and quantizer-test instrumentation.

The first diagnostic attempted to hash after uncontrolled live-input warm-up. Repeating unchanged firmware produced different checksums, so those comparisons were replaced with the controlled reset above. Reset also changes gains and tone; those diagnostic timings are excluded from this before/after table. Numerical qualification applies to the tested signal/model, not every possible model and input.

## Final production soak

The final image completed a 60-second synthetic-input run with mask 63 and production telemetry disabled: **2,880,080 captured frames**, zero USB errors, input/output drops, trimmed frames or silent frames. Cycle, deadline and peak telemetry are disabled in this run; the zero timing/peak fields are not measurements. The timed profile above provides the deadline results.

## UI and model reload stress

A 45-second `AUDIO TRY 45 ENGAGE MASK 63 MODEL 1 UISOAK` window successfully
loaded model 1 (`volum-ampete-4-v30`) and exercised live display/touch updates.
The live input was at the noise floor, so this is a control/transport check,
not a demanding signal comparison. The script toggles effects and the amp;
it does not hold the six-effect chain engaged throughout.

The optimized image recorded one core-0 overrun (1,344.15 µs, 10.82 µs over
deadline), zero core-1 overruns and zero USB transfer errors. It recorded
2,212 silent frames including startup/model transitions, or 1,637 after the
window was configured. This stress run is not represented as dropout-free.
The matching original-image run loaded the same model and used the same 45-second UI script:

| UI stress metric                             |    Original |   Optimized |
| -------------------------------------------- | ----------: | ----------: |
| Core 0 deadline overruns after configuration |         221 |           1 |
| Core 1 deadline overruns                     |           0 |           0 |
| Maximum core 0 time                          | 1,379.90 µs | 1,344.15 µs |
| USB transfer errors                          |           0 |           0 |
| Total silent frames, including transitions   |       2,166 |       2,212 |
| Silent frames after configuration            |       1,543 |       1,637 |

This establishes that the UI-overrun problem also occurs on the original firmware. The observed deadline count improved substantially, while transition silence remains. UI actions are driven by wall time and their application depends on rendering/processing load, so the instantaneous effect masks are not identical between runs. These counts are supplementary stress observations, not the basis for the CPU-speed percentages above. The original-image comparison was followed by restoration of the optimized production image, verification of its ELF hash, and return to normal audio mode.

## Reproduction and evidence

Build with the local Gea target checkout required by this board:

```sh
GEA_TARGETS_ROOT=/Users/dashersw/Projects/Code/github/geastack/targets npm run build:firmware
/usr/bin/python3 tools/esp32/amoled_remote.py --host 192.168.178.159 effects-profile --seconds 3 --input synthetic --output build/profile.json
```

Local evidence is preserved in `build/s3-performance-20260928/`: `baseline-profile.json`, `final-profile.json`, diagnostic profiles/logs, source snapshots, and baseline/final `.bin`, `.elf` and `.map` files. Original firmware is also retained as `original.bin`.

| Artifact       | SHA-256                                                            |
| -------------- | ------------------------------------------------------------------ |
| `baseline.elf` | `ca5d40198c4941cc1bb65fed5dd73c8258bb194718a15fd793a1108787c19e37` |
| `final.elf`    | `6e7e14a4d4557fc4df431cbe9a7083998228445b391051c9a72aff1bbe42f110` |
| `final.bin`    | `daba7463c236cf458a06c1e90c397f7afc42e53d75107a77cb24933871f4b01d` |

## Follow-up trials

The second round is recorded in [S3 follow-up performance trials](S3_PERFORMANCE_ROUND2_2026-09-28.md), including a fresh baseline, the retained vector/control changes, rejected trials, and their limits. The measurements above remain the first round's results.
