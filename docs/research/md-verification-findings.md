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
  initialising it; the full suite is UBSan-clean with the same counts. The whole console is
  UBSan-clean too: a `-fsanitize=undefined -fno-sanitize-recover=all` build of `mesen-headless`
  ran all 60 synthetic cartridges with `--trace bus --chip-audio` without a report.
- ASan does not start on the macOS development host (the runtime hangs in
  `InitializeShadowMemory` before `main` on Darwin 27, even for a four-line program, with both
  the Nix clang 21 and Apple's clang), so it runs on Linux in CI:
  `.github/workflows/md-sanitizers.yml` (GitHub Actions `ubuntu-24.04`, x86-64, clang 18,
  `-fsanitize=address,undefined -fno-sanitize-recover=all`).
  - 68000 suite under ASan + UBSan: 317,242 pass, 258 known, 0 fail, no reports. (The first run
    overflowed the stack in the harness: three ~2 MiB `TestCpu` objects, each holding ares'
    65,536-entry dispatch table, inlined into `main`'s frame; now heap-allocated.)
  - Whole console (`mesen-headless`, all 61 synthetic cartridges, `--trace bus --chip-audio`, 6
    frames each, including save-state and PNG writing): 0 failures, no reports (run 44138254070).
    One exemption, UBSan's alignment check for `Utilities/miniz.cpp` only
    (`.github/sanitizers/ubsan-ignorelist.txt`): vendored miniz deliberately loads unaligned
    words on x86 (`MINIZ_USE_UNALIGNED_LOADS_AND_STORES`). It was the only report, raised when
    writing the final PNG, and not Mega Drive code.
  - The CI runs also exposed two portability problems, now fixed: `MdDebugger` deleted its trace
    logger through `ITraceLogger`, which has no virtual destructor (undefined behaviour; a
    `-Werror` failure on Linux), and the Windows (MSVC `/W4 /WX`) build lacked the MD sources in
    `Core.vcxproj` and rejected shadowing names and the vendored ares code. The `Run tests`
    workflow (Windows, MesenTests) passes since `00d3f613`.

### C2. Reset, interrupts and STOP (`test:m68k-cpu`, `--scenarios`)

The suite tests single instructions only. Scenarios checked against the MC68000 User's Manual:
reset loads SSP/PC from vectors 0/1 and fills the queue; a level above the mask is taken via
autovector 24+level with PC/SR stacked and I set to the level; a level equal to the mask is
not taken; level 7 is taken at mask 7 on its edge only; STOP waits and an interrupt resumes
after the operand (stacked PC = STOP + 4). All pass. Interrupt-acknowledge timing (autovector
synchronisation with E) keeps ares' Mega Drive approximation (hardware-unverified).

### C3. Performance

`bin/m68k-cpu-singlestep --bench` on an Apple M1 Max, three runs each:

| Build | M instructions/s | × a 7.67 MHz 68000 |
|---|---|---|
| arm64 (native) | 43.1 / 45.3 / 45.4 | 77-81 |
| x86-64, native (GitHub Actions `ubuntu-24.04`, AMD EPYC 9V45, clang -O2) | 69.3 / 67.4 / 67.6 | 120-124 |
| x86-64, native (GitHub Actions `ubuntu-24.04`, AMD EPYC 7763, clang -O2) | 31.0 / 38.2 / 38.2 | 55-68 |
| x86-64 (`clang++ -arch x86_64`, run under Rosetta 2 on the M1 Max) | 24.7 / 21.4 / 26.0 | 38-46 |

The native x86-64 run is the `md-sanitizers` workflow (`.github/workflows/md-sanitizers.yml`);
GitHub assigns different CPUs per run, hence the two native rows; the Rosetta row only shows
translation overhead. The x86-64 build passes the full suite with the
same counts (317,242 pass, 258 known). Dispatch uses ares'
`std::function` table; a tighter dispatch is deferred until the console's own cost profile is
known.

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

### Z2. Mega Drive Z80 host (`Core/MD/MdZ80.{h,cpp}`)

- `MdZ80` is a second `Z80Engine` host with its own `CpuType::MdZ80`, `MemoryType::MdZ80Memory`,
  debugger (`MdDebugger` instance for the Z80) and trace/CDL hooks. Its bus is
  `MdMemoryManager::Z80Read/Z80Write`: 8 KiB Z80 RAM, YM2612, bank register, PSG, VDP ports
  and the 32 KiB 68000 bank window. A bank-window access adds 3 Z80 cycles of wait
  (judgment call: commonly cited figure, hardware-unverified); the 68000 is **not** stalled
  for the Z80's window accesses (approximation, listed under M3).
- Evidence: `z80_bank_window` and every `sound_*` case run Z80 programs (copied into Z80 RAM by
  the 68000, then released from reset) and match the BlastEm reads/audio (`test:md-synthetic`).

### Z3. `Sn76489` extracted from `SmsPsg` (no behaviour change)

- The tone/noise/volume core moved to `Core/Shared/Audio/Sn76489.h`; `SmsPsg` and `MdApu`
  both use it. `SmsPsg::Serialize` keeps its `_state.*` keys, so SMS save states are compatible.
- Evidence: 600 frames of `z80_timing.sms` on the pre-extraction build (ab55a742) and on the
  current build give identical `audio.wav`, `final.png` and memories. The two `final.mss` files
  differ by a few bytes inside the compressed payload (not decoded further); loading each of
  them into the current build and running 120 frames gives identical artifacts, so the old
  state loads and continues exactly.

## M3 — bus and scheduler

### B1. Bus map and scheduling (`Core/MD/MdMemoryManager.cpp`, `MdConsole.cpp`)

- Master clock 53,693,175 Hz (NTSC) / 53,203,424 Hz (PAL); 68000 = MCLK/7, Z80 = MCLK/15;
  3420 master clocks per line; 262 lines NTSC, 313 PAL (Sega Genesis Software Manual; note
  nativekit's cases document 312 for PAL, no synthetic case depends on it).
- The 68000 drives the schedule: after each instruction (and inside instructions on VDP
  stalls/DMA) the Z80, VDP and audio are caught up to the 68000's master clock. Every device
  exposes its state to `Serialize`, and the only save points are instruction boundaries.
- Implemented: header-based region and SRAM detection, SMD de-interleave, version register
  (model/region/expansion), I/O ports with TH-driven pad protocol, Z80 BUSREQ/RESET, bank
  register, Z80 RAM and YM/PSG access from the 68000, TMSS (`io_tmss_unlock`), `TAS` (write
  completes; `tas_write`).
- DRAM refresh (`MdMemoryManager::ApplyRefresh`): the 68000 loses 2 cycles every 128 68000
  cycles; the counter runs during VDP waits without adding delay and restarts after a
  68000-to-VDP DMA. Model and figures are BlastEm's (`genesis.c`, `refresh_advance`,
  `REFRESH_INTERVAL 128`, `REFRESH_DELAY 2`, in nativekit's pinned build 305189429e81); they are
  the transitional reference's, not hardware-measured. The delay loop feeds the stolen time back
  into the counter, so the result does not depend on how often it is applied. Effect on the
  synthetic corpus: all reads still match, and the total poll-count deviation from BlastEm
  (timing, reported by `compare.py --verbose`) drops from 370 to 38 iterations over the 7
  polls that differ.
- Approximations, not modelled (judgment calls, recorded here so they are not mistaken for
  verified behaviour): the 68000 stall while the Z80 uses the bank window;
  the VDP's position at power-on (MesenCE starts at line 0, H 0); VDP access through the bank
  window (returns $FF instead of locking up).
- Evidence: `bus_map`, `io_*`, `z80_bank_window`, `tas_write` match BlastEm's read values
  (`test:md-synthetic`).

### B2. Save/replay produces identical bus traces (`mise run test:md-replay`)

- `tests/MD/synthetic/replay_check.py`: run A = N frames from power-on with `--trace bus`;
  run B = W frames (leaves `final.mss`); run C = load B's state and run N−W frames. Requires
  trace(A) = trace(B) + trace(C) event for event (clock, device, op, address, value) and
  identical final picture and memories. Run with (W,N) = (1,4) and (2,5).
- `bin/savestate-roundtrip` (save → fresh console → per-frame state comparison) on all 60.
- The runs also use `--chip-audio`: A's YM2612 and PSG streams must equal B's followed by C's
  sample for sample (audio phase across the state), and cases with scripted pad input get it in
  all three runs (input frame numbers are absolute, so they continue across the load).
- Result: 61/61 replay identical for both splits (60 nativekit cases plus `io_pad6_across_frame`,
  R2); round trip consistent.

## M4 — VDP and video

### V1. Synthetic VDP/render cases vs BlastEm (`mise run test:md-synthetic`)

- `tests/MD/synthetic/compare.py` checks the 68000 read values of every READ step, that every
  POLL step terminates, and for render cases the final frame
  up to a colour relabelling (BlastEm and MesenCE use different DAC levels) after locating the
  active area inside BlastEm's bordered frame; interlace mode 2 is compared per field.
- Read values are compared the way nativekit's own runner accepts them
  (`consoles/md/tests/rom/run.py`, `_compare`): a READ that carries a `compare_mask` in
  `cases.py` must equal the case's expected value under that mask, and MesenCE and the reference
  are compared under the same mask; a READ without a mask is compared in full. Earlier versions
  of `compare.py` compared every read in full, which is stricter than the accepted method and
  produced five KNOWN entries for bits the cases do not assert (S3).
- Result: **60 pass, 0 KNOWN, 0 fail** of 60.

### V2. Decisions made while matching the reference

- **Status open bus.** VDP status bits 15-10 are not driven by the VDP. MesenCE returns the
  68000's prefetch register (IRC), as Genesis Plus GX does (`m68k_read_bus_16(pc)`); BlastEm
  returns the current opcode. Neither is hardware-verified here → KNOWN, bits 9-0 must match.
- **DMA fill** writes the fill byte to `vram[addr ^ 1]` after the address increment, and **DMA
  copy** reads/writes byte-swapped addresses (Charles MacDonald's VDP documentation; matches
  `dma_fill`/`dma_copy` readbacks).
- **Rendering granularity.** Each line is rendered from the registers/memories at the start of
  its active display. `render_midline_register` passes because the change lands between lines;
  mid-line raster effects are not modelled (approximation).
- **FIFO and DMA timing**: slot counts per line from the Sega manual (16/18 active,
  167/205 blank; VRAM words take two slots). `vdp_fifo`, `dma_active_vram_h32/h40` pass;
  poll iteration counts are reported by `--verbose` but not compared (they are timing, and
  BlastEm's slot model is not a hardware reference).

### S3. Differences from BlastEm outside the asserted bits, and the remaining audio KNOWN

Informational (not failures under the accepted comparison, V1):
- `sprite_collision_status`, `sprite_overflow_status_h32/h40`,
  `sprite_overflow_status_reread_h32`, `vdp_port_timing`: VDP status bits 15-10 (open bus,
  V2) differ; the cases assert only the collision/overflow/PAL bits. `vdp_port_timing`'s first
  status read also differs in the H-blank bit, which depends on the unmodelled power-on position.
- `sound_ym_ports`: the busy bit (7) after a data write differs (ymfm holds it 32×6 YM clocks =
  1344 MCLK, BlastEm about 900 MCLK longer); the case asserts only the timer flags (mask $03).

Still KNOWN (audio, `tests/MD/synthetic/KNOWN`): `sound_ym_envelope_key_on_off`, now classified as
a **BlastEm reference error**. The key-on pulse lasts 3136 master clocks (~58 µs, about 3 YM
samples) because the display is off and the VBlank poll ends at once. The channel's attack rate is
AR 28 with key scale 0, an effective rate of 58: fast, but **not** instant (instant needs 62), so the
pulse covers exactly one envelope step. (Earlier KNOWN texts said "well under one YM sample" and
"instant attack"; both were wrong.)

- Evidence: the YM2612 writes of MesenCE's bus trace replayed through Nuked-OPN2
  (github.com/nukeykt/Nuked-OPN2, commit 335747d78cb0, a die-shot based cycle-accurate YM3438/YM2612
  model, YM2612 mode, one sample per 24 internal cycles). Peak per 256 samples, median removed:
  MesenCE 231 ×8 blocks then silence; Nuked 264 ×2 blocks then silence; BlastEm 114 in the first
  block only (a start-up transient, rms 2.4). Both MesenCE and Nuked play a 260 Hz note (the
  channel's pitch); BlastEm plays none. nativekit's own model (`keyon_write`) also starts the attack
  on the write.
- Note length, investigated (A3): ~40 ms with ymfm, ~10 ms with Nuked-OPN2. Resolved by offering
  Nuked-OPN2 as a selectable FM core; with it MesenCE plays the ~10 ms note.
- Limits of this check: it compares activity and pitch only. On algorithms 0, 1 and 3 the replay
  harness reports dominant frequencies (16.7-24 kHz) that neither emulator shows, most likely an
  artifact of summing Nuked's time-multiplexed per-slot DAC outputs, so Nuked is not used as a
  general reference for the corpus. Activity and stereo placement agree with MesenCE on all 12 YM
  cases checked. The harness (`nuked_replay.c`) is not vendored or wired into a mise task.

## M5 — audio, controllers, debugger, determinism

### A1. YM2612 + PSG (`Core/MD/MdApu.cpp`, `tests/MD/synthetic/audio_compare.py`)

- `ymfm::ym2612` behind a `ymfm_interface` whose timers and busy flag run in master-clock time;
  status is readable on all four ports (BlastEm agrees). PSG = shared `Sn76489` (Z3).
- `--chip-audio` writes each chip's stream at the YM sample rate before mixing; the comparison
  requires the same activity, stereo placement and dominant frequency (1.5% or one FFT bin;
  broadband output, e.g. full-depth modulation in algorithms 2/3, compares spectral centroid
  and peak within 20%). Levels are not compared (different gain/DAC tables).
- Result: 17 audio cases, 16 agree + 1 KNOWN (`sound_ym_envelope_key_on_off`, S3).

### A3. Note length vs Nuked-OPN2, and the selectable Nuked-OPN2 FM core

- Method (investigation tools, not in the tree): drive ymfm (as `MdApu` does, writes between
  samples) and Nuked-OPN2 with the same writes and log each operator's envelope state and
  attenuation per sample. In `sound_ym_envelope_key_on_off` both take exactly one attack step
  before the key-off: ymfm 1023 → 511, Nuked 1023 → 767. Release is equally slow in both, so
  ymfm's note starts 6 dB louder and lasts ~4× longer. The attack step is
  `att += (~att * inc) >> 4`; at rate 58 the increment alternates between two values with the
  global envelope counter (Nuked: `eg_stephi[rate&3][timer_low] + 3`; ymfm: its increment table).
- Sweeping the key-on over 12 consecutive samples with the key held: both models produce the same
  two attack sequences (`256 384 96 …` and `512 128 192 …`), so the arithmetic agrees. They differ
  in (1) latency: ymfm reacts 0-2 samples after the key-on write, Nuked 3-5 (the die's register and
  envelope pipeline); (2) envelope-tick phase. A search over ymfm's initial envelope counter and a
  constant write delay matches at best 54 of 84 cases exactly, because Nuked latches key-on per
  operator slot on its own internal cycle. So this is the gap between a sample-level and a
  cycle-level model, not a fixable ymfm bug.
- Resolution (as Genesis Plus GX does): `MdConfig::FmCore` selects ymfm (default) or Nuked-OPN2
  (`Utilities/Audio/NukedOpn2`, LGPL-2.1+, commit 335747d7, unmodified). With Nuked, `MdApu`
  clocks the chip per internal cycle (42 master clocks), applies writes at their exact cycle, and
  uses the chip's own timers, busy flag and status reads (on a discrete YM2612, ports 1-3 return
  the last latched status). Output is the sum of the 24 slot outputs per sample × 7 (about ymfm's
  level; approximate). The save state records the core.
- Evidence: with `--md-fm-core nuked` the key-on case plays the ~10 ms note (peak 231 for 2 blocks
  of 256 samples, as the standalone replay). The whole corpus on the Nuked core: 60/60 reads, audio
  16 + 1 known, replay 62/62 (`test:md-synthetic` and `test:md-replay` run both cores). The default
  ymfm output is byte-identical to the previous build on all 61 cases (cross-build comparison).
- Cost: Nuked ~4.2% of one core at real-time speed vs 0.4% for ymfm (M1 Max), so it is an option,
  not the default (performance is a review criterion here).

### A2. 3- and 6-button pads (`Core/MD/Input/MdController.h`)

- TH-multiplexed protocol per the Sega 6-button pad documentation: the 6-button pad counts TH
  high→low transitions and returns X/Y/Z/MODE on the fourth cycle; the counter resets after
  80,000 master clocks (~1.5 ms) without a TH change (commonly cited figure,
  hardware-unverified).
- Evidence: `io_pad6_sequence` (scripted input, 6-button) and the pad reads in `io_*` match
  BlastEm; `--md-pads` selects 3/6-button per port in the runner.

### D1. Debugger, UI and Lua

- Core: `MdDebugger` instances for both CPUs (breakpoints, step, call stack, disassembly via
  `MdDisUtils`, trace logger, CDL, labels), `ExpressionEvaluator.Md.cpp`, memory types for
  68000/Z80 address spaces, ROM, work RAM, Z80 RAM, SRAM, VRAM, CRAM, VSRAM and TMSS ROM, event
  manager.
- UI: `MdConfig` (region, model, pads, layers, volumes) with `InteropMdConfig`; key presets for
  the 6-button pad; status views for the 68000 (`MdStatusView`) and the Z80 (the SMS view,
  parameterised by CPU type); "Z80 Debugger" menu entry; MD ROM extensions in the open dialog
  and folder scan. The C#/C++ layouts of `MdM68kState`, `MdVdpState`, `MdState` and `MdConfig`
  were checked field by field (`Marshal.SizeOf`/`OffsetOf` vs `sizeof`/`offsetof`: identical).
- Viewers (`Core/MD/Debugger/MdVdpTools.{h,cpp}` + C#): tile viewer (VRAM preset; Mega Drive
  cells use the existing packed-4bpp `WsBpp4Packed` format), tilemap viewer (Plane A, Plane B,
  Window; interlace-2 cells are 8×16), sprite viewer (80/64 sprites, 512×512 coordinate space,
  visible area at 128,128, sprites outside the link chain marked disabled), palette viewer (64
  CRAM colours, new `RawPaletteFormat.MdCram`, editable), register viewer (`MdRegisterViewer`:
  VDP state and all 24 registers decoded; Z80 bus, bank, SRAM, TMSS, I/O ports), event viewer
  (`MdEventViewerConfig`, VDP port reads/writes, IRQ/NMI, breakpoints; one column per 4 master
  clocks). Viewers refresh at the start of the selected line (the renderer works per line).
- Regression (`mise run test:md-viewers`, `tools/md-vdp-tools-check`): on all 20 render
  cartridges every rendered pixel is one of the palette viewer's colours; composing the Plane A
  and B tilemap views over the backdrop reproduces the rendered frame pixel for pixel on the 8
  single-scroll plane cases (57,344-76,800 pixels each); the sprite viewer's opaque screen
  pixels equal the rendered pixels on the 3 non-overflow sprite cases. (The two overflow cases
  are excluded on purpose: the viewer lists every linked sprite, while the frame drops the one
  past the per-line limit.)
- Not implemented: cheats (the M5 list does not include them; a Mega Drive Action Replay/Game
  Genie format needs 16-bit codes, while `InternalCheatCode` holds one byte); "Save ROM" from the
  debugger (`Debugger::SaveRomToDisk` has no MD case); OS file associations for `.md/.gen`.

### D2. Lua frame callbacks never fired on the Mega Drive (fixed)

- Repro: `Mesen --testRunner tests/headless/md_ui_smoke.lua <case>.md` timed out (exit −1)
  while the same pattern on an SMS ROM exited with the script's code: `MdVdp::EndFrame` never
  raised `EventType::EndFrame`/`StartFrame`, and never sent `PpuFrameDone` to the UI.
- Fix: both events and the notification are raised at the frame wrap (once per frame, no
  hot-path cost). Regression: `test:headless` "MD: Lua endFrame callbacks fire and the 68000
  state is readable".
- Related doc fix: the Lua stop function is `emu.stop(code)` (CLAUDE.md said `emu.exit`).

### D3. Debugger returned no VDP state for the Mega Drive (fixed)

- Repro: `tools/md-vdp-tools-check` saw an all-zero `MdVdpState` (display off, no shadow mode)
  on cartridges that render: `Debugger::GetPpuState`/`SetPpuState` had no `MdM68k`/`MdZ80`
  case, so every caller (status views' scanline, viewers, `DebugApi.GetPpuState`) got an
  untouched struct. Fix: both switches forward to `MdDebugger`. Regression: `test:md-viewers`
  (it fails without the fix, since the tilemap and sprite checks need the real state).

### R2. Pad handshake state was missing from save states (fixed)

- Repro: `io_pad6_across_frame` (`tests/MD/synthetic/extra_cases.py`, MesenCE's own case) runs
  `io_pad6_sequence`'s TH toggles and expected reads, but starts on NTSC line 254 and spaces the
  toggles with work-RAM reads (each gap far below the 1.5 ms phase timeout), so a save at the end
  of frame 1 falls after the first TH edge. `replay_check.py` (warmup 1): the restored run read
  $33 where the uninterrupted run read the 6-button ID $30. Cause: `MdControlManager::Serialize`
  stored only the base poll counter and never the pad devices (phase counter, TH level, time of
  the last TH change). The original `io_pad6_sequence` cannot show this: its handshake finishes
  in frame 1, before any save point.
- Fix: serialize the devices, as the SMS/NES/PCE control managers do. Regression:
  `test:md-replay` (61/61). Negative control: with the fix in place, removing only
  `SV(_phase)` from `MdController::Serialize` makes the case fail at the same read; restoring it
  passes.

### R3. Pads were recreated every frame once MdConfig gained padding (fixed)

- Repro: after adding the 1-byte `FmCore` field, `io_pad6_across_frame` read the 6-button
  handshake wrongly ($33 instead of $30), deterministically, in the new build only.
- Cause: `MdControlManager::UpdateControlDevices` compared a stack copy of `MdConfig` with the
  previous one by `memcmp` (`EmuSettings::IsEqual`), padding bytes included. The new field left 3
  indeterminate padding bytes, so the configs compared unequal at every frame end, the pads were
  recreated and the handshake reset. Fix: compare only the port fields (type, the four
  `KeyMapping`s — `uint16_t` members only, no padding — and turbo speed).
- Regression: `compare.py` now checks MesenCE's own cases against their expected read values
  (the replay check could not see this: both sides recreated the pads alike). Negative control:
  restoring the `memcmp` comparison fails the case at two reads; the fix passes.
- Not changed: other consoles use the same `IsEqual(copy)` pattern; their configs happen to have
  no trailing padding today. Recorded here as a latent risk.

### C4. Cheats: Game Genie and Pro Action Replay (`mise run test:md-cheats`)

- `CheatType::MdGameGenie` (ABCD-EFGH) and `MdProActionReplay` (AAAAAA:VVVV), the formats Genesis
  Plus GX and Kega Fusion accept. Each code patches a 16-bit word and is split into two byte codes.
  ROM addresses become patches applied on every cartridge bus read (68000, VDP DMA from ROM, Z80
  bank window), as the hardware devices sit on the cartridge bus; debugger peeks show the
  unpatched ROM. Work RAM codes are rewritten each frame. I/O and VDP addresses are rejected.
- Game Genie bit layout from two descriptions that agree bit for bit (Genesis Plus GX's
  `decode_cheat` and Charles MacDonald's Genesis Game Genie format notes). The test encodes its
  codes from the documented layout string, independently of the C++ decoder.
- Test (bus_map cartridge, which reads $004000): control read $0123; Action Replay 004000:BEEF
  and Game Genie 93AA-AZTA ($004000 = $CAFE) change the bus read; FF0100:1234 lands in work RAM;
  three invalid Game Genie codes and an I/O-area code are rejected. 8/8.
- Found along the way: a Lua error while a script *loads* did not fail a headless run (the runner
  entered headless mode after loading). Fixed in both runner paths; self-test added.

### B3. Sega bank-switching mapper ("SSF2 mapper")

- Super Street Fighter II's board and many large homebrew ROMs: eight 512 KiB windows; the odd
  registers $A130F3…$A130FF select the ROM bank in windows 1-7, window 0 is fixed to bank 0.
  Enabled for ROMs over 4 MiB or the header system name "SEGA SSF". Banks are serialized; the
  debugger's ROM address translation follows them.
- Test: `mapper_ssf2_banks` (MesenCE case, 5 MiB ROM): window 7 reads bank 7, then bank 9 after a
  byte write to $A130FF, then bank 8 after a word write to $A130FE; window 0 still reads the header.
  Passes, and replays identically across a save state.

### B4. Serial EEPROM saves (`Core/MD/MdEeprom.{h,cpp}`)

- Before: games that save to an I2C EEPROM (NBA Jam, NHLPA '93, Wonder Boy in Monster World, Mega
  Man: The Wily Wars, Micro Machines 2, ...) could not save; reads of the EEPROM returned ROM
  padding ($FF), so the games saw no EEPROM.
- Sources, by kind:
  - chip protocol: the Xicor X24C01 and Atmel/ST 24C01-24C65 datasheets (START/STOP, ACK on the
    9th clock, sequential reads, page writes wrapping inside the page; page sizes 4, 8, 16 or 32
    bytes);
  - board wiring (address and data bit of SDA in, SDA out, SCL): MAME
    `src/devices/bus/megadrive/eeprom.cpp` and `jcart.cpp` (BSD-3-Clause), whose table credits
    Eke-Eke's notes; Codemasters' wiring is marked as not fully confirmed there;
  - which games use which chip and board: ares' database (`mia/medium/mega-drive.cpp`, ISC),
    35 ROM hashes. The header does not describe EEPROM saves (a SpritesMind thread on Rockman Mega
    World: SRAM and EEPROM releases share a product ID), so there is no header detection. Genesis
    Plus GX's table was not used (non-commercial license).
- Judgment calls: NBA Jam's chip is a 24C02 with the standard device + address protocol (MAME) where
  ares names it "X24C02"; the EEPROM read address returns only the SDA bit, other bits 0 (as MAME);
  writes take effect immediately (no write-cycle busy time).
- Saves go to `<rom>.eep` through the battery manager, like `.srm`; the EEPROM is also in save
  states and shown as the save RAM memory type in the debugger.
- Tests: five MesenCE cases in `tests/MD/synthetic/extra_cases.py` (`mise run test:md-synthetic`),
  one per board (Sega and EA X24C01, Acclaim 16M 24C02, Acclaim 32M 24C04, Codemasters 24C65). Each
  bit-bangs I2C through the 68000: an erased cell reads $FF, page writes wrap (X24C01 4-byte and
  24C02 8-byte pages), the device block bits select 256-byte blocks (24C04), and 16-bit addresses
  work (24C65). The runner's `--md-eeprom CHIP,BOARD` test hook selects the board because the
  database is keyed by commercial ROM hashes. All pass; without the EEPROM the same cases fail
  (reads return $FF). The cases also replay identically across save states (`test:md-replay`).
- Not covered by automation: the hash lookup on real cartridges (no commercial ROMs in the repo),
  and the `.eep` file round trip (the headless runner never writes save files by design). SHA-256
  is shared with the runner (`Utilities/Sha256.h`) and matches `shasum -a 256`.

### V3. Mid-line rendering (`Core/MD/MdVdp.cpp`)

- Before: each line was drawn in one go when its active display started, so a change made during
  the line (an H-interrupt handler or a CPU loop writing CRAM, VSRAM, VRAM or a register while the
  beam is in the middle of the line) applied to the whole line, or to none of it.
- Now: the line is set up at the start of active display (horizontal scroll and the sprite line
  are latched there) and drawn in segments. Before a register write, a VDP data-port write (at the
  time its FIFO entry reaches an access slot), or a DMA, the pixels the beam has passed are drawn
  with the old state. Beam position: the active display starts at the existing render offset, at
  MCLK/10 per pixel in H32 and MCLK/8 in H40 (Sega Genesis Software Manual).
- Test: `vdp_midline_cram_h32/h40` (`tests/MD/midline/check_midline.py`, in
  `mise run test:md-synthetic`): every frame, the background colour is set to red in the middle of
  line 100 and back to black in the middle of line 200. Passes with one black-to-red and one
  red-to-black transition (H32 at x=156, H40 at x=193, rows 102 and 202). The build before this
  change fails the same check (no line contains two colours). The column itself is not asserted:
  there is no hardware or reference capture for it.
- Not modelled: the "CRAM dot" (a write during active display shows the written colour for one
  pixel); DMA still writes its whole transfer at once; a change of H32/H40 or of the interlace mode
  in the middle of a line. The 60 BlastEm-referenced cases are unchanged (60/60), and 3000 frames
  of `render_h40` take the same time (4.2-4.4 s) before and after.

### A4. Accessories: Team Player, Sega Mouse, Menacer, Justifier (`Core/MD/Input/`)

- Before: only 3- and 6-button pads; the control ports passed only TH to the device.
- The ports now pass every pin the console drives (data and control registers, `IMdPortDevice`), and
  the devices answer with their own pin levels. Sources, by device:
  - Team Player (Sega Tap), "Multi" mode, four 3/6-button pads on one port: MAME
    `src/devices/bus/sms_ctrl/teamplayer.cpp` (BSD-3-Clause; MAME says it is derived from what
    software expects). Pads are sampled when TH falls; each TR edge gives the next nibble with TL
    following TR: two zero nibbles, the four port IDs, then RLDU / SACB (/ MXYZ) per pad.
  - Sega Mouse: MAME `sms_ctrl/mouse.cpp` (BSD-3) and ares `md/controller/mega-mouse` (ISC). ID
    nibble $B, then per TR edge: two $F nibbles (movement latched on the first), sign/overflow
    flags, buttons, X and Y as high/low nibbles; data changes 7.2 us after the edge and TL follows
    TR 12 us after it (games poll TL).
  - Menacer and Justifier: Eke-Eke, "Sega Genesis Lightguns" (2008; notes from disassembling the
    gun games, untested on hardware according to the author). The gun pulls TH low when it sees the
    beam; with bit 7 of the port's control register set, that is the VDP's HL input: a level 2
    interrupt (register 11 bit 3) and an H/V counter latch (register 0 bit 1). Button bits and the
    Justifier's ID probe answers are as listed there.
- Judgment calls: the gun sees the beam at the mouse position whenever it is on screen (no
  brightness test); TH then stays low for one line. Latency from the beam to TH: Menacer 1344
  master clocks, chosen so that pixel 0 latches H counter $52 in H40, the Menacer 6-in-1 offset
  (Body Count uses $48, T2 $84 without latching, so those games may be a few pixels off until
  calibrated); Justifier 0 (Lethal Enforcers' default; Lethal Enforcers II adds $18 and offers
  calibration). Only the blue Justifier is connected. The mouse's counts are clamped to 9 bits, so
  its overflow flags stay clear. A mouse behind a Team Player is not supported (nor in MAME). The
  EA 4-Way Play is not implemented: no source under a usable license was found.
- Tests (`mise run test:md-synthetic`, cases in `tests/MD/synthetic/extra_cases.py`, input from Lua
  scripts in `tests/MD/accessories/`): `io_teamplayer` walks the whole Team Player sequence with
  pads 6/3/none/6 and buttons held on A, B and D; `io_mouse` reads the ID and the eight nibbles
  after polling TL; `io_menacer_hl` and `io_justifier_hl` read the gun's pins (Justifier: both ID
  probe answers) and the H/V counter latched through HL on line 50 ($3283 in H40, $3232 in H32).
  All four fail with plain pads in their place, pass with the devices, and replay identically
  across save states (`test:md-replay`). The runner's `--md-pads` takes `t:ABCD`, `m`, `g`, `j`.
- UI: the Mega Drive input page offers the four devices, with Team Player port A-D selectors and
  key bindings for the mouse and gun buttons; the mouse is captured for the Sega Mouse and the
  cursor becomes a crosshair for the guns. The C# and C++ `MdConfig` layouts were compared after
  the change (9952 bytes; sub-ports at 2016 and 5984 on both sides). Not exercised with real
  hardware or commercial games.

### D5. Lua could not read or set Mega Drive pad input (fixed)

- `emu.getInput`/`emu.setInput` use a device's button-name list; the Mega Drive pad had none, so
  both silently did nothing on the Mega Drive. The pads, mouse and guns now list their buttons
  (`up`, `down`, `left`, `right`, `start`, `a`, `b`, `c`, `x`, `y`, `z`, `mode`; mouse and guns
  also `x`/`y` coordinates). Found while writing the accessory tests, which depend on it.

### R4. Headless runner: a Lua script's input was dropped right after `--load-state` (fixed)

- Repro: `io_menacer_hl` replayed from a state saved after frame 2 read the gun's buttons as
  released in frame 3 ($40 instead of $49); the uninterrupted run read them pressed.
- Cause: the runner refreshes input once before the first frame, but loaded the Lua script after
  that refresh, so the script's `inputPolled` callback did not take part and the refresh replaced
  the restored input with none. The emulator itself does not refresh input on a state load.
- Fix: load the script before the refresh. `test:md-replay` is 73/73 again, and the runner's
  self-test still passes.

### D4. Mega Drive settings page

- Before: no settings UI for the Mega Drive (region, model, pads, key bindings and volumes could not
  be changed). Now: a Mega Drive tab in the settings window (also in the Options menu and the
  per-system links) with region, model, RAM power-on state, FM core, FM/PSG volume, pad types, a
  pad key-binding view (A/B/C with turbo, X/Y/Z, Start, Mode), video overrides, sprite limit and
  layer toggles. Checked by compiling and by the C#/C++ layout comparison of `MdConfig` (2016
  bytes, every field offset equal); not exercised by an automated UI test.

### R1. Recorded-ROM tests (`tests/MD/recorded`, `mise run test:ci`)

- Ten synthetic cartridges recorded for 120 frames (`tests/manifest.txt`; `build:test-roms` now
  generates `bin/test-roms/md/*.md`): H40, H32/V30, interlace 2, shadow, sprite link order,
  mid-frame register change, DMA fill, 68000→VRAM DMA, Z80 bank window, 6-button pad.
- Several render cases draw a uniform active area (they test mode, size and colour); the
  BlastEm reference frames agree. Result: `test:ci` fork tests 12/12 pass.

### X1. Determinism across save/restore and host architectures (`mise run test:md-xarch`)

- Repro: the same 60 captures on arm64 and on an x86-64 build (Rosetta) differed in
  `audio.wav` (1-LSB differences) and `final.mss` while trace, chip audio, picture and memories
  were identical. Cause: arm64 clang contracts `a*b+c` into fused multiply-add by default and
  x86-64 baseline code cannot, so the host-side resampler rounded differently. Fix:
  `-ffp-contract=off` for all builds (commit 33345e02). The remaining `final.mss` difference is
  miniz's x86-specific deflate path (different but equivalent compressed bytes).
- `tests/MD/synthetic/xarch_compare.py` requires every artifact to be byte-identical and
  cross-loads the states (each build continues from the other's `final.mss`).
- Result: 61/61 identical, including cross-loaded continuations, re-run after the refresh
  model and the pad serialization fix (x86-64 runner built from the same tree under
  `clang++ -arch x86_64`, run via Rosetta 2). `test:ci` passes with the flag.
- Speed (not a hardware claim): `render_h40` (idle 68000 loop, display on, audio) runs 3000
  frames in 3.9 s on arm64 (~770 fps). Commercial-game performance is M6 work.

## Z80 BUSACK while the Z80 is held in reset (2026-11-19)

- **Bug:** `$A11100` bit 8 read 0 (bus granted) whenever the 68000 requested the bus, even with
  the Z80 held in reset.
- **Behaviour on other emulators:** BlastEm (nativekit's oracle, measured) and MAME
  (`megadriv.cpp`, `z80_is_reset`) read 1 in that case.
- **Found with:** The Terminator (Sega CD), which waits for the 1. Details in
  `mcd-verification-findings.md` S5.
- **Fix:** `MdMemoryManager::ReadControlRegister` and `DebugRead`.
- **Test:** `z80_busack_reset` (`extra_cases.py`, `test:md-synthetic`).
- **Judgment call:** reset re-asserted after a grant follows MAME (reads 1); BlastEm keeps the
  grant.

## C1: library-wide comparison against GPGX and PicoDrive (2026-11-20)

Plan: [`md-oracle-testing-strategy.md`](md-oracle-testing-strategy.md), phases P0-P2. Tooling:
`tools/md-corpus` (`mise run md-corpus -- inventory|run|compare|report`).

### What was built

- **Inventory** (`inventory.py`): hashes every image under the ROM folders (raw, `.zip`, `.7z`;
  discs by their first track, cut out of single-BIN dumps at the next track's INDEX 00 or 01) and
  matches it against the No-Intro (Mega Drive 2026-11-20, 32X 2026-10-10) and Redump (Mega-CD
  2026-05-28) DATs. One preferred version per title: USA, World, Europe, Japan; plain releases
  before compilations and re-releases; latest revision; no betas, prototypes, pirates or
  unlicensed dumps.
  - Local library: Mega Drive 1,813 images (1,757 verified, 1,130 preferred), 32X 71 (56, 40),
    Sega CD 31 discs (22 matched Redump, 30 preferred), Sega CD 32X 12 discs (12, 12).
- **`mesen-headless --state-log FILE [--state-blocks] [--dump-at F,...]`**
  (`docs/headless.md`): per frame, CRC32 of every RAM-like memory and of the frame buffer, the
  pad-polled flag (`BaseControlManager::WasInputRead`, a read-only getter), the 68000 and sub
  68000 PC; with `--state-blocks`, 256 hashes of 256-byte work RAM blocks per frame. Checked in
  `test:headless` (`tests/headless/state_log_check.py`: the dumped work RAM hashes to the logged
  CRC32 and blocks). No measurable cost: Sonic 2, 3,600 frames in 6.5 s with or without it.
- **Oracle side** (`tools/libretro-capture/capture.py`): the same log, blocks and dumps from the
  memory a libretro core exposes (work RAM; PicoDrive also VRAM), `--swap-words`, port 2 input.
- **Runner, comparison, report** (`runner.py`, `compare.py`, `report.py`): sessions run in
  MesenCE, GPGX (MD, Sega CD) and PicoDrive (all four systems); each session is compared as soon
  as its runs finish (health, per-frame work RAM agreement with offset tracking, snapshots in VDP
  colour steps, audio through `tools/audio-compare`); `bin/md-corpus/report.html` ranks them.

### Facts established on the way

- **Byte order:** GPGX and PicoDrive keep 68000 work RAM as host-order words (byte-swapped on
  little-endian hosts). After swapping, Sonic 2's work RAM at frame 600 equals MesenCE's in
  65,534 of 65,536 bytes (PicoDrive) and 65,496 (GPGX, which runs 2 frames behind there).
- **Whole-RAM CRCs are too coarse:** they stop matching after about 150 frames in Sonic 2 even
  though the games stay in step. The remaining differences are the stack (`$FFFD9E`) and tables
  caught mid-update: each emulator ends its frame at a different point of the game's code
  (MesenCE at the wrap to line 0). Hence the per-block hashes, the noisy-block mask and the
  offset tracking in `compare.py`. With them, the three emulators agree on 243-256 of 256
  blocks over Sonic 2's attract mode, with offsets drifting by up to 3 frames during loads.
- **Colour:** MesenCE's three ladders (`MdDefaultVideoFilter.h`) fall on one 15-step scale
  (normal k = step 2k, shadow k = k, highlight k = 7 + k); GPGX and PicoDrive output RGB565 on a
  linear ladder topped at 232. Comparing in steps took Sonic 2's MesenCE-vs-oracle snapshot
  difference from 0.04-0.06 (raw RGB) to 0.001-0.01, the same as GPGX vs PicoDrive.
- **`capture.py` fixes:** the input entry for frame `"1"` was applied on the second
  `retro_run` (oracle input one frame late; harmless for the attract-mode audio runs it was
  written for, wrong for input replays); option values were handed to the core as a temporary
  buffer (now kept alive).
- **GPGX in `capture.py`** (judgment call, not understood): setting `region_detect`, `bios`,
  `force_dtack` or `addr_error` at load leaves GPGX silent, and `overscan` hangs it. The
  defaults are what the comparison wants (BIOS off, overscan off), so GPGX runs with defaults
  and auto region. MesenCE and PicoDrive get the region from the DAT name. A region mismatch
  would show as an early state divergence; none was seen.
- **Sega CD backup RAM:** with blank backup RAM, Road Rash stops on "BACKUP RAM NOT
  INITIALIZED", while the oracles start from a formatted image. MesenCE's Sega CD sessions now
  start from the same image GPGX creates (zeros plus the BIOS's 64-byte format block;
  `runner.BRAM_FORMAT`, byte-identical to 17 of 22 GPGX files). That is a test-condition fix,
  not an emulator bug.

### First library pass: attract mode, 3,600 frames (35 min, 10 cores)

1,212 titles, 3,584 runs, one nonzero exit. Clean (no flag): Mega Drive 1,040/1,130, 32X 23/40,
Sega CD 11/30 (the other 19 have audio flags only), Sega CD 32X 0/12 (not emulated).

| Group | Titles | Status |
|---|---|---|
| Lock-on window mirrored | Sonic & Knuckles | **Fixed** (below) |
| MesenCE crashes or hangs, oracles fine | Fatal Rewind, The Killing Game Show, Sesame Street Counting Cafe | **Fixed** (C2: interrupt enable latency and acknowledge priority) |
| Unsupported cartridge hardware | Star Odyssey (SF-004 mapper), both Radica Arcade Legends units | **Fixed** (C3: the two boards) |
| MesenCE crashes, black or frozen, oracles fine | Madden NFL 98, Buck Rogers | **Fixed** (C2: header-less battery RAM) |
| | European Club Soccer | Open (late divergence) |
| | Marvel Land | Not a bug (C2: false black flag) |
| Save RAM | HardBall III ("First time initialization complete. Press RESET"), HardBall '95 ("SRAM failure") | **Fixed** (C2) |
| 32X black or single-oracle divergence | 36 Great Holes, Brutal, WWF WrestleMania Arcade, WWF Raw, Spider-Man Web of Fire | Open, candidates (PicoDrive is the only oracle) |
| 32X ECCO CinePak demo black | | Expected: Sega's boot ROM rejects it (32X findings, S5) |
| Attract demos drift to other scenes after 25-55 s (sports titles: FIFA 97/98, NHL 98, NBA, MLBPA, Cal Ripken, World Heroes, ...) | 15 | Timing; low priority until a strict session shows a functional difference |
| Phantasy Star Fukkokuban | | Unsupported: the Master System game on a Mega Drive cartridge (needs the console's Master System mode); MesenCE refuses to load it |
| Sega CD 32X | 12 discs | Unsupported (strategy 10.1) |

Library-wide mix levels (judgment calls on both sides, no hardware measurement), recorded as
known offsets in `compare.py` (`KNOWN_LEVEL_DB`) so per-title outliers still flag:
- **Sega CD PCM:** relative to FM, GPGX's PCM is a median 7.2 dB louder than MesenCE's
  (20 titles, 4.8-9.3 dB, one at -1.4). CD audio agrees (median +0.3 dB over 14 titles).
  MesenCE mixes PCM unscaled (S2 judgment call). Not changed.
- **PSG:** GPGX's PSG is a median 2.8 dB below MesenCE's relative to FM (217 titles, p10
  -6.5 dB, p90 +0.8 dB), consistent with the 1-2 dB S5 saw on the Sonic games.

### Sonic & Knuckles: lock-on window mirrored (fixed)

- **Symptom:** standalone Sonic & Knuckles showed its "NO WAY?" screen (the screen for an
  unsupported cartridge locked on top) where GPGX and PicoDrive show the title. md-corpus
  flagged it from frame 6 (state, both oracles) with a snapshot difference of 0.35-0.50.
- **Cause:** `MdMemoryManager` mirrors every ROM up to the next power of two across
  `$000000-$3FFFFF`. The bus trace shows S&K reading `$200100-$20010F`, its lock-on slot, and
  finding its own header there ("SEGA GENESIS").
- **Sources:**
  - GPGX `core/cart_hw/md_cart.c`: for a cartridge named "SONIC & KNUCKLES", "disable ROM
    mirroring at $200000-$3fffff (normally mapped to external cartridge)", with the buffer
    padded with `$FF`.
  - MAME `src/devices/bus/megadrive/sk.cpp` maps the slot to the attached cartridge when there
    is one. With the slot empty its handler appears to fall through to the device's own ROM
    (not verified: `MD_ADDR` and the ROM size aren't in that file). Read as behaviour only.
  - Real hardware boots S&K alone to its own title, which requires the slot not to read "SEGA".
- **Fix:** a 2 MiB ROM named "SONIC & KNUCKLES" at `$150` is padded to 4 MiB with `$FF` at load,
  so no read path changes (no hot-path cost). The lock-on dumps (2.5-4 MiB) are unchanged.
- **Judgment call:** `$FF` is GPGX's value. The empty slot's bus value isn't measured; S&K only
  needs it not to read "SEGA".
- **Tests:** `cart_sk_lock_on_empty` and the control `cart_2m_mirror` (`extra_cases.py`,
  `test:md-synthetic`). The old build reads `$5345` and fails the first; both pass now.
  md-corpus: S&K is now clean against both oracles (snapshot difference 0.004 vs GPGX).

## C2: interrupts enabled while pending; save RAM detection (2026-11-21)

Clusters from C1, worked one at a time: reproduction from the md-corpus session, then MesenCE's
bus trace (`--load-state` a few frames before the failure, `--trace bus`) against nativekit's
BlastEm trace (NMDT v2) of the same ROM, then sources, a synthetic test and a library re-run
(`md_corpus.py diff`: verdicts changed since the previous build).

### Fatal Rewind / The Killing Game Show, Sesame Street Counting Cafe: interrupt enable (fixed)

- **Symptom:** Fatal Rewind (and its Japanese release, The Killing Game Show) crashed at frame
  314: the 68000 ran to `$FFFFFFFB` (line-F exception through vector `$FFFFFFFF`, then a double
  fault). Sesame Street Counting Cafe stopped at frame 1031 in a loop waiting for its VBlank
  counter. GPGX, PicoDrive and BlastEm run both.
- **Fatal Rewind, step by step:**
  - The game leaves VINT disabled over several VBlanks (register 1 = `$44`), so a VBlank stays
    pending. Mid-display (line ~143) it runs, at `$22486-$224A8`: `move #$2000,sr`;
    `move.w #$8014,$C00004` (HINT on; the HINT counter has expired); `move.w #$8164,$C00004`
    (VINT on); `jmp $2B7AC`, which installs the VINT handler's pointer (`$FFE03E`) later.
  - MesenCE took HINT straight after the first write, cleared HINT on the acknowledge, then took
    the stale VINT after the second write. Its handler called through `$FFE03E` = 0.
  - BlastEm's trace: no interrupt after the first write; HINT after the second (return address
    `$224A8`); no VINT at all. HINT runs again after `rte` (return `$2B7AC`).
- **Sesame Street:** the game enables VINT with a VBlank pending, `move.w #$8174,$C00004` at
  `$202A`, and updates its copy of register 1 in the next instruction. Its VINT handler restores
  register 1 from that copy after each DMA. MesenCE took VINT one instruction early (BlastEm one
  instruction later), so the handler restored `$14` (VINT off) and the game waited forever.
  - In MesenCE the write was followed by a DRAM refresh stall here: 42 master clocks between the
    write and the instruction's end, not 28, which the first version of the fix missed.
- **Sources:**
  - GPGX `m68kcpu.c` `m68k_set_irq_delay`: "IRQ latency (Fatal Rewind, Sesame's Street Counting
    Cafe)", one instruction, none for MOVE.L. `vdp_ctrl.c` `vdp_68k_irq_ack`: "VINT has higher
    priority (Fatal Rewind)", clearing VINT pending when it is enabled.
  - BlastEm `vdp.c` `vdp_int_ack`: clears VINT pending if pending and enabled, else HINT;
    "Fatal Rewind depends on this due to some buggy code".
  - PicoDrive `videoport.c`: a write enabling a pending interrupt raises it delayed ("make it
    delayed").
  - All three set VINT and HINT pending whatever the enable bits, and keep them until
    acknowledged (MesenCE already did).
- **Fix** (`MdVdp`):
  - `AcknowledgeInterrupt`: a level 4 or 6 acknowledge clears the enabled pending interrupt with
    the highest priority (VINT, else HINT), not the level taken.
  - `WriteRegister` (registers 0 and 1): a write that raises the asserted level is seen by the
    68000 one instruction later when it falls within two bus cycles (56 master clocks) of the
    instruction's end. That covers a MOVE.W (28 clocks after its write, 42 with a refresh stall)
    and not the first word of a MOVE.L (56 or more, GPGX's exception).
  - `GetM68kInterruptLevel` takes the instruction's end clock. The hold costs one predictable
    branch per instruction (`[[unlikely]]`). It is saved in save states. Speed: Sonic 2, 3,600
    frames headless, 6.09-6.27 s before and 6.16-6.17 s after (four runs each; noise).
- **Judgment calls:**
  - The window is derived from bus-cycle counts, not measured on hardware.
  - On the "HINT then VINT" sequence the oracles disagree. BlastEm and PicoDrive take HINT and
    drop VINT; GPGX takes VINT (its delayed register 1 write raises the level before the 68000
    samples HINT). MesenCE follows BlastEm and PicoDrive. Under GPGX's order Fatal Rewind's code
    at `$224A0` would crash, and the game runs on hardware.
- **Test:** `tests/MD/vdp/irq_enable_pending.py` (`test:md-synthetic`), on two cartridges:
  - **A**, the game's sequence after a VBlank with VINT off: HINT handler once, VINT handler
    never.
  - **B**, VINT alone, swept over 40 refresh phases (0-39 NOPs before the write): the stale VINT
    is taken, after the next instruction.
  - Before the fix both fail. With the first (28-clock) window, B fails in 5 of 40 phases. GPGX
    and PicoDrive give B's expected values in all 40 phases; on A, PicoDrive matches and GPGX
    takes VINT (above).
- **Library:** all three titles clean against both oracles; no other verdict changed
  (`md_corpus.py diff` over 1,212 titles).

### HardBall '95: battery RAM above a padded ROM (fixed)

- **Symptom:** "Cartridge Error Message: SRAM failure".
- **Cause:** its header ("RA", `$F820`) describes odd-byte RAM at `$300001-$30FFFF`, above its
  3 MiB ROM. Battery RAM above the ROM is enabled at power-on (B-series findings), but
  `MdMemoryManager` compared the start with the ROM size after padding to 4 MiB, so the RAM stayed
  disabled.
- **Fix:** compare with the dump's own size.
- **Test:** `cart_sram_above_padded_rom` (`extra_cases.py`). The old build reads `$FF`.

### HardBall III: battery RAM without a header (fixed)

- **Symptom:** "First time initialization complete. Press RESET to continue." on every boot.
- **Cause:** the cartridge has battery RAM but no "RA" header.
- **Sources:**
  - PicoDrive `carthw.cfg` lists it under "bad headers" (`check_str = 0x150, "  HardBall III"`,
    `sram_range = 0x200000,0x20ffff`).
  - GPGX's `sram.c` maps 64 KiB at `$200000-$20FFFF` for any header-less ROM up to 2 MiB.
  - MAME's software list has no entry.
- **Fix:** a table of header-less battery RAM by overseas name (`MdMemoryManager::Init`), with
  this entry; both byte lanes.
- **Judgment call:** a per-title table, as PicoDrive, rather than GPGX's blanket default. The
  default would replace the ROM mirror at `$200000` for every small header-less cartridge; that
  is a separate change to measure on the library first (open).
- **Test:** `cart_sram_headerless_hardball3`. The old build reads `$FF`/`$0`.

### Header-less battery RAM: Buck Rogers, Madden NFL 98, and GPGX's default (fixed)

- **Buck Rogers - Countdown to Doomsday:** stopped reading its pads with a stray "000FE000 03" on
  screen. GPGX matches its product code (T-50286) and gives it 16 KiB on the odd bytes of
  `$200001-$203FFF`, as for PGA Tour Golf (T-50086) and Winter Challenge (ACLD007,
  `$200001-$200FFF`). PicoDrive has no entry: its header-less fallback maps `$200000-$203FFF`.
  These three are now in the table, matched within the product field (`$180-$18D`).
- **Madden NFL 98:** frozen from frame 24 in an endless table search at `$462E0`. The table comes
  from MOVEP reads of `$204575-$2046B5`: rosters in battery RAM, no header.
- **GPGX's default:** any header-less ROM up to 2 MiB gets 64 KiB at `$200000-$20FFFF`, both
  lanes, initialised to `$FF` (`sram.c`: "by default, enable backup RAM for ROM smaller than
  2MB"). PicoDrive also maps a header-less default at `$200000`. MesenCE now does the same, except
  for Sonic & Knuckles (its lock-on slot) and Pugsy (T-113016, excluded by GPGX). GPGX also
  excludes Sonic 2; the library showed no reason to.
- **Measured on the library** (the reason to take the default rather than more table entries):
  re-run of all 1,212 titles, `md_corpus.py diff`: no regression. Madden NFL 98, FIFA Soccer 97
  and NHL 98 (attract demos that diverged without their battery RAM) became clean.
- **Judgment call:** the default follows two emulators and the library, not a hardware rule.
  Cartridges without battery RAM read a mirror of their ROM at `$200000` on hardware. No game in
  the library depends on that mirror there (S&K, the one that did, is excluded).
- **Tests:** `cart_sram_headerless_product` (Buck Rogers' layout) and
  `cart_sram_headerless_default` (RAM at `$200000` and `$204575`, ROM mirror again at `$210100`).
  Both fail on the old build. `cart_2m_mirror` now checks the mirror at `$210100`.

### Remaining Mega Drive findings after C2

1,051 of 1,130 titles are clean against both oracles (attract mode, 3,600 frames). MesenCE-only
failures left:
- **Unsupported cartridge hardware:**
  - Star Odyssey (SF-004, below).
  - Both Radica Arcade Legends units: their 4 MiB images start at `$3F0300`, a menu that selects
    games through Radica's bank switching.
  - Phantasy Star Fukkokuban: the console's Master System mode.
- **European Club Soccer:** departs from both oracles in its last 10 seconds (snapshot difference
  0.52 at frame 3600). Open.
- **Marvel Land** was a false "black" flag: its last frame is nearly black in all three emulators
  (difference 0.003). `mesen-black` now also requires the last snapshots to differ (> 0.05).
- **The rest:** state divergences in sports attract demos (timing, ranked low) and audio flags.

### Star Odyssey: SF-004 mapper (open, unsupported hardware)

- **Symptom:** black from frame 1; the 68000 ends at `$30040000`.
- **Cause:** the boot code reads `$A13001/5/9` and builds addresses from the values. The
  cartridge is Super Fighter Team's SF-004 board: 8 x 256 KiB banks, mode, lock and save RAM
  registers written at `$00xD00/$00xE00/$00xF00`, and the selected bank's index (high nibble)
  read at `$A130xx`.
- **Sources:** GPGX `mapper_sf004_r/w` (selected by the header), PicoDrive `carthw.cfg`
  (`[Star Odyssey]`, `hw = sf004_mapper`) and `carthw.c`, MAME `md_rom_starodys_device`. They
  agree on the registers, the reset state and the read value. They differ on the ROM-off mode
  and the mode 1 layout.
- **Status:** a mapper to add (it touches the ROM read path, like the Sega mapper in B3), not an
  accuracy fix. Not done in C2; added in C3.

### Tooling added on the way

- **Build keys:** md-corpus keys runs by the mesen-headless build. A session last run by another
  build is run again, and its old result is kept in `results/<session>/history/`.
  `md_corpus.py diff` lists the titles whose verdict changed (regressed / fixed / changed) and
  exits 1 on a regression.
- **`frozen_from`:** when MesenCE freezes, the comparison records the frame the freeze began and
  ranks by it. The state tracker couldn't date it: frozen RAM still matched the oracles' slowly
  changing RAM at some offset, so Fatal Rewind (frozen at 314) was reported as diverging at 1727.

## C3: cartridge boards: SF-004 and Radica (2026-11-21)

### The boards

MesenCE's ROM mapping is now a table of 64 KiB pages for $000000-$3FFFFF
(`MdMemoryManager::_romPage`, used only when a board is detected; `MapRom` stays one branch). Sega's
mapper (B3) fills it from its eight 512 KiB windows. Two boards are added:

- **SF-004** (Super Fighter Team; Star Odyssey). Detected by the header's type field ("SF-004", as
  GPGX). Registers, reset state, bank read-back and battery RAM follow GPGX `md_cart.c`
  (`mapper_sf004_w/_r`) and PicoDrive `carthw.c` (`carthw_sf004_*`), which agree on them:
  - `$00xD00` bit 7: 32 KiB battery RAM on the odd bytes of `$200000-$20FFFF` (`$A130F1` is
    ignored on this board).
  - `$00xE00`: bit 7 clear locks the registers until power-on; bit 6 maps five 256 KiB banks in a
    row from the selected one; otherwise the selected bank is mirrored over `$000000-$1FFFFF`.
  - `$00xF00` bits 6-4: the bank. Reading `$A130xx` returns it in bits 6-4.
  - **Judgment calls**, where the oracles differ (GPGX followed, the main Mega Drive oracle):
    `$E00` bit 5 unmaps the ROM, and the "in a row" mode leaves `$140000-$1FFFFF` unmapped
    (PicoDrive: eight banks, bit 5 ignored). Unmapped pages read `$FF`. GPGX also mirrors the
    battery RAM over `$200000-$2FFFFF`; MesenCE maps it once. Z80 bank-window writes don't reach
    the registers (GPGX's do). Star Odyssey's sessions don't exercise any of these.
- **Radica** (Arcade Legends units). Detected by header checksum plus the sum of the words from
  `$200`, as GPGX's `rom_database` (Volumes 1-3 and both Street Fighter II editions). Any read of
  `$A130xx` selects index = A6-A1, and page *i* then shows ROM page (index | *i*) (GPGX
  `mapper_64k_radica_r`; PicoDrive maps the bank linearly, which is the same for every game on
  these units). Such a read returns `$FFFF` (GPGX; PicoDrive 0; the menu ignores it). Not added:
  the Sensible Soccer Plus unit (128 KiB banks and battery RAM), which isn't in the library.

### Results

- **Star Odyssey:** boots; attract and walker sessions clean against both oracles (was black from
  frame 1).
- **Arcade Legends Sega Mega Drive:** the menu runs, and the walker starts Sonic the Hedgehog; both
  sessions clean (was frozen on a black screen from frame 250).
- **Arcade Legends Street Fighter II:** the menu and the game run in all three emulators. MesenCE
  and PicoDrive agree; GPGX takes another path through the menus in the walker session (state
  divergence at frame 1257; GPGX vs PicoDrive 1396), the sports-demo timing class.
- **md-corpus region fix:** that unit's DAT name says "(World)" but its header allows Europe only
  ("E"). md-corpus ran MesenCE and PicoDrive in NTSC, where the game stops at its "PAL and French
  SECAM only" screen; GPGX picks the region from the header and ran the game. A "(World)" image
  whose header excludes NTSC now runs in a region the header allows (`inventory.cart_region`;
  changes this title and one non-primary G-LOC beta).
- **Test:** `tests/MD/cart/mapper_boards.py` (in `test:md-synthetic`): an SF-004 and a Radica
  cartridge with the program copied into every bank; 24 reads of markers, registers and battery
  RAM against the behaviour above. Both fail on the old build.

### Library re-run (C3 build)

All 1,200 sessions, `md_corpus.py diff`: regressed 0; fixed Star Odyssey and both Radica units.
Changed: Fahrenheit (Sega CD 32X, unsupported) lost its `mesen-black` flag; it stays flagged
unsupported. Clean against both oracles: Mega Drive 1,054/1,130, 32X 23/40, Sega CD 11/30.
Speed: `mesen-headless` on Sonic the Hedgehog, 6,000 frames, four runs each: 10.36-10.47 s
against 10.34-10.73 s for the C2 build (no measurable change).

### TAS import (P3, tooling)

`tools/md-corpus/tas.py`: `.bk2` (BizHawk) and `.gmv` (Gens) movies to md-corpus sessions,
matched to the corpus by the movie's ROM SHA-1; `run --session tas`. Movies starting from a save
state or save RAM, or resetting during the movie, are imported as unsupported. Movies are
downloaded by hand: TASVideos' robots.txt disallows automated movie downloads (strategy 0.1).
Checked by `tas_selftest.py` (both formats round-trip a random 500-frame input) and an end-to-end
`.bk2` made from the Arcade Legends walker input, 3- and 6-button: same verdict and snapshots as
the walker session. The `.gmv` layout is from the format notes, not yet checked on a real movie.


## C4: first published TAS movies (2026-11-21)

22 movies downloaded by hand (7 BizHawk `.bk2`, 15 Gens `.gmv`; Sonic 1-3 and lock-ons, Phantasy
Star II/IV, The Lion King, Shinobi III, Prince of Persia). The first import matched none of them.

### Import fixes (tooling)

- **BizHawk's "SHA1" is an MD5.** Genplus-gx movies write the ROM's MD5 (32 hex digits) in the
  `SHA1` header line (all 7 here). The import now matches 32-digit hashes against the cartridges'
  MD5s (computed once, cached in `cache/md5.json` by SHA-1). All 7 matched.
- **`.gmv` names no ROM.** Each movie's ROM now comes from `movies/roms.txt` ("file = corpus
  title or SHA-1"), or `--sha1` as before. GoodGen names on the publication pages map to No-Intro
  ones by revision.
- **`.gmv` header, checked on these 15 files:** version letter at $0F (all "A"); the pad types at
  $14/$15 are per port (Phantasy Star IV: "36", a 3-button P1). The import had used 6-button for
  both if either was "6". Flags at $16 (bit 7 save state, bit 6 PAL, bit 5 three players; $1F/$5F
  here); a comment, not the game name, from $18. Pad types now go per port to all three emulators
  (`--md-pads P1,P2`, `capture.py --pads P1,P2`, PicoDrive `input1`/`input2`).
- **BizHawk's region is a number** in SyncSettings.json (`"Region": 0`, auto-detect; 1 Japan
  NTSC, 2 Japan PAL, 3 USA, 4 Europe), which the import had not read. When a movie leaves it to
  auto-detection, the region is now what GPGX and Gens pick from the header ($1F0: USA, then Japan,
  then Europe), not the corpus entry's. Sonic 1 Rev A ("(Japan, Europe, Korea)", header "JUE")
  runs as NTSC USA, not PAL. A `.gmv` without the PAL flag runs NTSC, USA or Japan by the header.
- `run --session tas --match` also matches the movie file name and id.

One movie (Phantasy Star IV "macroglitch") resets during play and stays unsupported.

### Results so far (three movies through all emulators)

- **Sonic 3 competition (Gens, 7,263 frames):** MesenCE and PicoDrive play the whole race (finish
  1'41"93 and 1'41"95; work RAM agreement 1.0). GPGX misses the one-frame Start press that skips
  the SEGA screen at frame 58, stays on the title and starts a 1-player game. A one-frame Start
  press is accepted at frames 58 and 62 in MesenCE and PicoDrive, at 60 and 64 in GPGX. With the
  movie's input two frames later, GPGX plays it exactly like MesenCE (finish 1'41"93).
  **Libretro GPGX acts on this game's pad reads two frames later.** GPGX samples the pads once per
  frame, just before VINT (`system.c`), and its first frame after power-on ends before the first
  VINT. MesenCE and PicoDrive read the pad when the game does.
- **Sonic 1 100% (BizHawk Genplus-gx, 60,053 frames):** all three stay in step for the whole
  movie, unshifted (agreement 1.0 / 0.996, no divergence), through Star Light Zone. So the GPGX
  lag depends on the game (where it reads the pads). A fixed per-emulator input shift would
  desync this one, so the shift is chosen per movie (below).
- **Phantasy Star II "game end glitch" (BizHawk, 10,252 frames, Japan):** all three reach the
  ending and return to the title.
- **Sonic 1 (Gens, aglar):** desyncs around frame 7,500 identically in all three emulators, on
  both revisions (Rev A gets further: Marble Zone with 3 lives at frame 6,000). An old Gens movie
  that depended on Gens itself, not a MesenCE finding. It still serves as a three-way
  comparison input.

No MesenCE divergence in these three movies.

### Per-movie GPGX input offset (`runner.calibrate_tas`)

Before a `tas` session, the first 5,400 frames (90 s) run in PicoDrive as recorded and in GPGX
with the input shifted by 0, ±1, ±2 and ±3 frames. The shift whose work RAM follows PicoDrive's
longest wins (latest `diverged_at`, then most lockstep frames; on a tie the order 0, +1, -1, +2, ...).
GPGX's session run then uses that shifted input; MesenCE and PicoDrive use the movie as recorded.
MesenCE never takes part in the choice, so its comparison against both oracles stays independent.
The choice, every shift's score and the tied shifts are cached in `tas/<id>/calibration.json`.
A session whose input files change is run again whole. Judgment calls: PicoDrive as the timing
reference (it agreed with MesenCE on both movies checked by hand) and the 90 s window (several
shifts tie when the opening presses don't matter, then 0 wins).

Checked against the hand results: Sonic 3 competition +2 (0, ±1, ±3 diverge at frame 369, -1/-2
at frame 30), Sonic 1 100% and Phantasy Star II 0 (all shifts tie).

### First full batch (21 movies) and the calibration rule

All 21 movies ran in all three emulators (2.6M frames, 32 min, no failed runs). Ten stay in step
in all three. Four were flagged with MesenCE alone diverging while the oracles agree: The Lion
King (both movies) and two Sonic 3 & Knuckles movies.

The first calibration rule broke ties on lockstep (frames whose work RAM matches exactly) and
picked noise: +3 for the Gens Sonic 1 movie, -3 for Gens Sonic 2, +3 for Shinobi III. In each,
GPGX then drifted from the other two. Revised (`CALIBRATE_VERSION` 2):
- tied shifts go on to a 36,000-frame stage;
- a tie that remains goes to the order 0, +1, -1, ..., with no lockstep;
- BizHawk movies are not calibrated. Their Genplus-gx core is the Genesis Plus GX code the gpgx
  oracle runs, so GPGX replays them with no shift.

### The Lion King: when the pads take each frame's input (not a MesenCE bug)

`shgofctas-thelionking-genesis-allbugs.bk2` (BizHawk Genplus-gx) went wrong at frame ~1,000 in
MesenCE only. The title menu's input is Down (990), Start (999), Left (1000), Start (1001).
MesenCE opened the options screen and stayed; both oracles started the game. No input shift
changed that.

Cause: after Start, the game's VBlank handler queues about 4 KB of VRAM DMA in 22 chunks with the
display on. The DMA rates are right: 203-211 bytes per line in VBlank and 18 in active display
(H40), measured with `--trace bus`. At those rates the queue runs from line 234 past the wrap,
to line ~62. The handler reads the pad after the queue, at line 71 of the next frame. MesenCE
applies frame N's input from the wrap to line 0, so that read got frame 1001's Start instead of
frame 1000's Left. The Left was the release between the two Starts, so the game never saw a new
press.

Genesis Plus GX updates its pads at the first VBlank line ("update inputs before VINT" in
`system.c`), and BizHawk records movies with that core. There, the late read still sees frame
1000's input. This is a convention, not hardware behaviour: a real console has no frames.

- **`mesen-headless --md-input-latch vblank`** (`MdConsole::ProcessVBlankStart`, test hook)
  applies each frame's input from its first VBlank line instead.
- The runner uses it for movies recorded in BizHawk's Genplus-gx core and keeps the frame
  boundary for Gens movies. Across all 21 movies it changed nothing except the two Lion King
  movies: `4564ff44ba50` now stays with PicoDrive to the end, where it used to split at frame
  2,670; `e6f6baab4775` stays in step to frame ~2,140, where it used to split at 1,000.
- Regression test: `io_input_latch_frame` / `io_input_latch_vblank` in `test:md-synthetic`
  (`tests/MD/accessories/input_latch.json`).

**Open (judgment call: not yet a bug):** in `e6f6baab4775` with the VBlank latch, MesenCE and
PicoDrive match byte for byte through frame 2,125. On frame 2,126, the game's object handler
(`$000F68`-`$000FDC`) clears `FF9160` in MesenCE but not in PicoDrive, and an animation
(`FF0027` = `$5E`) starts one frame earlier. Simba then lands differently after the jump at
2,134. Before that point, MesenCE's game state often equals PicoDrive's one frame later: the
game's logic (not locked to the frame rate) finishes earlier in the frame in MesenCE. That
suggests MesenCE's 68000 gets more done per frame than both oracles in this game. DMA rates,
the VDP FIFO stall and DRAM refresh are modeled and look right. An arbiter more accurate than
both oracles (BlastEm, or a console) is needed.

### Comparison weakness found on the way

The work RAM metric (`compare.state_pair`) reported The Lion King's divergence at frame 4,290,
but the screens differ from frame ~1,000. Before a level loads, most work RAM is static, and the
tracker accepts offsets up to ±60 frames, so a menu matches a cutscene well enough. Taking the
noisy-block mask from the oracle pair instead of from the pair under test made no difference,
so it was not kept. The snapshot comparison catches these cases; flags from the RAM metric need
the snapshots checked.

### Second batch: 59 movies (2026-11-21)

39 more movies imported (Contra Hard Corps, Comix Zone, Castlevania Bloodlines, Mega Turrican,
Ristar, Streets of Rage 1-3, Shining Force I/II, Phantasy Star III, Ys III, Strider, Aladdin,
Earthworm Jim 2, Ecco 1/2, Chakan, Flicky, Sonic 3D Blast, Kid Chameleon and others).
`roms.txt` maps the Gens movies and two BizHawk 1.11 movies, whose headers carry no ROM hash;
every mapping there is a guess from the file name. Not run: Earthworm Jim Special Edition (a
Sega CD movie) and Phantasy Star IV "macroglitch" (resets).

59 sessions, 6.9M frames, 177 runs in 91 min, no failed runs. All three emulators stay in step
for the whole movie in 41; MesenCE and PicoDrive in 50. Calibration v2 gave 0 for every Gens
movie except Sonic 3 competition (+2) and the Gens Sonic 1 movie (+3, a tie artefact; v3
counts divergences within 6 frames as a tie).

MesenCE alone diverging while the oracles agree (to investigate, not yet bugs):

| Movie | MesenCE diverges | Note |
|---|---|---|
| Mega Turrican (USA), Gens | ~21,288 (state); the snapshots from 20,700 show MesenCE on one unchanging screen while PicoDrive plays on | the strongest candidate: looks like a stall |
| Streets of Rage 2 (USA), Gens | ~84,050 | MesenCE's player loses a life the oracles don't |
| The Lion King `e6f6baab4775` | ~2,140 (snapshots) | the open item above |
| Sonic 3 & Knuckles `efa466ceedd5`, Gens | ~126,000 | GPGX drifts at 110,107, so weaker |
| Kid Chameleon, Gens | 108,091 | after the movie's last input (107,862) |

The other flags are not MesenCE-specific. In Strider (BizHawk 1.11) all three emulators diverge
from each other (23,500-25,000), and only MesenCE ends on a black frame (to check). In Shinobi
III, PicoDrive drifts from both. In Sonic 3D Blast (both movies), GPGX diverges from the other
two at frame ~700, although the movies were recorded in Genplus-gx: a libretro GPGX difference
from BizHawk's build, or an option. Several Gens movies end with GPGX drifting while MesenCE and
PicoDrive stay together.

### Mega Turrican: DMA on the last line of the frame (fixed), and a pause (open)

The Gens movie `mato-megaturrican.gmv` (`380a8f77d88f`) froze in MesenCE from frame ~1,200 to
20,700 while both oracles played the first level. The RAM metric did not see it until 21,288:
the game's work RAM barely changes while it is stopped. The snapshots did.

MesenCE was not hung. The main loop (`$8BC`: `SF $BC85` / `TST.B $BC85` / `BEQ`) waits for
the VBlank handler (vector `$528` → `(FFBC14)` = `$1D10`) to set the flag. It does, and the loop
then runs the game's pause state (`$19C6`): it buffers ten button presses for a cheat code
(`FFBC72`, count `FFBC6E`) and waits for Start. A one-frame Start press in the movie (912 or
953) landed in gameplay in MesenCE but in a transition in the oracles. MesenCE's game state is a
frame ahead from frame 585: it finishes decompressing into `FF638C` there, PicoDrive on 586.

Timing cartridges, built in Python and run in all three emulators (`tests/MD/dma/check_dma_timing.py`):

| Measurement | MesenCE before | GPGX | PicoDrive | MesenCE after |
|---|---|---|---|---|
| 4 KB 68000→VRAM DMA from line 253, H32, display on | resumes V $A3 | $AD | $AC | $AD |
| the same, H40 | $7E | $89 | $88 | $89 |
| the same in VBlank only, display off, or active display only | agree | | | agree |
| VRAM fill / copy DMA, in VBlank, across its end, active display | agree within a few H steps | | | agree |
| busy loop, 68000 alone (ROM or RAM, display on or off) | 6,994 / frame | 6,994 | 6,990 | |
| `MULU`/`MULS`/`DIVU`/`DIVS`/`LSL.L #31`/`MOVEM.L`/`TAS` loops | within 1.3 % (GPGX slightly faster) | | | |

**Fixed:** `MdVdp::ConsumeSlots` counted the frame's last line (261 NTSC, 311 PAL) as blank, so
a DMA running across the end of VBlank got the 167/205-bytes-per-line VBlank rate there instead
of the 16/18 of active display. `IsInVBlank` already ended VBlank one line before the end of the
frame, because the VDP fetches line 0's sprites and patterns during the last line. Source: both
oracles agree with each other (GPGX picks the DMA rate from the VBlank status flag, which
clears on that line), and MesenCE's own status flag clears there. Judgment call: no hardware
measurement here. Regression: `tests/MD/dma/check_dma_timing.py` in `test:md-synthetic`; the 60
BlastEm-referenced cartridges still pass with both FM cores.

The DMA fix alone does not move Mega Turrican's first difference (frame 585, the game a frame
ahead in MesenCE), and the movie still paused.

**Fixed: 68000 accesses to the Z80 area were a cycle too cheap.** More loop cartridges
(`tests/MD/timing/check_bus_timing.py`), iterations per frame:

| Loop | MesenCE before | GPGX | PicoDrive | MesenCE after |
|---|---|---|---|---|
| `move.b $A00000,d1` (Z80 RAM, bus held) | 3,702 | 3,603 | 3,595 | 3,597 |
| two YM2612 writes | 2,170 | 2,100 | 2,097 | 2,098 |
| YM2612 write + busy poll | 1,908 | 1,854 | 1,850 | 1,855 |
| VDP status, H/V counter, pad port, bus request and release | equal | | | equal |

Both oracles charge about one more 68000 cycle per access to `$A00000-$A0FFFF` while the 68000
holds the Z80 bus. Genesis Plus GX does it deliberately (`mem68k.c`, `z80_read_byte` and
`z80_write_byte`: `m68k.cycles += 1 * 7`, "Z80 bus access latency (fixes Pacman 2: New
Adventures sound engine crashes & Puyo Puyo 2 crash when exiting option menu)"); PicoDrive
measures the same. Kabuto's hardware notes cover only the opposite direction (the Z80 on the
68000 bus). Judgment call: two emulators and game behaviour, no hardware measurement.
`MdMemoryManager::AddZ80AreaLatency` now adds one 68000 cycle to each byte or word access
there while the 68000 owns the bus (a word access is one bus access, as in GPGX).

Mega Turrican's sound driver feeds the Z80 and YM2612 from the 68000, so its game logic ran
faster in MesenCE. With the latency, `mato-megaturrican.gmv` stays in step with GPGX for the
whole movie, and none of the 156 snapshots differ from PicoDrive (median 0.014, was 0.189). The
60 BlastEm-referenced cartridges still pass.

### Third batch: 59 movies after the two Mega Turrican fixes

All 59 sessions ran again with build `b55fcecb` (177 runs, 91 min, no failed runs). All three
emulators stay in step in 43 movies (was 41); MesenCE and PicoDrive in 52 (was 50).

- **Now in step to the end:** Mega Turrican (was 21,288) and Streets of Rage 2 (was 84,050; its
  player no longer loses an extra life). Streets of Rage 2 also uses a 68000-fed sound driver.
- **Unchanged, still MesenCE-only:** The Lion King `e6f6baab4775` (~2,140), Kid Chameleon
  (108,091, after the last input), Sonic 3 & Knuckles `efa466ceedd5` (MesenCE~PicoDrive 126,004;
  MesenCE~GPGX is now in step to the end).
- **Moved by one to three frames:** the movies where all three emulators drift (Gens Sonic 1,
  Sonic 3 & Knuckles `2d935d88003a`, Shinobi III).
- **Earlier GPGX divergence:** Comix Zone, MesenCE~GPGX 22,231 → 3,149 while MesenCE~PicoDrive
  stays in step to the end and GPGX~PicoDrive drifts at 24,093. To check.
