# Mega Drive / Genesis support: feasibility and implementation research

Snapshot: repository commit `2be9fe7b`, 2026-11-16.

## Decision

The recommendation in this section is an engineering judgment based on the inspected source and
evidence, not a completed implementation or hardware qualification.

Adding base Sega Mega Drive / Genesis support to MesenCE is feasible and is a good architectural
fit for the project, but it is a new console core rather than a two-processor plug-in. MesenCE
already contains most of the reusable infrastructure and two unusually valuable chip-level
assets:

- the SMS core's Z80 implementation, backed by the repository's 1.6 million-case
  `test:z80-cpu` task;
- the already-vendored, BSD-licensed `ymfm` implementation of the YM2612 and YM3438.

The recommended direction is a native `Core/MD` console integrated with MesenCE's scheduler,
debugger, serializer, trace hooks, and native headless build. Use a bus-visible 68000 interpreter
ported from the ISC-licensed ares component as the starting point, subject to a bounded spike
against the M68000 manual and the SingleStepTests suite. Extract the existing Z80 execution engine
behind system-specific bus and debugger adapters instead of keeping two diverging Z80s. Implement
the Mega Drive bus, VDP, arbitration, cartridge, I/O, and timing as MesenCE components. Reuse
`ymfm::ym2612` and generalize or adapt the SMS PSG where its behavior actually matches.

Do not import Genesis Plus GX: its core is explicitly non-commercial. Do not embed a complete
BlastEm or ares console behind `IConsole`: that would provide quick boot coverage, but leave two
state, debugger, scheduler, and trace architectures inside one binary and would not achieve the
consolidation sought by MesenCE and nativekit.

The first deliverable should be a deterministic native headless runner shared by every MesenCE
console, followed by CPU and system milestones. This makes the consolidation useful before the
Mega Drive implementation is complete and turns nativekit's current one-off NES Mesen host into a
supported repository capability.

## Scope and success criteria

The proposed first release is the base cartridge console:

- NTSC-J, NTSC-U, and PAL timing;
- Motorola 68000, Z80, 64 KiB work RAM, and 8 KiB Z80 RAM;
- VDP modes used by Mega Drive software, including both display widths, planes, window, sprites,
  scrolling, priorities, interlace, shadow/highlight, H/V counters, FIFO behavior, DMA, and H/V
  interrupts;
- YM2612/compatible YM3438 behavior and SN76489-compatible PSG output;
- 3- and 6-button controllers;
- ordinary ROM cartridges and the common battery-RAM header convention;
- save states, deterministic replay, debugger/trace support, Lua access, screenshots, WAV output,
  and bounded headless execution on x86-64 and arm64 Linux/macOS.

Sega CD, 32X, SVP, Master System compatibility mode, modem, unusual peripherals, lock-on
cartridges, EEPROM variants, and uncommon mapper/protection hardware should be separate follow-on
milestones. Their processors and synchronization substantially expand the state space; ares, for
example, models Mega CD and 32X as additional components rather than incidental cartridge cases.

Success is not “a title screen appeared.” The first support claim should require:

1. CPU instruction and bus-trace tests with documented exceptions;
2. synthetic system ROMs covering arbitration, interrupts, VDP transfers, DMA, controller timing,
   and both audio chips;
3. save-state replay equality and recorded-test determinism;
4. cross-reference runs against at least two independent implementations where possible;
5. a cloud/headless artifact contract that nativekit can consume without a UI, SDL device, or
   desktop automation;
6. representative commercial-title qualification with evidence recorded separately from the
   synthetic tests.

## Why this is more than adding two CPUs

Sega's own hardware summary identifies a 68000 main CPU at 7.67 MHz, a Z80A at 3.58 MHz, 64 KiB
of 68000 RAM, 8 KiB of Z80 RAM, and 64 KiB of VRAM. The official software manual additionally
defines the VDP ports, Z80/system I/O area, region/version register, RAM and cartridge layout,
interrupt rules, DMA, controller behavior, PAL differences, and bus hazards. In particular, the
manual says the Z80 can enter the 68000 bus and documents a failure case involving adjacent
`$A100xx` access. It also states that `TAS` performs the test but does not complete the set on the
console. These are system/bus rules, not properties that a standalone CPU interpreter supplies.

