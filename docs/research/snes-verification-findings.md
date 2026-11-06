# SNES implementation: verification and testing findings

This is a running log of what the fork's verification tooling has found in MesenCE's SNES core
(`Core/SNES`): bugs, deliberate divergences, and checks that came back clean. The method mirrors
the NES and Game Boy work in [nes-verification-findings.md](nes-verification-findings.md) and
[gb-verification-findings.md](gb-verification-findings.md). So far it covers the two CPUs: the
SPC700 sound CPU (findings 1–5), the 65816 main CPU (findings 6–10) and the SA-1's 65816
(findings 11–15).

Snapshot: `apply-verified-fixes` @ `c8ce0381`, 2026-11-05, aarch64-darwin.

## Summary

| # | Area | Finding | Status |
|---|---|---|---|
| 1 | SPC700 CPU | 254/256 opcodes match SingleStepTests on **bus trace, final registers, RAM and cycle count** across 249,535 tests (after fix 2) | **Clean** |
| 2 | CPU: `BRK` (`$0F`), `DAS` (`$BE`), `DAA` (`$DF`) | The first cycle after the opcode was idle; hardware does a dummy read of PC | **Fixed in this fork** (ares agrees with the suite) |
| 3 | Debugger: `SLEEP`/`STOP` (`$EF`/`$FF`) on `DummySpc` | Null-pointer dereference (`_emu` is null in the dummy CPU) | **Fixed in this fork** |
| 4 | CPU: `SLEEP`/`STOP` (`$EF`/`$FF`) | Endless-loop modelling differs from the suite | **Divergence**, not observable |
| 5 | `FolderUtilities::CombinePath("", ...)` | Reads `folder[-1]`. Loading a SNES cartridge from an in-memory file crashes in `Msu1::Init` | **Fixed in this fork** |
| 6 | 65816 CPU | 506/512 opcode files (native + emulation mode) match SingleStepTests on **bus trace, cycle count, registers and RAM** across 5,020,136 tests (after fix 7) | **Clean** |
| 7 | 65816: read-modify-write in emulation mode (28 opcodes) | The dummy write of the unmodified value ran the CPU cycle twice: `CycleCount`, NMI edge detection and DMA polling advanced by 2 | **Fixed in this fork** |
| 8 | 65816: `(dp,X)` in emulation mode, DL ≠ 0, pointer high byte at a page start | MesenCE wraps the high-byte fetch within the page (upstream `c0ea1245`), as ares does; the suite doesn't | **Divergence**, not changed (264 tests) |
| 9 | 65816: `WDM` (`$42`) | MesenCE fetches the signature byte; the suite has an internal cycle | **Divergence**, not changed |
| 10 | 65816: `WAI`/`STP` (`$CB`/`$DB`) | The 2 internal cycles happen in the halted state instead of in the instruction | **Divergence**, bookkeeping |
| 11 | SA-1 CPU (`Sa1Cpu`) | Same result as the main CPU: 5,020,136 tests pass, with the same divergences 8–10 (after fixes 12–13) | **Clean** |
| 12 | SA-1: vector fetch (`BRK`, `COP`, and NMI/IRQ entry) | The two vector reads took no CPU cycles: `BRK`/`COP` were 2 cycles short | **Fixed in this fork** (NMI/IRQ part: judgment call) |
| 13 | SA-1: read-modify-write in emulation mode | NMI edge detection ran twice for the dummy-write cycle | **Fixed in this fork** |
| 14 | SA-1: read-modify-write in native mode, with the debugger | The modify cycle was reported to the SNES CPU's debugger instead of the SA-1's | **Fixed in this fork** (code reading only) |
| 15 | System level | MesenTests has **no** SA-1 tests. The fork's `tests/SNES/sa1_timing.mtp` now checks fix 12 (BRK/COP) and the SA-1 memory speeds through the whole system | **Partly closed** |
| 16 | Save states | Save state → fresh console → 120 frames compared, on all 83 SNES ROMs in MesenTests plus the fork's SA-1 ROM | **Clean** (the check catches a removed `SV()`; see below) |
| 17 | 65816 interrupts (SNES CPU and SA-1) | 12 scenarios per CPU against the WDC datasheet and ares: entry sequence and cycle counts, pushed registers, NMI vs I, WAI, interruptible MVN (after fix 18) | **Clean** |
| 18 | 65816: IRQ/NMI in emulation mode | The pushed P had B (bit 4) = 1, as for BRK, so a handler on the shared `$FFFE` vector couldn't tell an IRQ from a BRK | **Fixed in this fork** |

