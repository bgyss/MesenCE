"""68000->VRAM DMA timing: a 4 KB DMA from ROM started when the V counter reads a chosen value;
the H/V counter read right after the 68000 resumes is checked.

Each cartridge (built here, no assembler needed) programs the VDP (display on or off, H32 or
H40), waits for a frame start, then for the start line, writes the DMA command and reads the
H/V counter into work RAM ($FF0000 = after, $FF0002 = before). mesen-headless runs it and dumps
work RAM.

Expected V values: Genesis Plus GX (and PicoDrive within a line) measured with
tools/libretro-capture on the same cartridges (findings C4, Mega Turrican). The cases across the
end of VBlank check that the last line (261) gets the active-display rate.

usage: check_dma_timing.py [mesen-headless]
"""
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]

# name: (h40, display on, start V, expected V after; GPGX / PicoDrive values in the comment)
CASES = {
    "h32_on_vblank_only": (False, True, 0xE1, 0xF3),    # GPGX $F3, PicoDrive $F3
    "h32_off_across_end": (False, False, 0xF7, 0x0F),   # $0F, $0F
    "h32_on_active_only": (False, True, 100, 0xE6),     # $E6, $E6
    "h32_on_across_end": (False, True, 0xF7, 0xAD),     # $AD, $AC (MesenCE was $A3 before the fix)
    "h40_on_across_end": (True, True, 0xF7, 0x89),      # $89, $88 (MesenCE was $7E)
}


def cartridge(h40, display, start_v, words=0x800):
    r = bytearray(0x40000)
    struct.pack_into(">II", r, 0, 0x00FF8000, 0x200)
    for v in range(2, 64):
        struct.pack_into(">I", r, v * 4, 0x400)
    r[0x100:0x110] = b"SEGA GENESIS    "
    r[0x1F0:0x1F3] = b"JUE"
    struct.pack_into(">II", r, 0x1A0, 0, len(r) - 1)
    code = bytes.fromhex("41F900C00004" "43F900C00008" "30BC8004")      # lea VDP ctrl,a0; lea HV,a1; reg 0
    code += struct.pack(">HH", 0x30BC, 0x8114 | (0x40 if display else 0))  # reg 1: DMA on, mode 5
    code += struct.pack(">HH", 0x30BC, 0x8C81 if h40 else 0x8C00)
    code += bytes.fromhex("30BC8F02")
    for reg, val in ((19, words & 0xFF), (20, words >> 8), (21, 0x00), (22, 0x80), (23, 0x00)):  # source $010000
        code += struct.pack(">HH", 0x30BC, 0x8000 | (reg << 8) | val)
    code += bytes.fromhex("1011" "6600FFFC")                            # wait for V = 0
    code += bytes.fromhex("1011") + struct.pack(">HH", 0x0C00, start_v) + bytes.fromhex("66F8")  # wait for V = start
    code += bytes.fromhex("3611" "20BC40000080" "3211")                 # H/V; VRAM DMA to 0; H/V
    code += bytes.fromhex("33C100FF0000" "33C300FF0002" "60FE")
    r[0x200:0x200 + len(code)] = code
    r[0x400:0x402] = bytes.fromhex("60FE")
    return bytes(r)


# The Z80 counts its reads in its own RAM ($1000) while the 68000 runs a 4 KB 68000->VRAM DMA in
# active display; the 68000 samples the count before and after (BUSREQ). A Z80 access to the
# 68000 bus (bank window) waits for the DMA to finish (Kabuto's notes; GPGX +0, PicoDrive +119,
# MesenCE was +119 too); a Z80 loop on its own RAM runs on (GPGX +165, PicoDrive +164).
Z80_DMA_CASES = {"z80_bank_reads_during_dma": (True, 0), "z80_ram_reads_during_dma": (False, 164)}


def z80_dma_cartridge(bank):
    r = bytearray(cartridge(False, True, 100))
    z = [0x3A, 0x00, 0x80 if bank else 0x18, 0x21, 0x00, 0x10, 0x34, 0x18, 0xF7]  # ld a,(nn); ld hl,$1000; inc (hl); jr
    s = "33FC010000A11100" "33FC010000A11200" "0839000000A11100" "66F6"           # BUSREQ, reset off, BUSACK
    for i, b in enumerate(z):
        s += "13FC00%02X00A0%04X" % (b, i)
    s += "33FC000000A11200" "33FC000000A11100" "33FC010000A11200"                # reset, release, run
    code = bytes.fromhex(s)
    code += bytes.fromhex("41F900C00004" "43F900C00008" "30BC8004" "30BC8154" "30BC8C00" "30BC8F02")
    for reg, val in ((19, 0x00), (20, 0x08), (21, 0x00), (22, 0x80), (23, 0x00)):
        code += struct.pack(">HH", 0x30BC, 0x8000 | (reg << 8) | val)
    code += bytes.fromhex("1011" "6600FFFC" "1011") + struct.pack(">HH", 0x0C00, 100) + bytes.fromhex("66F8")
    sample = "33FC010000A11100" "0839000000A11100" "66F6" "%s" "33FC000000A11100"
    code += bytes.fromhex(sample % "183900A01000") + bytes.fromhex("20BC40000080") + bytes.fromhex(sample % "1A3900A01000")
    code += bytes.fromhex("13C400FF0000" "13C500FF0002" "60FE")
    r[0x200:0x200 + 0x200] = bytes(0x200)
    r[0x200:0x200 + len(code)] = code
    return bytes(r)


def main():
    runner = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "bin/mesen-headless"
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, (h40, display, start_v, expected) in CASES.items():
            rom = pathlib.Path(tmp) / (name + ".md")
            rom.write_bytes(cartridge(h40, display, start_v))
            out = pathlib.Path(tmp) / name
            subprocess.run([str(runner), "run", "--rom", str(rom), "--frames", "6", "--out", str(out), "--allow-dirty",
                            "--region", "ntsc", "--dump-at", "6"], check=True, capture_output=True)
            after, before = struct.unpack(">HH", (out / "dumps/6/MdWorkRam.bin").read_bytes()[:4])
            ok = after >> 8 == expected and before >> 8 == start_v
            failed += not ok
            print("%s dma %-20s start V $%02X, resumed at V $%02X (expected $%02X)" % ("pass" if ok else "FAIL", name, before >> 8, after >> 8, expected))
        for name, (bank, expected) in Z80_DMA_CASES.items():
            rom = pathlib.Path(tmp) / (name + ".md")
            rom.write_bytes(z80_dma_cartridge(bank))
            out = pathlib.Path(tmp) / name
            subprocess.run([str(runner), "run", "--rom", str(rom), "--frames", "6", "--out", str(out), "--allow-dirty",
                            "--region", "ntsc", "--dump-at", "6"], check=True, capture_output=True)
            ram = (out / "dumps/6/MdWorkRam.bin").read_bytes()
            got = (ram[2] - ram[0]) & 0xFF
            ok = got == expected
            failed += not ok
            print("%s dma %-26s Z80 count +%d during the DMA (expected +%d)" % ("pass" if ok else "FAIL", name, got, expected))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
