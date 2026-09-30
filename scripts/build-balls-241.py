#!/usr/bin/env python3
"""Build the original Balls JSX app for the Wi-Fi-only 2.41 experiment.

Uses the existing current-Geastack comparison configuration. Restores the app
manifest and its ignored Wi-Fi settings; never flashes or changes partitions.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "build/css-feature-audit"
sys.path.insert(0, str(ROOT / "tools/esp32"))
from amoled_remote import Config


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--shared", type=int, choices=(0, 1), required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--profile", action="store_true")
    args = parser.parse_args()
    if Path(args.label).name != args.label:
        parser.error("label must be a filename prefix")
    inputs = json.loads((AUDIT / "pedal-241-current-inline-build-inputs.json").read_text())
    geastack = Path(inputs["overrides"]["GEA_COMPILER_DIR"]).parent
    examples = geastack / "examples"
    app = examples / "apps/bouncing-balls-jsx"
    manifest = app / "package.json"
    dot_env = app / ".env"
    sdk_defaults = app / "sdkconfig.defaults"
    original = manifest.read_bytes()
    original_env = dot_env.read_bytes() if dot_env.exists() else None
    original_mode = dot_env.stat().st_mode & 0o777 if dot_env.exists() else None
    original_sdk_defaults = sdk_defaults.read_bytes() if sdk_defaults.exists() else None
    package = json.loads(original)
    package["gea"]["ota"] = {**package["gea"].get("ota", {}), "wifi": True}
    package["gea"]["defines"] = {
        **package["gea"].get("defines", {}),
        "GEA_EMBEDDED_FRAME_BENCHMARK": 2,
        "GEA_EMBEDDED_FRAME_SCHEDULER_FPS_LOG": 0,
        "GEA_EMBEDDED_DIAGNOSTICS_ENABLED": 1,
        "GEA_EMBEDDED_SHARED_STYLES": args.shared,
    }
    if args.profile:
        package["gea"]["defines"]["GEA_EMBEDDED_PERF"] = 1
    pedal = json.loads((ROOT / "package.json").read_text())
    table = pedal["gea"]["targets"]["esp32"]["partitionsByTarget"]["esp32-s3-touch-amoled-2.41"]
    package["gea"]["targets"]["esp32"] = {
        "partitions": {
            name: {key: value for key, value in part.items() if key != "data"}
            for name, part in table.items()
        },
        "sdkconfig": "sdkconfig.defaults",
    }
    env = os.environ.copy()
    env.update(inputs["overrides"])
    env.update(CCACHE_DISABLE="1", TMPDIR=str(AUDIT))
    command = [
        "node",
        str(geastack / "cli/bin/gea.mjs"),
        "build",
        "--board",
        "amoled-241",
        "--app",
        "bouncing-balls-jsx",
        "--output",
        str(AUDIT / (args.label + ".bin")),
    ]
    metadata = {
        "command": command,
        "shared": args.shared,
        "wifiEnabled": True,
        "originalManifestSha256": hashlib.sha256(original).hexdigest(),
        "overrides": inputs["overrides"],
        "defines": package["gea"]["defines"],
    }
    (AUDIT / (args.label + "-build-inputs.json")).write_text(json.dumps(metadata, indent=2) + "\n")
    credentials = Config()
    assert not any(c in credentials.ssid + credentials.password for c in "\r\n")
    try:
        manifest.write_text(json.dumps(package, indent=2) + "\n")
        sdk_defaults.write_bytes(
            (original_sdk_defaults or b"")
            + b"\n"
            + (ROOT / "scripts/balls-241.sdkconfig.defaults").read_bytes()
        )
        fd = os.open(dot_env, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as file:
            file.write(
                f'GEA_WIFI_SSID="{credentials.ssid}"\nGEA_WIFI_PASSWORD="{credentials.password}"\n'
            )
        os.chmod(dot_env, 0o600)
        with (AUDIT / (args.label + "-build.log")).open("w") as log:
            result = subprocess.run(
                command, cwd=examples, env=env, stdout=log, stderr=subprocess.STDOUT
            )
        print(f"Gea build exit: {result.returncode}", flush=True)
        return result.returncode
    finally:
        manifest.write_bytes(original)
        if original_sdk_defaults is None:
            sdk_defaults.unlink(missing_ok=True)
        else:
            sdk_defaults.write_bytes(original_sdk_defaults)
        if original_env is None:
            dot_env.unlink(missing_ok=True)
        else:
            dot_env.write_bytes(original_env)
            os.chmod(dot_env, original_mode)


if __name__ == "__main__":
    sys.exit(main())
