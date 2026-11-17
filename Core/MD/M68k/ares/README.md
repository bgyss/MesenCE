# ares M68000 component (ported)

Source: <https://github.com/ares-emulator/ares>, `ares/component/processor/m68000`, commit
`140c0a322aee6773936da4b6a43f3cf054a74e6a` (2026-11-16). License: ISC, see `LICENSE` in
this directory; the notice must be kept with these files.

The `.cpp` files were renamed to `.inc` (they are textually included by
`Core/MD/M68k/M68kCore.cpp`, as ares does) and `m68000.hpp` became `m68000.h`.
`serialization.cpp` was dropped; `MdM68k` serializes the register file itself.
nall primitives are provided by `Core/MD/M68k/AresCompat.h`.

Every behavioural change is marked `//MesenCE:` in the code. All of them were made to match
the SingleStepTests/m68000 suite (MAME's microcoded 68000; `mise run test:m68k-cpu`):

- bus: odd word/long accesses raise an address error (`addressError`, handled as a group 0
  exception in `M68kCore`); program fetches, PC-relative operands and TAS cycles are tagged
  with an access kind (function code class);
- `push<Long>` (BSR/JSR/PEA/LINK) writes the high word first; group 1/2 exception frames are
  written PC low, SR, PC high with two dead cycles between the final prefetches;
- read-modify-write long writes store the low word first; ADDX/SUBX `-(An)` long reads fetch
  the low word first; `-(An)` decrements before the access;
- `MOVE <memory>,(xxx).l` performs the write before the last extension word leaves the queue;
- ANDI/ORI/EORI to CCR/SR and MOVE to CCR/SR re-read the extension word before refilling the
  queue; TRAPV performs its prefetch cycle before the frame; STOP takes its operand from IRC
  without a bus cycle;
- UNLK only replaces A7 once the frame-pointer read succeeded;
- the stopped state idles instead of issuing a bus wait.

See `docs/research/md-verification-findings.md` for the evidence and the one remaining
known divergence.
