---
name: building-pedal-firmware
description: Use when building, flashing, provisioning or OTA-updating the ESP32-S3 AMOLED pedal firmware, or when a firmware build fails, a native source or partition needs adding, or the device image seems stale.
---

# Building the pedal firmware

The firmware is a Gea app. The `gea` CLI (from `@geastack/cli`) owns the entire ESP-IDF
project; this repo only declares its contributions in `package.json` under `gea`.

Install the registry packages with `npm ci`. The release-qualified package set uses
targets 0.1.84, core 0.1.30 and engine 0.1.8, including compact UI storage and the
AMOLED display/runtime-stack fixes. Build without local Gea package or compiler path overrides;
the installed packages are sufficient. If an old CMake cache points to a local
checkout, preserve that target's build directory outside `.gea/` and let the CLI
create a fresh one under `.gea/build/`.

## Current connected-board configuration

The npm firmware scripts currently select `amoled-241`, the registered
ESP32-S3-Touch-AMOLED-2.41. Its target-specific partition table fits 16 MB flash.
The app manifest uses a 600 × 450 landscape canvas over the native 450 × 600
panel, a CSS pixel ratio of 2, and disables the 2.06 board's AXP2101 setup.
SD pins come from the 2.41 target. These display/power settings are app-wide:
restore the 2.06 settings before building for `amoled`. Published Gea targets 0.1.83 fixes
the shared target's dimension defaults so explicit app dimensions take
precedence without duplicate macros. Display and touch share landscape-primary
orientation; rotated RM69080 transfers reissue a gapped panel window for each
even-row chunk instead of relying on RAMWRC continuation.
The 2.41 runtime uses the shared-runtime PSRAM-stack fix in targets 0.1.83.
Published targets through 0.1.82 ignore the app's runtime stack
settings on this board: a 4 KiB main stack overflows, while enlarging it to
20 KiB prevents NAM and Wi-Fi allocations. Keep the 4 KiB boot stack and move
the event loop to PSRAM. Run `npm ci` to install the versions in the lockfile,
then use the normal npm build and flash commands. Do not set `GEA_TARGETS_ROOT`
or other package-path overrides to a local Gea checkout.
The same release also keeps RM69080 display transfers at an even row count
with a two-row minimum. Audio-mode RAM pressure otherwise selects one-row
transfers that violate the panel's address-window alignment despite clean
initialization and a non-black framebuffer.
Keep `GEA_EMBEDDED_DISPLAY_INTERNAL_RESERVE_BYTES=0`: native boot already owns
and releases a 4 KiB display reserve while placing NAM. A second 13 KiB
runtime reserve prevents the model's history buffers from fitting.

On this Mac, use `/usr/bin/python3` for the remote tool if the default Python
reports `No route to host` while curl can reach the board.

Build with `npm run build:firmware` and provision over USB with
`npm run flash:firmware`. The image remains `build/pedalboard.bin`.
The original 2.06 setup and commands below describe the `amoled` alias.

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
