# Sega CD / Mega-CD support: feasibility and plan

Snapshot: branch `md-support` at `a8453071`, 2026-11-18. Status: proposal; nothing implemented.

**Status (2026-11-18): S3 done** (CD drive and controller, CD audio, disc boot with the BIOS:
7/7 BIOS versions found locally boot and reach their CD player; the RAM cartridge is left out for
lack of a usable source; findings S3). **S2 done** (graphics chip, PCM, backup RAM, dot image, font registers,
VDP DMA from word RAM; findings S2). **S1 done, except comparing against ares** (no ares capture
harness yet).
The Sega CD unit runs in Mode 1 behind the headless runner's `--md-mcd` test hook: sub CPU, program
and word RAM, gate array registers, timer, stopwatch and the level 2/3 interrupts, covered by seven
`mcd_*` synthetic cases (findings S1). Not yet: BIOS, discs, PCM, graphics chip, backup RAM,
debugger and a UI setting.

**S0 (done).** Results are in [`mcd-verification-findings.md`](mcd-verification-findings.md):
- the figures below are checked against the BIOS manual, the LC8951 manual, ares and MAME (F1);
- Mode 1 testing without the BIOS is supported by two sources (F2);
- timebase: master clocks plus an exact remainder for the sub CPU (F3);
- a second 68000 measured at +28% frame time (F4);
- the Mega-CD hardware manual agrees with the plan's memory maps, registers and interrupts. One
  correction: the main CPU also gets program RAM while it holds the sub CPU in reset (F6).

## Decision

This is an engineering judgment from reading this repository and the sources listed at the end,
not a measured result.

Sega CD support is feasible and fits on top of the existing Mega Drive core. It is roughly as
large as the cartridge core was, but the hardest single piece of that work, a verified 68000, is
already done: the Sega CD's second CPU is another 68000, and `M68kCore` (the ares-derived core
checked against 317,242 SingleStepTests cases) can run as a second instance. Most of the new work
is in custom Sega chips that have no test suites: the gate array, the graphics chip, and the CD
drive and its controller.

Recommended direction:

- Add the Sega CD as an optional unit inside the Mega Drive console (`Core/MD/Mcd/`), not as a new
  console type. The cartridge core then stays as it is when no unit is attached.
- Bring it up in **Mode 1** first (a cartridge running with the Sega CD attached). In Mode 1 the
  main CPU loads the sub CPU's program into program RAM itself, so synthetic test cartridges can
  drive the whole unit **without Sega's BIOS**. That keeps the tests runnable in CI, where
  copyrighted BIOS files can't go. (Supported by the BIOS manual's Mode 1 boot sequence and the
  sub CPU memory map in both manuals, findings F2 and F6. The first S1 test confirms it in the
  emulator.)
- Use ares (ISC) as the main reference and MAME (BSD-3) as the second. Don't use Genesis Plus GX
  (non-commercial licence), as for the cartridge core.
- Add the disc path (BIOS boot, CD drive, CD audio) after Mode 1 is verified.

32X and Sega CD 32X are out of scope here; they need an SH-2 core and get their own document.

## What the hardware adds

Figures from ares `md/mcd` (ISC) and MAME `megacd.cpp` (BSD-3), which agree on them. The Sega
manuals (see Sources) are the primary reference to confirm them against in S0; Sega Retro's
copies could not be fetched automatically when this was written.

