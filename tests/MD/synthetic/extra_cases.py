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


def all_cases():
    return [_pad6_across_frame(), _bank_mapper()]