The components needed for useful decompilation support are therefore:

| Area | Required behavior | Current MesenCE leverage |
|---|---|---|
| 68000 | Instructions, exceptions, prefetch, UDS/LDS byte lanes, address errors, IRQ levels, per-access timing | New processor; existing CPU/debugger patterns |
| Z80 | NMOS behavior, interrupts, bank window, reset and bus-request stalls | Existing verified `SmsCpu`, but currently coupled to `SmsConsole` and `SmsMemoryManager` |
| Scheduler | One master-clock domain, CPU ratios, refresh, VDP stalls, FIFO/DMA stealing, safe synchronization | Per-console frame loops and timing interfaces exist; MD scheduler is new |
| 68000 bus | Cartridge, work-RAM mirrors, I/O, Z80 window, bus control, VDP ports, open-bus behavior | Memory registration, cheats, typed debug hooks, serialization |
| Z80 bus | 8 KiB RAM, YM2612, bank register, PSG/VDP access, 68000 window | New MD adapter around a shared Z80 engine |
| VDP | VRAM/CRAM/VSRAM, planes/window/sprites, counters, FIFO, DMA, interrupts, rendering | Video/filter pipeline is reusable; SMS VDP is not a drop-in Genesis VDP |
| Sound | YM2612 timers/status/DAC/FM and PSG, clocked mixing and resampling | `ymfm::ym2612` is already present; `SmsPsg` is a candidate for extraction |
| Cartridge/I/O | Header/region, SRAM, controllers and TH timing, TMSS option | Common battery, input, ROM and configuration infrastructure |
| Tooling | disassembly, stepping, breakpoints, CDL, labels, Lua, trace, save/replay | Shared debugger is reusable but needs two new CPU identities and MD memory types |
| Headless | deterministic input, bounded run, images/audio/state/trace plus provenance | Native core and test patterns exist; no first-class multi-console CLI yet |

The VDP and inter-processor bus are the highest-risk areas. A CPU-only milestone is valuable, but
it should not be mistaken for usable Genesis support.

## Current MesenCE fit

### Console boundary

[`IConsole`](../../Core/Shared/Interfaces/IConsole.h) already defines the required system boundary:
ROM loading, reset, frame execution, timing, CPU identities, memory translation, video, audio, and
serialization. [`Emulator::TryLoadRom`](../../Core/Shared/Emulator.cpp) selects a console by
extension/signature and then initializes the same debugger, rewind, movie, cheat, save-state,
audio, and video services for every implementation. A `MdConsole` can follow the NES, SNES, PCE,
and SMS layouts without introducing a second front-end API.

Required shared-enum additions include `ConsoleType::Md`, `RomFormat::Md`, `CpuType::MdM68k`,
`CpuType::MdZ80`, and MD-specific `MemoryType` values. These enums are used in C++ and mirrored by
parts of the C# interop/UI, so changing only the core enum is insufficient.

### Z80 reuse requires an extraction, not a type alias

[`SmsCpu`](../../Core/SMS/SmsCpu.h) implements the relevant processor, including undocumented
flags and timing state, but it owns concrete pointers to `SmsConsole` and `SmsMemoryManager` and
hard-codes `CpuType::Sms` in debugger/event calls. Its interrupt completion also calls SMS-specific
cheat refresh behavior. The instruction implementation is reusable; the host integration is not.

Extract a shared engine with these compile-time or narrow host callbacks:

- memory read/write with operation type;
- I/O read/write;
- cycle advancement and contention;
- instruction, memory, halt, interrupt, and event hooks;
- interrupt vector/open-bus value;
- a post-interrupt system callback where needed.

