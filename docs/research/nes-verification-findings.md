# NES implementation: verification and testing findings

This is a running log of what the fork's verification tooling has found in MesenCE's NES core:
bugs, deliberate divergences, and checks that came back clean. Each entry gives the evidence and
how to reproduce it. The broader context and the TLA+ model are in
[formal-verification-tla-lean.md](formal-verification-tla-lean.md) §6.

Snapshot: `master` @ `e59476fa` (upstream `4b8a196e` + fork tooling), 2026-11-04, aarch64-darwin.

## Summary

| # | Area | Finding | Status |
|---|---|---|---|
| 1 | DMA (`NesCpu::ProcessPendingDma`) | With *DMC sample duplication glitch* on, a nested DMA restarts OAM DMA: 257 `$2004` writes, OAM shifted by one | **Fixed in this fork** (opt-in setting). Regression test: `mise run test:nes-dma` |
| 2 | DMA (`StopDmcTransfer`) | Pending OAM DMA losing its halt request | **Retracted.** Model artifact |
| 3 | CPU instruction set | 241/256 opcodes match SingleStepTests exactly (2.44M tests: registers, RAM and per-cycle bus trace) | **Clean** |
| 4 | CPU: PLP/RTI | P bits 4/5 differ from the suite | **Not a bug.** Internal representation, never observable |
| 5 | CPU: LXA `$AB` | Result differs in 44% of tests | **Divergence.** Unstable opcode, different "magic constant" |
| 6 | CPU: STP/JAM (12 opcodes) | PC and bus pattern differ while jammed | **Deliberate simplification** |
| 7 | Save states | 248 test ROMs × 300 frames: loading into a fresh console reproduces emulation exactly | **Clean** |
| 8 | Save states | Unmapped (`NoAccess`) pages change encoding across load, so states are not byte-stable (22 ROMs: MMC3, VRC6) | **Minor**, behaviour-neutral |
| 9 | PPU (`NesPpu::GetPixelColor`) | `GetHighestBitIndex(0)` (`__builtin_clz(0)`, UB) gives sprite index −1. In the HD Pack Builder that becomes an out-of-bounds read of `_exSpriteInfo[255]` and OAM | **Fixed in this fork.** Found by UBSan |
| 10 | Undefined behaviour, everything else | UBSan over MesenTests, the CPU suite and the save-state round-trip | **Clean for NES** apart from #9. WonderSwan has two out-of-range palette indexes (not NES; logged) |

## How to reproduce

All of this runs from the Nix dev shell (`direnv allow`, or `nix develop`).

