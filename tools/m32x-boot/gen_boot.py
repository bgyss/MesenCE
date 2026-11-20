#!/usr/bin/env python3
"""Generate MesenCE's replacement 32X boot ROMs (Core/MD/M32x/M32xBootRom.h).

These are not copies of Sega's boot ROMs. They are written for MesenCE from the boot protocol in
Sega's 32X Hardware Manual (section 5.1, figures 5.1-5.3), so 32X software and the synthetic test
cartridges can run without the user's BIOS files (CI can't hold them):

  master: purge and enable the cache; load the initial data described by the MARS user header at
          cartridge $3C0 (source, destination, size) from the cartridge into SDRAM; wait 16 frames
          (stands in for the boot ROM's own run time, see below); write the header's checksum word
          to comm8 and "M_OK" to comm0; set VBR and jump to the master
          start address.
  slave:  purge and enable the cache; wait for "M_OK" in comm0; write "S_OK" to comm4; set VBR and
          jump to the slave start address.
  68000:  the vector ROM: SSP 0 and vectors pointing at the 6-byte jump table at $880200 (manual,
          "ROM Access When Using the 32X"), plus two helpers at $C0/$D4 that write the bank set
          registers with RV = 1 (carts call them by address).

Skipped from Sega's flow chart: the SDRAM test and clear, security, the BSC set-up and the sleep
until the 68000's initial program wakes the CPUs. The register state handed to the application is
documented in docs/research/32x-verification-findings.md (S2).

    python3 tools/m32x-boot/gen_boot.py   # rewrites Core/MD/M32x/M32xBootRom.h
"""
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/MD/synthetic"))
from sh2asm import assemble  # noqa: E402

COMMON_LITERALS = """
    .align 4
ccr:    .long 0xFFFFFE92
hdr:    .long 0x220003C0
cart:   .long 0x22000000
sdram:  .long 0x26000000
csum:   .long 0x2200018E
comm:   .long 0x20004020
mok:    .long 0x4D5F4F4B    ; "M_OK"
sok:    .long 0x535F4F4B    ; "S_OK"
fbctl:  .long 0x2000410A    ; frame buffer control (VBLK)
sysreg: .long 0x20004000
"""

CACHE_ON = """
    mov.l ccr,r1
    mov #0x10,r0        ; CP: purge all ways (cache disabled)
    mov.b r0,@r1
    mov #0x01,r0        ; CE: enable, 4-way
    mov.b r0,@r1
"""

BOOT_VBLANKS = 16

MASTER = """
start:
""" + CACHE_ON + """
    mov.l hdr,r8
    mov.l @(0x14,r8),r1 ; source: cartridge offset
    mov.l cart,r2
    add r2,r1
    mov.l @(0x18,r8),r3 ; destination: SDRAM offset
    mov.l sdram,r2
    add r2,r3
    mov.l @(0x1C,r8),r4 ; size in bytes (a multiple of 4)
    shlr2 r4
    tst r4,r4
    bt copied
copy:
    mov.l @r1+,r0
    mov.l r0,@r3
    dt r4
    bf/s copy
    add #4,r3
copied:
    ; Hand over after BOOT_VBLANKS frames. Sega's boot ROM takes about 80 frames (SDRAM test, security)
    ; and carts' 68000 start-up code relies on finishing first: it swaps the frame buffer with
    ; FM = 0 before it waits for the checksum, and the application's SH-2 code takes FM (Toughman
    ; Contest, Tempo). Judgment call, findings S5.
    mov.l fbctl,r5
    mov #BOOT_VBLANKS,r6
vwait:
    mov.w @r5,r0
    cmp/pz r0           ; VBLK (bit 15) = 0?
    bt vwait
vblank:
    mov.w @r5,r0
    cmp/pz r0
    bf vblank
    dt r6
    bf vwait
    mov.l csum,r1
    mov.w @r1,r0
    mov.l comm,r1
    mov.w r0,@(8,r1)    ; checksum -> comm8
    mov.l mok,r0
    mov.l r0,@r1        ; "M_OK" -> comm0
    mov.l sysreg,r1     ; GBR = system registers, as Sega's boot ROMs leave it (RBI Baseball 95
    ldc r1,gbr          ; reads FM with @(0,GBR) before setting GBR itself; findings S5)
    mov.l @(0x28,r8),r0
    ldc r0,vbr
    mov.l @(0x20,r8),r0
    jmp @r0
    nop
hang:
    bra hang
    nop
""" + COMMON_LITERALS

