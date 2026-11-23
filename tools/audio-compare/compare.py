#!/usr/bin/env python3
"""Compares a MesenCE run's audio with a reference emulator's, without listening to it.

Inputs: a mesen-headless run directory recorded with --chip-audio (audio.wav plus ym2612.raw,
psg.raw and, with a 32X, pwm.raw; with the Sega CD unit, pcm.raw and cdda.raw) and a reference
WAV of the same ROM and input script (e.g. tools/libretro-capture/capture.py with PicoDrive).

Method. Both mixes are reduced to loudness envelopes (RMS in 50 ms windows, DC removed per window)
and aligned by cross-correlation (boot times differ between emulators). Chips play mostly
uncorrelated signals, so energies add: the reference's energy per window is fitted as a
non-negative combination of MesenCE's per-chip energies,

    ref(t)^2 ~ wFM * fm(t)^2 + wPSG * psg(t)^2 + wPWM * pwm(t)^2

The fitted weights, divided by the weights MesenCE's own mix uses, give each chip's level in the
reference relative to MesenCE (dB; 0 = same balance; the overall level is normalised to FM).
Windows where the reference is loud and MesenCE's fit is quiet are sound MesenCE is missing; the
reverse is sound the reference doesn't have.

Spectrum. Loudness alone misses sound that is present but wrong (a sample stream held at one
level per frame has the right energy and no content). Both aligned mixes are therefore also
compared as octave-band spectra (energy share per band over the whole run, and per-window band
profiles); `spectrum_db_vs_reference` gives MesenCE minus the reference per band, after
normalising the total, and `spectral_distance_db` the RMS of those differences over the bands
holding the energy.

Limits: a game's timing differs between emulators (lag, RNG, input timing), so expect some
mismatch even when both are right; compare titles relative to each other and look at the listed
windows. Correlation is over the envelope, not the waveform.

    compare.py MESEN_RUN REFERENCE.wav [--json OUT.json] [--max-lag-seconds 5]
"""
import argparse
import json
import pathlib
import sys
import wave

try:
    import numpy as np
except ImportError:
    sys.exit("compare.py needs numpy (python3 -m pip install numpy)")

WINDOW = 0.05
# MesenCE's mix weights (MdApu::FinishSample at the default volumes): FM 1, PSG 1/2, PWM 1, and the
# Sega CD's PCM and CD audio unscaled
MESEN_WEIGHTS = {"ym2612": 1.0, "psg": 0.5, "pwm": 1.0, "pcm": 1.0, "cdda": 1.0}


def read_wav(path):
    with wave.open(str(path)) as w:
        rate = w.getframerate()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
        return data.reshape(-1, w.getnchannels()).mean(axis=1), rate


def read_raw(path):
    data = np.frombuffer(pathlib.Path(path).read_bytes(), dtype="<i2").astype(np.float64)
    return data.reshape(-1, 2).mean(axis=1)


def envelope(signal, rate):
    n = int(rate * WINDOW)
    count = len(signal) // n
    seg = signal[:count * n].reshape(count, n)
    seg = seg - seg.mean(axis=1, keepdims=True)
    return np.sqrt((seg ** 2).mean(axis=1))


BANDS = [0, 100, 200, 400, 800, 1600, 3200, 6400, 11025]


def band_energy(signal, rate, lag_windows, n_windows):
    """Octave-band energies over the aligned span (start at lag_windows, n_windows long)."""
    rate_common = 22050
    t = np.arange(0, len(signal) / rate, 1 / rate_common)
    x = np.interp(t, np.arange(len(signal)) / rate, signal)
    n = int(rate_common * WINDOW)
    start = lag_windows * n
    x = x[start:start + n_windows * n]
    block = 4096
    total = np.zeros(len(BANDS) - 1)
    f = np.fft.rfftfreq(block, 1 / rate_common)
    win = np.hanning(block)
    for i in range(0, len(x) - block, block):
        seg = x[i:i + block]
        spec = np.abs(np.fft.rfft((seg - seg.mean()) * win)) ** 2
        for b in range(len(BANDS) - 1):
            total[b] += spec[(f >= BANDS[b]) & (f < BANDS[b + 1])].sum()
    return total


