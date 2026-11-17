#!/usr/bin/env python3
"""Compare the YM2612 and PSG streams of the synthetic audio cases with the BlastEm reference.

  audio_compare.py ROM_DIR [--runner bin/mesen-headless] [--verbose]

Both sides provide per-chip streams at the YM2612 sample rate (MCLK/1008): MesenCE through
`--chip-audio`, BlastEm through nativekit's runner (reference/<case>/*.raw.z). The two emulators
use different output gains and DAC/PSG level tables, so levels are not compared directly. Per chip
the check requires the same activity (sound vs silence), the same dominant frequency (within 1.5%
or one FFT bin; for broadband output the spectral centroid and peak within 20%) and the same
stereo placement (left / right / both).
"""
import argparse, json, pathlib, struct, subprocess, sys, tempfile, zlib

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
REGIONS = {"U": "ntsc", "J": "ntsc-j", "E": "pal"}


def stereo(data):
    a = np.frombuffer(data, dtype="<i2").astype(np.float64)
    return a[0::2], a[1::2]


def features(left, right):
    mono = (left + right) / 2
    mono = mono - mono.mean() if len(mono) else mono
    rms = float(np.sqrt(np.mean(mono ** 2))) if len(mono) else 0.0
    lr, rr = float(np.sqrt(np.mean(left ** 2))), float(np.sqrt(np.mean(right ** 2)))
    place = "both"
    if lr > 4 * rr + 1:
        place = "left"
    elif rr > 4 * lr + 1:
        place = "right"
    freq = None
    centroid = peak = 0.0
    if len(mono):
        spec_all = np.abs(np.fft.rfft(mono * np.hanning(len(mono))))
        bins = np.fft.rfftfreq(len(mono))
        centroid = float((spec_all * bins).sum() / max(spec_all.sum(), 1e-9))
        peak = float(np.abs(mono).max())
    if rms > 20 and len(mono) >= 1024:
        n = 1 << int(np.log2(len(mono)))
        seg = mono[-n:] * np.hanning(n)
        spec = np.abs(np.fft.rfft(seg))
        spec[0] = 0
        freq = int(np.argmax(spec)), n
    return {"rms": rms, "active": rms > 20, "place": place, "freq": freq, "centroid": centroid, "peak": peak}


def same_freq(a, b, rate):
    (ia, na), (ib, nb) = a, b
    fa, fb = ia * rate / na, ib * rate / nb
    return abs(fa - fb) <= max(0.015 * max(fa, fb), rate / min(na, nb) * 1.01), fa, fb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roms", type=pathlib.Path)
    ap.add_argument("--runner", type=pathlib.Path, default=ROOT / "bin/mesen-headless")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    baseline = json.loads((HERE / "reference/baseline.json").read_text())
    known = {}
    for line in (HERE / "KNOWN").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            known[line.split()[0]] = line.split(" ", 1)[1]
    bad = total = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        for sp in sorted(args.roms.glob("*.json")):
            spec = json.loads(sp.read_text())
            if not any(k in spec["capture"] for k in ("psg", "ym", "mixed")):
                continue
            name = spec["name"]
            total += 1
            out = tmp / name
            cmd = [str(args.runner), "run", "--rom", str(sp.with_suffix(".md")), "--frames", str(spec["frames"] + 1), "--out", str(out),
                   "--chip-audio", "--region", REGIONS[spec["region"]], "--md-model", spec["model"], "--allow-dirty"]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                print("FAIL  %s: runner %s" % (name, r.stderr[-200:]))
                bad += 1
                continue
            rate = baseline["cases"][name].get("sample_rate", 53267.0)
            problems = []
            for chip, ref_file in (("ym2612", "ym2612.raw.z"), ("psg", "psg.raw.z")):
                ref = features(*stereo(zlib.decompress((HERE / "reference" / name / ref_file).read_bytes())))
                ours = features(*stereo((out / (chip + ".raw")).read_bytes()))
                if ref["active"] != ours["active"]:
                    problems.append("%s: active %s, reference %s (rms %.0f / %.0f)" % (chip, ours["active"], ref["active"], ours["rms"], ref["rms"]))
                    continue
                if not ref["active"]:
                    continue
                if ref["place"] != ours["place"]:
                    problems.append("%s: stereo %s, reference %s" % (chip, ours["place"], ref["place"]))
                if ref["freq"] and ours["freq"]:
                    ok, fo, fr = same_freq(ours["freq"], ref["freq"], rate)
                    if not ok and ref["centroid"] > 0.15 and ours["centroid"] > 0.15:
                        #Broadband (noise-like) output, e.g. full-depth modulation: no meaningful peak.
                        #Accept when the spectral centroid (within 20%) and the peak level (within 20%) agree.
                        ok = abs(ours["centroid"] - ref["centroid"]) <= 0.2 * ref["centroid"] and abs(ours["peak"] - ref["peak"]) <= 0.2 * ref["peak"]
                        if ok and args.verbose:
                            print("   info: %s %s broadband, centroid %.0f Hz (reference %.0f Hz)" % (name, chip, ours["centroid"] * rate, ref["centroid"] * rate))
                    if not ok:
                        problems.append("%s: dominant %.1f Hz, reference %.1f Hz" % (chip, fo, fr))
                    elif args.verbose:
                        print("   info: %s %s %.1f Hz (reference %.1f Hz), %s" % (name, chip, fo, fr, ours["place"]))
            status = "ok" if not problems else ("KNOWN" if name in known else "FAIL")
            bad += status == "FAIL"
            print("%-5s %s" % (status, name))
            for p in problems:
                print("      " + p)
            if status == "KNOWN":
                print("      known: " + known[name])
    print("\n%d of %d audio cases agree (excluding KNOWN)" % (total - bad, total))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