SLAVE = """
start:
""" + CACHE_ON + """
    mov.l comm,r1
    mov.l mok,r2
wait:
    mov.l @r1,r0
    cmp/eq r2,r0
    bf wait
    mov.l sok,r0
    mov.l r0,@(4,r1)    ; "S_OK" -> comm4
    mov.l hdr,r8
    mov.l sysreg,r1     ; GBR = system registers, as Sega's boot ROMs leave it (RBI Baseball 95
    ldc r1,gbr          ; reads FM with @(0,GBR) before setting GBR itself; findings S5)
    mov.l @(0x2C,r8),r0
    ldc r0,vbr
    mov.l @(0x24,r8),r0
    jmp @r0
    nop
hang:
    bra hang
    nop
""" + COMMON_LITERALS


def boot_rom(source, size, stack):
    code, labels = assemble(source.replace("BOOT_VBLANKS", str(BOOT_VBLANKS)), 0x200)
    rom = bytearray(size)
    vectors = [labels["start"], stack, labels["start"], stack] + [labels["hang"]] * 124
    for i, v in enumerate(vectors):
        rom[i * 4:i * 4 + 4] = struct.pack(">I", v)
    rom[0x200:0x200 + len(code)] = code
    assert 0x200 + len(code) <= size
    return bytes(rom)


# 68000 helpers in the vector ROM's reserved slots ($C0-$FF). Carts call them by address after the
# boot handshake (Knuckles' Chaotix: `jsr ($00C0).w` with a1 = $A130F1): the bank set registers
# need RV = 1 while they are written (manual p. 17, "The RV bit should be set to 1 beforehand").
# Interface found by running the cart; hand-assembled here (findings S5).
RV_SET = bytes.fromhex("08F9000000A15107")    # bset #0,$A15107.l
RV_CLEAR = bytes.fromhex("08B9000000A15107")  # bclr #0,$A15107.l
HELPER_WRITE_BYTE = (                          # $C0: write d0.b to (a1) with RV = 1
    RV_SET
    + bytes.fromhex("1280")                    # move.b d0,(a1)
    + RV_CLEAR
    + bytes.fromhex("4E75"))                   # rts
HELPER_WRITE_BANKS = (                         # $D4: write 8 bytes from (a0)+ to $A130F1-$A130FF
    bytes.fromhex("48E70140")                  # movem.l d7/a1,-(sp)
    + RV_SET
    + bytes.fromhex("43F900A130F1")            # lea $A130F1,a1
    + bytes.fromhex("7E07")                    # moveq #7,d7
    + bytes.fromhex("1298")                    # loop: move.b (a0)+,(a1)
    + bytes.fromhex("5489")                    # addq.l #2,a1
    + bytes.fromhex("51CFFFFA")                # dbf d7,loop
    + RV_CLEAR
    + bytes.fromhex("4CDF0280")                # movem.l (sp)+,d7/a1
    + bytes.fromhex("4E75"))                   # rts


def vector_rom():
    longs = [0] + [0x880200 + 6 * (i - 1) for i in range(1, 64)]
    rom = bytearray(b"".join(struct.pack(">I", v) for v in longs))
    assert len(HELPER_WRITE_BYTE) == 0xD4 - 0xC0 and 0xD4 + len(HELPER_WRITE_BANKS) <= 0x100
    rom[0xC0:0xD4] = HELPER_WRITE_BYTE
    rom[0xD4:0xD4 + len(HELPER_WRITE_BANKS)] = HELPER_WRITE_BANKS
    return bytes(rom)


def c_array(name, data):
    lines = ["\tstatic constexpr uint8_t %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 16):
        lines.append("\t\t" + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
    lines.append("\t};")
    return "\n".join(lines)


def main():
    master = boot_rom(MASTER, 0x800, 0x06040000)
    slave = boot_rom(SLAVE, 0x400, 0x0603F000)
    out = ROOT / "Core/MD/M32x/M32xBootRom.h"
    out.write_text("\n".join([
        "#pragma once",
        "#include <cstdint>",
        "",
        "// MesenCE's replacement 32X boot ROMs, generated by tools/m32x-boot/gen_boot.py (do not edit).",
        "// Written from the boot protocol in Sega's 32X Hardware Manual (section 5.1); not Sega's code.",
        "namespace M32xBootRom",
        "{",
        c_array("Master", master),
        c_array("Slave", slave),
        c_array("Vectors", vector_rom()),
        "}",
        "",
    ]))
    print("wrote", out)


if __name__ == "__main__":
    main()
