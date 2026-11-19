# Sega 32X / Mega 32X support: feasibility and plan

Snapshot: branch `sega-cd` at `0e1a17dd`, 2026-11-19.

**Status (2026-11-19): S0 and S1 done.** S1: SH-2 core, 68,140 `sh4` suite tests and 97 scenario
checks pass (`mise run test:sh2-cpu`). S0: the SH-1/SH-2 programming manual, SH7604 hardware
manual and Sega 32X hardware manual are checked; the plan's figures hold (findings S0-F2, F6).

## Decision

This is an engineering judgment from reading this repository and the sources listed at the end,
not a measured result.

32X support is feasible on top of the existing Mega Drive core, following the path the Sega CD
took: an optional unit inside `MdConsole` (`Core/MD/M32x/`), not a new console type. It is a
larger step than the Sega CD in one respect: **the SH-2 is a new CPU** and must be verified on its
own before anything is built on it. The Sega CD reused the verified 68000; the 32X has no such
head start. Its custom hardware (the "Mars" ASIC: VDP, PWM, communication registers, DREQ FIFO)
is smaller than the Sega CD's gate array, graphics chip and CD drive together.

Recommended direction:

- **SH-2 first, as its own milestone (S1)**, with a SingleStepTests-style harness like every other
  CPU in this fork. There is no SH-2 suite; SingleStepTests' `sh4` suite (MIT) covers the SH-4,
  whose integer instruction set is a superset of the SH-2's. Run the shared encodings with the
  SH-4-only state masked (findings S0-F3). This is the `test:pce-cpu` arrangement (an independent
  suite for the shared opcodes) and it is weaker than a hardware-generated suite: the `sh4` tests
  were generated from Reicast's interpreter, not from hardware.
- **Port the ares SH-2 interpreter** (`ares/component/processor/sh2`, ISC; the recompiler is
  dropped). ares charges one cycle per instruction, so timing comes from the Hitachi SH-1/SH-2
  programming manual's instruction timing table instead, with BlastEm's `sh2.cpu` (GPL-3, which
  cites the manual per instruction) as the cross-check.
- **BIOS:** the three Sega boot ROMs (68000 vector ROM, master and slave SH-2 boot ROMs) are
  user-supplied firmware, as for the Sega CD. CI can't hold them, so synthetic tests use small
  replacement boot ROMs written for this project (S2; findings S0-F5). Real-BIOS boots run
  locally.
- **References:** ares `md/m32x` (ISC) is the main one, as it was for the Sega CD. BlastEm
  (GPL-3) is the second and is already built under `nativekit` for the cartridge core's
  reference captures, so a 32X capture harness may come cheaply. MAME `sega_32x.cpp` (BSD-3) is
  the third. Genesis Plus GX and PicoDrive are not used (non-commercial licences).

## What the hardware adds

Figures from ares `md/m32x` and BlastEm `32x.c`, which agree on them. S0 checks them against
Sega's 32X hardware manual.

| Part | What it is | Notes |
|---|---|---|
| 2 × SH-2 (SH7604) | Master and slave, 23.01 MHz = MD master clock × 3/7 | Exact integer ratio with the MD clock: no remainder bookkeeping as for the Sega CD's 12.5 MHz |
| SH7604 on-chip | 4 KiB cache (4-way, or 2-way + 2 KiB RAM), DMAC (2 ch), DIVU, FRT, WDT, SCI, INTC, BSC | Games use the cache, DMAC (PWM and DREQ feeding), DIVU and the FRT; the SCI links the two SH-2s |
| SDRAM | 256 KiB at `$06000000` (SH-2 side) | Shared by both SH-2s |
| Frame buffer | 2 × 128 KiB DRAM, one shown while the other is drawn, swapped at VBlank | 68000 sees the back buffer at `$840000` (plus overwrite mode at `$860000`) when it has VDP access |
| 32X VDP | Packed (8 bpp, 256-colour palette), direct (15 bpp) and run-length modes; line table; fill; 224/240 lines | Overlaid on the MD picture per pixel, priority bit selects which plane wins |
| Palette | 256 × 15-bit + priority bit | Accessible only outside active display (PEN) |
| PWM | Stereo/mono PWM with 3-word FIFOs, sample rate from a cycle register, timer interrupt to the SH-2s | Fed by the SH-2 DMAC on most games |
| Communication | 8 words at `$A15120` (68000) / `$4020` (SH-2) plus interrupt lines both ways | |
| DREQ | 68000-to-SH-2 FIFO (`$A15112`), drained by an SH-2 DMA channel | Used for streaming |
| Cartridge mapping | With the adapter enabled, the cartridge moves to `$880000` (first 512 KiB) and `$900000` (1 MiB bank window); the SH-2s see it at `$02000000` | `RV` bit gives the 68000 the original mapping for VDP DMA |
| BIOS | 256-byte 68000 vector ROM; 2 KiB master and 1 KiB slave SH-2 boot ROMs | User-supplied |

