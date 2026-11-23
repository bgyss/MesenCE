# Synthetic Mega Drive bus/VDP/audio/I-O cases.
# Copied unchanged (apart from this header) from nativekit
# consoles/md/tests/rom/cases.py at commit 16d61e725e808776e499fa4f8fb7c0f7ba20607c, so that
# MesenCE and nativekit build the same 59 cartridges and their outputs can be compared case
# by case. nativekit is licensed AGPL-3.0; this file keeps that license. MesenCE builds the
# cartridges with its own encoder (gen.py) and needs no assembler.
"""Shared bus operations used by the BlastEm ROM and native testcart."""
from dataclasses import dataclass


READ = 1
WRITE = 2
TAS = 3
POLL = 4


@dataclass(frozen=True)
class Operation:
    code: int
    width: int
    addr: int
    value: int = 0
    compare_mask: int = 0


@dataclass(frozen=True)
class Case:
    name: str
    operations: tuple[Operation, ...]
    direct: bool = False
    render: bool = False
    region: str = "U"
    capture_frames: int = 2
    capture_audio: bool = False
    capture_ym_audio: bool = False
    capture_mixed_audio: bool = False
    weave_fields: bool = False
    weave_dimensions: tuple[int, int] = (256, 224)
    z80_fixture_id: int = 0
    z80_entry: int = 0
    z80_program: tuple[str, ...] = ()
    z80_load_index: int = 0
    z80_accesses: tuple[Operation, ...] = ()
    rom_size: int = 32768
    rom_data: tuple[tuple[int, int], ...] = ()
    pad1: str = "6"
    pad2: str = "6"
    input_frames: tuple[tuple[tuple[str, ...], tuple[str, ...]], ...] = ()
    model: str = "md1va3"


def _vdp_reg(index, value):
    return Operation(WRITE, 2, 0xC00004, 0x8000 | (index << 8) | value)


def _vdp_command(code, address):
    first = ((code & 3) << 14) | (address & 0x3FFF)
    second = ((code & 0x3C) << 2) | ((address >> 14) & 3)
    return (first << 16) | second


def _memory_dma_case(name, write_code, read_code, display=0x14, h40=False, words=2):
    source = 0x4000
    source_hi = (source >> 17) & 0x7F
    source_mid = (source >> 9) & 0xFF
    source_lo = (source >> 1) & 0xFF
    operations = []
    if display & 0x40:
        operations.append(_vdp_reg(0, 0x04))  # Select Mode 5 before the display starts.
    operations.extend([
        _vdp_reg(1, display),
        _vdp_reg(12, 0x81 if h40 else 0),
        _vdp_reg(15, 2),
        _vdp_reg(19, words & 0xFF),
        _vdp_reg(20, (words >> 8) & 0xFF),
        _vdp_reg(21, source_lo),
        _vdp_reg(22, source_mid),
        _vdp_reg(23, source_hi),
        Operation(WRITE, 4, 0xC00004, _vdp_command(write_code | 0x20, 0)),
        Operation(WRITE, 4, 0xC00004, _vdp_command(read_code, 0)),
        *(Operation(READ, 2, 0xC00000) for _ in range(words)),
    ])
    return Case(name, tuple(operations))


def _vdp_write_words(code, address, words):
    return [Operation(WRITE, 4, 0xC00004, _vdp_command(code, address)),
            *(Operation(WRITE, 2, 0xC00000, word) for word in words)]


