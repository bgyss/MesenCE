# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

MesenCE is a community fork of Mesen2: a multi-system emulator (NES, SNES, GB/SGB/GBC, GBA, PC Engine, SMS/Game Gear, WonderSwan, and in this fork Mega Drive/Genesis) with a C++17 emulation core and a C#/.NET 10 Avalonia UI.

**This repository is an independent fork of MesenCE.** It's run as its own project, as an example of LLM-assisted development backed by testing and formal verification. Fixes are applied here directly. Upstream branches are prepared only when the user asks (the Mega Drive core: `docs/upstream/`); upstream requires MIT-compatible code and currently prohibits AI-generated code, so such branches are proposals that disclose how they were made.

## Contribution policy (see CONTRIBUTING.md)

- **LLM-assisted code is welcome. Evidence is required:**
  - a reproduction (failing test, ROM, sanitizer report or model counterexample);
  - a source for the intended behaviour;
  - a regression test wired into a `mise` task;
  - the existing checks still passing;
  - an entry in `docs/research/*-findings.md`.
- Label judgment calls as such, and record retracted or "not a bug" results too.
- The license is GPL v3, the same as upstream. Keep upstream copyright notices.
- Performance is a review criterion: avoid adding branches or work to emulation hot paths. Max FPS is checked with F9 (unlocked framerate) and F10 (FPS display).
- MSVC builds treat warnings as errors. Linux clang builds use `-Werror`.
- Naming: `ExampleFunction`, `exampleVariable`, `_exampleMemberVariable`.
- Formatting is enforced:
  - C++: `find ./ -iname '*.h' -o -iname '*.cpp' | xargs clang-format -i`. Third-party dirs are excluded via `.clang-format-ignore`.
  - C#: `dotnet format`.

## Dev environment (Nix + mise)

This fork's addition, not upstream.

