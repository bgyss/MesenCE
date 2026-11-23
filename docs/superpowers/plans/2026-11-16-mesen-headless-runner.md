# Mesen native headless runner implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship one deterministic, display-free native runner for every MesenCE console so nativekit and cloud jobs no longer need a private per-console host.

**Architecture:** Add a C++ executable that links the same native core as the UI, owns one `Emulator`, and exposes `doctor`, `run`, and `validate` commands. Keep command parsing, deterministic settings, input scheduling, artifact writing, and manifest validation in focused tool-only files; do not add a second emulation API or route through .NET.

**Tech Stack:** C++17, MesenCE native core, nlohmann JSON, Nix, mise, Python validation checks.

**Spec:** [`docs/research/mega-drive-genesis-feasibility.md`](../../research/mega-drive-genesis-feasibility.md)

## Global constraints

- The runner must work without Avalonia, SDL audio, a window server, or desktop automation.
- A process owns exactly one `Emulator`, one isolated home directory, and one output directory.
- Qualified runs use deterministic power-on state, bounded frames/time/events, and explicit controller snapshots.
- The runner records ROM SHA-1 initially because `VirtualFile` already owns that implementation; adding SHA-256 is a separate shared-utility change.
- The runner never copies a ROM, battery file, or external Lua source into its artifact directory.
- A timeout, Lua error, missing artifact, truncation, or dirty qualified build produces a nonzero exit and an incomplete manifest.
- Existing NES/SNES behavior and `mise run test:ci` remain unchanged.

## Review focus

- Output directory already exists: `run` must fail before modifying it.
- A script stops before the first completed frame: screenshot generation must explicitly decode the current PPU buffer.
- Input references an unsupported player/button: parsing must fail before ROM execution.
- Save-state or WAV creation fails after emulation: the manifest must remain incomplete and validation must fail.
- Two concurrent jobs: isolated homes and outputs must prevent battery/config/artifact cross-talk.

---

### Task 1: Define the versioned command and manifest contract

**Files:**

- Create: `tools/headless-runner/HeadlessTypes.h`
- Create: `tools/headless-runner/HeadlessManifest.h`
- Create: `tools/headless-runner/HeadlessManifest.cpp`
- Create: `tools/headless-runner/tests/manifest_check.py`
- Create: `docs/headless-runner.md`

**Interfaces:**

- Produces: `HeadlessRunOptions`, `HeadlessCompletion`, `HeadlessArtifact`, and `HeadlessManifest` value types.
- Produces: `bool WriteManifest(const HeadlessManifest&, const std::filesystem::path&, std::string&)`.
- Produces: manifest schema `mesen-headless/v1` consumed by all later tasks.

- [ ] **Step 1: Write the failing manifest checks**

  Add cases for a complete manifest, missing identity, duplicate artifact names, path escape, hash mismatch, incomplete completion, and unknown schema version. Assert that only the complete fixture validates.

- [ ] **Step 2: Run the check and confirm it fails because fixtures/the writer do not exist**

  Run: `python3 tools/headless-runner/tests/manifest_check.py`

  Expected: nonzero exit naming the missing fixture or executable.

- [ ] **Step 3: Define the v1 schema and writer**

  Include build commit/dirty flag, ROM SHA-1, console/region/settings, controller types, input/Lua hashes, command, completion reason, frame/master clock, and artifact path/size/hash/schema. Use paths relative to the output directory and reject `..` components.

- [ ] **Step 4: Document the exact CLI and artifact contract**

  Document `doctor`, `run`, and `validate`; qualified versus exploratory runs; exit codes; isolation; and how nativekit should consume artifacts.

- [ ] **Step 5: Run the manifest checks**

  Run: `python3 tools/headless-runner/tests/manifest_check.py`

  Expected: all manifest cases pass.

- [ ] **Step 6: Commit**

  ```bash
  git add tools/headless-runner docs/headless-runner.md
  git commit -m "headless: define runner artifact contract"
  ```

### Task 2: Implement deterministic options and input schedules

**Files:**

- Create: `tools/headless-runner/HeadlessSettings.h`
- Create: `tools/headless-runner/HeadlessSettings.cpp`
- Create: `tools/headless-runner/InputSchedule.h`
- Create: `tools/headless-runner/InputSchedule.cpp`
- Create: `tools/headless-runner/tests/input_schedule_check.cpp`

**Interfaces:**

- Consumes: `HeadlessRunOptions` from Task 1.
- Produces: `void ApplyDeterministicSettings(Emulator&, const HeadlessRunOptions&)`.
- Produces: `InputSchedule InputSchedule::Load(path)`, `void InputSchedule::SetFrame(uint32_t)`, and `bool InputSchedule::SetInput(BaseControlDevice*) override` through the existing `IInputProvider` seam.

- [ ] **Step 1: Write failing parser tests**

  Cover empty schedules, two ports, held/released snapshots, duplicate frame keys, unknown buttons, invalid player numbers, descending frames, and malformed JSON.

- [ ] **Step 2: Run the focused test and confirm failure**

  Run: `mise run test:headless-unit`

  Expected: build failure because the schedule/settings implementation is absent.

- [ ] **Step 3: Implement complete-frame input snapshots**

  Use one-based frame keys and string port keys (`"1"`, `"2"`). Treat each listed frame as a full snapshot rather than an edge list. Resolve button names through `BaseControlDevice::GetKeyNameAssociations()`, register the schedule with `BaseControlManager::RegisterInputProvider()`, and fail on unsupported values.