Keep `SmsCpu` and a new `MdZ80` as adapters. Prefer a template/CRTP host in the hot path over a
virtual call for every byte access. Preserve `SmsCpuState`, including `WZ`, `Q`/flag-change state,
halt, and interrupt latches, or migrate it through an explicitly versioned serializer. Run the
existing SMS CPU, IRQ, recorded-ROM, and save-state tests before and after the extraction. A copy
of `SmsCpu` would be a faster prototype, but should not become the long-term architecture because
the two implementations would immediately diverge.

### The YM2612 implementation is already in the tree

`Utilities/Audio/ymfm/ymfm_opn.h` defines `ymfm::ym2612` and `ymfm::ym3438`; the repository already
builds the corresponding source for NES EPSM support. Its upstream and in-tree license is BSD
3-Clause. The MD work therefore needs a Mesen-facing device wrapper, clock/timer/IRQ integration,
mixing, serialization via `ymfm_saved_state`, and verification—not a new FM synthesizer.

This materially lowers risk, but audio is not finished merely by constructing the class. The
wrapper still has to model register access timing and busy status, timers, DAC writes, chip model,
master-clock-derived sample cadence, stereo routing, PSG mixing, and deterministic save/restore.

### Headless execution exists as building blocks

MesenCE already supports:

- `make core` and `nix build .#core` for the native shared library;
- a no-UI `Emulator` object and direct `IConsole::RunFrame()` behavior;
- a Lua test runner and recorded-ROM tests;
- native test programs that drive consoles without the emulation thread;
- video filters that can turn a PPU buffer into a screenshot;
- sound mixing/recording and save-state serialization.

The missing consolidation layer is a supported native executable with a stable input/artifact
contract. The sibling nativekit repository currently carries such an out-of-tree C++ host for
NES. It links MesenCE core sources directly, configures deterministic NES state, loads Lua before
the first opcode, records WAV, creates a screenshot and memory dumps, and emits provenance and
completion metadata without SDL, .NET, or a display. That pattern should move into MesenCE as a
generic tool rather than be reproduced again for MD.

## nativekit findings that affect this design

The sibling nativekit checkout was inspected because consolidation is the stated goal. It already
has a base-console `nativemd` implementation and a separate pinned BlastEm reference toolchain.
The relevant evidence is documented there in `tools/toolchains/README.md`,
`docs/superpowers/specs/2026-11-01-nativemd-design.md`, and the MD decomp documents.

Everything below about BlastEm patches, NMDH, case counts, and observed failures describes
nativekit's locally pinned candidate and patch set as inspected on 2026-11-16. It is not a claim
about unpatched BlastEm, its other front ends, or current upstream behavior. Re-run the identity
checks and qualification suite before using those observations after either repository advances.

The current BlastEm path is not just an unmodified libretro core:

- trace and decomp-hook patches expose 68000/Z80 accesses, VDP activity, selected sound writes,
  pictures, audio, and execution substitution;
- a Z80-bank-window patch prevents a synchronization path from moving the Z80 backward in time;
- a 68000 execution-target patch prevents a frame-clock rebase from stalling `retro_run`;
- save-state work audits state omitted by the reference core, beginning with Z80 `WZ`;
- 59 generated reference cartridges and headless checks exercise the current scope.

These findings are not proof that BlastEm is wrong on hardware, and nativekit correctly labels
some timing behavior unverified. They are proof that replacing the toolchain is a migration with
specific parity obligations. The MesenCE implementation should ingest the synthetic cases and
artifact schemas early, while keeping BlastEm available as a transitional differential oracle.
The live substitution/checkpoint contract is especially important for decomp work; ordinary game
compatibility tests do not exercise it.

## 68000 implementation options

### Option A: port the ares M68000 component, then integrate it natively (recommended)

