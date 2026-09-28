# S3 display-free comparison — 2026-09-28

The comparison uses the retained round-four production image and an experimental image with the same audio source, but no display or touch drivers, no compiled pedal UI, and no Gea runtime loop. Both were measured on the same AMOLED 2.41 V1 board at 192.168.178.159 (MAC 30:ed:a0:29:59:14).

## Result

Removing the display stack reduces the limiting full-chain stage by **0.196%** in this test. This is a steady-state comparison with the normal display idle; it does not measure the cost of tapping or scrolling the UI.

Mean microseconds per 64-frame block; lower is better. Each cell averages three independent five-second profiles. Stage A is core 0; stage B is core 1. The stage deadline is 1,333.333 µs.

| Workload           |  Final A | Audio-only A |  Final B | Audio-only B | Limiting-stage reduction |
| ------------------ | -------: | -----------: | -------: | -----------: | -----------------------: |
| NAM only           | 1032.697 |     1030.494 | 1180.926 |     1179.557 |                   0.116% |
| Hard Gate only     | 1042.642 |     1041.163 | 1181.372 |     1179.974 |                   0.118% |
| Studio VCA only    | 1038.207 |     1036.743 | 1181.085 |     1179.718 |                   0.116% |
| Chorus only        | 1043.479 |     1040.271 | 1179.210 |     1177.633 |                   0.134% |
| Klon only          | 1054.994 |     1053.511 | 1187.274 |     1186.004 |                   0.107% |
| Digital Delay only | 1033.981 |     1032.533 | 1191.979 |     1190.624 |                   0.114% |
| Spring Reverb only | 1092.861 |     1091.431 | 1187.433 |     1185.786 |                   0.139% |
| All six effects    | 1159.653 |     1156.621 | 1201.301 |     1198.947 |                   0.196% |

Full-chain run ranges:

- final: core 0 1159.575–1159.692 µs; core 1 1201.271–1201.329 µs.
- engine-only: core 0 1156.596–1156.650 µs; core 1 1198.942–1198.950 µs.

## Memory and firmware size

| Metric                              |       Final |  Audio-only |
| ----------------------------------- | ----------: | ----------: |
| Free internal SRAM during profiling |    11,712 B |    17,704 B |
| Largest free internal block         |     3,840 B |     8,704 B |
| Firmware binary                     | 3,026,864 B | 1,276,400 B |
| `.iram0.text`                       |   101,139 B |    99,991 B |
| `.dram0.data`                       |    53,846 B |    35,846 B |
| `.dram0.bss`                        |    63,536 B |    60,432 B |
| `.ext_ram.bss`                      |   245,760 B |   190,928 B |
| `.rtc.force_fast`                   |     3,824 B |     3,808 B |

The display-free boot also promotes the existing 16,384-byte modulation buffer into internal SRAM. Therefore the free-heap difference understates the SRAM released by removing the display stack. This is the net audio-only configuration, including the normal opportunistic buffer promotion.

## Reliability

- final, all 24 profile cases combined: `{"input_drops": 0, "output_clipped_samples": 0, "output_drops": 0, "silent_frames": 36, "stage_a_deadline_misses": 0, "stage_b_deadline_misses": 0, "transfer_errors": 0, "trimmed_frames": 0}`.
- engine-only, all 24 profile cases combined: `{"input_drops": 0, "output_clipped_samples": 0, "output_drops": 0, "silent_frames": 31, "stage_a_deadline_misses": 0, "stage_b_deadline_misses": 0, "transfer_errors": 0, "trimmed_frames": 0}`.
- final, 60-second all-effects soak: 2,880,080 captured frames; core 0 mean/max 1153.117/1183.517 µs, core 1 mean/max 1200.992/1217.963 µs; silence 0, trims 0, drops 0/0, USB errors 0, deadline misses 0/0.
- engine-only, 60-second all-effects soak: 2,879,992 captured frames; core 0 mean/max 1148.896/1186.458 µs, core 1 mean/max 1198.763/1214.246 µs; silence 0, trims 0, drops 0/0, USB errors 0, deadline misses 0/0.

These are device counters and synthetic-input tests, not an acoustic listening test or an end-to-end bit-exact output comparison. The ordinary audio heartbeat can show unsigned underflow when the profile deliberately resets transport counters between cases; the structured profile counters above use the measurement boundaries.

## What was removed

- The board definition declares no display, touch controller or offscreen canvas. The panel reset GPIO21 and touch reset GPIO3 are held low for this board revision. No panel or touch driver starts.
- `GEA_EMBEDDED_NO_DISPLAY=1` removes the app display reservation and UI startup. UI wakeups and the touch-soak harness are excluded. The preset/control model still initializes because the audio engine reads it.
- A minimal compiler entry replaces the JSX app. The native boot wrapper and `app_main` linker alias bypass Gea’s bootstrap; after native audio/maintenance services start, the main task exits.
- Linked-image inspection finds zero `Display::`, SH8601, FT6336, `TouchRuntime::`, `Runtime::boot`, `FrameScheduler::start`, `Application::init`, and UI pump symbols. A few shared control/event utility symbols remain linked, with no frame, render or touch tasks.
- USB host, NAM, effects, model/preset storage, the BOOT mode button and the bounded measurement/OTA recovery services remain. Wi-Fi is off throughout every audio measurement.

## Controls and evidence

CPU 240 MHz, sample rate 48 kHz, 64-frame blocks, NAM split 8, factory model index 0, preset 0, deterministic synthetic input, and the same eight effect masks. The board’s current preset/model differs from the model-1 round-four performance report, so this report uses fresh matched runs and does not subtract those historical numbers.

The audio/USB sources match the retained final source. Cache sizes, PSRAM/XIP settings, flash settings, USB settings and RTOS stack sizes match. The custom Gea target exposes additional unused SPIFFS Kconfig options and drops unused framework HTTP-client/TLS options; those services do not start in the audio-only image. The generated partition paths differ, while the actual partition layout is unchanged.

The initial no-display and minimal-entry images are exploratory artifacts only. The reported audio-only image is `engine-only.bin`; the main table uses `engine-only-{1,2,3}-profile.json` and `final-after-{1,2,3}-profile.json`. The first reference run identified model 0 when the harness expected the previous session’s model 1; it is excluded from these averages.

Artifacts: `build/s3-display-off-20260928/`, including the images, ELF/map files, source snapshots and patch, SDK configuration comparison, symbol audit, all raw profiles, soaks and device logs.

- final ELF SHA-256: `82960d5b20502aab5a9f52fc611135889289a3debc13fe85f98c2bd194899d3c`; binary SHA-256: `6a1f1772ea5fc2ccfee37451fa9ceda4121b4934fa033e086dd00795ee2caf5a`.
- engine-only ELF SHA-256: `3e97a9bbfc7d2bf79e274de9366dc6eb872a9c7e9ba5efe3ec803199f2b30e17`; binary SHA-256: `c507d4446291931b85caf37c35894fbf05f8296eabb4631c8feb378c34a95db6`.

Host tests, source checks, and the normal firmware rebuild pass. All temporary maintained-source and board-alias changes were restored. The audio-only image is retained as an experiment.

The original final firmware was restored and its ELF identity verified, then the device acknowledged `AUDIO MODE` to return to normal playing mode with radios off.
