# MesenCE's own synthetic Mega Drive cases (not part of nativekit's corpus; no BlastEm reference).
# They reuse cases.py's operation format and expected values and are built by gen.py next to the
# nativekit cases. Copyright notice and license: GPL-3.0, as the rest of MesenCE.
from dataclasses import replace
import pathlib

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


def _sk_lock_on(name, title, expected, address=0x200100):
    """Sonic & Knuckles on its own: the lock-on window $200000-$3FFFFF is empty.

    S&K decides whether a cartridge sits on top of it by reading the header at $200100-$20010F
    ("SEGA ..."). MesenCE mirrored every ROM up to the next power of two over $000000-$3FFFFF, so a
    2 MiB S&K saw its own header there and showed its "NO WAY?" screen (found by md-corpus against
    GPGX and PicoDrive, which both show the title). A 2 MiB ROM whose overseas name ($150) is
    "SONIC & KNUCKLES" now reads $FF there, as GPGX (md_cart.c: "disable ROM mirroring at
    $200000-$3fffff (normally mapped to external cartridge)", padded with $FF); any other 2 MiB ROM
    is still mirrored (the control case reads a marker at ROM $010100 through its mirror at
    $210100: a header-less ROM of 2 MiB or less has battery RAM at $200000-$20FFFF, C2). The $FF value is GPGX's; the empty slot's bus value is not
    measured (judgment call: S&K only needs it not to read "SEGA").
    """
    rom_data = tuple((0x150 + i, c) for i, c in enumerate(title.ljust(48).encode())) + ((0x10100, 0x53), (0x10101, 0x45))
    ops = (Operation(READ, 2, address, expected, compare_mask=0xFFFF),
           Operation(READ, 2, 0x000100, 0x5345, compare_mask=0xFFFF))
    return Case(name, ops, rom_size=0x200000, rom_data=rom_data, capture_frames=2)


def _sram_cases():
    """Battery RAM that MesenCE left unmapped (found by md-corpus, findings C1/C2).

    cart_sram_above_padded_rom: HardBall '95's layout, a 3 MiB ROM with an "RA" header for
    odd-byte RAM at $300001-$30FFFF. RAM above the ROM is enabled at power-on, but MesenCE compared
    with the ROM padded to 4 MiB, so the RAM stayed off and the game reported "SRAM failure".
    cart_sram_headerless_hardball3: HardBall III has battery RAM but no "RA" header; PicoDrive
    (carthw.cfg, "bad headers") and GPGX (default for header-less ROMs up to 2 MiB) map 64 KiB at
    $200000-$20FFFF, both lanes. Without it the game stops on "First time initialization
    complete. Press RESET to continue."
    cart_sram_headerless_default: any other header-less ROM up to 2 MiB gets 64 KiB at
    $200000-$20FFFF, both lanes (GPGX's default; Madden NFL 98, FIFA Soccer 97 and NHL 98 keep
    data there with no header).
    cart_sram_headerless_product: battery RAM matched by product code, as GPGX's "autodetect games
    with missing header infos" (Buck Rogers - Countdown to Doomsday, T-50286: 16 KiB on the odd
    bytes of $200001-$203FFF, 1 MiB ROM). Without it the game stopped reading its pads.
    """
    ra = b"RA" + bytes((0xF8, 0x20)) + (0x300001).to_bytes(4, "big") + (0x30FFFF).to_bytes(4, "big")
    above = Case("cart_sram_above_padded_rom",
                 (Operation(WRITE, 1, 0x300001, 0x5A),
                  Operation(READ, 1, 0x300001, 0x5A, compare_mask=0xFF),
                  Operation(WRITE, 1, 0x300003, 0xA5),
                  Operation(READ, 1, 0x300003, 0xA5, compare_mask=0xFF)),
                 rom_size=0x300000, rom_data=tuple((0x1B0 + i, b) for i, b in enumerate(ra)), capture_frames=2)
    name = tuple((0x150 + i, c) for i, c in enumerate(b"  HardBall III".ljust(48)))
    headerless = Case("cart_sram_headerless_hardball3",
                      (Operation(WRITE, 2, 0x200000, 0x1234),
                       Operation(READ, 2, 0x200000, 0x1234, compare_mask=0xFFFF),
                       Operation(WRITE, 2, 0x20FFFE, 0x5678),
                       Operation(READ, 2, 0x20FFFE, 0x5678, compare_mask=0xFFFF)),
                      rom_size=0x200000, rom_data=name, capture_frames=2)
    product = tuple((0x180 + i, c) for i, c in enumerate(b"GM T-50286 -00"))
    by_product = Case("cart_sram_headerless_product",
                      (Operation(WRITE, 1, 0x200001, 0x5A),
                       Operation(READ, 1, 0x200001, 0x5A, compare_mask=0xFF),
                       Operation(WRITE, 1, 0x203FFF, 0xA5),
                       Operation(READ, 1, 0x203FFF, 0xA5, compare_mask=0xFF)),
                      rom_size=0x100000, rom_data=product, capture_frames=2)
    default = Case("cart_sram_headerless_default",
                   (Operation(WRITE, 2, 0x200000, 0x1357),
                    Operation(READ, 2, 0x200000, 0x1357, compare_mask=0xFFFF),
                    Operation(WRITE, 1, 0x204575, 0x9A),
                    Operation(READ, 1, 0x204575, 0x9A, compare_mask=0xFF),
                    Operation(READ, 2, 0x210100, 0x5345, compare_mask=0xFFFF)),
                   rom_size=0x200000, rom_data=((0x10100, 0x53), (0x10101, 0x45)), capture_frames=2)
    return [above, headerless, by_product, default]


