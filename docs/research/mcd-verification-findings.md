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

## S1: Mode 1 core without the BIOS (2026-11-18)

### What was added

- `Core/MD/Mcd/McdUnit.{h,cpp}`:
  - The sub CPU (`McdM68k`, the same ares-derived `M68kCore` as the main CPU) on its own bus.
  - 512 KiB program RAM and 256 KiB word RAM.
  - Gate array registers: main `$A12000-$A120FF`, sub `$FF8000-$FF81FF`.
  - Timer, stopwatch and interrupt levels 2 and 3.
- `MdMemoryManager`:
  - Main `$400000-$7FFFFF` and `$A12000-$A120FF` go to the unit when it is attached.
  - The version register's bit 5 clears.
  - The sub CPU catches up after every main instruction and before every main access to the unit.
- Attaching: `mesen-headless --md-mcd` only (a test hook like `--md-eeprom`). There is no UI
  setting yet, and nothing changes when the unit is not attached.

### Sources by behaviour

| Behaviour | Source |
|---|---|
| Mode 1 map: boot ROM `$400000`, program RAM window `$420000`, word RAM `$600000`; address bits 18-20 not decoded | Hardware Manual pp. 5, 12, 16; mirrors from ares |
| `$A12000`: SRES, SBRQ, IFL2 (level 2 to the sub CPU), IEN2 (read: the sub CPU's level 2 enable) | Hardware Manual p. 56; ares `io-external.cpp` |
| SRES = 0 forces the bus request | ares (comment: confirmed by Eke-Eke) |
| Main CPU has program RAM while SBRQ = 1 or SRES = 0; bank in `$A12003` bits 7-6 | Hardware Manual pp. 6, 57 |
| Write protection: sub writes below WP × `$200` dropped | Hardware Manual pp. 14, 57 |
| 2M word RAM: DMNA = 1 (main) hands it to the sub CPU, RET = 1 (sub) returns it; RET/DMNA read back as described | Hardware Manual pp. 23-25, 57; ares |
| 1M word RAM: RET = 0 gives bank 0 to the main CPU and bank 1 to the sub CPU, RET = 1 swaps | Hardware Manual p. 24; bank interleaving by word from ares |
| Cell image views at main `$220000-$23FFFF` | Hardware Manual p. 16 (dot/cell table); ares formulas |
| Communication flags (main = upper byte, sub = lower byte), 8 command and 8 status words | Hardware Manual pp. 19, 59; "every write lands in the flag's own byte" from ares |
| Timer: interval (n + 1) × 30.72 µs, level 3; stopwatch: 12 bits, 30.72 µs per count, cleared by a sub write | Hardware Manual pp. 30, 59 |
| Interrupt enables `$FF8032` bits 1-6; a source becomes pending only while enabled | Hardware Manual p. 30; ares `IRQ::raise` |

### Corrections, disagreements and judgment calls

- **Correction to the plan:** nothing in the gate array interrupts the main CPU. `$A12000`'s IFL2
  raises level 2 on the *sub* CPU (Hardware Manual p. 56, ares). The plan had it the other way
  round, and has been corrected.
- **1M swap request:** ares sets the request flag when the main CPU writes DMNA = 0. The manual
  says writing 1 requests a swap. Implemented as ares; no test depends on it. Open.
- **Power-on program RAM access:** ares lets the main CPU into program RAM only after a write to
  `$A12001`; the manual says SRES = 0 is enough. Implemented as the manual; the tests write
  `$A12001` first, so they hold either way.
- **Sub write to 2M word RAM it doesn't own:** the manual says the CPU hangs until a forced reset;
  ares waits until the RAM is handed over. Judgment call: the write is dropped.
- **Interrupt priority:** the highest pending level is presented to the 68000, as on the real IPL
  lines. ares checks level 1 first. Judgment call.
- **Not modelled:**
  - contention and wait states (the manual gives no figures);
  - the gate array's forced reset sequence;
  - H-INT vector redirection (only the register exists; it matters in Mode 2, S3);
  - the dot image (S2);
  - VDP DMA from the unit. The debug read path returns word RAM without ares' latch; this is
    untested.
  - Open bus on the sub side reads 0.
- **Scheduling:** the sub CPU may run up to one of its instructions past a main-CPU access.

### Tests (`mise run test:md-synthetic`, `tests/MD/synthetic/mcd_cases.py`)

Each case loads a sub CPU program through `$420000` and releases the CPU; no BIOS is involved,
which confirms F2 in the emulator.

| Case | Checks |
|---|---|
| `mcd_boot_comm` | version bit 5; SRES/SBRQ at power-on and after `$02`; program RAM window read-back; sub boot; command → status round trip; flag bytes, including a word write to `$A1200E` |
| `mcd_int2` | IEN2 and IFL2 as read by the main CPU; level 2 handler (vector `$68`) runs; IFL2 clears when taken |
| `mcd_timer` | n = 9 gives 10 stopwatch counts between level 3 interrupts; the stopwatch advances 543-544 counts per NTSC frame (sub clock vs main clock, F3) |
| `mcd_wordram_2m` | handover in both directions with RET/DMNA read-back and data both ways |
| `mcd_wordram_1m` | 1M mode, swap by RET, each CPU sees the other's data after the swap, cell image for dots 512-513 and 8-9 |
| `mcd_prgram_protect_bank` | WP = 2 blocks `$000300` and not `$000400`; window bank 1 is program RAM `$020000` |
| `mcd_busy` | timer, interrupts and the sub CPU running across the save point (for replay only) |

**Results**
- All 7 pass.
- The rest of `test:md-synthetic` still passes: 60/60 BlastEm cases and every MesenCE case.
- `test:md-replay`: 80/80 on both FM cores and both save points. `test:md-cheats`,
  `test:md-viewers` and `test:headless` pass. `test:ci`: upstream MesenTests pass, fork 12/12.
- `test:md-xarch` against an x86-64 runner of `6158504c` (`/usr/bin/clang++ -arch x86_64`, run
  under Rosetta 2): 80/80 identical, including the mcd cases and states cross-loaded between the
  two builds.

**Negative controls**
- Without `--md-mcd`, the six cases with expected values all fail: their polls never end.
- Mutations of `McdUnit.cpp`, one at a time: 10/10 caught. They were:
  - timer interval n;
  - a tick every 400 cycles;
  - a 12 MHz conversion;
  - no write protection;
  - the window ignoring the bank;
  - the same 1M bank for both CPUs;
  - no cell image;
  - RET not returning 2M word RAM;
  - IFL2 ignored;
  - the main flag landing in the low byte.

  The last one was missed at first: a byte write puts the same byte on both lanes. The word write
  in `mcd_boot_comm` was added to catch it.

**Speed:** 3000 frames, five interleaved runs each, Apple M1 Max, medians. The pre-S1 runner was
built from `c6a775a2` in a scratch worktree.

| Configuration | Median | Runs |
|---|---|---|
| Pre-S1 runner, `render_h40` | 4.06 s | 4.97, 4.11, 4.06, 4.05, 4.01 |
| S1, unit detached, `render_h40` | 4.04 s | 4.08, 4.04, 4.01, 4.08, 4.01 |
| S1, unit attached, sub CPU held in reset, `render_h40` | 4.28 s (+5%) | 4.25, 4.29, 4.21, 4.38, 4.28 |
| S1, `mcd_busy` (sub CPU running, timer interrupts every 6 ms) | 5.76 s (+42%) | 5.76, 5.82, 5.84, 5.68, 5.70 |

- **Unit detached:** no cost. The only new work on the hot path is one null check per main
  instruction.
- **Sub CPU running:** +42%, within the +50% target set in F4. The S0 stub on a flat bus cost +28%;
  the rest is the unit's bus decoding and timer.

### R5. Save-state replay now compares the whole final state

- **Repro:** dropping single fields from `McdUnit::Serialize` (stopwatch, timer counter,
  interrupt enable, clock remainder). Replay caught only the stopwatch: the other fields did not
  change any later main-CPU read in these short cases.
- **Fix:** `replay_check.py` now also compares every field of the uninterrupted run's
  `final.mss` with the loaded run's.
  - The one field that legitimately differs, `controlManager.pollCounter`, is ignored: the runner
    refreshes input once more on a load (R4).
  - The new `mcd_busy` case keeps the sub CPU, its timer interrupts, a masked pending level 2 and
    1M word RAM writes going across the save point.
- **Result:** 8/8 dropped fields caught: stopwatch, timer counter, interrupt enable, clock
  remainder, tick divider, word RAM mode, pending interrupts, reset pending.
- **Existing cases:** all 80 cases pass the stricter check, so no missing state was found in the
  existing Mega Drive core.

### Open after S1

- **Comparison with ares:** S1's exit asks for one. There is no ares capture harness yet, so the
  expected values come from the hardware manual, with ares used as a code reference only.
- **Contention and the VDP DMA latch** (see above), and the 1M swap-request disagreement.
- **Licences** of `mcd-verificator` and the 240p Test Suite, before anything is vendored.

## S2: graphics chip, PCM, backup RAM (2026-11-18)

### What was added

- `McdUnit`:
  - **Graphics chip:** stamp map → stamps → image buffer, level 1 at the end, `$FF8058-$FF8066`.
  - **Font registers:** `$FF804C-$FF8057`.
  - **1M dot image:** with write priority (normal, underwrite, overwrite).
  - **Backup RAM:** 8 KiB on the lower byte of `$FE0000-$FE3FFF`, saved as the `.brm` battery
    file.
  - **VDP DMA from word RAM:** the one-read delay.
- `McdPcm`:
  - The RF5C164 at sub `$FF0000-$FF3FFF`.
  - Its output is mixed into the Mega Drive's sound at each FM sample (latest PCM sample at that
    master clock), through a queue that save states include.