def best_lag(a, b, max_lag):
    """Lag (in windows) that maximises the correlation of b shifted against a (log envelopes)."""
    la, lb = np.log1p(a), np.log1p(b)
    best, best_corr = 0, -2.0
    for lag in range(-max_lag, max_lag + 1):
        x, y = (la[lag:], lb) if lag >= 0 else (la, lb[-lag:])
        n = min(len(x), len(y))
        if n < 40:
            continue
        x, y = x[:n], y[:n]
        if x.std() == 0 or y.std() == 0:
            continue
        corr = float(np.corrcoef(x, y)[0, 1])
        if corr > best_corr:
            best, best_corr = lag, corr
    return best, best_corr


def nnls(A, b, iterations=5000):
    """Non-negative least squares by projected gradient (a handful of columns)."""
    scale = np.maximum(np.linalg.norm(A, axis=0), 1e-12)
    An = A / scale
    x = np.zeros(A.shape[1])
    step = 1.0 / (np.linalg.norm(An, 2) ** 2 + 1e-12)
    for _ in range(iterations):
        x = np.maximum(0.0, x - step * (An.T @ (An @ x - b)))
    return x / scale


def segments(mask, min_windows=4):
    """[(start, end)] runs of True at least min_windows long, in windows."""
    out, start = [], None
    for i, v in enumerate(list(mask) + [False]):
        if v and start is None:
            start = i
        elif not v and start is not None:
            if i - start >= min_windows:
                out.append((start, i))
            start = None
    return out