ares separates its M68000 component from the Mega Drive system through virtual `idle`, `wait`,
16-bit `read`, and `write` operations carrying upper/lower byte strobes. Its Mega Drive CPU then
adds RAM, refresh, interrupts, bus behavior, and thread synchronization. This is close to the seam
MesenCE needs: the CPU can expose each bus transaction and the MD adapter owns console-specific
timing.

Advantages:

- ISC license is compatible with MesenCE's GPLv3 when its notice is preserved;
- prefetch, exceptions, byte lanes, and cycle-visible bus callbacks are part of the design;
- the CPU component is smaller and less system-framework-bound than MAME's device;
- SingleStepTests is generated by MAME, so it is a useful cross-implementation baseline for an
  ares-derived port rather than a test generated by the same CPU.

Costs and risks:

- the source uses ares/nall fixed-width integer types and helpers, so this is a port, not a file
  copy;
- upstream timing choices and undocumented behavior still need verification;
- attribution and a source revision must be recorded;
- Mesen debugger, serializer, dummy/predictive execution, and style integration remain local work.

Gate the decision with a spike: compile only a 68000 core against a flat 16 MiB test bus, execute
the available SingleStepTests corpus, retain exact transaction order/UDS/LDS/cycle counts, test
reset and interrupt sequences, and measure interpreter speed on arm64 and x86-64. Any accepted
differences must be checked against the Motorola manual, hardware-derived evidence, or another
independent implementation.

### Option B: integrate current Musashi under an adapter

Current Musashi is MIT-licensed, portable C, mature, configurable, and exposes instruction hooks,
interrupt callbacks, function-code callbacks, prefetch/address-error options, and saveable CPU
contexts. It is easier to compile in isolation than ares or MAME.

Its own documentation also exposes the tradeoff: the basic configuration has no prefetch and its
host interface is organized around executing a cycle budget, with the host responsible for
timeslice adjustment on device access. It can be configured more accurately, but proving exact
bus traces and integrating mid-instruction VDP stalls would require careful work. Use it as the
fallback if the ares port spike fails its simplicity/performance gate, not as an assumption that a
portable CPU library automatically satisfies decomp trace requirements.

Do not copy the old Musashi fork bundled by Genesis Plus GX. Its historical terms were
non-commercial; use only the current upstream MIT revision if this option is selected.

### Option C: extract MAME's M68000 device

MAME's current M68000 source files are BSD 3-Clause and its microcoded engine is the source used to
generate SingleStepTests. This gives strong timing coverage, but the device is coupled to MAME's
device, execution, address-space, disassembler, and state infrastructure. The test suite would
also cease to be independent of the implementation. It is an excellent reference and a poor
first integration target.

### Option D: embed a complete existing Genesis emulator

| Candidate | License | Engineering result |
|---|---|---|
| ares MD | Primarily ISC; bundled components have their own notices | Accurate reference and source-reading candidate, but its `Node`, `Thread`, scheduler, memory, and serialization framework would be nested inside MesenCE |
| BlastEm | GPLv3 | License-compatible and already patched by nativekit, but brings generated CPU execution, its own clock/state/front-end assumptions, and known local integration repairs |
| Genesis Plus GX | Non-commercial core license plus component licenses | Reject for MesenCE integration; would impose a non-commercial restriction and complicate cloud distribution |
| PicoDrive/older MAME-derived cores | Mixed or historical non-commercial terms | Reject unless every imported file is independently audited; there is no advantage over the cleaner candidates |

A complete-core wrapper can be useful as a throwaway compatibility spike. It should not be merged
as the product architecture because it makes MesenCE's debugger, serializer, rewind, Lua,
recorded tests, and memory model second-class. It also fails the “single headless emulator” goal
in everything except process count.

License notes here are engineering screening, not legal advice. Preserve the exact upstream notice
for every imported file and perform a file-level audit at the selected revision.

## Proposed architecture

```text
Emulator / native headless runner
               |
           MdConsole
               |
     +---------+----------+-------------+
     |                    |             |
 MdM68k + MdBus      shared Z80 + MdZ80 MdVdp
     |                    |             |
 cartridge/RAM/IO <-> bus arbiter <-> FIFO/DMA/IRQ
                          |
                  MdYm2612 + MdPsg
                          |
                 SoundMixer / serializer
```

