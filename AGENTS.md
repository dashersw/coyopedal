# Agent notes

Instructions for AI coding agents working in this repository. People should
start with the [README](README.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

## Firmware

The firmware is a Gea app, and the `gea` CLI owns the whole ESP32 build.
`npm run build:firmware` (`gea build --board amoled`) builds it, and
`npm run flash:firmware` (`gea flash --board amoled`) flashes it over USB.

**A board already in maintenance mode is flashed over Wi-Fi, not USB.** That is
the normal way to update it, and it is two commands:

```bash
python3 tools/esp32/amoled_remote.py discover
python3 tools/esp32/amoled_remote.py --host PEDAL_IP ota
```

`discover` prints the address; `ota` uploads `build/pedalboard.bin`, validates
it and reboots. Don't reach for `gea flash` or a serial port for this. Don't
flash hardware unless asked. The board layer (panel driver, touch, power rails, frame
scheduler and the IDF project) comes from `@geastack/targets`, so never add a
board `CMakeLists.txt`, a partition CSV or a display driver here. Everything
this repository adds to the native build is declared in `gea.targets.esp32` in
`package.json`.

Read [docs/MEMORY.md](docs/MEMORY.md) before changing how the NAM engine or the
effects allocate memory, where code is placed, or the linker and sdkconfig
settings. Host tests and a clean build don't prove that audio timing is right on
the device.

## Frontend and models

The UI starts in `src/ui/index.tsx`, with JSX components, one CSS file per
component module, and `src/ui/stores/PedalboardStore.ts`. The browser preview
and the ESP32 build the same app; `src/preview/` holds browser-only fixtures.
The gea CLI compiles the app with the pinned `@geastack/compiler`. Don't edit
generated C++, don't add a JavaScript runtime, and don't add a second compiler
invocation. `scripts/panel-host-plugin.mjs` declares the board boundary. See
[src/ui/README.md](src/ui/README.md) for what this board asks of Gea, and why
it carries no local patches.

Users copy original `.nam` files to the SD card, and parsing and preparation
happen on the device. Never require a host-side conversion. Keep the source
files untouched and validate caches against the source model.

## Checks

Run `npm run check` after source changes, and `npm test` plus
`npm run build:firmware` after changes to audio, storage, USB or UI.
`npm run format` formats maintained sources; leave generated output,
credentials, downloaded components and `third_party/` alone.

## Skills

Task-specific instructions live in `skills/`. Read
[skills/README.md](skills/README.md) and load the matching `SKILL.md` before
building, flashing, measuring audio, changing memory placement, editing the UI,
editing factory assets or updating Gea packages.
