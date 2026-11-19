#!/usr/bin/env python3
"""Save-state replay equality for the synthetic cartridges.

  replay_check.py ROM_DIR [--runner bin/mesen-headless] [--warmup W] [--frames N]

Run A: N frames from power-on with the bus trace. Run B: W frames, which leaves final.mss.
Run C: load B's state and run the remaining N-W frames with the trace. A's trace must equal B's trace
followed by C's, A's YM2612 and PSG streams (--chip-audio) must equal B's followed by C's sample for
sample (audio phase), and C's final picture and memories must equal A's. Cases with scripted pad
input (e.g. the 6-button handshake) get it in all three runs.
This catches state that Serialize misses even when it only shows up in later bus events.
Finally, A's and C's final save states must hold the same value for every field (a field that is
saved but restored wrongly, or state that drifts after a load, shows up here even when no later bus
event depends on it).
"""
import argparse, json, pathlib, struct, subprocess, sys, tempfile, zlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
FM_CORE = "ymfm"
REGIONS = {"U": "ntsc", "J": "ntsc-j", "E": "pal"}


def run(runner, rom, spec, out, frames, extra):
    cmd = [str(runner), "run", "--rom", str(rom), "--frames", str(frames), "--out", str(out), "--region", REGIONS[spec["region"]],
           "--md-model", spec["model"], "--md-pads", "%s,%s" % (spec["pad1"], spec["pad2"]), "--chip-audio", "--allow-dirty", "--md-fm-core", FM_CORE] + spec.get("runner_args", []) + extra
    if spec["input"]["frames"]:
        #Scripted pad input; frame numbers are absolute, so they continue across a state load
        inp = out.parent / (out.name + ".input.json")
        inp.write_text(json.dumps(spec["input"]))
        cmd += ["--input", str(inp)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr[-300:])
    return json.loads((out / "manifest.json").read_text())


def trace(path):
    return [l for l in path.read_text().splitlines() if not l.startswith("#")]


#Fields that legitimately differ after a load: the runner refreshes input once more on a state load
#(tools/headless-runner, findings R4), which advances the poll counter
STATE_FIELDS_IGNORED = {"controlManager.pollCounter"}


def state_fields(path):
    """The emulation state of a .mss file as {key: bytes}: the largest zlib stream holds the
    serializer's "key\0 + uint32 length + data" records (the other one is the screenshot)."""
    data = path.read_bytes()
    best = b""
    i = 0
    while i < len(data) - 2:
        if data[i] == 0x78 and data[i + 1] in (0x01, 0x5E, 0x9C, 0xDA):
            d = zlib.decompressobj()
            try:
                raw = d.decompress(data[i:])
            except zlib.error:
                raw = b""
            if d.eof and raw:
                best = raw if len(raw) > len(best) else best
                i = len(data) - len(d.unused_data)
                continue
        i += 1
    fields, pos = {}, 0
    while pos < len(best):
        end = best.index(b"\0", pos)
        key = best[pos:end].decode("ascii", "replace")
        (size,) = struct.unpack_from("<I", best, end + 1)
        fields[key] = best[end + 5:end + 5 + size]
        pos = end + 5 + size
    return fields


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roms", type=pathlib.Path)
    ap.add_argument("--runner", type=pathlib.Path, default=ROOT / "bin/mesen-headless")
    ap.add_argument("--fm-core", choices=("ymfm", "nuked"), default="ymfm", help="YM2612 core (MdConfig::FmCore)")
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--frames", type=int, default=4)
    args = ap.parse_args()
    global FM_CORE
    FM_CORE = args.fm_core
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
            #Audio phase: the per-chip streams of the uninterrupted run are B's followed by C's, sample for sample
            for chip in ("ym2612.raw", "psg.raw", "pcm.raw"):
                if chip == "pcm.raw" and not (a / chip).exists():
                    continue  # Sega CD PCM: only with the unit attached
                if (a / chip).read_bytes() != (b / chip).read_bytes() + (c / chip).read_bytes():
                    problems.append(chip + " differs (audio not continuous across the state)")
            for f in ["final.png"] + sorted(p.relative_to(a).as_posix() for p in (a / "memory").iterdir()):
                if (a / f).read_bytes() != (c / f).read_bytes():
                    problems.append(f + " differs")
            sa, sc = state_fields(a / "final.mss"), state_fields(c / "final.mss")
            differing = sorted(k for k in sa.keys() | sc.keys() if k not in STATE_FIELDS_IGNORED and sa.get(k) != sc.get(k))
            if not sa:
                problems.append("final.mss: no state found")
            elif differing:
                problems.append("final state differs in %d field(s): %s" % (len(differing), ", ".join(differing[:6])))
            print("%-5s %s" % ("FAIL" if problems else "ok", name))
            for p in problems[:4]:
                print("      " + p)
            bad += bool(problems)
    print("\n%d of %d cases replay identically" % (len(specs) - bad, len(specs)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