## Fit with MesenCE

### The Mega Drive core

- **Unit, not console.** Like `McdUnit`: `MdConsole` owns an optional `M32xUnit`. Without one the
  cartridge core is unchanged and pays nothing.
- **Scheduler.** SH-2 cycles are 7/3 master clocks. The two SH-2s catch up to the 68000 on
  accesses the other side can see (communication words, FIFO, frame buffer, interrupt lines), as
  the Sega CD's sub CPU does. They also share SDRAM with each other, so they need tighter mutual
  sync. ares syncs them every 10 SH-2 cycles and notes games that hang if the windows are too
  wide (Brutal, Chaotix). The sync windows are a measured trade-off for S2.
- **Main CPU bus.** `MdMemoryManager` routes `$800000-$9FFFFF` and `$A15100-$A151FF` to "nothing
  connected" today (`MdMemoryManager.cpp`, the `$800000-$9FFFFF` comment). The 68000 vector area
  `$000000-$0000FF` is replaced by the vector ROM while the adapter is enabled.
- **Video.** `MdVdp` writes 16-bit palette-index pixels. 32X pixels are 15-bit direct colour, so
  the composited line needs a tagged pixel format (for example bit 15 = 32X RGB555) that
  `MdDefaultVideoFilter` decodes. The 32X image is offset from the MD image by a fraction of a
  pixel in H40 (ares: about 3¼ pixels); that offset needs a source before it is modelled.
- **Audio.** PWM is a third sound source mixed through `MdApu`, the way the Sega CD's PCM is.
- **Save states.** The unit adds about 550 KiB of RAM (SDRAM 256 KiB, frame buffers 256 KiB,
  caches 2 × 4 KiB, palette), plus both SH-2s and their on-chip peripherals.
- **Detection.** `.32x` extension, or the `"MARS"` security code / the 32X header at `$3C0`.

### Debugger, UI and scripting

Two new CPU types (SH-2 master and slave) with an SH-2 disassembler (ares has one), memory types
for SDRAM, frame buffer, palette and the boot ROMs. `DebugUtilities::GetLastCpuMemoryType()`
must move again (see CLAUDE.md, Sega CD S4). The Dummy-CPU pattern used for access prediction
needs a dummy SH-2. Lua needs the new memory types.

## Verification plan

| Layer | Evidence | Limitation |
|---|---|---|
| SH-2 instructions | `sh4` SingleStepTests, shared encodings, registers and memory accesses (`test:sh2-cpu`) | Generated from Reicast, not hardware; MAC.W/MAC.L excluded by the suite; SH-4 exception model differs (TRAPA, RTE, SLEEP and illegal-slot cases excluded) |
| SH-2 timing | Hitachi manual table; BlastEm comparison | No hardware timing suite |
| SH-2 exceptions, interrupts, delay slots | Scenario tests on the real core (like `test:z80-irq`), expected values from the Hitachi manual | |
| SH7604 peripherals | Scenario tests against the SH7604 hardware manual (DIVU results and overflow, FRT compare, DMAC counts) | |
| Mars ASIC | Synthetic 32X cartridges with replacement boot ROMs, expected values from Sega's 32X hardware manual; compared with ares/BlastEm | Two- and three-CPU races |
| Picture | Synthetic VDP-mode cases, frame hashes; compared with BlastEm | Sub-pixel offset |
| PWM | Per-chip audio capture as `test:md-synthetic` does | |
| Save states | `test:md-replay`-style replay and `test:md-xarch` | |
| Games | Local qualification on the user's 37-cartridge library (`~/Games/ROMs/Sega 32X`) | No commercial content in the repository |

