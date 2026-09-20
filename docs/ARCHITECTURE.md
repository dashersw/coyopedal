# Architecture

This document describes how the pedal works: the hardware, the signal chain, how
the DSP is divided between the two cores, how the amp engine runs on the
ESP32-S3, and how storage, the UI and the two operating modes fit around it.
Memory placement is covered in [MEMORY.md](MEMORY.md), and the USB transport in
[USB_AUDIO.md](USB_AUDIO.md).

## Hardware

| Part      | Details                                                                                                |
| --------- | ------------------------------------------------------------------------------------------------------ |
| Board     | Waveshare ESP32-S3-Touch-AMOLED-2.06                                                                   |
| SoC       | ESP32-S3R8: two Xtensa LX7 cores at 240 MHz, 512 KiB internal SRAM, 8 MB octal PSRAM                   |
| Flash     | 32 MB GD25Q256, QIO                                                                                    |
| Display   | 410 x 502 AMOLED over QSPI (driven in landscape as 502 x 410), FT3168 touch                            |
| Buttons   | PWR and BOOT (GPIO0). BOOT: tap toggles bypass; hold 1.5 s and release switches audio/maintenance mode |
| Storage   | Optional microSD card (1-bit SDMMC) for your own captures                                              |
| Audio I/O | A class-compliant USB audio interface on the board's USB-C port, which runs as USB host                |

The ESP32-S3 has a USB 1.1 (Full Speed) controller, so every interface runs at
Full Speed: one packet per millisecond, 48 frames per packet at 48 kHz. The
board has no analog audio path of its own.

## Signal chain

Audio is mono from the guitar input to the reverb, and stereo after it. The
order is fixed:

```
USB capture (channel 0)
  -> [tuner: feeds the pitch detector and mutes the output]
  -> gate -> compressor -> modulation -> overdrive
  -> NAM amp (A2-Full, 23 layers + head)
  -> modulation 2 -> delay -> reverb (stereo)
  -> PCM pack -> USB playback (left/right)
```

The master bypass passes the input straight to the output, gains included. The
amp block has its own switch and can be off while the effects run. Effects are
identified by a 7-bit mask in storage order (`coyopedal_fx_block_t` in
`src/audio/effects.h`): gate 1, compressor 2, overdrive 4, reverb 8,
modulation 16, delay 32, modulation 2 64. The storage order is not the signal
order.

| Block        | Notes                                                                            |
| ------------ | -------------------------------------------------------------------------------- |
| Gate         | Noise gate ahead of everything else                                              |
| Compressor   | Threshold, ratio, attack, release and makeup gain                                |
| Modulation   | Pre-amp slot, where a phaser or vibe sits on a real board                        |
| Overdrive    | Drive in front of the amp                                                        |
| Amp          | NAM A2-Full capture, plus a three-band tone stack and input/output gain          |
| Modulation 2 | Effects-loop slot, ahead of delay and reverb                                     |
| Delay        | Up to two seconds, float line in PSRAM                                           |
| Reverb       | Compact two-tank reverb with Q15 delay lines; the block that makes output stereo |

All effect code lives in `src/audio/effects.cpp`. The per-block entry points are
placed in IRAM so they never wait on the flash or PSRAM caches.

## The two-core pipeline

The DSP runs in 64-frame blocks at 48 kHz. One block is 1.333 ms, which is
320,000 CPU cycles at 240 MHz. That is the deadline for each stage on each
core, and a stage that overruns it drops a block, which is heard as a click.

The amp alone needs close to two cores' worth of that budget, so the work is
split across both cores as a pipeline (`src/native/drivers/usb_audio.cpp`):

| Stage   | Task    | Core | Priority | Work                                                                        |
| ------- | ------- | ---- | -------- | --------------------------------------------------------------------------- |
| USB     | client  | 0    | 20       | Isochronous completions: decode capture, fill playback from the output ring |
| Stage A | `nam_a` | 0    | 19       | Pull a block from the input ring, tuner or pre-amp effects, amp layers 0-7  |
| Stage B | `nam_b` | 1    | 22       | Amp layers 8-22 and the head, modulation 2, delay                           |
| Return  | `nam_a` | 0    | 19       | Reverb (when enabled) and PCM packing into the output ring                  |

