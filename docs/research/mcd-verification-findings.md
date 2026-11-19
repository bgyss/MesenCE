# Sega CD / Mega-CD verification findings

Findings for the Sega CD work planned in [`sega-cd-feasibility.md`](sega-cd-feasibility.md). Each
entry gives the source and how far it has been checked. "Two sources" means two independent
documents or implementations agree; it is not a hardware test.

## S0: sources and design (2026-11-18)

Status: S0 is done. The Mega-CD hardware manual was supplied by the user (it couldn't be
downloaded automatically) and checked against the plan in F6. Register-level details are read
from it as each part is implemented in S1/S2. Nothing here changes emulator code.

### Sources pinned

| Source | Revision / version | Licence | Used for |
|---|---|---|---|
| ares `ares/md/mcd` | `65c6fc2f` (master, 2026-11-18; last change to `md/mcd` `0b9b8bce`, 2026-07-24) | ISC | Primary reference implementation |
| MAME `src/mame/sega/megacd.cpp`, `src/mame/shared/megacdcd.cpp`, `src/devices/sound/rf5c68.cpp` | `ac08a4c2` (master, 2026-11-18) | BSD-3 | Second reference |
| Sega, *Mega-CD BIOS Manual* | Ver 2.00, 1992-02-24 (Sega Retro scan, `MCDBios.pdf`) | Sega confidential document, read for facts only | Sub-CPU memory map, boot sequence, interrupt levels, CD drive behaviour |
| Sega, *Mega-CD Hardware Manual* | Ver 1.0, 1991-10-14 (97-page scan; kept out of the repository) | Sega confidential document, read for facts only | Memory maps, register table, interrupts, timer, bus switching |
| Sanyo, *LC8950 & LC8951 Design Manual* | Sega Retro scan (139 pages, OCR) | Read for facts only | CD controller registers |

Genesis Plus GX was not consulted (non-commercial licence), as for the cartridge core.

### F1. Hardware figures in the plan, by source

| Figure | BIOS manual | LC8951 manual | ares | MAME | Status |
|---|---|---|---|---|---|
| Sub CPU is a 68000 at 12.5 MHz | | | `Thread::create(12'500'000)` | `SEGACD_CLOCK 12500000` | Two implementations; the hardware manual doesn't state it but its timer step (30.72 µs = 384 / 12.5 MHz, F6) agrees |
| Program RAM 512 KiB at sub `$000000-$07FFFF`, vectors at `$000000` | Memory map p. 4 (`$000000` vectors to `$07FFFF`) | | `pram 512_KiB`, sub bus `0x000000-0x07ffff` | | Two sources |
| Program RAM write protection, in 512-byte units | "Write protect area" `$000000-$005400` (p. 4) | | `address >= pramProtect << 9` | | Hardware manual: `$000000-$01FDFF` in `$200` units, set by main `$A12002` (F6) |
| Word RAM 256 KiB; sub `$080000-$0BFFFF` (2M) or `$0C0000-$0DFFFF` (1M bank) | | | `wram 256_KiB`, sub bus decode | | Hardware manual pp. 12-16 (F6) |
| Main CPU sees program RAM only while it holds the sub CPU's bus or holds it in reset | Mode 1 boot transfers programs before releasing the sub CPU (p. 30) | | `if(!io.request) return data` | | Hardware manual p. 6: SBRQ=1 or SRES=0 in `$A12001` (F6) |
| 1M mode "cell image" view at main `$220000-$23FFFF` (four address-swizzle ranges) | | | `bus-external.cpp` | | Hardware manual p. 16: the same four ranges (F6) |
| VDP DMA from word RAM goes through a one-word latch | | | `wramLatch` while `vdp.dma.active` | | One implementation; the effect (games add 2 to the source) is folklore until a test shows it |
| Main CPU's level 4 (H-INT) vector redirected by the gate array | | | `io.vectorLevel4` returned at `$70/$72` | | Hardware manual register table: main `$A12006` "H-INT vector" (F6) |
| Sub CPU interrupts: 2 = from main ("software V-INT"), 4 = CD drive, 5 = CD controller, 6 = subcode | p. 5 ("level 2 (software V-INT)", "level 4 (CDD)", "level 5 (CDC)", "level 6 (SCD)") | | IRQ sources | | Two sources |
| Sub CPU interrupts: 1 = graphics (2M mode only), 3 = timer | | | IRQ sources | | Hardware manual pp. 15, 30 (F6) |
| CD drive interrupt every 13.3 ms (75 Hz) | p. 5 ("level 4 interrupts with 13.3 msec interval") | | | | Manual |
| CD controller buffer 16 KiB, "up to 5 frames" | p. 1 | | `cdc.ram 16_KiB` | | Two sources |
| Subcode buffer: 128 bytes in the gate array | p. 1 | | `cdd.subcode[64]` (words) | | Two sources |
| Backup RAM 8 KiB at sub `$FE0000` (odd bytes) | p. 1 ("up to 8K bytes") | | `bram 8_KiB`, sub `0xfe0000-0xfeffff` | | Two sources (address from ares) |
| PCM: RF5C164, 8 channels, 64 KiB, output at 12.5 MHz / 384 | p. 1 ("[SDC] PCM sound generator chip") | | `pcm.ram 64_KiB`, `12'500'000 / 384.0` | `RF5C164(..., SEGACD_CLOCK)` | Two implementations |
| CD controller, CD drive, timer and PCM clocked from one 12.5 MHz / 384 divider | | | `MCD::step` | | Timer: hardware manual p. 30 (30.72 µs per count); the others one implementation |
| Fader volume `$000-$400`, master volume, emphasis flag | p. 23 | | | | Manual |
| LC8951: 16 registers selected through an address register that auto-increments (not at R0) | | §11.1 | `cdc.cpp` | | Manual |
| LC8951 read registers: COMIN, IFSTAT, DBCL/H, HEAD0-3, PTL/H, WAL/H, STAT0-3 | | Table 7 | | | Manual |
| LC8951 write registers: SBOUT, IFCTRL, DBCL/H, DACL/H, DTTRG, DTACK, WAL/H, CTRL0/1, PTL/H, RESET | | Table 8 | | | Manual |

