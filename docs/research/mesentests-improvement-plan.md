# Improving MesenTests: coverage gaps, material needed, and a plan

This document records what the fork needs in order to extend system-level regression testing
(`mise run test:ci`), and why. It complements the CPU-level verification in
`docs/research/{nes,gb,snes,sms}-verification-findings.md`. The SingleStepTests harnesses check
one instruction at a time against flat RAM. MesenTests is the only place where whole systems
(CPU + video + audio + DMA + mappers) are run, so fixes that change timing need it as well.

Snapshot: `master` @ `dad7a4bf` (after PRs #6–#8), 2026-11-05.

## Status

| Step (§8) | State |
|---|---|
| 1. Headless record mode, `tests/` folder in `test:ci` | **Done.** `tools/mtp-record` (`mise run test:record`), `tests/` with a manifest (`mise run test:record-fork`), and `test:ci` runs upstream, then `tests/`. See `tests/README.md` |
| 2. SMS Z80 timing ROM | **Done** (`tests/SMS/z80_timing`). Third-party SMS ROMs: not yet |
| 3. SA-1 ROM | **First version done** (`tests/SNES/sa1_timing`): BRK/COP and memory speeds. IRQ/NMI entry and conflicts: not yet |
| 4. 65816 edge-case ROM | Not started |
| 5. GBA | Not started (BIOS decision open) |
| Hardware runs | None yet |

Side finding: SMS recordings couldn't be reproducible because the VDP started on a random
scanline. Fixed for test mode (sms-verification-findings #10).

## 1. How MesenTests works today

**Source.** [nesdev-org/MesenTests](https://github.com/nesdev-org/MesenTests), cloned into
`bin/Tests` by `mise run test:ci`. It contains `.mtp` files only; nothing in this repo.

**Runner.** `test:ci` builds `PGOHelper` and runs `pgohelper <testsDir> citests`, which calls
`RunCiTests` (`InteropDLL/TestApiWrapper.cpp`):
- It collects every `.mtp` up to 5 folders deep and runs them on all CPU threads.
- **Skipped:** the whole `GBA/` folder ("GBA tests can't run without the GBA BIOS"), six GB
  tests that need the official boot ROM, and three SGB tests that need the SGB ROM.
- A test passes when every frame matches. If some frames differ but the last one matches, it
  "passed with warnings", which **CI counts as a pass**: only `RomTestState::Failed` (last frame
  wrong, or the test couldn't be loaded) fails the run. So intermediate frames can change
  without CI noticing. A failing test saves a screenshot of its last frame.

**The `.mtp` format** (`Core/Shared/RecordedRomTest.cpp`). It's a zip archive containing:

| Entry | Contents |
|---|---|
| `TestRom*` | The ROM itself |
| `TestMovie.mmo` | A Mesen movie (inputs from power-on, or from the current state) |
| `TestData.mrt` | `"MT2"`, a `uint32` hash count, then per entry a `uint8` repeat count and a 40-char SHA-1 of the frame buffer. Runs of identical frames are collapsed (up to 255 per entry) |

**Settings forced while recording and playing** (`RecordedRomTest::UpdateSettings`):
- RAM powers on as all zeros (every system).
- No run-ahead, no frame skipping.
- The sprite limit stays on.
- GBA doesn't skip the BIOS boot screen. WS uses its boot ROM if one is installed (and falls
  back to the post-boot state otherwise).
- NES region is PAL only for tests named `demo_pal` or `pal_*`.

**How tests were recorded before this plan.** Only from the UI's test menu (`RomTestRecord` /
`RomTestStop` in `TestApiWrapper.cpp`, `UI/Utilities/RomTestHelper.cs`). There's no headless
record mode.

### Current coverage

| System | `.mtp` files | Run in CI |
|---|---|---|
| GB/GBC | 264 | 258 (6 need the boot ROM) |
| NES | 248 | all |
| SNES | 91 | 88 (3 SGB tests need the SGB ROM) |
| GBA | 209 | **none** (BIOS) |
| WS | 15 | all |
| PCE | 3 | all |
| SMS / Game Gear / SG-1000 / ColecoVision | **0** (fork: 1, `tests/SMS`) | – |
| SNES coprocessors | 1 (`cx4test`) (fork: 1 SA-1, `tests/SNES`) | Cx4 only upstream. **No SA-1** upstream; no SuperFX, DSP-n, S-DD1, SPC7110 or ST01x |

612 tests pass, 218 are skipped (`test:ci`, 2026-11-05).

## 2. What a recorded test does and doesn't prove

A `.mtp` stores hashes of **MesenCE's own output**. It proves the output hasn't changed since
it was recorded, but **not that the output is correct**. Two consequences:

1. A test is only as good as the result it pins down. The useful ones are self-checking test
   ROMs that end on a PASS/FAIL or CRC screen, where that screen has been checked against real
   hardware, the ROM's documented expected output, or a trusted reference.
2. Any intended change to the picture (a PPU fix, or a timing fix that moves a raster split)
   fails existing tests. That's expected. The test then has to be **re-recorded**, with a
   findings entry explaining why the new output is the correct one.

So a good test ROM:
- **can be redistributed** (MesenTests and this repo are public; no commercial dumps);
- **checks itself**, preferably naming the failing case on screen;
- **ends on a stable screen** within a few seconds (CI runs everything, and repeated frames are
  cheap to store);
- **needs no BIOS or boot ROM**, or the runner has to skip it like the GBA folder;
- **is deterministic** from power-on with zeroed RAM.

## 3. Gaps left open by the CPU verification work

Each item below is listed as a gap in a findings log. It has been verified at CPU level only.

| Gap | Findings | What's unverified at system level |
|---|---|---|
| **SMS/GG/Coleco: no tests at all** | sms #9 | Z80 timing fixes (`LD (HL),n` 12→10, `INC/DEC (HL)` 10→11, `RLD` 14→18, indexed forms) relative to the VDP/PSG. Block I/O flags. `EX (SP),HL` write order with write-triggered mapper registers |
| **SA-1: no tests** | snes #15 | Vector-fetch cycles on BRK/COP/NMI/IRQ (the NMI/IRQ part is a judgment call). E-mode RMW NMI poll. The SA-1 wait-state model (BW-RAM 2 cycles, SNES-CPU bus conflicts, IRAM during FastROM, PRG-ROM jumps/odd branches) isn't covered by anything |
| **65816 behaviours the suite can't settle** | snes #7, #8, #9 | Whether the 5A22 turns the E-mode RMW dummy write into a /WR strobe. The E-mode `(dp,X)` wrap when DL ≠ 0 (MesenCE and ares wrap; the suite doesn't). `WDM`'s second cycle |
| **SPC700 I/O page** | snes "ideas" | Timers `$FA-$FF`, `$F0` speed/write-enable bits, `$F1` port clearing (4,465 suite tests skipped because they touch `$F0-$FF`) |
| **Interrupt timing** (65816, SA-1, Z80) | snes "ideas", sms "ideas" | NMI/IRQ entry, the dummy read before an interrupt, WAI wake-up, the DMA/HDMA `IrqLock` delay. Z80 IM 1/IM 2/NMI, the EI delay, `LD A,I` P/V on accept |
| **GBA: whole system** | – | No tests run in CI at all |
| **Game Gear CMOS Z80** | sms "ideas" | `OUT (C),0` outputs `$FF` on CMOS parts; MesenCE outputs 0 for all models |

## 4. Material needed

### 4.1 Test ROMs that already exist

Each one must have its **licence checked** before it is committed or recorded. Names below are
starting points to look for, not confirmed availability.

| For | Look for | Notes |
|---|---|---|
| SMS/GG Z80 | ZEXDOC / ZEXALL ported to the SMS (Frank Cringle's Z80 exercisers; SMS Power hosts ports) | ZEXALL checks undocumented flags, ZEXDOC documented ones. Long-running: minutes at full speed, so check that a CI run is practical |
| SMS/GG VDP and timing | Homebrew VDP / timing test ROMs on SMS Power | Look for ROMs that print results and cover VDP counters (V/H counter) or line-interrupt timing, which is where the Z80 timing fixes would show up |
| GBA | jsmolka's `gba-tests`; mGBA's test suite ROM | Both are commonly used and self-checking. Running them needs either a BIOS decision (§5.4) or a skip-boot-screen exception for these tests |
| SNES SPC700 I/O | blargg's SPC tests are already in MesenTests (`blargg-spc-6`, `blargg_2010-03-14`). Check what they cover before writing new ones | |
| SA-1 | **No known public SA-1 test suite** | Needs a new ROM (§4.2) |

### 4.2 Test ROMs to write (no outside material needed)

These follow the pattern of `docs/research/tla/rom/dmc_oam_overlap.s`: a small self-checking
ROM, built by a `mise` task, whose expected results come from documentation and are then
confirmed on hardware (§4.3).

1. **SMS Z80 timing ROM.** Uses the VDP V counter (or line interrupts) as a clock. It times
   blocks of `LD (HL),n`, `INC (HL)`, `RLD`, `LD (IX+d),n`, `INC (IX+d)` and `BIT b,(IX+d)` and
   prints the measured T-states next to the Zilog UM0080 values. Assembler: WLA-DX or z80asm
   (to be added to `flake.nix`).
2. **SA-1 ROM.** An SA-1 cartridge header plus ca65 (already in `flake.nix` for the NES ROM).
   It would test, using the SA-1 timer or the SNES H/V counters:
   - BRK/COP cycle counts on the SA-1;
   - SNES → SA-1 IRQ/NMI entry latency (tests the judgment call in snes #12);
   - BW-RAM vs IRAM access cost, and SNES-CPU bus conflicts;
   - E-mode RMW on an SA-1 register or a BW-RAM location (whether the dummy write lands).
3. **65816 edge cases** (main CPU): E-mode `(dp,X)` with DL ≠ 0 at a page end, `WDM` timing, and
   E-mode RMW on an MMIO register where a second write is visible (e.g. a register with a write
   side effect). These are the three snes #7–#9 questions.
4. **Interrupt scenarios**, if they're done as ROMs rather than as a C++ harness like
   `tools/gb-irq-scenarios`.

### 4.3 Hardware access (the highest-value material)

Every judgment call in the findings logs can be settled by running a test ROM on real
hardware. That needs:
- **SNES:** a console and a flashcart that supports the SA-1 (for example an FXPak Pro; confirm
  SA-1 support for the firmware in use). This settles snes #7 (5A22 write strobe), #8, #9 and
  #12 (NMI/IRQ part).
- **SMS / Game Gear:** a console and a flashcart (for example a Master EverDrive). The Game Gear
  is needed for the CMOS `OUT (C),0` question.
- **What to record:** a photo or capture of the result screen, the console model/revision, the
  flashcart and firmware, and the ROM's commit hash. These go into the relevant findings entry.
  The `.mtp` is then recorded from MesenCE once it matches.

### 4.4 Commercial games (local only)

A private folder of the owner's own dumps can serve as an extra regression net, but **must never
be committed**: games with known timing sensitivity, such as SA-1 titles (Super Mario RPG,
Kirby Super Star) or SMS/GG games with raster effects. These catch regressions, not
correctness, and don't count as a "source for the intended behaviour" under the contribution
policy.

## 5. Infrastructure work

### 5.1 Headless record mode

Add a way to record a `.mtp` without the UI. For example, a `pgohelper <rom> record <out.mtp>
--frames N`, or a small tool in `tools/` that loads a ROM, calls `RecordedRomTest::Record`, runs
N frames with no input (most test ROMs need none), and calls `Stop()`. A `mise run
test:record -- <rom> <frames>` task would wrap it. Needed so tests can be created and
re-recorded in CI-like conditions and in this session.

### 5.2 Where new tests live

The fork doesn't submit upstream, so new tests need a home in this fork. Options:

| Option | Pros | Cons |
|---|---|---|
| **A. `tests/` folder in this repo** (ROM sources + built ROMs + `.mtp`) | One repo; the ROM source, its `mise` build task and its recording are reviewed together | Grows the repo; binary files in git |
| B. A fork of MesenTests | Keeps binaries out of this repo; same layout as upstream | Two repos to keep in sync; `test:ci` has to clone the fork |

**Recommendation: A**, with only small, redistributable ROMs. `test:ci` would run `bin/Tests`
(upstream MesenTests) and then `tests/` (the fork's own), and fail if either fails.
`RunCiTests` already takes a folder.

### 5.3 Wiring into `test:ci`

- Run both folders and print one combined summary.
- Keep the skip lists in `RunCiTests` as they are. Add any fork-specific skips with a comment
  saying why, like the existing ones.
- Print the tests that "passed with warnings" (§1) by name, and count them in the summary. That
  shows timing changes that move intermediate frames while the end screen stays the same.
  Whether to fail on them is a separate decision.
- Optionally, a local-only `MESEN_PRIVATE_TESTS` folder (§4.4) that runs when the variable is
  set.

### 5.4 GBA in CI (decision needed)

All 209 GBA tests are skipped because the runner can't ship the GBA BIOS. Options:
1. Leave it as it is.
2. Let a locally installed BIOS (the user's own) enable the GBA folder in local `test:ci` runs,
   and keep CI skipping it.
3. Check whether MesenCE's HLE BIOS path, if any, can run some of the tests. They would then
   have to be re-recorded, since the boot screen differs.

Option 2 gives local coverage with no licensing problem.

## 6. Procedure for adding a test

1. Find or write the ROM (§4.1, §4.2). Record its source and licence.
2. If it's written here: add its source under `tests/<system>/src/` and a `mise` build task.
   Commit the built ROM only if the build isn't reproducible in CI.
3. Run it in MesenCE and check the result screen against the documented expected output.
4. If hardware is available, run it there (§4.3). Record any mismatch with MesenCE in the
   findings log, with the hardware result as the source.
5. Record the `.mtp` (§5.1) and commit it.
6. Add a line to the relevant findings log ("system-level coverage") and remove the matching
   "Gap" entry once the gap is closed.

## 7. Decisions and material needed from the owner

- **Where tests live:** option A or B (§5.2).
- **Hardware:** whether a SNES with an SA-1-capable flashcart and/or an SMS/GG with a flashcart
  is available, and who runs the ROMs (§4.3).
- **GBA BIOS:** which option in §5.4.
- **Licences:** a sign-off on each third-party test ROM before it's committed.
- **Private dumps:** whether to set up the local-only folder (§4.4), and which games.

## 8. Suggested order of work

1. Headless record mode (§5.1) and the `tests/` folder in `test:ci` (§5.2, §5.3).
2. SMS Z80 timing ROM (§4.2.1) and any licence-cleared SMS test ROMs. This closes sms #9.
3. SA-1 ROM (§4.2.2). This closes snes #15 at regression level, and sets up the hardware run
   for snes #12.
4. 65816 edge-case ROM (§4.2.3), to settle snes #7–#9 on hardware.
5. GBA, depending on the BIOS decision (§5.4).
