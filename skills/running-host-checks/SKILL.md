---
name: running-host-checks
description: Use when verifying a change on the host before committing — running tests, lint, TypeScript checks or formatters — or when a single native, Python or Node test needs to run on its own.
---

# Host checks

| Command                  | What it covers                                                                  |
| ------------------------ | ------------------------------------------------------------------------------- |
| `npm test`               | `scripts/test.sh`: every native, Node and Python host test listed below         |
| `npm run check`          | ESLint, both TypeScript projects (`src/ui`, `src/preview`), formatting check    |
| `npm run check:frontend` | TypeScript only                                                                 |
| `npm run format`         | Prettier, clang-format 22.1.8, Ruff and cmake-format on maintained sources      |
| `npm run format:setup`   | One-time: installs the pinned Python/CMake formatters into `build/format-tools` |

Run `npm run check` after source maintenance. Run `npm test` and `npm run build:firmware` after
changes to audio, storage, USB or UI.

## Run only the test your change can reach

`scripts/test.sh` runs these checks in order. To run one, copy its line from the script:

- USB descriptor parsing (ASan/UBSan): `tests/usb/uac2_test.cpp`, `tests/usb/xtone_test.cpp` with the `.bin` fixtures
- Effects and presets: `tests/effects_test.cpp`, `tests/effects_lifecycle_test.cpp`, `tests/panel_presets_test.cpp`
- NAM JSON parser: `tests/nam_parser_cli.cpp` + `third_party/cjson`; Python side `tests/test_nam_json.py`
- Prepared model / S3 reference kernels: `tests/nam/prepared_model_test.cpp` against `assets/models/volum-ampete-4-v30.namb`
- Bank allocator: `bash scripts/test-bank-allocator.sh`
- UI store and preview: `node --test tests/gea-store.test.mjs` (also `preset-preview`, `amp-browser`)
- Python checks (asset containers, SRAM budget, panic record, UI stack): `python3 -m unittest tests.test_<name>`

Binaries go to the ignored `build/`. Don't write test output anywhere else.

## Formatting scope

The formatter picks tracked, unignored files through Git. Leave generated output, `node_modules`,
downloaded IDF components, credentials (`src/native/services/remote_config.h`), board mappings
and the vendored `third_party/` sources alone. Xtensa `.S` files
get only whitespace cleanup.

## What the host checks do not prove

A passing host run says nothing about device frame rate, PSRAM or SRAM headroom, real-time
deadlines or clean audio. The XTONE fixture was captured at high speed on a Mac, and it does not
stand in for what the S3 sees at full speed. Say so when you report results, and use
`measuring-pedal-audio` for anything timing-related.