- **Toolchain:** `flake.nix` provides it: clang, .NET 10 SDK, SDL2, clang-format and ccache. `.envrc` (`use flake`) loads it through direnv.
- **mise:** `mise.toml` holds only env vars and tasks. It does not install tools.
- **Tasks:**
  - `mise run build`, `mise run build:debug`, `mise run build:asan`.
  - `mise run core`: native library only.
  - `mise run run`: launches the build, pointing the loader at Nix's SDL2.
  - `mise run test -- script.lua rom`.
  - `mise run test:ci`: the MesenTests regression suite, then the fork's own recorded tests in `tests/` (see `tests/README.md`).
  - `mise run build:test-roms`: assembles the fork's test ROMs (`tests/*/src`, WLA-DX for SMS, ca65 for SNES) into `bin/test-roms`.
  - `mise run test:record -- <rom> <out.mtp> [--frames N]`: records a `.mtp` headlessly (`tools/mtp-record`) and checks that it replays exactly. `mise run test:record-fork` re-records everything in `tests/manifest.txt`.
  - `mise run test:cpu`: NES CPU vs the SingleStepTests `nes6502` suite (2.56M tests incl. per-cycle bus traces). Downloads 1.2 GB into `bin/`. Known divergences are whitelisted in `tools/nes-cpu-singlestep/singlestep.cpp`.
  - `mise run test:gb-cpu`: Game Boy SM83 CPU vs the SingleStepTests `sm83` suite (500k tests: bus trace, registers, RAM and M-cycle count). Downloads about 150 MB into `bin/`.
  - `mise run test:spc-cpu`: SNES SPC700 vs the SingleStepTests `spc700` suite (256k tests: bus trace, registers, RAM and cycle count), run on `DummySpc`. Downloads about 100 MB into `bin/`. Tests touching the `$F0-$FF` I/O page are skipped.
  - `mise run test:gba-cpu`: GBA ARM7TDMI vs the SingleStepTests `ARM7TDMI` suite (2.25M tests: registers in every bank, CPSR/SPSRs, pipeline, data accesses and code-fetch addresses; no timing). The harness compiles `Core/GBA/GbaCpu*.cpp` itself. UNPREDICTABLE-encoding divergences are classified per test. Downloads about 1 GB into `bin/`.
  - `mise run test:pce-cpu`: PC Engine HuC6280 vs SingleStepTests `huc6280` (a Mesen-generated *regression baseline*, incl. timing and bus) and `rockwell65c02` (independent; shared opcodes, registers and RAM, with Mednafen as the reference for documented HuC6280 differences). The harness compiles `Core/PCE/PceCpu*.cpp` itself.
  - `mise run test:snes-cpu`: SNES 65816 vs the SingleStepTests `65816` suite (5.08M tests, native and emulation mode: bus trace, registers, RAM and cycle count). The harness compiles `Core/SNES/SnesCpu.cpp` itself against a flat 16 MiB test bus. Downloads about 3.1 GB into `bin/`.
  - `mise run test:sa1-cpu`: the same suite and harness on the SA-1's `Sa1Cpu` (`--cpu sa1`), compiled against a stub `Sa1`.
  - `mise run test:z80-cpu`: SMS/GG/Coleco Z80 vs the SingleStepTests `z80` suite (1.6M tests incl. WZ/Q, bus trace and T-states). The harness compiles `Core/SMS/SmsCpu.cpp` itself against a flat test bus. Downloads about 1.6 GB into `bin/`.
  - `mise run test:gb-irq`: Game Boy interrupt/HALT/EI scenarios on the real `GbCpu`, checked against Pan Docs rules. Known bugs are reported but don't fail the run.
  - `mise run test:nes-dma`: NES OAM-DMA regression test. It builds the self-checking ROM `docs/research/tla/rom/dmc_oam_overlap.s` with cc65 and runs it with the DMC duplication glitch on.
  - `mise run test:savestate -- <roms|dir>`: save state → fresh console → per-frame state comparison, for NES, GB/GBC, SNES, SMS/GG/SG, PCE and WS ROMs. It finds emulation state missing from `Serialize`. `mise run test:savestate-all` runs it on every ROM embedded in MesenTests and `tests/` (about 5 minutes).
  - Mega Drive (`Core/MD`, plan in `docs/research/mega-drive-genesis-feasibility.md`):
    - `mise run test:m68k-cpu`: the ares-derived 68000 (`Core/MD/M68k`) vs SingleStepTests `m68000` (plus `--scenarios` for reset/IRQ/STOP, `--bench`).
    - `mise run headless` / `test:headless`: the native runner `bin/mesen-headless` (no UI/SDL; contract in `docs/headless.md`) and its self-test.
    - `mise run test:md-synthetic`: 60 synthetic cartridges (`tests/MD/synthetic`) vs the recorded BlastEm reference (reads, frames, per-chip audio); reviewed differences live in `tests/MD/synthetic/KNOWN`. Plus MesenCE's own cases (`extra_cases.py`, checked against expected values): EEPROM boards, the bank mapper, Team Player/mouse/light guns (Lua input in `tests/MD/accessories`), mid-line rendering (`tests/MD/midline/check_midline.py`), and the Sega CD unit in Mode 1 (`mcd_cases.py`, `--md-mcd`). Per-case runner options (`--md-eeprom`, `--md-pads t:63n6,m`, `--md-mcd`) travel in the generated spec's `runner_args`.
    - `mise run test:md-replay`: save-state replay must reproduce the bus trace, chip audio and every field of the final state; `mise run test:md-xarch -- <other runner>`: byte-identical captures across builds/architectures.
    - `mise run test:md-cheats`: Game Genie / Pro Action Replay codes on the bus_map cartridge. `test:md-synthetic` and `test:md-replay` run both FM cores (ymfm, Nuked-OPN2).
    - Feature parity with other Mega Drive emulators: `docs/research/md-feature-parity.md`.
    - Library-wide testing against oracle emulators: `mise run md-corpus -- inventory|run|compare|report|diff|tas` (`tools/md-corpus`; TAS movies are imported from files you download, `tas import` (reads `bin/md-corpus/movies`, git-ignored, unless given folders), then `run --session tas`, never fetched: TASVideos' robots.txt; MesenCE vs GPGX and PicoDrive per session: health, per-frame work RAM blocks from `mesen-headless --state-log --state-blocks`, snapshots, audio; report in `bin/md-corpus/report.html`). Strategy, materials and phases: `docs/research/md-oracle-testing-strategy.md`; results: `md-verification-findings.md` C1-C4. BizHawk movies name their ROM by MD5; `.gmv` movies need `movies/roms.txt`. GPGX's input is shifted per movie, calibrated against PicoDrive (`tas/<id>/calibration.json`). A rerun of one emulator in a session needs the whole session rerun (comparison deletes the audio).
    - 32X (plan in `docs/research/32x-feasibility.md`, findings in `docs/research/32x-verification-findings.md`): `mise run test:sh2-cpu` checks the SH-2 core (`Core/MD/Sh2`, ported from ares) against the SH-2 encodings of SingleStepTests `sh4` (Reicast-generated; SH-4-only encodings, TRAPA/RTE/SLEEP and SR.RB bank switches are excluded), plus `--scenarios` for exceptions, interrupts, MAC, and the on-chip DIVU/FRT/DMAC/cache. Downloads about 90 MB into `bin/`. Instruction timing follows the Hitachi SH-1/SH-2 programming manual.
    - `mise run test:md-viewers`: the debugger's palette/tilemap/sprite viewers (`MdVdpTools`) must decode VRAM/CRAM exactly as the VDP renders.
  - Findings from these tools are logged in `docs/research/{nes,gb,snes,sms,gba,pce,md}-verification-findings.md`.
  - Sega CD (`Core/MD/Mcd`; S1-S5 done (S5: 31 commercial discs qualified, findings S5): Mode 1 unit behind `mesen-headless --md-mcd [--md-mcd-disc X.cue]`, disc boot from a Sega CD `.cue` with the BIOS from firmware or `--md-mcd-bios`; S4: sub CPU debugger `CpuType::McdM68k`, memory types `McdMemory`/`McdPrgRam`/`McdWordRam`/`McdBackupRam`/`McdPcmRam`/`McdCdcRam`/`McdBios`, checked by `tests/MD/mcd/debugger.lua` in `test:md-synthetic`). A new CPU address-space memory type must also move `DebugUtilities::GetLastCpuMemoryType()`. `mise run test:mcd-bios` boots the synthetic disc with each BIOS dump found locally (`MESEN_MCD_BIOS_DIR`, default `~/mame/roms/segacd`) and checks the disc region from the security code. Sub CPU writes to main-visible gate array registers that land past the catch-up target are deferred (`McdUnit::_deferred`, `tests/MD/mcd/swap_race.py`). `mesen-headless --snap-every N` writes screenshots during a run. Plan in `docs/research/sega-cd-feasibility.md`, findings in `docs/research/mcd-verification-findings.md`.
  - The makefile doesn't track `#include` dependencies. The dummy CPUs (`Debugger/Dummy*.cpp`) `#include` the real CPU `.cpp` files, so `touch` them after editing a CPU, or the debugger keeps stale code. **If you change a class layout in a header** (add or remove a member), touch every `.cpp` that includes it (e.g. `find Core/SMS -name '*.cpp' -exec touch {} +`). Otherwise objects compiled against the old layout corrupt memory: a stale `SmsCpu.h` made `test:ci` hang in `SmsPsg::Run`.
  - `mise run test:snes-irq` / `mise run test:z80-irq`: interrupt scenarios for the 65816 (SNES CPU and SA-1) and the Z80, on the real CPU sources against a flat test bus.
  - The harnesses drive consoles without the emulation thread. They set `Emulator::_isRunAheadFrame` (so frames aren't sent to the video decoder) and register the console as `Emulator::_console` (some hardware, e.g. the MBC3 RTC, reads `Emulator::GetMasterClock()`).
  - `mise run tla`: model-checks `docs/research/tla/NesDmaScheduler.tla` (mirrors `NesCpu::ProcessPendingDma`) and `GbIrqStateMachine.tla` (mirrors `GbCpu::Exec`'s IME/EI/HALT handling). If you change that code, update the matching spec and re-run.
  - `mise run format`, `mise run clean`.
- **`nix build .#core`:** hermetic build of only `MesenCore.{dylib,so}`, written to `result/lib`.
  - It uses `STATICLINK=false LTO=false` and drops upstream's `-Werror`.
  - The UI is not packaged in Nix because NuGet restore needs network access.
- **Running outside `mise run run`:** SDL2 is `dlopen`'d by bare name, so set `DYLD_FALLBACK_LIBRARY_PATH` (macOS) or `LD_LIBRARY_PATH` (Linux) to `$MESEN_RUNTIME_LIBRARY_PATH`. Otherwise the emulator runs with no audio.

## Build

- **Linux/macOS:** `make` builds the native core and the UI.
  - Requires the .NET 10 SDK. SDL2 is optional at build time and only provides audio at runtime.
  - Output: `bin/<os>-<arch>/Release/<os>-<arch>/publish/Mesen`. Launch it with `make run`.
- **Native core only:** `make core` builds `InteropDLL/obj.<platform>/MesenCore.{so,dylib}`. It's much faster when iterating on C++.
- **Build options:**
  - `DEBUG=1`: `-O0 -g`.
  - `SANITIZER=address|thread`: requires `DEBUG=1`.
  - `USE_GCC=true`: clang is the default and is faster.
  - `USE_AOT=true`: NativeAOT UI.
  - `LTO=false`.
  - `SYSTEM_LIBEVDEV=true`.
  - `make pgo` / `buildPGO.sh`: PGO build via `PGOHelper/`.
- `make clean` removes object files. Objects are built in-tree as `.o` next to their sources.
- **Windows:** open `Mesen.sln` in VS 2022/2026, build `Release|x64`, and set the startup project to `UI`.
- **Shaders:** the UI uses librashader (`UI/Dependencies/`). CI downloads it via `.github/actions/get-librashader-action`.

## Testing

There is no unit-test suite. Testing is ROM-based:

- **Regression suite (what CI runs):** `.github/workflows/tests.yml` checks out [nesdev-org/MesenTests](https://github.com/nesdev-org/MesenTests), which holds about 90 MB of `.mtp` recorded tests.
  - CI runs `PGOHelper <testsDir> citests`. This calls `RunCiTests` in `InteropDLL/TestApiWrapper.cpp`, which runs every test on all CPU threads.
  - GBA tests and a few boot-ROM-dependent GB/SGB tests are skipped.
  - Run it locally with `mise run test:ci`.

- **Headless Lua test runner:** `Mesen --testRunner script.lua rom.nes [--timeout=N]`.
  - Runs at max speed with no UI.
  - The process exit code is whatever the script passes to `emu.stop(code)`.
  - Implemented in `UI/Utilities/TestRunner.cs`.
- **Recorded ROM tests (`.mtp`):** these record per-frame hashes and replay them to detect regressions.
  - Run them from the UI's debug/test menus.
  - Implemented in `Core/Shared/RecordedRomTest.cpp`, `UI/Utilities/RomTestHelper.cs` and `UI/Interop/TestApi.cs`.
  - Test files live in the config's `TestFolder`.

## Architecture

### Layers

1. **`Core/`:** the emulation core, platform-independent C++.
2. **`Utilities/`:** shared C++ helpers (serializer, VirtualFile, audio/video filters, codecs) plus vendored third-party code.
3. **Platform layers:**
   - `Windows/`: DirectX renderer, XAudio/DirectSound, input.
   - `Linux/`: OpenGL renderer, libevdev controllers, X11 keyboard.
   - `MacOS/`: `.mm` key, mouse and GameController files.
   - `Sdl/`: **audio only**, on Linux and macOS. `Sdl/include/Sdl2.h` `dlopen`s `libSDL2` at runtime with hand-declared function pointers, so there's no link-time SDL dependency.
4. **`InteropDLL/`:** the `extern "C"` API, split into `*ApiWrapper.cpp` files (Emu, Debug, Config, Input, History, Netplay, Record, Test). It's compiled together with everything above into a single `MesenCore` shared library. It also picks the platform implementations (renderer, sound, key and mouse managers) via `#ifdef`.
5. **`UI/`:** the Avalonia app.
   - `UI/Interop/*Api.cs` P/Invoke the wrapper functions 1:1. Any C++ API change needs matching edits on both sides, and the C# struct layouts in `UI/Interop/ConsoleState/` must mirror the C++ state structs.
   - UI strings live in `UI/Localization/resources.en.xml`.
   - Config is defined in `UI/Config/` and pushed to the core through `ConfigApi`.

### Core structure

- **`Emulator` (`Core/Shared/Emulator.{h,cpp}`):** the central object.
  - It owns the active `IConsole`, settings, sound mixer, video decoder/renderer, rewind, save states, movies, cheats, notifications and the debugger.
  - Emulation runs on its own `_emuThread` (`Emulator::Run`), which calls `IConsole::RunFrame()`.
  - The UI thread interacts through `EmulatorLock` / pause, plus notifications.
- **Console selection:** `Emulator::TryLoadRom<T>` tries each console type in turn (`NesConsole`, `SnesConsole`, `Gameboy`, `PceConsole`, `SmsConsole`, `GbaConsole`, `WsConsole`), first by file extension and then by file signature.
- **`IConsole` (`Core/Shared/Interfaces/IConsole.h`):** the per-system contract (load, reset, frame execution, timing, CPU types, serialization).
- **One directory per system** (`Core/NES`, `SNES`, `Gameboy`, `GBA`, `PCE`, `SMS`, `WS`). Each has:
  - Its own CPU, PPU, APU, memory manager, control manager and cart/mapper code.
  - A `Debugger/` subfolder.
  - SNES also contains the coprocessors (SA-1, SuperFX/GSU, Cx4, DSP, SPC700, etc.), and the Game Boy core is reused for SGB.
- **Debugger hooks:**
  - CPUs and memory managers call templated hooks such as `_emu->ProcessMemoryRead<CpuType::X>(...)` and `ProcessInstruction`. These forward to `Core/Debugger/Debugger` only when a debugger is attached, so keep them cheap.
  - Per-CPU debuggers implement `IDebugger`.
  - Shared tools in `Core/Debugger/` include breakpoints, trace logger, CDL, disassembler, `ExpressionEvaluator.<System>.cpp` and Lua scripting (`ScriptingContext`).
- **Dummy CPUs:** `Debugger/Dummy*Cpu.{h,cpp}` re-include the real CPU source under `#define DUMMYCPU` / `#define NesCpu DummyNesCpu`. This predicts an instruction's memory accesses without side effects. When editing a CPU, keep the code compatible with this pattern.
- **State serialization:** components implement `ISerializable::Serialize(Serializer& s)` with `SV(...)` macros. This one mechanism powers save states, rewind (`RewindManager`), the history viewer, netplay and movies. New emulated state **must** be added to `Serialize` or those features desync.
- **Settings:** `EmuSettings` holds config pushed from the UI. Cores read it through `_emu->GetSettings()`.

### CI

`.github/workflows/build.yml` builds four configurations:

- Windows: MSBuild + `dotnet publish`.
- Linux: clang/gcc × JIT/AOT, x64 and arm64.
- AppImage.
- macOS: Intel and ARM.