def _render_scene(name, tile_colors, plane_a_rows=None, plane_b_row=None,
                  window_row=None, hscroll=(0, 0), vsram=(0, 0),
                  mode11=0, h40=False, v30=False, plane_size=0,
                  window_h=0, window_v=0, region="U", sprite_entries=None,
                  display=True, background=0, render=True,
                  vblank_sync=True,
                  shadow_highlight=False, interlace=0, cram_colors=None,
                  extra_operations=(),
                  capture_frames=2, weave_fields=False):
    width_cells = 64 if plane_size & 0x03 == 1 else 32
    operations = [
        (Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008)
         if vblank_sync else
         Operation(POLL, 2, 0xC00008, 0xE000, compare_mask=0xFF00)),
        _vdp_reg(0, 0x04),
        _vdp_reg(1, (0x44 if display else 0x04) | (0x08 if v30 else 0)),
        _vdp_reg(2, 0x10),
        _vdp_reg(3, 0x20),
        _vdp_reg(4, 0x03),
        _vdp_reg(7, background),
        _vdp_reg(11, mode11),
        _vdp_reg(12, (0x81 if h40 else 0) | ((interlace & 0x03) << 1) |
                 (0x08 if shadow_highlight else 0)),
        _vdp_reg(13, 0x2E),
        _vdp_reg(15, 2),
        _vdp_reg(16, plane_size),
        _vdp_reg(17, window_h),
        _vdp_reg(18, window_v),
    ]
    sprite_base = 0xA800 if h40 else 0xFE00
    if sprite_entries:
        operations.append(_vdp_reg(5, 0x54 if h40 else 0x7F))

    for tile, color in sorted(tile_colors.items()):
        packed = color * 0x1111
        operations.extend(_vdp_write_words(1, tile * 32, [packed] * 16))
    operations.extend(_vdp_write_words(
        3, 2, cram_colors or [0x000E, 0x00E0, 0x0E00, 0x0EEE]))

    for row, entries in sorted((plane_a_rows or {}).items()):
        operations.extend(_vdp_write_words(1, 0x4000 + row * width_cells * 2, entries))
    if plane_b_row:
        operations.extend(_vdp_write_words(1, 0x6000, plane_b_row))
    if window_row:
        operations.extend(_vdp_write_words(1, 0x8000, window_row))
    if sprite_entries:
        operations.extend(_vdp_write_words(1, sprite_base,
                                           [word for entry in sprite_entries for word in entry]))
    operations.extend(_vdp_write_words(1, 0xB800, hscroll))
    if vsram != (0, 0):
        operations.extend(_vdp_write_words(5, 0, vsram))
    operations.extend(extra_operations)
    return Case(name, tuple(operations), render=render, region=region,
                capture_frames=capture_frames, weave_fields=weave_fields,
                weave_dimensions=(320 if h40 else 256, 240 if v30 else 224))


def _z80_bank_window_case():
    z80_program = (
        "        ld      a,1",
        *("        ld      ($6000),a" for _ in range(7)),
        "        xor     a",
        *("        ld      ($6000),a" for _ in range(2)),
        "        ld      a,($FFFF)",
        "        ld      ($0010),a",
        "hang:   jr      hang",
    )
    operations = [
        Operation(WRITE, 2, 0xA11200, 0x0000),
        Operation(WRITE, 2, 0xA11100, 0x0100),
        Operation(WRITE, 2, 0xA11200, 0x0100),
        Operation(POLL, 2, 0xA11100, 0x0000, compare_mask=0x0100),
        Operation(WRITE, 2, 0xA11100, 0x0000),
        *(Operation(READ, 1, 0xE00000) for _ in range(48)),
        Operation(WRITE, 2, 0xA11100, 0x0100),
        Operation(POLL, 2, 0xA11100, 0x0000, compare_mask=0x0100),
        Operation(READ, 1, 0xA00010, 0xC3, compare_mask=0x00FF),
    ]
    z80_accesses = tuple(
        [Operation(WRITE, 1, 0x6000, 1) for _ in range(7)] +
        [Operation(WRITE, 1, 0x6000, 0) for _ in range(2)] +
        [Operation(READ, 1, 0xFFFF, 0xC3)])
    return Case("z80_bank_window", tuple(operations), z80_fixture_id=1,
                z80_entry=0, z80_program=z80_program, z80_load_index=4,
                z80_accesses=z80_accesses, rom_size=0x400000,
                rom_data=((0x3FFFFF, 0xC3),))


def _z80_psg_case(name, fixture_id, z80_program, writes, result):
    operations = [
        Operation(WRITE, 2, 0xA11200, 0x0000),
        Operation(WRITE, 2, 0xA11100, 0x0100),
        Operation(WRITE, 2, 0xA11200, 0x0100),
        Operation(POLL, 2, 0xA11100, 0x0000, compare_mask=0x0100),
        Operation(WRITE, 2, 0xA11100, 0x0000),
        *(Operation(READ, 1, 0xE00000) for _ in range(48)),
        Operation(WRITE, 2, 0xA11100, 0x0100),
        Operation(POLL, 2, 0xA11100, 0x0000, compare_mask=0x0100),
        Operation(READ, 1, 0xA00010, result, compare_mask=0x00FF),
    ]
    accesses = tuple(Operation(WRITE, 1, 0x7F11, value) for value in writes)
    return Case(name, tuple(operations), z80_fixture_id=fixture_id,
                z80_entry=0, z80_program=z80_program, z80_load_index=4,
                z80_accesses=accesses, capture_audio=True, capture_frames=1)


