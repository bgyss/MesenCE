# Formal verification of MesenCE with TLA+ and Lean 4: prospects

Research date: 2026-11-04. Repo snapshot: `master` @ `4b8a196e`.

**Conventions.** Repo claims cite `path:line` relative to the repo root. External claims cite primary
sources: official docs, project repos and papers. A claim marked **[unverified]** has no primary
source I could confirm. A claim marked **[judgment]** is my own estimate or opinion. None of the
effort figures here were measured.

---

## TL;DR

- **Whole-emulator verification is not realistic.** The C++ core is about 185k lines (see the
  table in section 1.1). It is also written in a style that formal tools handle badly: timing is
  done by catch-up calls, the CPU drives the PPU, timers and DMA from inside each memory access,
  and there are 376 `__forceinline` and 554 `template<` sites.
- **Some slices are realistic and useful:**
  1. **Lean 4:** an executable bit-level spec of one small CPU, ideally the Game Boy SM83
     (`Core/Gameboy/GbCpu.cpp`, 1,580 lines). Prove some properties with `bv_decide`/`simp`, then
     run the spec against MesenCE as a **differential-testing oracle**, using the public
     SingleStepTests JSON suites.
  2. **TLA+:** small models of the **threading protocols**: the SDL audio ring buffer, the
     `Emulator::Lock`/`WaitForLock` handshake and the debugger break/step handshake. These are
     hand-written concurrent protocols built on spin-waits and atomic counters. That is the kind
     of system TLA+ was designed for.
  3. **TLA+ on the NES DMA scheduler** (section 6). A first model exists, and TLC has been run on
     NTSC, PAL and RMW-on-`$4014` configurations; all pass every check. TLC's one counterexample
     is a **confirmed emulator bug** in the opt-in DMC sample-duplication mode: a nested DMA call
     restarts OAM DMA, writing 257 bytes. A 60-line test ROM reproduced it 29 times in 299
     frames. A small fix was validated in a throwaway worktree (TLC, probe ROM, full suite) but
     not applied.
- **A proof that the C++ refines the Lean model** (a refinement proof) is a research project
  [judgment]. No production-grade tool takes optimized C++17 like this into Lean. The
  established route used elsewhere is co-simulation against an executable formal model (ACL2
  x86isa, LNSym). That gives high value for its cost.
- **What this would catch:** ISA flag and edge-case bugs in the modelled CPU, and races,
  deadlocks or lost updates in thread protocols.
  **What it would not catch:** cycle-accurate hardware behaviour that the spec author also gets
  wrong, PPU/APU analog behaviour, mapper quirks, or anything outside the modelled slice. The
  existing test-ROM CI already covers a lot of that ground.

---

## 1. The codebase as a verification target

### 1.1 Size per core

These are raw line counts of `*.cpp` + `*.h` per directory under `Core/`. They come from
`find <dir> -name '*.cpp' -o -name '*.h' | xargs cat | wc -l` and include comments and blank
lines.

| Directory | Lines | Notes |
|---|---:|---|
| `Core/NES` | 48,196 | 290 mapper headers under `Core/NES/Mappers` |
| `Core/SNES` | 35,724 | includes 13 coprocessor dirs (SA1, GSU, CX4, DSP, …) |
| `Core/Shared` | 26,127 | Emulator, rewind, movies, savestates, audio mixer |
| `Core/GBA` | 14,342 | |
| `Core/Debugger` | 12,995 | |
| `Core/Gameboy` | 12,476 | |
| `Core/PCE` | 12,093 | |
| `Core/WS` | 10,984 | |
| `Core/SMS` | 10,168 | |
| `Core/Netplay` | 1,740 | |

### 1.2 Candidate units, smallest first