def _z80_busack_reset():
    """BUSREQ/BUSACK ($A11100 bit 8, 0 = the 68000 has the Z80 bus) with the Z80 held in reset.

    A Z80 in reset doesn't acknowledge a bus request, so the bit reads 1; it reads 0 once reset is
    released with the request held. Measured on BlastEm (nativekit's oracle core) and as MAME
    (megadriv.cpp: z80_is_reset reads 1). The Terminator (Sega CD) requests the bus with the Z80 in
    reset and waits for the bit to read 1 (findings: mcd-verification S5). Re-asserting reset
    after the grant is not checked: MAME reads 1, BlastEm keeps the grant (0).
    """
    bit = 0x0100
    ops = (
        Operation(WRITE, 2, 0xA11200, 0x0000),                     # Z80 reset
        Operation(WRITE, 2, 0xA11100, 0x0000),
        Operation(READ, 2, 0xA11100, bit, compare_mask=bit),       # reset, no request: 1
        Operation(WRITE, 2, 0xA11100, 0x0100),                     # bus request
        Operation(READ, 2, 0xA11100, bit, compare_mask=bit),       # reset + request: 1
        Operation(READ, 1, 0xA11100, 0x01, compare_mask=0x01),     # byte read: same
        Operation(WRITE, 2, 0xA11200, 0x0100),                     # release reset
        Operation(POLL, 2, 0xA11100, 0x0000, compare_mask=bit),    # granted
        Operation(WRITE, 2, 0xA11100, 0x0000),                     # release the bus
        Operation(READ, 2, 0xA11100, bit, compare_mask=bit),
    )
    return Case("z80_busack_reset", ops, capture_frames=2)


def _z80_area_open_bus():
    """A 68000 read of the Z80 area without the Z80 bus (findings C10). Nothing drives the data bus:
    the 68000 reads open bus, its prefetched instruction word (Genesis Plus GX: m68k_read_bus_8 is
    mapped at $A0xxxx while the bus is not granted; PicoDrive returns a floating value). The
    interpreter reads bytes with `move.b (a1),d1` followed by `bra next` ($60xx), so the YM2612
    status at $A04000 reads $60. MesenCE returned $FF, and Mega Games 3, which polls the YM2612's
    busy bit after its sound interrupt has released the bus, hung on a black screen.
    """
    ops = (
        Operation(WRITE, 2, 0xA11200, 0x0100),                     # Z80 out of reset, running
        Operation(WRITE, 2, 0xA11100, 0x0000),                     # no bus request
        Operation(READ, 1, 0xA04000, 0x60, compare_mask=0xFF),     # open bus: the prefetch's high byte
    )
    return Case("z80_area_open_bus", ops, capture_frames=2)


