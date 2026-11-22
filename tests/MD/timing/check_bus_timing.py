"""68000 bus timing: loop iterations per frame for one instruction under test.

Each cartridge (built here) runs {<instruction>; ADDQ.L #1,D0; BRA.S} forever; the VBlank
handler stores D0 per frame in work RAM ($FF0000 + 4n) and clears it. The count in a steady
frame reflects the instruction's cost including bus waits. Expected counts are MesenCE's own,
next to Genesis Plus GX and PicoDrive's for the same cartridges (tools/libretro-capture,
findings C4, Mega Turrican): a 68000 access to the Z80 area costs one more cycle while the
68000 holds the Z80 bus.

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

# name: (setup, instruction, expected MesenCE count, GPGX, PicoDrive)
CASES = {
    "nop": ("", "4E71", 5722, 5726, 5720),
    "z80_ram_read": (Z80_ON + BUS, "123900A00000", 3597, 3603, 3595),           # was 3702 without the wait
    "ym2612_write_pair": (Z80_ON + BUS, "13FC002B00A04000" "13FC000000A04001", 2098, 2100, 2097),  # was 2170
    "ym2612_write_busy": (Z80_ON + BUS, "13FC002B00A04000" "0839000700A04000" "66F6", 1855, 1854, 1850),  # was 1908
    "vdp_status_read": ("", "323900C00004", 3702, 3707, 3701),
    "io_pad_read": ("13FC004000A10009", "123900A10003", 3703, 3706, 3700),
    "z80_busreq_cycle": (Z80_ON, BUS + "33FC000000A11100", 1464, 1465, 1463),
}


def cartridge(setup, instruction):
    r = bytearray(0x20000)
    struct.pack_into(">II", r, 0, 0x00FF8000, 0x200)
    for v in range(2, 64):
        struct.pack_into(">I", r, v * 4, 0x400)
    struct.pack_into(">I", r, 0x78, 0x300)
    r[0x100:0x110] = b"SEGA GENESIS    "
    r[0x1F0:0x1F3] = b"JUE"
    struct.pack_into(">II", r, 0x1A0, 0, len(r) - 1)
    # VDP: mode 5, display off, VINT on; A1 = results; D0 = 0; then the setup and SR = $2300
    code = bytes.fromhex("41F900C00004" "30BC8004" "30BC8124" "43F900FF0000" "7000") + bytes.fromhex(setup) + bytes.fromhex("46FC2300")
    body = bytes.fromhex(instruction) + bytes.fromhex("5280")  # addq.l #1,d0
    code += body + struct.pack(">BB", 0x60, (-(len(body) + 2)) & 0xFF)
    r[0x200:0x200 + len(code)] = code
    # VINT: move.l d0,(a1)+; moveq #0,d0; wrap a1 at $FF0400; rte
    r[0x300:0x314] = bytes.fromhex("22C0" "7000" "B3FC00FF0400" "6606" "43F900FF0000" "4E73")
    r[0x400:0x402] = bytes.fromhex("60FE")
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