Suggested source layout:

```text
Core/MD/
  MdConsole.{h,cpp}
  MdTypes.h
  MdMemoryManager.{h,cpp}
  MdM68k.{h,cpp}
  MdZ80.{h,cpp}
  MdVdp.{h,cpp}
  MdPsg.{h,cpp}
  MdYm2612.{h,cpp}
  MdControlManager.{h,cpp}
  Carts/
  Debugger/
Core/Shared/Processors/Z80/
  Z80Cpu.{h,cpp}             # extracted instruction/state engine
tools/m68k-cpu-singlestep/
tools/headless-runner/
tests/MD/
```

Use a single integer master clock derived from the region's colorburst source. Do not schedule the
CPUs with floating-point elapsed time. Each component stores its position in that clock domain;
bus accesses synchronize only the devices that can affect the result, while end-of-frame and
serialization create a full safe point. The exact divider and line/frame constants should come
from selected hardware documentation and be fixed in tests, not inferred from nominal rounded
MHz values.

The initial 68000 bus should explicitly represent upper/lower data strobes. Byte writes on the
68000 are not equivalent to an 8-bit flat memory write, and devices may be connected to only one
lane. Long accesses should remain ordered pairs of word cycles. Address errors, open bus, refresh,
VDP FIFO stalls, DMA ownership, Z80 bus request/reset, and interrupt acknowledge must be state,
not incidental delays hidden in a frame loop.

Serialization must include CPU prefetch/internal state, pending IRQs, Z80 `WZ` and interrupt
latches, bus ownership/request transitions, scheduler positions, VDP command/FIFO/DMA state,
audio timers/busy state and sample phase, controller handshake timers, mapper/SRAM state, and all
RAM. A save-state round trip is insufficient if it only restores visible registers; replay must
produce identical later bus events, pixels, and samples.

## Headless and cloud contract

Create a native `mesen-headless` target that uses the same `Emulator` and console cores as the UI.
It should not P/Invoke the C# layer and should not require an SDL audio device, window server, or
desktop configuration. A proposed interface is:

```text
mesen-headless run --rom GAME --frames N --out NEW_DIR [--input INPUT.json]
                   [--lua COLLECTOR.lua] [--trace TRACE_PROFILE]
                   [--region auto|ntsc-u|ntsc-j|pal] [--timeout SECONDS]
mesen-headless validate OUT_DIR
mesen-headless doctor
```

The output manifest should bind:

- emulator commit/build identity and dirty-state refusal for qualified captures;
- ROM SHA-256 without copying the ROM;
- console, region, power-on policy, controller types, and all non-default settings;
- input/collector hashes and the exact command;
- completion reason, frame and master clock;
- hashes and schemas for screenshots, raw memory, save state, WAV/PCM, and optional trace;
- explicit limits/truncation status.

Trace profiles should be bounded and versioned. The decomp profile needs typed 68000 and Z80
instruction/data accesses, byte-lane information, CPU and master clocks, VDP/DMA activity,
interrupts, and selected sound writes. A lighter compatibility trace can omit electrical detail.
Reaching a frame/event/time limit must fail or state “incomplete”; it must never silently produce
a qualified partial artifact.

Package the runner with Nix as a Linux and macOS derivation and add a minimal container example
only after the native target is deterministic. Interpretive CPUs and software rendering avoid
architecture-specific JIT and GPU dependencies, making x86-64 and arm64 cloud workers equivalent
targets. The ROM, batteries, scripts, and output directory should be explicit mounts; do not use a
shared desktop home. Each process should own one `Emulator` instance and one isolated home/output
directory.

## Implementation milestones

