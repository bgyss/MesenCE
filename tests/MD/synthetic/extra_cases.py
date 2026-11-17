# MesenCE's own synthetic Mega Drive cases (not part of nativekit's corpus; no BlastEm reference).
# They reuse cases.py's operation format and expected values and are built by gen.py next to the
# nativekit cases. Copyright notice and license: GPL-3.0, as the rest of MesenCE.
from dataclasses import replace

import cases
from cases import Operation, READ, POLL


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


def all_cases():
    return [_pad6_across_frame()]