def _z80_vint_during_dma():
    """The Z80's /INT pulse keeps its own timing while the 68000 is halted by a DMA (findings C13).

    The Z80 runs IM 1 with a handler at $0038 that counts entries at $1000 and then waits about 350
    Z80 cycles (longer than the 171.5-cycle pulse, BlastEm's measured width); its main loop is
    `ei` / `jr` back. In active display the 68000 clears the counter, waits for VBlank and at once
    starts a VRAM DMA of $400 words, which halts it for several lines, then reads the counter in the
    next active display: one entry. MesenCE used to update the Z80's /INT only after each 68000
    instruction, so the Z80 ran the whole DMA with /INT asserted and re-entered the handler (seven
    times here; up to seven times a frame in Generations Lost).
    """
    program = (
        "        db $F3",                     # di
        "        db $ED,$56",                 # im 1
        "        db $31,$00,$1F",             # ld sp,$1F00
        "        db $C3,$50,$00",             # jp $0050
        "        db " + ",".join(["0"] * (0x38 - 9)),
        "        db $F5",                     # $0038: push af
        "        db $3A,$00,$10",             # ld a,($1000)
        "        db $3C",                     # inc a
        "        db $32,$00,$10",             # ld ($1000),a
        "        db $C5",                     # push bc
        "        db $06,$18",                 # ld b,24
        "        db $10,$FE",                 # djnz $ (24 x 13 cycles)
        "        db $C1",                     # pop bc
        "        db $F1",                     # pop af
        "        db $ED,$4D",                 # reti
        "        db " + ",".join(["0"] * (0x50 - 0x4A)),
        "        db $FB",                     # $0050: ei
        "        db $18,$FD",                 # jr $0050
    )
    vblank = 0x0008
    dma_regs = [Operation(WRITE, 2, 0xC00004, v) for v in (0x8154, 0x8F02, 0x9300, 0x9404, 0x9500, 0x9600, 0x9700)]
    def one_frame():
        return [
            Operation(POLL, 2, 0xC00004, 0x0000, compare_mask=vblank),   # active display
            Operation(WRITE, 2, 0xA11100, 0x0100),
            Operation(POLL, 2, 0xA11100, 0x0000, compare_mask=0x0100),
            Operation(WRITE, 1, 0xA01000, 0x00),                          # counter = 0
            Operation(WRITE, 2, 0xA11100, 0x0000),
            *dma_regs,
            Operation(POLL, 2, 0xC00004, vblank, compare_mask=vblank),   # VBlank: /INT pulse
            Operation(WRITE, 4, 0xC00004, 0x40000080),                    # VRAM DMA, $400 words
            Operation(POLL, 2, 0xC00004, 0x0000, compare_mask=vblank),   # next active display
            Operation(WRITE, 2, 0xA11100, 0x0100),
            Operation(POLL, 2, 0xA11100, 0x0000, compare_mask=0x0100),
            Operation(READ, 1, 0xA01000, 0x01, compare_mask=0xFF),        # one handler entry
            Operation(WRITE, 2, 0xA11100, 0x0000),
        ]
    operations = [
        Operation(WRITE, 2, 0xA11200, 0x0000),
        Operation(WRITE, 2, 0xA11100, 0x0100),
        Operation(WRITE, 2, 0xA11200, 0x0100),
        Operation(POLL, 2, 0xA11100, 0x0000, compare_mask=0x0100),
        Operation(WRITE, 2, 0xA11100, 0x0000),
        Operation(WRITE, 2, 0xC00004, 0x8154),                            # display and DMA on
    ] + one_frame() + one_frame()
    return Case("z80_vint_during_dma", tuple(operations), z80_fixture_id=4, z80_entry=0,
                z80_program=program, z80_load_index=4, capture_frames=20)


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


_ACCESSORIES = pathlib.Path(__file__).resolve().parents[1] / "accessories"


def _wait_frames(count):
    """Pass `count` frames (VBlank flag set, then clear, `count` times): Lua input is set from then on."""
    ops = []
    for _ in range(count):
        ops.append(Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008))
        ops.append(Operation(POLL, 2, 0xC00004, 0x0000, compare_mask=0x0008))
    return ops


def _team_player():
    """Team Player on port 1 in Multi mode (pads A=6, B=3, C=none, D=6; tests/MD/accessories/teamplayer.lua).

    The 68000 drives TH and TR ($A10009 = $60) and walks the protocol (MdTeamPlayer.h): each read
    expects TL to follow TR and the nibble for that step. Pressed: A Up+A, B Start, D Mode+X.
    """
    data, ctrl = 0xA10003, 0xA10009
    steps = [  # (TH/TR written, expected port byte under $7F)
        (0x60, 0x73),  # idle: TL=1, data 0011
        (0x20, 0x3F),  # TH low: selected, 1111
        (0x00, 0x00), (0x20, 0x30),  # 0000 0000
        (0x00, 0x01), (0x20, 0x30), (0x00, 0x0F), (0x20, 0x31),  # IDs: A 6-btn, B 3-btn, C none, D 6-btn
        (0x00, 0x0E), (0x20, 0x3B), (0x00, 0x0F),  # A: RLDU (Up), SACB (A), MXYZ
        (0x20, 0x3F), (0x00, 0x07),  # B: RLDU, SACB (Start)
        (0x20, 0x3F), (0x00, 0x0F), (0x20, 0x33),  # D: RLDU, SACB, MXYZ (Mode, X)
        (0x00, 0x00),  # past the last pad
        (0x60, 0x73),  # TH high: idle again
    ]
    ops = [cases._vdp_reg(1, 0x44)] + _wait_frames(3) + [Operation(WRITE, 1, ctrl, 0x60)]
    for written, expected in steps:
        ops += [Operation(WRITE, 1, data, written), Operation(READ, 1, data, expected, compare_mask=0x7F)]
    RUNNER_ARGS["io_teamplayer"] = ["--md-pads", "t:63n6,6", "--lua", str(_ACCESSORIES / "teamplayer.lua")]
    return Case("io_teamplayer", tuple(ops), capture_frames=6)