def _z80_psg_tone_case():
    return _z80_psg_case("sound_psg_tone", 2, (
        "        ld      a,$90",
        "        ld      ($7F11),a",
        "        ld      a,$84",
        "        ld      ($7F11),a",
        "        ld      a,$12",
        "        ld      ($7F11),a",
        "        ld      a,$5A",
        "        ld      ($0010),a",
        "hang:   jr      hang",
    ), (0x90, 0x84, 0x12), 0x5A)


def _z80_psg_noise_case():
    return _z80_psg_case("sound_psg_noise", 3, (
        "        ld      a,$E0",
        "        ld      ($7F11),a",
        "        ld      a,$E4",
        "        ld      ($7F11),a",
        "        ld      a,$F0",
        "        ld      ($7F11),a",
        "        ld      a,$A5",
        "        ld      ($0010),a",
        "hang:   jr      hang",
    ), (0xE0, 0xE4, 0xF0), 0xA5)


def _psg_data_only_volume_case():
    return Case("sound_psg_data_only_volume", (
        Operation(WRITE, 1, 0xC00011, 0x90),  # Channel 0, volume latch, full level.
        Operation(WRITE, 1, 0xC00011, 0x3F),  # Continuation updates volume to mute.
        Operation(WRITE, 1, 0xC00011, 0x81),  # Tone 0, divisor 1.
        Operation(WRITE, 1, 0xC00011, 0x00),
    ), capture_audio=True, capture_frames=1)


def _psg_tone3_noise_switch_case():
    operations = [
        Operation(WRITE, 1, 0xC00011, 0xC4),  # Tone 3 divider 4.
        Operation(WRITE, 1, 0xC00011, 0x00),
        Operation(WRITE, 1, 0xC00011, 0xE0),  # Fixed noise source.
    ]
    operations.extend(Operation(READ, 1, 0xE00000) for _ in range(57))
    operations.extend((
        Operation(WRITE, 1, 0xC00011, 0xE3),  # Select tone-3 noise clock mid-period.
        Operation(WRITE, 1, 0xC00011, 0xF0),  # Make noise audible.
    ))
    return Case("sound_psg_tone3_noise_switch", tuple(operations),
                capture_audio=True, capture_frames=1)


def _io_pad6_sequence_case():
    operations = [
        Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
        Operation(READ, 1, 0xA10003, 0x7F, compare_mask=0x7F),  # Settle the idle TH pull-up.
        Operation(WRITE, 1, 0xA10003, 0x40),  # Preload TH high while it is an input.
        Operation(WRITE, 1, 0xA10009, 0x40),  # Drive TH high without an initial edge.
        Operation(READ, 1, 0xA10003, 0x7F, compare_mask=0x7F),
        Operation(READ, 1, 0xA10005, 0x6F, compare_mask=0x7F),
        Operation(WRITE, 1, 0xA10003, 0x00),
        *(Operation(READ, 1, 0xFF0000) for _ in range(3)),
        Operation(READ, 1, 0xA10003, 0x33, compare_mask=0x7F),
        Operation(WRITE, 1, 0xA10003, 0x40),
        *(Operation(READ, 1, 0xFF0000) for _ in range(3)),
        Operation(READ, 1, 0xA10003, 0x7F, compare_mask=0x7F),
        Operation(WRITE, 1, 0xA10003, 0x00),
        *(Operation(READ, 1, 0xFF0000) for _ in range(3)),
        Operation(READ, 1, 0xA10003, 0x33, compare_mask=0x7F),
        Operation(WRITE, 1, 0xA10003, 0x40),
        *(Operation(READ, 1, 0xFF0000) for _ in range(3)),
        Operation(READ, 1, 0xA10003, 0x7F, compare_mask=0x7F),
        Operation(WRITE, 1, 0xA10003, 0x00),
        *(Operation(READ, 1, 0xFF0000) for _ in range(3)),
        Operation(READ, 1, 0xA10003, 0x30, compare_mask=0x7F),
        Operation(WRITE, 1, 0xA10003, 0x40),
        *(Operation(READ, 1, 0xFF0000) for _ in range(3)),
        Operation(READ, 1, 0xA10003, 0x70, compare_mask=0x7F),
        Operation(WRITE, 1, 0xA10003, 0x00),
        *(Operation(READ, 1, 0xFF0000) for _ in range(3)),
        Operation(READ, 1, 0xA10003, 0x3F, compare_mask=0x7F),
    ]
    return Case("io_pad6_sequence", tuple(operations), pad1="6", pad2="3",
                input_frames=((
                    ("MODE", "X", "Y", "Z"),
                    ("A", "B"),
                ),))