## Milestones

**S0: sources and design**
- Pin ares and BlastEm revisions; find the Sega 32X hardware manual and the Hitachi SH7604
  hardware manual and SH-1/SH-2 programming manual.
- Decide the SH-4 suite mapping (which files, which state is compared).
- Decide the replacement boot ROM approach.

Exit: a findings entry for every figure the core relies on, with its source.

**S1: SH-2 core** (`Core/MD/Sh2/`)
- Interpreter ported from ares, timing from the manual, on-chip peripherals (cache, DMAC, DIVU,
  FRT, WDT, SCI, INTC).
- `mise run test:sh2-cpu`: the `sh4` suite's shared encodings, plus exception/interrupt/delay-slot
  and peripheral scenarios.

Exit: every shared encoding passes or each divergence is explained in the findings.

**S2: Mars ASIC with replacement boot ROMs**
- `M32xUnit`: both SH-2s, SDRAM, communication, interrupts, DREQ FIFO, cartridge mapping, frame
  buffer access, scheduler.
- Synthetic 32X cartridges in `tests/MD/synthetic` (`m32x_*`), runnable in CI.

**S3: VDP and PWM**
- The three bitmap modes, line table, fill, palette, swap, priority composition; PWM audio.

**S4: real BIOS and integration**
- Firmware handling for the three Sega boot ROMs; debugger CPU and memory types; settings; Lua;
  headless runner options.

**S5: games**
Qualify titles chosen to cover: heavy SH-2 sync (Knuckles' Chaotix, Brutal), the DREQ FIFO
(Night Trap isn't in the library; Metal Head, Tempo), PWM (the Celtic PWM test, any title with
music), RLE mode (Doom), both SH-2s plus the cache (Virtua Racing Deluxe, Virtua Fighter, Star
Wars Arcade).

## Risks

| Risk | Consequence | Mitigation |
|---|---|---|
| No hardware-generated SH-2 suite | Wrong results the SH-4 suite can't see (exceptions, SH-2-specific behaviour, timing) | Scenario tests from the Hitachi manuals; BlastEm comparison; label it |
| Three-CPU synchronisation | Slow emulation, or hangs in sync-sensitive games | Measured sync windows; Chaotix/Brutal in S5 |
| Interpreter speed | Two 23 MHz CPUs is the heaviest load in the fork; ares relies on a recompiler | Measure in S1; keep the hot path tight; no recompiler planned |
| BIOS can't be in CI | Real boot path untested in CI | Replacement boot ROMs in CI; real-BIOS boots local |
| Timing references are emulators | Cycle-sensitive games differ | Manual first, emulators as cross-checks |

## Sources

- ares: [`component/processor/sh2`](https://github.com/ares-emulator/ares/tree/master/ares/component/processor/sh2),
  [`md/m32x`](https://github.com/ares-emulator/ares/tree/master/ares/md/m32x). ISC. Local copy
  `bin/ares-src` at `140c0a3`.
- BlastEm: `sh2.cpu`, `32x.c`, `32x_video.c` (GPL-3). Local copy under
  `~/src/nativekit/build/toolchains/src/blastem-inspect`.
- MAME [`sega_32x.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/sega/sega_32x.cpp),
  [`sh2.cpp`](https://github.com/mamedev/mame/tree/master/src/devices/cpu/sh): BSD-3.
- [SingleStepTests `sh4`](https://github.com/SingleStepTests/sh4) (MIT).
- Sega Retro: [32X official documentation](https://segaretro.org/Sega_32X/Technical_specifications)
  (hardware manual), Hitachi SH7604 hardware manual and SH-1/SH-2 programming manual.
- This repository: `Core/MD/MdConsole.{h,cpp}`, `Core/MD/MdMemoryManager.cpp`, `Core/MD/MdVdp.cpp`,
  `Core/MD/Mcd/McdUnit.h` (the unit pattern), `docs/research/sega-cd-feasibility.md`.