**Status (2026-11-16): M0-M5 implemented on branch `md-support`; M6 not started.** Evidence,
approximations and reference disagreements are in
[`md-verification-findings.md`](md-verification-findings.md). Checks: `test:m68k-cpu`,
`test:z80-cpu`, `test:z80-irq`, `test:headless`, `test:md-synthetic` (60/60 reads and frames
under nativekit's masked comparison; audio 16/17 plus one open KNOWN case,
`sound_ym_envelope_key_on_off`), `test:md-replay` (61/61 incl. audio phase and a mid-handshake
pad save), `test:md-viewers`, `test:md-xarch` (arm64 vs x86-64, 61/61) and `test:ci` (incl. 10
MD recorded tests). Open items: that audio case; ASan (the runtime does not start on this
macOS host; needs a Linux run); x86-64 performance measured only under Rosetta 2; cheats (not in
the M5 list). No commercial-game compatibility is claimed.

### M0: freeze the evidence and headless contract

- Reproduce nativekit's current artifact schema and 59 generated synthetic MD cases without
  importing commercial ROM content.
- Add `mesen-headless` for existing NES/SNES first, using deterministic settings and bounded input.
- Record baseline hashes for the existing nativekit NES capture and BlastEm MD synthetic outputs.
- Add a document listing which nativekit trace/substitution fields MesenCE must expose.

Exit: nativekit can invoke the in-repository runner for an existing MesenCE console; cloud runs do
not need .NET, SDL, or a display; provenance validation passes.

### M1: 68000 core spike and verification

- Port the ares M68000 component behind a flat test bus, preserving its ISC notice and source pin.
- Add `mise run test:m68k-cpu` using SingleStepTests/m68000.
- Check reset, IRQ/autovector, `STOP`, trace, address error, `TAS`, UDS/LDS, prefetch, and
  self-modifying-code cases against primary documentation and independent evidence.
- Add debugger state and disassembly data structures without an MD console dependency.
- Benchmark x86-64 and arm64; run ASan/UBSan where supported.

Exit: no unexplained CPU/register/bus differences in the accepted corpus; caveats such as the
suite's stated `TAS` and `TRAPV` gaps are represented explicitly rather than whitelisted broadly.

### M2: share the Z80 engine without SMS regression

- Extract the instruction/state engine and add SMS/MD host adapters.
- Retain `WZ`, undocumented flags, interrupt behavior, and dummy/predictive access support.
- Run `test:z80-cpu`, `test:z80-irq`, `test:ci`, SMS save-state tests, and performance comparison.

Exit: existing SMS outputs and accepted test results are unchanged; an MD adapter runs flat-bus
programs with its own `CpuType` and debugger hooks.

### M3: bootable MD bus and deterministic scheduler

- Add ROM/header detection, regions, work/Z80 RAM, reset vectors, bus mapping, version/I/O
  registers, Z80 reset/request/grant, bank register/window, open bus, and cartridge SRAM.
- Implement a master-clock scheduler, refresh, IRQ arbitration, and safe serialization points.
- Run authored ROMs that communicate between both CPUs and deliberately race bus ownership.

Exit: synthetic programs boot, both CPUs communicate, bus transitions match the selected hardware
model/reference evidence, and save/replay produces identical bus traces.

### M4: VDP and video

- Implement ports, command state, VRAM/CRAM/VSRAM, FIFO, DMA, counters, IRQs, and timing before
  broad renderer features.
- Add planes, window, scrolling, sprites, priorities, widths, interlace, and shadow/highlight.
- Port nativekit VDP cases and add focused FIFO/DMA/contention tests.

Exit: every synthetic frame and readback case matches the accepted references; no commercial game
claim yet.

### M5: audio, controllers, debugger, and complete state

- Wrap `ymfm::ym2612`/`ym3438`; extract/adapt PSG; implement clocked mixing and timers.
- Add 3/6-button controller protocol and timing.
- Add MD debugger instances, memory tools, trace logger, CDL, labels, Lua state, and state views for
  both CPUs.
- Add recorded-ROM and save-state-replay coverage including audio phase and controller handshakes.

