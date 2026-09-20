#!/usr/bin/env python3
"""Download the VoLum amp captures onto an SD card.

VoLum (https://github.com/guitarlum/VoLum, MIT License) publishes its captures
as .nam files under rigs/<amp>/ in its repository. This copies them, keeping
that layout, to <card>/nam/VoLum/, which the pedal's browser shows as folders.
Rearrange them afterwards however you like; the pedal reads whatever tree is
under /nam.

    python3 tools/fetch_volum.py /Volumes/SDCARD

Files that are already present with the right size are skipped, so an
interrupted run can be repeated.
"""

import argparse
import json
import pathlib
import sys
import urllib.parse
import urllib.request

REPOSITORY = "guitarlum/VoLum"
LICENSE_NOTICE = """\
These captures are from VoLum by Lum, https://github.com/{repository} ({tag}),
distributed under the MIT License:
https://github.com/{repository}/blob/{tag}/LICENSE
"""


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "nam-pedalboard"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def latest_tag() -> str:
    release = json.loads(fetch(f"https://api.github.com/repos/{REPOSITORY}/releases/latest"))
    return release["tag_name"]


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("card", type=pathlib.Path, help="the SD card's mount point")
    parser.add_argument("--tag", help="VoLum release tag (default: the latest release)")
    arguments = parser.parse_args()

    if not arguments.card.is_dir():
        print(f"error: {arguments.card} is not a directory", file=sys.stderr)
        return 1
    tag = arguments.tag or latest_tag()
    tree = json.loads(
        fetch(f"https://api.github.com/repos/{REPOSITORY}/git/trees/{tag}?recursive=1")
    )
    captures = [
        entry
        for entry in tree["tree"]
        if entry["type"] == "blob"
        and entry["path"].startswith("rigs/")
        and entry["path"].endswith(".nam")
    ]
    if not captures:
        print(f"error: no captures found in {REPOSITORY} at {tag}", file=sys.stderr)
        return 1

    destination = arguments.card / "nam" / "VoLum"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "LICENSE.txt").write_text(LICENSE_NOTICE.format(repository=REPOSITORY, tag=tag))
    print(f"VoLum {tag}: {len(captures)} captures -> {destination}")
    for index, entry in enumerate(captures, 1):
        relative = pathlib.PurePosixPath(entry["path"]).relative_to("rigs")
        target = destination.joinpath(*relative.parts)
        if target.is_file() and target.stat().st_size == entry["size"]:
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        url = f"https://raw.githubusercontent.com/{REPOSITORY}/{tag}/{urllib.parse.quote(entry['path'])}"
        data = fetch(url)
        partial = target.with_name(target.name + ".part")
        partial.write_bytes(data)
        partial.replace(target)
        print(f"  [{index}/{len(captures)}] {relative}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
