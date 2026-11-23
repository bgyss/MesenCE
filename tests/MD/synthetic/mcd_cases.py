# Sega CD (Mode 1) cases: a cartridge drives the Sega CD unit without the BIOS
# (docs/research/sega-cd-feasibility.md S1, mcd-verification-findings.md).
#
# Each case holds the sub CPU in reset with its bus requested ($A12001 = $02), copies a small sub
# CPU program into program RAM through the main CPU's window at $420000 (vectors at $000000,
# code at $000200), releases the sub CPU ($A12001 = $01) and checks the results through the
# communication registers. Expected values come from the Mega-CD Hardware Manual (page numbers in
# each case). Run with mesen-headless --md-mcd. Copyright notice and license: GPL-3.0.
import struct

import cases
from cases import Case, Operation, READ, WRITE, POLL
from gen import M68kAsm, BEQ, BNE, BRA

# Main CPU addresses in Mode 1
PRG_WINDOW = 0x420000
WORD_RAM = 0x600000
CELL_IMAGE = 0x620000
REG = 0xA12000
# Sub CPU addresses
SUB_REG = 0xFF8000
SUB_WORD_RAM_2M = 0x080000
SUB_WORD_RAM_1M = 0x0C0000


class SubAsm(M68kAsm):
    """The sub CPU instructions the cases use, on top of gen.M68kAsm's branches and labels."""

    def move_w_imm(self, value, addr):        # move.w #value,(addr).l
        self.raw(0x33FC, value, addr >> 16, addr)

    def move_b_imm(self, value, addr):        # move.b #value,(addr).l
        self.raw(0x13FC, value & 0xFF, addr >> 16, addr)

    def move_w_mem(self, src, dst):           # move.w (src).l,(dst).l
        self.raw(0x33F9, src >> 16, src, dst >> 16, dst)

    def move_w_to_d0(self, addr):             # move.w (addr).l,d0
        self.raw(0x3039, addr >> 16, addr)

    def move_b_to_d0(self, addr):             # move.b (addr).l,d0
        self.raw(0x1039, addr >> 16, addr)

    def move_w_from_d0(self, addr):           # move.w d0,(addr).l
        self.raw(0x33C0, addr >> 16, addr)

    def wait_main_flag(self, value, label):   # loop until the main CPU's flag byte ($FF800E) = value
        self.label(label)
        self.move_b_to_d0(SUB_REG + 0x0E)
        self.raw(0x0C00, value & 0xFF)        # cmpi.b #value,d0
        self.branch(BNE, label)

    def set_sub_flag(self, value):
        self.move_b_imm(value, SUB_REG + 0x0F)

    def branch_w(self, cond, target):         # Bcc.w: 16-bit displacement from PC+2
        def enc(pc, labels):
            return struct.pack(">Hh", 0x6000 | (cond << 8), labels[target] - (pc + 2))
        self.items.append((None, enc, 4))

    def halt(self, label="halt"):
        self.label(label)
        self.branch(BRA, label)


def _sub_image(code_asm, vectors):
    """Program RAM words {address: word}: SSP, PC = $000200, extra vectors, and the code."""
    words = {}

    def long(addr, value):
        words[addr] = value >> 16
        words[addr + 2] = value & 0xFFFF
    long(0x000000, 0x0007FF00)
    long(0x000004, 0x00000200)
    for vector_addr, target in vectors.items():
        long(vector_addr, target)
    code = code_asm.assemble()
    for i in range(0, len(code), 2):
        words[code_asm.origin + i] = struct.unpack(">H", code[i:i + 2])[0]
    return words


def _load_and_start(words, extra_before_start=()):
    """Main CPU steps: hold the sub CPU, copy the program, release it."""
    ops = [Operation(WRITE, 1, REG + 0x01, 0x02)]  # SRES = 0 (reset), SBRQ = 1
    for addr in sorted(words):
        ops.append(Operation(WRITE, 2, PRG_WINDOW + addr, words[addr]))
    ops += list(extra_before_start)
    ops.append(Operation(WRITE, 1, REG + 0x01, 0x01))  # SRES = 1 (run), SBRQ = 0
    return ops


def _poll_sub_flag(value):
    return Operation(POLL, 1, REG + 0x0F, value, compare_mask=0xFF)


def _boot_and_comm():
    """The sub CPU boots from program RAM and talks to the main CPU through the communication
    registers (Hardware Manual pp. 12-16, 56-59).

    - Unit attached: the version register's bit 5 ("no expansion unit") reads 0.
    - Power-on: SRES and SBRQ read 0 (sub CPU in reset). After $A12001 = $02, SBRQ reads 1.
    - The program RAM window reads back what was written.
    - The sub CPU writes status 0 = $1234 and sets its flag to 1; then waits for the main flag = 1,
      adds 1 to command 0 and writes it to status 1, and sets its flag to 2.
    """
    a = SubAsm(0x200)
    a.move_w_imm(0x1234, SUB_REG + 0x20)
    a.set_sub_flag(0x01)
    a.wait_main_flag(0x01, "wait")
    a.move_w_to_d0(SUB_REG + 0x10)
    a.raw(0x5240)                             # addq.w #1,d0
    a.move_w_from_d0(SUB_REG + 0x22)
    a.set_sub_flag(0x02)
    a.halt()
    words = _sub_image(a, {})
    ops = [Operation(READ, 1, 0xA10001, 0x00, compare_mask=0x20),
           Operation(READ, 2, REG, 0x0000, compare_mask=0x0003)]
    ops += _load_and_start(words, [Operation(READ, 2, REG, 0x0002, compare_mask=0x0003),
                                   Operation(READ, 2, PRG_WINDOW + 0x200, words[0x200], compare_mask=0xFFFF)])
    ops += [_poll_sub_flag(0x01),
            Operation(READ, 2, REG + 0x20, 0x1234, compare_mask=0xFFFF),
            Operation(WRITE, 2, REG + 0x10, 0x0041),
            Operation(WRITE, 1, REG + 0x0E, 0x01),
            _poll_sub_flag(0x02),
            Operation(READ, 2, REG + 0x22, 0x0042, compare_mask=0xFFFF),
            Operation(READ, 2, REG + 0x0E, 0x0102, compare_mask=0xFFFF),
            # A word write sets the main CPU's flag from bits 15-8 (Hardware Manual p. 59)
            Operation(WRITE, 2, REG + 0x0E, 0x0300),
            Operation(READ, 2, REG + 0x0E, 0x0302, compare_mask=0xFFFF),
            Operation(READ, 2, REG, 0x0001, compare_mask=0x0003)]  # running, bus released
    return Case("mcd_boot_comm", tuple(ops), capture_frames=4)


def _int2():
    """Level 2 interrupt from the main CPU (Hardware Manual pp. 15, 30, 56).

    The sub CPU enables level 2 in $FF8032, lowers its mask and sets its flag. The main CPU reads
    IEN2 = 1, IFL2 = 0 in $A12000, writes IFL2 = 1, and waits for the handler (vector $68) to set
    the flag to 2; IFL2 then reads 0 again (taken).
    """
    a = SubAsm(0x200)
    a.move_w_imm(0x0004, SUB_REG + 0x32)     # enable level 2
    a.raw(0x46FC, 0x2000)                     # move.w #$2000,sr
    a.set_sub_flag(0x01)
    a.halt()
    a.label("level2")
    a.move_w_imm(0x2222, SUB_REG + 0x20)
    a.set_sub_flag(0x02)
    a.raw(0x4E73)                             # rte
    handler = 0x200 + _label_offset(a, "level2")
    words = _sub_image(a, {0x68: handler})
    ops = _load_and_start(words)
    ops += [_poll_sub_flag(0x01),
            Operation(READ, 2, REG, 0x8000, compare_mask=0x8100),
            Operation(WRITE, 1, REG, 0x01),  # IFL2
            _poll_sub_flag(0x02),
            Operation(READ, 2, REG + 0x20, 0x2222, compare_mask=0xFFFF),
            Operation(READ, 2, REG, 0x8000, compare_mask=0x8100)]
    return Case("mcd_int2", tuple(ops), capture_frames=4)


