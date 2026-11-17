# Mega Drive / Genesis verification findings

Log for the Mega Drive work planned in
[`mega-drive-genesis-feasibility.md`](mega-drive-genesis-feasibility.md). Each entry gives the
reproduction, the source for the intended behaviour, the regression test (a `mise` task) and
the result. "Hardware-unverified", "reference disagreement" and "not assessed" are kept
distinct from "pass".

## M0 — headless runner and evidence baseline

### H1. Native headless runner (`mise run test:headless`)

`bin/mesen-headless` ([`docs/headless.md`](../headless.md)) links only the core, Lua and
SevenZip objects and runs NES, SNES, SMS (and every other console) without the UI, SDL or a
display. `Emulator::SetHeadlessMode` lets `Run()` execute on the caller's thread and return at
a frame boundary after N frames or a script stop; the console stays loaded for artifact
capture. Self-test: two runs of the same NES ROM and input produce identical SHA-256 for
every artifact; SNES and SMS outputs validate; a modified artifact, a timeout (exit 3,
`complete: false`) and a dirty build are rejected; nativekit's NES host contract works.

### H2. `emu.setInput` ignored its port argument (fixed)

- Reproduction: `tests/headless/setinput_port.lua` presses Start on port index 1. Before the
  fix the log read `port1.start=true port2.start=false` (run exit 65).
- Cause: `LuaApi::SetInput` padded the Lua stack to 4 slots after `ForceParamCount(3)`, so
  the top slot was nil; `ReadInteger` reads from the top, so the subport came from the nil and
  the port from the subport slot (default 0).
- Source for the intended behaviour: the documented signature `emu.setInput(input, port,
  subport)`; nativekit found the same bug with an actual-controller test and carries it as
  local patch `0002-lua-input-port.patch`.
- Fix: `lua_settop(lua, 3)`. Regression: `test:headless` ("emu.setInput addresses the
  requested port"). `test:ci` passes.

### H3. Lua errors in headless runs fail the run

A script error used to be logged and ignored, so a broken collector produced a normal-looking
capture. In headless mode `ScriptingContext::ProcessLuaError` now sets stop code 65 (as
nativekit's `0001-capture-shutdown.patch` does). UI behaviour is unchanged. Regression:
`tests/headless/lua_error.lua` in `test:headless`.

### H4. Synthetic Mega Drive corpus and BlastEm baseline

- `tests/MD/synthetic/gen.py` builds the 60 synthetic cartridges defined by nativekit's
  `cases.py` (copied with provenance) with its own 68000/Z80 encoder. Cross-check: all 60 ROMs
  are byte-identical to nativekit's vasm-built cartridges (`gen.py --check-vasm` plus a
  whole-ROM comparison against `mdtest.build_case`).
- `tests/MD/synthetic/record_blastem.py` ran nativekit's pinned BlastEm candidate (core
  SHA-256 recorded in `reference/baseline.json`) on every case: per-output SHA-256, frame sizes,
  68000 I/O read values in bus order, final frames (PNG) and chip PCM streams.
- Status: reference disagreement is expected on the six cases nativekit lists in
  `consoles/md/tests/rom/KNOWN`; BlastEm is a transitional differential oracle only.

### H5. Trace requirements

[`md-nativekit-trace-requirements.md`](md-nativekit-trace-requirements.md) lists the NMDT bus
trace and NMDH decomp-hook fields, with the MesenCE component that supplies each.

## M1 — 68000 core

### C1. ares M68000 port vs SingleStepTests/m68000 (`mise run test:m68k-cpu`)

- Source: ares `ares/component/processor/m68000` at `140c0a32` (ISC), ported with a nall
  compatibility header (`Core/MD/M68k/AresCompat.h`); every behavioural change is marked
  `//MesenCE:` and listed in `Core/MD/M68k/ares/README.md`.
- Reference: SingleStepTests/m68000 v1 (MAME microcoded 68000; 127 files, 317,500 tests).
  Compared: all registers, SR, PC (next prefetch address), IR/IRC, RAM, the bus trace (kind,
  address, UDS/LDS, strobed data lanes, per-step cycles, consecutive idles merged) and total
  cycles. Not compared: function codes on the bus (the console does not decode them; they are
  checked indirectly through the address-error status word).
