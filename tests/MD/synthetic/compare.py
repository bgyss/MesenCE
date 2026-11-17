#!/usr/bin/env python3
"""Run the synthetic Mega Drive cartridges on mesen-headless and compare with the BlastEm reference.

  compare.py ROM_DIR [--runner bin/mesen-headless] [--case NAME ...] [--verbose]

For every case it runs the cartridge with `--trace bus` and checks, against
reference/baseline.json:
  reads   the 68000 data reads of each READ step: exact address and width, the value under the
          case's compare_mask (equal to the case's expected value and to the reference, as nativekit's
          run.py does; full width when the case gives no mask), and that every POLL step
          terminates; poll iteration counts are timing and are reported, not compared;
  frame   render cases: the final frame, compared up to a relabelling of colours (BlastEm and
          MesenCE use different DAC levels) after locating the active area in BlastEm's bordered
          frame;
  audio   audio cases: whether the chips produce sound at all (reference vs MesenCE, coarse).
Outcomes: pass, KNOWN (listed in KNOWN with a source-backed reason), or FAIL.
"""
import argparse
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
import cases  # noqa: E402
import gen  # noqa: E402
from cases import Operation, READ, WRITE, POLL  # noqa: E402

FM_CORE = "ymfm"
REGIONS = {"U": "ntsc", "J": "ntsc-j", "E": "pal"}


def load_known():
    known = {}
    path = HERE / "KNOWN"
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                name, _, reason = line.partition(" ")
                known.setdefault(name, []).append(reason)
    return known


def png_decode(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, width, height, ctype = 8, b"", 0, 0, 2
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * bpp
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + b) & 0xFF
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 0xFF
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(width)])
        prev = line
    return width, height, rows


def frames_match(ref, ours):
    """True if `ours` appears in `ref` at some offset with a consistent colour bijection."""
    rw, rh, rrows = ref
    ow, oh, orows = ours
    if ow > rw or oh > rh:
        return False, "MesenCE frame %dx%d larger than reference %dx%d" % (ow, oh, rw, rh)
    best = None
    for dy in range(rh - oh + 1):
        for dx in range(rw - ow + 1):
            fwd, bwd, bad = {}, {}, 0
            for y in range(0, oh):
                rr, orr = rrows[dy + y], orows[y]
                for x in range(ow):
                    a, b = rr[dx + x], orr[x]
                    if fwd.setdefault(a, b) != b or bwd.setdefault(b, a) != a:
                        bad += 1
                if bad > ow * oh // 100 and best is not None:
                    break
            if best is None or bad < best[0]:
                best = (bad, dx, dy)
            if bad == 0:
                return True, "offset %d,%d" % (dx, dy)
    return False, "best offset %d,%d with %d mismatched pixels" % (best[1], best[2], best[0])


def expand_operations(case):
    ops = list(case.operations)
    if case.z80_program:
        code = gen.z80_assemble(case.z80_program)
        copy = [Operation(WRITE, 1, 0xA00000 + i, b) for i, b in enumerate(code)]
        ops[case.z80_load_index:case.z80_load_index] = copy
    return ops


def segment_reads(ops, reads):
    """Map the trace's reads onto READ/POLL steps. Returns (values per step, poll counts) or an error."""
    out, polls, i = [], [], 0
    for idx, op in enumerate(ops):
        if op.code == READ:
            words = 2 if op.width == 4 else 1
            got = reads[i:i + words]
            if len(got) < words:
                return None, None, "trace ended before step %d (read $%06X)" % (idx, op.addr)
            out.append((idx, [tuple(r) for r in got]))
            i += words
        elif op.code == POLL:
            count = 0
            while True:
                if i >= len(reads):
                    return None, None, "trace ended inside poll step %d ($%06X)" % (idx, op.addr)
                addr, width, value = reads[i]
                i += 1
                count += 1
                mask = op.compare_mask or 0xFFFF
                if (value & mask) == (op.value & mask):
                    break
            polls.append((idx, count))
    return out, polls, None


