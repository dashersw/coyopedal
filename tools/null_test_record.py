#!/usr/bin/env python3
"""Play a DI out of one audio device while recording from another.

The companion of tools/null_test.py: it produces the DUT recording. With no
arguments it lists the devices; otherwise it plays the DI, records for the same
length plus a tail, and writes the recording as a float WAV at the DI's rate,
resampling if the recording device could not run at it.

    tools/null_test_record.py
    tools/null_test_record.py --di build/null-test/di.wav --play "Prime S1" \\
        --record "Prime S1" --out recordings/prime-s1-herb-1.wav

Device names are matched as case-insensitive substrings. --play-channel and
--record-channel pick a channel (0-based) on each side; the DI is mono and is
sent to the chosen output channel only, and only the chosen input channel is
kept. Playback level is the file's own level; use --gain-db to change it.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np
import sounddevice as sd
import soundfile as sf
from scipy import signal


def find_device(name: str, kind: str) -> int:
    key = "max_input_channels" if kind == "input" else "max_output_channels"
    matches = [
        i
        for i, d in enumerate(sd.query_devices())
        if name.lower() in d["name"].lower() and d[key] > 0
    ]
    if len(matches) != 1:
        raise SystemExit(f"{kind} device {name!r}: {len(matches)} matches, need exactly one")
    return matches[0]


def usable_rate(device: int, kind: str, wanted: int) -> int:
    check = sd.check_input_settings if kind == "input" else sd.check_output_settings
    for rate in (wanted, 48000, 44100, 96000):
        try:
            check(device=device, samplerate=rate)
            return rate
        except Exception:
            continue
    raise SystemExit(f"{kind} device {device} accepts none of the usual sample rates")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--di", type=pathlib.Path)
    parser.add_argument("--play", help="output device name (substring)")
    parser.add_argument("--record", help="input device name (substring)")
    parser.add_argument(
        "--play-channel", type=int, default=0, help="-1 sends the DI to every output channel"
    )
    parser.add_argument("--record-channel", type=int, default=0)
    parser.add_argument("--gain-db", type=float, default=0.0, help="playback gain")
    parser.add_argument(
        "--tail", type=float, default=1.0, help="seconds to keep recording after the DI"
    )
    parser.add_argument(
        "--lead",
        type=float,
        default=0.0,
        help="seconds of silence to play before the DI (a device that fades its chain in "
        "at stream start needs about 2)",
    )
    parser.add_argument(
        "--discard",
        type=float,
        default=0.0,
        help="seconds to drop from the start of the recording before writing it (the Prime S1 "
        "chain takes about 15 s after a stream starts to settle: use --lead 16 --discard 14)",
    )
    parser.add_argument(
        "--rate",
        type=int,
        help="run both devices at this rate instead of the DI's (a pedal whose USB audio is "
        "44100 Hz gets the DI resampled to 44100, and the recording is resampled back)",
    )
    parser.add_argument("--out", type=pathlib.Path)
    arguments = parser.parse_args()

    if not (arguments.di and arguments.play and arguments.record and arguments.out):
        print(sd.query_devices())
        return 0

    di, rate = sf.read(arguments.di, dtype="float32", always_2d=True)
    di = di.mean(axis=1) * 10 ** (arguments.gain_db / 20)
    play = find_device(arguments.play, "output")
    record = find_device(arguments.record, "input")
    play_rate = usable_rate(play, "output", arguments.rate or rate)
    record_rate = usable_rate(record, "input", arguments.rate or rate)
    if play_rate != rate:
        print(f"output runs at {play_rate} Hz; resampling the DI from {rate} Hz", file=sys.stderr)
        di = signal.resample_poly(di, play_rate, rate).astype(np.float32)
    extra = None
    if sys.platform == "darwin" and arguments.rate:
        # CoreAudio otherwise resamples on the host and leaves the device at its
        # own nominal rate; a pedal that runs its chain at 44100 Hz needs the
        # USB clock actually switched.
        extra = sd.CoreAudioSettings(
            change_device_parameters=True, fail_if_conversion_required=True
        )
    out_channels = sd.query_devices(play)["max_output_channels"]
    in_channels = sd.query_devices(record)["max_input_channels"]
    lead = int(arguments.lead * play_rate)
    frames = lead + len(di) + int(arguments.tail * play_rate)
    playback = np.zeros((frames, out_channels), dtype=np.float32)
    if arguments.play_channel < 0:
        playback[lead : lead + len(di), :] = di[:, None]
    else:
        playback[lead : lead + len(di), arguments.play_channel] = di

    if play_rate != record_rate:
        # PortAudio can run the two devices in separate streams at their own rates.
        record_frames = int(frames * record_rate / play_rate)
        captured = sd.rec(
            record_frames,
            samplerate=record_rate,
            channels=in_channels,
            device=record,
            dtype="float32",
            blocking=False,
            extra_settings=extra,
        )
        sd.play(playback, samplerate=play_rate, device=play, blocking=True, extra_settings=extra)
        sd.wait()
    else:
        captured = sd.playrec(
            playback,
            samplerate=play_rate,
            channels=in_channels,
            dtype="float32",
            device=(record, play),
            blocking=True,
            extra_settings=extra,
        )
    taken = np.ascontiguousarray(captured[:, arguments.record_channel]).astype(np.float64)
    taken = taken[int(arguments.discard * record_rate) :]
    peak = float(np.abs(taken).max())
    rms = float(np.sqrt(np.mean(taken**2)))
    print(
        f"recorded {len(taken) / record_rate:.1f} s at {record_rate} Hz: peak {20 * np.log10(max(peak, 1e-9)):.1f} dBFS, "
        f"rms {20 * np.log10(max(rms, 1e-9)):.1f} dBFS",
        file=sys.stderr,
    )
    if peak < 1e-4:
        print("warning: the recording is silent", file=sys.stderr)
    if peak > 0.99:
        print("warning: the recording clips", file=sys.stderr)
    if record_rate != rate:
        print(f"resampling the recording from {record_rate} Hz to {rate} Hz", file=sys.stderr)
        taken = signal.resample_poly(taken, rate, record_rate)
    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    sf.write(arguments.out, taken.astype(np.float32), rate, subtype="FLOAT")
    print(f"wrote {arguments.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
