# S3 follow-up performance trials — 2026-09-28

The second round keeps the vector head reduction, vector input projection, and
short control updates that preserve USB buffering. Extending guarded S16
extraction to all high-limb layers was slower and was rejected.

The fresh three-second production comparison shows a **0.62% reduction on the
limiting stage with all six effects**, below another 5–10%. Results on core 0
are mixed: NAM alone improves by 1.87%, while the six-effect case takes 0.74%
longer. UI-transition silence improves substantially, but is not eliminated.

## Production before and after

Microseconds per 64-frame block at 240 MHz / 48 kHz. Both images use preset 0,
model 0 (VoLum Herbert C1), deterministic synthetic plucks and three seconds
per case. The deadline is 1,333.33 µs. “Before” is the first round's installed
optimized image, measured again during this round. Positive percentages mean
less processing time; negative percentages mean a regression.

| Case               | Core 0 before | Core 0 after | Reduction | Core 1 before | Core 1 after | Reduction |
| ------------------ | ------------: | -----------: | --------: | ------------: | -----------: | --------: |
| NAM only           |      1,048.59 |     1,028.99 |     1.87% |      1,217.95 |     1,212.33 |     0.46% |
| Hard Gate only     |      1,071.08 |     1,043.87 |     2.54% |      1,218.82 |     1,213.18 |     0.46% |
| Studio VCA only    |      1,062.20 |     1,049.31 |     1.21% |      1,218.30 |     1,211.52 |     0.56% |
| Chorus only        |      1,067.10 |     1,041.01 |     2.45% |      1,217.32 |     1,208.99 |     0.68% |
| Klon only          |      1,081.49 |     1,082.87 |    -0.13% |      1,224.36 |     1,215.92 |     0.69% |
| Digital Delay only |      1,063.41 |     1,035.43 |     2.63% |      1,240.57 |     1,232.21 |     0.67% |
| Spring Reverb only |      1,134.64 |     1,147.22 |    -1.11% |      1,211.66 |     1,203.37 |     0.68% |
| All six effects    |      1,206.20 |     1,215.17 |    -0.74% |      1,237.37 |     1,229.74 |     0.62% |

The six-effect limiting-stage average falls from 1,237.37 to 1,229.74 µs;
mean deadline headroom rises from 95.97 to 103.60 µs. This is an execution-time
comparison, not a change to sample rate or end-to-end audio latency.

The fresh baseline has zero deadline misses and eight silent frames across
its eight cases. The final image has zero silent frames, but one core-0
deadline miss in the six-effect case: 1,369.69 µs. Both have zero USB transfer
errors, input/output drops, trimmed frames and clipped samples in this profile.
The final reverb timing increased in that case; the profile does not establish
why. The follow-up ten-second-per-case profile completed all eight cases with
zero deadline misses, USB errors, input/output drops, trims and clips, but
recorded 62 silent frames (1.29 ms total). Its six-effect
means were 1,193.47 / 1,230.04 µs (core 0 / core 1). This longer run did not
reproduce the initial miss; it does not erase that earlier observation or
replace the equal-duration before/after comparison above.

## UI and model-load stress

Both images ran `AUDIO TRY 45 ENGAGE MASK 63 MODEL 1 UISOAK`, successfully
loading `volum-ampete-4-v30` and processing actual UI taps with the audio
transport active. Input was at the noise floor. These runs test transitions
and transport, not demanding guitar signals.

| Metric                                     |      Before |       Final |
| ------------------------------------------ | ----------: | ----------: |
| Silent frames after window configuration   |       1,556 |         432 |
| Equivalent inserted silence at 48 kHz      |    32.42 ms |     9.00 ms |
| Trimmed input frames after configuration   |           0 |         240 |
| Core 0 deadline misses                     |           3 |           0 |
| Core 1 deadline misses                     |           0 |           0 |
| Maximum core 0 time                        | 1,353.04 µs | 1,306.71 µs |
| USB transfer errors                        |           0 |           0 |
| Total silent frames, including setup       |       2,131 |       1,198 |
| Logged taps / observed effect-mask changes |     19 / 12 |     20 / 13 |