- [ ] **Step 4: Implement deterministic shared and per-console settings**

  Disable rewind, auto-save, dynamic audio sample rate, OSD, random RAM/alignment/mapper state, and network access. Require a console-specific settings function; initially implement NES and SNES and fail closed for unqualified consoles.

- [ ] **Step 5: Run the focused test**

  Run: `mise run test:headless-unit`

  Expected: all schedule and settings cases pass.

- [ ] **Step 6: Commit**

  ```bash
  git add tools/headless-runner mise.toml
  git commit -m "headless: add deterministic settings and input"
  ```

### Task 3: Implement `doctor`, `run`, and artifact capture

**Files:**

- Create: `tools/headless-runner/main.cpp`
- Create: `tools/headless-runner/HeadlessRun.h`
- Create: `tools/headless-runner/HeadlessRun.cpp`
- Modify: `mise.toml`

**Interfaces:**

- Consumes: Tasks 1-2 types, settings, input, and manifest writer.
- Produces: executable `bin/mesen-headless`.
- Produces: `int RunHeadless(const HeadlessRunOptions&)`, `int ValidateHeadless(path)`, and `int DoctorHeadless()`.

- [ ] **Step 1: Add a failing end-to-end smoke check**

  Run the existing authored NES test ROM for a bounded frame count and assert that `manifest.json`, `final.png`, `final.mss`, and `audio.wav` exist and validate.

- [ ] **Step 2: Build and run the failing smoke check**

  Run: `mise run test:headless`

  Expected: failure because `mesen-headless` is not built.

- [ ] **Step 3: Implement `doctor` and command parsing**

  `doctor` reports core/build identity, dirty status, supported consoles, and software screenshot/audio/state availability without reading user configuration.

- [ ] **Step 4: Implement bounded `run`**

  Set an isolated home before constructing `Emulator`; load the ROM; arm optional Lua before the first opcode; apply frame input; enforce timeout/frame limits; synchronously stop; record completion before writing final artifacts.

- [ ] **Step 5: Implement artifact finalization and `validate`**

  Decode `GetPpuFrame()` through the default software filter, record WAV through `SoundMixer`, save through `SaveStateManager`, hash artifacts, and write the complete manifest last. `validate` re-hashes every artifact and checks schemas and completion.

- [ ] **Step 6: Run focused and existing headless checks**

  Run: `mise run test:headless`

  Run: `mise run test:record -- bin/test-roms/z80_timing.sms /tmp/mesen-headless-plan-z80.mtp --frames 120`

  Expected: runner smoke passes and recorded-test replay passes.

- [ ] **Step 7: Commit**

  ```bash
  git add tools/headless-runner mise.toml
  git commit -m "headless: add native deterministic runner"
  ```

### Task 4: Prove failure controls and concurrency isolation

**Files:**

- Create: `tools/headless-runner/tests/integration_check.py`
- Modify: `mise.toml`

**Interfaces:**

- Consumes: `bin/mesen-headless` from Task 3.
- Produces: negative-control and parallel-isolation coverage in `mise run test:headless`.

- [ ] **Step 1: Add negative-control tests**

  Cover pre-existing output, invalid ROM, unsupported console qualification, timeout, Lua exception, missing artifact, modified artifact, path escape, and invalid input.

- [ ] **Step 2: Add two-process isolation test**

  Launch identical ROM/input runs with different temporary homes/outputs, assert both validate, and assert matching artifact hashes with no files outside their roots.

- [ ] **Step 3: Run all runner tests**

  Run: `mise run test:headless`

  Expected: positive, negative, and concurrent cases pass.

- [ ] **Step 4: Run repository regressions**

  Run: `mise run test:ci`

  Expected: upstream and fork suites pass with zero failures.

- [ ] **Step 5: Commit**

  ```bash
  git add tools/headless-runner/tests mise.toml
  git commit -m "test: qualify headless runner failure controls"
  ```

### Task 5: Package the runner for cloud use

**Files:**

- Modify: `flake.nix`
- Modify: `docs/headless-runner.md`
- Create: `tools/headless-runner/container/Containerfile`

**Interfaces:**

- Consumes: `bin/mesen-headless` and native core.
- Produces: `nix build .#headless` with `$out/bin/mesen-headless` and required runtime libraries.

- [ ] **Step 1: Add the Nix package and check**

  Build from a clean source filter, install the executable plus native library, and run `doctor` in `installCheckPhase`.

- [ ] **Step 2: Build on the current host**

  Run: `nix build .#headless`

  Expected: `result/bin/mesen-headless doctor` exits zero without a display or audio device.

- [ ] **Step 3: Add the minimal container example**

  Use the Nix-built closure or a pinned runtime base. Require explicit read-only ROM/input mounts and a writable output mount; do not bake ROMs or user state into the image.

- [ ] **Step 4: Re-run privacy and documentation checks**

  Run: `rg -n '/Users/|/home/|~|\.codex|/Applications|/private/tmp' docs/headless-runner.md tools/headless-runner`

  Expected: no personal-machine paths.

- [ ] **Step 5: Commit**

  ```bash
  git add flake.nix docs/headless-runner.md tools/headless-runner/container
  git commit -m "build: package native headless runner"
  ```