- Headless runner:
  - `pcm.raw` in `--chip-audio`;
  - `--battery FILE` to start from a save file;
  - `--save-battery` to keep the battery files in `OUT/saves`.

### Sources by behaviour

| Behaviour | Source |
|---|---|
| Font registers: one colour nibble per font bit, SC1 for 1, SC0 for 0, bit 15 → top nibble of `$FF8050` | Hardware Manual p. 34 (independent model `mcd_models.font_words`) |
| Graphics registers, trace vector format (13.3 start, 5.11 signed steps), stamp map entry (HFLP, RT1, RT0, stamp number), stamp storage as vertical cells, image buffer of vertical cells, repeat/outside, Vdot counting down, GRON, level 1 at the end, only in 2M mode | Hardware Manual pp. 35-53 |
| Rotation direction (counter-clockwise for RT0) | Manual figure p. 42 (90/180/270 pictures); ares `gpu.cpp` |
| Order of HFLP and rotation; stamp map address masks; nibble order in word RAM | ares `gpu.cpp` only (the figure for flip + rotation is not legible enough) |
| Graphics time per line: 13 + 2 × (offset AND 3) + 9 × ((H dots + offset − 1) >> 2) clocks, plus 3 per 192 for refresh; sub CPU word RAM accesses add 3 | Hardware Manual p. 53. **Judgment call:** the scan is partly illegible (read as above). ares uses 5 clocks per dot instead. |
| Dot image: one dot per byte, upper 4 bits read 0; priority modes, with the manual's own example values | Hardware Manual pp. 14, 23, 54; byte/bank arrangement from ares |
| Backup RAM 8 KiB, lower byte | Hardware Manual p. 15, BIOS Manual p. 1 |
| PCM registers, control register (ON, MOD, bank/channel), FD as an address step per sample, ST/LS, PAN nibbles (high = R), address read-back at `$10-$1F` | Hardware Manual appendix pp. 6-12 |
| PCM sample format (bit 7 set = positive, sign-magnitude), `$FF` loop marker, on/off bits active low, 16-bit clamp and 10-bit DAC rounding | ares `pcm.cpp` only |
| PCM mix level in the Mega Drive output | **Judgment call:** unscaled DAC value; no hardware level measured |
| VDP DMA from word RAM: one read late, first word undefined | Hardware Manual p. 61 ("output with a one cycle delay"; start one word later); ares `wramLatch` |

