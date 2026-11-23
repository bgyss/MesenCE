#!/usr/bin/env python3
"""Compares MesenCE's audio with PicoDrive's for every Mega Drive / 32X ROM in a folder.

Each ROM runs with no input (attract mode keeps both emulators on the same timeline) for
--frames frames in bin/mesen-headless (--chip-audio) and in PicoDrive through
tools/libretro-capture/capture.py, then tools/audio-compare/compare.py compares the two.
Writes OUT/summary.tsv and OUT/summary.json, prints the table and exits 1 if any title is flagged.

Flags (judgment calls, tuned on Virtua Fighter 32X before/after the PWM timing fix: spectral
distance 3.2 dB broken, 0.6 dB fixed; envelope correlation 0.98 both):
  spectral distance > 1.5 dB   sound present but with the wrong content or balance
  envelope correlation < 0.7   timelines diverge or sound is missing (check the screenshots)
  missing > 10 % of the run    reference sound MesenCE doesn't play
  a chip level > 6 dB off      chip balance (chips with at least 10 % of MesenCE's energy)
Titles where both emulators are silent are reported as "silent".

    run_library.py ROM_DIR OUT [--frames 3600] [--core PATH] [--bios DIR] [--match TEXT] [--jobs 4]

The PicoDrive core defaults to RetroArch's (~/Library/Application Support/RetroArch/cores/...);
set PICODRIVE_CORE or --core otherwise. PicoDrive is a reference, not ground truth: a flagged title
needs a look at which emulator is wrong (docs/research/32x-verification-findings.md, S5 "Audio").
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/audio-compare"))
import compare  # noqa: E402

DEFAULT_CORES = [
    pathlib.Path.home() / "Library/Application Support/RetroArch/cores/picodrive_libretro.dylib",
    pathlib.Path.home() / ".config/retroarch/cores/picodrive_libretro.so",
]
EXTENSIONS = {".md", ".gen", ".bin", ".smd", ".32x"}
LIMITS = {"spectral_distance_db": 1.5, "envelope_correlation": 0.7, "missing_fraction": 0.10, "chip_db": 6.0}


def flags(r):
    if r.get("silent"):
        return []
    out = []
    if r["spectral_distance_db"] > LIMITS["spectral_distance_db"]:
        out.append("spectrum")
    if r["envelope_correlation"] < LIMITS["envelope_correlation"]:
        out.append("envelope")
    if r["missing_seconds"] > LIMITS["missing_fraction"] * r["seconds_compared"]:
        out.append("missing")
    for chip, db in r["chip_level_db_vs_mesen"].items():
        #A chip that barely plays has no meaningful fitted level
        if abs(db) > LIMITS["chip_db"] and r["mesen_energy_share"].get(chip, 0) >= 0.10:
            out.append(chip + "-level")
    return out


def run_one(rom, out, frames, core, bios):
    name = re.sub(r"[^A-Za-z0-9]+", "_", rom.stem)[:48]
    mesen = out / name / "mesen"
    pico = out / name / "picodrive"
    cmd = [str(ROOT / "bin/mesen-headless"), "run", "--rom", str(rom), "--frames", str(frames),
           "--out", str(mesen), "--allow-dirty", "--chip-audio", "--snap-every", str(frames // 4)]
    if bios and rom.suffix.lower() == ".32x":
        cmd += ["--md-32x-bios", bios]
    p1 = subprocess.run(cmd, capture_output=True, text=True)
    if p1.returncode != 0 or not (mesen / "audio.wav").exists():
        return {"rom": rom.name, "error": "mesen-headless exit %d: %s" % (p1.returncode, p1.stderr[-300:])}
    p2 = subprocess.run([sys.executable, str(ROOT / "tools/libretro-capture/capture.py"), str(core), str(rom),
                         str(pico), "--frames", str(frames), "--snap-every", str(frames // 4)],
                        capture_output=True, text=True)
    if p2.returncode != 0:
        return {"rom": rom.name, "error": "capture exit %d: %s" % (p2.returncode, p2.stderr[-300:])}
    r = compare.compare(mesen, pico / "audio.wav", 5.0)
    r["rom"] = rom.name
    r["flags"] = flags(r)
    (out / name / "compare.json").write_text(json.dumps(r, indent=1))
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom_dir")
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=3600)
    ap.add_argument("--core", default=os.environ.get("PICODRIVE_CORE"))
    ap.add_argument("--bios", help="folder with the 32X BIOS files (default: replacement boot ROMs)")
    ap.add_argument("--match", default="", help="only ROMs whose file name contains this text")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    core = pathlib.Path(args.core) if args.core else next((c for c in DEFAULT_CORES if c.exists()), None)
    if not core or not core.exists():
        sys.exit("PicoDrive libretro core not found: install it in RetroArch or pass --core / PICODRIVE_CORE")
    roms = sorted(p for p in pathlib.Path(args.rom_dir).iterdir()
                  if p.suffix.lower() in EXTENSIONS and args.match.lower() in p.name.lower())
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda rom: run_one(rom, out, args.frames, core, args.bios), roms))

    lines = ["flags\tspectral_db\tenv_corr\tmissing_s\tfm_db\tpsg_db\tpwm_db\trom"]
    flagged = 0
    for r in results:
        if "error" in r:
            lines.append("ERROR\t\t\t\t\t\t\t%s: %s" % (r["rom"], r["error"].strip().replace("\n", " ")))
            flagged += 1
            continue
        lv = r["chip_level_db_vs_mesen"]
        if r.get("silent"):
            lines.append("silent\t\t\t\t\t\t\t%s" % r["rom"])
            continue
        lines.append("%s\t%.2f\t%.3f\t%.1f\t%s\t%s\t%s\t%s" % (
            ",".join(r["flags"]) or "ok", r["spectral_distance_db"], r["envelope_correlation"], r["missing_seconds"],
            lv.get("ym2612", ""), lv.get("psg", ""), lv.get("pwm", ""), r["rom"]))
        flagged += bool(r["flags"])
    (out / "summary.tsv").write_text("\n".join(lines) + "\n")
    (out / "summary.json").write_text(json.dumps(results, indent=1))
    print("\n".join(lines))
    print("%d of %d flagged" % (flagged, len(results)))
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