def _label_offset(asm, name):
    pc = 0
    for label, _, size in asm.items:
        if label == name:
            return pc
        pc += size
    raise KeyError(name)


def _timer():
    """Timer, level 3 and the stopwatch (Hardware Manual pp. 30, 59).

    The timer counts down from n every 30.72 us and raises level 3 every (n + 1) counts. With
    n = 9, the first level 3 handler clears the stopwatch and the second reads it: 10 counts.
    Then the main CPU reads the stopwatch at two successive VBlank starts: one NTSC frame is
    896,040 master clocks = 208,601.9 sub cycles = 543.2 counts of 384 sub cycles, so the
    difference is 543 or 544 (DERIVED_CHECKS; this checks the sub clock against the main one).
    """
    a = SubAsm(0x200)
    a.raw(0x7E00)                             # moveq #0,d7
    a.move_w_imm(0x0008, SUB_REG + 0x32)     # enable level 3
    a.raw(0x46FC, 0x2000)                     # move.w #$2000,sr
    a.move_w_imm(9, SUB_REG + 0x30)           # timer n = 9
    a.halt()
    a.label("level3")
    a.raw(0x4A47)                             # tst.w d7
    a.branch(BNE, "second")
    a.move_w_imm(0, SUB_REG + 0x0C)           # clear the stopwatch
    a.raw(0x7E01)                             # moveq #1,d7
    a.raw(0x4E73)                             # rte
    a.label("second")
    a.move_w_mem(SUB_REG + 0x0C, SUB_REG + 0x20)
    a.move_w_imm(0, SUB_REG + 0x30)           # stop the timer
    a.set_sub_flag(0x01)
    a.raw(0x4E73)                             # rte
    a.assemble()
    words = _sub_image(a, {0x6C: 0x200 + _label_offset(a, "level3")})
    ops = [cases._vdp_reg(1, 0x44)] + _load_and_start(words)
    ops += [_poll_sub_flag(0x01),
            Operation(READ, 2, REG + 0x20, 0x000A, compare_mask=0xFFFF),
            Operation(POLL, 2, 0xC00004, 0x0000, compare_mask=0x0008),
            Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
            Operation(READ, 2, REG + 0x0C, 0, compare_mask=0),
            Operation(POLL, 2, 0xC00004, 0x0000, compare_mask=0x0008),
            Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
            Operation(READ, 2, REG + 0x0C, 0, compare_mask=0)]
    return Case("mcd_timer", tuple(ops), capture_frames=6)


def _check_stopwatch_frame(values, outdir):
    first, second = values[-2], values[-1]
    diff = (second - first) & 0xFFF
    return [] if diff in (543, 544) else ["stopwatch advanced %d counts over one frame, expected 543 or 544" % diff]


def _word_ram_2m():
    """2M mode word RAM handover (Hardware Manual pp. 23-25, 57).

    Power-on: the main CPU owns word RAM (RET = 1, DMNA = 0). It writes $A5A5, sets DMNA = 1
    (word RAM to the sub CPU; RET = 0, DMNA = 1), and raises its flag. The sub CPU reads $A5A5 at
    $080000, writes $5A5A at $080002, sets RET = 1 and its flag. The main CPU then reads RET = 1,
    DMNA = 0 and the sub CPU's word.
    """
    a = SubAsm(0x200)
    a.wait_main_flag(0x01, "wait")
    a.move_w_mem(SUB_WORD_RAM_2M, SUB_REG + 0x20)
    a.move_w_imm(0x5A5A, SUB_WORD_RAM_2M + 2)
    a.move_b_imm(0x01, SUB_REG + 0x03)       # RET = 1
    a.set_sub_flag(0x01)
    a.halt()
    words = _sub_image(a, {})
    ops = _load_and_start(words)
    ops += [Operation(READ, 2, REG + 0x02, 0x0001, compare_mask=0x0007),
            Operation(WRITE, 2, WORD_RAM, 0xA5A5),
            Operation(WRITE, 1, REG + 0x03, 0x02),  # DMNA = 1
            Operation(READ, 2, REG + 0x02, 0x0002, compare_mask=0x0007),
            Operation(WRITE, 1, REG + 0x0E, 0x01),
            _poll_sub_flag(0x01),
            Operation(READ, 2, REG + 0x20, 0xA5A5, compare_mask=0xFFFF),
            Operation(READ, 2, REG + 0x02, 0x0001, compare_mask=0x0007),
            Operation(READ, 2, WORD_RAM + 2, 0x5A5A, compare_mask=0xFFFF)]
    return Case("mcd_wordram_2m", tuple(ops), capture_frames=4)


def _word_ram_1m():
    """1M mode banks, the RET swap, and the cell image (Hardware Manual pp. 16, 23-24).

    The sub CPU selects 1M mode with RET = 0 (main CPU: bank 0, sub CPU: bank 1) and raises its
    flag. The main CPU writes $1111 to its bank; the sub CPU writes $2222 to its own, sets RET = 1
    (swap) and reads the main CPU's word. The main CPU then reads MODE = 1, RET = 1 and the sub
    CPU's word. The cell image (32 cells) maps dot offsets $100 and $004 to cell offsets $004 and
    $400 (the manual's dot/cell table: dots 512-513 and 8-9).
    """
    a = SubAsm(0x200)
    a.move_b_imm(0x04, SUB_REG + 0x03)       # MODE = 1, RET = 0
    a.set_sub_flag(0x01)
    a.wait_main_flag(0x01, "wait")
    a.move_w_imm(0x2222, SUB_WORD_RAM_1M)
    a.move_b_imm(0x05, SUB_REG + 0x03)       # RET = 1: swap
    a.move_w_mem(SUB_WORD_RAM_1M, SUB_REG + 0x20)
    a.set_sub_flag(0x02)
    a.halt()
    words = _sub_image(a, {})
    ops = _load_and_start(words)
    ops += [_poll_sub_flag(0x01),
            Operation(READ, 2, REG + 0x02, 0x0004, compare_mask=0x0005),
            Operation(WRITE, 2, WORD_RAM, 0x1111),
            Operation(WRITE, 1, REG + 0x0E, 0x01),
            _poll_sub_flag(0x02),
            Operation(READ, 2, REG + 0x02, 0x0005, compare_mask=0x0005),
            Operation(READ, 2, WORD_RAM, 0x2222, compare_mask=0xFFFF),
            Operation(READ, 2, REG + 0x20, 0x1111, compare_mask=0xFFFF),
            Operation(WRITE, 2, WORD_RAM + 0x100, 0xBEEF),
            Operation(WRITE, 2, WORD_RAM + 0x004, 0xCAFE),
            Operation(READ, 2, CELL_IMAGE + 0x004, 0xBEEF, compare_mask=0xFFFF),
            Operation(READ, 2, CELL_IMAGE + 0x400, 0xCAFE, compare_mask=0xFFFF)]
    return Case("mcd_wordram_1m", tuple(ops), capture_frames=4)