| Task | What it runs |
|---|---|
| `mise run test:cpu` | `tools/nes-cpu-singlestep`. Clones [SingleStepTests/65x02](https://github.com/SingleStepTests/65x02) `nes6502` (1.2 GB) into `bin/` and runs all 256 × 10,000 tests. Exits non-zero only on *unexpected* failures |
| `mise run test:savestate -- <roms or dir>` | `tools/savestate-roundtrip`. Save state → fresh console → compare per frame |
| `mise run test:ci` | The MesenTests regression suite, as upstream CI runs it |
| `mise run tla` | TLA+ model of the DMA scheduler (findings 1 and 2) |

The ROMs used for finding 7 are the `TestRom.nes` files inside MesenTests' `NES/**/*.mtp` archives:
248 unique ROMs covering mappers 0, 1, 2, 3, 4, 5, 7, 12, 21–26, 28, 31, 34 and 69.

---

## 1. OAM DMA restarts when a duplicated DMC DMA lands inside it

See [formal-verification-tla-lean.md §6.3](formal-verification-tla-lean.md#63-the-tla-model-and-first-tlc-results).

- **The bug.** With `EnableDmcSampleDuplicationGlitch` on, a `$4015` enable followed immediately
  by a `$4014` write can start DMC and OAM DMA at the same halt. If the DMC fetch then lands on the
  output unit's reload cycle, the duplication path requests another DMC DMA.
  `ProcessPendingDma` handles that by recursing, and the recursive call restarts the *local* OAM
  counter at 0.
- **Evidence.**
  - TLC counterexample: `MC_NTSC_glitch_prefix_4b8a196e.cfg` (models the pre-fix code).
  - The probe ROM `docs/research/tla/rom/dmc_oam_overlap.s` gave 29 bad transfers in 299 frames,
    each one 257 writes.
- **The fix, applied in this fork.** The OAM DMA position (`_spriteDmaCounter`,
  `_spriteReadAddr`) is now `NesCpu` member state, reset by `RunDMATransfer`
  (`Core/NES/NesCpu.cpp`, `NesCpu.h`). It passes TLC (`MC_NTSC_glitch.cfg`), the probe ROM, and
  MesenTests with the glitch on and off.
- **Regression test** (`mise run test:nes-dma`). The probe ROM is now self-checking:
  - Page `$02` holds 0..255. After every OAM DMA the ROM reads OAM[0] back through `$2003`/`$2004`
    and counts mismatches in RAM.
  - A second timing sweep (a period-256 counter, coprime with the LFSR's 255) was added, because
    the extra check code shifted the loop length off the critical phase.
  - `tools/nes-dma-regression` runs the ROM headlessly with the glitch on for 600 frames.
  - Result: **19 failures on the pre-fix build, 0 with the fix.**

## 2. Retracted: `StopDmcTransfer` clearing a pending OAM halt

The model first allowed any CPU write after the `$4014` write. On a 6502, the only instructions
that write again are read-modify-write instructions, and their final write goes to `$4014` again,
which re-arms the halt. With that modelled, the property holds.

## 3–6. CPU instruction set vs SingleStepTests `nes6502`

**Method.** The harness (`tools/nes-cpu-singlestep/singlestep.cpp`) maps a flat 64 KiB RAM
handler over the whole CPU address space of a real `NesConsole`. It then runs each test through
`DummyNesCpu`: the production `NesCpu` source compiled with `DUMMYCPU`, the build the debugger
uses to predict accesses. That executes the real instruction code, and its bus operations are
logged in order. The harness compares four things:
- final registers;
- final RAM;
- the number of bus cycles;
- each cycle's address, value and read/write direction.

**Caveats.**
- The suite's expected values come from a reference *emulator*, not hardware captures
  ([suite README](https://github.com/SingleStepTests/65x02)). So a mismatch is a lead, not proof.
- The `DUMMYCPU` build skips `StartCpuCycle`/`EndCpuCycle`, so interrupt polling and DMA are not
  exercised here. Findings 1–2 cover DMA.
- `DummyNesCpu` does not apply writes. The harness applies them after the instruction. A read of
  an address written earlier in the same instruction would be flagged as a harness limitation;
  none occurred.

**Result.** 2,435,578 of 2,560,000 tests pass. All 124,422 failures are in the opcodes below;
every other opcode matches on all 10,000 tests, bus trace included. That includes the stable
undocumented opcodes and the `SHX`/`SHY`/`SHA`/`TAS` bus-conflict family.

### 4. PLP (`$28`) and RTI (`$40`): P bits 4 and 5 (not a bug)

- **What differs.** Every test differs only in bit 5 of P: the suite expects `$20` set, MesenCE
  has it clear.
- **Why.** `NesCpu::SetPS` stores `value & 0xCF` (`Core/NES/NesCpu.h:193`), so bits 4 (B) and 5
  are never held internally. They are not real flag-register bits on a 6502; they exist only in
  the byte that PHP/BRK/interrupts push. `PHP` ORs both in (`NesCpu.h:519-523`), and the PHP and
  BRK tests pass with exact bus traces.
- **Conclusion.** Software cannot observe the difference. The harness compares P with
  `& 0xCF`, after which both opcodes pass all 20,000 tests.

### 5. LXA / ATX (`$AB`, unstable): magic constant (divergence)

- **MesenCE:** `A = X = imm` (`NesCpu.h:749-755`), equivalent to a magic constant of `$FF`.
- **The suite:** `(A | $EE) & imm`. This explains **all 10,000** expected values (checked with a
  script). MesenCE's formula agrees in exactly the 5,578 tests where the two coincide.
- **Ground truth.** NESdev's unofficial-opcode pages give no defined NES result for `$AB`; they
  attribute its behaviour to data-bus noise and analog effects. MesenCE passes the
  hardware-written `instr_test`/`nes_instr_test` ROMs in MesenTests.
- **Conclusion.** Documented as a divergence, not a bug. Changing it would need hardware
  evidence from 2A03 chips.

### 6. STP/JAM (`$02 $12 $22 $32 $42 $52 $62 $72 $92 $B2 $D2 $F2`): simplification

- **MesenCE.** `HLT` freezes by re-executing the opcode (`PC -= 1`) and blocks IRQ/NMI
  (`NesCpu.cpp`, `HLT`). It also shows a "CPU crashed" message and can break into the debugger.
- **The suite.** It models the jammed CPU's bus activity: an operand read, then repeated reads of
  `$FFFE`/`$FFFF`, with PC advanced past the opcode.
- **Conclusion.** Both freeze the CPU until reset, so programs can't tell the difference. Only the
  debugger's view of PC and the bus differs.

## 7–8. Save-state round trip

**Method.**
1. Console A loads a ROM, runs W frames, and takes a save state using the same stream as
   `Emulator::Serialize`.
2. A runs K more frames, recording its full serialized state after each one.
3. A **fresh** console B loads the ROM and the state, then runs the same K frames.
4. B's state is compared with A's field by field, after every frame.

Anything that affects emulation but isn't saved makes B diverge from A. That would also break
rewind, netplay, movies and the history viewer. No input is applied, so the ROMs must run on their
own, which test ROMs do.

**Result.**
- **Behaviour:** all 248 ROMs stayed identical for 300 frames after a 120-frame warm-up. Nothing
  emulation-relevant is missing from NES save states for these mappers.
- **Representation (finding 8):** in 22 ROMs, the states differ *byte-wise* from the first frame
  after loading:

  ```
  mapper.prgMemoryOffset: 128 of 1024 bytes differ, first at +384 (expected 0, actual 255)
  mapper.prgMemoryType:   32 of 1024 bytes differ, first at +384 (expected 2, actual 0)
  ```

  Byte +384 is page `$60`, so this is `$6000-$7FFF` with work RAM disabled.
  - MMC3 disables work RAM via `SetCpuMemoryMapping(..., NoAccess)` (`MMC3.h:160-172`). That
    stores `{WorkRam, offset 0, NoAccess}`.
  - On load, `BaseMapper::RestorePrgChrState` (`BaseMapper.cpp:596-615`) maps every `NoAccess`
    page through `RemoveCpuMemoryMapping`, which stores the "unmapped" sentinel
    `{PrgRom, offset 255}`.
  - Both read open bus, so emulation is identical. But a state that is loaded and then saved
    again is not byte-identical to the original.
  - Visible effects: the debugger's mapping view shows a different type for those pages, and
    anything comparing save-state bytes or hashes sees a difference. Affected here: every MMC3
    ROM, VRC6 (`vrc6test24`/`26`), and the MMC3-based `sprite_evaluation_test`.
  - **Fix candidates (not applied):** keep type and offset for `NoAccess` pages in
    `RestorePrgChrState`, or always canonicalize `NoAccess` pages to the sentinel.

## 9. `NesPpu::GetPixelColor`: sprite index −1 when no sprite shifter is active

**Found by** UndefinedBehaviorSanitizer while running MesenTests:

```
Utilities/BitUtilities.h:57:39: runtime error: passing zero to __builtin_clz(), which is not a valid argument
    #0 NesPpu<DefaultNesPpu>::GetPixelColor() NesPpu.cpp:863
Core/NES/NesPpu.cpp:863:18: runtime error: index 4294967295 out of bounds for type 'NesSpriteInfo[64]'
```

**The code** (`Core/NES/NesPpu.cpp:858-863`):

```cpp
if(_processSprites && _prevRenderingEnabled) {
    uint8_t remainingShifters = _dotSkipped ? 0xFF : _activeSpriteShifters;
    _lastSprite = &_spriteTiles[BitUtilities::GetHighestBitIndex(_activeSpriteShifters)];
```

- **Why it fires.**
  - `_processSprites` is `_spriteCount || _activeSpriteShifters || _dotSkipped`
    (`NesPpu.cpp:1076`). So it is true on pixels where every sprite shifter has already drained
    (`_activeSpriteShifters == 0`), for example later on a scanline that has sprites.
  - `GetHighestBitIndex(0)` is `31 - __builtin_clz(0)` (`Utilities/BitUtilities.h:57`). That is
    undefined behaviour.
  - On ARM64 the `CLZ` instruction returns 32, which gives index −1 and
    `_lastSprite = &_spriteTiles[-1]`.
  - On x86, `__builtin_clz(0)` usually compiles to `BSR`/`LZCNT`, with a different or undefined
    result. So the value also differs by platform.
- **Impact.**
  - **Default PPU:** none observable. `_lastSprite` is only read by the HD-pack PPUs
    (`BaseNesPpu.h:76`, "used by HD ppu").
  - **`HdNesPpu` (HD pack playback):** `_lastSprite` is only used as an "is there a sprite here"
    flag, and sprites are re-checked by X position. So the result is extra work, not wrong
    output.
  - **`HdBuilderPpu` (HD Pack Builder recording): real out-of-bounds reads.**
    `spriteIndex = (uint8_t)(_lastSprite - _spriteTiles)` evaluates to 255, and
    `_exSpriteInfo[255]` is indexed although the array has 64 entries (`HdBuilderPpu.h:19,
    93-94`). `_lastSprite->PaletteOffset` reads `_spriteTiles[-1]`, which is the end of
    `_spriteRam` (OAM, `BaseNesPpu.h:84-88`). If the out-of-bounds `AbsoluteTileAddr` happens to
    be ≥ 0, the builder records a bogus sprite tile with a garbage palette into the HD pack.
- **The fix, applied in this fork** (`NesPpu.cpp`, `GetPixelColor`). It was first validated in a
  throwaway UBSan build. Only take
  the highest active shifter when one exists:

  ```cpp
  if(_activeSpriteShifters) {
      _lastSprite = &_spriteTiles[BitUtilities::GetHighestBitIndex(_activeSpriteShifters)];
  }
  ```

  The HD PPUs reset `_lastSprite = nullptr` before each pixel (`HdNesPpu.h:84`,
  `HdBuilderPpu.h:68`), and the `_spriteCount > 8` path below sets it when a sprite pixel is
  found. So "no active shifter" now means "no sprite" for the HD code.
- **Evidence after the fix:** MesenTests passes 612 / 0 / 218 under UBSan, and there are no NES
  UBSan reports from MesenTests, the CPU suite, or the 248-ROM save-state round-trip.
- **Not yet done:** a direct HD Pack Builder reproduction showing a bogus recorded tile. The
  out-of-bounds index is certain from the code; whether garbage reaches the pack depends on the
  memory past the array.

## 10. Undefined behaviour: method, and the rest of the results

- **Build.** UBSan with `-fsanitize=undefined -fsanitize-recover=undefined` at `-O1 -g`, via
  `make core pgohelper MESENFLAGS=...` in a throwaway worktree.
- **AddressSanitizer couldn't be used on this machine.** With the Nix LLVM 21 runtime on macOS
  (Darwin 27), every ASan binary hangs inside its own startup (`__asan::InitializeShadowMemory` →
  `get_dyld_hdr` → `dyld_shared_cache_iterate_text_swift`) before `main`. Retry it on Linux, or
  with a newer runtime.
- **What was run under UBSan:** MesenTests (all systems), the full SingleStepTests CPU suite,
  and the 248-ROM save-state round-trip. The only NES report was #9.
- **Not NES, logged here only** (WonderSwan, during MesenTests; not investigated):
  - `Core/WS/WsPpu.h:69`: index 767 / 316 out of bounds for `BwPalettes[64]`.
  - `Core/WS/WsPpu.h:70`: index 118 / 8 out of bounds for `BwShades[8]`.
  - `Core/WS/WsDefaultVideoFilter.cpp:93`: index 22386 out of bounds for
    `_calculatedPalette[0x1000]`.

  Monochrome rendering receives palette/colour values that are out of range, and the indexes vary
  between runs, which points to a stale or uninitialized value.
