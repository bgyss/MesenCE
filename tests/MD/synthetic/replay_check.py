#!/usr/bin/env python3
"""Save-state replay equality for the synthetic cartridges.

  replay_check.py ROM_DIR [--runner bin/mesen-headless] [--warmup W] [--frames N]

Run A: N frames from power-on with the bus trace. Run B: W frames, which leaves final.mss.
Run C: load B's state and run the remaining N-W frames with the trace. A's trace must equal B's trace
followed by C's, and C's final picture and memories must equal A's.
This catches state that Serialize misses even when it only shows up in later bus events.
"""
import argparse, json, pathlib, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
REGIONS = {"U": "ntsc", "J": "ntsc-j", "E": "pal"}


def run(runner, rom, spec, out, frames, extra):
    cmd = [str(runner), "run", "--rom", str(rom), "--frames", str(frames), "--out", str(out), "--region", REGIONS[spec["region"]],
           "--md-model", spec["model"], "--md-pads", "%s,%s" % (spec["pad1"], spec["pad2"]), "--allow-dirty"] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr[-300:])
    return json.loads((out / "manifest.json").read_text())


def trace(path):
    return [l for l in path.read_text().splitlines() if not l.startswith("#")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roms", type=pathlib.Path)
    ap.add_argument("--runner", type=pathlib.Path, default=ROOT / "bin/mesen-headless")
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--frames", type=int, default=4)
    args = ap.parse_args()
    bad = 0
    specs = sorted(args.roms.glob("*.json"))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        for sp in specs:
            spec = json.loads(sp.read_text())
            name, rom = spec["name"], sp.with_suffix(".md")
            a, b, c = tmp / (name + "-a"), tmp / (name + "-b"), tmp / (name + "-c")
            run(args.runner, rom, spec, a, args.frames, ["--trace", "bus"])
            run(args.runner, rom, spec, b, args.warmup, ["--trace", "bus"])
            run(args.runner, rom, spec, c, args.frames - args.warmup, ["--trace", "bus", "--load-state", str(b / "final.mss")])
            #The uninterrupted run's event stream must be exactly B's events followed by C's
            ta = trace(a / "trace.txt")
            tc = trace(b / "trace.txt") + trace(c / "trace.txt")
            problems = []
            if ta != tc:
                first = next((i for i, (x, y) in enumerate(zip(ta, tc)) if x != y), min(len(ta), len(tc)))
                problems.append("trace differs at event %d of %d/%d: %s vs %s" % (first, len(ta), len(tc), ta[first:first + 1], tc[first:first + 1]))
            for f in ["final.png"] + sorted(p.relative_to(a).as_posix() for p in (a / "memory").iterdir()):
                if (a / f).read_bytes() != (c / f).read_bytes():
                    problems.append(f + " differs")
            print("%-5s %s" % ("FAIL" if problems else "ok", name))
            for p in problems[:4]:
                print("      " + p)
            bad += bool(problems)
    print("\n%d of %d cases replay identically" % (len(specs) - bad, len(specs)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