def _io_tmss_unlock_case():
    return Case("io_tmss_unlock", (
        Operation(READ, 1, 0xA10001, 0xA1, compare_mask=0xFF),
        Operation(WRITE, 1, 0xA14000, 0x53),
        Operation(READ, 2, 0xA14000, 0x5300, compare_mask=0xFFFF),
        Operation(WRITE, 1, 0xA14001, 0x45),
        Operation(READ, 2, 0xA14000, 0x5345, compare_mask=0xFFFF),
        Operation(WRITE, 4, 0xA14000, 0x53454741),
        Operation(READ, 2, 0xA14000, 0x5345, compare_mask=0xFFFF),
        Operation(READ, 2, 0xA14002, 0x4741, compare_mask=0xFFFF),
        Operation(WRITE, 2, 0xC00004, 0x8104),
        Operation(WRITE, 2, 0xC00000, 0x000E),
    ), model="md1va6")


def _io_expansion_port_case():
    """The expansion port, port 3, with nothing connected: its data and
    control registers read, then written and read back (byte accesses; word
    reads of the I/O registers differ in the high byte, a separate finding)."""
    return Case("io_expansion_port", (
        Operation(READ, 1, 0xA10007),
        Operation(READ, 1, 0xA1000D),
        Operation(WRITE, 1, 0xA1000D, 0x40),
        Operation(READ, 1, 0xA1000D),
        Operation(WRITE, 1, 0xA10007, 0x40),
        Operation(READ, 1, 0xA10007),
        Operation(WRITE, 1, 0xA10007, 0x00),
        Operation(READ, 1, 0xA10007),
        Operation(WRITE, 2, 0xC00004, 0x8104),
        Operation(WRITE, 2, 0xC00000, 0x000E),
    ))


def _io_version_md1va3_case():
    return Case("io_version_md1va3", (
        Operation(READ, 1, 0xA10001, 0xA0, compare_mask=0xFF),
        Operation(WRITE, 2, 0xC00004, 0x8104),
        Operation(WRITE, 2, 0xC00000, 0x000E),
    ))


def _ym_write_register(bank, address, value):
    port = 0xA04000 + (bank * 2)
    return (Operation(WRITE, 1, port, address),
            Operation(WRITE, 1, port + 1, value))


def _ym_timer_case(name, timer_registers, control, status_mask):
    accesses = [
        *timer_registers,
        *_ym_write_register(0, 0x27, control),
        Operation(POLL, 1, 0xA04000, status_mask, compare_mask=status_mask),
        Operation(READ, 1, 0xA04000, status_mask, compare_mask=status_mask),
    ]
    return Case(name, tuple(_ym_bus_request() + tuple(accesses) + _ym_bus_release()))


def _ym_bus_request():
    return (
        Operation(WRITE, 2, 0xA11200, 0x0000),
        Operation(WRITE, 2, 0xA11100, 0x0100),
        Operation(WRITE, 2, 0xA11200, 0x0100),
        Operation(POLL, 2, 0xA11100, 0x0000, compare_mask=0x0100),
    )


def _ym_bus_release():
    return ()


