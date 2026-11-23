#!/usr/bin/env python3
"""Sega CD 1M word RAM swap handshake: the main CPU's request against the sub CPU's RET write
(findings S5, MegaRace).

MegaRace's main CPU requests a 1M swap with a read-modify-write loop,
    again: bclr #1,$A12003 / beq again      (DMNA = 0 requests a swap: Hardware Manual p. 57)
    wait:  btst #1,$A12003 / bne wait       (DMNA reads 0 once the swap is done: p. 24)
and its sub CPU answers from a polling loop,
    top: btst #1,$8003.w / beq top / btst #0,$8003.w / beq set / bclr #0,$8003.w ...
The sub CPU's RET write comes about 34.4-50 main cycles after the main CPU's first request write;
the second bclr writes again about 34 cycles after it. In time order the sub CPU's write comes
last and completes the swap. The emulator ran whole sub instructions past the main CPU's
position, so a sub write could be applied before an earlier main write: the request was re-armed
after the swap and the main CPU waited forever (MegaRace froze on the SEGA screen).

This cartridge runs both loops, with the instruction sequences of the game, for 64 main-CPU entry
phases (a dbra delay of 0-63 iterations). Like the game's, the sub CPU answers one request per
round (it then waits for the main CPU's flag to change), so a re-armed request is never answered. Each phase must finish its swap within a timeout; the
results go to work RAM $FF0000-$FF003F (0 = swapped, 1 = timed out), then $AA at $FF0040.

Usage: swap_race.py   (runs bin/mesen-headless, or $MESEN_HEADLESS)
"""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
PHASES = 64


sys.path.insert(0, str(ROOT / "tests/MD/synthetic"))
from gen import M68kAsm, BEQ, BNE, BRA  # noqa: E402


def sub_program():
    """Sub CPU program at $000200 in program RAM (MegaRace's sub CPU loop, $6CA2-$6CD8)."""
    a = M68kAsm(0x200)
    a.raw(0x13FC, 0x0005, 0x00FF, 0x8003)     # move.b #$05,$FF8003: MODE = 1 (1M), RET = 1
    a.raw(0x13FC, 0x0001, 0x00FF, 0x800F)     # move.b #$01,$FF800F: sub flag = 1
    a.label("top")
    a.raw(0x0838, 0x0001, 0x8003)             # btst #1,$8003.w (DMNA: swap requested)
    a.branch(BEQ, "top")
    a.raw(0x0838, 0x0000, 0x8003)             # btst #0,$8003.w (RET)
    a.branch(BEQ, "set")
    a.raw(0x08B8, 0x0000, 0x8003)             # bclr #0,$8003.w
    a.label("w1")
    a.raw(0x0838, 0x0000, 0x8003)             # btst #0,$8003.w
    a.branch(BNE, "w1")
    a.branch(BRA, "done")
    a.label("set")
    a.raw(0x08F8, 0x0000, 0x8003)             # bset #0,$8003.w
    a.label("w2")
    a.raw(0x0838, 0x0000, 0x8003)             # btst #0,$8003.w
    a.branch(BEQ, "w2")
    # One swap per round, as in the game: wait for the main CPU's next round (its flag changes)
    a.label("done")
    a.raw(0x1039, 0x00FF, 0x800E)             # move.b $FF800E,d0
    a.label("round")
    a.raw(0xB039, 0x00FF, 0x800E)             # cmp.b $FF800E,d0
    a.branch(BEQ, "round")
    a.branch(BRA, "top")
    code = a.assemble()
    image = {0: 0x0007, 2: 0xFF00, 4: 0x0000, 6: 0x0200}   # SSP $0007FF00, PC $000200
    for i in range(0, len(code), 2):
        image[0x200 + i] = struct.unpack(">H", code[i:i + 2])[0]
    return image


RACE_ORIGIN = 0x1000