def _prg_ram_protect_bank():
    """Program RAM write protection and the main CPU's bank window (Hardware Manual pp. 14, 57).

    With WP = 2 ($A12002 high byte), sub CPU writes below $000400 are ignored: the write of $1111
    to $000300 leaves $0300 (loaded by the main CPU), the write of $2222 to $000400 lands. With the
    window on bank 1 ($A12003 bits 7-6 = 01), the main CPU's write to $420000 is program RAM
    $020000, which the sub CPU reads as $7777.
    """
    a = SubAsm(0x1000)
    a.move_w_imm(0x1111, 0x000300)
    a.move_w_imm(0x2222, 0x000400)
    a.move_w_mem(0x000300, SUB_REG + 0x20)
    a.move_w_mem(0x000400, SUB_REG + 0x22)
    a.move_w_mem(0x020000, SUB_REG + 0x24)
    a.set_sub_flag(0x01)
    a.halt()
    words = _sub_image(a, {})
    words[0x000004], words[0x000006] = 0x0000, 0x1000  # PC = $001000
    words[0x000300] = 0x0300
    words[0x000400] = 0x0400
    bank1 = [Operation(WRITE, 1, REG + 0x03, 0x40),   # window on bank 1
             Operation(WRITE, 2, PRG_WINDOW, 0x7777),
             Operation(WRITE, 1, REG + 0x03, 0x00),
             Operation(WRITE, 1, REG + 0x02, 0x02)]   # WP = 2: $000000-$0003FF protected
    ops = _load_and_start(words, bank1)
    ops += [_poll_sub_flag(0x01),
            Operation(READ, 2, REG + 0x02, 0x0200, compare_mask=0xFF00),
            Operation(READ, 2, REG + 0x20, 0x0300, compare_mask=0xFFFF),
            Operation(READ, 2, REG + 0x22, 0x2222, compare_mask=0xFFFF),
            Operation(READ, 2, REG + 0x24, 0x7777, compare_mask=0xFFFF)]
    return Case("mcd_prgram_protect_bank", tuple(ops), capture_frames=4)


def _busy():
    """The sub CPU stays busy across frames, for save-state replay (replay_check.py saves after
    frame 1 or 2): the timer (n = 200) raises level 3, whose handler counts in status 0, while the
    main loop counts in d0 and copies it to status 1 and to its 1M word RAM bank. The sub CPU masks
    level 2 (SR = $2200) and the main CPU raises IFL2 once, so level 2 stays pending. At each VBlank
    start the main CPU reads both counters, the stopwatch and $A12000 (IFL2). No values are asserted
    (they depend on timing); replay must reproduce them and the final state exactly.
    """
    a = SubAsm(0x200)
    a.move_b_imm(0x04, SUB_REG + 0x03)       # 1M mode
    a.move_w_imm(0x000C, SUB_REG + 0x32)     # enable levels 2 and 3
    a.raw(0x46FC, 0x2200)                     # move.w #$2200,sr: level 2 masked, level 3 taken
    a.move_w_imm(200, SUB_REG + 0x30)
    a.set_sub_flag(0x01)
    a.raw(0x7000)                             # moveq #0,d0
    a.label("loop")
    a.raw(0x5240)                             # addq.w #1,d0
    a.move_w_from_d0(SUB_REG + 0x22)
    a.move_w_from_d0(SUB_WORD_RAM_1M)
    a.branch(BRA, "loop")
    a.label("level3")
    a.raw(0x5279, SUB_REG >> 16, (SUB_REG + 0x20) & 0xFFFF)  # addq.w #1,(status 0).l
    a.raw(0x4E73)                             # rte
    a.assemble()
    words = _sub_image(a, {0x6C: 0x200 + _label_offset(a, "level3")})
    ops = [cases._vdp_reg(1, 0x44)] + _load_and_start(words) + [_poll_sub_flag(0x01), Operation(WRITE, 1, REG, 0x01)]
    for _ in range(5):
        ops += [Operation(POLL, 2, 0xC00004, 0x0000, compare_mask=0x0008),
                Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
                Operation(READ, 2, REG + 0x20, 0, compare_mask=0),
                Operation(READ, 2, REG + 0x22, 0, compare_mask=0),
                Operation(READ, 2, REG + 0x0C, 0, compare_mask=0),
                Operation(READ, 2, REG, 0, compare_mask=0)]
    return Case("mcd_busy", tuple(ops), capture_frames=7)


import pathlib
import mcd_models

MCD_DIR = pathlib.Path(__file__).resolve().parents[1] / "mcd"

#Extra keys for the generated run spec (replay_check.py's save point), filled by _cd_replay_case
SPEC_EXTRA = {}


def _status_reads(expected, mask=0xFFFF, first=0):
    return [Operation(READ, 2, REG + 0x20 + 2 * (first + i), v, compare_mask=mask) for i, v in enumerate(expected)]


def _font():
    """Font registers (Hardware Manual p. 34): $FF8050-$FF8056 expand the 16 font bits into 4-bit
    colours, SC1 for 1 bits and SC0 for 0 bits, bit 15 in the top nibble of $FF8050."""
    a = SubAsm(0x200)
    configs = ((0xF1, 0xA5C3), (0x2D, 0x0F0F))
    for n, (color, bits) in enumerate(configs):
        a.move_w_imm(color, SUB_REG + 0x4C)
        a.move_w_imm(bits, SUB_REG + 0x4E)
        for k in range(4):
            a.move_w_mem(SUB_REG + 0x50 + 2 * k, SUB_REG + 0x20 + 2 * (4 * n + k))
    a.set_sub_flag(0x01)
    a.halt()
    expected = [w for color, bits in configs for w in mcd_models.font_words(color, bits)]
    ops = _load_and_start(_sub_image(a, {})) + [_poll_sub_flag(0x01)] + _status_reads(expected)
    return Case("mcd_font", tuple(ops), capture_frames=3)


def _dot_image():
    """1M mode dot image and write priority, with the manual's own example (p. 54): $0C0000 holds
    $0A and the word is written to $080000. Normal: $0507 -> $57, $0B00 -> $B0; overwrite: $57, $BA;
    underwrite: $5A, $BA. A read of the dot image gives one dot per byte, upper four bits 0 (p. 14)."""
    a = SubAsm(0x200)
    results = []
    for pm, expect in ((0, (0x57, 0xB0)), (2, (0x57, 0xBA)), (1, (0x5A, 0xBA))):
        for data, want in zip((0x0507, 0x0B00), expect):
            a.move_b_imm(0x04, SUB_REG + 0x03)               # 1M, RET = 0, PM = 0
            a.move_b_imm(0x0A, SUB_WORD_RAM_1M)
            a.move_b_imm(0x04 | (pm << 3), SUB_REG + 0x03)  # PM
            a.move_w_imm(data, SUB_WORD_RAM_2M)
            a.raw(0x13F9, SUB_WORD_RAM_1M >> 16, SUB_WORD_RAM_1M & 0xFFFF, SUB_REG >> 16, (SUB_REG + 0x21 + 2 * len(results)) & 0xFFFF)  # move.b
            results.append(want)
    a.move_b_imm(0x04, SUB_REG + 0x03)
    a.move_b_imm(0x3C, SUB_WORD_RAM_1M + 2)
    a.move_w_mem(SUB_WORD_RAM_2M + 4, SUB_REG + 0x20 + 2 * len(results))
    a.set_sub_flag(0x01)
    a.halt()
    ops = _load_and_start(_sub_image(a, {})) + [_poll_sub_flag(0x01)] + _status_reads(results, 0x00FF)
    ops.append(Operation(READ, 2, REG + 0x20 + 2 * len(results), 0x030C, compare_mask=0xFFFF))
    return Case("mcd_dot_image", tuple(ops), capture_frames=3)


