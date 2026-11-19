# Sega 32X: verification findings

Plan: [`32x-feasibility.md`](32x-feasibility.md). Entries are dated. "Judgement" marks a decision
that no primary source settled; "unverified" marks a point still waiting for one.

## S0: sources and design (2026-11-19)

### F1: references pinned

| Reference | Revision | Licence | Use |
|---|---|---|---|
| ares `component/processor/sh2`, `md/m32x` | `140c0a3` (2026-11-16), sparse checkout `bin/ares-src` | ISC | Port base for the SH-2; main 32X reference |
| BlastEm `sh2.cpu`, `32x.c`, `32x_video.c` | `2230add` (2026-10-27), `~/src/nativekit/build/toolchains/src/blastem-inspect` | GPL-3 | Second reference (read, not copied) |
| SingleStepTests `sh4` | `48975cb` (2026-06-13), `bin/SingleStepTests-sh4` | MIT | Instruction test suite (F3) |
| Hitachi *SH-1/SH-2 Programming Manual*, 3rd edition (1995) | bitsavers `components/hitachi/superH/1995_SH1_SH2_Programming_Manual_3rd_Edition.pdf` (sha256 `9e204ecf45a49bc6…`) | — | Instruction definitions (C descriptions), execution states |

| Hitachi *SH7604 Hardware Manual* (ADE-602-085C rev. 4.0, 633 pages, text layer) | supplied by the user from Sega Retro, sha256 `262cfff2abec2fa0…` | — | On-chip modules, exception stacking, interrupt priorities, cache |
| Sega *32X Hardware Manual* (US, 90 scanned pages) | supplied by the user from Sega Retro, sha256 `4f99f6503dd98e84…`; OCR'd with tesseract 5.5.4 to search it (the OCR is noisy; facts below were read on the page text) | — | Clocks, memory maps, interrupts, registers |

Sega Retro's bot protection blocked automated downloads of the two hardware manuals; the user
downloaded them by hand (2026-11-19).

### F2: SH-2 clock = MD master clock × 3/7