### Tests (`mcd_cases.py`, models in `mcd_models.py`)

| Case | Checks |
|---|---|
| `mcd_font` | two colour/bit combinations against the manual's mapping |
| `mcd_dot_image` | the manual's priority example (normal `$57`/`$B0`, overwrite `$57`/`$BA`, underwrite `$5A`/`$BA`); dot image read `$030C` |
| `mcd_backup_ram` | lower-byte reads and writes; the saved `.brm` holds them (`--save-battery`) |
| `mcd_backup_ram_load` | values from `tests/MD/mcd/backup_seed.brm` (`--battery`) |
| `mcd_pcm` | address read-back with the chip off; 400 output samples of two channels (full/half step, envelope, pan, loops) equal to the model |
| `mcd_graphics` | all 768 bytes of a 48x32 image built from every flip/rotation, scaling, right-to-left, a vertical vector and off-map dots, equal to the model; GRON, Vdot before and after; 9-10 stopwatch counts for the operation |
| `mcd_dma_wordram` | VRAM words 1-3 after a DMA started one word late |
| `mcd_graphics_busy` | graphics restarting from its own interrupt, with PCM playing, across the save point (replay only) |

**Mutations:** 16/16 caught. They were:
- PCM: sign, pan, loop to ST, DAC rounding, address byte order;
- graphics: HFLP, rotation direction, off-map dots, image column height, per-line time;
- dot image: underwrite/overwrite swap, read format;
- font colours;
- no DMA latch;
- backup RAM not saved, and not loaded.