def _mouse():
    """Sega Mouse on port 1 (tests/MD/accessories/mouse.lua: +5 X, +3 Y (up), left button).

    After each TR edge the 68000 polls until TL follows TR (the mouse answers about 12 us later),
    then reads the nibble: ID B, F, F, flags 0, buttons 1, X 0/5, Y 0/3 (MdMouse.h).
    """
    data, ctrl = 0xA10003, 0xA10009
    ops = [cases._vdp_reg(1, 0x44)] + _wait_frames(3) + [Operation(WRITE, 1, ctrl, 0x60), Operation(WRITE, 1, data, 0x60),
                             Operation(READ, 1, data, 0x70, compare_mask=0x7F),
                             Operation(WRITE, 1, data, 0x20), Operation(READ, 1, data, 0x3B, compare_mask=0x7F)]
    for i, nibble in enumerate((0xF, 0xF, 0x0, 0x1, 0x0, 0x5, 0x0, 0x3)):
        tr = 0x20 if i & 1 else 0x00
        ops += [Operation(WRITE, 1, data, tr),
                Operation(POLL, 1, data, 0x10 if tr else 0x00, compare_mask=0x10),
                Operation(READ, 1, data, tr | (0x10 if tr else 0) | nibble, compare_mask=0x7F)]
    ops.append(Operation(WRITE, 1, data, 0x60))
    RUNNER_ARGS["io_mouse"] = ["--md-pads", "m,6", "--lua", str(_ACCESSORIES / "mouse.lua")]
    return Case("io_mouse", tuple(ops), capture_frames=6)


def _light_gun(name, gun, h40, expected_hv, probe):
    """A light gun on port 2 aimed at pixel (100, 50) latches the H/V counter through the HL input.

    VDP register 0 bit 1 (latch) and register 11 bit 3 (level 2 interrupt) are set, and port 2's
    control register has bit 7 (HL) with TH as an input. After a full frame the H/V counter read
    returns the value latched when the gun pulled TH low (MdLightGun.h): line 50, H = pixel / 2
    plus the gun's latency. `probe` adds the gun's own pin checks.
    """
    data, ctrl = 0xA10005, 0xA1000B
    ops = [Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
           cases._vdp_reg(0, 0x06), cases._vdp_reg(1, 0x44), cases._vdp_reg(11, 0x08),
           cases._vdp_reg(12, 0x81 if h40 else 0x00)]
    ops += _wait_frames(3) + list(probe)
    ops += [Operation(WRITE, 1, data, 0x00), Operation(WRITE, 1, ctrl, 0xB0)] + _wait_frames(2)
    ops.append(Operation(READ, 2, 0xC00008, expected_hv, compare_mask=0xFFFF))
    RUNNER_ARGS[name] = ["--md-pads", "6," + gun, "--lua", str(_ACCESSORIES / (("menacer" if gun == "g" else "justifier") + ".lua"))]
    return Case(name, tuple(ops), capture_frames=8)


