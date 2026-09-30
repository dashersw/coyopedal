#!/usr/bin/env python3
"""Build the original Balls JSX workload for the registered USB 1.8 board."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "build/css-feature-audit"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--shared", type=int, choices=(0, 1), required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument(
        "--installed",
        action="store_true",
        help="Use installed npm packages with no local package overrides",
    )
    args = parser.parse_args()
    if Path(args.label).name != args.label:
        parser.error("label must be a filename prefix")
    inputs = json.loads((AUDIT / "pedal-241-current-inline-build-inputs.json").read_text())
    geastack = Path(inputs["overrides"]["GEA_COMPILER_DIR"]).parent
    examples = geastack / "examples"
    manifest = examples / "apps/bouncing-balls-jsx/package.json"
    original = manifest.read_bytes()
    package = json.loads(original)
    package["gea"]["defines"] = {
        **package["gea"].get("defines", {}),
        "GEA_EMBEDDED_FRAME_BENCHMARK": 2,
        "GEA_EMBEDDED_FRAME_SCHEDULER_FPS_LOG": 0,
        "GEA_EMBEDDED_SHARED_STYLES": args.shared,
    }
    env = os.environ.copy()
    overrides = {} if args.installed else inputs["overrides"]
    if args.installed:
        env = {key: value for key, value in env.items() if not key.startswith(("GEA_", "GEATSC"))}
    env.update(overrides)
    env.update(CCACHE_DISABLE="1", TMPDIR=str(AUDIT), GEA_IDF_JOBS="2")
    command = [
        "node",
        str(
            examples / "node_modules/@geastack/cli/bin/gea.mjs"
            if args.installed
            else geastack / "cli/bin/gea.mjs"
        ),
        "build",
        "--board",
        "amoled-18",
        "--app",
        "bouncing-balls-jsx",
        "--output",
        str(AUDIT / (args.label + ".bin")),
    ]
    metadata = {
        "command": command,
        "shared": args.shared,
        "board": "amoled-18",
        "target": "esp32-s3-touch-amoled-1.8",
        "usbSerial": "30:ED:A0:AC:90:DC",
        "wifiEnabled": False,
        "originalManifestSha256": hashlib.sha256(original).hexdigest(),
        "overrides": overrides,
        "examplesRoot": str(examples),
        "cli": command[1],
        "nativeBuild": str(
            (
                examples / ".gea/build"
                if args.installed
                else Path(overrides["GEA_PROJECT_BUILD_ROOT"])
            )
            / "esp32-s3-touch-amoled-1.8/app-builds/bouncing-balls-jsx"
        ),
        "packageRoots": {
            key: str(examples / "node_modules/@geastack" / name)
            for key, name in {
                "GEA_CORE_DIR": "core",
                "GEA_ENGINE_DIR": "engine",
                "GEA_HOST_DIR": "host",
                "GEA_ELEMENTS_DIR": "elements",
                "GEA_TARGETS_ROOT": "targets",
            }.items()
        }
        if args.installed
        else overrides,
        "defines": package["gea"]["defines"],
    }
    (AUDIT / (args.label + "-build-inputs.json")).write_text(json.dumps(metadata, indent=2) + "\n")
    try:
        manifest.write_text(json.dumps(package, indent=2) + "\n")
        with (AUDIT / (args.label + "-build.log")).open("w") as log:
            result = subprocess.run(
                command, cwd=examples, env=env, stdout=log, stderr=subprocess.STDOUT
            )
    finally:
        manifest.write_bytes(original)
    print(f"Gea build exit: {result.returncode}")
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