The loop mutation was missed at first, because the loop point equalled the start point. The
case now loops elsewhere.

**Replay:** dropping a field from `Serialize` was caught for all nine fields tried:
- graphics counter, Vdot, image address, vector address, active flag;
- PCM channel address, channel enable, chip on, current output.

The vector address was missed at first, because the next operation overwrote the wrong dots. The
busy case now uses underwrite, so wrong dots stay.

**Suites:**
- `test:md-synthetic`: 60/60, plus all MesenCE cases.
- `test:md-replay`: 88/88.
- `test:md-xarch` against an x86-64 build of `3118094c`: 88/88 identical.
- `test:ci`: upstream and fork pass.

**Speed:** `render_h40`, 3000 frames, medians.

| Configuration | Median | Change |
|---|---|---|
| Unit detached | 4.14 s | unchanged |
| `mcd_busy` | 5.84 s | — |
| `mcd_graphics_busy` (graphics chip re-run about 57 times per frame, PCM playing) | 6.76 s | +63% |

- The graphics worst case is above the +50% target. It still runs about 7x faster than real time.
- Real-game load is measured in S5.
- One saving was made: silence is no longer queued while the PCM chip is off.

### Open after S2

- **RAM cartridge:** needs Mode 2 (the cartridge slot is free only when booting from disc), so it
  moves to S3.
- **PCM mix level and the graphics time formula:** both need hardware data.
- **ares comparison:** still not done; there is no capture harness yet.

## S3: discs, the CD drive and controller, CD audio, BIOS boot (2026-11-18)

### What was added