The split layer is 8: layers 0-7 run on core 0 and layers 8-22 on core 1. It is
a build setting (`COYOPEDAL_PEDAL_S3_SPLIT_LAYER`, default in
`src/audio/nam/CMakeLists.txt`). Core 0 also carries the USB client, whose
completion callbacks cost a noticeable share of each block and must preempt
stage A: if stage A outranks the client, isochronous transfers are resubmitted
after their slot and the transport inserts silence. Split 8 balances that load:
it moves one more amp layer to core 1 and returns the reverb to core 0.

When the reverb is off, stage B packs the block itself and the return hop is
skipped. Blocks travel between stages through lock-free single-producer queues.
There are three pipeline slots and two amp scratch buffers, so up to two blocks
can be inside the amp at once and a third provides elasticity against the
48-frame USB packet and 64-frame block cadence.

Both idle tasks are removed from the task watchdog in audio mode. The stages
leave the idle tasks very little time, and a watchdog report printed from
interrupt context costs a block. Maintenance mode keeps the watchdog.

### Latency

Every latency difference the pipeline introduces is a whole number of 64-frame
blocks, because blocks move between stages once per scheduling pass. The main
contributors to the round trip, in order of size:

- **Pipeline depth.** Each stage hop is 1.33 ms. The amp adds several hops,
  more when both cores run close to their deadline. Delay and reverb add one
  more hop, for the return to core 0.
- **Full Speed packetization.** Each side of the interface holds at least one
  whole 1 ms packet. A High Speed host would move much smaller packets, but the
  ESP32-S3 cannot enumerate at High Speed.
- **S3 rings.** The input ring is held near one block plus one packet, and the
  UAC2 output ring near two packets plus one block. Without a feedback
  endpoint, the output ring depth is the only thing that absorbs DSP
  completion jitter.
- **The interface's converters**, typically around 1 ms.

In practice the round trip on a Full Speed interface is in the high single
digits of milliseconds with light effects and under about 20 ms with the amp and
the whole chain engaged. The buffers are bounded and are not the main lever;
pipeline depth is.

## The amp engine

The amp runs Neural Amp Modeler "A2-Full" captures: a WaveNet with 23 dilated
convolution layers of 8 channels plus a 16-tap head, 12,146 weights. On the S3
it runs in an engine built for the LX7 (`A2FullS3Native` in
`src/audio/nam/nam_a2_full_s3_native.hpp` and `.cpp`).

- **Block floating point.** Values are int16 (with narrower int8 limbs on the
  layers that allow it), and each layer's histories, weights and activations
  share their own exponent instead of one fixed Q15 binary point.
- **Calibration.** A model loaded from plain float weights is run over a
  deterministic calibration sweep at load time, and the per-layer scales and
  shifts are derived from the measured peaks with fixed headroom. This is the
  "loaded and calibrated" step in the boot log.
- **Tuned precision.** A model can also carry a prepared trailer (49,748 bytes
  instead of 48,616) holding precision parameters found by a search against the
  float reference, with a target error of -60 dBFS on a validation corpus.
  Imported and SD models are tuned on the device the first time they are
  loaded; the result is stored so the search runs once. A worker task on core
  1 (`nam_tune_b`) shares the search with the loading task, with the pipeline
  paused.
- **Native kernels.** The convolutions, activations, residual updates,
  quantizers and the head are hand-written LX7 assembly using the S3's
  multiply-accumulate instructions (`src/audio/nam/s3_*.S`). Each entry point is
  in its own section so unused variants are dropped at link time, and the hot
  ones are placed in IRAM.
- **Memory.** All 23 history rings and both coefficient tables are in internal
  SRAM, with the two tables and the largest core 0 history each starting on a
  32 KiB SRAM bank ([MEMORY.md](MEMORY.md)). Models are swapped with the
  pipeline drained, reusing the same fixed-shape storage.

## Modes

The pedal has two modes, and switching between them is always a reboot.

| Mode        | Running                                               | Radios |
| ----------- | ----------------------------------------------------- | ------ |
| Audio       | Amp, effects, USB host, UI                            | Off    |
| Maintenance | Wi-Fi, BLE discovery, authenticated HTTP API, OTA, UI | On     |

**Audio is the default boot.** Maintenance is entered from the UI (Setup,
then Maintenance mode) or with the BOOT button, and both write a one-shot flag
and restart. A boot that follows a panic, a watchdog reset or a brownout goes
to maintenance instead, so a crash in audio mode cannot lock the board out of
the network. The boot decision is in `gea_app_native_boot()` in
`src/native/main/main.cpp`; the flags are consumed at the start of the boot.

