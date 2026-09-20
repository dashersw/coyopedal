#!/usr/bin/env python3
"""Pack the factory captures into one binary image.

The image is embedded in the application (gea.targets.esp32.embedFiles) and
flashed into the `models` data partition, and the firmware reads entries out of
it on demand. It is self-describing, because the firmware has no other way to
know what it was given:

    offset  size  field
    0       4     magic "TMDL"
    4       2     format version, currently 1
    6       2     entry count
    8       4     total image length, header included
    12      4     reserved, zero
    16      n*64  entries: payload offset (u32), payload length (u32),
                  id (24 bytes) and display name (32 bytes), both NUL padded
    ...           payloads, each aligned to 4 bytes

scripts/pack-factory-assets.mjs runs this during `npx gea build`, writing
build/factory/models.bin. By hand:

    python3 tools/models_to_bin.py assets/models/factory.json . build/factory/models.bin

Entries whose file is missing are skipped with a warning rather than failing,
unless the entry is marked required.
"""

import argparse
import json
import pathlib
import struct
import sys

MAGIC = b"TMDL"
VERSION = 1
HEADER_SIZE = 16
ENTRY_SIZE = 64
ID_SIZE = 24
NAME_SIZE = 32


def fixed(text: str, size: int, what: str) -> bytes:
    encoded = text.encode("utf-8")
    if len(encoded) >= size:
        # Truncated silently would put a wrong name on the panel forever.
        raise SystemExit(f"error: {what} {text!r} needs {len(encoded) + 1} bytes, limit is {size}")
    return encoded + bytes(size - len(encoded))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=pathlib.Path, help="factory.json")
    parser.add_argument("root", type=pathlib.Path, help="repository root the paths are relative to")
    parser.add_argument("output", type=pathlib.Path, help="output .bin file")
    arguments = parser.parse_args()

    entries = []
    for entry in json.loads(arguments.manifest.read_text()):
        path = arguments.root / entry["file"]
        if not path.is_file():
            if entry.get("required"):
                print(f"error: required model {path} is missing", file=sys.stderr)
                return 1
            print(f"note: skipping absent model {entry['file']}", file=sys.stderr)
            continue
        entries.append((entry["id"], entry["name"], path.read_bytes()))

    if not entries:
        print("error: no models to pack", file=sys.stderr)
        return 1

    payload_start = HEADER_SIZE + len(entries) * ENTRY_SIZE
    table = bytearray()
    payloads = bytearray()
    for identifier, name, data in entries:
        table += struct.pack("<II", payload_start + len(payloads), len(data))
        table += fixed(identifier, ID_SIZE, "id")
        table += fixed(name, NAME_SIZE, "name")
        payloads += data
        # Keep every payload 4-byte aligned; the loader reads them as words.
        while len(payloads) % 4:
            payloads += b"\0"

    total = payload_start + len(payloads)
    image = struct.pack("<4sHHII", MAGIC, VERSION, len(entries), total, 0) + table + payloads
    assert len(image) == total, (len(image), total)

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_bytes(image)
    print(f"packed {len(entries)} profiles into {arguments.output.name}, {total} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