- **`McdCdc`:** the LC8951 CD data controller.
  - Registers through `$FF8004`/`$FF8006`.
  - Error-free decoding into its 16 KiB buffer.
  - Host reads at `$FF8008` (sub CPU) and `$A12008` (main CPU).
  - DMA to PCM RAM, program RAM and word RAM, one word per 6 sub cycles.
  - Level 5.
- **`McdCdd`:** the drive.
  - Command and status nibbles at `$FF8038-$FF804B`, with checksum.
  - TOC reading from the lead-in.
  - Report requests: position, relative position, track, disc length, first/last track, track
    start, errors.
  - Seek/play/pause, with a seek time from the radial distance.
  - Status interrupt (level 4) at 75 Hz.
  - Data sectors into the CDC.
  - Subcode buffer (level 6), filled with a Q channel computed from the position.
  - CD audio at 44.1 kHz through the fader, mixed into the Mega Drive output.
- **Disc boot (Mode 2):**
  - A `.cue` whose first data sector starts with `SEGADISCSYSTEM` loads in the Mega Drive core;
    `PceConsole` now returns `UnknownType` for such discs.
  - The BIOS comes from firmware: `McdUsa`/`McdEurope`/`McdJapan`, chosen by the disc header's
    region; UI hashes of the dumps tested here.
  - The unit sits at `$000000`, and `$000072` reads the H-INT vector register.
- **Headless runner:**
  - `--md-mcd-disc` (a disc in Mode 1);
  - `--md-mcd-bios`;
  - `cdda.raw` in `--chip-audio`.

### Sources

| Behaviour | Source |
|---|---|
| CDC registers, IFSTAT/IFCTRL bits, DBC/DAC/WA/PT, CTRL0/1, STAT0-3, HEAD0-3, address register auto-increment | LC8950/LC8951 Design Manual §11, tables 7-8 |
| `$FF8004`/`$A12004` (EDT, DSR, destinations), host data, DMA address bits | Hardware Manual pp. 26-27, 58 |
| Buffer layout (sync at the tail), DMA every 6 cycles, DBC underflow bookkeeping, error-free decoder | ares `cdc.cpp`, `cdc-transfer.cpp` |
| Drive protocol: command codes, report types and formats, checksum (`~sum & $F`), track start with the data flag in bit 3 of nibble 6 | ares `cdd.cpp` and MAME `megacdcd.cpp`, which agree. The Hardware Manual blacks the protocol out ("direct access prohibited"). |
| 75 Hz status interrupt (13.3 ms) | BIOS Manual p. 5; ares (434 × 384 cycles) |
| TOC read of 2 s from the lead-in, the 3-sector early landing of seeks, seek time `11 + 112.5 × Δradius` | ares only. MAME lands exactly on the target. |
| Fader: FD10-FD0 in bits 14-4, `$400` = full volume, one step per sample | Hardware Manual p. 31 (22 µs per step); ares |
| CD audio level: half scale | **Judgment call** following ares; no hardware level measured |
| Subcode buffer packing; Q channel computed from the position rather than read from the image | ares packing. **Judgment call:** the image's own subchannel is not used. |

### Tests

`mcd_cd_data` uses a synthetic disc built by `mcd_cases.build_disc`: track 1 is 200 data sectors
and track 2 is 100 audio sectors, written next to the cartridges by `gen.py`. It runs without the
BIOS and checks:
- the TOC report (tracks 1-2) once the drive is paused;
- the start of track 2 (00:04:50, audio);
- a sector's header (00:03:25, mode 1) and all 2048 data bytes read by the sub CPU (17 words
  compared);
- the next sector DMAed into word RAM;
- 2000 CD audio samples equal to the disc's;
- level 4 interrupts 434 stopwatch counts apart.

**Replay cases:** `mcd_cd_busy`, `mcd_cd_seeking` and `mcd_cd_audio` put a CDC DMA, a decoder
interrupt, a seek and a CD audio fade across the save point. `replay_check.py` gained a per-case
save point (`replay_warmup`), and `cdda.raw` is now part of its audio continuity check.

