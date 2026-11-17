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
samples) because the display is off and the VBlank poll ends at once; the channel's attack rate
(AR 28 plus key scaling) gives an instant attack. (The previous KNOWN text said "well under one YM
sample"; that was wrong.)

- Evidence: the YM2612 writes of MesenCE's bus trace replayed through Nuked-OPN2
  (github.com/nukeykt/Nuked-OPN2, commit 335747d78cb0, a die-shot based cycle-accurate YM3438/YM2612
  model, YM2612 mode, one sample per 24 internal cycles). Peak per 256 samples, median removed:
  MesenCE 231 ×8 blocks then silence; Nuked 264 ×2 blocks then silence; BlastEm 114 in the first
  block only (a start-up transient, rms 2.4). Both MesenCE and Nuked play a 260 Hz note (the
  channel's pitch); BlastEm plays none. nativekit's own model (`keyon_write`) also starts the attack
  on the write.
- Remaining difference (open, not hidden by the KNOWN entry): the note lasts ~40 ms in MesenCE
  (ymfm) and ~10 ms in Nuked. Possibly ymfm's attack/release timing for a key-off shortly after an
  instant attack; not investigated yet.
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