Mode switches reboot because neither heap can be reshaped in place: the radios
leave internal SRAM fragmented, and the amp graph needs large contiguous
blocks. A fresh boot places both reliably.

**OTA** runs in maintenance mode over the authenticated HTTP API. After the
image is validated, the board reboots into audio mode, so the new image
completes a full audio startup and is marked valid (rollback is enabled). It
then returns to maintenance on its own if the update was sent from
maintenance. `REBOOT` follows the same route. A maintenance boot that cannot
bring up its network reboots itself after 30 seconds.

## Storage

The partition table is declared in `package.json` under
`gea.targets.esp32.partitions`:

| Partition     | Size    | Contents                                             |
| ------------- | ------- | ---------------------------------------------------- |
| `nvs`         | 24 KiB  | Settings, mode flags, user presets (`panel_presets`) |
| `otadata`     | 8 KiB   | OTA slot selection                                   |
| `phy_init`    | 4 KiB   | RF calibration                                       |
| `ota_0`       | 8 MiB   | Application slot                                     |
| `ota_1`       | 8 MiB   | Application slot                                     |
| `models`      | 256 KiB | Factory model library                                |
| `presets`     | 64 KiB  | Factory presets                                      |
| `user_models` | 512 KiB | Models imported over HTTP, in fixed 52 KiB slots     |
| `storage`     | ~15 MiB | SPIFFS, mounted by the Gea runtime at boot           |

**Factory content.** `assets/models/factory.json` lists the factory captures
and `assets/presets.json` the factory presets. The prebuild step
(`scripts/pack-factory-assets.mjs`) packs the library with
`tools/models_to_bin.py`; the presets are not packed at all —
`tools/presets_to_json.py` checks `assets/presets.json` and copies it, so what is
in the `presets` partition is that file, which `src/native/storage/preset_json.c`
parses at boot. Both are written to their partitions and also embedded in the app
image as a fallback, along with one fallback model. A preset is the whole chain:
the capture, the amp's gains and every effect's state and parameters. User
presets are stored in NVS as a struct — 24 KiB of text will not fit in a 24 KiB
NVS partition twice during a write — and mirrored onto the card as
`coyopedal-presets.json`, the same document the factory list is.

**Imported models.** `POST /v1/models/import` takes a model, tunes it on the
device and writes it to its own slot in `user_models`. Slots are committed
independently, so a power cut during a write cannot damage existing imports.

**SD card models.** At boot, `/nam` on the card and its folders (six levels
deep) are scanned for `.nam` (JSON, up to 2 MiB) and `.namb` files
(`src/native/storage/sd_models.cpp`). A capture is named after its file stem
and identified by a CRC of its path, and the browser shows the card's own
folder tree; nothing on the card needs a catalogue.
A `.nam` file is parsed and tuned when it is first selected, and the prepared
result is written next to it as `<file>.s3cache`. The cache is used only if it
matches the source model byte for byte and validates, so a changed file is
tuned again. Loading and tuning happen with the audio pipeline paused, and the
current model keeps playing if either fails.

## UI

The UI is a Gea app written in TSX under `src/ui/`. The same components run in
the browser preview (`npm run dev`) and on the device. For the firmware, the gea
CLI compiles the app to native C++, with no JavaScript VM on the board, and
`@geastack/targets` provides the runtime, layout, renderer, display driver and
touch.

- `src/ui/board.ts` is the typed boundary to the pedal. On the device its calls
  are host functions (`pbGet`, `pbSet`, `pbLabel`, `pbAction`, `pbPresetName`)
  implemented in `src/native/ui/board_bridge.cpp` and wired up by
  `scripts/panel-host-plugin.mjs`.
- `src/native/ui/controls.c` holds the control state the UI edits and applies it
  to the engine. `src/native/ui/task.cpp` runs the tuner and preset-save pumps.
- The render loop runs in its own task at priority 18, below both DSP stages,
  with its stack in PSRAM, at up to 15 frames per second.
- Gea state, caches and the render, frame and touch task stacks are kept out
  of internal SRAM by the `GEA_EMBEDDED_*_EXTERNAL` switches in `gea.defines`
  in `package.json`, where the cache sizes are also reduced;
  `src/native/gea_memory.lf` adds the board's own placements.
