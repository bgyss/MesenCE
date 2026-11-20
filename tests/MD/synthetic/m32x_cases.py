# 32X cases: synthetic 32X cartridges that boot through MesenCE's replacement boot ROMs (no Sega
# BIOS, so they run in CI), with SH-2 programs and 68000 bus operations
# (docs/research/32x-feasibility.md S2, 32x-verification-findings.md).
#
# Cartridge layout: $000 vectors (68000 reset to $200), $100 "SEGA 32X", $200 a stub that copies
# a trampoline into work RAM and jumps to it; the trampoline sets ADEN and RES ($A15101 = 3) and
# continues at $880800, the cartridge's mirror (with ADEN = 1 the cartridge leaves $000000), where
# gen.py's position-independent bus-operation interpreter runs the case's steps. $3C0 holds the
# MARS user header; the SH-2 image (vector tables, programs) is at $1000 and the boot ROM copies it
# to SDRAM $06000000. Expected values come from Sega's 32X Hardware Manual (pages cited per case).
# Copyright notice and license: GPL-3.0.
import struct
import zlib

import cases
from cases import Case, Operation, READ, WRITE, POLL
import gen
from sh2asm import assemble

IMAGE_CART = 0x1000
IMAGE_SDRAM = 0x06000000
SLAVE_VBR = IMAGE_SDRAM + 0x200
INTERP = 0x800
COMM = 0xA15120

#Common SH-2 definitions: GBR = $20004000 (system registers) in every program
PRELUDE = """
    mov.l sysreg,r0
    ldc r0,gbr
    mov.l stack_top,r15
"""

LITERALS = """
    .align 4
sysreg:     .long 0x20004000
fbreg:      .long 0x20004100
palreg:     .long 0x20004200
fb:         .long 0x24000000
sdram_ct:   .long 0x26000000
"""


def _comm(i):
    return COMM + 2 * i


def _poll_comm(i, value):
    return Operation(POLL, 2, _comm(i), value, compare_mask=0xFFFF)


def _read(addr, value, mask=0xFFFF, width=2):
    return Operation(READ, width, addr, value, compare_mask=mask)


def _image(master, slave):
    """SDRAM image: master vectors at $000, slave vectors at $200, code from $400."""
    src = ".space 0x400\n" + master + "\nslave_entry:\n" + slave + "\nhang:\n bra hang\n nop\n" + LITERALS
    code, labels = assemble(src, IMAGE_SDRAM)
    image = bytearray(code)

    def vectors(base, handlers, stack):
        table = [labels["hang"]] * 128
        table[0] = labels.get("master_entry" if base == 0 else "slave_entry")
        table[1] = stack
        for vec, name in handlers.items():
            table[vec] = labels[name]
        for i, v in enumerate(table):
            image[base + i * 4:base + i * 4 + 4] = struct.pack(">I", v)

    vectors(0x000, {v: n for v, n in _HANDLERS["master"]}, 0x0603FC00)
    vectors(0x200, {v: n for v, n in _HANDLERS["slave"]}, 0x0603F800)
    while len(image) % 4:
        image.append(0)
    return bytes(image), labels


_HANDLERS = {"master": [], "slave": []}


