# Changelog

Changes in this fork (bgyss/MesenCE) since upstream MesenCE 2.2.1. Each entry links to the
findings log that holds its evidence: the reproduction, the source for the intended behaviour, and
the test that keeps it fixed.

## 2.2.1-fork.1 (2026-11-25)

The first release of this fork. Compared with upstream MesenCE 2.2.1 it adds three systems and
fixes emulation bugs found by differential tests, sanitizers and formal models.

### New: Mega Drive / Genesis

- A new console core: 68000 (ported from ares), Z80, VDP, YM2612 and PSG, for NTSC, PAL and
  Japanese consoles, with or without TMSS. Cartridges with battery RAM, serial EEPROM and the Sega
  mapper; 3- and 6-button pads, Team Player, Sega Mouse, Menacer and Justifier; Game Genie and Pro
  Action Replay cheats; the debugger, save states, rewind, movies, netplay and Lua.
- YM2612: Nuked-OPN2 (a cycle-accurate model of the chip die) is the default FM core; ymfm stays
  selectable as the faster option (Options → Mega Drive). Existing configurations keep the core they
  saved. ([findings C14, C15](docs/research/md-verification-findings.md))
- Sound balance: the PSG sits 2.8 dB lower against FM than in the first builds, matching Genesis
  Plus GX, PicoDrive and BlastEm (C16, C17). A DC blocker removes the YM2612's offset (C9).
- Accuracy fixes found by testing the whole library against Genesis Plus GX, PicoDrive and BlastEm
  (`mise run md-corpus`, [md-oracle-testing-strategy.md](docs/research/md-oracle-testing-strategy.md)):
  - The Z80's vertical-blank interrupt keeps its length (2,573 master clocks, measured in BlastEm)
    when a 68000 DMA stalls the bus (C13).
  - 68000 reads of the Z80 area without the Z80 bus return open bus, not `$FF` (Mega Games 3 in PAL,
    C10).
- Status: 1,119 of 1,130 cartridges in the local library agree with the reference emulators or
  differ for a reviewed reason (8, e.g. random attract modes); the 11 still flagged are listed in
  the findings.

### New: Sega CD / Mega-CD

- BIN/CUE discs with the Sega CD BIOS: sub CPU, program and word RAM, gate array, graphics chip, CD
  drive and controller, CD audio, RF5C164 PCM and backup RAM; Mode 1 cartridges (Flux) too. The sub
  CPU and its memory are in the debugger.
- All 31 discs tested reach their title screen, gameplay or FMV; 7 BIOS versions boot
  ([mcd-verification-findings.md](docs/research/mcd-verification-findings.md)).
- The PCM chip's level is halved to match the references (C12).
- Not yet supported: `.chd`, `.iso` + audio sets, the RAM cartridge, Sega CD 32X.

### New: 32X

- Both SH-2s (ported from ares, checked against 68,140 SingleStepTests), the 32X VDP and PWM sound.
  Runs with Sega's boot ROMs when present and MesenCE's replacement boot ROMs otherwise.
- 41 of 42 titles reach their title screen or gameplay with Sega's BIOS; Sega's Mars Check Program
  passes 159 of 161 tests ([32x-verification-findings.md](docs/research/32x-verification-findings.md)).
- Fixes since the 32X core was merged: SH-2 wait states and SDRAM burst timing from the
  Hardware Manual, the H32 picture, battery RAM through the bank window (36 Great Holes), PWM DC
  blocker and bus contention (C7-C9).

### Fixes in the other systems

- **NES:** OAM DMA restarted when a duplicated DMC DMA landed inside it (TLA+ model + test ROM);
  out-of-bounds reads in the HD Pack Builder (UBSan).
- **Game Boy:** `EI; HALT` with an interrupt pending returned past the HALT.
- **SNES / SA-1:** an extra cycle on read-modify-write instructions in emulation mode; SA-1
  vector-fetch cycles and NMI polling; three interrupt bugs on the 65816 and Z80.
- **GBA:** Thumb `PUSH` with an empty register list.
- **PC Engine:** decimal `SBC` with invalid BCD operands.
- **SPC700, Z80:** fixes from their SingleStepTests suites.

Logs: [docs/research/](docs/research/) (`*-verification-findings.md`).

### Tools and testing

- CPU test harnesses against the SingleStepTests suites (NES, Game Boy, SNES, SA-1, SPC700, Z80,
  GBA, PC Engine, 68000, SH-2), interrupt scenario harnesses, TLA+ models, save-state round trips,
  and recorded `.mtp` tests in `tests/` (all run by `mise` tasks; see [CLAUDE.md](CLAUDE.md)).
- `mesen-headless`: a native runner with bus traces, per-chip audio and state logs
  ([docs/headless.md](docs/headless.md)).
- CI runs ASan + UBSan over the whole Mega Drive console, including the vendored FM cores.
- Nix dev shell and `mise` tasks for building and testing.
