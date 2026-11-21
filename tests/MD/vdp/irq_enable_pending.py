#!/usr/bin/env python3
"""Enabling a VDP interrupt that is already pending (findings C2, Fatal Rewind).

Fatal Rewind / The Killing Game Show leave VINT disabled over several VBlanks, then, mid-display,
enable HINT and VINT and jump to code that installs the VINT handler's pointer:

    move.w #$8014,$C00004   ; HINT on (HINT pending: the counter expired)
    move.w #$8164,$C00004   ; VINT on (a VBlank is still pending)
    jmp    ...              ; sets the VINT handler pointer later

On BlastEm (nativekit's trace) the 68000 takes HINT after the second write, and VINT never: an
interrupt enabled by a write in an instruction's last bus cycle is seen one instruction later, and
the VDP's acknowledge clears the enabled pending interrupt with the highest priority (VINT) rather
than the level the 68000 took. GPGX implements both (m68k_set_irq_delay, "IRQ latency (Fatal
Rewind, Sesame's Street Counting Cafe)"; vdp_68k_irq_ack, "VINT has higher priority (Fatal
Rewind)"), as does BlastEm (vdp_int_ack). MesenCE took HINT at once, cleared HINT on its
acknowledge, then took the stale VINT through a null pointer and crashed.

Two cartridges, each run in mesen-headless; results in work RAM:
  A (the game's sequence): HINT handler count ($FF0000) = 1, VINT handler count ($FF0002) = 0.
  B (VINT alone, control): the stale VINT is still taken (all three emulators agree): HINT 0,
    VINT 1, and the instruction after the enabling write ran first: $FF0008 (a copy of $FF0006
    made by the VINT handler) = 1. B runs with 0-39 NOPs before the write, so that a DRAM refresh
    stall falls inside the writing instruction in some of them (Sesame Street Counting Cafe: its
    VINT handler restores register 1 from a copy the game updates in the next instruction; taken
    one instruction early, VINT stayed off for good).
$FF0004 = $AA when the program finished.

Usage: irq_enable_pending.py   (runs bin/mesen-headless, or $MESEN_HEADLESS)
"""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/MD/synthetic"))
from gen import M68kAsm, BEQ, BNE, BRA  # noqa: E402

VDP_CTRL = (0x00C0, 0x0004)
STATUS_LO = (0x00C0, 0x0005)
HINT_HANDLER, VINT_HANDLER = 0x1000, 0x1100


def handler(origin, counter, disable_reg, copy_marker):
    a = M68kAsm(origin)
    a.raw(0x5279, 0x00FF, counter)                    # addq.w #1,$FFxxxx
    if copy_marker:
        a.raw(0x13F9, 0x00FF, 0x0006, 0x00FF, 0x0008)  # move.b $FF0006,$FF0008
    a.raw(0x33FC, disable_reg, *VDP_CTRL)             # move.w #reg,$C00004 (disable this interrupt)
    a.raw(0x4E73)                                     # rte
    return a.assemble()


def program(enable_hint, pad=0):
    a = M68kAsm(0x200)
    a.raw(0x46FC, 0x2700)                             # move #$2700,sr
    a.raw(0x33FC, 0x8004, *VDP_CTRL)                  # reg 0: HINT off
    a.raw(0x33FC, 0x8144, *VDP_CTRL)                  # reg 1: display on, VINT off, mode 5
    a.raw(0x33FC, 0x8A00, *VDP_CTRL)                  # reg 10: HINT counter expires every line
    a.raw(0x42B9, 0x00FF, 0x0000)                     # clr.l $FF0000
    a.raw(0x42B9, 0x00FF, 0x0004)                     # clr.l $FF0004
    a.raw(0x42B9, 0x00FF, 0x0008)                     # clr.l $FF0008
    a.label("w1")                                     # wait for VBlank (VINT pending, disabled)
    a.raw(0x0839, 0x0003, *STATUS_LO)                 # btst #3,$C00005
    a.branch(BEQ, "w1")
    a.label("w2")                                     # then for the active display
    a.raw(0x0839, 0x0003, *STATUS_LO)
    a.branch(BNE, "w2")
    for _ in range(pad):
        a.raw(0x4E71)                                 # nop (moves the write against the refresh)
    a.raw(0x46FC, 0x2000)                             # move #$2000,sr
    if enable_hint:
        a.raw(0x33FC, 0x8014, *VDP_CTRL)              # reg 0: HINT on
    a.raw(0x33FC, 0x8164, *VDP_CTRL)                  # reg 1: VINT on
    a.raw(0x13FC, 0x0001, 0x00FF, 0x0006)             # move.b #1,$FF0006
    for _ in range(8):
        a.raw(0x4E71)                                 # nop
    a.raw(0x46FC, 0x2700)                             # move #$2700,sr
    a.raw(0x13FC, 0x00AA, 0x00FF, 0x0004)             # move.b #$AA,$FF0004
    a.label("end")
    a.branch(BRA, "end")
    code = a.assemble()
    rom = bytearray(0x8000)
    rom[0:8] = struct.pack(">II", 0x00FFFF00, 0x200)
    rom[0x70:0x74] = struct.pack(">I", HINT_HANDLER)
    rom[0x78:0x7C] = struct.pack(">I", VINT_HANDLER)
    rom[0x100:0x110] = b"SEGA GENESIS    "
    rom[0x120:0x150] = b"MESENCE IRQ ENABLE PENDING".ljust(48, b" ")
    rom[0x150:0x180] = b"MESENCE IRQ ENABLE PENDING".ljust(48, b" ")
    rom[0x1F0:0x200] = b"JUE             "
    rom[0x200:0x200 + len(code)] = code
    h = handler(HINT_HANDLER, 0x0000, 0x8004, False)
    v = handler(VINT_HANDLER, 0x0002, 0x8144, True)
    rom[HINT_HANDLER:HINT_HANDLER + len(h)] = h
    rom[VINT_HANDLER:VINT_HANDLER + len(v)] = v
    return bytes(rom)


def run(rom_bytes, tmp, name):
    rom = tmp / (name + ".md")
    rom.write_bytes(rom_bytes)
    out = tmp / name
    proc = subprocess.run([os.environ.get("MESEN_HEADLESS", str(ROOT / "bin/mesen-headless")), "run", "--rom", str(rom),
                           "--frames", "10", "--out", str(out), "--allow-dirty"], capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stderr)
        sys.exit("FAIL irq enable pending (%s): runner exit %d" % (name, proc.returncode))
    ram = (out / "memory/MdWorkRam.bin").read_bytes()
    hint, vint = struct.unpack(">HH", ram[0:4])
    return hint, vint, ram[4], ram[8]


def main():
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        hint, vint, done, _ = run(program(True), tmp, "hint_then_vint")
        ok = (hint, vint, done) == (1, 0, 0xAA)
        failed |= not ok
        print("%s irq enable pending, hint_then_vint: HINT %d, VINT %d, done %#04x (expected HINT 1, VINT 0)" % (
            "ok  " if ok else "FAIL", hint, vint, done))
        bad = []
        for pad in range(40):
            hint, vint, done, copied = run(program(False, pad), tmp, "vint_alone_%d" % pad)
            if (hint, vint, done, copied) != (0, 1, 0xAA, 1):
                bad.append("pad %d: HINT %d VINT %d done %#04x before %d" % (pad, hint, vint, done, copied))
        failed |= bool(bad)
        print("%s irq enable pending, vint_alone over 40 refresh phases: %d wrong%s" % (
            "ok  " if not bad else "FAIL", len(bad), ("; " + "; ".join(bad[:4])) if bad else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