Observed post-configuration silence falls **72.24%**. The retained control
path still drains in-flight DSP blocks, but keeps capture/playback buffering
active and avoids discarding the input ring when a short edit ends. A model
or preset update retains the full pause, including when nested inside a short
edit. This does not eliminate every transition gap: the final UI run also
trims 240 input frames (5 ms). The baseline's full pauses discard capture
outside this trim counter, so these counters do not fully quantify lost input
on the baseline. Wall-time-driven UI actions also occur at different instants;
the 72.24% figure describes these runs, not a general per-action guarantee.

A control-only candidate independently recorded 400 silent and 207 trimmed
frames after configuration, with no deadline misses. Its 18 logged taps
included nine observed effect-mask changes.

## Qualification and rejected trials

All CPU trial numbers below use the same controlled processor reset and
checksum instrumentation. They are separate from the production table because
the reset changes the gains/tone state.

| Diagnostic image              | NAM core 0 | NAM core 1 | Six-effect core 0 | Six-effect core 1 | Decision                            |
| ----------------------------- | ---------: | ---------: | ----------------: | ----------------: | ----------------------------------- |
| Baseline                      |   1,052.18 |   1,192.09 |          1,196.99 |          1,212.02 | Reference                           |
| Broad guarded extraction      |   1,046.82 |   1,243.13 |          1,175.85 |          1,262.73 | Reject: limiting stage 4.18% slower |
| Head reduction                |   1,043.98 |   1,186.37 |          1,181.23 |          1,206.83 | Keep                                |
| Head + corrected vector input |   1,024.57 |   1,185.69 |          1,169.14 |          1,206.76 | Keep                                |

- All completed assembly trials match CRC **`d019effc`** over 49,152 settled
  amp-output samples from identical reset/input state.
- The input-vector self-test passes **524,288 comparisons per invocation**:
  every supported shift (0–15), signed edge/random values, partial blocks,
  head-buffer zeroing and untouched trailing canaries.
- The first input-vector candidate passed its arithmetic checks but crashed
  when returning through model prewarm, causing automatic OTA rollback. Its
  rounding scratch overlapped the register-window save area in a 32-byte
  stack frame. The corrected kernel uses 48 bytes and passed boot, arithmetic,
  checksum and timing qualification. The failed image was not retained.
- `npm run check`, `npm test`, `git diff --check`, and the Gea firmware build
  pass. An additional ASan/UBSan control-lifecycle harness covers nested edits,
  upgrade to a full pause, draining, timeout recovery and pre-task boot edits.
- The installed production build contains none of the temporary checksum or
  scalar-comparison instrumentation. Old diagnostic lines remain in the
  reboot-surviving log ring; they do not indicate instrumentation in this image.

The production image completed a **60-second** six-effect synthetic-input
soak: **2,880,080 captured frames**, zero silent/trimmed frames, input/output
drops and USB errors. Production cycle/peak telemetry is disabled for this
soak; zero timing and peak fields are not measured deadline/peak results.

## Memory and artifacts

IRAM text decreases by another **292 bytes** (100,467 → 100,175), and free
internal memory in the profile increases by **500 bytes** (9,736 → 10,236).
Clock, block size, core split, model precision and effect definitions are
unchanged. The input weights now explicitly require 16-byte alignment.

Evidence, source snapshots, test harnesses and recovery images are under
`build/s3-performance-round2-20260928/`. The original first-round production
image is `baseline.bin`; the retained image is `production.bin`. The manifest
records image hashes and trial decisions.

| Artifact     | SHA-256                                                            |
| ------------ | ------------------------------------------------------------------ |
| Baseline ELF | `6e7e14a4d4557fc4df431cbe9a7083998228445b391051c9a72aff1bbe42f110` |
| Final ELF    | `48b30ede3ba9afd05a4b015ebac0d9ed0d4843c155ad8e4602ea363d4de5f082` |
| Final binary | `b27a62df39652ba51a5e82a9eff71de248cb73bc33ca3da6cae549fe99c63cfe` |

The final installed ELF hash was checked again after all trials, and the
board accepted the command to return to normal audio mode.
