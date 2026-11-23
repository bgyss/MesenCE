"""68000 bus timing: loop iterations per frame for one instruction under test.

Each cartridge (built here) runs {<instruction>; ADDQ.L #1,D0; BRA.S} forever; the VBlank
handler stores D0 per frame in work RAM ($FF0000 + 4n) and clears it. The count in a steady
frame reflects the instruction's cost including bus waits. Expected counts are MesenCE's own,
next to Genesis Plus GX and PicoDrive's for the same cartridges (tools/libretro-capture,
findings C4, Mega Turrican and The Lion King): a 68000 access to the Z80 area costs one more
cycle while the 68000 holds the Z80 bus, and a Z80 access to the 68000 bus takes that bus away
from the 68000 for about 11 cycles.

usage: check_bus_timing.py [mesen-headless]
"""
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
Z80_ON = "33FC010000A11200"                         # move.w #$100,$A11200: Z80 out of reset
BUS = "33FC010000A11100" "0839000000A11100" "66F6"   # request the Z80 bus, wait for BUSACK

def z80_program(code):
    """68000 setup: load `code` into Z80 RAM at 0 and start the Z80 on it (bus released)."""
    s = "33FC010000A11100" "33FC010000A11200" "0839000000A11100" "66F6"   # BUSREQ, reset off, wait for BUSACK
    for i, b in enumerate(code):
        s += "13FC00%02X00A0%04X" % (b, i)                                      # move.b #b,$A00000+i
    return s + "33FC000000A11200" "33FC000000A11100" "33FC010000A11200"      # reset, release the bus, run


# name: (setup, instruction, expected MesenCE count, GPGX, PicoDrive)
CASES = {
    "nop": ("", "4E71", 5722, 5726, 5720),
    "z80_ram_read": (Z80_ON + BUS, "123900A00000", 3597, 3603, 3595),           # was 3702 without the wait
    "ym2612_write_pair": (Z80_ON + BUS, "13FC002B00A04000" "13FC000000A04001", 2098, 2100, 2097),  # was 2170
    "ym2612_write_busy": (Z80_ON + BUS, "13FC002B00A04000" "0839000700A04000" "66F6", 1855, 1854, 1850),  # was 1908
    "vdp_status_read": ("", "323900C00004", 3702, 3707, 3701),
    "io_pad_read": ("13FC004000A10009", "123900A10003", 3703, 3706, 3700),
    "z80_busreq_cycle": (Z80_ON, BUS + "33FC000000A11100", 1464, 1465, 1463),
    # The Z80 runs on its own while the 68000 counts NOPs. Each Z80 access to the 68000 bus (bank
    # window, $7F00 VDP/PSG) takes the bus for about 11 68000 cycles (Kabuto's notes); the NOP
    # loop needs the bus all the time, so it loses them. MesenCE lost nothing before (5,723).
    "z80_idle_68k_nop": (z80_program([0x18, 0xFE]), "4E71", 5723, 5726, 5719),                         # jr $
    "z80_own_ram_68k_nop": (z80_program([0x3A, 0x00, 0x10, 0x18, 0xFB]), "4E71", 5723, 5726, 5719),    # ld a,($1000)
    "z80_bank_read_68k_nop": (z80_program([0x3A, 0x00, 0x80, 0x18, 0xFB]), "4E71", 4662, 4673, 4950),  # ld a,($8000)
    "z80_bank_read_10_nops_68k_nop": (z80_program([0x3A, 0x00, 0x80] + [0] * 10 + [0x18, 0xF1]), "4E71", 5291, 5316, 5402),
    "z80_bank_read_68k_ram_read": (z80_program([0x3A, 0x00, 0x80, 0x18, 0xFB]), "323900FF2000", 3024, 3023, 3204),
    # A DIVU loop barely needs the bus (long internal operation): the 68000 loses less per Z80
    # access in MesenCE (~7 cycles) than in GPGX (~10) or PicoDrive (~8), which charge it even
    # while the 68000 works internally. Judgment call: the 68000 gives up the bus and stalls only
    # at its next bus cycle (findings C4).
    "z80_bank_read_68k_divu": (z80_program([0x3A, 0x00, 0x80, 0x18, 0xFB]) + "7208" "7402", "82C2" "7208", 711, 666, 690),
    "z80_idle_68k_divu": (z80_program([0x18, 0xFE]) + "7208" "7402", "82C2" "7208", 807, 817, 796),
    "z80_psg_write_68k_nop": (z80_program([0x3E, 0x9F, 0x32, 0x11, 0x7F, 0x3A, 0x00, 0x18, 0x18, 0xF6]), "4E71", 5106, 5148, 5719),
}


def cartridge(setup, instruction):
    r = bytearray(0x20000)
    struct.pack_into(">II", r, 0, 0x00FF8000, 0x200)
    for v in range(2, 64):
        struct.pack_into(">I", r, v * 4, 0x1100)
    struct.pack_into(">I", r, 0x78, 0x1000)
    r[0x100:0x110] = b"SEGA GENESIS    "
    r[0x1F0:0x1F3] = b"JUE"
    struct.pack_into(">II", r, 0x1A0, 0, len(r) - 1)
    # VDP: mode 5, display off, VINT on; A1 = results; D0 = 0; then the setup and SR = $2300
    code = bytes.fromhex("41F900C00004" "30BC8004" "30BC8124" "43F900FF0000" "7000") + bytes.fromhex(setup) + bytes.fromhex("46FC2300")
    body = bytes.fromhex(instruction) + bytes.fromhex("5280")  # addq.l #1,d0
    code += body + struct.pack(">BB", 0x60, (-(len(body) + 2)) & 0xFF)
    assert 0x200 + len(code) <= 0x1000
    r[0x200:0x200 + len(code)] = code
    # VINT: move.l d0,(a1)+; moveq #0,d0; wrap a1 at $FF0400; rte
    r[0x1000:0x1014] = bytes.fromhex("22C0" "7000" "B3FC00FF0400" "6606" "43F900FF0000" "4E73")
    r[0x1100:0x1102] = bytes.fromhex("60FE")
    return bytes(r)


def main():
    runner = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "bin/mesen-headless"
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, (setup, instruction, expected, gpgx, pico) in CASES.items():
            rom = pathlib.Path(tmp) / (name + ".md")
            rom.write_bytes(cartridge(setup, instruction))
            out = pathlib.Path(tmp) / name
            subprocess.run([str(runner), "run", "--rom", str(rom), "--frames", "20", "--out", str(out), "--allow-dirty",
                            "--region", "ntsc", "--dump-at", "20"], check=True, capture_output=True)
            count = struct.unpack(">I", (out / "dumps/20/MdWorkRam.bin").read_bytes()[12:16])[0]
            ok = count == expected
            failed += not ok
            print("%s bus %-18s %5d per frame (expected %d; GPGX %d, PicoDrive %d)" % ("pass" if ok else "FAIL", name, count, expected, gpgx, pico))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
