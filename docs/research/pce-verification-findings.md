# PC Engine implementation: verification and testing findings

This is a running log of what the fork's verification tooling has found in MesenCE's PC Engine
core (`Core/PCE`). The method mirrors the other systems' logs. So far it covers the HuC6280 CPU
only.

Snapshot: `master` @ `028ef139` (after PR #13), 2026-11-06, aarch64-darwin.

## Summary

| # | Area | Finding | Status |
|---|---|---|---|
| 1 | HuC6280 vs the Rockwell 65C02 (independent) | On the opcodes the two chips share, MesenCE matches SingleStepTests `rockwell65c02` in registers and RAM: 1,802,985 of 1,880,154 tests. The other 77,169 are documented HuC6280 differences, each checked against Mednafen's HuC6280 where it applies (after fix 3) | **Clean** |
| 2 | HuC6280 vs SingleStepTests `huc6280` (Mesen-generated) | A **regression baseline, not an independent source**. 625,232 of 627,519 match, including timing and bus cycles. The 2,287 differences are fix 3, an unobservable `TMA #$00` buffer, and TSB/TRB's Z flag, where MesenCE and Mednafen agree against the generator | **Clean** |
| 3 | Decimal-mode SBC with invalid BCD digits | MesenCE's result differed from both the 65C02 suite and Mednafen's HuC6280 (e.g. `$91 - $2E` gave `$6C`; both references give `$5C`) | **Fixed in this fork** |
| 4 | System level | MesenTests has 3 PCE tests, which pass. No test ROM checks decimal mode | **Gap** |

## How to reproduce

| Task | What it runs |
|---|---|
| `mise run test:pce-cpu` | `tools/pce-cpu-singlestep`, both suites. It clones [SingleStepTests/huc6280](https://github.com/SingleStepTests/huc6280) (about 140 MB) and the `rockwell65c02` folder of [SingleStepTests/65x02](https://github.com/SingleStepTests/65x02) (sparse checkout) into `bin/`. Exits non-zero only on *unexpected* failures |

---

## Method

`tools/pce-cpu-singlestep/singlestep.cpp` compiles the **real** `PceCpu.cpp` and
`PceCpu.Instructions.cpp` as `TestPceCpu`, with `PceMemoryManager` replaced by a `TestBus`: a
flat 2 MiB physical space behind the 8 MPRs. TAM/TMA work as in `PceMemoryManager`, including
the "last TAM value" buffer for `TMA #$00`.

### 1. Against the Rockwell 65C02 (independent)

The `rockwell65c02` tests come from Thomas Harte's Clock Signal, independently of Mesen. The
HuC6280 is a Rockwell 65C02 derivative, so on the opcodes the two share, their results must
agree, apart from documented HuC6280 differences.

- **Memory map.** The HuC6280 keeps its zero page and stack at logical `$2000`/`$2100`. The
  harness maps logical pages 0 and 1 both to physical bank 0 (MPR0 = MPR1 = 0, MPRn = n
  otherwise), so they land on the 65C02's `$00xx`/`$01xx`. Tests that touch `$2000-$3FFF` (an
  alias of bank 0 in this setup) are skipped: 369,846.
- **Opcodes that are different instructions on the HuC6280** aren't run: SXY, SAX, SAY, ST0-2,
  TAM, TMA, CLA/CLX/CLY, the block transfers, TST, BSR, CSL/CSH, SET, BRK (vector `$FFF6`), and
  the 65C02's multi-byte NOPs.
- **The T flag** (bit 5) starts clear, and P and a pushed P are compared without bit 5.
- **Only registers and RAM are compared.** The two chips' bus activity and timing differ by
  design.

**Known differences (documented HuC6280 behaviour):**

| Category | Tests | What | Check |
|---|---|---|---|
| `decimal-adc-sbc` | 35,890 | Decimal-mode ADC/SBC: the HuC6280 leaves V unchanged; the 65C02 defines it | Accepted only if MesenCE matches Mednafen's decimal formula, or if V is the *only* difference and MesenCE's V is unchanged |
| `tsb-trb-flags` | 25,893 | TSB/TRB: the HuC6280 sets N/V from memory and Z from the result; the 65C02 changes only Z, from A AND M | Accepted only if MesenCE's flags equal Mednafen's TSB/TRB macros |
| `undefined-db` | 8,787 | `$DB`, undefined on both: MesenCE runs a 1-byte NOP, the Rockwell suite a 2-byte one | A/X/Y must match |
| `bit-imm-nv` | 6,599 | `BIT #imm`: the HuC6280 copies bits 7/6 into N/V (Mednafen: for every addressing mode); the 65C02 only sets Z | – |

**Second source: Mednafen.** Mednafen's HuC6280 core (`mednafen/pce/huc6280.cpp`, via
libretro's `beetle-pce-libretro` mirror) is the reference for every category above. The
harness implements Mednafen's decimal ADC/SBC formulas and checks MesenCE against them.

### 2. Against SingleStepTests `huc6280` (Mesen-generated baseline)

Its README says it was **generated from Mesen** ("these tests are only as good as Mesen"). So it
can't show that MesenCE is correct, only where MesenCE now behaves differently from the code that
generated it. It's still useful because it checks timing and bus cycles (physical addresses),
which the 65C02 comparison can't.

It also isn't byte-for-byte this code. MesenCE's TSB/TRB Z flag hasn't changed since the
original import (`git log -L`), yet the suite expects a different Z. The generator was most
likely a port of Mesen's core into the author's own framework.

**Harness choices for this suite:**
- **Truncated tests skipped (12,481).** The generator stops recording after 500 cycles / 500
  RAM pairs, so long block transfers have no reliable expected state (suite README).
- **VDC delay off.** ST0/ST1/ST2 run without MesenCE's VDC access delay (`PceMemoryManager::WriteVdc`
  adds an idle CPU cycle). That's memory-system behaviour, and the generator didn't model it. The
  65C02 comparison doesn't run these opcodes.

