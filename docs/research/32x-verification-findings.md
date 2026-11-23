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

### F5: no BIOS in CI (decided in S2)

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
| 21 | CCR address | `$FFFFFE92` and `$FFFFFE93` | `$FFFFFE92` only; `$FE93` is unassigned (reads 0, writes ignored) | SH7604 appendix A register map (`H'FFFFFE93` to `H'FFFFFE9F`: "—"). NFL Quarterback Club writes CCR = `$11`, then 0 to `$FE93`: with the mirror its cache was off (md-verification-findings C6) |

All the SH7604 points above were checked on 2026-11-19 against the hardware manual the user
supplied; none in this table is still unverified.

### Not modelled yet

DIVU intermediate results on overflow, the extra wait after a DIVU write, multiplier contention, watchdog-mode
reset, DMAC burst/cycle-steal timing and DACK, address errors, UBC breaks, SCI timing (transfers
are instant, as in ares), the FRT input capture, and the cycles of exception processing itself
(the manual's pipeline figures give 9-10 stages; the core charges the stacking accesses only).

## S2: the 32X unit (2026-11-19)

`Core/MD/M32x/M32xUnit.{h,cpp}`: both SH-2s on the unit bus, SDRAM, system and communication
registers, SH-2 interrupts, the DREQ FIFO, the 68000's view (vector ROM, `$880000`/`$900000`
cartridge windows, frame buffer, registers at `$A15100`, "MARS" at `$A130EC`), scheduling.

### Sources and decisions

