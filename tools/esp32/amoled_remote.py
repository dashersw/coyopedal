#!/usr/bin/env python3
"""Authenticated discovery, diagnostics, commands and OTA for the AMOLED pedal."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import http.client
import json
from pathlib import Path
import re
import socket
import secrets
import sys
import time
from typing import NoReturn


ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "src/native"
CONFIG_PATH = PROJECT / "services" / "remote_config.h"
DEFAULT_IMAGE = ROOT / "build" / "pedalboard.bin"
DISCOVERY_PORT = 32123
HTTP_PORT = 8080
# The suffix names the build -- "coyopedal-amoled-2.06" for a panel board,
# "coyopedal-headless" for one without -- so matching the whole string would
# make this tool refuse every board but the one it was written against. The
# prefix is the family; the HMAC proof below is what actually authenticates.
DEVICE_PREFIX = "coyopedal-"


class RemoteError(RuntimeError):
    pass


def fail(message: str) -> NoReturn:
    raise RemoteError(message)


def is_coyopedal(device: object) -> bool:
    return isinstance(device, str) and device.startswith(DEVICE_PREFIX)


def macro(text: str, name: str) -> str:
    match = re.search(rf'^#define\s+{re.escape(name)}\s+"([^"\\]*(?:\\.[^"\\]*)*)"\s*$', text, re.M)
    if not match:
        fail(f"Missing {name} in {CONFIG_PATH}; run configure_remote.py first.")
    return bytes(match.group(1), "utf-8").decode("unicode_escape")


class Config:
    def __init__(self) -> None:
        if not CONFIG_PATH.exists():
            fail(f"{CONFIG_PATH} does not exist; run configure_remote.py first.")
        text = CONFIG_PATH.read_text()
        self.ssid = macro(text, "COYOPEDAL_REMOTE_WIFI_SSID")
        self.password = macro(text, "COYOPEDAL_REMOTE_WIFI_PASSWORD")
        self.token = macro(text, "COYOPEDAL_REMOTE_TOKEN")


def discover(config: Config, timeout: float = 2.0) -> list[tuple[str, dict[str, object]]]:
    nonce = secrets.token_hex(16)
    message = f"COYOPEDAL_DISCOVER_V1 {nonce}".encode()
    proof = hmac.new(config.token.encode(), message, hashlib.sha256).hexdigest()
    payload = message + b" " + proof.encode()
    found: dict[str, dict[str, object]] = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(0.2)
        for address in (
            "255.255.255.255",
            "192.168.1.255",
            "192.168.4.255",
            # iPhone Personal Hotspot uses 172.20.10.0/28. Its directed
            # broadcast is not reached by every macOS route when only the
            # limited broadcast above is sent.
            "172.20.10.15",
        ):
            try:
                sock.sendto(payload, (address, DISCOVERY_PORT))
            except OSError:
                pass
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                body, peer = sock.recvfrom(2048)
                decoded = json.loads(body)
                network = decoded.get("network")
                port = decoded.get("port")
                mac = decoded.get("mac")
                response_proof = decoded.get("proof")
                signed_response = (
                    f"COYOPEDAL_RESPONSE_V1\n{nonce}\n{mac}\n{port}\n{network}".encode()
                )
                expected = hmac.new(
                    config.token.encode(), signed_response, hashlib.sha256
                ).hexdigest()
                if (
                    is_coyopedal(decoded.get("device"))
                    and decoded.get("nonce") == nonce
                    and isinstance(network, str)
                    and port == HTTP_PORT
                    and isinstance(mac, str)
                    and isinstance(response_proof, str)
                    and hmac.compare_digest(response_proof, expected)
                ):
                    found[peer[0]] = decoded
            except socket.timeout:
                continue
            except (OSError, ValueError):
                continue
    return sorted(found.items())


def realtime_request(
    config: Config, host: str, verb: str, timeout: float = 2.0
) -> dict[str, object]:
    nonce = secrets.token_hex(16)
    message = f"COYOPEDAL_REALTIME_V1 {verb} {nonce}".encode()
    proof = hmac.new(config.token.encode(), message, hashlib.sha256).hexdigest()
    payload = message + b" " + proof.encode()
    deadline = time.monotonic() + timeout
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(0.2)
        sock.sendto(payload, (host, DISCOVERY_PORT))
        while time.monotonic() < deadline:
            try:
                body, peer = sock.recvfrom(2048)
                if peer[0] != host:
                    continue
                decoded = json.loads(body)
                network = decoded.get("network")
                port = decoded.get("port")
                mac = decoded.get("mac")
                response_proof = decoded.get("proof")
                signed_response = (
                    f"COYOPEDAL_RESPONSE_V1\n{nonce}\n{mac}\n{port}\n{network}".encode()
                )
                expected = hmac.new(
                    config.token.encode(), signed_response, hashlib.sha256
                ).hexdigest()
                if (
                    is_coyopedal(decoded.get("device"))
                    and decoded.get("nonce") == nonce
                    and decoded.get("realtime") is True
                    and isinstance(network, str)
                    and port == HTTP_PORT
                    and isinstance(mac, str)
                    and isinstance(response_proof, str)
                    and hmac.compare_digest(response_proof, expected)
                ):
                    return decoded
            except socket.timeout:
                continue
            except (OSError, ValueError):
                continue
    fail(f"No authenticated realtime response from {host}.")


def resolve_host(args: argparse.Namespace, config: Config) -> str:
    if args.host:
        return args.host
    found = discover(config)
    if len(found) == 1:
        return found[0][0]
    if not found:
        fail("No CoyoPedal AMOLED answered discovery. Pass --host IP if broadcast is filtered.")
    fail("Multiple CoyoPedal AMOLED devices answered; pass --host IP explicitly.")


def request(
    config: Config,
    host: str,
    method: str,
    path: str,
    body: bytes | None = None,
    *,
    timeout: float = 90,
) -> tuple[bytes, dict[str, str]]:
    # Engine qualification can reload, prewarm, and calibrate the embedded
    # A2-Full model before replying.  Leave room for the slowest factory model.
    connection = http.client.HTTPConnection(host, HTTP_PORT, timeout=timeout)
    headers = {"X-CoyoPedal-Token": config.token, "Connection": "close"}
    if body is not None:
        headers["Content-Length"] = str(len(body))
        headers["Content-Type"] = "text/plain"
    try:
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        data = response.read()
        response_headers = {key.lower(): value for key, value in response.getheaders()}
    except OSError as error:
        fail(f"Could not reach {host}:{HTTP_PORT}: {error}")
    finally:
        connection.close()
    if response.status >= 300:
        fail(f"Device returned HTTP {response.status}: {data.decode(errors='replace').strip()}")
    return data, response_headers


def upload(config: Config, host: str, image: Path) -> None:
    if not image.is_file():
        fail(f"Firmware image not found: {image}")
    size = image.stat().st_size
    # The OTA slots are 8 MiB since the whole 32 MB flash was laid out; the
    # device rejects anything larger than its running slot itself.
    if size > 8 * 1024 * 1024:
        fail(f"Firmware is {size} bytes; the OTA slot is 8 MiB.")
    # Production audio can legitimately starve the low-priority HTTP server.
    # Newer firmware exposes a tiny authenticated UDP control path at audio
    # priority; park the pipeline there before opening the OTA connection.
    # Keep compatibility with older images, whose HTTP service is reachable
    # whenever audio is already idle or the interface is disconnected.
    try:
        realtime = realtime_request(config, host, "PAUSE")
    except RemoteError:
        realtime = None
    if realtime is not None and realtime.get("paused") is not True:
        fail("Device did not confirm that its audio pipeline is paused.")
    connection = http.client.HTTPConnection(host, HTTP_PORT, timeout=60)
    connection.putrequest("POST", "/v1/ota")
    connection.putheader("X-CoyoPedal-Token", config.token)
    connection.putheader("Content-Type", "application/octet-stream")
    connection.putheader("Content-Length", str(size))
    connection.endheaders()
    sent = 0
    try:
        with image.open("rb") as firmware:
            # The device parks the audio pipeline at a safe block boundary
            # before it starts receiving the image.  Send promptly so larger
            # images remain inside the firmware's bounded OTA window.
            while chunk := firmware.read(16 * 1024):
                connection.send(chunk)
                sent += len(chunk)
                print(f"\r{sent:>8}/{size} bytes ({100 * sent / size:5.1f}%)", end="", flush=True)
        print()
        response = connection.getresponse()
        data = response.read().decode(errors="replace").strip()
    except OSError as error:
        fail(f"OTA transfer failed after {sent} bytes: {error}")
    finally:
        connection.close()
    if response.status >= 300:
        fail(f"Device rejected OTA (HTTP {response.status}): {data}")
    print(data)


def wait_for_status(config: Config, host: str, *, timeout: float) -> dict[str, object]:
    deadline = time.monotonic() + timeout
    last_error = "device did not return"
    while time.monotonic() < deadline:
        try:
            body, _ = request(config, host, "GET", "/v1/status")
            decoded = json.loads(body)
            if not isinstance(decoded, dict):
                fail("Device status was not a JSON object.")
            return decoded
        except (RemoteError, json.JSONDecodeError) as error:
            last_error = str(error)
            time.sleep(0.25)
    fail(f"Timed out waiting for {host} after audio test: {last_error}")


def effects_profile(
    config: Config,
    host: str,
    seconds: int,
    output: Path | None,
    input_mode: str = "synthetic",
) -> None:
    # Follow signal-chain order, but isolate each effect against the same
    # NAM-only baseline. The final row measures the real all-effects-on chain.
    cases = (
        ("NAM only", 0x00),
        ("Hard Gate only", 0x01),
        ("Studio VCA only", 0x02),
        ("Chorus only", 0x10),
        ("Klon only", 0x04),
        ("Digital Delay only", 0x20),
        ("Spring Reverb only", 0x08),
        ("All six effects", 0x3F),
    )
    body, _ = request(
        config,
        host,
        "POST",
        "/v1/command",
        f"EFFECT PROFILE {input_mode.upper()} {seconds}".encode(),
    )
    print(body.decode(errors="replace").strip(), flush=True)
    profile = wait_for_effect_result(config, host, seconds * len(cases) + 1.0)
    status = wait_for_status(config, host, timeout=10.0)

    rows = profile.get("rows")
    if not isinstance(rows, list):
        fail("Device effect profile omitted its measurement rows.")
    measurements: list[dict[str, object]] = []
    baseline_a: int | None = None
    baseline_b: int | None = None
    for index, row in enumerate(rows):
        if not isinstance(row, dict) or index >= len(cases):
            fail("Device returned a malformed effect profile row.")
        name, expected_mask = cases[index]
        mask = int(row.get("mask", -1))
        if mask != expected_mask:
            fail(f"{name}: expected mask {expected_mask:#04x}, got {mask:#04x}.")
        stage_a = int(row.get("core0", 0))
        stage_b = int(row.get("core1", 0))
        measurement = {
            "name": name,
            "mask": mask,
            "seconds": seconds,
            "connected": row.get("connected"),
            "stage_a_cycles_per_block": stage_a,
            "stage_b_cycles_per_block": stage_b,
            "stage_a_increment": None if baseline_a is None else stage_a - baseline_a,
            "stage_b_increment": None if baseline_b is None else stage_b - baseline_b,
            "stage_a_max_cycles": row.get("max0"),
            "stage_b_max_cycles": row.get("max1"),
            "stage_a_deadline_misses": row.get("miss0"),
            "stage_b_deadline_misses": row.get("miss1"),
            "captured_frames": row.get("captured"),
            "played_frames": row.get("played"),
            "input_drops": row.get("input_drops"),
            "output_drops": row.get("output_drops"),
            "silent_frames": row.get("silent"),
            "trimmed_frames": row.get("trimmed"),
            "transfer_errors": row.get("errors"),
            "usb_callback_cycles_per_block": row.get("usb_callback_cycles_per_block"),
            "usb_capture_callback_cycles": row.get("usb_capture_callback_cycles"),
            "usb_capture_callback_count": row.get("usb_capture_callback_count"),
            "usb_playback_callback_cycles": row.get("usb_playback_callback_cycles"),
            "usb_playback_callback_count": row.get("usb_playback_callback_count"),
            "usb_feedback_callback_cycles": row.get("usb_feedback_callback_cycles"),
            "usb_feedback_callback_count": row.get("usb_feedback_callback_count"),
            "output_clipped_samples": row.get("clipped"),
            "output_peak": row.get("peak"),
            "stage_a_stack_free": row.get("stack0"),
            "stage_b_stack_free": row.get("stack1"),
            "internal_free": row.get("internal_free"),
            "internal_largest": row.get("internal_largest"),
            "reverb_input_cycles": row.get("reverb_input_cycles"),
            "reverb_tank_cycles": row.get("reverb_tank_cycles"),
            "reverb_blocks": row.get("reverb_blocks"),
        }
        if row.get("connected") is not True:
            fail(f"{name}: the USB audio interface disconnected during the measurement.")
        measurements.append(measurement)
        if baseline_a is None:
            baseline_a = stage_a
            baseline_b = stage_b
        print(
            f"{name}: core0={stage_a} ({measurement['stage_a_increment']}) "
            f"core1={stage_b} ({measurement['stage_b_increment']}) "
            f"drops={row.get('input_drops')}/{row.get('output_drops')} "
            f"miss={row.get('miss0')}/{row.get('miss1')} "
            f"usb={row.get('usb_callback_cycles_per_block')} "
            f"clip={row.get('clipped')} peak={row.get('peak')}"
        )

    result = {
        "host": host,
        "device": status.get("device"),
        "mac": status.get("mac"),
        "firmware": status.get("firmware"),
        "sample_rate_hz": 48000,
        "block_frames": 64,
        "cycle_budget": 320000,
        "packet_per_urb": 1,
        "profile_mode": "isolated-effects-plus-full-chain",
        "input": profile.get("input"),
        "device_error": profile.get("error"),
        "preset": profile.get("preset"),
        "model": profile.get("model"),
        "layer_blocks": profile.get("layer_blocks"),
        "layer_cycles": profile.get("layer_cycles"),
        "wide_mixin_blocks": profile.get("wide_mixin_blocks"),
        "measurements": measurements,
    }
    write_effect_result(result, output)
    if profile.get("error") != 0 or len(measurements) != len(cases):
        fail(
            f"Device effect profile incomplete: error={profile.get('error')} "
            f"rows={len(measurements)}/{len(cases)}."
        )


def wait_for_effect_result(config: Config, host: str, initial_wait: float) -> dict[str, object]:
    time.sleep(initial_wait)
    deadline = time.monotonic() + 90.0
    profile: dict[str, object] | None = None
    while time.monotonic() < deadline:
        try:
            body, _ = request(
                config,
                host,
                "POST",
                "/v1/command",
                b"EFFECT PROFILE RESULT",
                timeout=5.0,
            )
            decoded = json.loads(body)
            if isinstance(decoded, dict) and decoded.get("state") == "complete":
                profile = decoded
                break
        except (RemoteError, json.JSONDecodeError):
            pass
        time.sleep(0.5)
    if profile is None:
        fail("Timed out waiting for the device-side effect measurement.")
    return profile


def write_effect_result(result: dict[str, object], output: Path | None) -> None:
    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(encoded)
        print(f"saved {output}")
    else:
        print(encoded, end="")


def effects_soak(
    config: Config,
    host: str,
    seconds: int,
    output: Path | None,
    input_mode: str = "synthetic",
    production: bool = False,
) -> None:
    telemetry_mode = " PRODUCTION" if production else ""
    body, _ = request(
        config,
        host,
        "POST",
        "/v1/command",
        f"EFFECT SOAK {input_mode.upper()}{telemetry_mode} {seconds}".encode(),
    )
    print(body.decode(errors="replace").strip(), flush=True)
    profile = wait_for_effect_result(config, host, seconds + 1.0)
    status = wait_for_status(config, host, timeout=10.0)

    rows = profile.get("rows")
    if not isinstance(rows, list) or len(rows) != 1 or not isinstance(rows[0], dict):
        fail("Device all-effects soak did not return exactly one measurement row.")
    row = rows[0]
    if int(row.get("mask", -1)) != 0x3F:
        fail(f"All-effects soak returned mask {int(row.get('mask', -1)):#04x}.")
    result = {
        "host": host,
        "device": status.get("device"),
        "mac": status.get("mac"),
        "firmware": status.get("firmware"),
        "block_frames": 64,
        "cycle_budget": 320000,
        "packet_per_urb": 1,
        "input": profile.get("input"),
        "production_telemetry_off": production,
        "device_error": profile.get("error"),
        "measurement": {"name": "All six effects", "seconds": seconds, **row},
    }
    print(
        f"All six effects: core0={row.get('core0')} core1={row.get('core1')} "
        f"max={row.get('max0')}/{row.get('max1')} "
        f"drops={row.get('input_drops')}/{row.get('output_drops')} "
        f"miss={row.get('miss0')}/{row.get('miss1')}"
    )
    reverb_blocks = int(row.get("reverb_blocks", 0))
    if reverb_blocks:
        print(
            "Reverb: "
            f"input={int(row.get('reverb_input_cycles', 0)) / reverb_blocks:.0f} "
            f"tank={int(row.get('reverb_tank_cycles', 0)) / reverb_blocks:.0f} "
            f"cycles/block ({reverb_blocks} blocks)"
        )
    write_effect_result(result, output)
    if profile.get("error") != 0:
        fail(f"Device all-effects soak failed: error={profile.get('error')}.")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", help="device IP; found by discovery when omitted")
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("discover")
    subcommands.add_parser("credentials", help="show the Wi-Fi credentials stored locally")
    subcommands.add_parser("status")
    subcommands.add_parser(
        "live-status",
        help="read realtime-safe UDP audio counters even when HTTP is starved",
    )
    subcommands.add_parser(
        "pause-audio",
        help="park the realtime pipeline over authenticated UDP",
    )
    subcommands.add_parser(
        "resume-audio",
        help="resume a pipeline parked over authenticated UDP",
    )
    subcommands.add_parser(
        "audio-mode",
        help="leave maintenance and restore audio without rebooting",
    )
    logs = subcommands.add_parser("logs")
    logs.add_argument("--follow", action="store_true")
    command = subcommands.add_parser("command")
    command.add_argument("text", nargs="+", help="HELP lists the bounded command set")
    ota = subcommands.add_parser("ota")
    ota.add_argument("image", nargs="?", type=Path, default=DEFAULT_IMAGE)
    profile = subcommands.add_parser(
        "effects-profile",
        help="measure each effect alone, then the complete six-effect chain",
    )
    profile.add_argument("--seconds", type=int, default=5)
    profile.add_argument("--input", choices=("live", "synthetic"), default="synthetic")
    profile.add_argument("--output", type=Path)
    soak = subcommands.add_parser(
        "effects-soak",
        help="measure the complete six-effect chain for one bounded run",
    )
    soak.add_argument("--seconds", type=int, default=60)
    soak.add_argument("--input", choices=("live", "synthetic"), default="synthetic")
    soak.add_argument("--output", type=Path)
    soak.add_argument(
        "--production",
        action="store_true",
        help="run with cycle/reverb telemetry disabled, as during normal audio",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    config = Config()
    if args.command == "credentials":
        if not config.ap_mode:
            print(f"station SSID: {config.ssid}")
            return 0
        print(f"SSID base: {config.ssid} (device appends its MAC suffix)")
        print(f"password: {config.password}")
        return 0
    if args.command == "discover":
        found = discover(config)
        if not found:
            fail("No CoyoPedal AMOLED answered authenticated discovery.")
        for address, details in found:
            print(f"{address}\t{json.dumps(details, sort_keys=True)}")
        return 0

    host = resolve_host(args, config)
    if args.command == "status":
        body, _ = request(config, host, "GET", "/v1/status")
        print(json.dumps(json.loads(body), indent=2, sort_keys=True))
    elif args.command == "live-status":
        print(json.dumps(realtime_request(config, host, "STATUS"), indent=2, sort_keys=True))
    elif args.command == "pause-audio":
        result = realtime_request(config, host, "PAUSE")
        if result.get("paused") is not True:
            fail("Device did not confirm that its audio pipeline is paused.")
        print(json.dumps(result, indent=2, sort_keys=True))
    elif args.command == "resume-audio":
        result = realtime_request(config, host, "RESUME")
        if result.get("paused") is not False:
            fail("Device did not confirm that its audio pipeline resumed.")
        print(json.dumps(result, indent=2, sort_keys=True))
    elif args.command == "audio-mode":
        body, _ = request(config, host, "POST", "/v1/command", b"AUDIO MODE")
        print(body.decode(errors="replace"), end="")
    elif args.command == "command":
        body, _ = request(config, host, "POST", "/v1/command", " ".join(args.text).encode())
        print(body.decode(errors="replace"), end="")
    elif args.command == "ota":
        upload(config, host, args.image)
    elif args.command == "effects-profile":
        if not 1 <= args.seconds <= 60:
            fail("--seconds must be between 1 and 60.")
        effects_profile(config, host, args.seconds, args.output, args.input)
    elif args.command == "effects-soak":
        if not 1 <= args.seconds <= 60:
            fail("--seconds must be between 1 and 60.")
        effects_soak(config, host, args.seconds, args.output, args.input, args.production)
    elif args.command == "logs":
        cursor = 0
        while True:
            body, headers = request(config, host, "GET", f"/v1/logs?since={cursor}")
            if body:
                print(body.decode(errors="replace"), end="", flush=True)
            cursor = int(headers.get("x-coyopedal-log-cursor", cursor))
            if not args.follow:
                break
            time.sleep(0.5)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RemoteError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