def _input_latch(mode, second):
    """When the pads take each frame's input (mesen-headless --md-input-latch, findings C4).

    tests/MD/accessories/input_latch.json holds B on frame 5 only. The 68000 waits for frame 5's
    VBlank and reads the pad (TH high: ?1CBRLDU), then waits for line 1 of frame 6
    and reads again. With the frame latch, frame 6's input (nothing) is in from line 0; with the
    VBlank latch, as in Genesis Plus GX, frame 5's input (B) holds until frame 6's VBlank.
    """
    data, ctrl = 0xA10003, 0xA10009
    ops = [cases._vdp_reg(1, 0x44), Operation(WRITE, 1, ctrl, 0x40), Operation(WRITE, 1, data, 0x40)] + _wait_frames(4)
    ops += [Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
            Operation(READ, 1, data, 0x2F, compare_mask=0x3F),  # frame 5, VBlank: B pressed
            # The VBlank flag clears on line 261, before the wrap: wait for the V counter's line 1
            Operation(POLL, 2, 0xC00008, 0x0100, compare_mask=0xFF00),
            Operation(READ, 1, data, second, compare_mask=0x3F)]  # frame 6, line 1
    name = "io_input_latch_" + mode
    RUNNER_ARGS[name] = ["--md-pads", "3,3", "--md-input-latch", mode, "--input", str(_ACCESSORIES / "input_latch.json")]
    return Case(name, tuple(ops), capture_frames=8)


def _accessory_cases():
    menacer_probe = [Operation(WRITE, 1, 0xA1000B, 0x30), Operation(WRITE, 1, 0xA10005, 0x00),
                     Operation(READ, 1, 0xA10005, 0x49, compare_mask=0x4F)]  # TH high, Start + B (trigger)
    justifier_probe = [Operation(WRITE, 1, 0xA1000B, 0x40),  # ID probe: TH output
                       Operation(WRITE, 1, 0xA10005, 0x40), Operation(READ, 1, 0xA10005, 0x70, compare_mask=0x7F),
                       Operation(WRITE, 1, 0xA10005, 0x00), Operation(READ, 1, 0xA10005, 0x33, compare_mask=0x7F)]
    return [_team_player(), _mouse(),
            # H40: pixel 0 latches H $52 (the 6-in-1 offset), pixel 100 latches $83
            _light_gun("io_menacer_hl", "g", True, 0x3283, menacer_probe),
            # H32, no latency: H = 50 = $32
            _light_gun("io_justifier_hl", "j", False, 0x3232, justifier_probe),
            _input_latch("frame", 0x3F), _input_latch("vblank", 0x2F)]


#Extra mesen-headless options per case (gen.py copies them into the run spec)
RUNNER_ARGS = {}

#Extra keys for the generated run spec (e.g. replay_warmup), by case
SPEC_EXTRA = {}

#Checks across several reads of one case (compare.py): name -> function(READ values) -> problems
DERIVED_CHECKS = {}

#Cases built by their own function instead of gen.build (the 32X cartridges): name -> build(case)
CUSTOM_BUILD = {}

#Cases checked on their picture by their own script (their steps run until the capture ends, so
#compare.py's expected-read check does not apply): tests/MD/midline/check_midline.py
PICTURE_ONLY = {"vdp_midline_cram_h32", "vdp_midline_cram_h40"}


def _mcd_cases():
    """Sega CD unit in Mode 1 (mcd_cases.py), run with --md-mcd."""
    import mcd_cases
    found = mcd_cases.all_cases()
    for case in found:
        RUNNER_ARGS[case.name] = list(mcd_cases.RUNNER_ARGS_MCD) + mcd_cases.RUNNER_ARGS_EXTRA.get(case.name, [])
    DERIVED_CHECKS.update(mcd_cases.DERIVED_CHECKS)
    SPEC_EXTRA.update(mcd_cases.SPEC_EXTRA)
    return found


def _m32x_cases():
    """32X cartridges (m32x_cases.py): replacement boot ROMs, SH-2 programs, expected values."""
    import m32x_cases
    found = m32x_cases.all_cases()
    for case in found:
        CUSTOM_BUILD[case.name] = m32x_cases.build_32x
    DERIVED_CHECKS.update(m32x_cases.DERIVED_CHECKS)
    RUNNER_ARGS.update(m32x_cases.RUNNER_ARGS)
    return found


def write_assets(out_dir):
    """Files the cases need next to the cartridges (the Sega CD test disc)."""
    import mcd_cases
    mcd_cases.write_assets(out_dir)


def all_cases():
    return ([_pad6_across_frame(), _bank_mapper(), _z80_busack_reset(), _z80_area_open_bus(), _z80_vint_during_dma(),
             _sk_lock_on("cart_sk_lock_on_empty", "SONIC & KNUCKLES", 0xFFFF),
             _sk_lock_on("cart_2m_mirror", "NATIVEKIT BUS TEST", 0x5345, 0x210100)] + _sram_cases() + _eeprom_cases() + [_midline_cram(False), _midline_cram(True)] +
            _accessory_cases() + _mcd_cases() + _m32x_cases())