| Point | Source |
|---|---|
| Memory maps, register layouts (adapter control, interrupt control, bank set, DREQ, comm, interrupt mask, H count, clear registers) | 32X Hardware Manual figures 3.1-3.2, pp. 19-29 |
| RES: 0 = SH-2s in reset, set by the 68000's initial program | p. 19 (ares starts the SH-2s at power-on; MesenCE follows the manual, and Sega's BIOS boots with it) |
| SH-2 interrupt levels 14/12/10/8/6, auto-vectors 64 + level / 2 | pp. 69, 87; SH7604 table 5.3 |
| V, H, PWM requests latch only while enabled and stay asserted until cleared; CMD is gated by its mask | "Points to be aware of concerning interrupt" (p. 28). Judgment call: the manual describes the masked-after-request case; that the request does not latch while masked is inferred |
| INTM/INTS ($A15102) are plain read/write bits, writing 0 clears a request | Sega's Mars Check Program V1.0, test 3: writes `$02` with a request pending and expects `$02` (MesenCE read `$03`: fixed) |
| DREQ FIFO: one 8-word queue, `68S` cleared after `length` words, flushed when 68S is cleared | ares (the manual's two 4-word blocks are not modelled) |
| SH-2 wait states: boot ROM 1, system registers 1, VDP registers 5, palette 5, cartridge 6, frame buffer read 5, frame buffer write 1 through a 4-word FIFO (5 clocks per write while it is full); SDRAM read 10 per 8-word burst (a cache line fill, or any cache-through read), SDRAM write 1 per word | Hardware Manual 4.4, pp. 77-78 (minimum values; md-verification-findings C5, C6). Until C5 these were ares' figures (registers 1, palette 4, frame buffer write 4), and until C6 SDRAM cost 2 per word read (16 per line fill, 2 per cache-through read). The 12-clock burst is charged as 10 waits, the convention of the manual's other figures (1 wait = a 3-clock cycle); a cache-through long read is one burst; SDRAM writes keep 1 per word (2 clocks per word: a long write is 1 + 2 with the instruction's cycle); SH-2 DMAC reads keep 2 per word (DMA bus timing is not modelled). The two SH-2s share the bus: every access occupies it for its waits + 2 clocks, and an SDRAM read burst waits while the other SH-2's access is in progress (C7; arbitrating every access made four games drift from PicoDrive). Judgment calls |
| SH-2 cartridge reads while RV = 1 return the cartridge instead of waiting | Simplification (the manual: the SH-2 waits until RV = 0) |
| SH-2 scheduling: the CPU that is behind runs next, one instruction at a time; both catch up to the 68000 after each 68000 instruction and before each 68000 access to the unit | Design (ares syncs the SH-2s every 10 cycles and the 68000 every 50) |
| The VDP is brought up to date before the SH-2s run, and runs them up to each line's start before raising that line's H/V interrupts | Fix: a 68000 stall (VDP DMA) made the VDP catch up several lines at once and the SH-2s took one merged H interrupt instead of one per line (Mars Check Program tests 55/56; MesenCE lost 18 of 262 per frame) |

### Replacement boot ROMs (F5 decided)

`tools/m32x-boot/gen_boot.py` assembles MesenCE's own boot programs (`Core/MD/M32x/M32xBootRom.h`)
from the manual's boot protocol (section 5.1, figures 5.1-5.3): cache purge and enable, the MARS
user header's initial data load, the header's checksum word to comm8, "M_OK" / "S_OK", VBR and the
start address. The 68000 vector ROM follows "ROM Access When Using the 32X" (vectors at
`$880200 + 6n`). Not done by the replacements: the SDRAM test and clear, security, BSC set-up,
and the sleep until the 68000 wakes the SH-2s. **Revised in S5:** S2 left out the bank-switch
helpers Sega's vector ROM holds at `$C0-$FF` ("no game is known to call it"). That was wrong:
Knuckles' Chaotix calls `$C0`. S5 also adds a hand-over delay and GBR; see "Replacement boot ROMs"
under S5.
The Egypt sample renders the same picture with Sega's BIOS and with the replacements.

### Tests

`tests/MD/synthetic/sh2asm.py` (an SH-2 assembler, checked by assembling every instruction form and
disassembling the result with `Sh2Core::Disassemble`) and `m32x_cases.py`: synthetic 32X
cartridges that boot through the replacement boot ROMs (runnable in CI), with expected values from
the manual: boot handshake and IDs, CMD interrupt and its mask, V/H interrupts and HCount, H
interrupts across a 2048-word 68000 DMA, slave/master SDRAM sharing, DREQ FIFO with SH-2 DMA,
frame buffer fill and swap, 68000 VDP/palette access, cartridge windows and vector ROM, PWM FIFO
and timer interrupt. All pass (`mise run test:md-synthetic`); the first 10 also replay identically
from a save state (`replay_check.py`). Mutation checks: a wrong HCount comparison, the old INTM
behaviour and the old scheduling each fail their case.

### Sega's Mars Check Program V1.0 (hardware test)

Run locally with the user's BIOS and cartridge dump (161 register and function tests). After the
two fixes above: 159 pass. Remaining: tests 55/56, "SH2 MASTER/SLAVE H INTERRUPT (MD 1)": with HEN
= 1 and HCount = 0 the program counts H interrupts over 30 frames and expects `$1E00` (256 per
frame); MesenCE (like ares and BlastEm, which also raise one per line) gives 262 per frame. Not
resolved; hypotheses: no H interrupt on six lines around vertical sync, or SH-2 accesses stalled
while the 68000's VDP DMA holds the bus. Recorded as a known divergence.

## S3: VDP and PWM (2026-11-19)

| Point | Source |
|---|---|
| Packed pixel, direct colour and run-length modes; line table; dot shift | Manual pp. 36-45 |
| Priority: PRI = 0 puts 32X pixels with the through bit in front of the MD planes and the others behind; PRI = 1 the reverse; MD colour code 0 (backdrop) always shows the 32X | Manual figure 3.9 (p. 46), as ares |
| Frame buffer swap at VBlank (FS takes effect at once in VBlank or with mode 0); FEN during a fill or a pending swap | ares; fill time 7 + 3 x (length + 1) SH-2 cycles is ares' "according to official docs" |
| Fill wraps within a 256-word row; 8-bit `$00` writes to the frame buffer don't go through; the overwrite image skips zero bytes | ares; the overwrite rule is also in the manual |
| PWM: a sample every (cycle - 1) SH-2 clocks, 3-word FIFOs (oldest dropped when full), timer interrupt and DREQ1 every TM samples | ares; output level centred on half the cycle, half scale in the mix: judgment call |
| Composition | The VDP marks backdrop pixels (bit 11) when a 32X is attached; the 32X composites each line at its end in 15-bit colour (bit 15); `MdDefaultVideoFilter` converts both. The sub-pixel offset of the 32X image in H40 (ares: about 3 1/4 pixels) and H32 are not modelled |

Tests: `m32x_render`, `m32x_priority_32x` (PRI = 1, direct colour), `m32x_priority_md` (PRI = 0,
run-length), `m32x_dot_shift`: pixels of the final picture checked against the expected colours.
Sega's sample programs (Egypt, Gnu Sierra, Runlength Mode Test, Texture Test, SOJ, Celtic PWM test)
show their pictures.

## S4: integration (2026-11-19)

- Debugger: `CpuType::M32xMaster` / `M32xSlave` with `Sh2Debugger` (breakpoints, stepping, call
  stack across delayed branches, code/data logging of SDRAM, trace logger, interrupts),
  `Sh2DisUtils`, `Sh2TraceLogger`, expression tokens r0-r15/sp/sr/pc. SH-2 hooks run only while a
  debugger is attached (`Sh2Core::SetDebugHooks`), so emulation pays nothing otherwise; cache
  hits are reported too. Debugger addresses are physical (29 bits): cache-through addresses show
  as their cached ones.
- Memory types `M32xMasterMemory`, `M32xSlaveMemory` (CPU spaces, `$00000000-$0603FFFF`),
  `M32xSdram`, `M32xFrameBuffer` (both buffers), `M32xPalette`, `M32xBootRom`;
  `GetLastCpuMemoryType()` moved to `M32xSlaveMemory`. The Sega CD BIOS and the 32X boot ROM now
  count as ROM for the "uninitialized read" warning (both were reported as uninitialized RAM).
- UI: debugger menu entries, SH-2 status view, trace logger settings, firmware settings for the
  three boot ROMs (optional), `*.32x` in the file dialogs and folder scans.
- Test: `tests/MD/m32x/debugger.lua` via `debugger_check.py` in `test:md-synthetic` (exec and write
  callbacks on the SH-2s, SDRAM and CPU-space reads and writes, memory sizes); a wrong expectation
  makes it fail.

## S5: games (2026-11-19)

### Method

The local 32X library: 42 titles (one dump per title; commercial games, Sega's sample programs and
the Mars Check Program). Each runs headless for 3,600 frames with Sega's BIOS (`--md-32x-bios`),
and for 1,800 frames with the replacement boot ROMs. START is pressed every 300 frames, and a
screenshot is taken every 300 frames. Pass: the title reaches its title screen or gameplay and the
run exits cleanly. Black screens and hangs were traced with Lua scripts (CPU positions, comm
words, 68000 exception vectors, memory dumps) and the disassemblers.

### Results with Sega's BIOS

41 of 42 reach their title screen or gameplay, and none crash. Highlights: Knuckles' Chaotix and
Brutal (heavy SH-2 sync), Doom (run-length mode), Virtua Fighter, Virtua Racing Deluxe and Star
Wars Arcade (both SH-2s, cache), Metal Head and Tempo (DREQ), and the Celtic PWM test. The Celtic
test plays its one sample (0xB6E84 samples at 22 kHz, about 34 s) and then goes silent, as the
program intends.

The one exception is the ECCO the Dolphin CinePak demo. Sega's master boot ROM compares the
cartridge's 68000 start-up code with its own copy (`$22A-$238`), and on a mismatch it writes
"SQER" to comm0 and stops (`$248`). The demo's start-up code starts 16 bytes later than the
standard one (`$3F0-$3FF` are zero), so the BIOS rejects it. That is the BIOS's own check, not
an emulation bug. The replacement boot ROMs have no security check, and the demo plays with them.

### Fixes

| Title | Symptom | Cause and fix | Source | Test |
|---|---|---|---|---|
| NBA Jam TE, NFL Quarterback Club | Blue stripes | Their start-up check probes the Acclaim EEPROM at `$200000` with RV = 1 and stops when nothing answers. `MdEepromDatabase` had no entries for the 32X dumps: added (24C04 and 24C02, Acclaim board) | MAME's 32X list (NBA Jam TE: "Missing I2C hookup so POST fails with blue/black vertical stripes (same I2C type as plain MD version)"); NFL QB Club: the Mega Drive version's chip, judgment call | Both reach their intros (local); the EEPROM itself is covered by the MD EEPROM cases |
| Metal Head | Black after the SEGA logo | The master waits for FEN = 1 with no fill running. FEN now also reads 1 in a refresh window at the start of each HBlank (`MdVdp::Is32xRefreshWindow`) | BlastEm: the first 6 pixels of HBlank (`32x_video.c`); ares reports FEN during the VDP's refresh slots. Window: judgment call | `m32x_fen_refresh` (fails without the window) |

### Replacement boot ROMs

With the replacement ROMs, 8 titles stayed black at 1,800 frames (Chaotix, Virtua Racing Deluxe,
Tempo, Toughman Contest, 36 Great Holes, Sangokushi IV, Motocross Championship, RBI Baseball 95).
They all boot with Sega's BIOS. Three causes:

1. **Vector ROM helpers.** After the boot handshake, Chaotix and Virtua Racing Deluxe call
   `jsr ($00C0).w` with a1 = `$A130F1`. Sega's vector ROM keeps two routines in the reserved
   vector slots `$C0-$FF`. Each sets RV, writes the bank set registers (`$C0`: d0.b to (a1);
   `$D4`: 8 bytes from (a0)+ to `$A130F1-$A130FF`, preserving d7/a1), then clears RV. The
   manual requires RV = 1 for those writes (p. 17). The replacement vector ROM now implements both
   entry points, hand-assembled in `gen_boot.py`. Without them, the 68000 executed vector words
   as code and took an illegal-instruction exception.
2. **Hand-over time.** Sega's master BIOS sleeps only while ADEN = 0. It starts when the 68000
   releases RES (frame 2), and writes "M_OK" about 80 frames later. Carts' 68000 start-up code
   relies on that time: it swaps the frame buffer with FM = 0 before it waits for the checksum.
   With an instant hand-over, the application's SH-2 code took FM first, and the 68000 looped
   on FS forever (Toughman Contest, Tempo, 36 Great Holes, Sangokushi IV). The replacement master
   now waits 16 VBlanks after loading the initial data. Judgment call: no source gives the BIOS
   time as a requirement; 16 frames is well under the real BIOS's 80 and enough for every
   title here.
3. **GBR at entry.** Both Sega boot ROMs set GBR = `$20004000` and jump to the application with
   it. RBI Baseball 95 and Motocross Championship read `@(0,GBR)` (FM) before setting GBR
   themselves. The replacements now set GBR the same way.

The handover is covered by `m32x_handover`: GBR at the master's entry, and the helpers' first
words at `$C0`/`$C8`/`$D4`. It fails when the GBR set-up and the `$C0` helper are removed. The
other 32X cases now run 17 frames longer (`BOOT_FRAMES`), as does `tests/MD/m32x/debugger.lua`.

After the three fixes, all 42 titles reach their title screen, gameplay or test screen with the
replacement boot ROMs (1,800 frames), including the ECCO CinePak demo.

### Audio against PicoDrive (2026-11-20)

Reported: Virtua Fighter played its music but no sound effects. To check audio without
listening to each title:

- `tools/libretro-capture/capture.py`: a headless libretro host (Python, ctypes). It runs a core
  (PicoDrive 1.99 from RetroArch here) with the same input script format as `mesen-headless`, and
  writes the mix and screenshots.
- `tools/audio-compare/compare.py`: aligns the two mixes by their loudness envelopes (RMS per
  50 ms), then fits the reference's energy as a weighted sum of MesenCE's per-chip energies.
  `mesen-headless --chip-audio` now also writes `pwm.raw`. The fit gives each chip's level
  relative to MesenCE's mix, the windows where sound is missing or extra, and the octave-band
  spectra of both mixes.
- `tools/audio-compare/run_library.py` / `mise run test:md-audio-ref -- <rom dir>`: runs every
  ROM in attract mode with no input, which keeps both emulators on one timeline, and flags
  outliers. It works for Mega Drive ROMs too, and `--core` takes any libretro core (Genesis Plus
  GX for the Mega Drive).

Loudness alone was not enough. Virtua Fighter's envelopes correlated at 0.98 while the bug below
was present. Only the spectra showed it: 95 % of MesenCE's energy was below 150 Hz during a
PWM-only sound, against 9 % in PicoDrive.

| Finding | Cause and fix | Test |
|---|---|---|
| PWM held at one level per frame (Virtua Fighter's sound effects and every PWM sound in the library: right loudness, no content) | `MdApu` mixed `M32xUnit`'s *current* PWM output when it caught up. The 32X runs ahead of the APU (it catches up after each 68000 instruction; the APU runs in batches), so a whole batch, usually a frame, got the newest level. PWM levels are now queued with the master clock of their PWM tick and consumed at each APU sample's clock, like the Sega CD PCM chip's (`McdPcm::GetOutput`), and saved in save states. PWM level changes per second in Virtua Fighter went from 58 to about 10,700. Spectral distance from PicoDrive went from 3.2 dB to 0.6 dB. | `m32x_pwm_stream` (a square wave at about 22.5 kHz; fails with the old mixing); `pwm.raw` added to `replay_check.py` |
| No sound at all in Blackthorne, Toughman Contest, T-MEK and World Series Baseball '95; no PWM in Star Trek | Their Z80 sound drivers reach the 32X through the Z80's 68000 bank window: they poll INTM/INTS (`$A15102/3`) and raise CMD interrupts. MesenCE's window served only cartridge, work RAM and save RAM: register reads returned a debug peek and writes were dropped, so Blackthorne's Z80 spun on `$A15102` forever. Z80 window accesses to the 32X registers now act as 68000 byte accesses. | `m32x_z80_window` (the Z80 reads comm0 and raises a CMD interrupt; fails without the routing) |

Library results (42 titles, 60 s of attract mode, replacement boot ROMs) after both fixes:
- 28 titles pass, and 5 test programs make no sound in either emulator.
- Remaining flags, none of them a missing-sound case:
  - **Timing, not audio:** Darxide and World Series Baseball '95. Their attract modes run on
    different timelines in the two emulators (screenshots), so the envelopes can't be aligned.
  - **Spectral differences of 1.5-3.4 dB** in Blackthorne, Brutal, Primal Rage, NBA Jam TE, WWF
    Wrestlemania Arcade, the Mars Check Program and the Celtic PWM test. The sound is present;
    MesenCE has less energy around 400-1,600 Hz in WWF Wrestlemania (-6 dB). Not investigated.
  - **PWM level:** relative to FM, PicoDrive's PWM is a median 3 dB louder than MesenCE's (range
    -5 to +7 dB across titles). PicoDrive scales a full-width pulse to ±32768
    (`pwm_mult = 0x10000 / cycles`), MesenCE to ±16384 (S3, judgment call). The hardware's
    analogue mix is not measured, so the level is left as is.
- **Mega Drive check:** Sonic & Knuckles + Sonic 2 and + Sonic 3 against Genesis Plus GX:
  spectral distance 0.4 dB, envelope correlation 0.98; PSG 1-2 dB quieter in GPGX relative to FM.

PicoDrive is a reference, not ground truth. A flagged title needs a look at which emulator is
wrong.

### Open issues

- **Speed.** After Burner Complete, the slowest title, runs 3,600 frames in 59.7 s headless on its
  own (about 60 fps, real BIOS; Apple Silicon, 10 cores): real time with no headroom. Profile of the
  emulation thread: the SH-2 interpreter dominates, with per-instruction overhead in
  `RunPeripherals` (FRT/WDT) and `CheckInterrupts` at about a third of it. Candidates: run the
  on-chip peripherals lazily (on register access and at the next event) and skip the interrupt
  scan while no source can exceed the mask. Not done in S5.
- Carried over: H interrupts 262 vs 256 per frame (Mars Check Program 55/56), the 32X image's
  sub-pixel offset, SH-2 cartridge reads while RV = 1 (no wait), palette/frame buffer access
  waits.