| Part | What it is | Notes |
|---|---|---|
| Sub CPU | 68000 at 12.5 MHz (its own crystal, unrelated to the Mega Drive's 53.69 MHz) | Reuse `M68kCore` |
| Program RAM | 512 KiB, sub CPU's main memory (its vectors are at `$000000`) | Main CPU sees a 128 KiB window, bank-selected, only while it holds the sub CPU's bus; write protection in 512-byte units |
| Word RAM | 256 KiB shared between the CPUs | 2M mode: one CPU owns all of it, ownership swapped by handshake. 1M mode: two 128 KiB banks, one per CPU, plus a "cell image" view for the main CPU |
| Gate array | Registers at `$A12000` (main) and `$FF8000` (sub): reset/bus request of the sub CPU, word RAM mode and ownership, 8+8 communication words and flags, interrupts | The main CPU raises level 2 on the sub CPU (IFL2); nothing interrupts the main CPU (corrected in S1, findings S1) |
| Sub CPU interrupts | 1 graphics, 2 from main CPU, 3 timer, 4 CD drive, 5 CD controller, 6 subcode | Order matches ares' IRQ sources |
| Timer | 8-bit, ticks at 12.5 MHz / 384 | ares clocks CDC, CDD, timer and PCM from the same /384 divider |
| Graphics chip | Rotates and scales 16x16 or 32x32 "stamps" from word RAM into an image in word RAM | Used for Mode 7-like effects (Batman Returns, Sol-Feace intro) |
| PCM | Ricoh RF5C164: 8 channels, 64 KiB wave RAM, about 32.55 kHz (12.5 MHz / 384) | MAME has an RF5C68/RF5C164 device (BSD-3) |
| CD controller | Sanyo LC8951: sector decoding, 16 KiB buffer, transfers to word RAM, program RAM, PCM RAM or the main/sub CPU | Datasheet on Sega Retro |
| CD drive | Status/command protocol of 10 nibbles, 75 Hz sector rate, seek times, subcode, CD audio at 44.1 kHz with a fader | The least documented part; ares has one model |
| Backup RAM | 8 KiB internal (odd bytes on the sub bus), optional 128 KiB RAM cartridge | Battery save |
| BIOS | 128 KiB, per region (US, Europe, Japan, plus model variants) | User-supplied firmware |

## Fit with MesenCE

### The Mega Drive core

- **Second 68000.** `M68kCore` is a class with virtual bus callbacks and no static state, so an
  `McdM68k` subclass can provide the sub CPU's bus.
- **Scheduler: the biggest change.** `MdConsole::RunFrame` steps the main 68000 and everything
  else catches up to it on one master clock. The sub CPU runs from a separate 50 MHz crystal (/4),
  so the two clocks have no small integer ratio.
  - Decided in S0 (findings F3): keep master clocks everywhere. The sub CPU converts its cycles
    with an exact remainder. A common unit would overflow `uint64_t` after about 190 hours and
    would change every existing component.
  - Synchronise only on accesses the other side can see: gate array registers, word RAM, program
    RAM, and the interrupt lines.
  - ares synchronises at every bus wait and uses `double` for its CD-audio sample counter. MesenCE
    should stay integer-only so replays stay bit-identical across architectures (`test:md-xarch`).
- **Main CPU bus:**
  - The expansion area (`$000000-$3FFFFF` with no cartridge, `$400000-$7FFFFF` in Mode 1) and
    `$A12000-$A1202F` are currently "nothing connected" in `MdMemoryManager`.
  - The version register's bit 5 ("no expansion unit") must clear when the unit is attached.
  - (Corrected in S1: the gate array does not interrupt the main CPU, so the light-gun level 2
    path is unaffected. The main CPU's level 2 write goes to the sub CPU.)
  - VDP DMA from word RAM has a known one-word offset that games compensate for. It needs a test
    and a source before it is modelled.
- **Save states.** The unit adds about 860 KiB of RAM (program 512 KiB, word 256 KiB, PCM 64 KiB,
  CDC buffer 16 KiB, backup 8 KiB), plus drive position, CDC state and both CPUs. Rewind keeps
  many states, so check rewind's memory use and speed.

### CD infrastructure

- **Disc images.** `Core/Shared/CdReader` (used by the PC Engine CD core) parses `.cue` files
  with `BINARY` and 16-bit 44.1 kHz `WAVE` tracks, plus subcode files.
  - Missing: `.iso` + audio sets, `.chd` and compressed audio. They aren't required for a first
    version, but many Sega CD dumps are distributed as `.chd`.
- **Console selection.** `PceConsole` claims every `.cue` and returns `Failure` if it doesn't
  look like a PC Engine disc.
  - It is tried before `MdConsole` (`Emulator.cpp`, the `TryLoadRom` order), so a Sega CD `.cue`
    never reaches the Mega Drive core today.
  - Fix: both consoles check the disc signature. Sega CD discs start with `SEGADISCSYSTEM` in
    the first data sector; a non-matching disc returns `UnknownType` so the next console can try.
- **CD audio.** `PceCdAudioPlayer` and `PceAudioFader` are PC Engine specific. The sector reading
  and resampling could be shared; the Sega CD fader and CD drive timing are different.
- **Firmware.** `FirmwareHelper` already prompts for PC Engine CD BIOS files. Add Sega CD types
  for US, Europe and Japan, identified by hash, with a region chosen from the disc header.

### Debugger, UI and scripting

New CPU type (sub 68000) and memory types (program RAM, word RAM, PCM RAM, CDC buffer, backup
RAM). The 68000 disassembler and debugger code can serve both CPUs. Event viewer entries for CD
controller and graphics chip activity. Lua needs the new memory types. Cheats would cover the main
CPU first.

## Verification plan

There are no SingleStepTests-style suites for the custom chips. Evidence has to come from the Sega
manuals and chip datasheets, comparison with other emulators, and hardware-tested programs where
they exist. The 68000 is already covered.

| Layer | Evidence | Limitation |
|---|---|---|
| Sub 68000 | Existing `test:m68k-cpu` (same core) | Bus timing on the sub bus is new |
| Gate array, word RAM, program RAM, timer, interrupts | Synthetic Mode 1 cartridges (no BIOS), expected values from the hardware manual; compared with ares | Two-CPU races need care; the manual is not cycle-exact |
| Graphics chip | Synthetic stamp/rotation cases, picture compared with ares | Timing (how long a render takes) is poorly documented |
| PCM | Per-chip audio capture (as `test:md-synthetic` does for the YM2612/PSG), compared with ares and MAME | Both are emulators |
| CD controller / drive | LC8951 datasheet; synthetic disc images (generated cue/bin) | Booting a disc needs the BIOS, so these run locally, not in CI |
| Accuracy tests by others | `mcd-verificator` (gendev forum; CDC register tests, reported against hardware), 240p Test Suite Sega CD build, the 68000 timing test's Sega CD build | Availability and licences to be checked before vendoring anything |
| Save states | `test:md-replay`-style replay equality, extended to the sub CPU and CD state | |
| Games | A recorded list of commercial titles exercising each feature, checked locally | No commercial content in the repository |

A capture harness for ares is needed in order to compare against it. That is new work: the
cartridge core used nativekit's patched BlastEm, and BlastEm's Mega CD support was still in
progress according to the gendev thread above.

## Milestones

**S0: sources and design** (done 2026-11-18; findings S0)
- Read the Mega-CD hardware and BIOS manuals and the LC8951 and RF5C164 datasheets. Correct the
  table above where they differ.
- Pin an ares revision for reference.
- Decide the scheduler timebase. Measure the overhead of a second 68000 in a stub `RunFrame`.

Exit: a findings entry for every figure the core relies on, with its source.

**S1: Mode 1 core, no BIOS** (done 2026-11-18 except the comparison with ares; findings S1)
- Sub 68000, gate array, program RAM, both word RAM modes, communication registers, timer, and
  interrupts (main to sub; the sub CPU cannot interrupt the main CPU).
- Synthetic Mode 1 cartridges in `tests/MD/synthetic` (new `mcd_*` cases) with expected values.
- Save-state replay and cross-architecture checks.

Exit: all cases pass in CI; ares agrees, or each difference is explained.

**S2: graphics chip, PCM, backup RAM** (done 2026-11-18; the RAM cartridge moved to S3)
- Stamp rendering, PCM mixing into the sound mixer, and backup RAM (internal, then the RAM
  cartridge) saved as a battery file.
- Pictures and per-chip audio captured and compared.

**S3: discs** (done 2026-11-18, without the RAM cartridge)
- Disc selection by signature (with the PC Engine change above).
- CD drive and CD controller with seek timing, subcode, and CD audio through the fader.
- BIOS firmware handling.

Exit: each regional BIOS reaches its CD player, plays audio tracks, and boots a synthetic disc.

**S4: integration** (done 2026-11-18, without Mode 1 disc pairing in the UI; findings S4)
Debugger views, settings (region, BIOS, RAM cartridge), Lua memory types, the headless runner
(`--disc`, `--bios`), and the input and recording paths.

**S5: games** (partial 2026-11-18: Flux, a Mode 1 cartridge, plays audio CDs; no disc images available for the rest; findings S5)
Qualify a list of commercial titles chosen to cover:
- word RAM 1M mode streaming (FMV games);
- graphics chip (Batman Returns, Silpheed's intro);
- PCM-heavy audio;
- CD audio (Sonic CD);
- backup RAM saves.

The helios emulator's notes list titles that broke there, which is a useful starting list.

## Risks

| Risk | Consequence | Mitigation |
|---|---|---|
| CD drive timing is underspecified | Games hang on seeks or stream badly | Model ares' timing as a labelled approximation; test FMV games early |
| Two-CPU synchronisation | Slow emulation if every access syncs; wrong results if too few do | Sync only on shared resources; benchmark in S0 |
| Floating-point timing from references | Replays differ between machines | Integer clocks; `test:md-xarch` on all cases |
| BIOS can't be in CI | Disc path untested in CI | Mode 1 tests in CI; disc tests local, documented in findings |
| State size | Slower rewind, bigger states | Measure in S1; compress or exclude unused RAM later if needed |
| `.chd` not supported | Many users' dumps don't load | Document; consider CHD support as separate work (libchdr is BSD-3) |
| Upstream proposal | Even larger branch | Keep out of `md-upstream-proposal`; propose only after the cartridge core is decided |

## Sources

- ares `md/mcd`: [directory](https://github.com/ares-emulator/ares/tree/master/ares/md/mcd),
  [`mcd.hpp`](https://github.com/ares-emulator/ares/blob/master/ares/md/mcd/mcd.hpp),
  [`mcd.cpp`](https://github.com/ares-emulator/ares/blob/master/ares/md/mcd/mcd.cpp) (memory
  sizes, 12.5 MHz sub CPU, /384 divider, IRQ sources), [`pcm.cpp`](https://github.com/ares-emulator/ares/blob/master/ares/md/mcd/pcm.cpp)
  (RF5C164, 64 KiB, 12.5 MHz / 384). ISC licence.
- MAME [`megacd.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/sega/megacd.cpp),
  [`megacdcd.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/shared/megacdcd.cpp),
  [`rf5c68.cpp`](https://github.com/mamedev/mame/blob/master/src/devices/sound/rf5c68.cpp):
  BSD-3. MAME's file notes save states are still a TODO, so it is the second reference.
- Sega Retro: [Mega-CD official documentation](https://segaretro.org/Mega-CD_official_documentation)
  (hardware manual, BIOS manual), [Sanyo LC8951 datasheet](https://segaretro.org/images/5/55/Sanyo_LC8950_%26_LC8951.pdf),
  [Sega CD programming FAQ](https://segaretro.org/Sega_CD_programming_FAQ_(1998-12-06)),
  [Boot ROM](https://segaretro.org/Sega_Mega-CD/Boot_ROM).
- [helios Mega CD notes](https://github.com/fedex81/helios/blob/master/megacd_notes.txt): word
  RAM modes, mirrors, titles that failed there.
- Test programs: [mcd-verificator thread](https://gendev.spritesmind.net/forum/viewtopic.php?p=37413),
  [240p Test Suite](https://artemiourbina.itch.io/240p-test-suite/devlog/337805/240p-test-suite-for-genesismega-drive-and-segamega-cd-123),
  [68000 timing test](https://gendev.spritesmind.net/forum/viewtopic.php?p=37516),
  [Mega CD Mode 1 template](https://github.com/OrionNavattan/Mega-CD-Mode-1-Template).
- This repository: `Core/MD/M68k/M68kCore.h`, `Core/MD/MdConsole.cpp` (`RunFrame`),
  `Core/MD/MdMemoryManager.cpp` (expansion area, version register), `Core/Shared/CdReader.{h,cpp}`,
  `Core/PCE/PceConsole.cpp` (`.cue` handling), `Core/Shared/Emulator.cpp` (console order),
  `Core/Shared/FirmwareHelper.h`.