def _ym_fm_channel_ops(channel, algorithm, attack=31, decay=0, sustain_rate=0,
                       release=15, channel3_special=False, lfo=False, pan=0xC0,
                       key_scale_register=0):
    operator_offsets = (0, 4, 8, 12)
    bank = channel // 3
    local = channel % 3
    operations = []

    if channel3_special:
        operations.extend(_ym_write_register(0, 0x27, 0x40))
    if lfo:
        operations.extend(_ym_write_register(0, 0x22, 0x0F))

    for offset in operator_offsets:
        reg = 0x30 + local + offset
        operations.extend(_ym_write_register(bank, reg, 0x01))
        operations.extend(_ym_write_register(bank, reg + 0x10, 0x00))
        operations.extend(_ym_write_register(
            bank, reg + 0x20, ((key_scale_register & 3) << 6) | (attack & 0x1F)))
        operations.extend(_ym_write_register(bank, reg + 0x30, decay & 0x1F))
        operations.extend(_ym_write_register(bank, reg + 0x40, sustain_rate & 0x1F))
        operations.extend(_ym_write_register(bank, reg + 0x50, release & 0xFF))
        operations.extend(_ym_write_register(bank, reg + 0x60, 0x00))

    operations.extend(_ym_write_register(bank, 0xA4 + local, 0x22))
    operations.extend(_ym_write_register(bank, 0xA0 + local, 0x69))
    operations.extend(_ym_write_register(bank, 0xB0 + local, algorithm & 0x3F))
    operations.extend(_ym_write_register(bank, 0xB4 + local, 0xF7 if lfo else pan))

    if channel3_special:
        for register, value in ((0xAC, 0x00), (0xA8, 0x58), (0xAD, 0x38),
                                (0xA9, 0x70), (0xAE, 0x08), (0xAA, 0x88)):
            operations.extend(_ym_write_register(0, register, value))
    operations.extend(_ym_write_register(0, 0x28, 0xF0 | channel))
    return operations


def _ym_algorithm_case(algorithm):
    operations = [
        *_ym_bus_request(),
        Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
        *_ym_fm_channel_ops(0, algorithm),
    ]
    return Case(f"sound_ym_algorithm_{algorithm}", tuple(operations),
                capture_ym_audio=True, capture_frames=2)


def _ym_sound_cases():
    cases = [
        Case("sound_ym_ports", (
            *_ym_bus_request(),
            *_ym_write_register(0, 0x30, 0xA5),
            *_ym_write_register(1, 0x31, 0xB6),
            *(Operation(READ, 1, 0xA04000 + port, 0, compare_mask=0x03)
              for port in range(4)),
            *_ym_bus_release(),
        )),
        _ym_timer_case("sound_ym_timer_a", (
            *_ym_write_register(0, 0x24, 0xFF),
            *_ym_write_register(0, 0x25, 0x03),
        ), 0x05, 0x01),
        _ym_timer_case("sound_ym_timer_b", (
            *_ym_write_register(0, 0x26, 0xFE),
        ), 0x0A, 0x02),
        Case("sound_ym_dac", (
            *_ym_bus_request(),
            Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
            *_ym_write_register(0, 0x2B, 0x80),
            *_ym_write_register(0, 0x2A, 0xFF),
            *_ym_bus_release(),
        ), capture_ym_audio=True, capture_frames=2),
    ]
    cases.extend(_ym_algorithm_case(algorithm) for algorithm in range(8))
    envelope_operations = [
        *_ym_bus_request(),
        *_ym_fm_channel_ops(0, 7, attack=28, decay=12, sustain_rate=4, release=10),
        Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
        *_ym_write_register(0, 0x28, 0x00),
    ]
    cases.append(Case("sound_ym_envelope_key_on_off", tuple(envelope_operations),
                      capture_ym_audio=True, capture_frames=3))
    channel3_operations = [
        *_ym_bus_request(),
        Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
        *_ym_fm_channel_ops(2, 7, attack=3, channel3_special=True, lfo=True,
                            key_scale_register=0),
    ]
    cases.append(Case("sound_ym_channel3_lfo", tuple(channel3_operations),
                      capture_ym_audio=True, capture_frames=4))
    pan_operations = [
        *_ym_bus_request(),
        Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
        *_ym_fm_channel_ops(0, 7, pan=0x80),
        *_ym_fm_channel_ops(1, 7, pan=0x40),
    ]
    cases.append(Case("sound_ym_pan", tuple(pan_operations),
                      capture_ym_audio=True, capture_frames=2))
    mixed_operations = [
        *_ym_bus_request(),
        Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
        *_ym_fm_channel_ops(0, 7),
        Operation(WRITE, 1, 0xC00011, 0x90),
        Operation(WRITE, 1, 0xC00011, 0x84),
        Operation(WRITE, 1, 0xC00011, 0x12),
    ]
    cases.append(Case("sound_mixed_psg_ym", tuple(mixed_operations),
                      capture_mixed_audio=True, capture_frames=2))
    return cases