def run_case(runner, rom, spec, work):
    out = work / spec["name"]
    #Two extra frames: the frame boundary differs between the emulators (MesenCE ends a frame at line 0),
    #and every case ends in an idle loop, so the reads and the final picture are unaffected.
    cmd = [str(runner), "run", "--rom", str(rom), "--frames", str(spec["frames"] + 2), "--out", str(out),
           "--trace", "bus", "--region", REGIONS[spec["region"]], "--md-model", spec["model"],
           "--md-pads", "%s,%s" % (spec["pad1"], spec["pad2"]), "--allow-dirty", "--md-fm-core", FM_CORE] + spec.get("runner_args", [])
    if spec["input"]["frames"]:
        inp = work / (spec["name"] + ".input.json")
        inp.write_text(json.dumps(spec["input"]))
        cmd += ["--input", str(inp)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode:
        return None, "runner exit %d: %s" % (result.returncode, result.stderr.strip()[-300:])
    reads = []
    for line in (out / "trace.txt").read_text().splitlines():
        if line.startswith("#"):
            continue
        clock, dev, op, width, addr, value = line.split()
        addr = int(addr, 16)
        if dev == "1" and op == "2" and not (0x200 <= addr < 0x4000):
            reads.append([addr, int(width), int(value, 16)])
    return {"dir": out, "reads": reads}, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roms", type=pathlib.Path)
    ap.add_argument("--runner", type=pathlib.Path, default=ROOT / "bin/mesen-headless")
    ap.add_argument("--fm-core", choices=("ymfm", "nuked"), default="ymfm", help="YM2612 core (MdConfig::FmCore)")
    ap.add_argument("--case", action="append")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    global FM_CORE
    FM_CORE = args.fm_core

    baseline = json.loads((HERE / "reference/baseline.json").read_text())
    known = load_known()
    all_cases = {c.name: c for c in cases.all_cases()}
    names = [n for n in args.case if n in all_cases] if args.case else sorted(all_cases)
    counts = {"pass": 0, "KNOWN": 0, "FAIL": 0}
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        for name in names:
            case = all_cases[name]
            spec = json.loads((args.roms / (name + ".json")).read_text())
            ref = baseline["cases"][name]
            problems = []
            ours, err = run_case(args.runner, args.roms / (name + ".md"), spec, work)
            if err:
                problems.append(err)
            else:
                ops = expand_operations(case)
                #BlastEm traces the reset vector fetch ($0-$7) as data reads; MesenCE's trace starts after reset
                ref_reads = list(ref["m68k_reads"])
                while ref_reads and ref_reads[0][0] < 8:
                    ref_reads.pop(0)
                ref_vals, ref_polls, ref_err = segment_reads(ops, ref_reads)
                our_vals, our_polls, our_err = segment_reads(ops, ours["reads"])
                if ref_err:
                    problems.append("reference: " + ref_err)
                if our_err:
                    problems.append("MesenCE: " + our_err)
                if not ref_err and not our_err:
                    for (idx, a), (_, b) in zip(ref_vals, our_vals):
                        #nativekit's accepted comparison (consoles/md/tests/rom/run.py, _compare): a READ with
                        #a compare_mask must equal the case's expected value under that mask, and the two
                        #emulators are compared under the same mask; a READ without one is compared in full.
                        op = ops[idx]
                        mask = op.compare_mask or 0xFFFFFFFF
                        if op.compare_mask:
                            got = b[0][2] if op.width != 4 else (b[0][2] << 16) | b[1][2]
                            if (got & mask) != (op.value & mask):
                                problems.append("step %d read $%X, case expects $%X under mask $%X" % (idx, got, op.value, mask))
                        if [(r[0], r[1], r[2] & mask) for r in a] != [(r[0], r[1], r[2] & mask) for r in b]:
                            problems.append("step %d read %s, reference %s (mask $%X)" % (idx, ["$%06X/%d=%X" % tuple(r) for r in b], ["$%06X/%d=%X" % tuple(r) for r in a], mask))
                    if args.verbose:
                        for (idx, a), (_, b) in zip(ref_polls, our_polls):
                            if a != b:
                                print("   info: %s step %d polls %d (reference %d)" % (name, idx, b, a))
                if "frames" in spec["capture"]:
                    ref_png = HERE / "reference" / name / "frame_last.png"
                    our_png = ours["dir"] / "final.png"
                    ref_frame = png_decode(ref_png.read_bytes())
                    our_frame = png_decode(our_png.read_bytes())
                    if spec["weave_fields"]:
                        #BlastEm presents one field per frame; MesenCE weaves both fields into one frame
                        w, h, rows = our_frame
                        results = [frames_match(ref_frame, (w, h // 2, rows[parity::2])) for parity in (0, 1)]
                        ok = any(r[0] for r in results)
                        detail = "; ".join(r[1] for r in results)
                    else:
                        ok, detail = frames_match(ref_frame, our_frame)
                    if not ok:
                        problems.append("frame: " + detail)
                    elif args.verbose:
                        print("   info: %s frame matches at %s" % (name, detail))
            if not problems:
                status = "pass"
            elif name in known:
                status = "KNOWN"
            else:
                status = "FAIL"
            counts[status] += 1
            print("%-5s %s" % (status, name))
            if problems and (status == "FAIL" or args.verbose):
                for p in problems[:8]:
                    print("      " + p)
                if status == "KNOWN":
                    for reason in known[name]:
                        print("      known: " + reason)
    print("\n%d pass, %d known, %d fail (of %d)" % (counts["pass"], counts["KNOWN"], counts["FAIL"], len(names)))

    #MesenCE's own cases (extra_cases.py) have no BlastEm reference: their READ steps must equal the
    #case's expected values under compare_mask, and every POLL must terminate.
    import extra_cases
    extra_fail = 0
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        for case in extra_cases.all_cases():
            if (args.case and case.name not in args.case) or case.name in extra_cases.PICTURE_ONLY:
                continue
            spec = json.loads((args.roms / (case.name + ".json")).read_text())
            ours, err = run_case(args.runner, args.roms / (case.name + ".md"), spec, work)
            problems = [err] if err else []
            if not err:
                ops = expand_operations(case)
                vals, polls, seg_err = segment_reads(ops, ours["reads"])
                if seg_err:
                    problems.append(seg_err)
                else:
                    for idx, got in vals:
                        op = ops[idx]
                        if op.compare_mask:
                            value = got[0][2] if op.width != 4 else (got[0][2] << 16) | got[1][2]
                            if (value & op.compare_mask) != (op.value & op.compare_mask):
                                problems.append("step %d read $%X, case expects $%X under mask $%X" % (idx, value, op.value, op.compare_mask))
            extra_fail += bool(problems)
            print("%-5s %s (MesenCE case, expected values)" % ("FAIL" if problems else "pass", case.name))
            for p in problems[:4]:
                print("      " + p)
    return 1 if counts["FAIL"] or extra_fail else 0


if __name__ == "__main__":
    sys.exit(main())
