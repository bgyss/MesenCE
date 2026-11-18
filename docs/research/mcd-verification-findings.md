# Sega CD / Mega-CD verification findings

Findings for the Sega CD work planned in [`sega-cd-feasibility.md`](sega-cd-feasibility.md). Each
entry gives the source and how far it has been checked. "Two sources" means two independent
documents or implementations agree; it is not a hardware test.

## S0: sources and design (2026-11-18)

Status: S0 is done except for one source. The Mega-CD hardware manual could not be downloaded
automatically (Sega Retro's wiki pages are behind bot protection), so register-level details for
the gate array and the graphics chip still rest on ares and MAME. The manual still has to be read
before those parts are written in S1/S2. Nothing here changes emulator code.

### Sources pinned

| Source | Revision / version | Licence | Used for |
|---|---|---|---|
| ares `ares/md/mcd` | `65c6fc2f` (master, 2026-11-18; last change to `md/mcd` `0b9b8bce`, 2026-07-24) | ISC | Primary reference implementation |
| MAME `src/mame/sega/megacd.cpp`, `src/mame/shared/megacdcd.cpp`, `src/devices/sound/rf5c68.cpp` | `ac08a4c2` (master, 2026-11-18) | BSD-3 | Second reference |
| Sega, *Mega-CD BIOS Manual* | Ver 2.00, 1992-02-24 (Sega Retro scan, `MCDBios.pdf`) | Sega confidential document, read for facts only | Sub-CPU memory map, boot sequence, interrupt levels, CD drive behaviour |
| Sanyo, *LC8950 & LC8951 Design Manual* | Sega Retro scan (139 pages, OCR) | Read for facts only | CD controller registers |

Genesis Plus GX was not consulted (non-commercial licence), as for the cartridge core.

### F1. Hardware figures in the plan, by source

| Figure | BIOS manual | LC8951 manual | ares | MAME | Status |
|---|---|---|---|---|---|
| Sub CPU is a 68000 at 12.5 MHz | | | `Thread::create(12'500'000)` | `SEGACD_CLOCK 12500000` | Two implementations; manual pending |
| Program RAM 512 KiB at sub `$000000-$07FFFF`, vectors at `$000000` | Memory map p. 4 (`$000000` vectors to `$07FFFF`) | | `pram 512_KiB`, sub bus `0x000000-0x07ffff` | | Two sources |
| Program RAM write protection, in 512-byte units | "Write protect area" `$000000-$005400` (p. 4) | | `address >= pramProtect << 9` | | Two sources (the unit size is from ares) |
| Word RAM 256 KiB; sub `$080000-$0BFFFF` (2M) or `$0C0000-$0DFFFF` (1M bank) | | | `wram 256_KiB`, sub bus decode | | One implementation |
| Main CPU sees program RAM only while it holds the sub CPU's bus | Mode 1 boot transfers programs before releasing the sub CPU (p. 30) | | `if(!io.request) return data` | | Two sources |
| 1M mode "cell image" view at main `$220000-$23FFFF` (four address-swizzle ranges) | | | `bus-external.cpp` | | One implementation |
| VDP DMA from word RAM goes through a one-word latch | | | `wramLatch` while `vdp.dma.active` | | One implementation; the effect (games add 2 to the source) is folklore until a test shows it |
| Main CPU's level 4 (H-INT) vector redirected by the gate array | | | `io.vectorLevel4` returned at `$70/$72` | | One implementation |
| Sub CPU interrupts: 2 = from main ("software V-INT"), 4 = CD drive, 5 = CD controller, 6 = subcode | p. 5 ("level 2 (software V-INT)", "level 4 (CDD)", "level 5 (CDC)", "level 6 (SCD)") | | IRQ sources | | Two sources |
| Sub CPU interrupts: 1 = graphics, 3 = timer | | | IRQ sources | | One implementation |
| CD drive interrupt every 13.3 ms (75 Hz) | p. 5 ("level 4 interrupts with 13.3 msec interval") | | | | Manual |
| CD controller buffer 16 KiB, "up to 5 frames" | p. 1 | | `cdc.ram 16_KiB` | | Two sources |
| Subcode buffer: 128 bytes in the gate array | p. 1 | | `cdd.subcode[64]` (words) | | Two sources |
| Backup RAM 8 KiB at sub `$FE0000` (odd bytes) | p. 1 ("up to 8K bytes") | | `bram 8_KiB`, sub `0xfe0000-0xfeffff` | | Two sources (address from ares) |
| PCM: RF5C164, 8 channels, 64 KiB, output at 12.5 MHz / 384 | p. 1 ("[SDC] PCM sound generator chip") | | `pcm.ram 64_KiB`, `12'500'000 / 384.0` | `RF5C164(..., SEGACD_CLOCK)` | Two implementations |
| CD controller, CD drive, timer and PCM clocked from one 12.5 MHz / 384 divider | | | `MCD::step` | | One implementation |
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

### Open for S1

- Read the Mega-CD hardware manual (gate array registers at `$A12000`/`$FF8000`, word RAM
  switching, the graphics chip). Every row above marked "one implementation" should get a second
  source or be labelled as ares-only in the code.
- Licences of `mcd-verificator` and the 240p Test Suite, before anything is vendored.