**`test:mcd-bios` (local only; needs the BIOS dumps):** 7 of 7 BIOS versions found here boot the
synthetic disc:
- US Model 1 v1.10, US Model 2 v2.00w and v2.21;
- EU Model 1 v1.00 and Model 2 v2.00;
- JP Model 1 v1.00p and Mega-CD 2 v2.00C.

Each one plays its intro music through the PCM chip and reaches its CD player. For the four whose
player starts on PLAY, pressing A/START plays track 2, and the CD audio output equals the disc
sample for sample.

**Reference check:** MAME 0.289's `segacd` driver (Nix binary) ran with the same BIOS
(`mpr-15045b.bin`) and disc. Both reach "Press the START BUTTON" and then the CD player listing
tracks 1-2. The player's elapsed time differs (00:06 here, 00:04 in MAME). That fits the different
seek landing (LBA −8 here, −5 in MAME), so it is left as a known reference difference.

### Bugs found with the BIOS

- **The pregap belongs to track 1:**
  - **Repro:** the US v1.10 BIOS seeks to 00:01:70/00:01:73 (LBA −8/−5) to read the boot area.
  - **Symptom:** the drive treated negative LBAs as "no track" and never decoded or advanced, so the
    BIOS fell back to its attract loop and never showed "Press the START BUTTON".
  - **Fix:** LBAs −150 to −1 are track 1, as in ares. The track report now gives 01 there (00 only
    in the lead-in before −150).
  - **Regression test:** `test:mcd-bios`; the mutation that reverts the fix fails it.
- **Seeks past the disc:**
  - **Symptom:** a seek beyond the last sector left the drive "playing" with no track, stalled.
  - **Fix:** it now reports the lead-out.
  - **How it was found:** a wrong seek in the first version of `mcd_cd_busy`.

### Checks of the checks

- **Mutations:** 12/12 caught. They were:
  - status checksum;
  - last track;
  - track start without the 2-second offset;
  - header frame not BCD;
  - the drive skipping a sector;
  - sync not moved to the tail;
  - the host data source not advancing;
  - word RAM DMA going to program RAM;
  - CD audio at full scale;
  - audio advancing a sector early;
  - pregap not part of track 1 (`test:mcd-bios`);
  - a 75 Hz step every 400 ticks.

  The last one was missed until the 434-count check was added.
- **Replay:** dropping each of 12 fields from `Serialize` was caught:
  - drive sector, drive status, 75 Hz counter, status pending, seek latency;
  - CDC block pointer, decoder interrupt, transfer active;
  - 44.1 kHz phase, sample offset, fader position and fader target.

  Several were missed at first. Fixing that took the per-case save points, `cdda.raw` in the
  replay comparison, publishing IFSTAT, and a fader toggled between half and zero (so the
  power-on value never matches).
- **Suites:**
  - `test:md-synthetic`: 60/60, plus all MesenCE cases.
  - `test:md-replay`: 92/92.
  - `test:md-xarch` (x86-64 build of `9a2237cf`): 92/92 identical.
  - `test:ci`: upstream and fork pass.
- **Speed:** the BIOS intro and CD player run 3000 frames in 6.88 s (2.3 ms per frame, about 7x
  real time).

### Not done in S3

- **RAM cartridge (backup RAM cartridge in the Mode 2 cartridge slot):** neither MAME nor ares
  implements it, and the only implementation known to me (Genesis Plus GX) is under a
  non-commercial licence. The BIOS manual describes its use, not its interface. Left out until a
  usable source exists.
- **Not modelled:**
  - `.chd` images;
  - CD-ROM XA (mode 2) sectors;
  - read errors;
  - de-emphasis;
  - the CDC status FIFO (SBOUT);
  - the gate array's forced reset.
- **Commercial games:** none tested yet (S5; there are no Sega CD game images on this machine).