- First run (unmodified port): 240,981 pass, 76,519 fail. Every failure class was traced to a
  documented or observable 68000 behaviour and fixed:
  - **Address errors** (odd word/long access, odd branch target) were not modelled at all. Now
    a group 0 exception with the 14-byte frame of MC68000 UM 6.3.9.1; the access is not
    performed (the suite logs it as an `re`/`we` cycle without AS). Frame details matched to
    the suite: write order, the 32-bit access address, the status word (IR bits 15-5, R/W,
    FC; I/N always clear), stacked IR = the prefetch-latched IR, side effects of `(An)+`/`-(An)`
    on the faulting access, and the stacked PC (instruction-, mode- and direction-dependent).
    Double fault halts the CPU until reset.
  - Exception frames (TRAP, CHK, DIV0, illegal, privilege, TRAPV) write PC low, SR, PC high,
    with two dead cycles between the last two prefetches.
  - `push<Long>` (BSR/JSR/PEA/LINK) writes the high word first.
  - Long read-modify-write writes store the low word first; ADDX/SUBX `-(An)` long reads fetch
    the low word first.
  - `MOVE <memory>,(xxx).l` writes before the last extension word leaves the queue.
  - ANDI/ORI/EORI to CCR/SR and MOVE to CCR/SR re-read the extension word; TRAPV prefetches
    before stacking; STOP takes its operand from IRC with no bus cycle; UNLK replaces A7 only
    after the frame-pointer read; PC-relative operands are read in program space.
- Result: **317,242 pass, 0 unexplained failures.** 258 tests are a known divergence:
  *MOVE.l whose write raises an address error* — MAME leaves N/Z/V/C in states that depend on
  which micro-step faulted and we found no consistent rule (register sources leave flags
  unchanged; memory sources mostly set N/Z from the low word). Those tests are compared with
  the flag bits of SR masked (all other state, the frame and the bus trace must match) and are
  counted separately. Impact on the console: none known — software does not resume after an
  address error.
- Suite caveats (README): TAS and TRAPV are flagged as suspect. TRAPV passes after matching the
  suite's cycle order (hardware-unverified). The suite logs TAS's read and write as plain
  cycles; the harness accepts the CPU's TAS-tagged cycles in their place. The Mega Drive's
  failure to complete TAS writes is a bus property and belongs to M3.
- Sanitizers: UBSan (`-fsanitize=undefined -fno-sanitize-recover`) found one porting bug:
  ares' `b1` (nall `Boolean`) default-initialises to false, but the compatibility header mapped
  it to plain `bool`, leaving `EffectiveAddress::valid` uninitialised in the instruction table
  (a stale "already computed" flag could skip an effective-address calculation). Fixed by
  initialising it; the full suite is UBSan-clean with the same counts. ASan: not assessed on
  this host — the ASan runtime hangs in `InitializeShadowMemory` before `main` on Darwin 27.

### C2. Reset, interrupts and STOP (`test:m68k-cpu`, `--scenarios`)

The suite tests single instructions only. Scenarios checked against the MC68000 User's Manual:
reset loads SSP/PC from vectors 0/1 and fills the queue; a level above the mask is taken via
autovector 24+level with PC/SR stacked and I set to the level; a level equal to the mask is
not taken; level 7 is taken at mask 7 on its edge only; STOP waits and an interrupt resumes
after the operand (stacked PC = STOP + 4). All pass. Interrupt-acknowledge timing (autovector
synchronisation with E) keeps ares' Mega Drive approximation (hardware-unverified).

### C3. Performance

`bin/m68k-cpu-singlestep --bench` (arm64, Apple silicon): 44.8 M instructions/s, about
80 times a 7.67 MHz 68000 on a flat bus. Dispatch uses ares' `std::function` table; a
tighter dispatch is deferred until the console's own cost profile is known. x86-64: not
assessed on this host.

## M2 — shared Z80 engine

### Z1. `Z80Engine<THost>` extracted from `SmsCpu` (no behaviour change)

- `Core/Shared/Processors/Z80/Z80Engine.{h,inl}` holds the complete instruction set, flag
  logic (F3/F5, WZ, Q via `FlagsChanged`), interrupt acceptance and HALT, unchanged apart from
  host calls. The host is a CRTP parameter: memory/port access, cycle advance, IM 2 data bus,
  the NMOS `LD A,I`/`LD A,R` P/V quirk, and the debugger/cheat hooks are inline calls on the
  host class, so the hot path has no virtual calls. `SmsCpu` is now the SMS host;
  `DummySmsCpu` (predictive debugger CPU) and the test-harness `TestSmsCpu` still build from the
  same sources by renaming, and each host explicitly instantiates the engine.
- `SmsCpuState` and its serialized field names are unchanged, so save states are compatible.
- Evidence (all after the extraction): `test:z80-cpu` 1,604,000/1,604,000 pass; `test:z80-irq`
  14/14; `test:savestate` on the SMS test ROM consistent; `test:ci` upstream and fork pass.
  6000 frames of `z80_timing.sms` through `mesen-headless` give byte-identical artifacts
  (audio, save state, screenshot, memory) on the pre- and post-extraction builds, in
  7.41 s vs 7.44 s (three runs each; within run-to-run noise).
