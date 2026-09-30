#!/usr/bin/env python3
"""Upload a qualified Balls image and wait for the 2.41's new OTA slot.

For a board already running Gea's native OTA service. The first transition
from the pedal uses amoled_remote.py instead. Never changes partition data.
"""

import hashlib
import json
from pathlib import Path
import sys
import time
import urllib.request

AUDIT = Path(__file__).resolve().parents[1] / "build/css-feature-audit"
BASE = "http://192.168.178.159:8080"
label = sys.argv[1]
if Path(label).name != label:
    raise ValueError("Supply one output filename prefix")
qualified = json.loads((AUDIT / (label + "-qualification.json")).read_text())
image = (AUDIT / (label + ".bin")).read_bytes()
assert qualified["qualified"] and hashlib.sha256(image).hexdigest() == qualified["sha256"]


def status():
    with urllib.request.urlopen(BASE + "/ota/status", timeout=5) as response:
        return json.load(response)


before = status()
assert before["running"]["version"] == "bouncing-balls-jsx"
started = time.monotonic()
request = urllib.request.Request(
    BASE + "/ota",
    data=image,
    headers={"Content-Type": "application/octet-stream"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=180) as response:
    receipt = {"status": response.status, "response": response.read().decode()}
receipt.update(seconds=time.monotonic() - started, bytes=len(image), previous=before)
(AUDIT / (label + "-ota.json")).write_text(json.dumps(receipt, indent=2) + "\n")
deadline = time.monotonic() + 45
while time.monotonic() < deadline:
    time.sleep(1)
    try:
        current = status()
    except OSError:
        continue
    if current["running"]["label"] != before["running"]["label"]:
        assert current["running"]["version"] == "bouncing-balls-jsx"
        (AUDIT / (label + "-running-status.json")).write_text(json.dumps(current, indent=2) + "\n")
        print(json.dumps({"rebooted": True, "slot": current["running"]["label"], **receipt}))
        break
else:
    raise TimeoutError("The new OTA slot did not become reachable; do not measure the old firmware")