def all_cases():
    return [Case("bus_map", (
        Operation(READ, 2, 0x00004000),
        Operation(WRITE, 2, 0xE00000, 0xABCD),
        Operation(READ, 2, 0xFF0000),
        Operation(WRITE, 1, 0xE00000, 0x12),
        Operation(READ, 2, 0xFF0000),
        Operation(WRITE, 4, 0xFF0002, 0x13579BDF),
        Operation(READ, 4, 0xE00002),
        Operation(READ, 2, 0x01004000),
        Operation(WRITE, 2, 0xA11000, 0x0100),
        *_ym_bus_request(),
        Operation(WRITE, 1, 0xA04000, 0x30),
        *_ym_bus_release(),
        Operation(WRITE, 1, 0xA06000, 0x01),
        Operation(WRITE, 1, 0xA10003, 0xFF),
        Operation(WRITE, 2, 0xC00004, 0x8104),
        Operation(WRITE, 2, 0xC00006, 0x8104),
        Operation(WRITE, 2, 0xD00004, 0x8104),
        Operation(WRITE, 2, 0xC00004, 0x8F02),
        Operation(WRITE, 4, 0xC00004, 0x40800000),
        Operation(WRITE, 2, 0xC00000, 0x000E),
        Operation(WRITE, 1, 0xC00011, 0x9F),
        Operation(WRITE, 2, 0xA11200, 0x0000),
        Operation(WRITE, 2, 0xA11100, 0x0100),
        Operation(WRITE, 2, 0xA11200, 0x0100),
        Operation(POLL, 2, 0xA11100, 0x0000, compare_mask=0x0100),
        Operation(WRITE, 1, 0xA00000, 0x5A),
        Operation(READ, 1, 0xA00000),
        Operation(WRITE, 2, 0xA11100, 0x0000),
        Operation(POLL, 2, 0xA11100, 0x0100, compare_mask=0x0100),
        Operation(WRITE, 1, 0xFF0010, 0x00),
        Operation(READ, 1, 0xFF0010),
    )), _z80_bank_window_case(), _z80_psg_tone_case(), _z80_psg_noise_case(),
    _psg_data_only_volume_case(), _psg_tone3_noise_switch_case(),
    _io_pad6_sequence_case(), _io_tmss_unlock_case(), _io_version_md1va3_case(),
    _io_expansion_port_case(),
    *_ym_sound_cases(), Case("tas_write", (
        Operation(WRITE, 1, 0xFF0020, 0x00),
        Operation(TAS, 1, 0xFF0020),
        Operation(READ, 1, 0xFF0020),
    )), Case("vdp_port_timing", (
        Operation(WRITE, 2, 0xC00004, 0x8104),
        Operation(WRITE, 2, 0xC00000, 0x000E),
        Operation(READ, 2, 0xC00004, compare_mask=0x0001),
        Operation(READ, 2, 0xC00000),
    ), direct=True), Case("vdp_fifo", (
        Operation(WRITE, 2, 0xC00004, 0x8154),
        Operation(WRITE, 2, 0xC00004, 0x8C01),
        Operation(WRITE, 2, 0xC00004, 0x8F02),
        Operation(WRITE, 4, 0xC00004, 0x40000000),
        *(Operation(WRITE, 2, 0xC00000, 0x1234 + i) for i in range(16)),
    )), _memory_dma_case("dma_memory_vram", 1, 0),
    _memory_dma_case("dma_memory_cram", 3, 8),
    _memory_dma_case("dma_memory_vsram", 5, 4), Case("dma_fill", (
        _vdp_reg(1, 0x14),
        _vdp_reg(15, 1),
        _vdp_reg(19, 3),
        _vdp_reg(20, 0),
        _vdp_reg(23, 0x80),
        Operation(WRITE, 4, 0xC00004, _vdp_command(0x21, 0x100)),
        Operation(WRITE, 2, 0xC00000, 0xA500),
        *(Operation(READ, 2, 0xE00000) for _ in range(20)),
        Operation(WRITE, 4, 0xC00004, _vdp_command(0, 0x100)),
        Operation(READ, 2, 0xC00000),
        Operation(READ, 2, 0xC00000),
        Operation(READ, 2, 0xC00000),
    )), Case("dma_copy", (
        _vdp_reg(1, 0x14),
        _vdp_reg(15, 1),
        Operation(WRITE, 4, 0xC00004, _vdp_command(1, 0)),
        Operation(WRITE, 2, 0xC00000, 0xA55A),
        _vdp_reg(19, 2),
        _vdp_reg(20, 0),
        _vdp_reg(21, 0),
        _vdp_reg(22, 0),
        _vdp_reg(23, 0xC0),
        Operation(WRITE, 4, 0xC00004, _vdp_command(0x21, 4)),
        *(Operation(READ, 2, 0xE00000) for _ in range(8)),
        Operation(WRITE, 4, 0xC00004, _vdp_command(0, 4)),
        Operation(READ, 2, 0xC00000),
    )), _memory_dma_case("dma_active_vram_h32", 1, 0,
                          display=0x54, words=4),
    _memory_dma_case("dma_active_vram_h40", 1, 0,
                     display=0x54, h40=True, words=4), Case("render_plane_a", (
        Operation(POLL, 2, 0xC00004, 0x0008, compare_mask=0x0008),
        _vdp_reg(0, 0x04),
        _vdp_reg(1, 0x44),
        _vdp_reg(2, 0x10),
        _vdp_reg(7, 0),
        _vdp_reg(11, 0),
        _vdp_reg(12, 0),
        _vdp_reg(13, 0),
        _vdp_reg(15, 2),
        _vdp_reg(16, 0),
        Operation(WRITE, 4, 0xC00004, _vdp_command(1, 0)),
        *(op for _row in range(8) for op in (
            Operation(WRITE, 2, 0xC00000, 0x1111),
            Operation(WRITE, 2, 0xC00000, 0x1111),
        )),
        Operation(WRITE, 4, 0xC00004, _vdp_command(1, 0x4000)),
        Operation(WRITE, 2, 0xC00000, 0),
        Operation(WRITE, 4, 0xC00004, _vdp_command(3, 2)),
        Operation(WRITE, 2, 0xC00000, 0x000E),
        Operation(WRITE, 4, 0xC00004, _vdp_command(0, 0)),
        Operation(READ, 2, 0xC00000),
        Operation(WRITE, 4, 0xC00004, _vdp_command(0, 0x4000)),
        Operation(READ, 2, 0xC00000),
    ), render=True), _render_scene("render_plane_b", {1: 2}, plane_b_row=[1]),
    _render_scene("render_tile_map", {0: 1, 1: 2, 2: 3, 3: 4},
                  plane_a_rows={0: [0, 1, 2, 3]}),
    _render_scene("render_scroll_horizontal", {0: 1, 1: 2, 2: 3},
                  plane_a_rows={0: [0, 1, 2, 0]}, hscroll=(8, 0)),
    _render_scene("render_scroll_vertical", {0: 1, 1: 2},
                  plane_a_rows={1: [1] * 32}, vsram=(8, 0)),
    _render_scene("render_window", {0: 1, 1: 3}, window_row=[1, 1], window_h=1),
    _render_scene("render_h40", {0: 1}, h40=True, plane_size=1),
    _render_scene("render_h32_v30", {0: 1}, v30=True, region="E"),
    _render_scene("render_h40_v30", {0: 1}, h40=True, v30=True,
                  plane_size=1, region="E"),
    _render_scene("render_display_disabled", {0: 1},
                  plane_a_rows={0: [0]}, background=2, display=False),
    _render_scene("render_sprite_basic", {0: 1, 1: 2},
                  sprite_entries=[(0x0080, 0x0000, 0x0001, 0x0080)]),
    _render_scene("render_sprite_clipping", {0: 1, 1: 2},
                  sprite_entries=[(0x0080, 0x0000, 0x0001, 0x007C)]),
    _render_scene("render_sprite_link_priority", {0: 1, 1: 2, 2: 3},
                  sprite_entries=[(0x0080, 0x0002, 0x0000, 0x0080),
                                  (0x0080, 0x0000, 0x0002, 0x0080),
                                  (0x0080, 0x0000, 0x0001, 0x0080)]),
    _render_scene("render_plane_sprite_priority", {0: 1, 1: 2},
                  plane_a_rows={0: [0x8000]},
                  sprite_entries=[(0x0080, 0x0000, 0x0001, 0x0080)]),
    _render_scene("render_sprite_overflow_h32", {0: 0, 1: 2, 2: 1},
                  plane_a_rows={0: [2]},
                  sprite_entries=[(0x0080, index + 1, 0, 0x0080)
                                  for index in range(16)] +
                                 [(0x0080, 0, 1, 0x0080)]),
    _render_scene("render_sprite_overflow_h40", {0: 0, 1: 2, 2: 1}, h40=True,
                  plane_size=1, plane_a_rows={0: [2]},
                  sprite_entries=[(0x0080, index + 1, 0, 0x0080)
                                  for index in range(20)] +
                                 [(0x0080, 0, 1, 0x0080)]),
    _render_scene("render_shadow_operator", {0: 1, 1: 15}, plane_a_rows={0: [0]},
                  sprite_entries=[(0x0080, 0, 0x6001, 0x0080)],
                  shadow_highlight=True, cram_colors=[0x0008, 0x00E0, 0x0E00, 0x0EEE]),
    _render_scene("render_highlight_operator", {0: 1, 1: 14},
                  plane_a_rows={0: [0x8000]},
                  sprite_entries=[(0x0080, 0, 0x6001, 0x0080)],
                  shadow_highlight=True, cram_colors=[0x0008, 0x00E0, 0x0E00, 0x0EEE]),
    _render_scene("render_interlace2", {0: 1}, interlace=3, weave_fields=True),
    _render_scene("render_midline_register", {0: 1},
                  extra_operations=(Operation(POLL, 2, 0xC00008, 0x0010,
                                              compare_mask=0xFFF0),
                                    _vdp_reg(7, 1)),
                  capture_frames=1),
    _render_scene("sprite_overflow_status_h32", {0: 0, 1: 2},
                  sprite_entries=[(0x0080, index + 1, 0, 0x0080)
                                  for index in range(16)] +
                                 [(0x0080, 0, 1, 0x0080)],
                  render=False,
                  extra_operations=(Operation(POLL, 2, 0xC00004, 0,
                                              compare_mask=0x0008),
                                    Operation(POLL, 2, 0xC00008, 0x0100,
                                              compare_mask=0xFF00),
                                    Operation(READ, 2, 0xC00004, 0x0040,
                                              compare_mask=0x0040))),
    _render_scene("sprite_overflow_status_reread_h32", {0: 0, 1: 2},
                  sprite_entries=[(0x0080, index + 1, 0, 0x0080)
                                  for index in range(16)] +
                                 [(0x0080, 0, 1, 0x0080)],
                  render=False,
                  extra_operations=(Operation(POLL, 2, 0xC00004, 0,
                                              compare_mask=0x0008),
                                    Operation(POLL, 2, 0xC00008, 0x0100,
                                              compare_mask=0xFF00),
                                    Operation(READ, 2, 0xC00004, 0x0040,
                                              compare_mask=0x0040),
                                    Operation(READ, 2, 0xC00004, 0,
                                              compare_mask=0x0040))),
    _render_scene("sprite_overflow_status_h40", {0: 0, 1: 2}, h40=True,
                  plane_size=1,
                  sprite_entries=[(0x0080, index + 1, 0, 0x0080)
                                  for index in range(20)] +
                                 [(0x0080, 0, 1, 0x0080)],
                  render=False,
                  extra_operations=(Operation(POLL, 2, 0xC00004, 0,
                                              compare_mask=0x0008),
                                    Operation(POLL, 2, 0xC00008, 0x0100,
                                              compare_mask=0xFF00),
                                    Operation(READ, 2, 0xC00004, 0x0040,
                                              compare_mask=0x0040))),
    _render_scene("sprite_collision_status", {1: 2},
                  sprite_entries=[(0x0080, 1, 1, 0x0080),
                                  (0x0080, 0, 1, 0x0080)],
                  render=False,
                  extra_operations=(Operation(POLL, 2, 0xC00004, 0,
                                              compare_mask=0x0008),
                                    Operation(POLL, 2, 0xC00008, 0x0100,
                                              compare_mask=0xFF00),
                                    Operation(READ, 2, 0xC00004, 0x0020,
                                              compare_mask=0x0020)))]