ares creates the SH-2 threads at `system.frequency() / 7.0 * 3.0`; BlastEm clocks its PWM (which
runs on the SH-2 clock) at `MCLKS * 3 / 7`. **Confirmed** by the 32X hardware manual ("Clock
Used by the 32X", p. 52): Vclk = 7 Mck, Sclk = Vclk / 3, with fosc = 53.693175 MHz (NTSC) or
53.203424 MHz (PAL); the PWM section gives 23.01 MHz (NTSC) and 22.8 MHz (PAL). The ratio is exact,
so the 32X needs no remainder bookkeeping (unlike the Sega CD's 12.5 MHz sub CPU).

### F6: the plan's figures against the 32X hardware manual

- **Memory:** 2 Mbit SDRAM (256 KiB), frame buffer 1 Mbit DRAM × 2 (alternating draw/display), 4 KB
  SH-2 cache: as in the plan.
- **SH-2 map** (figure 3.2; cache-through addresses are + `$20000000`): boot ROM `$0`, system
  registers `$4000`, VDP registers `$4100`, colour palette `$4200` (words only), cartridge
  `$02000000-$023FFFFF` (blocked while RV = 1), frame buffer `$04000000`, overwrite image
  `$04020000`, SDRAM `$06000000-$0603FFFF`. VDP, palette and frame buffer can't be accessed while
  FM = 0. As in ares.
- **68000 map** (figure 3.1, p. 14): with ADEN = 1 the vector area `$000000-$0000FF` comes from a
  built-in ROM whose vectors point into a jump table at `$880200` (cartridge `$200`); the first
  4 Mbit of the cartridge is fixed at `$880000`, and `$900000-$9FFFFF` is a 1 MiB window into four
  banks (bank set register). With RV = 1 the cartridge returns to its normal mapping (for VDP DMA)
  and SH-2 cartridge accesses wait until RV = 0. "MARS" at `$A130EC`. When the 68000 and an SH-2
  access the same area at the same time, the SH-2 wins; otherwise first come, first served.
- **SH-2 interrupts** (p. 69 and "Restrictions concerning SH2 interrupt"): VRES 14, V 12, H 10,
  command 8, PWM 6, in that priority order; each "continues indefinitely until cleared"
  (level-held), masked per CPU in the interrupt mask register. CMDINT differs: masking it drops
  the request, and it is asserted again on unmasking if it was never cleared. Auto-vectors
  64 + level / 2 (SH7604 table 5.3).
- **For S2:** Sega forbids programs from using the SH-2 FRT and prescribes fixed settings (TIER
  `$01`, OCRA `$0002`, FTCSR `$01`, TOCR `$E2`), so the board appears to use the FRT output; to
  find out what it drives before modelling it.

### F3: the `sh4` suite as an SH-2 test

- **What it is:** 236 files, one per SH-4 encoding (× FPU mode bits), 500 tests each. Each test
  runs four instructions: `NOP`, the instruction under test, `ADD R1,R1` (the delay slot when it
  branches) and `NOP`; a branch target holds `ADD R2,R2`. Generated from Reicast's interpreter,
  so it is an emulator reference, not hardware.
- **Mapping decision:** run every `sz0_pr0` file whose encoding the SH-2 decodes (137 files); skip
  SH-4-only encodings (96 files). Compare R0-R15, PC, GBR, VBR, MACH, MACL, PR, SR masked to the
  SH-2's `$3F3`, every fetch address and every data access (address, size, written value), in
  order.
- **Excluded, with reasons:** TRAPA, RTE, SLEEP (the SH-4's exception model, with TRA/EXPEVT and
  SSR/SPC, differs from the SH-2's stack frames); LDC/LDC.L to SR when SR.RB flips (the SH-4
  switches register banks, 357 tests); STC SR and STC.L SR compare the stored value masked to
  `$3F3`. MAC.W and MAC.L aren't in the suite (it allows one data access per instruction).
- **Format notes:** a data access is logged in the cycle record of the *following* instruction
  (pipeline offset), so the harness matches accesses in order; a read-modify-write logs both in
  one record, read first. Addresses can be unaligned (random registers), so the harness bypasses
  the SH-2 area decoding (`SetFlatBus`).
- **Coverage:** of the 142 SH-2 instruction patterns, 140 are in the suite; the two missing are
  MAC.W and MAC.L, covered by scenarios (S1).

### F4: instruction timing from the programming manual

ares charges 1 cycle per instruction. The core charges the programming manual's "normal minimum
number of execution states" (instruction summary tables, appendix A), all checked against the
manual: BT/BF 3 taken / 1 not; BT/S, BF/S 2 / 1; BRA, BRAF, BSR, BSRF, JMP, JSR, RTS 2; RTE 4;
TRAPA 8; SLEEP 3; LDC.L @Rm+,SR/GBR/VBR 3; STC.L SR/GBR/VBR 2; TAS.B 4; AND.B/OR.B/XOR.B/TST.B
#imm,@(R0,GBR) 3; MUL.L, DMULS.L, DMULU.L 2 ("2 to 4"); MULS.W, MULU.W 1 ("1 to 3"); MAC.W 3
("3/(2)"); MAC.L 3 ("3/(2 to 4)"). Everything else is 1. Multiplier contention (the upper
bounds) and memory wait states are not in the core: wait states are the bus owner's job
(`AddCycles`), contention is not modelled (judgement; BlastEm picks middle values instead).

### F5: no BIOS in CI (decision pending)

The 32X boots through three Sega ROMs (68000 vector ROM, master and slave SH-2 boot ROMs). All
three are on this machine (`~/mame/roms/segacd/32X_{G,M,S}_BIOS.bin`; MD5 `6a5433f6…`,
`f88354ec…`, `7f041b6a…`), so local tests can boot the real path. CI needs replacement boot
ROMs. Decision for S2: small SH-2 boot programs written for this project, assembled by a script
in `tests/`, doing only what the synthetic cartridges need (set VBR and SP, copy the cartridge's
SH-2 code to SDRAM per the 32X header, jump to it). Not decided: whether the 68000 side needs a
replacement vector ROM or can use the cartridge's own vectors in the tests.

## S1: SH-2 core (2026-11-19)

`Core/MD/Sh2/Sh2Core.{h,cpp}` (interpreter, cache, INTC, DMAC, DIVU, FRT, WDT, SCI; BSC, UBC,
SBYCR stored), `Sh2Disassembler.cpp`, `Sh2Serialize.cpp`. Ported from ares (ISC); no recompiler.

### Results

`mise run test:sh2-cpu`:

- **Suite:** 68,140 passed, 0 failed (137 files). Skipped: 357 LDC SR tests with an SR.RB bank
  switch, 3 malformed records (below).
- **Scenarios:** 97 checks, 0 failed (85 at first; 12 added with the hardware-manual fixes): power-on and manual reset, TRAPA (frame and 8 cycles), RTE
  (SR restored before the slot, 4 cycles), delayed branches, MOVA in a delay slot, BT/BF/BT/S
  timing, illegal and slot-illegal instructions, IRL interrupts (acceptance, masking, no
  acceptance in a delay slot or right after LDC), priority (DIVU over IRL; IRL over DMAC at equal
  level), SLEEP and wake-up, MAC.W and MAC.L with S = 0 and 1, DIVU 32/32 and 64/32 including
  overflow, FRT compare match with CCLRA and its interrupt, DMAC auto-request, cache fill, hit,
  write-through and purge; DIVU latency and DVDNTH sign extension; FRT access through TEMP;
  DMAC round-robin vs fixed priority.
- **The harness catches bugs (mutation checks):** each mutation was applied to a copy of the
  core. Suite: ADDC carry from Rm (254 failures), PC-relative base +2 instead of +4 (1,019), BSRF
  return address (500), DIV1 Q update (232), SHAR without sign (225). Scenarios: ares' unsigned
  MACL in MAC.W (2), TRAPA return address (1), ares' DIVU threshold (1, after adding the
  `$FFFFFFFE / 2` case it first missed). Two mutations survived because they are equivalent:
  MOV.L @Rm+,Rm (the load overwrites the increment) and an unmasked LDC.L SR (the extra bits are
  not visible).
- **Sanitizers:** the suite and scenarios under UBSan (`-fsanitize=undefined
  -fno-sanitize-recover`): no reports. An ASan build hung in dyld's initializers before `main`
  (the Nix clang ASan runtime on this macOS), so ASan is still to run, on Linux or via
  `mise run build:asan`.
- **Performance:** the whole suite runs in 0.17 s (the harness is not a speed measurement; S2
  measures frame time).

### Malformed suite records

Three tests (`BT/S`/`BF/S` with displacement −2, i.e. a branch to itself) have corrupt fetch
records: the first fetch "value" is a 32-bit address, the second is 0. The harness skips any test
with a fetch value wider than 16 bits and counts them. Worth reporting upstream.

### Differences from ares

| # | Point | ares | Core | Source |
|---|---|---|---|---|
| 1 | DIV1 Rn,Rn | Reads Rm before shifting Rn | Shifts Rn, then reads Rm (so Rn,Rn uses the shifted value) | Programming manual's DIV1 C description (p. 66-67); the suite (Reicast) agrees: 43 tests failed with ares' behaviour |
| 2 | MAC.W with S = 1 | MACL zero-extended; MACH bit 0 set on overflow | MACL signed; MACH unchanged | Programming manual's MACW C description (the MACH lines are marked "SH-1 only") |
| 3 | DIVU 64/32 overflow threshold | `quotient > 0x7FFF7FFF` | `> 0x7FFFFFFF` | SH7604 10.3.3: overflow when the result exceeds signed 32 bits |
| 4 | DIVU overflow results; `INT32_MIN / -1`, `INT64_MIN / -1` | UB in C++ for the latter | DVDNTL = `$7FFFFFFF` (positive overflow) or `$80000000` (negative) | SH7604 10.3.3 and table 10.2 for OVFIE = 0. With OVFIE = 1, and for DVDNTH, the unit leaves its intermediate result after 6 cycles, which isn't modelled (remainder / saturated value instead) |
| 5 | DIVU overflow interrupt | Never raised | OVF & OVFIE request at DIVU priority | SH7604 10.2.3 (OVFI) |
| 6 | Timing | 1 cycle per instruction | Manual table (F4) | Programming manual |
| 7 | Slot illegal instruction, saved PC | Branch target | Branch target (same as ares) | SH7604 4.5.3 and table 4.11: "jump destination address of the delayed branch instruction". **Corrected 2026-11-19:** the first version of the core saved the branch's own address, a guess the manual disproved |
| 8 | Interrupt arbitration | External IRL checked first, then on-chip sources in a fixed order, each a one-shot latch | Highest level wins across NMI, IRL and on-chip; equal levels in the default order; on-chip requests are level-held (flag AND enable) | SH7604 4.4 and table 5.4 (default order NMI > user break > IRL > DIVU > DMAC0 > DMAC1 > WDT > REF > SCI ERI/RXI/TXI/TEI > FRT ICI/OCI/OVI); level-held: "TXI is requested when TDRE is 1", "output continuously until the CMF bit is cleared" (5.x, 7.x, 13.4) |
| 9 | FRT overflow interrupt | Uses the output-compare vector | Its own vector (VCRD) | SH7604 table 5.4 and 11.5 (ICI, OCI and OVI are separate sources) |
| 10 | DMAC | A channel waiting for DREQ blocks the other | Priority only between simultaneous requests: channel 0 first (PR = 0) or round-robin (PR = 1, added 2026-11-19) | SH7604 9.3.3 |
| 11 | TCR.IEDG | Written from bit 2, read at bit 7 | Bit 7 both ways | SH7604 11.2.6 |
| 12 | Cache address array read | `tag & 0x7ffff << 10` (operator precedence: returns garbage) | `(tag & 0x7FFFF) << 10` | Bug fix |
| 13 | Cache address array format | LRU written from data bits 11-6, read at bits 9-4 | Read: tag 28-10, LRU 9-4, V bit 2; write: tag and V from the address, LRU from data bits 9-4 | SH7604 8.4.9, figure 8.11 |
| 14 | Cache tags at power-on | Tag 0 valid | All invalid | The cache isn't initialised by a reset and must be purged by software (SH7604 8.5.1), so any start value is valid; judgement |
| 15 | Cache line fill | Adds 12 cycles itself | Four bus reads; wait states come from the bus owner | Design |
| 16 | Architectural PC | PC = instruction + 4, delay-slot mode register | PC = next instruction, explicit slot state | Design (debugger shows real addresses) |
| 17 | CCR.TW = 1 | Purges ways 0-1 | No purge: software must invalidate ways 0-1 before two-way mode | SH7604 8.4.5 (tags are compared on all four ways even in two-way mode) |
| 18 | FRT 16-bit registers (FRC, OCRA/B, FICR) | Bytes accessed directly | Through the 8-bit TEMP register: an upper-byte write is buffered until the lower-byte write; an upper-byte read latches the lower byte | SH7604 11.3 |
| 19 | DIVU timing | Instant | 39 cycles (6 on overflow); DIVU accesses during a division wait for the end | SH7604 10.3, 10.4.1 |
| 20 | DVDNT write | Sets DVDNTL only | Also sign-extends into DVDNTH (32/32 = 64/32 on the sign-extended dividend) | SH7604 10.2.2, 10.2.5 |

All the SH7604 points above were checked on 2026-11-19 against the hardware manual the user
supplied; none in this table is still unverified.

### Not modelled yet

DIVU intermediate results on overflow, the extra wait after a DIVU write, multiplier contention, watchdog-mode
reset, DMAC burst/cycle-steal timing and DACK, address errors, UBC breaks, SCI timing (transfers
are instant, as in ares), the FRT input capture, and the cycles of exception processing itself
(the manual's pipeline figures give 9-10 stages; the core charges the stacking accesses only).
