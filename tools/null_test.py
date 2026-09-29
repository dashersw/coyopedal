#!/usr/bin/env python3
"""Null-test a device's rendering of a NAM capture against the original model.

The reference is the original ``.nam`` rendered on the host by the official
NeuralAmpModelerCore (float, A2-Full submodel). The device under test (DUT) is
a recording of the same DI played through the hardware, for example a MOOER
pedal running its "A2 conversion" of the same file. The tool lines the two up,
gain-matches them, subtracts, and reports how deep the null is.

    tools/null_test.py --model build/null-test/models/V30-Ampt-4.nam \\
        --di build/null-test/di.wav --dut recordings/ampete-4-mooer.wav \\
        [--bypass recordings/bypass-mooer.wav] [--out build/null-test/out]

Or compare two renders that already exist:

    tools/null_test.py --reference full.wav --dut lite.wav

What it does:

1. Loads mono 48 kHz audio (a stereo DUT is averaged to mono; use --channel to
   pick one instead).
2. If ``--bypass`` is given, it is a recording of the same DI through the device
   with the amp block off. From it the tool measures the round-trip latency and
   the level scaling of the recording chain, and reports both. The input gain is
   applied to the DI before rendering the reference, because a NAM model is
   level-dependent: a DI that reaches the model 2 dB hotter is a different
   sound, not a louder one.
3. Renders the reference with NeuralAmpModelerCore's ``render`` (``--slim 1.0``,
   the A2-Full member of a SlimmableContainer).
4. Aligns the DUT to the reference by cross-correlation, to a fraction of a
   sample, tries both polarities, and fits one gain by least squares.
5. Reports the null depth (reference RMS over residual RMS, in dB), the worst
   100 ms window, the residual per octave band, and writes the aligned
   reference, aligned DUT and residual as float WAVs.

Yardsticks, measured with this tool on the 15 s guitar DI: two float
renders of the same model null past 300 dB; a fractional delay, a gain change
and a polarity flip with -60 dB noise added recover to 57 dB; and the A2-Lite
member of the same VoLum capture nulls against A2-Full at only 11 dB (Ampete
One C4), 21 dB (Diezel Herbert C1) and 21 dB (Marshall 2204). A converted model
that lands near those Lite figures is a re-trained smaller network, not the
capture; one that lands near the bypass floor of the recording chain is
faithful.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

import numpy as np
import soundfile as sf
from scipy import signal

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_RENDER = ROOT / "build/NeuralAmpModelerCore/build-render/tools/render"
SAMPLE_RATE = 48000
# The NAM renderer and the DI run at 48 kHz; a device at another rate gets the
# DI and the reference through the same polyphase resampler.
RENDER_RATE = 48000
BANDS_HZ = [
    (20, 80),
    (80, 160),
    (160, 320),
    (320, 640),
    (640, 1280),
    (1280, 2560),
    (2560, 5120),
    (5120, 10240),
    (10240, 20000),
]


def db(x: float) -> float:
    return 20.0 * np.log10(max(float(x), 1e-30))


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(x, dtype=np.float64)))) if len(x) else 0.0


def load_mono(path: pathlib.Path, channel: int | None, expected: int | None = None) -> np.ndarray:
    expected = expected or SAMPLE_RATE
    data, rate = sf.read(path, dtype="float64", always_2d=True)
    if rate != expected:
        raise SystemExit(f"{path}: {rate} Hz, expected {expected} Hz (resample it first)")
    if channel is not None:
        return np.ascontiguousarray(data[:, channel])
    return np.ascontiguousarray(data.mean(axis=1))


def render(
    render_binary: pathlib.Path,
    model: pathlib.Path,
    di: pathlib.Path,
    out: pathlib.Path,
    slim: float,
) -> np.ndarray:
    if not render_binary.is_file():
        raise SystemExit(
            f"{render_binary} is missing. Clone https://github.com/sdatkinson/NeuralAmpModelerCore "
            "into build/ and build the 'render' target in build-render/ (see docs/NULL_TEST.md)."
        )
    command = [str(render_binary), "--slim", repr(slim), str(model), str(di), str(out)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"render failed:\n{result.stderr}")
    return to_device_rate(load_mono(out, None, RENDER_RATE))


def to_device_rate(x: np.ndarray) -> np.ndarray:
    """Resample a 48 kHz signal to --rate (a Kaiser-windowed polyphase filter,
    the same one that makes the 44.1 kHz DI the device is played)."""
    if SAMPLE_RATE == RENDER_RATE:
        return x
    g = np.gcd(SAMPLE_RATE, RENDER_RATE)
    return signal.resample_poly(x, SAMPLE_RATE // g, RENDER_RATE // g, window=("kaiser", 14.0))


def integer_lag(reference: np.ndarray, dut: np.ndarray, max_lag: int) -> tuple[int, float]:
    """Lag (samples the DUT is late by) and the normalised correlation at it."""
    n = min(len(reference), len(dut))
    a = reference[:n] - reference[:n].mean()
    b = dut[:n] - dut[:n].mean()
    correlation = signal.correlate(b, a, mode="full", method="fft")
    lags = signal.correlation_lags(len(b), len(a), mode="full")
    keep = np.abs(lags) <= max_lag
    correlation, lags = correlation[keep], lags[keep]
    best = int(np.argmax(np.abs(correlation)))
    scale = np.linalg.norm(a) * np.linalg.norm(b) or 1.0
    return int(lags[best]), float(correlation[best] / scale)


def fractional_shift(x: np.ndarray, shift: float) -> np.ndarray:
    """Delay x by `shift` samples (may be fractional, may be negative) using an FFT."""
    n = len(x)
    padded = 1 << int(np.ceil(np.log2(n * 2)))
    spectrum = np.fft.rfft(x, padded)
    frequencies = np.fft.rfftfreq(padded)
    spectrum *= np.exp(-2j * np.pi * frequencies * shift)
    return np.fft.irfft(spectrum, padded)[:n]


def refine_lag(reference: np.ndarray, dut: np.ndarray, lag: int) -> float:
    """Fractional refinement of an integer lag by minimising the residual."""
    lo, hi = -1.0, 1.0
    best_shift, best_cost = 0.0, np.inf
    for _ in range(3):
        for shift in np.linspace(lo, hi, 21):
            candidate = fractional_shift(dut, -(lag + shift))
            n = min(len(reference), len(candidate))
            gain = np.dot(candidate[:n], reference[:n]) / (
                np.dot(candidate[:n], candidate[:n]) or 1.0
            )
            cost = rms(reference[:n] - gain * candidate[:n])
            if cost < best_cost:
                best_cost, best_shift = cost, float(shift)
        step = (hi - lo) / 20
        lo, hi = best_shift - step, best_shift + step
    return lag + best_shift


def band_report(reference: np.ndarray, residual: np.ndarray) -> list[tuple[str, float, float]]:
    rows = []
    for low, high in BANDS_HZ:
        sos = signal.butter(
            4, [low, min(high, SAMPLE_RATE / 2 - 1)], btype="band", fs=SAMPLE_RATE, output="sos"
        )
        ref_band = signal.sosfiltfilt(sos, reference)
        res_band = signal.sosfiltfilt(sos, residual)
        rows.append(
            (f"{low}-{high} Hz", db(rms(ref_band)), db(rms(ref_band) / (rms(res_band) or 1e-30)))
        )
    return rows


def worst_window(reference: np.ndarray, residual: np.ndarray, window: int) -> tuple[float, float]:
    count = len(reference) // window
    if not count:
        return 0.0, db(rms(reference) / (rms(residual) or 1e-30))
    ref = reference[: count * window].reshape(count, window)
    res = residual[: count * window].reshape(count, window)
    ref_rms = np.sqrt(np.mean(ref**2, axis=1))
    res_rms = np.sqrt(np.mean(res**2, axis=1))
    active = ref_rms > rms(reference) * 0.1  # ignore near-silence, where the depth is noise
    if not active.any():
        active[:] = True
    depth = 20 * np.log10(np.maximum(ref_rms, 1e-30) / np.maximum(res_rms, 1e-30))
    index = int(np.argmin(np.where(active, depth, np.inf)))
    return index * window / SAMPLE_RATE, float(depth[index])


def linear_report(
    reference: np.ndarray, dut: np.ndarray
) -> tuple[list[tuple[str, float, float]], float]:
    """Coherence and the DUT/reference transfer function per band, and the null
    left after the reference is passed through that fitted linear response.

    A low null with high coherence and a sloped transfer function is an EQ, a
    cab or a low-pass in the chain. A low null with low coherence is nonlinear:
    a different model."""
    segment = 4096
    frequencies, coherence = signal.coherence(reference, dut, fs=SAMPLE_RATE, nperseg=segment)
    _, cross = signal.csd(reference, dut, fs=SAMPLE_RATE, nperseg=segment)
    _, auto = signal.welch(reference, fs=SAMPLE_RATE, nperseg=segment)
    transfer = cross / np.maximum(auto, 1e-20)
    rows = []
    for low, high in BANDS_HZ:
        mask = (frequencies >= low) & (frequencies < high)
        rows.append(
            (f"{low}-{high} Hz", float(coherence[mask].mean()), db(np.abs(transfer[mask]).mean()))
        )
    impulse = np.roll(np.fft.irfft(transfer, segment), segment // 2) * signal.windows.hann(segment)
    fitted = signal.fftconvolve(reference, impulse)[segment // 2 : segment // 2 + len(reference)]
    fitted *= np.dot(fitted, dut) / (np.dot(fitted, fitted) or 1.0)
    return rows, db(rms(dut) / (rms(dut - fitted) or 1e-30))


def remove_drift(reference: np.ndarray, dut: np.ndarray, lag: int) -> tuple[np.ndarray, float]:
    """Resample the DUT so its clock matches the reference's.

    Two devices on two crystals drift by a few ppm, which over 15 s is a few
    samples: one alignment then fits only one end of the file. The lag is
    tracked in 2 s windows, a line is fitted, and the DUT is resampled by that
    ratio. Returns the corrected DUT and the drift in ppm."""
    window, step, margin = 2 * SAMPLE_RATE, SAMPLE_RATE, 100
    points = []
    for start in range(0, min(len(reference), len(dut)) - window - lag - margin, step):
        a = reference[start : start + window]
        b = dut[start + lag - margin : start + lag + window + margin]
        if start + lag - margin < 0 or rms(a) < 1e-4:
            continue
        c = np.abs(signal.correlate(b - b.mean(), a - a.mean(), mode="valid", method="fft"))
        i = int(np.argmax(c))
        if 0 < i < len(c) - 1:
            y0, y1, y2 = c[i - 1 : i + 2]
            i = i + 0.5 * (y0 - y2) / ((y0 - 2 * y1 + y2) or 1.0)
        points.append((start, lag - margin + i))
    if len(points) < 3:
        return dut, 0.0
    times, lags = np.array(points).T
    slope = float(np.polyfit(times, lags, 1)[0])  # samples of lag per sample
    if abs(slope) < 1e-8:
        return dut, slope * 1e6
    ratio = 1.0 + slope
    return signal.resample(dut, int(round(len(dut) / ratio))), slope * 1e6


def compare(
    reference: np.ndarray,
    dut: np.ndarray,
    max_lag: int,
    trim: int,
    label: str,
    out: pathlib.Path | None,
) -> dict:
    lag, correlation = integer_lag(reference, dut, max_lag)
    polarity = 1.0 if correlation >= 0 else -1.0
    dut = polarity * dut
    dut, drift_ppm = remove_drift(reference, dut, lag)
    if drift_ppm:
        lag, correlation = integer_lag(reference, dut, max_lag)
    exact_lag = refine_lag(reference, dut, lag)
    aligned = fractional_shift(dut, -exact_lag)
    n = min(len(reference), len(aligned)) - trim
    reference, aligned = reference[trim:n], aligned[trim:n]
    gain = float(np.dot(aligned, reference) / (np.dot(aligned, aligned) or 1.0))
    aligned = gain * aligned
    residual = reference - aligned
    depth = db(rms(reference) / (rms(residual) or 1e-30))
    at, worst = worst_window(reference, residual, SAMPLE_RATE // 10)
    bands = band_report(reference, residual)

    print(f"== {label}")
    print(
        f"   DUT is late by {exact_lag:.2f} samples ({exact_lag / SAMPLE_RATE * 1e3:.3f} ms), "
        f"polarity {'normal' if polarity > 0 else 'inverted'}, correlation {abs(correlation):.4f}"
    )
    print(
        f"   gain applied to DUT: {db(abs(gain)):+.2f} dB, clock drift removed: {drift_ppm:+.1f} ppm"
    )
    print(
        f"   reference {db(rms(reference)):.1f} dBFS RMS, residual {db(rms(residual)):.1f} dBFS RMS"
    )
    print(f"   null depth: {depth:.1f} dB   (worst 100 ms window: {worst:.1f} dB at {at:.1f} s)")
    print("   band            reference   null")
    for name, level, band_depth in bands:
        print(f"   {name:14s} {level:8.1f} dB {band_depth:6.1f} dB")
    linear, corrected = linear_report(reference, aligned)
    print(f"   null after fitting the DUT's linear response onto the reference: {corrected:.1f} dB")
    print("   band           coherence  DUT/reference")
    for name, coherence, transfer in linear:
        print(f"   {name:14s} {coherence:7.2f}   {transfer:7.1f} dB")

    if out:
        out.mkdir(parents=True, exist_ok=True)
        sf.write(out / f"{label}-reference.wav", reference.astype(np.float32), SAMPLE_RATE)
        sf.write(out / f"{label}-dut-aligned.wav", aligned.astype(np.float32), SAMPLE_RATE)
        sf.write(out / f"{label}-residual.wav", residual.astype(np.float32), SAMPLE_RATE)
        print(f"   wrote {out}/{label}-{{reference,dut-aligned,residual}}.wav")
    return {"lag": exact_lag, "gain_db": db(abs(gain)), "depth_db": depth, "worst_db": worst}


def render_at(
    arguments, di: np.ndarray, label: str, out: pathlib.Path, level_db: float
) -> np.ndarray:
    """Render the model with the DI scaled by level_db, cached under out/."""
    scaled = out / f"{label}-di{level_db:+.1f}dB.wav"
    rendered = out / f"{label}-reference{level_db:+.1f}dB.wav"
    if not rendered.is_file():
        sf.write(scaled, (di * 10 ** (level_db / 20)).astype(np.float32), RENDER_RATE)
        return render(arguments.render, arguments.model, scaled, rendered, arguments.slim)
    return to_device_rate(load_mono(rendered, None, RENDER_RATE))


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--model", type=pathlib.Path, help="original .nam to render the reference from"
    )
    parser.add_argument("--di", type=pathlib.Path, help="the DI that was played into the device")
    parser.add_argument(
        "--reference", type=pathlib.Path, help="an already rendered reference instead"
    )
    parser.add_argument(
        "--dut", type=pathlib.Path, required=True, help="recording of the device's output"
    )
    parser.add_argument(
        "--bypass",
        type=pathlib.Path,
        help="recording of the DI through the device with the amp block off",
    )
    parser.add_argument("--channel", type=int, help="use this channel of a multichannel recording")
    parser.add_argument(
        "--input-gain-db",
        type=float,
        help="level at which the DI reached the device, relative to the file; measured from "
        "the bypass recording when not given (a digital loop whose output is attenuated, "
        "such as a volume knob on the return, needs this stated: usually 0)",
    )
    parser.add_argument(
        "--sweep",
        metavar="LOW:HIGH:STEP",
        help="try reference renders with the DI at each level in this dB range and report "
        "the best one, e.g. -18:6:2; the final comparison uses that level",
    )
    parser.add_argument(
        "--slim",
        type=float,
        default=1.0,
        help="SlimmableContainer member to render (1.0 = A2-Full, 0.0 = A2-Lite)",
    )
    parser.add_argument(
        "--max-lag", type=float, default=1.0, help="largest latency to search, in seconds"
    )
    parser.add_argument(
        "--trim",
        type=float,
        default=0.2,
        help="seconds to drop at each end after alignment (edge effects, warm-up)",
    )
    parser.add_argument(
        "--render",
        type=pathlib.Path,
        default=DEFAULT_RENDER,
        help="NeuralAmpModelerCore render binary",
    )
    parser.add_argument(
        "--rate",
        type=int,
        default=SAMPLE_RATE,
        help="sample rate of the DUT recording; with --model the DI stays 48 kHz, the reference "
        "is rendered at 48 kHz and resampled to this rate",
    )
    parser.add_argument("--out", type=pathlib.Path, help="directory for aligned and residual WAVs")
    parser.add_argument("--label", help="name for the report and output files")
    arguments = parser.parse_args()
    globals()["SAMPLE_RATE"] = arguments.rate

    if bool(arguments.model) == bool(arguments.reference):
        parser.error("give exactly one of --model (with --di) or --reference")
    if arguments.model and not arguments.di:
        parser.error("--model needs --di")

    max_lag = int(arguments.max_lag * SAMPLE_RATE)
    trim = int(arguments.trim * SAMPLE_RATE)
    label = arguments.label or (arguments.model or arguments.reference).stem
    dut = load_mono(arguments.dut, arguments.channel)

    if arguments.reference:
        reference = load_mono(arguments.reference, None)
    else:
        di = load_mono(arguments.di, None, RENDER_RATE)
        di_path = arguments.di
        if arguments.bypass:
            bypass = load_mono(arguments.bypass, arguments.channel)
            loop = compare(to_device_rate(di), bypass, max_lag, trim, f"{label}-bypass", None)
            # The device saw the DI at (recording gain)^-1 relative to what it
            # played back at us; what reached the model is what matters.
            input_gain = 10 ** (-loop["gain_db"] / 20)
            if arguments.input_gain_db is not None:
                input_gain = 10 ** (arguments.input_gain_db / 20)
            print(
                f"   => chain: {loop['lag'] / SAMPLE_RATE * 1e3:.3f} ms round trip, DI reached the "
                f"device at {db(input_gain):+.2f} dB relative to the file; bypass null "
                f"{loop['depth_db']:.1f} dB is the floor the amp test can reach"
            )
        else:
            input_gain = 10 ** ((arguments.input_gain_db or 0.0) / 20)
        out = arguments.out or (ROOT / "build/null-test/out")
        out.mkdir(parents=True, exist_ok=True)
        if arguments.sweep:
            low, high, step = (float(v) for v in arguments.sweep.split(":"))
            print(f"== {label}: reference level sweep")
            best = None
            for level_db in np.arange(low, high + step / 2, step):
                candidate = render_at(arguments, di, label, out, float(level_db))
                lag, correlation = integer_lag(candidate, dut, max_lag)
                shifted = fractional_shift(np.sign(correlation) * dut, -lag)
                n = min(len(candidate), len(shifted)) - trim
                gain = np.dot(shifted[trim:n], candidate[trim:n]) / (
                    np.dot(shifted[trim:n], shifted[trim:n]) or 1.0
                )
                depth = db(
                    rms(candidate[trim:n])
                    / (rms(candidate[trim:n] - gain * shifted[trim:n]) or 1e-30)
                )
                print(
                    f"   DI at {level_db:+.1f} dB: correlation {abs(correlation):.4f}, null {depth:.1f} dB"
                )
                if best is None or depth > best[1]:
                    best = (float(level_db), depth)
            print(f"   => best at {best[0]:+.1f} dB; the device saw the DI about there")
            input_gain = 10 ** (best[0] / 20)
        if input_gain != 1.0:
            di_path = out / f"{label}-di-scaled.wav"
            sf.write(di_path, (di * input_gain).astype(np.float32), RENDER_RATE)
        reference = render(
            arguments.render,
            arguments.model,
            di_path,
            out / f"{label}-reference-render.wav",
            arguments.slim,
        )

    compare(reference, dut, max_lag, trim, label, arguments.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