BACKUP_SEED = MCD_DIR / "backup_seed.brm"  # 8 KiB, byte i = (7 * i + 3) & $FF (see _backup_ram_load)


def _backup_ram():
    """Backup RAM: 8 KiB on the lower byte of $FE0000-$FE3FFF (Hardware Manual p. 15), saved as the
    .brm battery file (DERIVED_CHECKS: run with --save-battery)."""
    a = SubAsm(0x200)
    a.move_b_imm(0x5A, 0xFE0001)
    a.move_b_imm(0xC3, 0xFE3FFF)
    a.move_w_imm(0x1234, 0xFE0004)
    a.move_w_mem(0xFE0000, SUB_REG + 0x20)
    a.move_w_mem(0xFE3FFE, SUB_REG + 0x22)
    a.move_w_mem(0xFE0004, SUB_REG + 0x24)
    a.set_sub_flag(0x01)
    a.halt()
    ops = _load_and_start(_sub_image(a, {})) + [_poll_sub_flag(0x01)] + _status_reads((0x5A, 0xC3, 0x34), 0x00FF)
    return Case("mcd_backup_ram", tuple(ops), capture_frames=3)


def _check_backup_file(values, outdir):
    path = pathlib.Path(outdir) / "saves" / "mcd_backup_ram.brm"
    if not path.exists():
        return ["no saves/mcd_backup_ram.brm"]
    data = path.read_bytes()
    want = {0: 0x5A, 0x1FFF: 0xC3, 2: 0x34}
    if len(data) != 0x2000:
        return ["backup RAM file is %d bytes, expected 8192" % len(data)]
    return ["backup RAM file byte $%X = $%02X, expected $%02X" % (i, data[i], v) for i, v in want.items() if data[i] != v]


def _backup_ram_load():
    """The backup RAM starts from the .brm file (runner --battery tests/MD/mcd/backup_seed.brm)."""
    a = SubAsm(0x200)
    for k, i in enumerate((0x0000, 0x0010, 0x1FFF)):
        a.move_w_mem(0xFE0000 + 2 * i, SUB_REG + 0x20 + 2 * k)
    a.set_sub_flag(0x01)
    a.halt()
    expected = [(7 * i + 3) & 0xFF for i in (0x0000, 0x0010, 0x1FFF)]
    ops = _load_and_start(_sub_image(a, {})) + [_poll_sub_flag(0x01)] + _status_reads(expected, 0x00FF)
    return Case("mcd_backup_ram_load", tuple(ops), capture_frames=3)


PCM_RAM = {0x000: (0xFE, 0x40, 0x90, 0x7F, 0x00, 0xA0, 0xFF), 0x100: (0xC0, 0x30, 0xE0, 0xFF)}
PCM_CHANNELS = (dict(env=0xFF, pan=0xFF, step=0x0800, loop=0x0002, start=0x00),   # loops back to the third sample
                dict(env=0x80, pan=0x0F, step=0x0400, loop=0x0101, start=0x01))


def _pcm():
    """PCM (manual appendix pp. 6-12): the channel address registers read back ST while the chip is
    off; then two channels play from wave RAM (channel 1 at full step, both sides; channel 2 at half
    step, left only, half envelope), and the pcm.raw capture must equal mcd_models.pcm_stream
    sample for sample from the first sounding sample (DERIVED_CHECKS, runner --chip-audio)."""
    pcm = 0xFF0000
    reg = lambda n: pcm + 2 * n + 1  # noqa: E731
    a = SubAsm(0x200)
    a.move_b_imm(0x40, reg(7))                # MOD = 1: channel 1, chip off
    a.move_b_imm(0x12, reg(6))                # ST = $12 (channel off: address = $1200)
    a.move_w_mem(pcm + 0x20, SUB_REG + 0x20)  # address low ($10), high ($11) of channel 1 (lower byte)
    a.move_w_mem(pcm + 0x22, SUB_REG + 0x22)
    a.move_b_imm(0x00, reg(7))                # MOD = 0: wave RAM bank 0
    for base, data in PCM_RAM.items():
        for i, v in enumerate(data):
            a.move_b_imm(v, pcm + 0x2000 + 2 * (base + i) + 1)
    for n, ch in enumerate(PCM_CHANNELS):
        a.move_b_imm(0x40 | n, reg(7))
        for r, v in ((0, ch["env"]), (1, ch["pan"]), (2, ch["step"] & 0xFF), (3, ch["step"] >> 8),
                     (4, ch["loop"] & 0xFF), (5, ch["loop"] >> 8), (6, ch["start"])):
            a.move_b_imm(v, reg(r))
    a.move_b_imm(0xFC, reg(8))                # channels 1 and 2 on
    a.move_b_imm(0x80, reg(7))                # chip on
    a.set_sub_flag(0x01)
    a.halt()
    ops = _load_and_start(_sub_image(a, {})) + [_poll_sub_flag(0x01)]
    ops += [Operation(READ, 2, REG + 0x20, 0x0000, compare_mask=0x00FF),
            Operation(READ, 2, REG + 0x22, 0x0012, compare_mask=0x00FF)]
    return Case("mcd_pcm", tuple(ops), capture_frames=4)


def _check_pcm(values, outdir):
    path = pathlib.Path(outdir) / "pcm.raw"
    if not path.exists():
        return ["no pcm.raw (run with --chip-audio)"]
    raw = path.read_bytes()
    samples = [struct.unpack_from("<hh", raw, i) for i in range(0, len(raw) - 3, 4)]
    first = next((i for i, s in enumerate(samples) if s != (0, 0)), None)
    if first is None:
        return ["the PCM never produced a sample"]
    ram = bytearray(0x10000)
    for base, data in PCM_RAM.items():
        ram[base:base + len(data)] = bytes(data)
    want = mcd_models.pcm_stream(ram, PCM_CHANNELS, 400)
    got = samples[first:first + 400]
    if len(got) < 400:
        return ["only %d PCM samples after the first sounding one" % len(got)]
    bad = next((i for i in range(400) if tuple(got[i]) != tuple(want[i])), None)
    return [] if bad is None else ["PCM sample %d is %s, model %s" % (bad, got[bad], want[bad])]


GFX = dict(stamp=0x80, map_base=0x10000, vectors=0x12000, image=0x18000, vcells=3, hdots=48, vdots=32)
GFX_MAP = {(0, 0): 0x0001, (1, 0): 0x8001, (0, 1): 0x2001, (1, 1): 0x4001, (2, 0): 0x6001, (2, 1): 0xA001}


def _gfx_vectors():
    vectors = []
    for line in range(GFX["vdots"]):
        if line < 16:
            vec = (0, line << 3, 0x0800, 0)
        elif line == 28:
            vec = (47 << 3, 20 << 3, 0xF800, 0)       # right to left
        elif line == 29:
            vec = (5 << 3, 0, 0, 0x0800)              # down a column
        elif line == 31:
            vec = (250 << 3, 31 << 3, 0x0800, 0)      # runs off the 256-dot map: 0 without repeat
        else:
            vec = (0, line << 3, 0x0400, 0)           # half steps: 2x horizontal scaling
        vectors.append(vec)
    return vectors


def _gfx_wordram():
    wram = bytearray(0x40000)
    wram[GFX["stamp"]:GFX["stamp"] + 128] = mcd_models.stamp_bytes(lambda x, y: ((x * 5 + y * 3) % 15) + 1)
    for (mx, my), entry in GFX_MAP.items():
        offset = GFX["map_base"] + 2 * (my * 16 + mx)
        wram[offset:offset + 2] = struct.pack(">H", entry)
    for i, vec in enumerate(_gfx_vectors()):
        wram[GFX["vectors"] + 8 * i:GFX["vectors"] + 8 * i + 8] = struct.pack(">HHHH", *vec)
    return wram


