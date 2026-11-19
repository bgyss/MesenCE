#!/usr/bin/env python3
"""Sega CD disc region from the boot program's security code (local only: the security code is
Sega's and comes from the BIOS dumps, which are not in the repository).

  disc_region.py [--bios-dir DIR] [--runner bin/mesen-headless]

The BIOS compares the first block of a disc's boot program (sector 0, $200) with its own copy and
refuses a disc from another region, so that block tells the region the disc runs in; the header
region field at $1F0 can disagree (Snatcher (U) says "J", findings S5). For the US and European
BIOS dumps found locally, the script takes the BIOS's copy of the block (located by its own length,
lea / jsr / bra.w past the data, and the CRC32 MdConsole::GetSecurityCodeRegion checks), puts it in
the synthetic disc (tests/MD/synthetic/mcd_cases.py) under a header that says "J", and reads the
version register $A10001 after power-on with region auto:
  - US block: overseas (bit 7 = 1), NTSC (bit 6 = 0);
  - European block: overseas, PAL (bit 6 = 1);
  - no known block (the synthetic disc as is, header "J"): domestic (bit 7 = 0), the header field.
Missing BIOS dumps are skipped. Copyright notice and license: GPL-3.0.
"""
import argparse
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tests/MD/synthetic"))
sys.path.insert(0, str(HERE))
import mcd_cases  # noqa: E402
from bios_boot import BIOSES, find_bioses  # noqa: E402

CRCS = {0xF361AB57: "us", 0x6FFB4732: "eu"}  # Core/MD/MdConsole.cpp GetSecurityCodeRegion
LUA = """
emu.addEventCallback(function()
  emu.log(string.format("version=%02X", emu.read(0xA10001, emu.memType.mdMemory, false)))
  emu.stop(0)
end, emu.eventType.endFrame)
"""


def security_block(bios):
    """The block of `bios` whose length (from its bra.w at +8) and CRC32 match a known region."""
    for i in range(0, len(bios) - 12, 2):
        if bios[i + 8:i + 10] != b"\x60\x00":
            continue
        length = 0x0A + struct.unpack_from(">H", bios, i + 10)[0]
        block = bios[i:i + length]
        if len(block) == length and zlib.crc32(block) in CRCS:
            return CRCS[zlib.crc32(block)], block
    return None, None


def disc_with(folder, name, block):
    raw, cue = mcd_cases.build_disc()
    raw = bytearray(raw)
    user = 16  # sector 0's user data in the raw 2352-byte sector
    raw[user + 0x1F0:user + 0x1F3] = b"J  "
    if block:
        raw[user + 0x200:user + 0x200 + len(block)] = block
    (folder / (name + ".bin")).write_bytes(raw)
    (folder / (name + ".cue")).write_text(cue.replace("mcd_disc.bin", name + ".bin"))
    return folder / (name + ".cue")


def version(runner, cue, bios, folder):
    out = folder / ("run-" + cue.stem)
    r = subprocess.run([str(runner), "run", "--rom", str(cue), "--frames", "5", "--out", str(out), "--allow-dirty",
                        "--md-mcd-bios", str(bios), "--lua", str(folder / "version.lua")], capture_output=True, text=True)
    log = (out / "lua.log").read_text() if (out / "lua.log").exists() else r.stderr
    line = next((l for l in log.splitlines() if l.startswith("version=")), None)
    return int(line[8:], 16) if line else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios-dir", type=pathlib.Path,
                    default=pathlib.Path(os.environ.get("MESEN_MCD_BIOS_DIR", pathlib.Path.home() / "mame/roms/segacd")))
    ap.add_argument("--runner", type=pathlib.Path, default=ROOT / "bin/mesen-headless")
    args = ap.parse_args()
    found = find_bioses(args.bios_dir)
    blocks = {}
    for name, sha, region, _ in BIOSES:
        if sha in found and region in ("ntsc", "pal"):
            kind, block = security_block(found[sha].read_bytes())
            if kind and kind not in blocks:
                blocks[kind] = (block, found[sha])
    if not blocks:
        print("skipped: no US or European Sega CD BIOS in %s" % args.bios_dir)
        return 0
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "version.lua").write_text(LUA)
        any_bios = next(iter(blocks.values()))[1]
        cases = [(kind, disc_with(tmp, "disc_" + kind, block), bios) for kind, (block, bios) in sorted(blocks.items())]
        cases.append(("header", disc_with(tmp, "disc_header", None), any_bios))
        expect = {"us": (0x80, 0x00), "eu": (0x80, 0x40), "header": (0x00, 0x00)}
        for kind, cue, bios in cases:
            v = version(args.runner, cue, bios, tmp)
            ok = v is not None and (v & 0x80, v & 0x40) == expect[kind]
            failures += not ok
            got = "none" if v is None else "$%02X" % v
            print("%-5s %s security code, header \"J\": version register %s" % ("pass" if ok else "FAIL", kind, got))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
