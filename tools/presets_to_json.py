#!/usr/bin/env python3

"""Check the factory presets and write the file the board is flashed with.

There used to be a container here: magic, a version, a count, a length, and then
128 bytes per preset, packed by this script and unpacked again by
src/native/storage/factory_presets.c. Two descriptions of one thing, in two
languages, which had to be changed together and which nobody could read off the
board -- `esptool read_flash` on the presets partition gave you 16 KB of binary.

So the presets partition now holds assets/presets.json itself. This script does
not pack it; it checks it and copies it. What is flashed is the file in this
repo, the board parses it with preset_json.c, the same parser reads a preset file
off an SD card, and the same writer produces one -- so a preset made on the pedal
and a preset in this repo are the same document and can be moved either way.

What is checked is what the board cannot recover from:

  * every named profile is in the model library, so a preset cannot name a
    capture the board was never given
  * names and profile ids fit the fixed fields a record has in RAM
  * every block is one the firmware has, with no more parameters than it holds
  * the whole document fits COYOPEDAL_PRESET_JSON_MAX, which is the buffer the
    board parses it in, and the partition it is flashed into

    python3 tools/presets_to_json.py assets/presets.json build/factory/presets.json
"""

import argparse
import json
import pathlib
import sys

# src/native/storage/factory_presets.h.
NAME_SIZE = 24
PROFILE_SIZE = 24
PARAMS = 5
AMP = 6
# src/native/storage/preset_json.h.
JSON_MAX = 24576
JSON_VERSION = 3

# coyopedal_fx_block_t's order, which is the order preset_json.c writes them in.
BLOCK_ORDER = ["gate", "compressor", "overdrive", "reverb", "modulation", "delay"]


def fits(text: str, size: int, what: str) -> None:
    encoded = text.encode("utf-8")
    if len(encoded) >= size:
        # Truncated silently would put a wrong name on the panel forever.
        raise SystemExit(f"error: {what} {text!r} needs {len(encoded) + 1} bytes, limit is {size}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("presets", type=pathlib.Path, help="presets.json")
    parser.add_argument("output", type=pathlib.Path, help="the file to flash")
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        help="models.json, to check every named profile is in the library",
    )
    arguments = parser.parse_args()

    source = arguments.presets.read_text()
    document = json.loads(source)
    presets = document.get("presets") or []

    if not presets:
        print("error: no presets to flash", file=sys.stderr)
        return 1
    if document.get("version") != JSON_VERSION:
        print(
            f"error: {arguments.presets} says version {document.get('version')!r};"
            f" the firmware reads {JSON_VERSION}",
            file=sys.stderr,
        )
        return 1

    library = None
    if arguments.manifest is not None:
        library = {entry["id"] for entry in json.loads(arguments.manifest.read_text())}

    for preset in presets:
        fits(preset["name"], NAME_SIZE, "preset name")
        fits(preset["profile"], PROFILE_SIZE, "preset profile")
        if library is not None and preset["profile"] not in library:
            print(
                f"error: preset {preset['name']!r} names profile {preset['profile']!r},"
                " which is not in the library",
                file=sys.stderr,
            )
            return 1
        if len(preset["amp"]) != AMP:
            print(f"error: preset {preset['name']!r} needs {AMP} amp values", file=sys.stderr)
            return 1
        for block in preset["blocks"]:
            if block["block"] not in BLOCK_ORDER:
                print(
                    f"error: preset {preset['name']!r} has no block called {block['block']!r}",
                    file=sys.stderr,
                )
                return 1
            if len(block["params"]) > PARAMS:
                print(
                    f"error: preset {preset['name']!r} gives block {block['block']!r}"
                    f" {len(block['params'])} parameters, and a block holds {PARAMS}",
                    file=sys.stderr,
                )
                return 1

    # Copied verbatim rather than re-serialised: the file in the repo is the file
    # on the board, byte for byte, and a diff between them is a diff of the thing
    # itself rather than of two spellings of it.
    image = source.encode("utf-8")
    if len(image) > JSON_MAX:
        print(
            f"error: {len(image)} bytes of presets, and the board parses them in"
            f" a {JSON_MAX} byte buffer",
            file=sys.stderr,
        )
        return 1

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_bytes(image)
    print(f"checked {len(presets)} presets into {arguments.output.name}, {len(image)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
