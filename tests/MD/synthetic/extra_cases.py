# MesenCE's own synthetic Mega Drive cases (not part of nativekit's corpus; no BlastEm reference).
# They reuse cases.py's operation format and expected values and are built by gen.py next to the
# nativekit cases. Copyright notice and license: GPL-3.0, as the rest of MesenCE.
from dataclasses import replace

import cases
from cases import Case, Operation, READ, WRITE, POLL


def _pad6_across_frame():
    """io_pad6_sequence, started on the frame's last line and stretched over the frame wrap.

    MesenCE ends a frame (and takes save states) when the V counter wraps to line 0. The case
    waits for V counter $F8 (NTSC line 254: the counter runs $00-$EA, then $E5-$FF), so that a save
    at the end of the frame falls after the first TH edges. It then runs the same TH toggles and
    expected reads as io_pad6_sequence with eight work-RAM reads between toggles, so the 6-button
    handshake is in progress at the frame boundary. Each gap between TH changes stays far below
    the pad's ~1.5 ms phase timeout (Core/MD/Input/MdController.h), so the expected values are
    unchanged. Used by replay_check.py (save in the middle of the handshake).
    """
    base = {c.name: c for c in cases.all_cases()}["io_pad6_sequence"]
    ops = list(base.operations)
    filler = Operation(READ, 1, 0xFF0000, 0)
    stretched = [ops[0], Operation(POLL, 2, 0xC00008, 0xF800, compare_mask=0xFF00)]
    for op in ops[1:]:
        if op == filler:
            stretched += [filler] * 8
        else:
            stretched.append(op)
    return replace(base, name="io_pad6_across_frame", operations=tuple(stretched), capture_frames=3)


def _bank_mapper():
    """Sega's bank-switching mapper on a 5 MiB ROM (Super Street Fighter II's board).

    Window 7 ($380000-$3FFFFF) shows bank 7 at power-on; a byte write of 9 to $A130FF maps bank 9
    ($480000), a word write of $0008 to $A130FE (low byte -> $A130FF) maps bank 8 ($400000).
    Window 0 is fixed to bank 0 (the header at $000100 reads "SE").
    """
    rom_data = ((0x380000, 0x12), (0x380001, 0x34),   # bank 7
                (0x400000, 0x56), (0x400001, 0x78),   # bank 8
                (0x480000, 0xAB), (0x480001, 0xCD))   # bank 9
    ops = (
        Operation(READ, 2, 0x380000, 0x1234, compare_mask=0xFFFF),
        Operation(WRITE, 1, 0xA130FF, 0x09),
        Operation(READ, 2, 0x380000, 0xABCD, compare_mask=0xFFFF),
        Operation(WRITE, 2, 0xA130FE, 0x0008),
        Operation(READ, 2, 0x380000, 0x5678, compare_mask=0xFFFF),
        Operation(READ, 2, 0x000100, 0x5345, compare_mask=0xFFFF),
    )
    return Case("mapper_ssf2_banks", ops, rom_size=0x500000, rom_data=rom_data, capture_frames=2)


class _I2c:
    """Bit-banged I2C on one of the cartridge EEPROM boards, as READ/WRITE steps.

    Wiring per board (MAME src/devices/bus/megadrive/eeprom.cpp, jcart.cpp): the address and data
    bit of SDA (console out), SCL and SDA (console in). Lines are written with byte writes; when SDA
    and SCL share an address both are written together. Every READ of SDA has its expected bit.
    """

    BOARDS = {
        "Sega":        (0x200001, 0, 0x200001, 1, 0x200001, 0),
        "Ea":          (0x200001, 7, 0x200001, 6, 0x200001, 7),
        "Acclaim16":   (0x200001, 0, 0x200001, 1, 0x200001, 1),
        "Acclaim32":   (0x200001, 0, 0x200000, 0, 0x200001, 0),
        "Codemasters": (0x300000, 0, 0x300000, 1, 0x380001, 7),
    }

    def __init__(self, board):
        self.sda_addr, self.sda_bit, self.scl_addr, self.scl_bit, self.read_addr, self.read_bit = self.BOARDS[board]
        self.ops = []
        self.scl = self.sda = 1

    def _set(self, scl, sda):
        if self.sda_addr == self.scl_addr:
            self.ops.append(Operation(WRITE, 1, self.sda_addr, (sda << self.sda_bit) | (scl << self.scl_bit)))
        else:
            #Separate addresses: change the line that is not the clock first only when the clock is low
            if sda != self.sda:
                self.ops.append(Operation(WRITE, 1, self.sda_addr, sda << self.sda_bit))
            if scl != self.scl:
                self.ops.append(Operation(WRITE, 1, self.scl_addr, scl << self.scl_bit))
        self.scl, self.sda = scl, sda

    def _read(self, expected):
        self.ops.append(Operation(READ, 1, self.read_addr, expected << self.read_bit, compare_mask=1 << self.read_bit))

    def start(self):
        self._set(self.scl, 1)
        self._set(1, 1)
        self._set(1, 0)
        self._set(0, 0)

    def stop(self):
        self._set(0, 0)
        self._set(1, 0)
        self._set(1, 1)

    def send(self, byte, ack=0):
        for i in range(7, -1, -1):
            bit = (byte >> i) & 1
            self._set(0, bit)
            self._set(1, bit)
            self._set(0, bit)
        self._set(0, 1)  # release SDA for the EEPROM's acknowledge
        self._set(1, 1)
        self._read(ack)
        self._set(0, 1)

    def receive(self, byte, last):
        for i in range(7, -1, -1):
            self._set(1, 1)
            self._read((byte >> i) & 1)
            self._set(0, 1)
        nack = 1 if last else 0
        self._set(0, nack)
        self._set(1, nack)
        self._set(0, nack)
        self._set(0, 1)