## S4: integration (2026-11-18)

### What was added

- **Sub CPU in the debugger:**
  - New CPU type `McdM68k`, with its own debugger window (Debug menu → "Sega CD Sub CPU Debugger").
  - The window has disassembly, breakpoints, a trace logger, a call stack and the 68000 status
    view. `MdM68k::BuildState`/`ApplyState` are shared by both 68000s.
  - The sub CPU calls the same hooks as the main CPU: `ProcessInstruction`, 8- and 16-bit
    `ProcessMemoryRead`/`Write`, and `ProcessInterrupt`.
  - Vectors listed: reset `$04` and levels 2-4 (`$68`, `$6C`, `$70`).
  - Code/data logging covers program RAM. It isn't saved to a file, because program RAM is
    loaded by the game.
  - Checked through Lua in the headless runner (below). The debugger window in the UI app
    builds, but hasn't been exercised by hand.
- **Memory types (debugger, memory viewer, Lua):**
  - `McdMemory`: the sub CPU's address space.
  - `McdPrgRam`, `McdWordRam`, `McdBackupRam`, `McdPcmRam` and `McdCdcRam`.
  - `McdBios`.
  - Word RAM and the CDC buffer are now stored as big-endian bytes, so the viewers show them as
    the CPUs see them (no behaviour change: all 19 `mcd_*` cases pass unchanged).
  - Address translation works both ways for the sub CPU (`SubGetAbsoluteAddress` /
    `SubGetRelativeAddress`) and for the unit's main CPU windows (BIOS, program RAM window, word
    RAM in both modes and both boot modes).
- **Side-effect-free reads:** `McdUnit::SubDebugRead` serves the memory viewer and the
  disassembler (`McdM68k::Peek`). The earlier `Peek` went through `SubRead`. It skips the two
  registers whose reads have side effects, the CDC register data and host data (`$FF8006/8`).
- **Settings:** Emulation settings → Firmware has a "Sega CD / Mega-CD" section that selects the
  USA, Europe and Japan BIOS. The file names and SHA-256 lists match `FirmwareHelper::LoadMcdBios`.
  The region comes from the existing Mega Drive region setting (Auto reads the disc header).
- **Open dialog:** a "Sega CD / Mega-CD disc images" (`*.cue`) filter. `.cue` was already in "All
  supported files".
- **Headless runner:** done in S3 (`--md-mcd`, `--md-mcd-disc`, `--md-mcd-bios`; `.cue` as
  `--rom`).

### Bug found while testing

- `DebugUtilities::GetLastCpuMemoryType()` still returned `MdZ80Memory`. `McdMemory` was
  therefore not treated as a CPU address space, so a Lua callback on it compared absolute
  addresses and never matched. **Reproduction:** `tests/MD/mcd/debugger.lua` reported "sub CPU
  exec callback at $200 fired 0 times", although the sub CPU's instruction hook was called
  (checked with a temporary trace). Fixed by returning `McdMemory`.
- **Build note:** objects that include `DebugUtilities.h` indirectly kept the old inline
  function. `ScriptingContext.o` was missed by a `grep -l` touch, so after a header change,
  touch every `.cpp` file.

### Tests

- **`tests/MD/mcd/debugger_check.py`** (in `test:md-synthetic`) runs
  `tests/MD/mcd/debugger.lua` on `mcd_boot_comm` with the unit attached. It checks:
  - an exec callback on the sub CPU at `$000200` fires exactly once;
  - the program the main CPU copied into program RAM reads the same through `mcdPrgRam` and
    `mcdMemory` (byte and 16-bit reads);
  - a write through `mcdMemory` lands in program RAM;
  - the sizes of the six Sega CD memories.
- **Checks of the check:**
  - With `GetLastCpuMemoryType()` reverted, the exec check fails (the bug above).
  - Without the memory type argument (`emu.addMemoryCallback` then defaults to the main CPU's
    memory), it also fails.
