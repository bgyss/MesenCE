#!/usr/bin/env python3
"""Sega CD S0 measurement (mcd-verification-findings.md, F4).

Times N frames of a Mega Drive ROM with and without a stub second 68000, using a runner built
from a worktree with second-68000-stub.patch applied (the stub is enabled by MESEN_S0_STUB=1).

  git worktree add --detach ../MesenCE-s0 <commit>
  cd ../MesenCE-s0 && git apply docs/research/mcd-s0/second-68000-stub.patch && make headless
  python3 docs/research/mcd-s0/time_second_68000.py ../MesenCE-s0/bin/mesen-headless \
      bin/md-synthetic/render_h40.md OUT_DIR [--runs 5] [--frames 3000]
"""
import argparse, os, pathlib, statistics, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("runner")
ap.add_argument("rom")
ap.add_argument("out", help="new directory for the runner outputs")
ap.add_argument("--runs", type=int, default=5)
ap.add_argument("--frames", type=int, default=3000)
args = ap.parse_args()

out = pathlib.Path(args.out)
out.mkdir(parents=True, exist_ok=False)
times = {"off": [], "on": []}
report = []
for i in range(args.runs):
    for mode in ("off", "on"):
        env = dict(os.environ)
        env.pop("MESEN_S0_STUB", None)
        if mode == "on":
            env["MESEN_S0_STUB"] = "1"
            env["MESEN_S0_REPORT"] = "1"
        start = time.monotonic()
        r = subprocess.run([args.runner, "run", "--rom", args.rom, "--frames", str(args.frames),
                            "--out", str(out / f"{mode}-{i}"), "--allow-dirty"],
                           env=env, capture_output=True, text=True)
        times[mode].append(time.monotonic() - start)
        if r.returncode != 0:
            sys.exit(f"runner failed ({mode}): {r.stderr[-500:]}")
        if mode == "on":
            report = [l for l in r.stderr.splitlines() if l.startswith("[S0]")][-1:]

for mode, ts in times.items():
    print(f"{mode}: median {statistics.median(ts):.2f} s  ({', '.join(f'{t:.2f}' for t in ts)})")
off, on = statistics.median(times["off"]), statistics.median(times["on"])
print(f"added: {on - off:.2f} s per {args.frames} frames = {(on - off) / args.frames * 1000:.3f} ms/frame "
      f"({(on / off - 1) * 100:.0f}%)")
print("\n".join(report))