def compare(run, ref_path, max_lag_seconds):
    run = pathlib.Path(run)
    manifest = json.loads((run / "manifest.json").read_text())
    chip_rate = manifest["chip_audio"]["sample_rate"]
    chips = {}
    for name in ("ym2612", "psg", "pwm"):
        if (run / (name + ".raw")).exists():
            chips[name] = envelope(read_raw(run / (name + ".raw")), chip_rate)
    # Sega CD streams run at their own rates (manifest chip_audio.pcm / .cdda)
    for name in ("pcm", "cdda"):
        info = manifest["chip_audio"].get(name)
        if info and (run / info["file"]).exists():
            chips[name] = envelope(read_raw(run / info["file"]), info["sample_rate_hz"])
    mesen_mix, mesen_rate = read_wav(run / "audio.wav")
    mix = envelope(mesen_mix, mesen_rate)
    ref_signal, ref_rate = read_wav(ref_path)
    ref = envelope(ref_signal, ref_rate)

    n = min(len(mix), *(len(c) for c in chips.values()))
    mix = mix[:n]
    chips = {k: v[:n] for k, v in chips.items()}
    if np.percentile(ref, 99) < 8 and np.percentile(mix, 99) < 8:  # (ignores a start-up click)
        return {"run": str(run), "reference": str(ref_path), "silent": True}
    lag, corr_raw = best_lag(ref, mix, int(max_lag_seconds / WINDOW))
    if lag >= 0:
        ref = ref[lag:]
    else:
        mix, chips = mix[-lag:], {k: v[-lag:] for k, v in chips.items()}
    n = min(len(ref), len(mix))
    ref, mix = ref[:n], mix[:n]
    chips = {k: v[:n] for k, v in chips.items()}

    ref_start = lag if lag >= 0 else 0
    mesen_start = -lag if lag < 0 else 0
    ref_bands = band_energy(ref_signal, ref_rate, ref_start, n)
    mesen_bands = band_energy(mesen_mix, mesen_rate, mesen_start, n)
    ref_share = ref_bands / max(ref_bands.sum(), 1e-12)
    mesen_share = mesen_bands / max(mesen_bands.sum(), 1e-12)
    spectrum_db = {}
    held = []
    for b in range(len(BANDS) - 1):
        label = "%d-%d" % (BANDS[b], BANDS[b + 1])
        diff = 10 * np.log10(max(mesen_share[b], 1e-9) / max(ref_share[b], 1e-9))
        spectrum_db[label] = round(float(diff), 1)
        if ref_share[b] > 0.02 or mesen_share[b] > 0.02:
            held.append(diff)
    spectral_distance = float(np.sqrt(np.mean(np.square(held)))) if held else 0.0

    names = [k for k in chips if chips[k].max() > 1.0]
    A = np.stack([chips[k] ** 2 for k in names], axis=1) if names else np.zeros((n, 0))
    w = nnls(A, ref ** 2) if names else np.zeros(0)
    fit = np.sqrt(np.maximum(A @ w, 0)) if names else np.zeros(n)

    # Levels relative to MesenCE's mix, normalised so FM (or the first chip present) is 0 dB
    rel = {}
    for k, wk in zip(names, w):
        rel[k] = 10 * np.log10(max(wk, 1e-12) / MESEN_WEIGHTS[k] ** 2)
    # The base is FM when it carries at least 1% of the fitted energy, else the chip that carries
    # most: a (near) zero-weight base (FM barely playing, as in Tempo) put every other chip at
    # about +120 dB
    contrib = {k: float(wk * A[:, i].sum()) for i, (k, wk) in enumerate(zip(names, w))}
    total = sum(contrib.values())
    if total <= 0:
        base_key = names[0] if names else None
    elif contrib.get("ym2612", 0.0) >= 0.01 * total:
        base_key = "ym2612"
    else:
        base_key = max(contrib, key=contrib.get)
    base = rel[base_key] if base_key else 0.0
    rel_db = {k: round(v - base, 1) for k, v in rel.items()}
    gain_db = round(base, 1)

    loud = max(np.percentile(ref, 95), 1.0)
    missing = (ref > 0.15 * loud) & (fit < 0.3 * ref)
    extra = (fit > 0.15 * loud) & (ref < 0.3 * fit)
    corr_fit = float(np.corrcoef(np.log1p(ref), np.log1p(fit))[0, 1]) if fit.std() > 0 else 0.0

    def times(segs, offset):
        return [[round((s + offset) * WINDOW, 2), round((e + offset) * WINDOW, 2)] for s, e in segs]

    mesen_offset = -lag if lag < 0 else 0
    chip_share = {}
    for k in names:
        mixed = MESEN_WEIGHTS[k] ** 2 * chips[k] ** 2
        chip_share[k] = round(float(mixed.sum() / max(sum((MESEN_WEIGHTS[j] ** 2 * chips[j] ** 2).sum() for j in names), 1e-12)), 3)
    return {
        "run": str(run), "reference": str(ref_path),
        "lag_seconds": round(lag * WINDOW, 2),
        "envelope_correlation": round(corr_raw, 3),
        "fit_correlation": round(corr_fit, 3),
        "reference_gain_db": gain_db,
        "chip_level_db_vs_mesen": rel_db,
        "mesen_energy_share": chip_share,
        "spectrum_db_vs_reference": spectrum_db,
        "spectral_distance_db": round(spectral_distance, 2),
        "missing_seconds": round(float(missing.sum() * WINDOW), 2),
        "extra_seconds": round(float(extra.sum() * WINDOW), 2),
        "missing_windows_mesen_time": times(segments(missing), mesen_offset)[:20],
        "extra_windows_mesen_time": times(segments(extra), mesen_offset)[:20],
        "seconds_compared": round(n * WINDOW, 1),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("reference")
    ap.add_argument("--json")
    ap.add_argument("--max-lag-seconds", type=float, default=5.0)
    args = ap.parse_args()
    result = compare(args.run, args.reference, args.max_lag_seconds)
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(result, indent=1))
    print(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