## How to reproduce

| Task | What it runs |
|---|---|
| `mise run test:spc-cpu` | `tools/spc-cpu-singlestep`. Clones [SingleStepTests/spc700](https://github.com/SingleStepTests/spc700) (about 100 MB) into `bin/` and runs all 256 × 1,000 tests. Exits non-zero only on *unexpected* failures. It also loads an in-memory SNES cartridge first, which is the regression test for finding 5 |
| `mise run test:snes-cpu` | `tools/snes-cpu-singlestep`. Clones [SingleStepTests/65816](https://github.com/SingleStepTests/65816) (about 3.1 GB checked out) into `bin/` and runs all 512 × 10,000 tests (MVN/MVP: 200 each). Exits non-zero only on *unexpected* failures. Before fix 7, it fails |
| `mise run test:sa1-cpu` | The same harness with `--cpu sa1`. Before fixes 12 and 13, it fails |
| `mise run test:ci` | MesenTests. Its 91 SNES tests pass with fixes 2, 3, 5, 7 and 12–14 applied |

---

## 1–4. SPC700 vs SingleStepTests `spc700`

**Method.** `tools/spc-cpu-singlestep/singlestep.cpp` runs every test on **`DummySpc`**: the
`Spc` source (`Spc.cpp` + `Spc.Instructions.cpp`) compiled with `DUMMYSPC`, which the debugger
uses for predictive breakpoints and effective addresses. It shares all instruction code with
the real `Spc`. Only `Read`/`Write` differ: the dummy logs writes instead of applying them, and
has no DSP behind `$F3`.

- **Bus trace:** the logged reads and writes must match the suite's `read`/`write` cycles in
  order. A `null` address or value in the suite is a wildcard. `wait` cycles must have no access.
- **Timing:** `SpcState::Cycle` advances by 2 per SPC cycle at the default speed. Its total must
  equal the number of cycle entries, `wait` included.
- **Final state:** registers, plus RAM = initial RAM with the logged writes applied in order.

The real `Spc` isn't used. `Spc::Run()` runs to a cycle target, and some instructions end with a
step that takes no time (e.g. a branch that isn't taken ends with a zero-cycle `EndOp`). So an
outside caller can't stop it on an instruction boundary.

**Notes on the suite.**
- The expected values are generated by an emulator (ares-derived), not captured from hardware.
- The suite assumes flat RAM everywhere. On the SPC, `$F0-$FF` are I/O registers and
  `$FFC0-$FFFF` is the IPL ROM (the harness disables the ROM). **4,465 tests** that touch
  `$F0-$FF` are skipped, which leaves 251,535 tests.

**Result.** 249,535 of 251,535 pass, with 0 unexpected failures. The 2,000 known divergences are
SLEEP and STOP (finding 4).

### 2. `BRK`, `DAS`, `DAA`: dummy read of PC (fixed)

Before the fix, all three failed 1000/1000 on the bus trace, with the correct cycle count:

```
0F 0000:
  actual bus:   R5058=0F W016B=50 W016A=59 W0169=94 RFFDE=10 RFFDF=F6
  expected bus: R5058=0F R5059=?? W016B=50 W016A=59 W0169=94 -- RFFDE=10 RFFDF=F6
BE 0000:
  actual bus:   RE938=BE
  expected bus: RE938=BE RE939=?? --
```

**Source.** ares' SPC700 core (`instructionBreak`, `instructionDecimalAdjustAdd/Sub`) starts all
three with `read(PC)`, like every other implied-operand SPC700 instruction (which MesenCE
already implements as `DummyRead()`).

**Effect.** Mostly the debugger's bus trace. There is one timing effect: `Idle()` is clocked at
the *internal* speed, while a dummy read of RAM is clocked at the *external* speed (test
register `$F0` bits 4–7). The two only differ when a program changes `$F0`, which games don't
do.

**Fix.** In each of the three, `case 0: Idle()` became `case 0: DummyRead()`. This is off any hot
path.

### 3. `SLEEP`/`STOP` crash `DummySpc` (fixed)

**Reproduction.** Running `bin/SingleStepTests-spc700/v1/ef.json` through the harness before the
fix ends with SIGSEGV (exit 139). `Spc::STOP()` calls `_emu->GetSoundMixer()->StopAudio()`, and
`DummySpc` is constructed with only a RAM pointer, so `_emu` is null.

**Reachability (from reading the code; not reproduced end to end in the UI).**
`SpcDebugger::ProcessInstruction` runs `_dummyCpu->Step()` before every SPC instruction when
`_step->StepCount != 0` (its default is -1, i.e. while running), the SPC breakpoint manager has
breakpoints, and "Use predictive breakpoints" is enabled (the default). So: the debugger is open
with any SPC breakpoint, and the SPC executes SLEEP or STOP. The emulator then crashes.

**Fix.** The StopAudio call is under `#ifndef DUMMYSPC`. A dummy CPU must not have side effects
anyway.

### 4. `SLEEP`/`STOP`: modelling of the stopped state (divergence)

The suite records 3 iterations of ares' endless `read(PC); idle()` loop, with PC left at
opcode+1. MesenCE re-executes the opcode instead: PC stays on it, at 1 cycle per pass. Only a
reset leaves this state, and nothing in it is observable to software. **Not changed.** It's
listed as a known divergence in the harness.

## 5. `FolderUtilities::CombinePath` with an empty folder (fixed)

**Reproduction.** The SPC harness first built its SNES console with
`VirtualFile(data, size, "singlestep.sfc")`. `SnesConsole::LoadRom` → `Msu1::Init` →
`FolderUtilities::CombinePath(romFile.GetFolderPath() /* "" */, ...)`, which evaluates
`folder[folder.length() - 1]`, i.e. `folder[npos]`. The Nix clang/libc++ build has hardening on,
so this traps (EXC_BREAKPOINT, exit 133). Without hardening it's an out-of-bounds read.

**Reachability.** Any VirtualFile without a folder part: in-memory files, or a bare relative
file name. The UI normally passes absolute paths, so this mostly affects tools and tests.
**Judgment call:** fixed anyway, since the guard is trivial and the function is shared by every
core.

**Fix.** `CombinePath` returns `filename` when `folder` is empty. The SPC harness keeps loading
its cartridge from memory, so it doubles as the regression test.

## 6–10. 65816 vs SingleStepTests `65816`

**Method.** `tools/snes-cpu-singlestep/singlestep.cpp` compiles the **real**
`Core/SNES/SnesCpu.cpp` (with `SnesCpu.Instructions.h` and `SnesCpu.Shared.h`, which the SA-1's
`Sa1Cpu` also includes) into the harness as `TestSnesCpu`. `SnesMemoryManager` is replaced by a
`TestBus` (flat 16 MiB RAM), `SnesDmaController` by a `TestDma` with no transfers, and
`SnesConsole` by a stub for the constructor. This is the same technique as the Z80 harness.

- **Bus trace:** each cycle with VDA, VPA or VPB active, or with RWB low (see fix 7), must match
  the next `TestBus` read/write (address, value, direction). Internal cycles must not access the
  bus. Null read values in the suite are wildcards.
- **Timing:** the `SnesCpuState::CycleCount` delta (one per `Read`/`Write`/`Idle`) must equal the
  number of cycle entries. Memory speed (6/8/12 master clocks) isn't checked; the suite has no
  notion of it.
- **NMI/IRQ polling:** `DetectNmiSignalEdge()` must run exactly once per cycle. The harness
  starts `NmiFlagCounter` at 200 and checks that it went down by the cycle count; the counter
  is how the CPU times NMIs. This was added for the SA-1 (finding 13) and also passes on the
  main CPU. It's skipped for MVN/MVP.
- **Final state:** A (16-bit), X, Y, S, D, DBR, PBR, P, E, and RAM.
- **MVN/MVP:** the suite runs the whole move, so the harness calls `Exec()` until PC leaves the
  instruction.

**Notes on the suite.**
- It's generated by an emulator (Thomas Harte's Clock Signal), not captured from hardware.
- Its emulation-mode tests start with a random S high byte, which the CPU can't hold (S is
  `$01xx` in emulation mode). The harness forces it to `$01`; the expected values all assume that.

**Result.** 5,020,136 of 5,080,400 pass, with 0 unexpected failures. The 60,264 known
divergences are findings 8–10.

### 7. Emulation-mode RMW counted its dummy-write cycle twice (fixed)

**Reproduction.** Before the fix, all 28 emulation-mode RMW files (`ASL LSR ROL ROR INC DEC` on
`dp`, `dp,X`, `abs` and `abs,X`, and `TSB TRB` on `dp` and `abs`) failed 10000/10000 on cycle
count, e.g. (an earlier version of this log said 16 files; it is 28):

```
06 e 1: cycles=7(exp 6)
  actual bus:   RD6C50E=06 RD6C50F=0C R0021CB=74 W0021CB=74 W0021CB=E8
  expected bus: RD6C50E=06 RD6C50F=0C -- R0021CB=74 -- W0021CB=E8
```

The bus accesses were right; the count was one too high. `SnesCpu::IdleOrDummyWrite` called
`ProcessCpuCycle()` and then, in emulation mode, `Write()`, which calls `ProcessCpuCycle()` again.

**Effect.** The master clock was right (only `Write` advances it), but everything driven by
`ProcessCpuCycle` ran twice for that cycle: `CycleCount`, the NMI edge detector's
`NmiFlagCounter` and the pending-DMA check. `AluMulDiv` uses `SnesCpu::GetCycleCount()` to time
the `$4202-$4206` multiply/divide results, so an emulation-mode RMW right after starting a
multiply or divide made the result appear one cycle early. Few games run in emulation mode
beyond the reset code, so the effect is small.

**Fix.** `IdleOrDummyWrite` only calls `SetCpuSpeed(6)`/`ProcessCpuCycle()` on the idle path; in
emulation mode `Write()` handles the cycle. This removes work rather than adding it.

**The dummy write itself (judgment call).** The suite drives RWB low with the unmodified value
on the modify cycle but leaves VDA/VPA low, which this harness (the "environment" in the suite's
README) treats as no memory access. Upstream (`ef64e869`, "Perform dummy writes when needed in
emulation mode") makes it a real write, as the 6502 does. The harness counts an RWB-low cycle as
a write, so it checks that MesenCE drives the same value. Whether the 5A22 turns that cycle into
a /WR strobe isn't something the suite can answer, and the fork keeps upstream's behaviour.

**The SA-1.** `Sa1Cpu::IdleOrDummyWrite` has its own version, with a related bug. See finding 13.

### 8. `(dp,X)` emulation-mode page wrap (divergence)

When E = 1 and DL ≠ 0, and the pointer's low byte is at the end of a page (`D + dp + X` =
`$xxFF`), MesenCE fetches the high byte from the start of the *same* page
(`SnesCpu::GetDirectAddressIndirectWordWithPageWrap`, added upstream in `c0ea1245` as "Fixed CPU
6502 emulation mode edge case"; its comment says "cpu bug?"). The suite reads the next page.

**Second source.** ares (`readDirectX` in `component/processor/wdc65816/memory.cpp`) does the
same as MesenCE: "The (direct,X) addressing mode has a bug in which the high byte is wrapped
within the page if E = 1 and D&0xFF != 0." That makes the suite the odd one out. Neither side
cites a hardware test, though, so this is labelled a divergence rather than a suite bug.
**Not changed.**

264 of the 80,000 emulation-mode `(dp,X)` tests hit this; all of the others pass. The harness
excludes exactly these cases (`IsDirIdxIndXWrapCase`), not the whole files.

### 9. `WDM` (divergence)

MesenCE reads the second byte of `WDM` as an operand fetch (VPA). The suite has an internal
cycle there. ares does the same as MesenCE (`instructionPrefix`: `fetch()`), so this is **not
changed**. The only effect is timing
when the next byte is in slow memory, and games don't use `WDM`.

### 10. `WAI`/`STP` (divergence, bookkeeping)

The suite gives both instructions 3 cycles (opcode plus 2 internal) and then a stopped bus.
MesenCE executes the opcode fetch, sets the stop state, and spends its internal cycles in
`ProcessHaltedState`. Nothing observable depends on where those cycles are counted, apart from
the wake-up latency, which an interrupt scenario test would have to cover (see below).

## 11–15. SA-1 (`Sa1Cpu`) vs SingleStepTests `65816`

**Method.** The same harness, run with `--cpu sa1` (`mise run test:sa1-cpu`). It compiles
`Core/SNES/Coprocessors/SA1/Sa1Cpu.cpp` as `TestSa1Cpu`, with `Sa1` replaced by a `TestSa1` over
the same flat `TestBus`. `Sa1Cpu.h` uses `#pragma once`, so it's included for the first time
under the renames, and `CpuType::Sa1` (which the `Sa1` rename touches) is mapped back to the
real value. Every access reports SA-1 IRAM with no SNES-CPU conflict, so the SA-1's own wait
states (BW-RAM, bus conflicts, ROM jumps/odd branches) don't apply. They aren't covered here.
`TestSa1::ReadVector` mirrors `Sa1::ReadVector`.

**Result.** 5,020,136 of 5,080,400 pass, with 0 unexpected failures. The known divergences are
the same as the main CPU's (findings 8–10), since the instruction code is shared. Before
fixes 12 and 13, 32 files failed.

### 12. SA-1 vector fetches took no time (fixed)

**Reproduction.** `BRK` and `COP` failed 10000/10000 in both modes, with a correct bus trace:

```
00 n 1: cycles=6(exp 8) nmiPolls=6(exp 8)
02 e 1: cycles=5(exp 7) nmiPolls=5(exp 7)
```

`Sa1Cpu::ReadVector` called `Sa1::ReadVector`. That returns the NMI, IRQ and reset vectors from
the SA-1's registers and reads the others with `ReadSa1()`, without `Sa1Cpu::Read()`'s
`ProcessCpuCycle()`. So the two vector-fetch cycles didn't count.

**Source.** The suite, and the WDC W65C816S datasheet, where `BRK`/`COP` take 7 cycles
(emulation) or 8 (native), the last two being the vector fetch. `SnesCpu` already does this
(`ReadVector` → `ReadDataWord`).

**Fix.** `Sa1Cpu::ReadVector` reads the `BRK`/`COP` vectors with `Read()`, so each read gets its
cycle (and SA-1 wait states) in order. For the NMI, IRQ and reset vectors it runs
`ProcessCpuCycle()` twice and then takes the value from the SA-1 registers as before.

**Judgment call.** The suite doesn't test interrupt entry, so the NMI/IRQ part rests on the
datasheet's interrupt sequence (which ends with the same two vector-fetch cycles) and on
consistency with `SnesCpu`. The SA-1 substitutes the vector data, but nothing suggests it skips
the cycles. Effect: each SA-1 NMI/IRQ now takes 2 SA-1 cycles longer.

### 13. SA-1 emulation-mode RMW: NMI edge detection twice (fixed)

**Reproduction.** The 28 emulation-mode RMW files failed the NMI-polling check, with the right
cycle count (`06 e 1: nmiPolls=7(exp 6)`). `Sa1Cpu::IdleOrDummyWrite` ended with
`DetectNmiSignalEdge()` on both paths, but in emulation mode `Write()` already runs it through
`ProcessCpuCycle()`. That's the same mistake as finding 7, in a different place: here the cycle
count was right and only the NMI/IRQ poll ran twice, so an NMI pending on the SA-1 could be
taken one cycle early.

**Fix.** `DetectNmiSignalEdge()` is called on the native-mode (idle) path only.

### 14. SA-1 native-mode RMW reported to the wrong debugger (fixed, code reading)

On the native-mode path, `Sa1Cpu::IdleOrDummyWrite` called `_emu->ProcessIdleCycle<CpuType::Snes>()`.
`Sa1Cpu::Idle()` and every other SA-1 cycle use `CpuType::Sa1`. With the debugger attached, that
cycle advanced the **SNES CPU's** step-by-cycle counter and set its last memory operation, and
the SA-1 debugger missed it. There is no effect without a debugger. **Judgment call:** found by
code reading while fixing finding 13 (it's the same line), not reproduced in the UI. It's now
`CpuType::Sa1`.

### 15. Coverage gap: no SA-1 tests in MesenTests

None of the 24 SNES test groups in MesenTests uses the SA-1. Fixes 12–14 are checked at
instruction level, and `test:ci` shows they don't affect anything else, but no SA-1 game or test
ROM has been run. The SA-1's wait-state model (BW-RAM, conflicts with the SNES CPU, ROM timing)
is also untested.

**Update: partly closed.** `tests/SNES/sa1_timing.mtp` (source `tests/SNES/src/sa1_timing.s`, run
by `test:ci`) is a self-checking ROM written for the fork.

- **How it times.** The SNES CPU is the clock. It runs its timing loop from WRAM, so there are no
  SNES/SA-1 bus conflicts. It syncs to line 0 with a V-IRQ and WAI, and starts the SA-1 through
  the CCNT/CFR message registers. The SA-1 runs 1,364 unrolled copies from ROM and answers
  through SCNT/SFR. The SNES CPU then latches the V counter, giving SA-1 cycles = lines / 2,
  exactly.
- **What it covers:** NOP 2, `LDA abs` from ROM 4 and from I-RAM 4, `LDA long` from BW-RAM 6
  (one extra cycle, since BW-RAM runs at half speed), and BRK+RTI and COP+RTI 16.
- **Where the expected values come from.** BRK/COP 8 + RTI 7 come from the WDC datasheet. The
  extra 1 is MesenCE's existing penalty for returning into PRG ROM (upstream `fe470dd8`); that
  penalty is itself unconfirmed and should be checked on hardware.

**Results:**
- On the current core: "ALL PASS".
- On a core with `7aa572af` (fixes 12–14) reverted: BRK+RTI and COP+RTI measure 14, "2 FAILED".

**Still not covered:** NMI/IRQ entry (the judgment-call part of fix 12), conflicts with the
SNES CPU, the PRG-ROM jump and odd-branch penalties on their own, and fix 13, which isn't
observable as timing. Note that MesenCE doesn't implement the SA-1's own H/V counter reads
(`$2302-$2305`) or its timer, so SA-1 code can't time itself in MesenCE. **Not yet run on
hardware.**

## 16. Save-state round trip (clean)

**Method.** `tools/savestate-roundtrip` now handles `.sfc`/`.smc`, plus SMS, PCE and WS. For
each ROM:
1. Console A runs 60 frames and takes a save state (the same `Serializer` stream as
   `Emulator::Serialize`).
2. A runs 120 more frames.
3. A fresh console B loads the ROM and the state, then runs the same 120 frames.
4. The full serialized state of A and B is compared after every frame.

Anything that affects emulation but isn't serialized makes B diverge. `mise run
test:savestate-all` extracts the ROM embedded in every MesenTests `.mtp` and in the fork's
`tests/` (`tools/savestate-roundtrip/extract-mtp-roms.py`) and runs them all.

**Result.** 83/83 consistent. The set is 36 `.sfc` and 44 `.smc` test ROMs, plus the three SGB
test ROMs, which run as plain `.gb`, and it includes the fork's `sa1_timing` ROM, so SA-1 state
is covered. One `.swc` ROM isn't picked up.

**Is the check sensitive? (mutation test).** The tool can only see a missing field through its
effect on fields that are saved. So, in a throwaway worktree, `SV(_oddFrame)` was removed from
`SnesPpu::Serialize`. That flag picks the 4-clock-short scanline 240 on alternate frames. 59 of
the 83 ROMs then diverged on the first frame after loading the state, with exactly that
difference (`memoryManager.masterClock: expected 21748860, actual 21748864`). A first attempt
removed `SV(_frameCount)` instead, and nothing diverged. That's correct, not a blind spot:
`_frameCount` only decides which interlaced frames to skip rendering at high emulation speed,
and is reported to the UI. Neither affects emulated state.

**Limits.**
- No input is applied, so the ROMs are test ROMs that mostly settle on a result screen after the
  60-frame warmup.
- Coprocessors other than the SA-1 and Cx4 (`cx4test`) aren't exercised.
- The comparison is of the serialized state, so state that only affects video or audio output
  isn't covered.

## 17–18. 65816 interrupt scenarios

**Method.** `tools/snes-irq-scenarios` (`mise run test:snes-irq`) compiles the real `SnesCpu` and
`Sa1Cpu` against the flat test bus of the SingleStepTests harness. That setup now lives in
`tools/snes-cpu-singlestep/TestCpus.h`, shared by both tools. The harness raises interrupts
through the CPUs' own entry points (`SetIrqSource`, `SetNmiFlag`) and checks, on both CPUs, in
both native and emulation mode:
- IRQ and NMI entry: 8 cycles native, 7 emulation; PBR (native), PC of the next instruction
  and P pushed; I = 1, D = 0, PBR = 0; the vector read. On the SA-1, the NMI/IRQ vectors come
  from its registers.
- NMI isn't masked by I; IRQ is.
- WAI with I = 1: an IRQ resumes execution at the next instruction without the interrupt
  sequence. With I = 0 it's taken, with the address after WAI pushed.
- MVN is interruptible: the MVN's own address is pushed, and the move completes after RTI.
- BRK in emulation mode pushes B = 1.

Sources: the WDC W65C816S datasheet (interrupt sequence, WAI, MVN/MVP), and ares
`component/processor/wdc65816/instruction.cpp` `interrupt()` for the sequence and pushed P.

**Result.** 24/24 after fix 18. Before it, the 4 emulation-mode IRQ/NMI entry scenarios failed
(two per CPU). Since the harness compiles the CPU sources itself, it's also the regression test.

### 18. Emulation-mode IRQ/NMI pushed B = 1 (fixed)

**Reproduction.**

```
FAIL  SNES: IRQ entry, emulation: pushed P=$39 (expected $29)
FAIL  SNES: NMI entry with I = 1, emulation: pushed P=$34 (expected $24)
```

The same two failed on the SA-1. `ProcessInterrupt` pushed `PS | 0x20` for every interrupt.
In emulation mode MesenCE keeps bit 4 of P set, because that's the X flag, forced to 1. So
hardware interrupts pushed B = 1, the same as BRK.

**Source.** In emulation mode, IRQ and BRK share the `$FFFE` vector, and the pushed B bit is how
a handler tells them apart: 1 for BRK, 0 for IRQ/NMI (WDC datasheet). ares does the same:
`push(EF ? P & ~0x10 : P)` for hardware interrupts, `push(P)` for BRK/COP.

**Effect.** Any emulation-mode IRQ handler that checks B (the usual 6502 idiom on the
shared vector) treated every IRQ as a BRK. SNES games normally run in native mode, so this
mostly matters for boot code, homebrew and SA-1 programs that stay in emulation mode.

**Fix.** In emulation mode, `ProcessInterrupt` pushes P with bit 4 cleared when
`forHardwareInterrupt` is set. This is in `SnesCpu.Instructions.h`, so it applies to both
CPUs. It's on the interrupt path only.

## Notes for maintainers

- The makefile has no header dependency tracking. `Core/SNES/Debugger/DummySpc.cpp` `#include`s
  `Spc.cpp` and `Spc.Instructions.cpp`, so after changing those, `touch` it (or `make clean`).
  Otherwise the debugger keeps the old instruction code.

## Ideas for further SNES verification

- The save-state round trip with input applied (e.g. replaying each `.mtp` movie on both
  consoles), and on ROMs that use the other coprocessors (finding 16).
- A test for the SPC I/O page: timers (`$FA-$FF`), the `$F0` speed and write-enable bits, and
  `$F1` port clearing, against fullsnes / anomie's docs.
- 65816 interrupts not covered by finding 17: interrupt latency relative to the PPU
  (H/V-IRQ timing), the `IdleOrRead` dummy read before an interrupt (a MesenCE model of SNES
  behaviour with no source cited), and the DMA/HDMA interaction (`IrqLock`). These need the real
  `SnesConsole` rather than the flat test bus.
- SA-1 system level: extend `tests/SNES/sa1_timing.s` with SNES → SA-1 IRQ/NMI entry latency and
  SNES-CPU/SA-1 ROM conflicts, and run it on hardware (finding 15).
- The SA-1 H/V counter reads (`$2302-$2305`) and timer are unimplemented (`Sa1.cpp`).
- A hardware test ROM for findings 8 and 9.
