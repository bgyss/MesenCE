# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

MesenCE is a community fork of Mesen2: a multi-system emulator (NES, SNES, GB/SGB/GBC, GBA, PC Engine, SMS/Game Gear, WonderSwan) with a C++17 emulation core and a C#/.NET 10 Avalonia UI.

## Contribution policy (from CONTRIBUTING.md)

- **Upstream prohibits AI-generated code in submitted PRs.** Contributors must write and understand the code they submit. Keep this in mind when anything here is intended for upstream.
- Contributions are licensed MIT, so do not base code on GPL sources or pull in GPL libraries.
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
  - `mise run test:ci`: the MesenTests regression suite.
  - `mise run test:cpu`: NES CPU vs the SingleStepTests `nes6502` suite (2.56M tests incl. per-cycle bus traces). Downloads 1.2 GB into `bin/`. Known divergences are whitelisted in `tools/nes-cpu-singlestep/singlestep.cpp`.
  - `mise run test:gb-cpu`: Game Boy SM83 CPU vs the SingleStepTests `sm83` suite (500k tests: bus trace, registers, RAM and M-cycle count). Downloads about 150 MB into `bin/`.
  - `mise run test:gb-irq`: Game Boy interrupt/HALT/EI scenarios on the real `GbCpu`, checked against Pan Docs rules. Known bugs are reported but don't fail the run.
  - `mise run test:savestate -- <roms|dir>`: save state → fresh console → per-frame state comparison, for `.nes`, `.gb` and `.gbc`. It finds emulation state missing from `Serialize`.
  - Findings from these tools are logged in `docs/research/nes-verification-findings.md` and `docs/research/gb-verification-findings.md`.
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
  - The process exit code is whatever the script passes to `emu.exit(code)`.
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