def build_32x(case):
    """The cartridge for a 32X case (gen.py calls this instead of gen.build)."""
    spec = _SPECS[case.name]
    _HANDLERS["master"] = spec.get("master_irq", [])
    _HANDLERS["slave"] = spec.get("slave_irq", [])
    image, labels = _image(spec["master"], spec["slave"])
    rom = bytearray(case.rom_size)
    rom[0:8] = struct.pack(">II", 0x00FFFF00, 0x00000200)
    rom[0x100:0x110] = b"SEGA 32X        "
    rom[0x120:0x150] = b"MESENCE 32X TEST".ljust(48, b" ")
    rom[0x150:0x180] = b"MESENCE 32X TEST".ljust(48, b" ")
    rom[0x18E:0x190] = struct.pack(">H", 0xBEEF)  # checksum word: the boot ROM copies it to comm4
    rom[0x1F0:0x200] = b"JUE             "

    # $200: copy the trampoline to $FF0000 and jump there (68000 code, encoded by hand)
    tramp = struct.pack(">HHHH", 0x13FC, 0x0003, 0x00A1, 0x5101)          # move.b #3,($A15101).l
    tramp += struct.pack(">HI", 0x4EF9, 0x880000 | INTERP)                 # jmp ($880800).l
    tramp += b"\x4E\x71" * ((-len(tramp)) % 4 // 2)
    stub = b""
    for i in range(0, len(tramp), 4):
        stub += struct.pack(">HII", 0x23FC, struct.unpack(">I", tramp[i:i + 4])[0], 0xFF0000 + i)  # move.l #x,(abs).l
    stub += struct.pack(">HI", 0x4EF9, 0xFF0000)                           # jmp ($FF0000).l
    rom[0x200:0x200 + len(stub)] = stub
    assert 0x200 + len(stub) <= 0x3C0

    # $3C0: MARS user header (manual figure 5.1)
    rom[0x3C0:0x3D0] = b"MARS CHECK MODE "
    rom[0x3D0:0x3F0] = struct.pack(">8I", 0, IMAGE_CART, 0, len(image),
                                   labels["master_entry"], labels["slave_entry"], IMAGE_SDRAM, SLAVE_VBR)

    program = gen.interpreter(gen.HEADER.size) + gen.encode_steps(case, list(case.operations))
    assert INTERP + len(program) <= IMAGE_CART, case.name + ": steps overlap the SH-2 image"
    rom[INTERP:INTERP + len(program)] = program
    rom[IMAGE_CART:IMAGE_CART + len(image)] = image
    assert IMAGE_CART + len(image) <= 0x4000
    rom[0x4000:0x4008] = bytes((0x01, 0x23, 0x04, 0x56, 0x07, 0x89, 0x0A, 0xBC))
    for offset, value in case.rom_data:
        rom[offset] = value
    return bytes(rom)


_SPECS = {}


def _case(name, ops, master, slave="sleep_loop:\n bra sleep_loop\n nop\n", master_irq=(), slave_irq=(), frames=4,
          rom_size=0x8000, rom_data=(), render=False):
    _SPECS[name] = {"master": "master_entry:\n" + PRELUDE.replace("stack_top", "mstack") + master + "\n .align 4\nmstack: .long 0x0603FC00\n",
                    "slave": PRELUDE.replace("stack_top", "sstack") + slave + "\n .align 4\nsstack: .long 0x0603F800\n",
                    "master_irq": list(master_irq), "slave_irq": list(slave_irq)}
    return Case(name, tuple(ops), capture_frames=frames, rom_size=rom_size, rom_data=tuple(rom_data), render=render)


def _wait_boot():
    """The boot ROMs' handshake (manual 5.1, figure 5.2/5.3): "M_OK" in comm0-1, "S_OK" in comm2-3."""
    return [_poll_comm(0, 0x4D5F), _read(_comm(1), 0x4F4B), _poll_comm(2, 0x535F), _read(_comm(3), 0x4F4B)]


IDLE = "idle:\n bra idle\n nop\n"
#Lower the interrupt mask (SR.I = 0) and enable the listed 32X interrupts (mask register, p. 22)
def _irq_on(mask):
    return " mov #%d,r0\n mov.w r0,@(0,gbr)\n mov #0,r0\n ldc r0,sr\n" % mask


def _boot():
    """Boot handshake and identification.

    - The 32X ID at $A130EC reads "MARS" (manual figure 3.1).
    - Adapter control $A15100: ADEN (bit 0) and RES (bit 1) read back as written (p. 19).
    - The master boot ROM writes the header's checksum word to comm4 ("Check SUM -> comm8"), then
      "M_OK"; the slave waits for "M_OK" and writes "S_OK" (figures 5.2, 5.3).
    - Each application then writes its own word: master $A5A5 to comm6, slave $5A5A to comm7.
    """
    ops = _wait_boot() + [_read(_comm(4), 0xBEEF), _poll_comm(6, 0xA5A5), _poll_comm(7, 0x5A5A),
                          _read(0xA130EC, 0x4D41), _read(0xA130EE, 0x5253), _read(0xA15100, 0x0003, mask=0x0003)]
    master = " mov.w a5a5,r0\n mov.w r0,@(0x2C,gbr)\n" + IDLE + "a5a5: .word 0xA5A5\n"
    slave = " mov.w s5a5a,r0\n mov.w r0,@(0x2E,gbr)\n" + IDLE.replace("idle", "sidle") + "s5a5a: .word 0x5A5A\n"
    return _case("m32x_boot", ops, master, slave)


def _cmd_irq():
    """Command interrupt (manual p. 20 INTM/INTS, p. 22 mask, pp. 69 and 'Points to be aware of').

    The master enables CMD (mask bit 1); the slave doesn't. A write of INTM ($A15103 bit 0) raises
    level 8 on the master: its handler counts in comm6 and clears the request ($401A). INTM/INTS
    read back while a request is pending. INTS with the slave's CMD masked stays pending (bit 1
    reads 1) and the slave takes no interrupt. Writing 0 clears the requests: the bits read back as
    written (findings S2: Sega's Mars Check Program, test 3, writes $02 and expects $02 back).
    """
    master = _irq_on(0x02) + " mov #1,r0\n mov.w r0,@(0x2E,gbr)\n" + IDLE + """
cmd_handler:
    mov.w @(0x2C,gbr),r0
    add #1,r0
    mov.w r0,@(0x2C,gbr)
    mov.w r0,@(0x1A,gbr)
    rte
    nop
"""
    ops = _wait_boot() + [_poll_comm(7, 1), _read(0xA15102, 0, mask=3),
                          Operation(WRITE, 1, 0xA15103, 1), _poll_comm(6, 1), _read(0xA15102, 0, mask=1),
                          Operation(WRITE, 1, 0xA15103, 1), _poll_comm(6, 2),
                          Operation(WRITE, 1, 0xA15103, 2), _read(0xA15102, 2, mask=3), _read(_comm(6), 2),
                          Operation(WRITE, 1, 0xA15103, 0), _read(0xA15102, 0, mask=3)]
    return _case("m32x_cmd_irq", ops, master, master_irq=[(68, "cmd_handler")])


def _vint_hint():
    """V and H interrupts (manual pp. 22-23, 69): V at each VBlank; H every HCount + 1 lines of the
    display (HEN = 0: not in VBlank). With HCount = 1, a frame of 224 lines gives 112 H interrupts.
    The V handler copies the frame's H count to comm6 and counts frames in comm5."""
    master = """
    mov #1,r0
    mov.w r0,@(4,gbr)       ; HCount = 1
    mov #0,r9               ; H count
""" + _irq_on(0x0C) + " mov #1,r0\n mov.w r0,@(0x2E,gbr)\n" + IDLE + """
h_handler:
    add #1,r9
    mov.w r0,@(0x18,gbr)
    rte
    nop
v_handler:
    mov r9,r0
    mov.w r0,@(0x2C,gbr)
    mov #0,r9
    mov.w @(0x2A,gbr),r0
    add #1,r0
    mov.w r0,@(0x2A,gbr)
    mov.w r0,@(0x16,gbr)
    rte
    nop
"""
    ops = _wait_boot() + [_poll_comm(7, 1), _poll_comm(5, 3), _read(_comm(6), 112)]
    return _case("m32x_vint_hint", ops, master, master_irq=[(70, "v_handler"), (69, "h_handler")], frames=6)


def _hint_dma():
    """H interrupts across a long 68000 stall (findings S2: Mars Check Program tests 55/56).

    HEN = 1, HCount = 0: an H interrupt on every line, VBlank included, so 262 per NTSC frame. The
    68000 runs a 2048-word 68000-to-VRAM DMA (it waits for the whole transfer) right after a
    VBlank, then reads the 32X: every line's interrupt must still reach the SH-2, so the frame that
    holds the DMA also counts 262 (MesenCE lost the lines the VDP caught up in one go)."""
    master = """
    mov #0,r0
    mov.w r0,@(4,gbr)       ; HCount = 0
    mov #0,r9
    mov.w hmask,r0          ; HEN, V, H
    mov.w r0,@(0,gbr)
    mov #0,r0
    ldc r0,sr
    mov #1,r0
    mov.w r0,@(0x2E,gbr)
""" + IDLE + """
h_handler:
    add #1,r9
    mov.w r0,@(0x18,gbr)
    rte
    nop
v_handler:
    mov r9,r0
    mov.w r0,@(0x2C,gbr)
    mov #0,r9
    mov.w @(0x2A,gbr),r0
    add #1,r0
    mov.w r0,@(0x2A,gbr)
    mov.w r0,@(0x16,gbr)
    rte
    nop
hmask: .word 0x008C
"""
    vdp = [Operation(WRITE, 2, 0xC00004, 0x8000 | (r << 8) | v)
           for r, v in ((0, 0x04), (1, 0x54), (12, 0x81), (15, 0x02), (19, 0x00), (20, 0x08), (21, 0x00), (22, 0x80), (23, 0x7F))]
    dma = Operation(WRITE, 4, 0xC00004, cases._vdp_command(0x21, 0))
    ops = _wait_boot() + vdp + [_poll_comm(7, 1), _poll_comm(5, 2), _read(_comm(6), 262), dma, _poll_comm(5, 3),
                                _read(_comm(6), 262), _poll_comm(5, 4), _read(_comm(6), 262)]
    return _case("m32x_hint_dma", ops, master, master_irq=[(70, "v_handler"), (69, "h_handler")], frames=7)


def _sdram_shared():
    """Both SH-2s share SDRAM (manual figure 3.2): the master stores $12345678 at $06010000 (through
    the cache-through address), the slave reads it after the master's flag and reports the low
    word + 1 in comm7."""
    master = """
    mov.l val,r1
    mov.l addr,r2
    mov.l r1,@r2
    mov #1,r0
    mov.w r0,@(0x2C,gbr)
""" + IDLE + " .align 4\nval: .long 0x12345678\naddr: .long 0x26010000\n"
    slave = """
swait:
    mov.w @(0x2C,gbr),r0
    cmp/eq #1,r0
    bf swait
    mov.l saddr,r2
    mov.l @r2,r1
    add #1,r1
    mov r1,r0
    mov.w r0,@(0x2E,gbr)
sidle:
    bra sidle
    nop
 .align 4
saddr: .long 0x26010000
"""
    ops = _wait_boot() + [_poll_comm(7, 0x5679)]
    return _case("m32x_sdram_shared", ops, master, slave)


def _dreq_fifo():
    """68000 -> SH-2 DREQ (manual pp. 21, 44-46): with 68S set and length 8, the 68000 writes eight
    words to the FIFO ($A15112); the master's DMA channel 0 (DREQ0, $20004012 -> SDRAM, word units)
    moves them; 68S clears after the eighth word. The master sums the words (1..8 = 36) into comm6."""
    master = """
    mov.l sar,r1
    mov.l dmac,r2
    mov.l r1,@(0,r2)        ; SAR0 = $20004012 (FIFO)
    mov.l dar,r1
    mov.l r1,@(4,r2)        ; DAR0 = $26020000
    mov #8,r1
    mov.l r1,@(8,r2)        ; TCR0 = 8
    mov.l chcr,r1
    mov.l r1,@(12,r2)       ; CHCR0: DM = 1, SM = 0, TS = word, DREQ, DE
    mov.l dmaor,r2
    mov #1,r1
    mov.l r1,@r2            ; DMAOR: DME
    mov #1,r0
    mov.w r0,@(0x2E,gbr)    ; ready
    mov.l dmac,r2
wait_te:
    mov.l @(12,r2),r0
    tst #2,r0
    bt wait_te
    mov.l dar,r1
    mov #8,r3
    mov #0,r4
sum:
    mov.w @r1+,r0
    extu.w r0,r0
    add r0,r4
    dt r3
    bf sum
    mov r4,r0
    mov.w r0,@(0x2C,gbr)
""" + IDLE + """
 .align 4
sar:   .long 0x20004012
dar:   .long 0x26020000
dmac:  .long 0xFFFFFF80
dmaor: .long 0xFFFFFFB0
chcr:  .long 0x00004401
"""
    ops = _wait_boot() + [_poll_comm(7, 1), Operation(WRITE, 2, 0xA15110, 8), Operation(WRITE, 2, 0xA15106, 4),
                          _read(0xA15106, 4, mask=4)]
    ops += [Operation(WRITE, 2, 0xA15112, v) for v in range(1, 9)]
    ops += [_poll_comm(6, 36), _read(0xA15106, 0, mask=4)]
    return _case("m32x_dreq_fifo", ops, master)


def _frame_buffer():
    """Frame buffer fill and swap (manual pp. 47-51, VDP registers p. 24-27).

    The master takes the VDP (FM = 1), selects packed-pixel mode, fills 16 words at word address
    $100 with $ABCD (length register = count - 1), waits for FEN to clear, and reads the frame
    buffer back: word $100 and word $10F hold $ABCD, word $110 is still 0 (comm6/comm5 report
    $ABCD/$ABCD, comm4 0). It then waits for the display (VBLK = 0), writes FS = 1 and reads FS:
    still 0 until VBlank, then 1 (comm7 = $10 + the FS read before VBlank, so $10).
    """
    master = """
    mov.w fm,r0
    mov.w r0,@(0,gbr)       ; FM = 1
    mov.l fbreg,r8
    mov #1,r0
    mov.w r0,@(0,r8)        ; packed pixel
    mov #15,r0
    mov.w r0,@(4,r8)        ; fill length - 1
    mov.w faddr,r0
    mov.w r0,@(6,r8)        ; fill address
    mov.w fdata,r0
    mov.w r0,@(8,r8)        ; fill data: starts the fill
fen:
    mov.w @(10,r8),r0
    tst #2,r0
    bf fen
    mov.l fbw,r1
    mov.w @r1,r0
    mov.w r0,@(0x2C,gbr)
    mov.w @(30,r1),r0
    mov.w r0,@(0x2A,gbr)
    mov r1,r2
    add #32,r2
    mov.w @r2,r0
    mov.w r0,@(0x28,gbr)
vdisp:
    mov.w @(10,r8),r0
    shll r0                 ; T = VBLK
    bt vdisp
    mov #1,r0
    mov.w r0,@(10,r8)       ; FS = 1
    mov.w @(10,r8),r0
    and #1,r0
    mov r0,r9               ; FS before VBlank
vwait:
    mov.w @(10,r8),r0
    tst #1,r0
    bt vwait
    mov r9,r0
    add #0x10,r0
    mov.w r0,@(0x2E,gbr)    ; comm7 = $10 + FS before VBlank
""" + IDLE + """
fm:    .word 0x8000
faddr: .word 0x0100
fdata: .word 0xABCD
 .align 4
fbw:   .long 0x24000200
"""
    ops = _wait_boot() + [_poll_comm(7, 0x10), _read(_comm(6), 0xABCD), _read(_comm(5), 0xABCD), _read(_comm(4), 0)]
    return _case("m32x_frame_buffer", ops, master, frames=4)


def _fen_refresh():
    """FEN reads 1 while the frame buffer DRAM is refreshed, in a short window at the start of each
    HBlank (findings S5: Metal Head waits for FEN = 1 without a fill; BlastEm's window, ares reports
    it during the VDP's refresh slots). The master polls FEN with no fill running and reports
    comm7 = 1 once it has seen FEN = 1, then FEN = 0 again."""
    master = """
    mov.w fm,r0
    mov.w r0,@(0,gbr)       ; FM = 1
    mov.l fbreg,r8
fen1:
    mov.w @(10,r8),r0
    tst #2,r0
    bt fen1
fen0:
    mov.w @(10,r8),r0
    tst #2,r0
    bf fen0
    mov #1,r0
    mov.w r0,@(0x2E,gbr)
""" + IDLE + "fm: .word 0x8000\n"
    ops = _wait_boot() + [_poll_comm(7, 1)]
    return _case("m32x_fen_refresh", ops, master)


def _vdp_68k():
    """68000 access to the 32X VDP and palette while FM = 0 (manual pp. 14, 24-29): the palette is
    word-accessible ($A15200), the bitmap mode register reads back mode/PRI and bit 15 = 1 on NTSC
    (U region). With FM = 1 the 68000 reads FM back as 1 ($A15100 bit 15)."""
    ops = _wait_boot() + [Operation(WRITE, 2, 0xA15202, 0x7C1F), _read(0xA15202, 0x7C1F),
                          Operation(WRITE, 2, 0xA15180, 0x0081), _read(0xA15180, 0x8081, mask=0x80C3),
                          Operation(WRITE, 2, 0xA15100, 0x8003), _read(0xA15100, 0x8003, mask=0x8003)]
    return _case("m32x_vdp_68k", ops, IDLE)


def _cart_map():
    """Cartridge mapping with ADEN = 1 (manual pp. 13-14, bank set register p. 20):

    - $880000-$8FFFFF is the first 512 KiB of the cartridge: $884000 reads the fixture at $4000;
    - $900000-$9FFFFF is a 1 MiB bank: bank 0 shows $004000 at $904000, bank 1 shows $104000;
    - the 68000 vectors come from the vector ROM: vector 1 = $00880200 (p. 14);
    (RV, which gives the 68000 the original map and hides $880000, needs the code to run from RAM;
    not covered here.)
    - the SH-2 reads the cartridge at $22004000 (comm6).
    """
    ops = _wait_boot() + [_read(0x884000, 0x0123), _read(0x904000, 0x0123),
                          Operation(WRITE, 1, 0xA15105, 1), _read(0x904000, 0x4567), Operation(WRITE, 1, 0xA15105, 0),
                          _read(0x000004, 0x0088), _read(0x000006, 0x0200), _poll_comm(6, 0x0123)]
    master = " mov.l cart,r1\n mov.w @r1,r0\n mov.w r0,@(0x2C,gbr)\n" + IDLE + " .align 4\ncart: .long 0x22004000\n"
    return _case("m32x_cart_map", ops, master, rom_size=0x200000, rom_data=((0x104000, 0x45), (0x104001, 0x67)))


def _pwm():
    """PWM (manual pp. 30-35): control = L and R on, TM = 1 (an interrupt every sample); cycle
    = $400. Three words fill the L FIFO (bit 15 FULL reads 1, comm6 = $8000); the PWM interrupt
    (level 6) counts in comm5 while the FIFO drains; once 8 interrupts have passed the FIFO is
    empty (bit 14, comm4 = $4000)."""
    master = """
    mov.w cycle,r0
    mov.w r0,@(0x32,gbr)
    mov.w sample,r0
    mov.w r0,@(0x34,gbr)
    mov.w r0,@(0x34,gbr)
    mov.w r0,@(0x34,gbr)
    mov.w @(0x34,gbr),r0
    mov.w mask,r1
    and r1,r0
    mov.w r0,@(0x2C,gbr)
    mov #0,r9
    mov.w ctrl,r0
    mov.w r0,@(0x30,gbr)    ; start: L and R on, TM = 1
""" + _irq_on(0x01) + """
cnt:
    mov #8,r0
    cmp/hs r0,r9
    bf cnt
    mov.w @(0x34,gbr),r0
    mov.w mask,r1
    and r1,r0
    mov.w r0,@(0x28,gbr)
    mov r9,r0
    mov.w r0,@(0x2A,gbr)
""" + IDLE + """
pwm_handler:
    add #1,r9
    mov.w r0,@(0x1C,gbr)
    rte
    nop
cycle:  .word 0x0400
ctrl:   .word 0x0105
sample: .word 0x0200
mask:   .word 0xC000
"""
    ops = _wait_boot() + [_poll_comm(6, 0x8000), _poll_comm(4, 0x4000), _read(_comm(5), 8, mask=0xFFF8)]
    return _case("m32x_pwm", ops, master, master_irq=[(67, "pwm_handler")])


def _render():
    """The 32X picture over the Mega Drive's (manual pp. 36-43, figure 3.9 priority).

    Packed-pixel mode, 224 lines: the line table points every line at word $100; the line's 320
    dots are colour 1 for x < 160 and colour 2 after. Palette 1 = green ($03E0), palette 2 = blue
    ($7C00). The Mega Drive shows only its backdrop (display on, no planes), PRI = 0: the 32X shows
    through the backdrop everywhere. Checked on the final picture (derived check): (40, 100) is
    green, (280, 100) blue."""
    master = """
    mov.w fm,r0
    mov.w r0,@(0,gbr)
    mov.l palreg,r1
    mov.w green,r0
    mov.w r0,@(2,r1)
    mov.w blue,r0
    mov.w r0,@(4,r1)
    mov.l fb,r1             ; line table: 256 entries of $0100
    mov.w line,r2
    mov #0,r3
    mov.w lines,r4
table:
    mov.w r2,@r1
    add #2,r1
    dt r4
    bf table
    mov.l fbline,r1         ; dots: 80 words of $0101, 80 of $0202
    mov.w c1,r2
    mov #80,r4
dots1:
    mov.w r2,@r1
    add #2,r1
    dt r4
    bf dots1
    mov.w c2,r2
    mov #80,r4
dots2:
    mov.w r2,@r1
    add #2,r1
    dt r4
    bf dots2
    mov.l fbreg,r8
    mov #1,r0
    mov.w r0,@(10,r8)       ; FS = 1: show what was drawn (swaps at VBlank)
    mov #1,r0
    mov.w r0,@(0,r8)        ; packed pixel, PRI = 0
    mov #1,r0
    mov.w r0,@(0x2E,gbr)
""" + IDLE + """
fm:    .word 0x8000
green: .word 0x03E0
blue:  .word 0x7C00
line:  .word 0x0100
lines: .word 256
c1:    .word 0x0101
c2:    .word 0x0202
 .align 4
fbline: .long 0x24000200
"""
    vdp = [Operation(WRITE, 2, 0xC00004, 0x8000 | (r << 8) | v) for r, v in ((0, 0x04), (1, 0x44), (12, 0x81), (7, 0x00))]
    cram = [Operation(WRITE, 4, 0xC00004, 0xC0000000), Operation(WRITE, 2, 0xC00000, 0x000E)]  # colour 0 = red
    ops = _wait_boot() + vdp + cram + [_poll_comm(7, 1)]
    return _case("m32x_render", ops, master, frames=5, render=True)


def _md_planes():
    """Mega Drive side for the priority cases: H40 display on; planes A and B at $E000 (all tile
    0), tile 0 solid colour 1 = blue ($0E00), background colour 0 = red: every MD pixel is a
    non-transparent plane pixel (colour code 1)."""
    ops = [Operation(WRITE, 2, 0xC00004, 0x8000 | (r << 8) | v)
           for r, v in ((0, 0x04), (1, 0x44), (12, 0x81), (2, 0x38), (4, 0x07), (7, 0x00), (15, 0x02))]
    ops.append(Operation(WRITE, 4, 0xC00004, 0x40000000))  # VRAM write at $0000
    ops += [Operation(WRITE, 4, 0xC00000, 0x11111111)] * 8
    ops.append(Operation(WRITE, 4, 0xC00004, 0xC0000000))  # CRAM write at 0
    ops += [Operation(WRITE, 2, 0xC00000, 0x000E), Operation(WRITE, 2, 0xC00000, 0x0E00)]
    return ops


def _draw_case(name, mode, pri, line_words, palette, pixels, doc, dot_shift=False, md=None):
    """Fills the line table (every line -> word $100), writes `line_words` at $100 and the palette,
    then shows the buffer in `mode` with PRI = `pri`. `pixels` = {(x, y): (r, g, b)} on the final
    picture (derived check)."""
    words = "\n".join(" .word 0x%04X" % w for w in line_words)
    pal = "".join(" mov.w p%d,r0\n mov.w r0,@(%d,r1)\n" % (i, i * 2) for i in range(len(palette)))
    pal_data = "\n".join("p%d: .word 0x%04X" % (i, c) for i, c in enumerate(palette))
    master = """
    mov.w fm,r0
    mov.w r0,@(0,gbr)
    mov.l palreg,r1
""" + pal + """
    mov.l fb,r1
    mov.w line,r2
    mov.w lines,r4
table:
    mov.w r2,@r1
    add #2,r1
    dt r4
    bf table
    mov.l fbline,r1
    mova data,r0
    mov r0,r3
    mov.w count,r4
copy:
    mov.w @r3+,r0
    mov.w r0,@r1
    add #2,r1
    dt r4
    bf copy
    mov.l fbreg,r8
    mov #%d,r0
    mov.w r0,@(2,r8)        ; dot shift
    mov #1,r0
    mov.w r0,@(10,r8)       ; FS = 1
    mov.w modereg,r0
    mov.w r0,@(0,r8)
    mov #1,r0
    mov.w r0,@(0x2E,gbr)
""" % (1 if dot_shift else 0) + IDLE + """
fm:      .word 0x8000
line:    .word 0x0100
lines:   .word 256
count:   .word %d
modereg: .word 0x%04X
%s
 .align 4
fbline:  .long 0x24000200
data:
%s
""" % (len(line_words), mode | (0x80 if pri else 0), pal_data, words)
    base = md if md is not None else _md_planes()
    ops = _wait_boot() + base + [_poll_comm(7, 1)]
    PIXEL_CHECKS[name] = pixels
    case = _case(name, ops, master, frames=5, render=True)
    return case


PIXEL_CHECKS = {}


def _priority_32x():
    """PRI = 1, direct colour (manual p. 46 figure 3.9, p. 47): the 32X is in front of the MD
    planes except where a pixel's through bit is set. Left half $001F (red, through 0) shows the
    32X; right half $83E0 (green, through 1) shows the MD plane (blue)."""
    words = [0x001F] * 160 + [0x83E0] * 160
    return _draw_case("m32x_priority_32x", 2, True, words, [], {(40, 100): (255, 0, 0), (280, 100): (0, 0, 255)}, "")


def _priority_md():
    """PRI = 0, run-length mode (manual p. 46, p. 48): the MD planes are in front of 32X pixels
    without the through bit. Runs: 160 dots of colour 1 ($001F red, through 0) -> MD blue shows;
    160 dots of colour 2 ($83E0 green, through 1) -> 32X green in front. Run word = (count - 1) << 8
    | colour."""
    words = [0x9F01, 0x9F02]
    return _draw_case("m32x_priority_md", 3, False, words, [0x0000, 0x001F, 0x83E0],
                      {(40, 100): (0, 0, 255), (280, 100): (0, 255, 0)}, "")


def _dot_shift():
    """Packed pixel with the screen shift bit (manual p. 25, packed pixel control): bytes cycle
    1, 2, 3 (red, green, blue); with SFT = 1 the line starts one dot later, so x = 0 shows byte 1
    (green), x = 1 blue, x = 2 red. MD backdrop only (display on, no planes)."""
    data = bytes(((i % 3) + 1) for i in range(322))
    words = [(data[i] << 8) | data[i + 1] for i in range(0, 322, 2)]
    md = [Operation(WRITE, 2, 0xC00004, 0x8000 | (r << 8) | v) for r, v in ((0, 0x04), (1, 0x44), (12, 0x81), (7, 0x00))]
    return _draw_case("m32x_dot_shift", 1, True, words, [0x0000, 0x001F, 0x03E0, 0x7C00],
                      {(0, 100): (0, 255, 0), (1, 100): (0, 0, 255), (2, 100): (255, 0, 0)}, "", dot_shift=True, md=md)


def _png_pixel(path, x, y):
    data = open(path, "rb").read()
    pos, chunks = 8, {}
    idat = b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            chunks["ihdr"] = (w, h, ctype)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    w, h, ctype = chunks["ihdr"]
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    i = 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for k in range(stride):
            a = line[k - bpp] if k >= bpp else 0
            b = prev[k]
            c = prev[k - bpp] if k >= bpp else 0
            if f == 1:
                line[k] = (line[k] + a) & 0xFF
            elif f == 2:
                line[k] = (line[k] + b) & 0xFF
            elif f == 3:
                line[k] = (line[k] + (a + b) // 2) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[k] = (line[k] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append(line)
        prev = line
    return tuple(rows[y][x * bpp:x * bpp + 3])


def _pixel_check(name):
    def check(values, outdir):
        problems = []
        png = outdir / "final.png"
        for (x, y), want in PIXEL_CHECKS[name].items():
            got = _png_pixel(png, x, y)
            if got != want:
                problems.append("pixel (%d, %d) = %s, expected %s" % (x, y, got, want))
        return problems
    return check


def all_cases():
    PIXEL_CHECKS["m32x_render"] = {(40, 100): (0, 255, 0), (280, 100): (0, 0, 255)}
    found = [_boot(), _cmd_irq(), _vint_hint(), _hint_dma(), _sdram_shared(), _dreq_fifo(), _frame_buffer(), _vdp_68k(), _fen_refresh(), _cart_map(),
             _pwm(), _render(), _priority_32x(), _priority_md(), _dot_shift()]
    for name in PIXEL_CHECKS:
        DERIVED_CHECKS[name] = _pixel_check(name)
    return found


DERIVED_CHECKS = {}


def is_32x(name):
    return name in _SPECS