### F2. Mode 1 can be tested without the BIOS (supports the plan's main decision)

- BIOS manual §4-1, Mode 1 boot: the main CPU copies the CD system program and the sub CPU's
  program into program RAM, then releases the sub CPU from reset.
- The sub CPU's reset vectors are at `$000000` of program RAM (BIOS manual memory map; ares'
  sub bus decode).
- So a test cartridge can copy its own sub CPU program instead of the BIOS's and release the reset.
  Nothing on this path reads the BIOS ROM.
- Two sources agree; the first S1 test will confirm it in the emulator.
- The BIOS is still needed to boot discs (Mode 2, §4-2), so disc tests stay local.

### F3. Timebase decision

The Mega Drive master clock (NTSC 53,693,175 Hz, PAL 53,203,424 Hz, `Core/MD/MdTypes.h`) and the
sub CPU's 12.5 MHz have no small integer ratio. NTSC is 4.295454 master clocks per sub cycle and
PAL is 4.25627392.

| Option | Exact | Limits | Effect on existing code |
|---|---|---|---|
| A. Common tick (lcm of both rates / gcd) | Yes | NTSC 26.85 T ticks/s: `uint64_t` overflows after 190.9 hours (PAL 246.6 h) | Every Mega Drive component would change units |
| B. Keep master clocks; the sub CPU converts with a remainder | Yes | None (the remainder is < 12,500,000) | None outside the Sega CD unit |
| C. Floating-point ratio (as ares' CD audio counter) | No | Rounding can differ between compilers and architectures | None, but breaks `test:md-xarch` |

**Decision: B.** After each sub CPU step of `n` cycles:
- `num = n * mclkRate + remainder`;
- position += `num / 12500000`;
- remainder = `num % 12500000`.

Both the position and the remainder are serialized. This keeps every existing component on the
master clock and stays exact.

### F4. Cost of a second 68000 (measured)

**Method:** a throwaway patch (not committed) in a separate worktree, built from `sega-cd` at
`243c4f74`.
- A second `M68kCore` runs on a flat 512 KiB bus, executing the loop from
  `m68k-cpu-singlestep --bench`.
- After every main CPU instruction, `MdConsole::RunFrame` catches it up to the main CPU using
  option B.
- 3000 frames of `render_h40` through `bin/mesen-headless`, five runs each way, Apple M1 Max.
- To reproduce: [`mcd-s0/second-68000-stub.patch`](mcd-s0/second-68000-stub.patch) and
  [`mcd-s0/time_second_68000.py`](mcd-s0/time_second_68000.py); the script's docstring has the
  steps.

| | Median | Runs |
|---|---|---|
| Without the stub | 4.16 s | 5.06, 4.16, 4.18, 4.13, 4.16 |
| With the stub | 5.34 s | 5.34, 5.34, 5.36, 5.35, 5.34 |

**Results:**
- **Added cost:** 0.395 ms per frame (+28%). Emulation is still about 9.4 times faster than real
  time; it was 12 times.
- **Prediction:** the flat-bus benchmark (590 MHz-equivalent, so 12.5 MHz costs about 2.1% of a
  core, or 0.35 ms per frame) predicted most of this.
- **Option B checked:** after 3000 frames the stub had run 625,805,798 sub cycles (208,602 per
  frame; 12.5 MHz over one NTSC frame of 3420 × 262 master clocks is 208,601.9). Its master-clock position was 2,688,120,018 against the
  main CPU's 2,688,120,008, so it stayed within one sub instruction.
- **What's left out:** the real unit adds bus decoding, gate array synchronisation, the graphics
  chip, PCM and the CD controller. 28% is a lower bound for the CPU part.

**S1 performance target (judgment):** keep the whole unit under +50% frame time on the cartridge
benchmark. Measure it whenever a component is added.

### F5. Disc selection

`Emulator::LoadRom` tries `PceConsole` before `MdConsole` (`Core/Shared/Emulator.cpp`).
`PceConsole::LoadRom` claims every `.cue` and returns `Failure` if the firmware/disc checks fail
(`Core/PCE/PceConsole.cpp`), so a Sega CD disc never reaches the Mega Drive core today.

The S3 change: check for `SEGADISCSYSTEM` in the first data sector, and have both consoles return
`UnknownType` for discs that aren't theirs. This needs a regression test that a PC Engine CD image
still loads.

### F6. Hardware manual check (Ver 1.0, 1991-10-14)

Read: system modes (p. 5), bus construction (pp. 6-10), mapping (pp. 12-17), register table
(p. 19), timer and interrupt mask (p. 30). Page numbers are the manual's own.

- **System modes (p. 5).**
  - Mode 1 (boot from cartridge): cartridge at main `$000000`, the boot ROM from `$400000`.
  - Mode 2 (boot from disc): cartridge area at `$400000-$7FFFFF`, the boot ROM from `$000000`.
  - This matches the plan.
- **Bus switching (p. 6).** Each memory has its own bus selector and is accessed without wait
  states when nothing else competes for it. Who owns each memory:
  - Program RAM goes to the main CPU when `$A12001` has SBRQ=1 (bus request) or SRES=0 (sub CPU
    held in reset). The plan said "holds the bus"; reset also counts.
  - Word RAM: bank 0 goes to the main side when RET=1 in 2M mode or RET=0 in 1M mode. Bank 1
    goes to the main side when RET=1, in either mode. RET is set from the sub side at `$FF8002`.
  - Refresh, CD DMA and the graphics chip are bus masters on the same selectors (pp. 7-10). S1
    needs a decision on modelling their contention; the manual gives no cycle counts.
- **Sub CPU map (pp. 14-15).**
  - Program RAM `$000000-$07FFFF`, write-protectable from `$000000` to `$01FDFF` in `$200` units
    through main `$A12002`.
  - `$080000-$0BFFFF`: word RAM in 2M mode; in 1M mode, a "dot image" (decoded format, one 4-bit
    pixel per byte).
  - `$0C0000-$0DFFFF`: the sub CPU's 1M bank in 1M mode, unassigned in 2M mode.
  - Backup RAM `$FE0000-$FE3FFF`, odd bytes only (8 KiB).
  - PCM `$FF0000-$FF3FFF`, lower byte only.
  - Sub registers `$FF8000-$FF81FF`.
- **Main CPU map (p. 16).**
  - Boot ROM `$000000-$01FFFF`.
  - Program RAM window `$020000-$03FFFF`.
  - Word RAM `$200000-$23FFFF`. In 1M mode it is the bank at `$200000-$21FFFF`, plus cell-image
    views at `$220000` (32 cells), `$230000` (16), `$238000` (8), and `$23C000`/`$23E000` (4).
    These are the same four ranges as ares.
- **Register table (p. 19).** Main `$A12000-$A1202F` and sub `$FF8000-$FF8057` are as in the
  plan: reset/INT2, memory mode and write protect, CDC mode/host data, H-INT vector (main only),
  stop watch, communication flags/command/status, timer, interrupt mask, fader, CDD
  control/status/command, font colour/bit/data. The graphics chip's registers run up to
  `$FF8066`.
- **Interrupts (pp. 15, 30).**
  - Levels 1-6 as in F1; level 1 (graphics) is raised only in 2M mode.
  - Each level has an enable bit in `$FF8032`. The manual advises that level 2 be issued from the
    main CPU's V-INT.
- **Timer (p. 30).**
  - 8-bit, counts down from n every 30.72 µs; reaching 0 raises level 3; the interval is
    (n+1) × 30.72 µs.
  - 30.72 µs is exactly 384 cycles of 12.5 MHz, which agrees with ares' /384 divider and the
    12.5 MHz clock. The manual itself never states the CPU clock.

### Open for S1

- Contention between the sub CPU, refresh, CD DMA and the graphics chip on program RAM and word
  RAM: the manual says who can access what, not how long it takes. Judgment call for S1: no wait
  states at first (as the manual says for the uncontended case), labelled as an approximation.
- The VDP DMA word-RAM latch remains ares-only; it needs a test that shows the effect.
- Licences of `mcd-verificator` and the 240p Test Suite, before anything is vendored.