**Known differences:**

| Category | Tests | What |
|---|---|---|
| `tsb-trb-z` | 1,492 | TSB/TRB Z flag: MesenCE (and Mednafen) use the result; the generator used A AND M. Accepted only if Z is the only difference |
| `decimal-sbc-fixed` | 782 | Fix 3: the generator recorded MesenCE's old decimal SBC. Accepted only if MesenCE's new result matches Mednafen's |
| `tma-zero` | 13 | `TMA #$00` returns the last value written by TAM, an internal buffer the tests don't specify |

### 3. Decimal SBC with invalid BCD digits (fixed)

**Reproduction.** 2,534 decimal-mode SBC tests in the Rockwell comparison failed on A, for
example `e9 16`: A=`$91`, operand `$2E`, carry 0. MesenCE gave `$6C`; the 65C02 suite gives `$5C`.

**Sources.** Mednafen's HuC6280 SBC (subtract each digit with borrow, then correct each digit that
borrowed) gives `$5C`. Run over all **44,848** decimal-mode SBC tests in the 65C02 suite,
Mednafen's formula matches every one (A, C, Z, N). The HuC6280's decimal logic comes from the
R65C02, so two independent implementations agree, and MesenCE was the outlier. Valid BCD inputs
were already right; only operands with a digit above 9 differed.

**Fix.** `PceCpu::SUB` (decimal path) uses the per-digit subtraction. V is still left unchanged
in decimal mode, and the extra cycle is still there. **Effect:** only decimal-mode SBC with
invalid BCD digits. Games rarely use decimal mode on the PC Engine, and valid BCD is unaffected.

**Harness note.** In T mode the decimal extra cycle is an idle instead of a dummy read. A first
version of the classifier read the wrong byte as the operand there; fixed.

## 4. Coverage gap

MesenTests' 3 PCE tests (`testvblank-rcr-to-vblank`, `testvdc-rcr-to-start-of-next`,
`Greyscale_Test`) pass, and so does the save-state round trip on them. None of them exercises
decimal mode.

## Ideas for further PCE verification

- A self-checking decimal-mode test ROM in `tests/PCE`, ideally also run on hardware.
- The ST0/ST1/ST2 VDC access delay and the block-transfer timing on the real `PceConsole`,
  which the flat test bus can't cover.
- The WonderSwan V30MZ: there's no direct suite. SingleStepTests' NEC V20 and Intel 80186 suites
  could check documented-flag semantics, with timing and undefined flags excluded.