def race_routine():
    """The main CPU's side, run from work RAM $FF1000 like the game's (ROM fetches would add
    refresh delays and change the timing)."""
    a = M68kAsm(0xFF0000 + RACE_ORIGIN)
    a.raw(0x7E00)                             # moveq #0,d7 (phase)
    a.raw(0x41F9, 0x00FF, 0x0000)             # lea $FF0000,a0
    a.label("outer")
    a.raw(0x3207)                             # move.w d7,d1
    a.label("delay")
    a.raw(0x51C9, 0xFFFE)                     # dbra d1,delay
    # MegaRace $FF058E-$FF05A0, the game's swap request
    a.label("again")
    a.raw(0x08B9, 0x0001, 0x00A1, 0x2003)     # bclr #1,$A12003
    a.branch(BEQ, "again")
    a.raw(0x2C3C, 0x0000, 0x2000)             # move.l #$2000,d6 (timeout)
    a.label("wait")
    a.raw(0x0839, 0x0001, 0x00A1, 0x2003)     # btst #1,$A12003
    a.branch(BEQ, "ok")
    a.raw(0x5386)                             # subq.l #1,d6
    a.branch(BNE, "wait")
    a.raw(0x10FC, 0x0001)                     # move.b #1,(a0)+ (timed out)
    a.branch(BRA, "next")
    a.label("ok")
    a.raw(0x10FC, 0x0000)                     # move.b #0,(a0)+
    a.label("next")
    a.raw(0x5247)                             # addq.w #1,d7
    a.raw(0x13C7, 0x00A1, 0x200E)             # move.b d7,$A1200E: next round
    a.raw(0x0C47, PHASES)                     # cmpi.w #PHASES,d7
    a.branch(BNE, "outer")
    a.raw(0x10FC, 0x00AA)                     # move.b #$AA,(a0)+
    a.label("end")
    a.raw(0x60FE)                             # bra *
    return a.assemble()


def main_program():
    a = M68kAsm(0x200)
    a.raw(0x46FC, 0x2700)                     # move #$2700,sr
    a.raw(0x13FC, 0x0002, 0x00A1, 0x2001)     # $A12001 = $02: reset, bus request
    for addr, w in sorted(sub_program().items()):
        a.raw(0x33FC, w, 0x0042, addr)        # move.w #w,$420000+addr
    a.raw(0x13FC, 0x0001, 0x00A1, 0x2001)     # $A12001 = $01: run
    a.label("flag")
    a.raw(0x1039, 0x00A1, 0x200F)             # move.b $A1200F,d0
    a.raw(0x0C00, 0x0001)                     # cmpi.b #1,d0
    a.branch(BNE, "flag")
    race = race_routine()
    for i in range(0, len(race), 2):
        a.raw(0x33FC, struct.unpack(">H", race[i:i + 2])[0], 0x00FF, RACE_ORIGIN + i)  # move.w #w,$FFxxxx
    a.raw(0x4EF9, 0x00FF, RACE_ORIGIN)        # jmp RACE_ORIGIN
    code = a.assemble()
    rom = bytearray(0x8000)
    rom[0:8] = struct.pack(">II", 0x00FFFF00, 0x200)
    rom[0x100:0x110] = b"SEGA GENESIS    "
    rom[0x120:0x150] = b"MESENCE MCD SWAP RACE".ljust(48, b" ")
    rom[0x150:0x180] = b"MESENCE MCD SWAP RACE".ljust(48, b" ")
    rom[0x1F0:0x200] = b"JUE             "
    rom[0x200:0x200 + len(code)] = code
    return bytes(rom)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        rom = tmp / "mcd_swap_race.md"
        rom.write_bytes(main_program())
        out = tmp / "run"
        proc = subprocess.run([os.environ.get("MESEN_HEADLESS", str(ROOT / "bin/mesen-headless")), "run", "--rom", str(rom), "--frames", "300",
                               "--out", str(out), "--allow-dirty", "--md-mcd"], capture_output=True, text=True)
        if proc.returncode != 0:
            print(proc.stderr)
            sys.exit(f"FAIL mcd swap race: runner exit {proc.returncode}")
        ram = (out / "memory/MdWorkRam.bin").read_bytes()
    results, done = ram[:PHASES], ram[PHASES]
    if done != 0xAA:
        sys.exit(f"FAIL mcd swap race: did not finish (marker {done:#04x})")
    hung = [i for i, r in enumerate(results) if r]
    if hung:
        sys.exit(f"FAIL mcd swap race: the swap never completed for {len(hung)}/{PHASES} phases {hung[:16]}")
    print(f"ok   mcd swap race ({PHASES} phases of MegaRace's 1M swap handshake complete)")


if __name__ == "__main__":
    main()
