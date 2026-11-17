#!/usr/bin/env python3
"""Cross-build determinism for the synthetic cartridges (e.g. arm64 vs x86-64).

  xarch_compare.py ROM_DIR OTHER_RUNNER [--runner bin/mesen-headless] [--frames 4]

Runs every cartridge on both runners with the bus trace and per-chip audio, then requires every
artifact (trace, chip streams, mixed WAV, picture, memories) to be byte-identical. Save states are
compared by behaviour instead of bytes: each runner loads the *other* runner's final state and
runs two more frames, and those artifacts must match too. (The state payload is compressed with
miniz, whose x86-specific code paths produce different but equivalent deflate streams.)

To get an x86-64 runner on Apple silicon: build `make headless` in a worktree under
`arch -x86_64` with an x86-64 toolchain, then pass its bin/mesen-headless here (runs via Rosetta).
"""
import argparse, json, pathlib, subprocess, sys, tempfile

REGIONS = {"U": "ntsc", "J": "ntsc-j", "E": "pal"}
ROOT = pathlib.Path(__file__).resolve().parents[3]


def run(runner, rom, spec, out, frames, extra):
    cmd = [str(runner), "run", "--rom", str(rom), "--frames", str(frames), "--out", str(out), "--region", REGIONS[spec["region"]],
           "--md-model", spec["model"], "--md-pads", "%s,%s" % (spec["pad1"], spec["pad2"]), "--trace", "bus", "--chip-audio",
           "--allow-dirty"] + spec.get("runner_args", []) + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("%s: %s" % (runner, r.stderr[-300:]))
    return {a["path"]: a["sha256"] for a in json.loads((out / "manifest.json").read_text())["artifacts"] if a["path"] != "final.mss"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roms", type=pathlib.Path)
    ap.add_argument("other", type=pathlib.Path)
    ap.add_argument("--runner", type=pathlib.Path, default=ROOT / "bin/mesen-headless")
    ap.add_argument("--frames", type=int, default=4)
    args = ap.parse_args()
    bad = total = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        for sp in sorted(args.roms.glob("*.json")):
            spec = json.loads(sp.read_text())
            rom, name = sp.with_suffix(".md"), spec["name"]
            total += 1
            a = run(args.runner, rom, spec, tmp / (name + "-a"), args.frames, [])
            b = run(args.other, rom, spec, tmp / (name + "-b"), args.frames, [])
            problems = [k for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)]
            #Cross-load: each build continues from the other build's state
            ca = run(args.runner, rom, spec, tmp / (name + "-ca"), 2, ["--load-state", str(tmp / (name + "-b") / "final.mss")])
            cb = run(args.other, rom, spec, tmp / (name + "-cb"), 2, ["--load-state", str(tmp / (name + "-a") / "final.mss")])
            problems += ["cross-loaded " + k for k in sorted(set(ca) | set(cb)) if ca.get(k) != cb.get(k)]
            if problems:
                bad += 1
                print("DIFF  %s: %s" % (name, ", ".join(problems)))
    print("%d of %d cases identical across the two builds (artifacts and cross-loaded states)" % (total - bad, total))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