Exit: synthetic pictures, samples, reads, interrupts, and later traces are deterministic across
save/restore and across supported host architectures.

### M6: nativekit migration and compatibility qualification

- Teach nativekit to consume the MesenCE headless manifest/trace/checkpoint APIs.
- Re-run its 59 MD synthetic cases and live substitution/checkpoint controls.
- Differential-test representative titles and features against transitional BlastEm and ares
  builds; investigate every divergence rather than voting by majority.
- Measure maximum-speed performance and capture size on x86-64 and arm64 cloud workers.
- Only then retire the patched BlastEm dependency from the default nativekit workflow.

Exit: nativekit's old and new reference paths agree for the qualified spans, known differences are
source-backed, and the MesenCE path supplies every decomp artifact required by the workflow.

## Test and evidence plan

| Layer | Evidence | Important limitation |
|---|---|---|
| 68000 semantics | Motorola/NXP programmer's reference | Architectural semantics do not fully specify console bus timing |
| 68000 transactions | SingleStepTests/m68000 and targeted ROMs | Suite is MAME-generated; README flags `TAS`, `TRAPV`, and inherited MAME bugs |
| Z80 | Existing MesenCE SingleStepTests and IRQ tasks | Preserve existing accepted classifications; add MD arbitration tests |
| Console contract | Sega software manual and official hardware summary | Manual contains documented cautions/errata; hardware tests still matter |
| Differential | ares, BlastEm, MAME, nativekit synthetic cases | Emulator agreement is evidence, not hardware proof |
| State | save, reload, replay event/pixel/sample hashes | Visible-register equality alone misses hidden scheduler/prefetch state |
| Cloud | clean x86-64/arm64 build and repeated isolated runs | A successful build does not qualify timing or game compatibility |

Each emulation correction should follow this fork's existing policy: reproduction, source for the
intended behavior, regression test in a `mise` task, existing-suite results, performance impact,
and an entry in a new `docs/research/md-verification-findings.md` log. Keep “not assessed,”
“reference disagreement,” and “hardware-unverified” distinct from “pass.”

## Risks and mitigations

| Risk | Consequence | Mitigation |
|---|---|---|
| Treating CPU cycle totals as bus timing | Incorrect VDP/Z80 races despite passing instructions | Bus-visible CPU callbacks; synthetic contention ROMs; master-clock trace |
| Z80 extraction regresses SMS | Existing system breaks during reuse work | Adapter seam first; before/after recorded hashes and full Z80/SMS suite |
| VDP scope dominates schedule | Long gap between CPU success and useful games | Vertical milestones: ports/readback, FIFO/DMA, simple frame, then rendering features |
| Save states omit hidden state | Lockstep and decomp substitution diverge after restore | Replay equality on event stream, pixels, samples, not state struct alone |
| One reference becomes the oracle | Shared bugs are accepted | Primary docs, targeted hardware evidence, and cross-implementation comparison |
| Imported license is misclassified | Distribution/cloud restriction | Pin revisions, preserve notices, audit every imported file; exclude non-commercial cores |
| UI work delays headless value | Consolidation cannot be used in CI/cloud | Native runner and core integration first; UI configuration after system qualification |
| `.bin` extension is ambiguous | Wrong console selected | Prefer `.md`/`.gen`; validate the Sega header/signature before accepting `.bin` |
| Interpreter is too slow for large decomp captures | Cloud cost and feedback time rise | Benchmark every milestone; optimize mapped memory/synchronization after correctness; defer JIT |

## Recommendation and next action

Proceed, but approve it as a multi-milestone console project with headless consolidation as M0,
not as a processor integration patch.

The next implementation action should be a bounded M0/M1 branch:

1. upstream the generic native headless host for NES/SNES with a versioned manifest;
2. add a standalone 68000 test harness;
3. port enough of the ares M68000 component to run reset plus representative instructions and bus
   traces;
