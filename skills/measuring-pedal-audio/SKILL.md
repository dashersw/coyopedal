---
name: measuring-pedal-audio
description: Use when the pedal is silent, crackles, drops audio or misses deadlines, when CPU/latency numbers are needed, or when talking to the board over Wi-Fi (maintenance mode, logs, remote commands, heap maps).
---

# Measuring audio on the device

**Core rule:** diagnose from a fresh on-device measurement. Never reason from cycle or byte
counts remembered from older builds, including the approximate figures in `docs/`. Placement
changes move them, and stale numbers have already led to wrong conclusions.

## Reaching the board

- The radios are **off** in audio mode. Wi-Fi and BLE run only in **maintenance mode**. To enter
  it, tap the preset name, then Setup, then Maintenance mode. The BOOT button also works.
- Credentials live in `src/native/services/remote_config.h` (gitignored). Create them with
  `python3 tools/esp32/configure_remote.py`.
- Remote tool: `python3 tools/esp32/amoled_remote.py [--host IP] <verb>`. The main verbs are
  `discover`, `status`, `logs [--follow]`, `command <TEXT>`, `ota <image>`, `audio-mode`,
  `live-status`, `effects-profile`. `command HELP` lists every device command.
- The usual host is in `.gea/boards.json` (`transports.ota.host`). AP mode uses `192.168.4.1`.

## The measurement loop

1. From maintenance, run `command AUDIO TRY <5..120> [ENGAGE] [MASK <n>] [MODEL <idx>]`. The
   board boots into real audio for that many seconds, with radios off, then returns to
   maintenance. A timer enforces the return even if audio fails to start.
2. Wait for the board to come back, then run `logs` to read the reboot-surviving log ring.
3. Read the `audio:` heartbeat lines. `audio_heartbeat_task` in `src/native/drivers/usb_audio.cpp`
   writes one every 5 s in every build:

```
audio: cap=+240024 play=+240024 silence=+0 rings=22/122 err=0 paused=0 bypass=0 amp=1
       in=0.04937 out=0.37613 ch=0.04937/0.00187
       blk=3750 C0=412/998us 31% C1=506/1120us 38% budget=1333us miss=0/0
```

`MASK` is a bitmask in `coyopedal_fx_block_t` order. This is the storage order, not the order the
chain runs in: gate=1, compressor=2, overdrive=4, reverb=8, modulation=16, delay=32.
For example, gate + compressor + reverb is `MASK 11`.

## Reading it

| Symptom                                            | Meaning                                                                        |
| -------------------------------------------------- | ------------------------------------------------------------------------------ |
| `miss=a/b` non-zero                                | **Crackle.** Stage A (core 0) or stage B (core 1) missed its 1,333 µs deadline |
| `cap` flat                                         | USB capture is dead                                                            |
| `cap` moving, `play == silence`                    | The DSP published nothing                                                      |
| `paused=1` stuck                                   | `model_load_pause` is stuck, so bypass is mute too                             |
| `in` at the noise floor                            | Nobody is playing                                                              |
| Full stream, real `out` amplitude, still inaudible | Stale UAC2 alt setting after a soft reset. Power-cycle and compare             |

`bypass` is the footswitch. `amp` is the amp block's own switch.

## Other instruments

- `HEAP MAP` prints the maintenance heap census (format in `docs/MEMORY.md`, "How to
  measure"); the audio-boot census is in `logs`.
- `effects-profile --seconds N --input synthetic|live` measures each effect alone, then the full chain.

## Settled facts: don't re-test

The USB client task (prio 20) must preempt stage A. Split 8 beats split 9. Hot per-block code
belongs in IRAM. RTC fast memory is slow for hot buffers. `NOUI`, `CONFIG_ESP_PHY_IRAM_OPT`,
`run_layer` placement, model choice, SRAM bank layout and a heap-placed graph were all dead
ends. `docs/MEMORY.md` ("Things that were tried and did not help") summarizes why. The
interface on the bench is an **XTONE Pro** (UAC2, 24-bit packed stereo, no feedback endpoint),
not an iRig.

## Common mistakes

- Calling audio clean because the counters are clean. Only the user's ears can confirm that.
- Opening a USB serial monitor during a window. It can toggle reset or BOOT and invalidate the run.
- Measuring again just to confirm a result you already have. Measure only when the number changes what you do next.
