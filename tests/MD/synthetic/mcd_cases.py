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


def _check_stopwatch_frame(values):
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


RUNNER_ARGS_MCD = ["--md-mcd"]

#Checks on several reads of a case (compare.py): name -> function(read values) -> problems
DERIVED_CHECKS = {"mcd_timer": _check_stopwatch_frame}


def all_cases():
    return [_boot_and_comm(), _int2(), _timer(), _word_ram_2m(), _word_ram_1m(), _prg_ram_protect_bank(), _busy()]
