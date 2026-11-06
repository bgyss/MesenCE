# GBA implementation: verification and testing findings

This is a running log of what the fork's verification tooling has found in MesenCE's GBA core
(`Core/GBA`). The method mirrors the other systems' logs
([nes](nes-verification-findings.md), [gb](gb-verification-findings.md),
[snes](snes-verification-findings.md), [sms](sms-verification-findings.md)). So far it covers the
ARM7TDMI CPU only.

Snapshot: `master` @ `c4cc4742` (after PR #12), 2026-11-06, aarch64-darwin.

## Summary

| # | Area | Finding | Status |
|---|---|---|---|
| 1 | ARM7TDMI CPU | All 45 encoding files (ARM and Thumb) match SingleStepTests on **registers in every bank, CPSR/SPSRs, the pipeline, data accesses in order and code-fetch addresses**: 2,146,838 of 2,250,000 tests. The other 103,162 are classified below; 0 are unexplained (after fix 2) | **Clean** |
| 2 | Thumb `PUSH {}` (empty register list) | Did nothing. On ARMv4 it stores PC+2 and moves SP down by `0x40`, like MesenCE's own empty-list Thumb STMIA/POP | **Fixed in this fork** |
| 3 | UNPREDICTABLE / undefined encodings | MesenCE and NanoBoyAdvance (the suite's source) differ only in encodings the ARM ARM leaves UNPREDICTABLE or undefined, plus MesenCE's deliberate CP14 model. Each category is listed below | **Divergence**, not changed |
| 4 | System level | MesenTests' 209 GBA tests are all skipped by `test:ci` (they need the GBA BIOS), so fix 2 is checked at CPU level only | **Gap** |

## How to reproduce

| Task | What it runs |
|---|---|
| `mise run test:gba-cpu` | `tools/gba-cpu-singlestep`. Clones [SingleStepTests/ARM7TDMI](https://github.com/SingleStepTests/ARM7TDMI) (about 1 GB) into `bin/`, reads the binary `.json.bin` files directly, and runs all 45 × 50,000 tests. Exits non-zero only on *unexpected* failures. `--dump <opcode>` prints MesenCE's bus activity and registers for the first test with that opcode |

---

## 1. ARM7TDMI vs SingleStepTests `ARM7TDMI`

**Method.** The harness compiles the **real** `GbaCpu.cpp`, `GbaCpu.Arm.cpp` and `GbaCpu.Thumb.cpp`
as `TestGbaCpu`, with `GbaMemoryManager` replaced by `TestGbaBus` and `GbaRomPrefetch` by a no-op,
the same technique as the Z80 and 65816 harnesses.

- **Reads.** Code reads return the opcode at `base_addr` and otherwise the address itself (the
  suite's convention). Data reads return the recorded transaction value. Alignment, sign
  extension and rotation of misaligned reads are done exactly as `GbaMemoryManager::Read`.
- **Registers.** The suite's `R` array is the user bank. `R_fiq`/`R_svc`/… hold each mode's own
  registers. This was established from the data: in SVC mode, a store's address matches
  `R_svc[0]`, not `R[13]`. These map onto MesenCE's `UserRegs`/`FiqRegs`/`SupervisorRegs`/….
- **Pipeline.** MesenCE keeps one more fetched word than NanoBoyAdvance (its Fetch stage), so it
  fetches the word at R15 one instruction earlier. The harness therefore drops NBA's first code
  fetch and MesenCE's last before comparing code-fetch addresses. It compares the final
  Execute/Decode opcodes with the suite's two pipeline entries.
- **Data accesses** are compared in order: address, size, direction, and the value for writes.
- **Not checked:** cycle counts. The suite's README says they aren't recorded reliably.

**Notes on the suite.**
- It's generated from NanoBoyAdvance (fleroviux's GBA emulator) and marked **EXPERIMENTAL**. Its
  README warns that edge cases with R15 as both writeback and destination may be wrong, and that
  alignment isn't restricted.
- The files hold 50,000 tests each, not the 20,000 the README says.

**Harness bug found on the way.** A first run showed LDM^ loading zeros. The cause was MesenCE's
`_ldmGlitch` carrying over from the previous test, because the harness only reset `_state`.
Fixed in the harness; it's not a MesenCE issue.

### 2. Thumb `PUSH {}` with an empty list (fixed)

**Reproduction.** 46 `thumb_push_pop` tests (opcode `B400`) failed:

```
0000B400 (thumb): R13=07E0E47C(exp 07E0E43C)
  actual bus:   (no write)
  expected bus: W4:07E0E43C=17F48CC4
```

**Source.**
- GBATEK's ARMv4 empty-list behaviour: R15 is transferred and the base moves by `0x40`.
- NanoBoyAdvance, per the suite.
- MesenCE's own handling of the same quirk in Thumb `STMIA`/`LDMIA` and `POP {}` (comment: "only
  R15 is stored/loaded, but address changes as if all 16 were written/loaded"). The value stored
  is PC + 2, the same as MesenCE's empty `STMIA`.

**Fix.** `ThumbPushPopReg` handles the empty `PUSH`. No assembler emits it, so the effect is
limited to hand-written code and tests.

### 3. Divergences in UNPREDICTABLE / undefined encodings (not changed)

The harness classifies a *failing* test into one of these categories only if it matches the
pattern. For the PC-alignment and MSR categories it also requires that nothing other than R15,
the pipeline and the code-fetch addresses differs. Everything else counts as an unexpected
failure.

| Category | Tests | What differs |
|---|---|---|
| `thumb-bcc-cond-e` | 50,000 (the whole `thumb_undefined_bcc` file) | Thumb conditional branch with cond `1110` (`DExx`), undefined in ARMv4T. MesenCE takes the undefined-instruction exception; NBA branches |
| `pc-low-bits` | 37,429 | PC written with misaligned low bits: an ARM-state LDR/LDM/data-processing/BX result with bits 1:0 ≠ 0, or Thumb bit 0 kept. UNPREDICTABLE in ARMv4 for ARM state. NBA keeps the raw bits in R15 and fetches word-aligned; MesenCE clears bit 0 only. On the GBA bus a word fetch ignores the low address bits, so the fetched data is the same |
| `r15-operand` | 11,656 | R15 as an operand or destination in a role the ARM ARM marks UNPREDICTABLE: multiplies, SWP, MRS Rd, register-shift data processing, writeback base, register offset, LDM/STM base |
| `msr-t-bit` | 2,386 | MSR changing the T bit (UNPREDICTABLE). MesenCE switches to Thumb fetching; NBA doesn't |
| `cp14` | 1,500 | MCR/MRC/LDC/STC/CDP on coprocessor 14. MesenCE models the ARM7TDMI's debug comms channel (MRC reads open bus, MCR updates it; comment in `GbaCpu.Arm.cpp`); NBA raises the undefined-instruction exception |
| `ldm-stm-user-writeback` | 191 | LDM/STM with `^` and writeback, UNPREDICTABLE (LDM(2)/STM(2)). MesenCE writes the base back while switched to the user bank; NBA writes it to the current mode's bank |

**Judgment call.** None of these is changed. Games don't use them, and in each case the ARM ARM
allows either result. `thumb-bcc-cond-e` and `cp14` are the two where hardware tests could pick a
side; both are listed under further ideas.

## 4. Coverage gap: GBA tests need the BIOS

`RunCiTests` skips the whole `GBA/` folder of MesenTests (209 tests, including jsmolka's
`gba-tests` and mGBA's suite ROM), because they can't run without the GBA BIOS. So no GBA test
ROM runs in this fork's checks. See `docs/research/mesentests-improvement-plan.md` §5.4 for the
options. Using a locally installed BIOS for local `test:ci` runs is the recommended one.

## Ideas for further GBA verification

- Run the MesenTests GBA folder locally with a user-supplied BIOS (plan §5.4).
- A hardware test ROM for `DExx` in Thumb and for MRC/MCR on CP14, to settle the two categories
  where hardware could disagree with MesenCE.
- Cycle timing: the suite doesn't record it reliably. The test ROMs in MesenTests (waitstates,
  prefetch) cover it once the BIOS question is settled.
- The save-state round trip for `.gba`, which also needs the BIOS.