- **Recording paths (judgment: checked once, not wired into a task):**
  - The mixed output (`audio.wav` in the headless runner, the same mixer output that the WAV and
    video recorders take) has peaks of 5258 (`mcd_pcm`) and 5188 (`mcd_cd_audio`), against
    555 with neither chip playing (`mcd_boot_comm`).
  - Save states, rewind, movies and netplay all use `Serialize`. That path is covered by
    `test:md-replay`.

- **Suites:**
  - `test:md-synthetic`: all cases, both FM cores, plus the debugger check.
  - `test:md-replay`: 92/92 in all three runs.
  - `test:md-viewers`: pass.
  - `test:mcd-bios`: 7/7.
  - `test:ci`: upstream 612 passed, fork 12 passed.

### Limitations

- **Recorded tests (`.mtp`) can't hold a disc.** `RecordedRomTest` stores the loaded file only,
  which for a disc is the `.cue` without its tracks. The PC Engine CD has the same limitation
  upstream. Movies are unaffected: they reference the loaded game.
- **No Mode 1 disc pairing in the UI** (a cartridge with the unit attached and a disc, as used
  by MSU-MD hacks). Mode 1 is reachable only through the headless runner's `--md-mcd` options.
  This is a judgment call: the main Sega CD use is booting discs.
- **The RAM cartridge is still missing** (see S3).

## S5: commercial software (2026-11-18, partial)

### What was available

No Sega CD disc images exist on this machine. I searched the home folder, `/Volumes` and Spotlight
for `.cue`, `.chd`, `.iso`, `.ccd` and `.mds`. The only Sega CD folders hold screenshots and a DAT
file. The GoodGen set has three Mega-CD-related cartridges:
- **Flux (Virgin, 1994):** a cartridge that plays audio CDs through the Mega-CD in Mode 1 with
  visuals.
- **Sega-CD BIOS:** BIOS dumps.
- **"Visual Scene Sega-CD Image":** a bad dump (`[b1]`), not used.

### Flux: Mode 1 with the BIOS

- **Bug:** in Mode 1 the unit had no BIOS, so its main CPU window `$400000-$41FFFF` was open bus.
  On hardware the BIOS is mapped there in Mode 1 (Mega-CD Hardware Manual, memory map), and Flux
  calls into it.
- **Reproduction:** Flux with a disc, run without the BIOS, produces no CD audio (peak 0).
- **Fix:** `MdConsole` now loads the BIOS for the cartridge's region when the unit is attached in
  Mode 1. The load is optional (`LoadMcdBios(..., required = false)` doesn't ask for a missing
  file), so the synthetic Mode 1 cases, which run without one, are unchanged.
- **Result:** with the European v1.00 BIOS and a generated audio CD (30 s at 440 Hz, then 30 s at
  660 Hz):
  - Flux boots: the "exp" logo appears, then the visualizer.
  - CD audio starts at 1.51 s.
  - 440 Hz plays at 18 s, and 660 Hz at 36 s (the drive moved on to track 2).
- **Test:** `tests/MD/mcd/mode1_flux.py`, run by `test:mcd-bios`. It's local only (the cartridge
  is commercial) and skips when Flux or the BIOS is missing. It checks all of the above, plus
  silence without the BIOS.
- **Judgment:** the start time and the track change match the disc layout. I have no hardware or
  MAME recording of Flux to compare the 1.51 s start against.

### Not qualified (no images)

S5's list needs disc images, and none were available:
- word RAM 1M streaming (FMV games);
- the graphics chip (Batman Returns, Silpheed);
- PCM-heavy audio;
- Sonic CD's CD audio;
- backup RAM saves in a game.

Those components are covered by the synthetic cases (S1-S3) and by the BIOS boots (7 versions),
but not by a commercial game. **S5 stays open until disc images are supplied.**
