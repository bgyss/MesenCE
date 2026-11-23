#!/usr/bin/env python3
"""Bank-switching cartridge boards: Super Fighter Team's SF-004 and the Radica units (findings C3).

Each case is a hand-assembled cartridge whose program is copied into every bank, so that it keeps
running whatever the board maps at $000000. The program reads markers (each bank holds its own
number) and the board's registers, and stores every value in work RAM ($FF0000 up, one word per
read); the script runs it in mesen-headless and compares the words with the expected list.

SF-004 (Star Odyssey; GPGX md_cart.c mapper_sf004_w/_r, PicoDrive carthw.c carthw_sf004_*):
  8 x 256 KiB banks. Registers written at $00xD00 (bit 7: battery RAM at $200001, odd bytes),
  $00xE00 (bit 7 clear: lock the registers; bit 6: five banks in a row from the selected one,
  nothing above $140000; else the selected bank mirrored over $000000-$1FFFFF) and $00xF00
  (bits 6-4: the bank). Reading $A130xx returns the bank number in bits 6-4.
Radica (GPGX mapper_64k_radica_r): a read of $A130xx selects index = A6-A1; 64 KiB page i of
  $000000-$3FFFFF then shows ROM page (index | i). Detected by header checksum and word sum (the
  Volume 1 pair, $24F4/$FC84, made to match here with a fix-up word).

Usage: mapper_boards.py   (runs bin/mesen-headless, or $MESEN_HEADLESS)
"""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/MD/synthetic"))
from gen import M68kAsm, BRA  # noqa: E402

CODE_SPAN = 0x8000  # vectors, header and program: copied into every bank


class Program:
    def __init__(self):
        self.a = M68kAsm(0x200)
        self.a.raw(0x46FC, 0x2700)  # move #$2700,sr
        self.slot = 0
        self.expected = []

    def read_word(self, addr, expected, what):
        self.a.raw(0x33F9, addr >> 16, addr & 0xFFFF, 0x00FF, self.slot)  # move.w addr,$FFxxxx
        self.expected.append((expected, what, 0xFFFF))
        self.slot += 2

    def read_byte(self, addr, expected, what):
        self.a.raw(0x13F9, addr >> 16, addr & 0xFFFF, 0x00FF, self.slot + 1)  # move.b addr,$FFxxxx+1
        self.expected.append((expected, what, 0x00FF))
        self.slot += 2

    def write_byte(self, addr, value):
        self.a.raw(0x13FC, value, addr >> 16, addr & 0xFFFF)  # move.b #value,addr

    def touch(self, addr):
        self.a.raw(0x4A79, addr >> 16, addr & 0xFFFF)  # tst.w addr

    def rom(self, size, title, kind):
        self.a.label("end")
        self.a.branch(BRA, "end")
        code = self.a.assemble()
        rom = bytearray(b"\xFF" * size)
        rom[0:8] = struct.pack(">II", 0x00FFFF00, 0x200)
        rom[0x100:0x110] = b"SEGA GENESIS    "
        rom[0x120:0x150] = title.ljust(48).encode()
        rom[0x150:0x180] = title.ljust(48).encode()
        rom[0x180:0x18E] = kind.ljust(14).encode()
        rom[0x1F0:0x200] = b"JUE".ljust(16)
        rom[0x200:0x200 + len(code)] = code
        return rom


def copy_code(rom, bank_size):
    for base in range(bank_size, len(rom), bank_size):
        rom[base:base + CODE_SPAN] = rom[0:CODE_SPAN]


