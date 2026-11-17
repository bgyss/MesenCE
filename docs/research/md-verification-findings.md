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
- Approximations, not modelled (judgment calls, recorded here so they are not mistaken for
  verified behaviour): DRAM refresh stalls; the 68000 stall while the Z80 uses the bank window;
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
- Result: 60/60 replay identical for both splits; round trip 60/60 consistent.

## M4 — VDP and video

### V1. Synthetic VDP/render cases vs BlastEm (`mise run test:md-synthetic`)

- `tests/MD/synthetic/compare.py` checks the 68000 read values of every READ step (exact
  address, width, value), that every POLL step terminates, and for render cases the final frame
  up to a colour relabelling (BlastEm and MesenCE use different DAC levels) after locating the
  active area inside BlastEm's bordered frame; interlace mode 2 is compared per field.
- Result: **54 pass, 6 KNOWN, 0 fail** of 60. KNOWN cases are listed with reasons in
  `tests/MD/synthetic/KNOWN` (S3 below).

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

### S3. KNOWN reference disagreements (`tests/MD/synthetic/KNOWN`)

- `sprite_collision_status`, `sprite_overflow_status_h32/h40`,
  `sprite_overflow_status_reread_h32`: open-bus status bits 15-10 only (V2).
- `vdp_port_timing`: the same open-bus bits, plus the H-blank bit of the first status read
  after reset, which depends on the VDP's power-on position (unmodelled in both emulators).
- `sound_ym_ports`: YM2612 busy duration after a data write: ymfm 32×6 YM clocks
  (1344 MCLK), BlastEm about 900 MCLK longer. No hardware measurement available.
- `sound_ym_envelope_key_on_off` (audio): key-off follows key-on by far less than one YM
  sample; ymfm plays the attack and release, BlastEm produces nothing. Hardware-unverified.

## M5 — audio, controllers, debugger, determinism

### A1. YM2612 + PSG (`Core/MD/MdApu.cpp`, `tests/MD/synthetic/audio_compare.py`)

- `ymfm::ym2612` behind a `ymfm_interface` whose timers and busy flag run in master-clock time;
  status is readable on all four ports (BlastEm agrees). PSG = shared `Sn76489` (Z3).
- `--chip-audio` writes each chip's stream at the YM sample rate before mixing; the comparison
  requires the same activity, stereo placement and dominant frequency (1.5% or one FFT bin;
  broadband output, e.g. full-depth modulation in algorithms 2/3, compares spectral centroid
  and peak within 20%). Levels are not compared (different gain/DAC tables).
- Result: 17 audio cases, 16 agree + 1 KNOWN (`sound_ym_envelope_key_on_off`).

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
- Not implemented (the menu items are disabled for MD rather than broken): tile, tilemap,
  sprite, palette, register and event viewers (`ConsoleType.SupportsPpuTools`); cheats; OS file
  associations for `.md/.gen`.

### D2. Lua frame callbacks never fired on the Mega Drive (fixed)

- Repro: `Mesen --testRunner tests/headless/md_ui_smoke.lua <case>.md` timed out (exit −1)
  while the same pattern on an SMS ROM exited with the script's code: `MdVdp::EndFrame` never
  raised `EventType::EndFrame`/`StartFrame`, and never sent `PpuFrameDone` to the UI.
- Fix: both events and the notification are raised at the frame wrap (once per frame, no
  hot-path cost). Regression: `test:headless` "MD: Lua endFrame callbacks fire and the 68000
  state is readable".
- Related doc fix: the Lua stop function is `emu.stop(code)` (CLAUDE.md said `emu.exit`).

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
- Result: 60/60 identical, including cross-loaded continuations. `test:ci` passes with the flag.
- Speed (not a hardware claim): `render_h40` (idle 68000 loop, display on, audio) runs 3000
  frames in 3.9 s on arm64 (~770 fps). Commercial-game performance is M6 work.
