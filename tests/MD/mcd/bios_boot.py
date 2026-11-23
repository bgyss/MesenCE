#!/usr/bin/env python3
"""Sega CD BIOS boot checks (local only: the BIOS is copyrighted and not in the repository).

  bios_boot.py [--bios-dir DIR] [--runner bin/mesen-headless]

The synthetic disc (tests/MD/synthetic/mcd_cases.py: a data track without a valid boot program,
and an audio track) goes into each BIOS found. No BIOS can boot it, so each one reaches its CD
player after the intro. Checks:
  - every BIOS plays its intro music (PCM output);
  - for the BIOS versions whose CD player starts on PLAY (A or START), pressing the buttons plays
    track 2: the CD audio output must start with the disc's samples, sample for sample (full fader,
    half scale).
BIOS files are looked up by SHA-256 (UI/Interop/FirmwareTypeExtensions.cs lists the same dumps),
in --bios-dir (default: $MESEN_MCD_BIOS_DIR, then ~/mame/roms/segacd). Missing ones are skipped;
with none found the script reports "skipped" and succeeds. Copyright notice and license: GPL-3.0.
"""
import argparse, hashlib, json, os, pathlib, struct, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tests/MD/synthetic"))
import mcd_cases  # noqa: E402

# (name, SHA-256, region, plays track 2 with the button script)
BIOSES = [
    ("US Model 1 v1.10", "807384678EF259494D943EDDB6954DFC7069693D0DDE0C424E2028488D88E81A", "ntsc", True),
    ("US Model 2 v2.00w", "B1C2036B79467514EAAA69C76E5BA83801621E821059CAA53CB921A0B21E3AF4", "ntsc", True),
    ("US Model 2 v2.21", "559A2914E18B91B772629A3088EBECFD34128ED9880FC31C286ADD7F76A6A8F6", "ntsc", True),
    ("EU Model 1 v1.00", "16735DF92FCC1D0FC6A1ECED244867E95281C5006340926082B74E363E073DC4", "pal", False),
    ("EU Model 2 v2.00", "FE608A2A07676A23AB5FD5EEE2F53C9E2526D69A28AA16CCD85C0EC42E6933CB", "pal", True),
    ("JP Model 1 v1.00p", "923EB9DA740F98AB1572085E3D351E2C0982DF0906B0B99EFB68A9997C94CF8F", "ntsc-j", False),
    ("JP Mega-CD 2 v2.00C", "7133FC2DD2FE5B7D0ACD53A5F10F3D00B5D31270239AD20D74EF32393E24AF88", "ntsc-j", False),
]


def find_bioses(directory):
    found = {}
    if directory.is_dir():
        for f in sorted(directory.iterdir()):
            if f.is_file() and f.stat().st_size == 0x20000:
                found.setdefault(hashlib.sha256(f.read_bytes()).hexdigest().upper(), f)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios-dir", type=pathlib.Path,
                    default=pathlib.Path(os.environ.get("MESEN_MCD_BIOS_DIR", pathlib.Path.home() / "mame/roms/segacd")))
    ap.add_argument("--runner", type=pathlib.Path, default=ROOT / "bin/mesen-headless")
    args = ap.parse_args()
    found = find_bioses(args.bios_dir)
    available = [b for b in BIOSES if b[1] in found]
    if not available:
        print("skipped: no known Sega CD BIOS in %s" % args.bios_dir)
        return 0
    presses = {}
    for f in range(1300, 2600, 100):
        presses[str(f)] = {"1": ["START"] if (f // 100) % 2 else ["A"]}
        presses[str(f + 10)] = {"1": []}
    want = [(l >> 1, r >> 1) for l, r in (mcd_cases.disc_audio_sample(k) for k in range(mcd_cases.DISC_AUDIO_SECTORS * 588))]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        mcd_cases.write_assets(tmp)
        (tmp / "input.json").write_text(json.dumps({"frames": presses}))
        for name, sha, region, plays in available:
            out = tmp / ("run-" + sha[:8])
            r = subprocess.run([str(args.runner), "run", "--rom", str(tmp / "mcd_disc.cue"), "--frames", "2800", "--out", str(out),
                                "--allow-dirty", "--md-mcd-bios", str(found[sha]), "--region", region, "--chip-audio",
                                "--input", str(tmp / "input.json")], capture_output=True, text=True)
            problems = []
            if r.returncode:
                problems.append("runner exit %d: %s" % (r.returncode, r.stderr.strip()[-200:]))
            else:
                pcm = (out / "pcm.raw").read_bytes()
                if not any(pcm[i:i + 4] != b"\0\0\0\0" for i in range(0, len(pcm) - 3, 4)):
                    problems.append("no PCM output (intro music)")
                raw = (out / "cdda.raw").read_bytes()
                samples = [struct.unpack_from("<hh", raw, i) for i in range(0, len(raw) - 3, 4)]
                first = next((i for i, s in enumerate(samples) if s != (0, 0)), None)
                if plays:
                    if first is None:
                        problems.append("track 2 did not play")
                    elif samples[first:first + len(want)] != want:
                        bad = next(i for i, (g, w) in enumerate(zip(samples[first:], want)) if g != w)
                        problems.append("CD audio sample %d is %s, disc %s" % (bad, samples[first + bad], want[bad]))
            failures += bool(problems)
            print("%-5s %s%s" % ("FAIL" if problems else "pass", name, "" if plays else " (boot and intro music)"))
            for p in problems:
                print("      " + p)
    print("\n%d of %d BIOS versions pass (%d known ones not found)" % (len(available) - failures, len(available), len(BIOSES) - len(available)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
