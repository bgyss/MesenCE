#!/usr/bin/env python3
"""Mid-line rendering: a CRAM write during active display must split the line at the beam.

Runs the vdp_midline_cram_h32/h40 cartridges (tests/MD/synthetic/extra_cases.py) on
mesen-headless and checks final.png: the background is black above the first change, the change
line is black then red with a single transition strictly inside the line, the lines after it are
red up to a second change line (red then black), and black below. With line-granularity
rendering (each line drawn from the state at its start) no line can contain both colours.

  check_midline.py ROM_DIR [--runner bin/mesen-headless]
"""
import argparse, json, pathlib, subprocess, sys, tempfile, zlib

ROOT = pathlib.Path(__file__).resolve().parents[3]
BLACK, RED = (0, 0, 0), (255, 0, 0)


def read_png(path):
    """Minimal PNG reader for the runner's 8-bit RGB/RGBA, non-interlaced output."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, width = 8, b"", 0
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height = int.from_bytes(body[0:4], "big"), int.from_bytes(body[4:8], "big")
            depth, ctype, interlace = body[8], body[9], body[12]
            assert depth == 8 and ctype in (2, 6) and interlace == 0, "unsupported PNG"
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
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
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(width)])
        prev = line
    return rows


def classify(row):
    """('solid', colour) or ('split', left, right, column) for a row with one transition."""
    cuts = [x for x in range(1, len(row)) if row[x] != row[x - 1]]
    if not cuts:
        return ("solid", row[0])
    if len(cuts) == 1:
        return ("split", row[0], row[-1], cuts[0])
    return ("multi", cuts)


def check(rows):
    kinds = [classify(r) for r in rows]
    splits = [y for y, k in enumerate(kinds) if k[0] != "solid"]
    if len(splits) != 2:
        return "expected two split lines, found %s" % [(y, kinds[y]) for y in splits[:6]]
    top, bottom = splits
    if kinds[top][:3] != ("split", BLACK, RED) or kinds[bottom][:3] != ("split", RED, BLACK):
        return "split lines %d %s and %d %s are not black->red and red->black" % (top, kinds[top], bottom, kinds[bottom])
    for y, k in enumerate(kinds):
        want = BLACK if y < top or y > bottom else RED if top < y < bottom else None
        if want and k != ("solid", want):
            return "line %d is %s, expected solid %s" % (y, k, want)
    return None, top, kinds[top][3], bottom, kinds[bottom][3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roms", type=pathlib.Path)
    ap.add_argument("--runner", type=pathlib.Path, default=ROOT / "bin/mesen-headless")
    args = ap.parse_args()
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name in ("vdp_midline_cram_h32", "vdp_midline_cram_h40"):
            spec = json.loads((args.roms / (name + ".json")).read_text())
            out = pathlib.Path(tmp) / name
            r = subprocess.run([str(args.runner), "run", "--rom", str(args.roms / (name + ".md")), "--frames",
                                str(spec["frames"] + 2), "--out", str(out), "--allow-dirty"], capture_output=True, text=True)
            if r.returncode:
                print("FAIL  %s: runner %s" % (name, r.stderr[-300:]))
                bad += 1
                continue
            result = check(read_png(out / "final.png"))
            if isinstance(result, str):
                print("FAIL  %s: %s" % (name, result))
                bad += 1
            else:
                _, top, x1, bottom, x2 = result
                print("pass  %s: line %d splits at x=%d, line %d at x=%d" % (name, top, x1, bottom, x2))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