| Unit | Files (lines) | Why it is a candidate |
|---|---|---|
| GB timer (DIV/TIMA) | `Core/Gameboy/GbTimer.cpp` (157) + `.h` (34) | Small state machine with falling-edge counter logic (`GbTimer.cpp:46-62`) and reload-delay flags (`GbTimer.cpp:29-44`). Easy to model completely. |
| SNES ALU mul/div | `Core/SNES/AluMulDiv.cpp` (136) + `.h` (32) | Pure arithmetic unit. Good for a first `bv_decide` exercise. |
| GB OAM DMA | `Core/Gameboy/GbDmaController.cpp` (216) | Small, but tied to the memory manager. |
| **SM83 CPU (Game Boy)** | `Core/Gameboy/GbCpu.cpp` (1,580) + `GbCpu.h` (191) | **Recommended pilot.** 8-bit, small ISA, a public per-instruction test suite exists (§4.1), and a "dummy CPU" build variant already exists (§1.4). |
| NES 6502 (2A03) | `Core/NES/NesCpu.cpp` (612) + `NesCpu.h` (914) | Small, but per-cycle DMA/NMI/IRQ edge logic is interleaved with execution (`NesCpu.cpp:298-327`). Test suite exists (`nes6502` in ProcessorTests, §4.1). |
| PCE HuC6280 | `Core/PCE/PceCpu*.cpp/h` (~1,840) | SingleStepTests' HuC6280 suite is described as *"Mesen-generated"* ([SingleStepTests org](https://github.com/orgs/SingleStepTests/repositories)), so it is not an independent oracle for this codebase. |
| SNES SPC700 | `Core/SNES/Spc.cpp` (654) + `Spc.h` (400) + `Spc.Instructions.cpp` (2,193) | Test suite exists. The clock-ratio sync with the main CPU uses `double` (`Spc.cpp:124-141`). |
| SMS Z80 | `Core/SMS/SmsCpu.cpp` (2,130) + `.h` (277) | Test suite exists ([SingleStepTests/z80](https://github.com/orgs/SingleStepTests/repositories)). |
| GBA ARM7TDMI | `Core/GBA/GbaCpu*.{cpp,h}` (~2,300) | Larger ISA. The SingleStepTests suite is labelled *"Experimental"*. |
| SNES 65816 | `Core/SNES/SnesCpu*` (~2,580) | Mode-dependent widths (M/X flags) make proofs harder. |

### 1.3 How components are wired and timed

All cores use **catch-up / CPU-driven scheduling** on one emulation thread.

- **Game Boy.** Every CPU memory access calls `ExecCpuCycle()`, which advances the memory
  manager twice (`GbCpu.cpp:482-488`). The memory manager's `Exec()` advances `CycleCount`,
  then runs the timer, DMA and serial, then the PPU (`GbMemoryManager.cpp:89-105`). As a
  result, *instruction semantics and whole-system timing share one call path*: `Read()` ticks
  the system before it reads (`GbCpu.cpp:520-525`). Some register writes have extra
  sub-instruction timing hacks; for example, the LCDC write runs `Exec()` before the write
  (`GbMemoryManager.cpp:511-523`).
- **NES.** `StartCpuCycle`/`EndCpuCycle` advance a master clock and call `PPU->Run(...)`, the
  mapper and the APU (`NesCpu.cpp:298-327`, `NesConsole.cpp:63-69`). NMI/IRQ edge detection
  runs in `EndCpuCycle` (`NesCpu.cpp:303-321`).
- **SNES.** The SPC700 is caught up lazily when the main CPU touches its ports
  (`Spc.cpp:349-357`). Its target cycle is computed as `masterClock * _clockRatio`, where
  `_clockRatio` is a **`double`** (`Spc.h:39`, `Spc.cpp:124-141`). The code silently resyncs if
  the cycle drifts by more than 20 cycles (`Spc.cpp:128-134`). Floating-point timing is
  unattractive for formal reasoning, but it is exactly the kind of invariant ("SPC never drifts
  more than N cycles") that could be stated and tested.

**Consequence.** A pure ISA spec (`state → state`) does not match the C++ structure, because in
the C++ a single instruction performs memory accesses that tick the rest of the machine. A
model has two options. It can be **bus-transaction level**: an instruction emits an ordered list
of (address, value, read/write) per M-cycle. Or it can be parameterised over a `tick` effect.
The SM83 SingleStepTests format is already bus-transaction level (§4.1).

### 1.4 Hooks that already exist and help verification

- **Dummy CPUs.** `GbCpu.cpp` is compiled a second time as `DummyGbCpu`, with `DUMMYCPU` defined
  (`Core/Gameboy/Debugger/DummyGbCpu.cpp:1-8`). In that build:
  - `ExecCpuCycle()` becomes a no-op, so no system ticking (`GbCpu.cpp:482-488`).
  - Reads go through `DebugRead`, and every access is logged into `_memOperations`
    (`GbCpu.cpp:527-546`, `DummyGbCpu.cpp:23-32`).
  - IRQ dispatch is compiled out (`GbCpu.cpp:54`).

  The same pattern exists for NES, SNES, SMS and GBA (`Core/NES/Debugger/DummyNesCpu.cpp:5`,
  `Core/SMS/Debugger/DummySmsCpu.cpp:4`, `Core/GBA/Debugger/DummyGbaCpu.cpp:4`,
  `Core/SNES/SnesCpu.h:3`). This is close to an "instruction → bus trace" function, which is
  what a differential harness needs. The main caveat: `GbCpu` holds a concrete
  `GbMemoryManager*` (`GbCpu.h:27`; `GbMemoryManager` is a concrete class,
  `GbMemoryManager.h:20`), not an interface. A standalone harness therefore needs either a real
  `Gameboy` instance with RAM poked through the debug API, or a link-time stub.
- **Externally settable state.** The interop layer exports `SetCpuState`, `GetCpuState`,
  `SetMemoryValues`, `GetMemoryState` and `Step`
  (`InteropDLL/DebugApiWrapper.cpp:84,186,196,242,252,272`). A harness can therefore drive the
  real, non-dummy CPU through the public API without changing any code.
- **State structs are POD.** For example `GbCpuState` (`Core/Gameboy/GbTypes.h:6-28`), which
  maps directly to a Lean `structure`.
- **Savestates.** Every component implements `Serialize(Serializer&)` using the `SV(...)` macro
  (`Utilities/Serializer.h:11`, e.g. `GbCpu.cpp:1561-1566`). Useful for snapshotting state
  around a divergence.
- **Existing tests.** CI checks out `nesdev-org/MesenTests` and runs `PGOHelper.exe <dir>
  citests` (`.github/workflows/tests.yml:42-49`). That calls `RunCiTests`, which replays
  recorded `.mtp` test-ROM runs and compares frame hashes (`InteropDLL/TestApiWrapper.cpp:79-104`,
  `Core/Shared/RecordedRomTest.h:18-23`). GBA tests are skipped because there is no BIOS
  (`TestApiWrapper.cpp:86-87`). I found no C++ unit-test framework (no gtest, Catch2 or doctest
  in `Core/`).

### 1.5 What makes this C++ hard to verify formally

- **Performance-driven structure.**
  - 376 `__forceinline` and 554 `template<` occurrences in `Core/` (grep counts).
  - Opcode dispatch through member-function-pointer tables (`NesCpu.h:27,35`,
    `NesCpu.cpp:67,179`).
  - Builds use `-O3` with ThinLTO and optional PGO (`makefile:66-74`, `makefile:13-22`).
- **Aliasing by design.** `Register16` holds raw pointers into `GbCpuState`
  (`GbTypes.h:53-74`, `GbCpu.h:22-25`). That is fine in C++, but it needs separation-logic-style
  reasoning in a verifier.
- **Cross-object mutation through raw back-pointers.** The CPU calls into the memory manager,
  which mutates the CPU's `CycleCount` by reference (`GbMemoryManager.cpp:91,101`). So there is
  no clean ownership boundary.
- **Conditional compilation.** The same source has two semantics (`DUMMYCPU`), so whatever is
  verified must be pinned to one build configuration.
- **Concurrency outside the C++ memory model.** See §2.2. A data race on a non-atomic object is
  undefined behaviour in C++ ([C++ draft \[intro.races\]/17](https://eel.is/c++draft/intro.races):
  *"Any such data race results in undefined behavior."*).

---

## 2. TLA+

### 2.1 What it is and is not good for

- **What it is for.** TLA+ is *"a high-level language for modeling programs and
  systems—especially concurrent and distributed ones,"* aimed at *"eliminating fundamental design
  errors"*. Its main tools are the TLC model checker and the TLAPS proof system
  ([Lamport, TLA+ home](https://lamport.azurewebsites.net/tla/tla.html)).
- **Apalache.** A symbolic model checker that *"translates TLA+ into the logic supported by SMT
  solvers such as Microsoft Z3."* It supports bounded model checking and inductive-invariant
  checking ([apalache-mc.org](https://apalache-mc.org/)).
- **Industrial use.** AWS reported using TLA+ since 2011 on critical distributed-systems designs
  ([Newcombe et al., CACM 58(4) 2015](https://cacmb4.acm.org/magazines/2015/4/184701-how-amazon-web-services-uses-formal-methods)).
- **Hardware precedent.** TLA+ was used to check the Alpha EV6/EV7 cache-coherence protocols
  ([Joshi, Lamport, Matthews, Tasiran, Tuttle, Yu, *Checking Cache-Coherence Protocols with TLA+*, FMSD 22, 2003](https://lamport.azurewebsites.net/pubs/fmsd.pdf)).
  That is a protocol, not an ISA.
- **Linking a spec back to running code.** Trace validation checks recorded implementation
  traces against a TLA+ spec using TLC
  ([Cirstea, Kuppe, Loillier, Merz, arXiv:2404.16075](https://arxiv.org/abs/2404.16075)).
- **What it is not good for [judgment].** Bit-level instruction semantics. TLC enumerates
  explicit states, so 8/16-bit registers plus RAM explode the state space immediately. TLA+ also
  has no native bitvector theory comparable to Lean's `BitVec`/`bv_decide`. The TLA+ home page
  does not say this explicitly; it follows from the design focus quoted above.
- **No prior art found.** I found no primary-source example of TLA+ applied to a video-game
  emulator or to CPU ISA semantics **[unverified: absence]**.

### 2.2 Where TLA+ fits in MesenCE

These are the concurrency protocols I found. Each one is small, hand-rolled and
correctness-critical, which makes each a good TLA+ target.

**(a) SDL audio ring buffer (best first TLA+ target).**

- **The two sides:**
  - `FillAudioBuffer` is SDL's audio callback, which calls `ReadFromBuffer`
    (`Sdl/SdlSoundManager.cpp:35-40, 134-149`).
  - `PlayBuffer` → `WriteToBuffer` runs on the emulation thread (`SdlSoundManager.cpp:151-182`).
- **The shared fields are plain integers.** `_readPosition` and `_writePosition` are non-atomic
  `uint32_t` (`Sdl/SdlSoundManager.h:43-44`). Both sides read the other side's index:
  `ReadFromBuffer` at `SdlSoundManager.cpp:146`, and `PlayBuffer`/`ProcessEndOfFrame` at
  `SdlSoundManager.cpp:175-177, 202`.
- **No lock is taken.** `SDL_LockAudioDevice` is never called. SDL documents that *"The audio
  callback runs in a separate thread in most cases"* and that you *"should protect data
  structures that it accesses"* with `SDL_LockAudioDevice`
  ([SDL2 wiki: SDL_OpenAudioDevice](https://wiki.libsdl.org/SDL2/SDL_OpenAudioDevice),
  [SDL_LockAudioDevice](https://wiki.libsdl.org/SDL2/SDL_LockAudioDevice)). Under the C++ memory
  model this is a data race, and therefore UB (§1.5).
- **No overrun check.** `WriteToBuffer` never checks whether the writer laps the reader
  (`SdlSoundManager.cpp:151-161`). Overruns are only handled indirectly, when average latency
  drifts by more than 50 ms and `Stop()` resets both indices (`SdlSoundManager.cpp:200-213,
  188-197`). `Stop()` writes both indices after `SDL_PauseAudioDevice`. Whether SDL2's pause
  waits for an in-flight callback to finish is **[unverified]**. I did not confirm it from SDL
  docs or source.
- **What to model.** Two processes (Producer, Consumer) over a bounded buffer of size B, with
  wrap-around arithmetic matching lines 136-160. Check two properties:
  - *Safety:* the consumer never reads bytes that were overwritten before being consumed, or
    else an explicit "overrun" flag is raised.
  - *Liveness:* under fairness, underrun eventually recovers.

  TLC handles B ≈ 4–8 slots with small `len` in seconds [judgment]. TLA+ assumes sequential
  consistency, so the model checks the *protocol logic*, not the memory-ordering bug. For the
  memory-ordering side, ThreadSanitizer (or a `std::atomic` with acquire/release) is the
  complementary tool.
- **Likely outcome [judgment].** TLA+ will confirm that the protocol allows lost or overwritten
  audio when the producer outruns the consumer. In practice that is an audible glitch, not a
  crash. The value is low-to-moderate, but it is a cheap, well-scoped learning exercise.

**(b) Emulator pause/lock handshake.** Involved code:

- `Emulator::Lock()` increments the atomic `_lockCounter`, then spins on `SimpleLock::Acquire`
  (`Core/Shared/Emulator.cpp:920-932`).
- The emu thread's `WaitForLock()` releases `_runLock`, sets `_threadPaused`, then spin-waits on
  `_lockCounter > 0` and on the debugger's break-request count (`Emulator.cpp:947-967`).
- `SimpleLock` is reentrant using a **non-atomic** `_lockCount` and `_holderThreadID`, which are
  read by other threads in `Acquire()` (`Utilities/SimpleLock.h:24-26`,
  `Utilities/SimpleLock.cpp:24-35`).

TLC can check mutual exclusion and deadlock-freedom over 2–3 threads, including the interaction
with the debugger.

**(c) Debugger break/step protocol.** Involved code:

- `DebugBreakHelper` loops: it sends `BreakRequest`, spins on `IsExecutionStopped()`, and steps
  if execution stopped mid-instruction (`Core/Debugger/DebugBreakHelper.h:13-41`).
- The emu thread's `SleepUntilResume` coordinates `_executionStopped`, `_breakRequestCount` and
  `_suspendRequestCount` (`Core/Debugger/Debugger.cpp:468-531`, `Debugger.h:82-84`).
- `IsExecutionStopped()` also ORs in `_emu->IsThreadPaused()` (`Debugger.cpp:761-763`), which
  couples it to protocol (b).

The code's own comments warn about deadlocks (`Emulator.cpp:910-916`, `Emulator.cpp:551-553`).
This is the **highest-value TLA+ target** [judgment]: several spin-waiting parties, counters
used as semaphores, and known deadlock hazards.

**(d) Netplay / movies / rewind determinism.**

- Netplay uses a small message protocol: HandShake, SaveState, InputData, MovieData, …
  (`Core/Netplay/MessageType.h:4-15`). The server forces the client's state with a savestate
  (`GameServerConnection.cpp:56`), then streams inputs.
- A TLA+ model of connect, savestate sync and input lockstep is feasible. It would check, for
  example, "a client never applies input for frame f before receiving the state for f".
- **What TLA+ cannot check here.** Determinism of the emulator itself, meaning
  `step(state, input)` is a pure function, is a property of the C++ core. TLA+ can only
  *assume* it. That is better tested empirically: record a movie, replay it twice, and compare
  savestate hashes per frame.

**(e) Inter-component timing.** Catch-up scheduling is single-threaded and deterministic, so
TLA+ adds little here. The interesting invariants are better expressed as runtime assertions or
Lean lemmas: "PPU never runs ahead of CPU master clock", "SPC drift ≤ 20 cycles"
(`Spc.cpp:128-134`).

---

## 3. Lean 4

### 3.1 Suitability for ISA semantics

- **Bitvectors.** Lean core provides `BitVec n` (*"A bitvector of the specified width"*).
- **`bv_decide`.** It *"takes the goal, hands it over to a SAT solver and verifies the generated
  LRAT UNSAT proof"*, using CaDiCaL. *"the external prover is not part of the trusted code base"*
  ([Lean ref: Bitvectors](https://lean-lang.org/doc/reference/latest/Basic-Types/Bitvectors/),
  [Lean API: BVDecide](https://lean-lang.org/doc/api/Lean/Elab/Tactic/BVDecide.html)).
  **Caveat:** it *"adds the result of the compiled check as an axiom, thus adding the Lean
  compiler to the trusted code base"* (same API page). `bv_check` can replay a stored LRAT file
  without a SAT solver (same page).
- **Fit for 8-bit CPUs [judgment].** For an 8/16-bit CPU like the SM83, flag lemmas are
  small SAT problems and should be quick for `bv_decide`. Examples: "half-carry from
  `(A ^ v ^ r) & 0x10` equals carry out of bit 3", the trick used in `GbCpu.cpp:720-731`, or DAA
  correctness.
- **Executable specs.** Lean compiles to C. The FFI provides `@[extern]` and `@[export]`, but
  the interface *"was designed for internal use in Lean and should be considered unstable"*, and
  structs cannot be passed by value
  ([Lean ref: FFI](https://lean-lang.org/doc/reference/latest/Run-Time-Code/Foreign-Function-Interface/)).
  Practical consequence: exchange state with C++ as `UInt8`/`UInt16`/`ByteArray` fields, or
  through a JSON/process boundary, not through shared structs.

### 3.2 Prior art in Lean

- **AWS LNSym.**
  - What it is: *"a symbolic simulator for Armv8 machine-code programs"* in Lean, with a
    formalization of AArch64 and proofs of native-code programs
    ([leanprover/LNSym](https://github.com/leanprover/LNSym)). Its `Proofs/` directory contains
    `AES-GCM`, `SHA512`, `Popcount32.lean` and others (GitHub API listing, 2026-11-04). The repo
    is not archived; last push was 2026-07-08.
  - How it is validated: by conformance testing, via the `cosim` target, which *"perform[s]
    conformance testing"* with random tests per instruction class and is skipped off-AArch64
    (LNSym README). **This is the closest analogue to what MesenCE would do:** an executable
    Lean ISA model validated by co-simulation.
  - Link to `bv_decide`: Lean 4.12's release notes name LNSym as a motivating workload for
    `bv_decide`. I saw this only via a search snippet, because the release-notes page redirects
    to an archive I could not fetch **[partially verified]**.
- **Sail → Lean.**
  - Sail generates *"theorem-prover definitions for interactive proof (in Isabelle, Rocq, and
    Lean)"*, and sail-riscv states *"An experimental emulator in the Lean language is
    available"* ([riscv/sail-riscv](https://github.com/riscv/sail-riscv)).
  - The Sail repo itself lists C/OCaml emulator generation and in-progress SystemVerilog, plus
    models for Arm-A, RISC-V, CHERI, x86 (from ACL2), MIPS and POWER
    ([rems-project/sail](https://github.com/rems-project/sail)). No Sail model exists for 6502,
    SM83 or Z80 **[unverified: absence]**.
  - Downstream use: Succinct's sp1-lean takes *"The Sail model … as the ground-truth definition of
    RISC-V"* and proves zkVM constraints against it
    ([reservoir: sp1-lean](https://reservoir.lean-lang.org/@succinctlabs/sp1-lean)). That shows
    Sail-generated Lean is usable as a proof target.
- **Lean models of 6502/SM83/Z80.** I found none from a primary source **[unverified: absence]**.

### 3.3 Non-Lean prior art (for positioning)

| Project | What it is | Relevance |
|---|---|---|
| ACL2 x86isa (Goel, Hunt, Kaufmann) | Executable formal x86 ISA model that can *simulate* and *reason about* machine code. Validated *"by co-simulating it regularly against a physical x86 machine"* ([ACL2 manual: X86ISA](https://www.cs.utexas.edu/users/moore/acl2/manuals/current/manual/index-seo.php/X86ISA____INTRODUCTION), [books/projects/x86isa](https://github.com/acl2/acl2/tree/master/books/projects/x86isa)) | The co-simulation playbook to copy |
| Sail | ISA DSL that generates emulators and prover definitions ([rems-project/sail](https://github.com/rems-project/sail)) | Could host an SM83/6502 spec, emitting both C (to diff against MesenCE) and Lean |
| CompCert | Formally verified C compiler; Rocq proofs; ARM/PowerPC/RISC-V/x86 targets ([compcert.org](https://compcert.org/)) | Shows what full verification costs. Not applicable to C++ |
| CakeML / Silver | Verified ML compiler ([cakeml.org](https://cakeml.org/)). *Verified compilation on a verified processor*, PLDI 2019 ([Lööw et al.](https://researchr.org/publication/LoowKTMNAF19)) | Verified CPU + compiler stack in HOL4. Shows the scale of end-to-end efforts |
| Kami | Coq/Rocq platform for *"High-Level Parametric Hardware Specification and its Modular Verification"* ([mit-plv/kami](https://github.com/mit-plv/kami)) | Hardware-design refinement. Overkill for an emulator |
| HOL4 ARM6 | Fox verified an ARM6 model against its ISA. The memoir says it took about a year ([Gordon memoir, arXiv:1806.04002](https://arxiv.org/pdf/1806.04002)) | Gives a sense of effort |

---

## 4. Connecting a formal model to the C++

### 4.1 Differential testing / co-simulation (recommended)

- **Test data.** SingleStepTests publishes per-instruction JSON for SM83, Z80, 65x02, SPC700,
  65816, ARM7TDMI ("Experimental"), HuC6280 ("Mesen-generated") and others
  ([SingleStepTests org](https://github.com/orgs/SingleStepTests/repositories)). The SM83 set
  has *"1000 tests"* per opcode (00–FF and CB 00–CB FF). Each test has `initial`/`final`
  registers and RAM, plus `cycles`: *"a list of processor bus states, sampled BETWEEN
  M-STATES"* ([SingleStepTests/sm83](https://github.com/SingleStepTests/sm83)).
- **The pipeline:**
  1. Write the SM83 spec in Lean as `step : CpuState → Mem → CpuState × List BusOp`.
  2. Run it on the JSON suite inside Lean. This validates the *spec*.
  3. Run MesenCE's `GbCpu`/`DummyGbCpu` on the same suite. This validates the *implementation*.
  4. Run both on random states and diff them. This catches spec/implementation disagreement
     outside the suite.
- **Two-oracle advantage.** With two independent oracles (hardware-derived JSON and a Lean
  spec), a three-way disagreement tells you which party is wrong. Note that the SM83 tests are
  *"JSMoo-based"*, i.e. emulator-generated, not hardware-captured. They are still independent of
  MesenCE.
- **Cost [judgment].** About 1–3 person-weeks for one 8-bit CPU, mostly writing the Lean spec
  and a C++ shim that loads JSON into `GbCpuState` and RAM.
- **Effectiveness.** This is the approach ACL2 x86isa and LNSym rely on (§3.2, §3.3).

### 4.2 Trace validation for TLA+ models

Instrument the audio and debugger protocols to log variable updates, then check the logs
against the spec with TLC, following Cirstea et al.
([arXiv:2404.16075](https://arxiv.org/abs/2404.16075)). Their API targets Java, so a C++
logger would need to be written [judgment].

### 4.3 Refinement / translation validation (proof that C++ implements spec)

- I found no primary-source tool that translates optimized C++17 into Lean.
- The general approach of proving an imperative implementation refines an executable spec is
  established in HOL4 and Rocq work (§3.3), but each case is a multi-person-year research effort.
- The practical variant is to **rewrite** the chosen unit in Lean and call it from C++ via FFI.
  That gives a verified component, but it replaces MesenCE code rather than verifying it, and
  costs performance on a hot path. Lean-compiled code speed versus `-O3` C++ is **[unverified]**.
  I found no primary benchmark.

### 4.4 C/C++ verification tools on the existing code

| Tool | C++ support (primary source) | Use here |
|---|---|---|
| CBMC | *"Bounded Model Checker for C and C++ programs"*; checks memory safety, UB variants and assertions by loop unwinding ([cprover.org/cbmc](https://www.cprover.org/cbmc/)). The site does not detail C++ feature coverage. | Bounded check of `GbTimer`, `AluMulDiv` or a single opcode handler, after extracting it into a harness. Templates and `__forceinline` may need stubbing **[unverified]** |
| ESBMC | Clang front end for C/C++. Checks overflow, bounds, division by zero and, for pthreads, data races and deadlocks ([esbmc/esbmc](https://github.com/esbmc/esbmc)) | Same as CBMC. Its concurrency checks target pthreads, not `std::atomic` spin-loops **[unverified for this code]** |
| Frama-C | C only. Frama-Clang (C++ plugin) status **[unverified]**: the plugin page returned 404 | Not a fit |
| ThreadSanitizer | Dynamic race detector (manual page could not be fetched; **[unverified]** here) | Cheapest way to *confirm* the §2.2(a) race at runtime |

**Assessment [judgment].** A bounded model checker on one extracted pure function, for example
"for all A and v, `ADD` flag results match the spec", gives the same guarantee as a
`bv_decide` proof about the *C++ itself*, not about a Lean rewrite. That is the cheapest true
code-level guarantee available. Doing this requires untangling the function from
`ExecCpuCycle()` side effects; the `DUMMYCPU` build helps here.

---

## 5. Recommended phased plan

The effort figures are rough single-engineer estimates **[judgment]**.

| Phase | Work | Effort | Value |
|---|---|---|---|
| **0. Cheap baselines** | Build with `-fsanitize=thread`, run a game with SDL audio and open/close the debugger repeatedly. Add a determinism check: replay a movie twice and compare per-frame savestate hashes. | 2–4 days | Confirms or refutes the §2.2 races empirically. The determinism check guards netplay and rewind. |
| **1. TLA+ pilot: audio ring** | PlusCal/TLA+ model of `ReadFromBuffer`/`WriteToBuffer`/`Stop` with TLC. | 3–5 days | Learning vehicle, and documents the overrun behaviour. Low product risk either way. |
| **2. TLA+: emu lock + debugger break** | Model `Lock/Unlock/WaitForLock`, `SimpleLock`, `BreakRequest`, `SleepUntilResume` and `DebugBreakHelper` with 2–3 threads. Check mutual exclusion and deadlock-freedom. | 2–3 weeks | **Highest TLA+ value.** Deadlocks here hang the UI and are hard to reproduce. |
| **3. Lean pilot: SM83 spec + differential test** | Lean `BitVec` spec of all 512 SM83 opcodes at M-cycle bus granularity. Validate against SingleStepTests/sm83 in Lean. Add a C++ shim driving `DummyGbCpu` (or the real CPU through debug APIs) on the same JSON and on Lean-generated random states, then diff. | 3–6 weeks | **Highest Lean value.** Finds real flag/edge-case divergences, gives a reusable oracle, and is publishable. |
| **4. Lean proofs on the spec** | `bv_decide` lemmas: DAA, half-carry, `ADD SP,e8` flags; instruction-length/PC invariants; "no opcode writes outside its declared bus ops". | 2–4 weeks | Raises trust in the spec itself. Does not touch the C++. |
| **5. Optional: CBMC/ESBMC on extracted C++** | Prove that `GbCpu` ALU helpers (`ADD`, `ADC`, `SUB`, `SBC`, `DAA`; `GbCpu.cpp:720-1167`) equal reference formulas for all inputs. | 1–3 weeks | The only *code-level* proof in this plan. |
| **6. Optional: extend** | NES 6502 (`nes6502` tests), SPC700, Z80. GB timer as a Lean/TLA+ state machine. | 2–6 weeks each | Diminishing returns. |
| **Not recommended** | Refinement proof of C++ to Lean, whole-system models, PPU/APU formalization | person-years | Cost far exceeds what test ROMs and co-simulation already give. |

### What verification would catch

- Opcode semantic and flag bugs, and bus-ordering bugs in the modelled CPU (Phases 3–5).
- Lost updates, deadlocks and missing wake-ups in the thread protocols (Phases 1–2).
- Netplay or movie protocol ordering errors, if modelled (§2.2(d)).

### What it would not catch

- Errors shared by the spec and the hardware documentation the spec author relied on. A formal
  spec is only as correct as its author's understanding. SingleStepTests mitigates this only
  where the test data is hardware-derived; the SM83 and HuC6280 sets are emulator-generated.
- Cross-component timing (PPU mid-scanline effects, DMA/IRQ interplay, SPC drift), mapper and
  coprocessor quirks, audio/video output. These are the dominant bug classes in
  accuracy-focused emulators [judgment], and they are already targeted by the test-ROM CI
  (`.github/workflows/tests.yml:42-49`).
- Compiler, LTO or PGO miscompilation, and UB-driven optimisation in the C++ (Phases 3–4 test
  the binary, but only on sampled inputs).
- Memory-model bugs, under TLA+'s default sequential-consistency semantics.

### Frank value-versus-cost summary [judgment]

- **Phases 0–3 (about 2–3 months total) give real, demonstrable value:**
  - a verified-against-tests executable SM83 spec;
  - a differential harness that keeps paying off on regressions;
  - checked models of the debugger/pause handshake.
- **Beyond that, value per unit of effort drops sharply.** MesenCE's accuracy already comes
  from extensive test-ROM coverage. The remaining bugs are mostly in timing interactions that
  neither tool models cheaply.
- **On choosing the pilot CPU.** The SM83 is the right first target because three things exist
  together: a small ISA, a JSON test suite with bus traces, and a `DUMMYCPU` seam. The NES 6502
  is the runner-up. Section 6 makes the case that the NES DMA scheduler is a better TLA+ target
  than either CPU's ISA.

---

## 6. NES CPU (2A03): Lean for the ISA, TLA+ for DMA and interrupts

*Added 2026-11-04.* This section revisits the NES CPU as a target. The ISA is a reasonable Lean
pilot. The higher-value formal target is the per-cycle **DMA scheduler** around it, which is a
small state machine driven by external events and suits TLA+ model checking. A first TLA+ model
of it exists in [`tla/NesDmaScheduler.tla`](tla/NesDmaScheduler.tla), and TLC has already been
run on it. The results are in §6.3.

### 6.1 The ISA in Lean

- **Size.** `Core/NES/NesCpu.cpp` is 612 lines and `Core/NES/NesCpu.h` is 914, about the same
  as the SM83 (§1.2).
- **No decimal mode.** The 2A03 lacks the 6502's BCD mode. MesenCE's `ADD` ignores the D flag
  (`Core/NES/NesCpu.h:313-326`), and `SBC` is `ADD(operand ^ 0xFF)` (`NesCpu.h:329`). There is no
  analogue of the SM83's DAA, which is the hardest instruction to specify there. Lemmas about the
  carry, overflow, zero and negative flags are exactly what `bv_decide` handles.
- **The same differential seams as the SM83.**
  - `DummyNesCpu` re-includes the real CPU source under `DUMMYCPU`
    (`Core/NES/Debugger/DummyNesCpu.cpp`). Under that macro, `MemoryRead`/`MemoryWrite` only log
    the bus operation (`NesCpu.cpp:245-270`).
  - SingleStepTests/ProcessorTests publishes `nes6502` JSON with per-cycle bus activity (§4.1).
    Its provenance (hardware-captured or emulator-generated) is **[unverified]**.
- **Cycle-exact bus traces are the costly part.** A bus-trace-accurate model has to include the
  6502's dummy reads and writes: page-cross reads, the double write in read-modify-write
  instructions, and dummy PC reads in implied mode (`FetchOperand`, `NesCpu.cpp:272-293`).
  That is more work than modelling just the end result of each instruction, but it is also
  where divergences tend to be.
- **Unofficial opcodes.**
  - The stable ones (LAX, SAX, DCP, ISC, …) are straightforward.
  - The unstable ones are not well defined. `ANE` uses `(A | 0xEE) & X & imm`
    (`NesCpu.h:847`), a constant that varies by chip. The `SHX`/`SHY`/`SHA`/`TAS` family
    depends on bus conflicts (`NesCpu.h:803-836`).
  - Leave the unstable opcodes out of proofs, or make the magic constant a model parameter.
- **Effort [judgment].** 3–5 weeks for official opcodes, flag lemmas and a differential harness.
  That is a little less than the SM83, because there is no DAA, but the cycle-exact dummy
  accesses add work.

### 6.2 Why the DMA and interrupt scheduler is the better target

Every CPU read first runs `ProcessPendingDma`, then `StartCpuCycle`/`EndCpuCycle`
(`NesCpu.cpp:258-270`). Those advance the PPU, clock the APU and mapper
(`_console->ProcessCpuClock()`), and sample IRQ/NMI with a one-cycle pipeline
(`_prevRunIrq`/`_runIrq`, `NesCpu.cpp:298-319`). `ProcessPendingDma` (`NesCpu.cpp:329-430`)
interleaves two DMA engines on one bus:

- **OAM DMA** is requested by a `$4014` write (`RunDMATransfer`, `NesCpu.cpp:511`).
- **DMC DMA** is requested and cancelled by the APU (`StartDmcTransfer`/`StopDmcTransfer`,
  `NesCpu.cpp:518-540`). The cancellation comes through `DeltaModulationChannel::ProcessClock`,
  which runs *inside* `StartCpuCycle` (`DeltaModulationChannel.cpp:278-292`).
- **The engines are coordinated by flags:** `_needHalt`, `_needDummyRead` and `_abortDmc`, plus
  get/put cycle parity.
- **There is a re-entrant path:** after a DMC read, a new request recurses through
  `NoInlineProcessPendingDma` (`NesCpu.cpp:395-400`).

This is a finite state machine whose inputs (APU requests and cancellations) can arrive on any
cycle. That is the case where exhaustive model checking beats tests, which cover only the timings
someone thought to write. MesenTests has many test ROMs aimed at exactly this area
(`NES/CPU/sprdma_and_dmc_dma`, `dmc_dma_during_read4`, `dma_sync_test`, `cpu_interrupts_v2`,
`shxdma`, …). They all passed in a local run on 2026-11-04 (612 passed, 0 failed, 218 skipped,
via `mise run test:ci`).

### 6.3 The TLA+ model and first TLC results

**What it models.** [`tla/NesDmaScheduler.tla`](tla/NesDmaScheduler.tla) is a model of the
**C++ code**, not of the hardware. Each action cites the lines it mirrors.

- **Granularity:** one step is one CPU cycle.
- **Abstracted away:** bus values.
- **Kept:** the scheduling flags, get/put parity, the local `spriteDmaCounter`, and the kind of
  each cycle (halt, dummy, align, dmcRead, oamRead, oamWrite, cpuRead, cpuWrite).
- **The environment** (the APU and the running program) may do the following, up to
  `MaxEnvEvents` times per behaviour:
  - request or cancel a DMC DMA on any cycle;
  - start an OAM DMA on a CPU write cycle.
- **CPU constraints:**
  - CPU writes are never halted, matching `MemoryWrite`.
  - At most 3 consecutive writes occur.
  - On PAL, DMA may halt only an opcode fetch (`NesCpu.cpp:335-339`).

**Properties checked:**

| Property | Meaning |
|---|---|
| `DummyReadJustified` | The C++ `assert(_needHalt \|\| _needDummyRead)` in the dummy-read branch (`NesCpu.cpp:410`) always holds |
| `OamWritesExact` | Every OAM DMA writes exactly `OamBytes` bytes to `$2004` |
| `OamOnlyLength` | An OAM DMA with no DMC activity takes `2·OamBytes+1` or `+2` cycles (513/514 for 256 bytes, NESdev wiki) |
| `DmcOnlyLength` | A single completed DMC DMA steals 3 or 4 cycles (halt, dummy, optional align, get; NESdev wiki) |
| `DmcFinishes`, `OamFinishes`, `CpuResumes` | Liveness: pending DMA always completes, and the CPU always gets the bus back |

**Results.** TLC 1.7.4 was run with `OamBytes = 2` and `MaxEnvEvents = 3`, a few thousand
distinct states per configuration:

| Config | Result |
|---|---|
| `MC_NTSC.cfg` (default settings) | **All properties hold** |
| `MC_PAL.cfg` | **All properties hold** |
| `MC_NTSC_glitch.cfg` (`EnableDmcSampleDuplicationGlitch` on) | **`OamWritesExact` violated** — see finding 1 |
| `MC_NTSC_glitch_fixed.cfg` (glitch on, `OamProgressPersists = TRUE`, the proposed fix) | **All properties hold** |
| `MC_NTSC_rmw4014.cfg` (`$4014` also written by RMW instructions) | **All properties hold** — see the retracted finding 2 |

**Finding 1: the nested DMA call restarts OAM DMA from byte 0.**

- **The code.** `spriteDmaCounter` and `spriteReadAddr` are *locals* of `ProcessPendingDma`
  (`NesCpu.cpp:362-363`). When a DMC read during an OAM DMA is followed immediately by a new DMC
  request, the code recurses (`NesCpu.cpp:398-399`). The inner call starts both at 0 and runs the
  OAM DMA to completion from byte 0. The outer loop then exits.
- **TLC's counterexample:** OAM byte 0 is copied, a DMC read happens with duplication, there is
  a nested halt, and OAM bytes 0 and 1 are copied again. That is 3 `$2004` writes for a 2-byte
  transfer.
- **The consequence:** on hardware this would write 256 + *k* bytes, with OAM shifted by *k*,
  where *k* bytes were copied before the DMC fetch.
- **Reachability in the model.**
  - The new request comes from `SetDmcReadBuffer`'s duplication path
    (`DeltaModulationChannel.cpp:96-110`).
  - That path only runs with `EnableDmcSampleDuplicationGlitch` on, which is off by default
    (`SettingTypes.h:733`).
  - The model lets that request happen after *any* DMC read. That over-approximates the real
    condition, which is that the DMC DMA ends on the exact cycle the output unit reloads its bit
    counter (`_bitsRemaining == 8 && timer == period`).
- **Checked in the emulator (2026-11-04).** I made a throwaway build with three `stderr` probes:
  - when the duplication path fires, and whether OAM DMA is in progress at the time;
  - when a nested `ProcessPendingDma` starts during OAM DMA;
  - when an OAM DMA does not write exactly 256 bytes to `$2004`.

  I ran the MesenTests suite with the glitch forced on, and again with default settings.

  | Run | Duplication path fired | …during OAM DMA | Nested call during OAM DMA | OAM DMA ≠ 256 writes | Suite |
  |---|---|---|---|---|---|
  | Glitch on | 7 | 0 | 0 | 0 | 612 pass / 0 fail |
  | Default | — | — | 0 | 0 | 612 pass / 0 fail |

- **Why the suite never hits it.**
  - A normal DMC request is raised *at* an output-unit reload. At the fastest NTSC rate,
    reloads are at least 54 × 8 = 432 CPU cycles apart.
  - During an OAM DMA, the DMC fetch is serviced within a few cycles, because it takes the next
    get cycle ahead of OAM.
  - So a reload-aligned request cannot end on the *next* reload. Only an *unaligned* request
    can, meaning one started by a `$4015` enable.
- **The sequence that reaches it.**
  1. `STA $4015` (enable) raises a DMC request 2–3 cycles later.
  2. With `STX $4014` straight after, a request raised during `STX`'s third cycle (an operand
     read) is only serviced at the next read. That read follows the `$4014` write, so DMC DMA
     and OAM DMA start at the same halt.
  3. Inside the combined DMA, OAM does one read and one write. The DMC fetch comes next.
  4. Because the request was unaligned, some timer phase makes that fetch end exactly on a
     reload. That triggers the duplication request while the OAM DMA is still running.
- **Reproduced (2026-11-04).**
  - [`tla/rom/dmc_oam_overlap.s`](tla/rom/dmc_oam_overlap.s) is a 60-line NROM test ROM, built
    with cc65 using `tla/rom/nrom.cfg`. It sweeps the delay before that `STA $4015`/`STX $4014`
    pair.
  - It was run for 5 s at maximum speed (299 frames) through `PGOHelper`'s ROM mode, with the
    glitch forced on:

  | Build | Duplication fired during OAM DMA | Nested call during OAM DMA | OAM DMAs with ≠ 256 `$2004` writes |
  |---|---|---|---|
  | `4b8a196e`, glitch on | 29 | 29 (each after exactly 1 OAM byte) | **29, every one 257 writes** |
  | `4b8a196e`, default | 0 | 0 | 0 |
  | With the hardening below, glitch on | 56 | 56 | **0** |

  Each failure is the k = 1 case the TLC trace predicted: OAM byte 0 is written, the transfer
  restarts, and 256 more bytes are written. OAM ends up shifted by one entry.
- **Status.** A **confirmed emulator bug**, but only with the opt-in "DMC sample duplication
  glitch (late-G & H CPU behavior)" option on. It needs a `$4015` enable immediately followed by
  a `$4014` write at an unlucky timer phase. Real hardware keeps OAM DMA progress in the DMA
  unit, so it does not restart the transfer **[judgment, from NESdev's description of a single
  DMA unit]**.

**Finding 2 (retracted): `StopDmcTransfer` cancelling a pending OAM DMA's halt.**

- **The concern.** If a DMC DMA is cancelled before its halt cycle, `StopDmcTransfer` clears
  `_needHalt` (`NesCpu.cpp:525-540`), even if an OAM DMA is also pending.
- **Why it doesn't happen.**
  - The first model let *any* CPU write follow the `$4014` write, and TLC found the stall that
    way.
  - On a real 6502 only two cases exist. `STA/STX/STY $4014` write on their last cycle, so an
    opcode fetch follows. Read-modify-write instructions (`INC $4014`, …) follow their dummy
    write with a final write *to `$4014` again*. That calls `RunDMATransfer` and re-arms
    `_needHalt` after the APU has run in that cycle.
  - With the model corrected (`RmwFinalWrite`, constant `RmwOn4014`), `MC_NTSC_rmw4014.cfg`
    passes every property.
- **Lesson.** This counterexample came from an environment that was too permissive. That is why
  every model counterexample needs checking against the code and the CPU before it is reported
  as a bug.

**Running it.**

```sh
cd docs/research/tla
tlc -config MC_NTSC.cfg -workers auto NesDmaScheduler.tla
```

`tlc` comes from `pkgs.tlaplus` in the dev shell. `mise run tla` runs all four configs.

### 6.4 Next steps

1. **Upstream finding 1.** File an issue with the probe ROM and the 257-write evidence, and
   propose the fix below. Under `CONTRIBUTING.md`, the patch itself has to be written by a
   human contributor.
2. **Trace validation (§4.2).** Have a debug build log, each cycle, the
   `{kind, needHalt, needDummyRead, dmcRunning, abortDmc, spriteDma, parity}` tuple, then check
   recorded runs of the MesenTests DMA ROMs against the spec with TLC. This turns "the model
   mirrors the code" from a claim made by reading into a checked one.
3. **Extend the model.**
   - Add the IRQ/NMI pipeline: `_prevRunIrq`/`_runIrq`, NMI hijacking `BRK`
     (`NesCpu.cpp:223-238`), and the `_prevNeedNmi` reset after `BRK`.
   - Add the `$4015`/`$4016` double-read side effects as observable events.
   - Add a hardware-level spec from the NESdev wiki DMA page and check that the code-level model
     refines it. That would separate "the code does what we think" from "what we think matches
     the hardware".
4. **Fix for finding 1. Validated in a throwaway worktree; not applied to this repo.**
   - **The change.** Move `spriteDmaCounter`/`spriteReadAddr` from locals of
     `ProcessPendingDma` into `NesCpu` members (`_spriteDmaCounter`, `_spriteReadAddr`), and
     reset them in `RunDMATransfer`. A nested call then continues the transfer instead of
     restarting it.
   - **Evidence, 2026-11-04:**
     - The probe ROM shows 0 bad OAM DMAs, from 56 nested calls.
     - MesenTests: 612 pass / 0 fail, both with the glitch forced on and with default settings.
     - TLC: `MC_NTSC_glitch_fixed.cfg` passes every property.
   - **Not needed:** a save-state change. `ProcessPendingDma` always runs a whole DMA within one
     CPU read, so the members never hold live state at a frame boundary **[judgment]**.

**The instrumentation used for finding 1** is three `stderr` probes, about 15 lines. It lived in
a throwaway git worktree and is not in the repo. It is easy to recreate:

- in `NesCpu.cpp`, after `SetDmcReadBuffer`, log `_needHalt && _spriteDmaTransfer`;
- in the OAM write branch, count writes and log any count other than 256 at completion;
- in `DeltaModulationChannel.cpp`, at the duplication `StartDmcTransfer`, log
  `_spriteDmaTransfer`;
- force the glitch on in `RecordedRomTest::UpdateSettings` via an environment variable.

---

## Sources

**Repo (MesenCE @ 4b8a196e)**
- `Core/Gameboy/GbCpu.{h,cpp}`, `GbTypes.h`, `GbMemoryManager.{h,cpp}`, `GbTimer.cpp`
- `Core/Gameboy/Debugger/DummyGbCpu.cpp`
- `Core/NES/NesCpu.{h,cpp}`, `NesConsole.cpp`, `Core/SNES/Spc.{h,cpp}`
- `Core/NES/APU/DeltaModulationChannel.cpp`, `Core/NES/Debugger/DummyNesCpu.cpp`,
  `Core/Shared/SettingTypes.h`
- `docs/research/tla/NesDmaScheduler.tla` and `MC_*.cfg` (model written for this report)
- `Sdl/SdlSoundManager.{h,cpp}`
- `Core/Shared/Emulator.{h,cpp}`, `Utilities/SimpleLock.{h,cpp}`
- `Core/Debugger/Debugger.{h,cpp}`, `DebugBreakHelper.h`
- `Core/Netplay/*`, `Utilities/Serializer.h`
- `InteropDLL/DebugApiWrapper.cpp`, `TestApiWrapper.cpp`
- `.github/workflows/tests.yml`, `makefile`

**External**
- TLA+ home — https://lamport.azurewebsites.net/tla/tla.html
- Apalache — https://apalache-mc.org/
- Joshi et al., Checking Cache-Coherence Protocols with TLA+ — https://lamport.azurewebsites.net/pubs/fmsd.pdf
- Newcombe et al., How AWS Uses Formal Methods — https://cacmb4.acm.org/magazines/2015/4/184701-how-amazon-web-services-uses-formal-methods
- Cirstea et al., Validating Traces … Against TLA+ — https://arxiv.org/abs/2404.16075
- Lean BitVec reference — https://lean-lang.org/doc/reference/latest/Basic-Types/Bitvectors/
- Lean BVDecide API docs — https://lean-lang.org/doc/api/Lean/Elab/Tactic/BVDecide.html
- Lean FFI reference — https://lean-lang.org/doc/reference/latest/Run-Time-Code/Foreign-Function-Interface/
- LNSym — https://github.com/leanprover/LNSym
- Sail — https://github.com/rems-project/sail
- sail-riscv — https://github.com/riscv/sail-riscv
- sp1-lean — https://reservoir.lean-lang.org/@succinctlabs/sp1-lean
- ACL2 x86isa — https://www.cs.utexas.edu/users/moore/acl2/manuals/current/manual/index-seo.php/X86ISA____INTRODUCTION
- CompCert — https://compcert.org/
- CakeML — https://cakeml.org/
- Lööw et al., PLDI 2019 — https://researchr.org/publication/LoowKTMNAF19
- Kami — https://github.com/mit-plv/kami
- Gordon memoir (ARM6/HOL4) — https://arxiv.org/pdf/1806.04002
- NESdev wiki, DMA (OAM DMA 513/514 cycles; DMC DMA halt/dummy/align/get) — https://www.nesdev.org/wiki/DMA
- SingleStepTests — https://github.com/orgs/SingleStepTests/repositories
- SingleStepTests/sm83 — https://github.com/SingleStepTests/sm83
- ProcessorTests — https://github.com/SingleStepTests/ProcessorTests
- CBMC — https://www.cprover.org/cbmc/
- ESBMC — https://github.com/esbmc/esbmc
- SDL2 SDL_OpenAudioDevice — https://wiki.libsdl.org/SDL2/SDL_OpenAudioDevice
- SDL2 SDL_LockAudioDevice — https://wiki.libsdl.org/SDL2/SDL_LockAudioDevice
- C++ draft [intro.races] — https://eel.is/c++draft/intro.races