def _graphics():
    """Graphics chip (Hardware Manual pp. 35-53): a 1x1-screen map of 16x16 stamps with every
    flip/rotation, sampled by 32 trace vectors (identity, 2x horizontal scaling, right to left, down a
    column, off the map). The main CPU builds the data in 2M word RAM, hands it to the sub CPU
    (DMNA), the sub CPU starts the operation and takes level 1 at the end, then returns word RAM
    (RET). Every word of the 48x32 image buffer must equal mcd_models.GraphicsModel. Also: GRON
    reads 1 and Vdot 32 right after the start, Vdot reads 0 at the end, and the operation takes
    9-10 stopwatch counts (32 lines of 113 clocks by the manual's time formula, judgment call)."""
    wram = _gfx_wordram()
    model = mcd_models.GraphicsModel(wram)
    result = model.run(GFX["map_base"], GFX["image"], GFX["vcells"], GFX["hdots"], GFX["vdots"], GFX["vectors"])
    image_bytes = (GFX["hdots"] // 8) * (GFX["vcells"] + 1) * 32
    a = SubAsm(0x200)
    a.move_w_imm(0x0002, SUB_REG + 0x32)                 # enable level 1
    a.raw(0x46FC, 0x2000)
    a.move_w_imm(0x0000, SUB_REG + 0x58)                 # 16x16 stamps, 1x1 map, no repeat
    a.move_w_imm(GFX["map_base"] >> 2, SUB_REG + 0x5A)
    a.move_w_imm(GFX["vcells"], SUB_REG + 0x5C)
    a.move_w_imm(GFX["image"] >> 2, SUB_REG + 0x5E)
    a.move_w_imm(0, SUB_REG + 0x60)
    a.move_w_imm(GFX["hdots"], SUB_REG + 0x62)
    a.move_w_imm(GFX["vdots"], SUB_REG + 0x64)
    a.move_w_imm(0, SUB_REG + 0x0C)                      # clear the stopwatch
    a.move_w_imm(GFX["vectors"] >> 2, SUB_REG + 0x66)    # start
    a.move_w_mem(SUB_REG + 0x58, SUB_REG + 0x20)
    a.move_w_mem(SUB_REG + 0x64, SUB_REG + 0x22)
    a.halt()
    a.label("level1")
    a.move_w_mem(SUB_REG + 0x0C, SUB_REG + 0x24)
    a.move_w_mem(SUB_REG + 0x64, SUB_REG + 0x26)
    a.move_b_imm(0x01, SUB_REG + 0x03)                  # RET = 1: word RAM back to the main CPU
    a.set_sub_flag(0x01)
    a.raw(0x4E73)
    a.assemble()
    words = _sub_image(a, {0x64: 0x200 + _label_offset(a, "level1")})
    ops = []
    for start, length in ((GFX["stamp"], 128), (GFX["map_base"], 2 * 32), (GFX["vectors"], 8 * GFX["vdots"])):
        for i in range(0, length, 2):
            word = (wram[start + i] << 8) | wram[start + i + 1]
            if word:
                ops.append(Operation(WRITE, 2, WORD_RAM + start + i, word))
    ops.append(Operation(WRITE, 1, REG + 0x03, 0x02))   # DMNA = 1
    ops += _load_and_start(words)
    ops += [_poll_sub_flag(0x01),
            Operation(READ, 2, REG + 0x20, 0x8000, compare_mask=0xFFFF),
            Operation(READ, 2, REG + 0x22, GFX["vdots"], compare_mask=0xFFFF),
            Operation(READ, 2, REG + 0x24, 0, compare_mask=0),
            Operation(READ, 2, REG + 0x26, 0, compare_mask=0xFFFF),
            Operation(READ, 2, REG + 0x02, 0x0001, compare_mask=0x0007)]
    for i in range(0, image_bytes, 2):
        word = (result[GFX["image"] + i] << 8) | result[GFX["image"] + i + 1]
        ops.append(Operation(READ, 2, WORD_RAM + GFX["image"] + i, word, compare_mask=0xFFFF))
    return Case("mcd_graphics", tuple(ops), capture_frames=4)


def _graphics_busy():
    """The graphics chip restarts from its own level 1 handler, so an operation is in flight at
    every save point (for replay_check.py): the handler counts operations in status 0 and starts
    the next one. Underwrite priority keeps any wrong dot written into an empty area. The main CPU reads the count and the stopwatch at each VBlank start. PCM channel 1
    plays a looping sample meanwhile. No values asserted; replay must reproduce them exactly."""
    wram = _gfx_wordram()
    a = SubAsm(0x200)
    a.move_w_imm(0x0002, SUB_REG + 0x32)
    a.raw(0x46FC, 0x2000)
    a.move_w_imm(0x0001, SUB_REG + 0x58)                 # repeat on
    a.move_w_imm(GFX["map_base"] >> 2, SUB_REG + 0x5A)
    a.move_w_imm(GFX["vcells"], SUB_REG + 0x5C)
    a.move_w_imm(GFX["image"] >> 2, SUB_REG + 0x5E)
    a.move_w_imm(3, SUB_REG + 0x60)                      # dot offset 3
    a.move_w_imm(GFX["hdots"], SUB_REG + 0x62)
    a.move_b_imm(0x08, SUB_REG + 0x03)                  # PM = underwrite: a wrong dot in an empty area stays
    pcm = 0xFF0000
    for reg, value in ((7, 0x00), (0x1000, 0x90), (0x1001, 0x70), (0x1002, 0xFF), (7, 0x40), (0, 0xC0), (1, 0x5A),
                       (2, 0x00), (3, 0x03), (4, 0x00), (5, 0x00), (6, 0x00), (8, 0xFE), (7, 0x80)):
        a.move_b_imm(value, pcm + 2 * reg + 1)
    a.set_sub_flag(0x01)
    a.label("restart")
    a.move_w_imm(GFX["vdots"], SUB_REG + 0x64)
    a.move_w_imm(GFX["vectors"] >> 2, SUB_REG + 0x66)
    a.halt()
    a.label("level1")
    a.raw(0x5279, SUB_REG >> 16, (SUB_REG + 0x20) & 0xFFFF)  # addq.w #1,(status 0).l
    a.move_w_imm(GFX["vdots"], SUB_REG + 0x64)
    a.move_w_imm(GFX["vectors"] >> 2, SUB_REG + 0x66)
    a.raw(0x4E73)
    a.assemble()
    words = _sub_image(a, {0x64: 0x200 + _label_offset(a, "level1")})
    ops = [cases._vdp_reg(1, 0x44)]
    for start, length in ((GFX["stamp"], 128), (GFX["map_base"], 2 * 32), (GFX["vectors"], 8 * GFX["vdots"])):
        for i in range(0, length, 2):
            word = (wram[start + i] << 8) | wram[start + i + 1]
            if word:
                ops.append(Operation(WRITE, 2, WORD_RAM + start + i, word))
    ops.append(Operation(WRITE, 1, REG + 0x03, 0x02))
    ops += _load_and_start(words) + [_poll_sub_flag(0x01)]
    for _ in range(5):
        ops += [Operation(POLL, 2, 0xC00004, 0x0000, compare_mask=0x0008),
                Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
                Operation(READ, 2, REG + 0x20, 0, compare_mask=0),
                Operation(READ, 2, REG + 0x0C, 0, compare_mask=0)]
    return Case("mcd_graphics_busy", tuple(ops), capture_frames=7)


def _check_graphics_time(values, outdir):
    counts = values[2]
    return [] if counts in (9, 10) else ["graphics operation took %d stopwatch counts, expected 9-10" % counts]


def _dma_word_ram():
    """68000-to-VRAM DMA from word RAM (Hardware Manual p. 61): the data comes one read late, so a
    transfer started one word after the data delivers the data from the second destination word on
    (the first word is not defined). Words $1111-$5555 at $600000; DMA of 4 words from $600002 to
    VRAM 0: VRAM words 1-3 are $2222, $3333, $4444."""
    data = (0x1111, 0x2222, 0x3333, 0x4444, 0x5555)
    source = (WORD_RAM + 2) >> 1
    ops = [Operation(WRITE, 2, WORD_RAM + 2 * i, w) for i, w in enumerate(data)]
    ops += [cases._vdp_reg(1, 0x14), cases._vdp_reg(15, 2), cases._vdp_reg(19, 4), cases._vdp_reg(20, 0),
            cases._vdp_reg(21, source & 0xFF), cases._vdp_reg(22, (source >> 8) & 0xFF), cases._vdp_reg(23, (source >> 16) & 0x7F),
            Operation(WRITE, 4, 0xC00004, cases._vdp_command(1 | 0x20, 0)),
            Operation(WRITE, 4, 0xC00004, cases._vdp_command(0, 0)),
            Operation(READ, 2, 0xC00000, 0, compare_mask=0),
            Operation(READ, 2, 0xC00000, 0x2222, compare_mask=0xFFFF),
            Operation(READ, 2, 0xC00000, 0x3333, compare_mask=0xFFFF),
            Operation(READ, 2, 0xC00000, 0x4444, compare_mask=0xFFFF)]
    return Case("mcd_dma_wordram", tuple(ops), capture_frames=3)


# ---------------------------------------------------------------------------------------------
# S3: a synthetic disc in the drive, driven without the BIOS

DISC_DATA_SECTORS = 200      # track 1: MODE1/2352, LBA 0-199
DISC_AUDIO_SECTORS = 100     # track 2: AUDIO, LBA 200-299 (cue INDEX 01 00:02:50)


def disc_user_data(lba):
    data = bytearray(((lba * 7 + j * 3 + (j >> 8)) & 0xFF) for j in range(2048))
    if lba == 0:
        data[0:16] = b"SEGADISCSYSTEM  "
        data[0x100:0x110] = b"SEGA MEGA DRIVE "
        data[0x1F0:0x1F3] = b"JUE"
    return data


def disc_audio_sample(k):
    """Sample k of track 2: (left, right), 16-bit."""
    return (((k * 37) % 4096) - 2047) * 8, (((k * 53) % 4096) - 2047) * 8


def _bcd(v):
    return ((v // 10) << 4) | (v % 10)


def build_disc():
    raw = bytearray()
    sync = bytes([0x00] + [0xFF] * 10 + [0x00])
    for lba in range(DISC_DATA_SECTORS):
        msf = lba + 150
        raw += sync + bytes((_bcd(msf // 75 // 60), _bcd(msf // 75 % 60), _bcd(msf % 75), 0x01)) + disc_user_data(lba) + bytes(288)
    for k in range(DISC_AUDIO_SECTORS * 588):
        raw += struct.pack("<hh", *disc_audio_sample(k))
    cue = ('FILE "mcd_disc.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
           '  TRACK 02 AUDIO\n    INDEX 01 00:02:50\n')
    return bytes(raw), cue


def write_assets(out_dir):
    raw, cue = build_disc()
    out = pathlib.Path(out_dir)
    if not (out / "mcd_disc.bin").exists() or (out / "mcd_disc.bin").read_bytes() != raw:
        (out / "mcd_disc.bin").write_bytes(raw)
    (out / "mcd_disc.cue").write_text(cue)


def _cdd_words(command):
    """CDD command nibbles 0-8 plus checksum, as the 5 words written to $FF8042-$FF804A."""
    nibbles = list(command) + [(~sum(command)) & 0x0F]
    return [(nibbles[i] << 8) | nibbles[i + 1] for i in range(0, 10, 2)]


def _status_words(status):
    nibbles = list(status) + [(~sum(status)) & 0x0F]
    return [(nibbles[i] << 8) | nibbles[i + 1] for i in range(0, 10, 2)]


class CdAsm(SubAsm):
    COUNTER = 0x7000

    def send(self, command):
        for i, word in enumerate(_cdd_words(command)):
            self.move_w_imm(word, SUB_REG + 0x42 + 2 * i)  # the last word sends the command

    def snapshot(self):                       # move.w (counter).l,d1
        self.raw(0x3239, 0, self.COUNTER)

    def wait_int4(self, label):               # until the level 4 count moves
        self.label(label)
        self.raw(0xB279, 0, self.COUNTER)     # cmp.w (counter).l,d1
        self.branch(BEQ, label)

    def command(self, command, label):
        self.snapshot()
        self.send(command)
        self.wait_int4(label)

    def save_status(self, dest):
        for i in range(5):
            self.move_w_mem(SUB_REG + 0x38 + 2 * i, dest + 2 * i)

    def cdc_reg(self, reg):                   # move.b #reg,$FF8005
        self.move_b_imm(reg, SUB_REG + 0x05)

    def cdc_write(self, value):               # move.b #value,$FF8007
        self.move_b_imm(value, SUB_REG + 0x07)

    def read_cdc_to(self, dreg):              # move.b $FF8007,dN
        self.raw(0x1039 | (dreg << 9), SUB_REG >> 16, (SUB_REG + 0x07) & 0xFFFF)

    def dbf(self, dreg, target):              # dbf dN,target
        def enc(pc, labels):
            return struct.pack(">Hh", 0x51C8 | dreg, labels[target] - (pc + 2))
        self.items.append((None, enc, 4))

    def wait_sector(self, frame_bcd, label):
        """Poll IFSTAT until a sector is decoded; keep the header in d3-d6 and the block pointer in
        d2 (PT); clear DECI by reading STAT3; repeat until the header frame is `frame_bcd`."""
        self.label(label)
        self.cdc_reg(0x01)
        self.read_cdc_to(0)
        self.raw(0x0800, 5)                   # btst #5,d0 (DECI, active low)
        self.branch(BNE, label)
        self.cdc_reg(0x04)
        for d in (3, 4, 5, 6):
            self.read_cdc_to(d)               # HEAD0-3
        self.cdc_reg(0x08)
        self.read_cdc_to(7)                   # PTL -> d7
        self.read_cdc_to(2)                   # PTH -> d2
        self.cdc_reg(0x0F)
        self.read_cdc_to(0)                   # STAT3 (clears DECI)
        self.raw(0x0C05, frame_bcd)           # cmpi.b #frame,d5
        self.branch(BNE, label)
        self.raw(0xE14A)                      # lsl.w #8,d2
        self.raw(0x1407)                      # move.b d7,d2: d2 = PT
        self.raw(0x5842)                      # addq.w #4,d2: data after the 4-byte header

    def start_transfer(self, destination):
        """DBC = 2047, DAC = d2, destination, DTTRG."""
        self.move_b_imm(destination, SUB_REG + 0x04)
        self.cdc_reg(0x02)
        self.cdc_write(0xFF)                  # DBCL
        self.cdc_write(0x07)                  # DBCH
        self.raw(0x13C2, SUB_REG >> 16, (SUB_REG + 0x07) & 0xFFFF)  # move.b d2,$FF8007: DACL
        self.raw(0xE04A)                      # lsr.w #8,d2
        self.raw(0x13C2, SUB_REG >> 16, (SUB_REG + 0x07) & 0xFFFF)  # DACH
        self.cdc_write(0x00)                  # DTTRG


RESULTS = 0x8000  # results in program RAM, read by the main CPU through $420000 with the sub CPU's bus


def _cd_data():
    """The drive and the CD controller without the BIOS (Mode 1 with a disc: runner --md-mcd-disc).

    Sources: CDD protocol and reports as in ares md/mcd/cdd.cpp and MAME megacdcd.cpp (both agree on
    the checksum, the first/last track and track start reports); CDC registers from the LC8951
    manual (IFCTRL, CTRL0, HEAD0-3, PT, DBC, DAC, DTTRG, STAT3); $FF8004 destinations from the
    Hardware Manual p. 26.

    1. HOCK on; report requests (first/last track) until the drive has read the TOC (status 4,
       paused): tracks 1-2. Track 2 starts at 00:04:50 and is audio.
    2. Seek and play from 00:03:25 (LBA 100) with the decoder and buffer writes on; wait for the
       header 00:03:25 (mode 1); read the 2048 data bytes through $FF8008 (sub CPU read).
    3. The next sector (00:03:26) goes by DMA into 2M word RAM at 0, then RET returns word RAM.
    4. Seek and play track 2 (00:04:50): CD audio, checked in cdda.raw (DERIVED_CHECKS).
    Also: during the TOC loop the level 4 interrupts are one 75 Hz step apart (DERIVED_CHECKS).
    The main CPU then holds the sub CPU's bus and reads the results from program RAM.
    """
    a = CdAsm(0x200)
    a.move_w_imm(0x0010, SUB_REG + 0x32)     # level 4
    a.raw(0x46FC, 0x2000)
    a.snapshot()
    a.move_b_imm(0x04, SUB_REG + 0x37)       # HOCK
    a.wait_int4("hock")
    a.label("toc")
    a.command((2, 0, 0, 4, 0, 0, 0, 0, 0), "toc_wait")
    a.move_b_to_d0(SUB_REG + 0x38)            # status 0 (upper byte)
    a.raw(0x0C00, 0x04)                       # paused: the TOC is read
    a.branch(BNE, "toc")
    a.save_status(RESULTS)
    a.move_w_mem(CdAsm.COUNTER + 2, RESULTS + 0x20)  # stopwatch at the last two level 4 interrupts
    a.move_w_mem(CdAsm.COUNTER + 4, RESULTS + 0x22)
    a.command((2, 0, 0, 5, 0, 2, 0, 0, 0), "start2")
    a.save_status(RESULTS + 0x0A)
    # CDC: reset, IFCTRL = DECIEN | DOUTEN, CTRL0 = DECEN | WRRQ, CTRL1 = 0
    a.cdc_reg(0x0F)
    a.cdc_write(0x00)
    a.cdc_reg(0x01)
    a.cdc_write(0x22)
    a.cdc_reg(0x0A)
    a.cdc_write(0x84)
    a.cdc_write(0x00)
    a.command((3, 0, 0, 0, 0, 3, 2, 5, 0), "seek1")
    a.wait_sector(0x25, "sector100")
    a.raw(0x13C3, 0, RESULTS + 0x14)          # move.b d3,(results+$14): HEAD0-3
    a.raw(0x13C4, 0, RESULTS + 0x15)
    a.raw(0x13C5, 0, RESULTS + 0x16)
    a.raw(0x13C6, 0, RESULTS + 0x17)
    a.start_transfer(3)                       # sub CPU read
    a.raw(0x43F9, 0, RESULTS + 0x100)         # lea (results+$100).l,a1
    a.raw(0x323C, 1023)                       # move.w #1023,d1
    a.label("read")
    a.raw(0x32F9, SUB_REG >> 16, (SUB_REG + 0x08) & 0xFFFF)  # move.w $FF8008,(a1)+
    a.dbf(1, "read")
    a.wait_sector(0x26, "sector101")
    a.move_w_imm(0x0000, SUB_REG + 0x0A)      # DMA address: word RAM 0
    a.start_transfer(7)                       # word RAM DMA
    a.label("dma")
    a.move_w_to_d0(SUB_REG + 0x04)
    a.raw(0x0800, 15)                         # btst #15,d0: EDT
    a.branch(BEQ, "dma")
    a.move_b_imm(0x01, SUB_REG + 0x03)        # RET = 1
    a.command((3, 0, 0, 0, 0, 4, 5, 0, 0), "seek2")  # play track 2
    a.set_sub_flag(0x01)
    a.halt()
    a.label("level4")
    a.move_w_mem(CdAsm.COUNTER + 2, CdAsm.COUNTER + 4)   # previous stopwatch
    a.move_w_mem(SUB_REG + 0x0C, CdAsm.COUNTER + 2)      # stopwatch at this interrupt
    a.raw(0x5279, 0, CdAsm.COUNTER)           # addq.w #1,(counter).l
    a.raw(0x4E73)
    a.assemble()
    words = _sub_image(a, {0x70: 0x200 + _label_offset(a, "level4")})

    toc = _status_words((4, 4, 0, 1, 0, 2, 0, 0, 0))
    start2 = _status_words((4, 5, 0, 0, 0, 4, 5, 0, 2))
    sector100 = disc_user_data(100)
    sector101 = disc_user_data(101)
    ops = [Operation(WRITE, 1, REG + 0x03, 0x02)]            # DMNA: word RAM to the sub CPU
    ops += _load_and_start(words) + [_poll_sub_flag(0x01)]
    ops.append(Operation(WRITE, 1, REG + 0x01, 0x03))        # hold the sub CPU's bus
    ops += [Operation(READ, 2, PRG_WINDOW + RESULTS + 2 * i, w, compare_mask=0xFFFF) for i, w in enumerate(toc + start2)]
    ops += [Operation(READ, 2, PRG_WINDOW + RESULTS + 0x14, 0x0003, compare_mask=0xFFFF),
            Operation(READ, 2, PRG_WINDOW + RESULTS + 0x16, 0x2501, compare_mask=0xFFFF)]
    for offset in list(range(0, 32, 2)) + [2046]:
        ops.append(Operation(READ, 2, PRG_WINDOW + RESULTS + 0x100 + offset, (sector100[offset] << 8) | sector100[offset + 1], compare_mask=0xFFFF))
    for offset in list(range(0, 32, 2)) + [2046]:
        ops.append(Operation(READ, 2, WORD_RAM + offset, (sector101[offset] << 8) | sector101[offset + 1], compare_mask=0xFFFF))
    ops += [Operation(READ, 2, PRG_WINDOW + RESULTS + 0x20, 0, compare_mask=0),
            Operation(READ, 2, PRG_WINDOW + RESULTS + 0x22, 0, compare_mask=0)]
    ops.append(Operation(WRITE, 1, REG + 0x01, 0x01))        # release it: CD audio keeps playing
    return Case("mcd_cd_data", tuple(ops), capture_frames=260)


def _cd_start(a):
    """HOCK on, CDC reset, IFCTRL = DECIEN | DOUTEN, CTRL0 = DECEN | WRRQ."""
    a.move_w_imm(0x0010, SUB_REG + 0x32)
    a.raw(0x46FC, 0x2000)
    a.snapshot()
    a.move_b_imm(0x04, SUB_REG + 0x37)
    a.wait_int4("hock")
    a.cdc_reg(0x0F)
    a.cdc_write(0x00)
    a.cdc_reg(0x01)
    a.cdc_write(0x22)
    a.cdc_reg(0x0A)
    a.cdc_write(0x84)
    a.cdc_write(0x00)


def _cd_replay_case(name, program, frames, warmup, status_read=False):
    a = CdAsm(0x200)
    _cd_start(a)
    program(a)
    a.label("level4")
    a.raw(0x5279, 0, CdAsm.COUNTER)
    a.raw(0x4E73)
    a.assemble()
    words = _sub_image(a, {0x70: 0x200 + _label_offset(a, "level4")})
    ops = [cases._vdp_reg(1, 0x44), Operation(WRITE, 1, REG + 0x03, 0x02)] + _load_and_start(words) + [_poll_sub_flag(0x01)]
    for _ in range(frames - 2):
        ops += [Operation(POLL, 2, 0xC00004, 0x0000, compare_mask=0x0008),
                Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
                Operation(READ, 2, REG + 0x0C, 0, compare_mask=0)]
        if status_read:
            ops.append(Operation(READ, 2, REG + 0x20, 0, compare_mask=0))
    SPEC_EXTRA[name] = {"replay_warmup": warmup}
    return Case(name, tuple(ops), capture_frames=frames)


def _cd_busy():
    """The CD controller busy across the save point (frame 20, for replay_check.py): the drive plays
    the data track from LBA 10; the sub CPU leaves each decoded sector's interrupt pending for about
    6 ms before taking it (read PT, STAT3), and DMAs the latest sector into 2M word RAM over and over,
    so a decoder interrupt and a DMA are usually in progress. The main CPU reads the stopwatch at
    each VBlank start. No values asserted; replay must reproduce them and the final state exactly."""
    def program(a):
        a.command((3, 0, 0, 0, 0, 2, 1, 0, 0), "seek")   # 00:02:10 = LBA 10
        a.set_sub_flag(0x01)
        a.label("loop")
        a.cdc_reg(0x01)
        a.read_cdc_to(0)
        a.raw(0x0800, 5)                      # btst #5,d0: a decoded sector?
        a.branch_w(BNE, "dma")
        a.raw(0x323C, 8000)                   # move.w #8000,d1
        a.label("delay")
        a.dbf(1, "delay")                     # about 6 ms with DECI pending
        a.cdc_reg(0x08)
        a.read_cdc_to(7)
        a.read_cdc_to(2)
        a.cdc_reg(0x0F)
        a.read_cdc_to(0)
        a.raw(0xE14A)
        a.raw(0x1407)
        a.raw(0x5842)
        a.raw(0x33C2, 0, CdAsm.COUNTER + 0x10)    # move.w d2,(source).l
        a.label("dma")
        a.raw(0x3439, 0, CdAsm.COUNTER + 0x10)    # move.w (source).l,d2
        a.move_w_imm(0x0000, SUB_REG + 0x0A)
        a.start_transfer(7)
        a.label("dmawait")
        a.move_w_to_d0(SUB_REG + 0x04)
        a.raw(0x0800, 15)                     # btst #15,d0: EDT
        a.branch(BEQ, "dmawait")
        a.branch_w(BRA, "loop")
    return _cd_replay_case("mcd_cd_busy", program, 26, 20)


def _cd_seek_play(name, msf_nibbles, frames, warmup):
    """Seek and play, then keep the drive reporting (a report request after every level 4). Each
    round also publishes the CDC's IFSTAT in status 0 (read by the main CPU) and toggles the fader
    between half volume and silence, so a fade is always in progress (512 steps of one sample)."""
    def program(a):
        a.command((3, 0) + msf_nibbles + (0,), "seek")
        a.set_sub_flag(0x01)
        a.label("loop")
        a.command((2, 0, 0, 0, 0, 0, 0, 0, 0), "report")
        a.cdc_reg(0x01)
        a.read_cdc_to(0)
        a.move_w_from_d0(SUB_REG + 0x20)
        a.raw(0x0A79, 0x2000, 0, CdAsm.COUNTER + 0x12)   # eori.w #$2000,(fader).l: half volume / silence
        a.move_w_mem(CdAsm.COUNTER + 0x12, SUB_REG + 0x34)
        a.branch_w(BRA, "loop")
    return _cd_replay_case(name, program, frames, warmup, status_read=True)


def _cd_seeking():
    """The save point (frame 5) falls inside a seek to track 2 (replay only)."""
    return _cd_seek_play("mcd_cd_seeking", (0, 0, 0, 4, 5, 0), 9, 5)


def _cd_audio():
    """The save point (frame 20) falls while track 2 plays: CD audio continuity in cdda.raw (replay
    only)."""
    return _cd_seek_play("mcd_cd_audio", (0, 0, 0, 4, 5, 0), 26, 20)


def _check_cdda(values, outdir):
    # A command sent right after a level 4 interrupt is answered at the next 75 Hz step: 13.3 ms
    # (BIOS Manual p. 5) = 434 stopwatch counts of 30.72 us (434 x 384 sub cycles)
    period = (values[-2] - values[-1]) & 0xFFF
    if period != 434:
        return ["level 4 interrupts %d stopwatch counts apart, expected 434" % period]
    path = pathlib.Path(outdir) / "cdda.raw"
    if not path.exists():
        return ["no cdda.raw (run with --chip-audio)"]
    raw = path.read_bytes()
    samples = [struct.unpack_from("<hh", raw, i) for i in range(0, len(raw) - 3, 4)]
    first = next((i for i, s in enumerate(samples) if s != (0, 0)), None)
    if first is None:
        return ["no CD audio"]
    count = 2000
    got = samples[first:first + count]
    if len(got) < count:
        return ["only %d CD audio samples" % len(got)]
    # Full fader volume, output at half scale (ares)
    want = [(l >> 1, r >> 1) for l, r in (disc_audio_sample(k) for k in range(count))]
    bad = next((i for i in range(count) if tuple(got[i]) != want[i]), None)
    return [] if bad is None else ["CD audio sample %d is %s, disc %s" % (bad, got[bad], want[bad])]


RUNNER_ARGS_MCD = ["--md-mcd"]

RUNNER_ARGS_EXTRA = {"mcd_backup_ram": ["--save-battery"], "mcd_backup_ram_load": ["--battery", str(BACKUP_SEED)],
                     "mcd_pcm": ["--chip-audio"], "mcd_graphics_busy": ["--chip-audio"],
                     "mcd_cd_data": ["--md-mcd-disc", "{roms}/mcd_disc.cue", "--chip-audio", "--trace-limit", "100000000"],
                     "mcd_cd_busy": ["--md-mcd-disc", "{roms}/mcd_disc.cue"],
                     "mcd_cd_seeking": ["--md-mcd-disc", "{roms}/mcd_disc.cue"],
                     "mcd_cd_audio": ["--md-mcd-disc", "{roms}/mcd_disc.cue", "--chip-audio"]}

#Checks on several reads of a case (compare.py): name -> function(read values) -> problems
DERIVED_CHECKS = {"mcd_timer": _check_stopwatch_frame, "mcd_backup_ram": _check_backup_file, "mcd_pcm": _check_pcm,
                  "mcd_graphics": _check_graphics_time, "mcd_cd_data": _check_cdda}


def all_cases():
    return [_boot_and_comm(), _int2(), _timer(), _word_ram_2m(), _word_ram_1m(), _prg_ram_protect_bank(), _busy(),
            _font(), _dot_image(), _backup_ram(), _backup_ram_load(), _pcm(), _graphics(), _dma_word_ram(), _graphics_busy(), _cd_data(), _cd_busy(), _cd_seeking(), _cd_audio()]