4. run a sampled and then complete SingleStepTests corpus, document differences and performance;
5. stop for an architecture decision before adding `Core/MD`.

This order tests the most consequential unknown—the 68000 core seam—while delivering the cloud
runner independently. If the ares port is too coupled or fails the bus/performance gate, repeat the
same harness with current MIT Musashi. Do not begin the VDP or alter `SmsCpu` until one 68000 path
has passed the spike and the headless artifact contract is stable.

## Sources

Primary and owning sources used for this report:

- [Sega Mega Drive hardware summary](https://www.sega.jp/history/hard/megadrive/) — official CPU,
  memory, VRAM, and display summary.
- [Sega Genesis Software Manual, rev. 1992-02-20](https://segaretro.org/images/9/95/GenesisSoftwareManual.pdf)
  — scanned Sega manual covering the memory map, VDP, DMA, system I/O, Z80 control, controllers,
  initialization, PAL behavior, and hardware cautions.
- [Motorola M68000 Family Programmer's Reference Manual](https://www.nxp.com/docs/en/reference-manual/M68000PRM.pdf)
  — instruction set, registers, exceptions, and architectural semantics.
- [SingleStepTests/m68000 README](https://github.com/SingleStepTests/m68000/blob/main/README.md)
  — suite origin, transaction format, and stated `TAS`/`TRAPV`/MAME caveats.
- [ares license](https://github.com/ares-emulator/ares/blob/master/LICENSE),
  [M68000 interface](https://github.com/ares-emulator/ares/blob/master/ares/component/processor/m68000/m68000.hpp),
  [MD CPU](https://github.com/ares-emulator/ares/blob/master/ares/md/cpu/cpu.cpp), and
  [MD system composition](https://github.com/ares-emulator/ares/blob/master/ares/md/md.cpp) — license,
  CPU/bus seam, synchronization model, and subsystem inventory.
- [Musashi upstream README/license](https://github.com/kstenerud/Musashi/blob/master/readme.txt) —
  current MIT terms, host API, configuration, prefetch/address-error options, hooks, and cycle
  integration guidance.
- [MAME M68000 header](https://github.com/mamedev/mame/blob/master/src/devices/cpu/m68000/m68000.h)
  and [MAME licensing documentation](https://github.com/mamedev/mame/blob/master/docs/source/license.rst)
  — file license and framework interface.
- [ymfm license](https://github.com/aaronsgiles/ymfm/blob/master/LICENSE) and the in-tree
  `Utilities/Audio/ymfm/ymfm_opn.h` — BSD terms and existing YM2612/YM3438 implementation.
- [BlastEm license](https://github.com/libretro/blastem/blob/master/COPYING) — GPLv3 terms for the
  current nativekit reference candidate.
- [Genesis Plus GX README](https://github.com/ekeeke/Genesis-Plus-GX) and
  [license](https://github.com/ekeeke/Genesis-Plus-GX/blob/master/LICENSE.txt) — upstream's stated
  non-commercial core terms and component notices.
- This repository's [`CLAUDE.md`](../../CLAUDE.md),
  [`SmsCpu`](../../Core/SMS/SmsCpu.h), [`SmsConsole`](../../Core/SMS/SmsConsole.cpp),
  [`IConsole`](../../Core/Shared/Interfaces/IConsole.h),
  [`Emulator`](../../Core/Shared/Emulator.cpp), and in-tree `ymfm` sources — current architecture,
  verification tasks, coupling, console selection, headless building blocks, and reusable audio.
- Sibling nativekit sources inspected at the same date: `README.md`,
  `tools/toolchains/README.md`, `consoles/nes/docs/capture.md`,
  `consoles/nes/tools/capture/mesen_host.cpp`,
  `docs/superpowers/specs/2026-11-01-nativemd-design.md`, and MD decomp documentation — current
  Mesen NES host, BlastEm patch set, synthetic MD corpus, checkpoint/trace requirements, and
  migration constraints. These are project evidence, not independent hardware authorities.