def _eeprom_case(name, board, chip, writes, reads):
    """Writes `writes` [(address, bytes)], then reads back [(address, expected bytes)].

    X24C01: one byte of address << 1 | R/W. 24C01-24C16: device byte $A0 | block << 1 | R/W and
    an 8-bit address; 24C64/65: device byte and a 16-bit address. Random reads use a dummy write
    to set the address, then a repeated START.
    """
    bus = _I2c(board)
    bus.stop()  # lines idle high

    def address(addr, read):
        if chip == "X24C01":
            bus.send((addr << 1) | read)
        elif chip in ("C24C64", "C24C65"):
            bus.send(0xA0)
            bus.send(addr >> 8)
            bus.send(addr & 0xFF)
        else:
            bus.send(0xA0 | ((addr >> 8) << 1))
            bus.send(addr & 0xFF)

    for addr, data in writes:
        bus.start()
        address(addr, 0)
        for b in data:
            bus.send(b)
        bus.stop()
    for addr, data in reads:
        bus.start()
        if chip == "X24C01":
            address(addr, 1)
        else:
            address(addr, 0)
            bus.start()
            bus.send(0xA1 | (((addr >> 8) << 1) if chip not in ("C24C64", "C24C65") else 0))
        for i, b in enumerate(data):
            bus.receive(b, i == len(data) - 1)
        bus.stop()
    RUNNER_ARGS[name] = ["--md-eeprom", "%s,%s" % (chip, board)]
    return Case(name, tuple(bus.ops), capture_frames=4)


def _eeprom_cases():
    return [
        #Sega board, X24C01: an erased cell reads $FF; a 4-byte page write from address 5 wraps
        #inside the page ($04-$07), so the fourth byte lands on address 4
        _eeprom_case("eeprom_sega_x24c01", "Sega", "X24C01",
                     [(5, (0x5A, 0xC3, 0x81, 0x7E))],
                     [(0, (0xFF,)), (4, (0x7E, 0x5A, 0xC3, 0x81))]),
        _eeprom_case("eeprom_ea_x24c01", "Ea", "X24C01",
                     [(0x10, (0x12, 0x34))], [(0x10, (0x12, 0x34))]),
        #Acclaim 16M (NBA Jam), 24C02: 8-byte pages; the ninth byte wraps onto the first
        _eeprom_case("eeprom_acclaim16_24c02", "Acclaim16", "C24C02",
                     [(0x10, (1, 2, 3, 4, 5, 6, 7, 8, 9))], [(0x10, (9, 2, 3, 4, 5, 6, 7, 8))]),
        #Acclaim 32M (NBA Jam TE), 24C04: block bit in the device byte selects the upper 256 bytes
        _eeprom_case("eeprom_acclaim32_24c04", "Acclaim32", "C24C04",
                     [(0x1F0, (0xA5,)), (0x0F0, (0x5A,))], [(0x1F0, (0xA5,)), (0x0F0, (0x5A,))]),
        #Codemasters (Micro Machines, Brian Lara), 24C65: 16-bit address
        _eeprom_case("eeprom_codemasters_24c65", "Codemasters", "C24C65",
                     [(0x1234, (0xBE, 0xEF))], [(0x1234, (0xBE, 0xEF))]),
    ]


def _midline_cram(h40):
    """A background colour change in the middle of a line, repeated every frame.

    Each frame: wait for V counter 100, spend a few work-RAM reads so the beam is inside the
    active display, set CRAM entry 0 (the background, register 7 = 0) to red; wait for line 200
    and set it back to black. tests/MD/midline/check_midline.py checks the final picture: lines
    above 100 black, line 100 black then red from one column on, 101-199 red, line 200 red then
    black, the rest black. The exact column is timing-dependent and is not asserted.
    """
    def cram0(color):
        return [Operation(WRITE, 4, 0xC00004, cases._vdp_command(3, 0)), Operation(WRITE, 2, 0xC00000, color)]
    filler = [Operation(READ, 1, 0xFF0000, 0)] * 8  # each interpreted step is a fraction of a line
    frame = ([Operation(POLL, 2, 0xC00008, 0x6400, compare_mask=0xFF00)] + filler + cram0(0x000E) +
             [Operation(POLL, 2, 0xC00008, 0xC800, compare_mask=0xFF00)] + filler + cram0(0x0000))
    return cases._render_scene("vdp_midline_cram_" + ("h40" if h40 else "h32"), {}, h40=h40,
                               extra_operations=tuple(frame * 12), capture_frames=4)


#Extra mesen-headless options per case (gen.py copies them into the run spec)
RUNNER_ARGS = {}


def all_cases():
    return [_pad6_across_frame(), _bank_mapper()] + _eeprom_cases() + [_midline_cram(False), _midline_cram(True)]