def sf004():
    p = Program()
    # Power-on: bank 0 mirrored over $000000-$1FFFFF, registers writable, battery RAM off
    p.read_word(0x010000, 0xB000, "bank 0 at $010000")
    p.read_word(0x1D0000, 0xB000, "bank 0 mirrored at $1D0000")
    p.read_byte(0xA13001, 0x00, "$A13001: bank 0")
    p.read_word(0x200000, 0xFFFF, "$200000: nothing (battery RAM off)")
    # $F00: bank 3
    p.write_byte(0x000F01, 0x30)
    p.read_word(0x010000, 0xB003, "bank 3 at $010000")
    p.read_word(0x1D0000, 0xB003, "bank 3 mirrored at $1D0000")
    p.read_byte(0xA13001, 0x30, "$A13001: bank 3")
    # $E00 bit 6: five banks in a row from bank 3, nothing above $140000
    p.write_byte(0x000E01, 0xC0)
    p.read_word(0x010000, 0xB003, "row: bank 3 at $010000")
    p.read_word(0x050000, 0xB004, "row: bank 4 at $050000")
    p.read_word(0x110000, 0xB007, "row: bank 7 at $110000")
    p.read_word(0x150000, 0xFFFF, "row: nothing at $150000")
    # $D00 bit 7: battery RAM on the odd bytes of $200000
    p.write_byte(0x000D01, 0x80)
    p.write_byte(0x200001, 0x5A)
    p.read_byte(0x200001, 0x5A, "battery RAM at $200001")
    p.write_byte(0x000D01, 0x00)
    p.read_word(0x200000, 0xFFFF, "battery RAM off again")
    # $E00 = 0: back to the mirror, and the registers lock
    p.write_byte(0x000E01, 0x00)
    p.write_byte(0x000F01, 0x50)
    p.read_byte(0xA13001, 0x30, "locked: still bank 3")
    p.read_word(0x050000, 0xB003, "locked: bank 3 mirrored at $050000")
    p.write_byte(0x000D01, 0x80)
    p.read_word(0x200000, 0xFFFF, "locked: battery RAM stays off")

    rom = p.rom(0x200000, "MESENCE SF-004 BOARD", "SF-004")
    copy_code(rom, 0x40000)
    for bank in range(8):
        rom[bank * 0x40000 + 0x10000:bank * 0x40000 + 0x10002] = struct.pack(">H", 0xB000 | bank)
    return rom, p.expected


def radica():
    p = Program()
    p.read_word(0x058000, 0xC005, "power-on: page 5 at $050000")
    p.touch(0xA13020)  # index $10
    p.read_word(0x058000, 0xC015, "index $10: page $15 at $050000")
    p.read_word(0x158000, 0xC015, "index $10: page $15 at $150000 (OR, not add)")
    p.read_word(0x208000, 0xC030, "index $10: page $30 at $200000")
    p.touch(0xA1307E)  # index $3F: the menu's own page everywhere
    p.read_word(0x008000, 0xC03F, "index $3F: page $3F at $000000")
    p.read_word(0x1F8000, 0xC03F, "index $3F: page $3F at $1F0000")
    p.touch(0xA13000)
    p.read_word(0x008000, 0xC000, "index 0: page 0 at $000000")
    p.read_word(0x3F8000, 0xC03F, "index 0: page $3F at $3F0000")

    rom = p.rom(0x400000, "MESENCE RADICA BOARD", "GM 00000000-00")
    copy_code(rom, 0x10000)
    for page in range(64):
        rom[page * 0x10000 + 0x8000:page * 0x10000 + 0x8002] = struct.pack(">H", 0xC000 | page)
    # Header checksum and word sum of Radica Volume 1 (MdMemoryManager::InitMapper's table)
    rom[0x18E:0x190] = struct.pack(">H", 0x24F4)
    rom[0x3FFFFE:0x400000] = b"\x00\x00"
    total = sum(struct.unpack(">%dH" % ((len(rom) - 0x200) // 2), bytes(rom[0x200:]))) & 0xFFFF
    rom[0x3FFFFE:0x400000] = struct.pack(">H", (0xFC84 - total) & 0xFFFF)
    return rom, p.expected


def run(name, rom, expected, tmp):
    path = tmp / (name + ".md")
    path.write_bytes(rom)
    out = tmp / name
    proc = subprocess.run([os.environ.get("MESEN_HEADLESS", str(ROOT / "bin/mesen-headless")), "run", "--rom", str(path),
                           "--frames", "5", "--out", str(out), "--allow-dirty"], capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stderr)
        return ["runner exit %d" % proc.returncode]
    ram = (out / "memory/MdWorkRam.bin").read_bytes()
    bad = []
    for i, (value, what, mask) in enumerate(expected):
        got = struct.unpack(">H", ram[i * 2:i * 2 + 2])[0] & mask
        if got != value:
            bad.append("%s: %#06x, expected %#06x" % (what, got, value))
    return bad


def main():
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        for name, build in (("sf004", sf004), ("radica", radica)):
            rom, expected = build()
            bad = run(name, rom, expected, tmp)
            failed |= bool(bad)
            print("%s mapper %s: %d reads%s" % ("ok  " if not bad else "FAIL", name, len(expected),
                                                 ("; " + "; ".join(bad[:6])) if bad else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
