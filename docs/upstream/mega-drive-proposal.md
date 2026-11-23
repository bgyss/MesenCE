# Proposal to upstream MesenCE: Mega Drive / Genesis support

This folder holds what is needed to offer the fork's Mega Drive core to
[nesdev-org/MesenCE](https://github.com/nesdev-org/MesenCE). Nothing here has been sent upstream.

- Branch: `md-upstream-proposal` (fork), based on upstream `master` (`a8e5d023`).
- Upstream's [CONTRIBUTING.md](https://github.com/nesdev-org/MesenCE/blob/master/CONTRIBUTING.md)
  sets three conditions that matter here:
  1. **"AI-generated code is prohibited."** This branch is LLM-written (Claude), reviewed and
     tested as described below. It cannot be merged under the current policy; the point of
     offering it is to ask whether the policy could allow evidence-backed contributions.
  2. **Contributions are licensed under MIT and must not contain GPL code or libraries.** The
     branch therefore leaves out the fork's optional Nuked-OPN2 FM core (LGPL-2.1). Third-party
     code that remains: the ares 68000 core (ISC, notice kept in `Core/MD/M68k/ares/LICENSE`) and
     ymfm (BSD-3, already in upstream). Facts used from MAME (BSD-3) and ares (ISC) are cited in
     the source comments. One item for the maintainers to judge: the DRAM refresh model
     (2 68000 cycles every 128) uses BlastEm's figures (GPLv3); the code was written for MesenCE,
     but the constants and model come from there (`Core/MD/MdMemoryManager.h`).
  3. **"Please consult with the team before making any significant changes."** So the first step
     is an issue or discussion (text below), not a pull request.

Suggested order: open the issue, link the branch, and open the PR only if the maintainers want to
look at it.

## Verification of the branch itself (2026-11-17)

The branch was built from scratch on macOS arm64 (core and UI, no errors) and checked against the
fork's tests by overlaying the fork's headless runner on a throwaway copy:

- MesenTests: 612 / 612 pass. The fork's recorded tests: the SMS Z80 timing test and all 10 Mega
  Drive tests pass; the SA-1 timing test fails as expected (it needs a separate fork fix to the SA-1,
  not part of this branch).
- Synthetic cartridges: 60 / 60 against the BlastEm reference, the 13 MesenCE cases pass, audio
  17 / 17, save-state replay 73 / 73, mid-line check passes. The three Lua-driven accessory cases
  pass only with commit 6 (without it, the Lua input never reaches ports 2 or the Team Player's
  sub-ports).
- The app runs the synthetic cartridges from `--testRunner` without errors.
- Windows/MSVC and Linux builds of this exact branch have not run yet; the fork's CI builds the
  same code (plus Nuked-OPN2) on all platforms.

## 1. Issue / discussion text

> **Title:** Proposal: Mega Drive / Genesis support (and a question about the AI-code policy)
>
> I've been maintaining a fork of MesenCE ([bgyss/MesenCE](https://github.com/bgyss/MesenCE)) as
> an experiment in LLM-assisted emulator development where every change has to come with
> evidence: a reproduction, a source for the intended behaviour, a regression test, and a
> findings log. That work produced a Mega Drive / Genesis core for MesenCE, and I'd like to offer
> it upstream.
>
> I know CONTRIBUTING.md prohibits AI-generated code, and this core was written with an LLM
> (Claude), so I'm not asking you to merge it as things stand. I'm asking whether you would
> consider a policy that allows LLM-written code when it comes with the kind of evidence below,
> and if so, whether you'd want this core.
>
> **What it is** (branch: `md-upstream-proposal`, on top of current `master`):
> - Cartridge games: 68000 (ares-derived, ISC), Z80 (the SMS core's Z80, extracted into a shared
>   engine), VDP (all display modes, shadow/highlight, interlace, DMA and FIFO timing, mid-line
>   changes), YM2612 (ymfm) and PSG; NTSC/PAL/Japan, with or without TMSS.
> - Battery RAM, serial EEPROM saves (35 known ROMs), the Sega bank mapper (SSF2).
> - 3/6-button pads, Team Player, Sega Mouse, Menacer and Justifier.
> - Cheats (Game Genie, Action Replay), save states, rewind, movies, netplay, the debugger
>   (68000 and Z80: breakpoints, trace, CDL, memory, tile/tilemap/sprite/palette/register and
>   event viewers), Lua, and a Mega Drive settings page.
>
> **How it was checked** (all in the fork, with sources and results in
> [md-verification-findings.md](https://github.com/bgyss/MesenCE/blob/md-support/docs/research/md-verification-findings.md)):
> - 68000 against SingleStepTests m68000: 317,242 tests pass (258 known divergences, listed).
> - Z80 against SingleStepTests z80: 1,604,000 / 1,604,000 (this found SMS Z80 bugs; the fixes
>   are the first two commits of the branch and also apply to the SMS core).
> - 60 synthetic cartridges compared with BlastEm captures (bus reads, frames, per-chip audio),
>   plus 13 MesenCE cases with expected values (EEPROM boards, mapper, accessories, mid-line
>   rendering). Save-state replay is bit-identical on all of them; captures are identical across
>   arm64 and x86-64; ASan/UBSan runs are clean.
> - The existing MesenTests suite still passes (612 tests).
> - Performance: the 68000 core runs at 31-69 M instructions/s on x86-64 CI machines; adding
>   mid-line rendering did not change frame time.
>
> **What it isn't:** Sega CD, 32X, Pico and SMS mode are not included. Gaps compared with Genesis
> Plus GX, BlastEm and ares are listed in
> [md-feature-parity.md](https://github.com/bgyss/MesenCE/blob/md-support/docs/research/md-feature-parity.md).
> It has not yet been tested on a large set of commercial games.
>
> If the answer is no, that's fine; the fork will keep it. If it's "maybe", I'm happy to split it
> into smaller PRs, change anything you'd like, and walk through any part of it. The fork's
> verification tooling (CPU test harnesses for 8 CPUs, save-state round trips, TLA+ models) and
> the bugs it found in existing cores could be a separate conversation.

## 2. Pull request description (if invited)

> **Title:** Mega Drive / Genesis support
>
> Adds a Mega Drive / Genesis console (cartridges only). Disclosure: written with an LLM
> (Claude) and checked as described below; see issue #<n> for the policy discussion.
>
> **Commits**
> 1. `SMS: Z80 timing, WZ and undocumented-flag fixes`: found with SingleStepTests z80.
> 2. `SMS: NMOS LD A,I/R P/V quirk on interrupt; R increments during HALT`.
> 3. `Core: share the Z80 and SN76489 between the SMS and Mega Drive cores`: `Z80Engine<THost>`
>    and `Sn76489` extracted from `SmsCpu`/`SmsPsg` with no behaviour change (the SMS core runs
>    on them; MesenTests unchanged).
> 4. `Mega Drive / Genesis core`: `Core/MD`, emulator/debugger/cheat/settings integration,
>    InteropDLL, Visual Studio projects.
> 5. `UI: Mega Drive support`: settings and input pages, debugger views, menus, localization.
> 6. `Lua: emu.setInput ignored its port argument`: an existing bug (any port or sub-port
>    argument addressed port 0), found by the accessory tests; independent of the rest.
>
> **Licensing:** MIT for the new code (as CONTRIBUTING requires). Third-party: ares 68000 (ISC,
> notice kept), ymfm (BSD-3, already vendored). Facts cited from MAME (BSD-3, EEPROM wiring,
> Team Player and mouse protocols) and ares (ISC, EEPROM game list). Please check the DRAM refresh
> constants, which come from BlastEm (GPLv3); I can replace them if needed.
>
> **Testing:** see the issue. The tests live in the fork (headless runner, synthetic cartridges,
> CPU harnesses); they are not part of this PR, but I can propose any of them separately.
>
> **Not included from the fork:** the optional Nuked-OPN2 FM core (LGPL), the headless runner and
> its hooks in `Emulator`, and the fork's dev environment (Nix, mise) and CI jobs.
