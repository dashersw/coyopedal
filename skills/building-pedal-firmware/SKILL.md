---
name: building-pedal-firmware
description: Use when building, flashing, provisioning or OTA-updating the ESP32-S3 AMOLED pedal firmware, or when a firmware build fails, a native source or partition needs adding, or the device image seems stale.
---

# Building the pedal firmware

The firmware is a Gea app. The `gea` CLI (from `@geastack/cli`) owns the entire ESP-IDF
project; this repo only declares its contributions in `package.json` under `gea`.

Install the registry packages with `npm ci`. The committed package set uses
targets 0.1.87, CLI 0.1.89, core 0.1.35, engine 0.1.10 and host 0.1.12, including compact UI storage,
AMOLED display/runtime-stack fixes, LCD speaker support and rotated projected text.
Build without local Gea package or compiler path overrides;
the installed packages are sufficient. If an old CMake cache points to a local
checkout, preserve that target's build directory outside `.gea/` and let the CLI
create a fresh one under `.gea/build/`.

## Board selection

The default build and flash commands select AMOLED 2.06 (`amoled`). Optional
boards use the same manifest with per-target display, power and partition settings:
`npm run build:firmware -- --board lcd-35b` and
`npm run flash:firmware -- --board lcd-35b`. Register the board with `gea setup`
first. AMOLED 2.41 is also supported with its own registered alias.
All boards use installed npm packages, without local checkout overrides.
The image remains `build/pedalboard.bin`.

## Commands

| Task                                   | Command                                          |
| -------------------------------------- | ------------------------------------------------ |
| Build                                  | `npm run build:firmware` (`gea build --board amoled`) |
| Full USB provisioning + reset          | `npm run flash:firmware` (`gea flash --board amoled`) |
| Show images and offsets, write nothing | `npm run flash:firmware -- --dry-run`            |
| Other gea verbs                        | `npx gea --help`                                 |

- **Do not flash hardware unless the user asked for it.** A build is always fine.
- The board alias `amoled` is defined in `.gea/boards.json` (USB serial number and OTA host).
  Always select the board by alias. Never pick or name a device by its `/dev/cu.usbmodem…` path.
- Setup from scratch (README "Building"): `npm i -g @geastack/cli`, `gea setup --esp-idf`,
  `npm ci`, `gea setup` (known board → Waveshare AMOLED 2.06, alias `amoled`), `gea doctor`.

## Where things live

- **Firmware image: `build/pedalboard.bin`.** `gea.targets.esp32.output` makes every build copy
  the app image there (`--output <file>` overrides it). A gea CLI without `output` support
  ignores the field, and the image is then only in the build tree.
- The ESP-IDF build tree is `.gea/build/esp32-s3-touch-amoled-2.06/app-builds/nam-pedalboard/`.
  Don't send people there.
- `build/esp32/` is left over from before the gea CLI. It is stale.
- `gea.targets.esp32` in `package.json` declares the native component directories, IDF
  requirements, link options, embedded files, the **partition table**, `ldFragments`
  (`src/native/gea_memory.lf`), `sdkconfig` (`src/native/sdkconfig.defaults`), the
  `prebuild` step (`scripts/pack-factory-assets.mjs`, which writes `build/factory/*.bin`) and
  the `output` image path.
- `gea.nativeSources` lists every native `.cpp/.c/.S` outside the component directories. A
  new native file compiles only after you add it here.
- `gea.defines` holds the Gea memory and display tuning.

**Never** add a board `CMakeLists.txt`, a partition CSV, a display/touch driver or a second
compiler invocation. `@geastack/targets` provides the board layer. Never edit generated C++
under `.gea/build/`.

## Flash vs OTA

- `gea flash` writes the bootloader, partition table, OTA selector, app, `models` and
  `presets`. It never writes NVS (saved user presets) or `user_models`. It is the only way
  to change the partition table.
- **A board already in maintenance mode is flashed over Wi-Fi.** That is the normal update
  path; don't reach for `gea flash` or a serial port. Find it, then upload:
  `python3 tools/esp32/amoled_remote.py discover`, then
  `python3 tools/esp32/amoled_remote.py --host PEDAL_IP ota`. It uploads `build/pedalboard.bin`.
- After an OTA the board reboots into audio, then goes back to maintenance once.
- A software reset (OTA or mode switch) can leave the XTONE Pro silent until the board is
  power-cycled. `start_streams()` works around this. If it happens anyway, check whether the
  last boot was a soft reset before you debug the DSP.

## Build-time knobs (CMake cache, not defaults to change casually)

- `COYOPEDAL_PEDAL_S3_SPLIT_LAYER` (default **8**, `src/audio/nam/CMakeLists.txt`): the first NAM
  layer that runs on core 1. Split 8 measured better than 9 on this board.
- `COYOPEDAL_PEDAL_BLOCK_FRAMES` (`src/audio/block_frames.cmake`): even, ≤ 64. Production uses 64.

## Common mistakes

- Treating a successful build as proof of audio timing. Only the device can show that (`measuring-pedal-audio`).
- Qualifying a compact-node development build from one debug-info record. Run
  `tools/esp32/check_ui_layout.py <unstripped-firmware.elf>` with ESP-IDF's Python;
  it checks record sizes and member offsets across all linked translation units.
  An incremental development image once linked both 176-byte and 180-byte Nodes,
  producing a black framebuffer and misleadingly large free-heap gains. Preserve
  the rejected image, clean the native build with Ninja, rebuild through Gea,
  then repeat the layout check and device pixel census before measuring savings.
- Using old README or doc commands (`npm run flash:gea`, `--split-layer`, `--app-only`).
  Those came from the old hand-written IDF wrapper.
