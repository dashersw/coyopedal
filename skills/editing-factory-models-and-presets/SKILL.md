---
name: editing-factory-models-and-presets
description: Use when adding or changing factory amp models, the capture library catalogue, starter presets, effect block parameters, or anything that ends up in the models/presets flash partitions or NVS preset storage.
---

# Factory models and presets

## Sources and packing

| File                         | Content                                                                 |
| ---------------------------- | ----------------------------------------------------------------------- |
| `assets/models/factory.json` | Models packed into the `models` partition (`required: true` must exist) |
| `assets/models/*.namb`       | Prepared A2-Full captures referenced by `factory.json`                  |
| `assets/presets.json`        | Starter presets, flashed verbatim into the `presets` partition          |

The build's `prebuild` step, `scripts/pack-factory-assets.mjs`, runs `tools/models_to_bin.py` and
`tools/presets_to_json.py` into `build/factory/`. The results are flashed to their partitions and
also embedded in the app, so an OTA replaces the factory list. Run the script by hand to check
packing: `node scripts/pack-factory-assets.mjs`.

The presets are not packed into anything: `presets_to_json.py` checks the file and copies it, so
the partition holds `assets/presets.json` byte for byte and `esptool read_flash` on it gives back
something you can read. The board parses it with `src/native/storage/preset_json.c`, which is also
what reads a preset file off a card and what writes one — so a list saved on the pedal, a list in
this repo and a list in a folder in the browser are the same document.

## Invariants

- **Never renumber `coyopedal_fx_block_t`** (`src/audio/effects.h`). Stored presets use its order:
  gate, compressor, overdrive, reverb, modulation, delay. Chain order is set in
  `coyopedal_fx_process_*`, not by the enum.
- A preset is a name, a profile id, six amp values in tenths of a dB, whether the amp is engaged,
  and six blocks of an enabled flag and up to five int16 params. The document is described at the
  top of `src/native/storage/preset_json.h`, and `tests/test_preset_document.py` pins the field
  widths, the block names and their order across the checker, the parser and the enum. Changing
  the shape means bumping `COYOPEDAL_PRESET_JSON_VERSION` and updating the reader and writer in
  `src/native/storage/preset_json.c`.
- A preset's `profile` must name a model the packer knows about. Categories must be known ones.
- Up to 256 catalogue entries and 32 saved presets. Saved user presets live in NVS, which
  `gea flash` never overwrites, so test on a board with existing presets too.
- One pedal per block, so presets carry no algorithm choice.
- **Users load original `.nam` files from SD.** Parsing, validation and cache preparation happen
  on the device (`src/native/storage/`, `src/audio/nam/`). Never add a host-side conversion step
  the user would need. Keep the source file, and check `.s3cache` contents against it.
- Only 48 kHz, eight-channel A2-Full WaveNet models are supported, including a matching member
  of a `SlimmableContainer`.

## Verify

Run `npm test` (the parser round trip, the prepared-model test against the factory `.namb`, and the
container tests), then `npm run build:firmware`. Clean-sounding presets can only be confirmed by
the user listening on the device.
