# Mega Drive / Sega CD / 32X / Sega CD 32X: automated testing against oracle emulators

Status: strategy and methodology (2026-11-20). P0-P2 are implemented (section 0.1); the rest is
plan unless it says **exists**. It plans how to compare MesenCE's Mega Drive family against
reference emulators across a whole commercial library, mostly without anyone playing the games
by hand. Locally first, then in the cloud. It builds on the tools the M/S phases already
produced (see "What already exists").

## 0. Summary and the most expedient path

**The key point: you don't need to record sessions in the oracle.** A play session is
*power-on state + settings + one input log*. The input log is emulator-independent. Record it
once, from any source, and replay it in MesenCE and in every oracle. Then compare what each
emulator produced, frame by frame. Every emulator involved is deterministic from power-on, so
two runs of one session diverge only where the emulators differ.

**Where sessions come from, without manual play** (cheapest first):

1. **Attract mode:** no input, 1-3 minutes per title. This already exists for audio
   (`test:md-audio-ref`).
2. **Menu walker:** START every few seconds, then a fixed button pattern. This **exists**
   (`tools/audio-compare/make_input.py`).
3. **TAS movies from TASVideos:** complete playthroughs, already made by others.
   - Hundreds of published Genesis movies, plus Sega CD and 32X ones. Most are BizHawk `.bk2`,
     recorded on Genesis Plus GX (Mega Drive / Sega CD) or PicoDrive (32X); older ones are Gens
     `.gmv`.
   - Each comes with the ROM hash it syncs on. This is the single biggest source of deep,
     whole-game coverage, at no recording cost.
4. **Novelty-search explorer:** a bot that branches from save states, keeps inputs that reach
   new screens or RAM states, and writes the input log from power-on to each discovery. It
   covers titles without a TAS.
5. **Human sessions:** only for specific bugs or titles the bot can't get through (adventure
   games, menus that need text input).

**Comparison levels** (cheapest first):

- **L0 health:** no oracle needed (crash, hang, black screen, silence, exception vectors).
- **L1 functional:** normalised screenshots and audio.
- **L2 state:** per-frame hashes of work RAM, VRAM, CRAM, VSRAM and Z80 RAM, with first
  divergence.
- **L3 bus trace:** a diff of the bus trace around the first divergence (BlastEm through
  nativekit only).

Results are clustered by symptom. Each cluster becomes a minimised synthetic reproduction. The
fix then follows the contribution policy: a reproduction, a source, a regression test, a
findings entry.

**Expedient path**, in order. Each step is useful on its own:

| Step | Work | Result |
|---|---|---|
| 1 | You provide the materials (section 12), mainly the Mega Drive library and the Sega CD 32X discs; inventory script with No-Intro / Redump hash matching | Corpus manifest |
| 2 | `mesen-headless --state-log` (per-frame hashes) and the matching dump in `tools/libretro-capture/capture.py` | L2 comparison possible |
| 3 | Attract + menu walker, whole library vs GPGX (MD/MCD) and PicoDrive (32X), L0-L2 | First ranked bug list, overnight on the 10-core Mac |
| 4 | TASVideos import: fetch, match hashes, `.bk2`/`.gmv` → input JSON, replay | Whole-game coverage for hundreds of titles |
| 5 | Triage tooling: first divergence, consensus vote, clustering, evidence bundle | Bugs ready for a fix agent |
| 6 | Mesen-only goldens for every session that matches the oracles | Regression net without oracles, runs per commit |
| 7 | Explorer for titles without a TAS | Coverage for the rest |
| 8 | Cloud: Nix-built OCI image, private ROM bucket, job queue, scheduled fix agents | Scale and continuous runs |
| ∥ | Sega CD 32X support (Mode 2 + 32X; not emulated today, see 10.1) | Last platform enters the pipeline |

**Throughput, measured 2026-11-20** (`mesen-headless`, Apple Silicon, one core per run):

- Knuckles' Chaotix (32X): 1,800 frames in 28.1 s (64 fps, about real time).
- Sonic CD: 1,800 frames in 3.1 s (580 fps). That run covers the BIOS screens and boot.

A first full pass is about 100 core-hours (estimate in section 11.4). That's one night on the
local machine. **The cloud is not needed for the first pass.** It pays off for repeated passes
on every change and for running fix agents in parallel.

### 0.1 Status (2026-11-20)

Decided by the user: local runs first, the cloud (AWS) tabled as an aspirational goal; automated
TASVideos fetching allowed, done as respectfully as possible (P3); the pipeline before Sega CD 32X.

Implemented (`tools/md-corpus`, `mise run md-corpus -- inventory|run|compare|report`):
- **P0:** inventory against the No-Intro and Redump DATs, one preferred version per title.
- **P1:** `mesen-headless --state-log/--state-blocks/--dump-at`; the same in
  `tools/libretro-capture/capture.py`. L2 compares 68000 work RAM in 256-byte blocks with offset
  tracking (section 2.5). VRAM/CRAM/VSRAM from the oracles' save states and the colour
  calibration cartridge are not done: snapshots are compared in VDP colour steps instead.
- **P2:** the first library pass (attract mode, 1,212 titles, 35 minutes locally) and its ranked
  report. Results and the bug they found: `md-verification-findings.md`, C1.

Deviations from the plan, all recorded in C1: GPGX runs with its default options (some options
break it in the libretro host); MesenCE's Sega CD sessions start from formatted backup RAM, as
the oracles do; known library-wide mix-level differences (Sega CD PCM, PSG) are subtracted before
audio flags.

- **P3 (2026-11-21), partly:** `.bk2`/`.gmv` converters and `md_corpus.py tas import|list`,
  `run --session tas` (each movie's own length, region and pad type; 6-button pads in all three
  emulators). Self-test: `tools/md-corpus/tas_selftest.py`; an end-to-end run of a `.bk2` written
  from a walker input reproduces the walker session.
  - **Fetching is not automated, a deviation from the decision above.** TASVideos' robots.txt
    (read 2026-11-21) disallows `/*?handler=Download` (the movie downloads) for every client,
    asks for a 60-second crawl delay, and blocks AI agents, Claude-Code among them, from the
    whole site. Respecting it means the movies are downloaded by hand and imported from a folder
    (material 11 in section 12). Nothing in md-corpus contacts the site.
  - Not done yet: a run of real published movies (none downloaded), the TAS goal check (ending
    reached in the oracle), the BizHawk reference environment.

Next: the open clusters in C2 (European Club Soccer, 32X candidates); real TAS movies once
downloaded; P4 (goldens).

## 1. What already exists

| Piece | Where | Role here |
|---|---|---|
| Native headless runner: deterministic power-on, input JSON, Lua, snaps, manifests with hashes | `bin/mesen-headless`, [`../headless.md`](../headless.md) | MesenCE side of every run |
| Bus trace (`--trace bus`), per-chip audio (`--chip-audio`: YM2612, PSG, PWM, PCM, CD-DA) | same | L3 and audio |
| Byte-identical output on arm64 and x86-64 | `mise run test:md-xarch` | Mac results and Linux cloud results can be mixed |
| Save-state replay must reproduce trace, audio and final state | `mise run test:md-replay` | Save states can be trusted for bisection |
| Headless libretro host (Python, ctypes): any core, same input JSON, WAV and snaps | `tools/libretro-capture/capture.py` | Oracle side; extend it for RAM and state dumps |
| Audio comparison: envelope alignment, per-chip energy fit, octave spectra; library runner | `tools/audio-compare/`, `mise run test:md-audio-ref` | L1 audio. Found the 32X PWM and Z80-window bugs |
| 60 synthetic cartridges against a recorded BlastEm reference; MesenCE's own cases for EEPROM, mapper, accessories, Sega CD, 32X | `tests/MD/synthetic`, `test:md-synthetic` | Where minimised reproductions go |
| nativekit's patched BlastEm with a bus trace (NMDT v2) | sibling checkout `~/src/nativekit`; [`md-nativekit-trace-requirements.md`](md-nativekit-trace-requirements.md) | L3 oracle |
| Commercial qualification method: START presses, snaps, Lua probes, disassembly | findings S5 in `mcd-` and `32x-verification-findings.md` | Found 6 game bugs by hand; this plan automates it |
| BizHawk movie playback (NES, SNES, GBA only) | `Core/Shared/Movies/BizHawkMovie.cpp` | Could learn `GEN`, to play TASes in the UI |
| Local oracle cores | RetroArch: `genesis_plus_gx_libretro.dylib`, `picodrive_libretro.dylib` | Ready now |
| Local BIOS dumps | `~/mame/roms/segacd` (Sega CD US/EU/JP, 32X M/S/G) | Ready now |
| Local library | `~/Games/ROMs/Sega 32X` (42 titles, GoodTools names, `.7z`), `~/Games/ROMs/Sega CD` (31 US discs) | **The Mega Drive folder is empty** |

## 2. Methodology

### 2.1 A session

A session is a JSON spec. It is content-addressed: its hash is the cache key for every result.

```json
{
  "schema": "md-session/1",
  "rom": {"sha1": "…", "system": "md|mcd|32x|mcd32x", "title": "…", "redump_or_nointro": "…"},
  "bios": {"mcd": "sha1 or null", "32x": ["sha1 M", "sha1 S", "sha1 G"] },
  "settings": {"region": "ntsc-u", "model": "md1va6", "pads": "6,3", "tmss": false,
               "ram_init": "zero", "fm_core": "nuked"},
  "input": {"kind": "frame|poll", "file": "input.json", "sha256": "…"},
  "frames": 36000,
  "battery": "optional backup RAM / SRAM image sha256",
  "source": {"kind": "attract|walker|tas|explorer|human", "ref": "tasvideos 1234M"},
  "checkpoints": [3600, 7200]
}
```

A result is keyed by `(session hash, emulator name + build hash, options)`. Every emulator is
deterministic, so results are pure functions and cacheable. A re-run is needed only when the
MesenCE commit or an oracle pin changes.

### 2.2 Same conditions in every emulator

Power-on differences cause divergence before the game even starts. Fix them per oracle, and
check each setting once with a synthetic cartridge, as the BlastEm baseline did:

- **TMSS** off everywhere (or on everywhere, with the same boot ROM).
- **RAM** zero-initialised. MesenCE's headless runner already does this; check each oracle's
  default and set its core option. Z80 RAM and VRAM too.
- **Model and region.** Use the same console revision where the oracle has a choice. Region
  comes from the session, not auto-detected per emulator.
- **Pads.** The type per port follows the session. TASes record it in their sync settings.
- **FM core.** Use Nuked-OPN2 in MesenCE when comparing audio with an oracle that uses Nuked
  (GPGX option `ym2612` = nuked). Use ymfm for the default goldens.
- **Off:** sprite-limit removal, overclocking, low-pass filters and blending in every oracle.
- **Sega CD:** the same BIOS file in every emulator, and the same backup RAM image, formatted
  once by the BIOS itself as in S5.
- **32X:** Sega's boot ROMs in every emulator. MesenCE's replacement boot ROMs need a separate
  MesenCE-only run, because the hand-over time differs by design (S5).

### 2.3 Oracles

No Mega Drive emulator is cycle-accurate in every respect, and none is ground truth. Each oracle
is a **voter**, and disagreements are weighed (section 2.6). The table gives the role of each.

| Oracle | MD | Sega CD | 32X | CD 32X | Automation | Role |
|---|---|---|---|---|---|---|
| **Genesis Plus GX** (libretro) | yes | yes | — | — | `capture.py` today | Main L1/L2 oracle for MD and Sega CD; the core most Genesis/Sega CD TASes sync on (BizHawk's fork) |
| **PicoDrive** (libretro) | yes | yes | yes | reported, verify on our discs | `capture.py` today | Main 32X oracle and the only automatable CD 32X one; less precise timing, so weigh lower on MD |
| **BlastEm** (nativekit's pinned, patched libretro) | yes | experimental in recent upstream, not in the pin | — | — | nativekit `run.py`; NMDT v2 bus trace | L3 trace oracle for MD; precise VDP/68000 timing |
| **clownmdemu** (libretro) | yes | yes | — | — | `capture.py` (to try) | Independent third voter for MD and Sega CD |
| **ares** | yes | yes | yes | check the current release | no libretro and no headless frontend: needs a thin host (ISC licence) | Strongest 32X/Sega CD timing reference. **Not independent for the 68000:** MesenCE's 68000 is ported from ares, and the Sega CD/32X units used ares as their main reference |
| **MAME** (`megadriv`, `segacd`, `32x`) | yes | yes | yes | — | `-autoboot_script` Lua, `-video none -nothrottle`, `-record/-playback .inp` | Tie-breaker for Sega CD and 32X. Its source has already settled two disputes (`megadriv.cpp`) |
| **BizHawk** (EmuHawk) | via GPGX | via GPGX | via PicoDrive | — | `--movie`, `--lua`, Linux build | Runs TAS movies in the exact environment they were made in. Second-line, for TASes that desync in libretro GPGX too |
| **Real hardware** (flash cart + capture) | | | | | manual | Final word on reviewed disputes. Optional, rare |

Licences don't matter for oracle use: we compare outputs and copy no code. When a fix takes
its *behaviour* from an oracle's source, the findings entry cites it, as before. The fork has
cited MAME (BSD-3) and ares (ISC).

**Pin every oracle.** Each oracle gets a source commit, a build recipe in the flake
(section 11.1) and a SHA-256 of the binary, recorded in each result as nativekit does. A
RetroArch download is fine for exploration, not for recorded results.

### 2.4 Observables and normalisation

**L0, health.** No oracle needed. Signals:

- **Exceptions:** the 68000 enters an exception vector other than the expected interrupts
  (address error, illegal instruction, zero divide, the BIOS fatal handlers). Detected through
  the debugger hook, or Lua on the vector addresses.
- **Hangs:**
  - PC stays in a small address set for more than N seconds while the screen doesn't change;
  - or a CPU is stopped while nothing can wake it.
- **Bad output:** a black or constant screen for N seconds after boot; silence where the oracle
  has sound.
- **Runner failures:** non-zero exit, timeout, sanitizer reports (the ASan build on a sample of
  sessions).
- **Self-consistency:** save-state round trip mid-session (as `test:md-replay`), and a speed
  floor (fps per title, so performance regressions show).

**L1, functional.**

- **Frames.** Crop to the active display (H32/H40, V28/V30; each emulator's border and overscan
  handling differs). Then map colours to the hardware's levels before comparing. MD colour is
  3 bits per channel, plus shadow and highlight. Each emulator maps those levels to RGB with
  its own ladder, so calibrate once per oracle: a synthetic cartridge shows all 512 colours in
  normal, shadow and highlight. Map each emulator's RGB back to (level, mode); frames then
  compare exactly, not perceptually. The 32X's 15-bit layer needs no ladder.
- **Audio.** Use the existing `compare.py`: envelope alignment, chip fit, spectra.

**L2, state.** Per-frame CRC32 or xxhash of:

- work RAM, Z80 RAM, VRAM, CRAM, VSRAM and the VDP registers;
- Sega CD: PRG-RAM, word RAM, PCM RAM, backup RAM;
- 32X: SDRAM, frame buffers, palette.

When hashes differ, dump both full regions at that frame and diff them byte by byte.

- **Byte order:** GPGX and PicoDrive keep 68000 RAM word-swapped on little-endian hosts. Detect
  this per oracle with a synthetic cartridge that writes a known pattern, and normalise.
- **Oracle memory access:** libretro gives work RAM (`RETRO_MEMORY_SYSTEM_RAM`) and sometimes a
  memory map (`SET_MEMORY_MAPS`). VRAM, CRAM and VSRAM come from parsing the core's serialised
  state (`retro_serialize`) with its layout, which is pinned with the core. That costs a
  per-oracle parser, and the version pin makes it stable. MAME's Lua reads every address space
  directly.
- **Gating:** L2 is the strict level. Work RAM equality per frame means the game logic is in
  lockstep. VDP memory equality means the picture is built from the same data, whatever the
  renderer does.

**L3, trace.** MesenCE `--trace bus` against BlastEm's NMDT v2 over a window ending at the first
L2 divergence.

- MesenCE starts from a save state a few frames before the divergence (`--load-state`).
- BlastEm runs from power-on, which is fast.
- Align on the first common event, then report the first differing event: clock, device,
  address, value.
- This is how timing bugs get a cycle-level explanation. MD only, until another oracle has a
  trace (MAME's Lua taps could provide 68000 reads and writes for Sega CD/32X, at low speed).

### 2.5 Alignment and divergence

- **Start offset.** Emulators can disagree on which frame is frame 1: partial first frame,
  reset timing, BIOS length. For each pair, search offsets in ±N frames (N = 10, or 300 with a
  BIOS) for the best L2 match over the first seconds of identical state, then fix the offset
  for the session. A session with no offset where work RAM ever matches is an early divergence
  in its own right.
- **First divergence.** The first frame where L2 differs and doesn't converge again within K
  frames. Transient differences that re-converge (a one-frame timing difference in a busy-wait)
  are recorded as "jitter" with their count, not as divergences.
- **Lag-frame sequence.** Per frame, whether the game read the pad (I/O ports `$A10003/5`).
  - The sequence of polled and lag frames is a cheap, robust timing signature: two emulators
    with the same sequence agree on how much work each frame took.
  - MesenCE reads it from a memory callback; MAME from a Lua tap. libretro cores don't expose
    it.
  - BizHawk's own lag log is the reference for TASes.
- **Poll-indexed input** (`input.kind = "poll"`). The input advances on each pad read, not on
  each frame. Session inputs then stay attached to the game's own progress when loading times
  differ (Sega CD seeks).
  - Recorded from the source run's lag log; replayed by hosts that can hook the read (MesenCE,
    MAME).
  - Frame-indexed stays the default because every host supports it.
  - Judgment call: a session replayed by poll can still reach different states. It's for
    Sega CD functional comparison, not a strict test.

### 2.6 Consensus and verdicts

For each session, compare MesenCE with each oracle that supports the system.

| MesenCE vs oracles | Verdict | Priority |
|---|---|---|
| Matches all | pass → becomes a golden (section 7) | — |
| Differs from ≥ 2 oracles that agree with each other (not counting ares on 68000 behaviour) | **probable MesenCE bug** | high |
| Differs from the only oracle (32X: PicoDrive alone) | **candidate**: needs a source (manual, ares/MAME code reading, hardware test cart) before any fix | medium |
| Oracles disagree with each other | **disputed**: record it; fix only with a source; a hardware test if it matters | low |
| L0 failure in MesenCE only | **probable bug**, whatever the oracles say | highest |
| L0 failure in the oracle too (TAS desyncs in GPGX) | oracle problem; re-run under BizHawk; ignore if it persists | — |

These rules are judgment calls. The thresholds and the ares exclusion get revisited once real
results exist. As the contribution policy requires, a "not a bug" verdict is recorded, not
dropped.

## 3. Where sessions come from

### 3.1 Attract mode and the menu walker

These **exist** for audio. Extend them to L0-L2 and frames.

- **Attract:** 3,600 frames, no input. It keeps the emulators on one timeline more often than
  any input does, so it's the best strict L2 test per CPU second.
- **Walker:** `make_input.py`. Sega CD: also open the BIOS control panel and the CD player
  (the BIOS is code too).

### 3.2 TAS movies

TASVideos publishes movies for Genesis, Sega CD and 32X, each with the emulator, core, sync
settings and ROM hash it syncs on.

- **Import:**
  1. Download each publication's movie file. The site has a public publication listing; respect
     its rate limits and terms, use a generic client name, and cache.
  2. Read the movie's ROM hash and match it against the inventory. GoodTools names don't help
     here: only the hash matches.
  3. Convert the movie's input to `md-session/1`.
- **`.bk2`:** a zip. `Header.txt` holds the platform, core and SHA-1. `SyncSettings.json` holds
  region, pads, etc. `Input Log.txt` has a `LogKey:` line naming the columns and one `|…|` line
  per frame, with power and reset flags. The converter maps columns by `LogKey`, so it doesn't
  depend on BizHawk's column order.
- **`.gmv` (Gens):** a 64-byte header, then 3 bytes per frame.
- **Run:** in MesenCE and in libretro GPGX/PicoDrive.
  - The TAS's goal is reached when the oracle's final state shows the ending: credits or the
    known end RAM (the movie's last frames are by definition the end).
  - If libretro GPGX desyncs too, the movie needs BizHawk's GPGX fork. Re-run it under EmuHawk
    on Linux (headless with Xvfb); that run becomes the reference.
- **Why it's worth it:**
  - TASes reach the late levels, bosses and endings no attract mode shows.
  - They exercise exact timing (lag frames, sprite flicker, glitches TASers use).
  - **A MesenCE desync in a TAS that syncs in its own emulator is the strictest whole-game test
    available.**
  - Expect many desyncs at first. Not all are MesenCE bugs: TASes depend on emulator-specific
    lag. The consensus rules apply.
- **UI option** (later): teach `BizHawkMovie.cpp` the `GEN` platform, so a TAS plays in the
  MesenCE UI for a human to watch where it goes wrong.

### 3.3 Explorer (novelty search)

For titles without a TAS. A Go-Explore-style bot:

- **Archive.** Cells are a coarse game state: an 8×8-downscaled, 3-level-quantised frame, plus
  a hash of a few RAM bytes that change slowly (found automatically: bytes with few distinct
  values over a run, typically level and area numbers).
- **Loop:**
  1. Pick a cell, preferring rarely chosen ones.
  2. Restore its save state.
  3. Play a random burst: held directions, attack buttons, START only rarely.
  4. Archive new cells together with their input log from power-on.
- **Which emulator explores:** an oracle, through libretro `retro_serialize/unserialize`. Paths
  found that way are paths the reference reaches, so a MesenCE replay that fails to follow them
  is evidence.
  - Exploring in MesenCE (Lua save states) finds MesenCE-only crashes too. Run a little of both.
- **Output:** for each title, the K most novel paths as sessions (start → furthest cell).
- **Cost:** an oracle runs at hundreds to thousands of fps, so a budget of 30 CPU-minutes per
  title is enough for many action games. It isn't for RPGs.
- **Later:** an LLM player (screenshots + a short memory) for menus and text-heavy games. It is
  expensive, so only for titles where explorer coverage stalls. Judgment call, deferred.

### 3.4 Human sessions

When a specific title needs a specific scene, play it in MesenCE's UI with movie recording on.
A converter turns the `.mmo` (a zip: `Input.txt`, `GameSettings.txt`) into session input. RetroArch
`.bsv` replays could be converted too, but they carry the emulator's state, so start from
power-on.

## 4. The pipeline

Proposed tool layout: `tools/md-corpus/`, Python, with numpy as the only dependency, like
`audio-compare`. Every stage reads and writes plain files under one output root, and is
restartable and parallel per session.

```
inventory ─▶ sessions ─▶ run (MesenCE × oracles) ─▶ compare (L0-L2) ─▶ triage ─▶ report
   │            ▲                                         │               │
   │            └── tas-import / explore / walker         └─▶ goldens     └─▶ bundle ─▶ fix loop
   └── corpus.json (hash, system, title, region, dat name, TAS ids, BIOS needs)
```

| Stage | Command (proposed) | Output |
|---|---|---|
| Inventory | `md-corpus inventory ~/Games/ROMs/... --dat nointro.dat --redump redump.dat` | `corpus.json`. Extracts `.7z` into a cache keyed by hash; prefers verified dumps (`[!]`, DAT match); flags bad dumps (`[f1]`, `[b]`, overdumps) |
| Sessions | `md-corpus sessions attract\|walker\|tas\|explore` | `sessions/<hash>.json` |
| Run | `md-corpus run --emu mesen,gpgx,picodrive [--jobs N]` | `runs/<session>/<emu>/`: manifest, state log, snaps, audio, final state |
| Compare | `md-corpus compare` | `results/<session>.json`: offset, first divergence per level, jitter, audio metrics, verdict |
| Triage | `md-corpus triage` | `clusters.json`, `report.html` (ranked clusters; screenshots side by side, first divergence, memory diff) |
| Bundle | `md-corpus bundle <cluster>` | Directory for a fix agent: sessions, a save state 60 frames before the divergence, memory diffs, an L3 trace diff (MD), disassembly around the PC, relevant findings excerpts |
| Goldens | `md-corpus golden update` | `tests/MD/corpus/goldens/<rom sha1>/<session>.json` (hashes only) |

**MesenCE side: one new runner option.**

- `mesen-headless run … --state-log FILE` writes one line per frame: frame number, master clock,
  pad-polled flag, PC of each CPU at the frame end, and the region hashes from 2.4. This keeps
  per-frame cost to hashing about 200 KB, which is cheap next to emulation.
- Full region dumps at chosen frames: `--dump-at F1,F2,…`.
- Lua could prototype both, but hashing 64 KB per frame in Lua would dominate run time.

**Oracle side.** `capture.py` gets:

- `--state-log` (RAM from `retro_get_memory_data`; VDP memories by parsing `retro_serialize`
  with a per-core layout module);
- `--dump-at`;
- core options from the session settings;
- the same output layout as MesenCE's.

MAME and an ares host get adapters with the same output layout.

## 5. Triage

1. **Bucket:** L0 failures first, then by verdict (2.6).
2. **Cluster:** sessions whose first divergence has the same signature:
   - first differing region (work RAM, VRAM, CRAM, …);
   - the differing address range, modulo the game;
   - MesenCE's 68000/Z80/SH-2 PC at the divergence frame, as a ROM offset;
   - the system and the active features (from Lua: H40, interlace, DMA type, 32X FM bit, Sega
     CD mode).

   Across games, the strongest key is "which hardware feature was active". A VDP DMA bug shows
   up in fifty games with different RAM addresses.
3. **Rank:** by cluster size × verdict priority × how early the divergence comes (an early,
   widespread divergence hides everything after it).
4. **Drill down** (MD with BlastEm): L3 trace diff over the frame before the divergence. For
   Sega CD/32X: MAME Lua taps, ares reading, and the S5 approach (CPU positions, comm words,
   exception vectors).
5. **Minimise** into a synthetic case: reproduce the behaviour in a `gen.py`-style cartridge
   (or `sh2asm.py`, `mcd_models.py`) that fails before the fix.
   - S5 did this for every game bug: `swap_race.py`, `z80_busack_reset`, `m32x_z80_window`,
     `m32x_fen_refresh`.
   - That synthetic case, not the commercial session, is the regression test the contribution
     policy asks for, because it runs in CI without ROMs.

## 6. Fix loop, with LLM agents

Each cluster bundle becomes one task with a fixed definition of done, the fork's evidence
policy:

1. **Reproduction:** the failing session, plus a synthetic case that fails before the fix.
2. **Source:** a hardware manual page, a hardware test, or oracle source code read for
   behaviour. Two oracles agreeing is a reason to look, not a source by itself. Label judgment
   calls.
3. **Fix**, within the performance rule: no new work on hot paths without a measured cost (F9/F10,
   or headless fps on the slowest titles).
4. **Regression test** wired into `test:md-synthetic` (or a new `mise` task).
5. **Checks:**
   - `test:ci`, `test:md-synthetic`, `test:md-replay`, `test:md-xarch` and the CPU suites the
     change touches;
   - **plus re-running every corpus session and golden that touches the changed component.**
     No golden may break unless it now matches the oracles better (the cluster's sessions move
     their first divergence later or pass).
6. **Findings entry** in the system's `*-verification-findings.md`, in the S5 table style.

Agents work on branches and open pull requests. A human reviews and merges. An agent that
can't find a source records the cluster as "candidate, no source" and stops; it doesn't guess
a fix to make the numbers match. The pipeline gives agents a measurable objective (sessions
passing, divergences moving later). That objective is easy to overfit, and the source
requirement is the guard against overfitting.

## 7. Goldens: regression testing without oracles

Every session that passes at L2 against the oracles becomes a **golden**: MesenCE's state-log
hashes at every Nth frame and at the end.

- **Size:** they hold only hashes, a few KB per session. Their path is keyed by ROM SHA-1.
  They can live in the repository (`tests/MD/corpus/`) and contain no game data.
- **Checking:** re-run MesenCE alone; any hash change fails. This is fast (no oracle) and runs
  on every commit, locally or in the cloud, wherever the ROMs are present. It skips cleanly
  where they aren't, like `test:mcd-bios`.
- **Updating:** only through `md-corpus golden update` after a review shows the change is an
  improvement.
- **Scope:** sessions that diverge from the oracles can still get a *MesenCE-only* golden,
  marked as such. Any change to their output is worth a look either way.

## 8. Sega CD specifics

- **Seek and load timing:**
  - No emulator models drive seek and CD controller timing exactly, and they differ from each
    other. Strict L2 lockstep usually breaks at the first load.
  - Expect Sega CD L2 passes only within segments (title screens, attract segments between
    loads, the BIOS).
  - The working level is L0 + L1 + poll-indexed replay, plus MesenCE-only goldens.
  - Judgment call: chasing seek-time agreement with an oracle is not a goal without a hardware
    source.
- **Audio:** CD-DA and PCM separately (`--chip-audio` already writes `cdda.raw` and `pcm.raw`).
- **Multi-disc games:** sessions need a disc-swap event. Needs a runner option
  (`--md-mcd-swap FRAME:FILE`). Not available today.
- **Backup RAM:** start every session from the same formatted image (S5 method). For save and
  load paths, use a two-session chain (save in one, load in the next).
- **Regions:** only US discs locally. The Japanese security-code region is open (findings S5).
  Japanese and European discs would close it.

## 9. 32X specifics

- **Single oracle:** only PicoDrive among the convenient ones, so most 32X divergences are
  *candidates* (2.6). MAME `32x` and ares are the tie-breakers; MAME through Lua is
  automatable today.
- **Speed:** about 1× real time for heavy titles in MesenCE. 32X sessions dominate local cost.
  The SH-2 speed work listed in the 32X findings ("Open issues") pays for itself here.
- **Boot ROMs:** compare with Sega's in every emulator. The replacement boot ROMs get
  MesenCE-only goldens.
- **SH-2 timing in lockstep:** both SH-2s, the 68000 and the Z80 must all agree. Expect jitter
  (2.5) from comm-register polling loops. Treat jitter counts as a metric, not a failure.

## 10. Gaps to close in MesenCE first

1. **Sega CD 32X is not emulated.**
   - `MdConsole::Init` attaches the 32X only when `!mode2` (`Core/MD/MdConsole.cpp:204`), so a
     CD 32X disc boots as a plain Sega CD game.
   - This needs its own S-phase:
     - the 32X and the Sega CD unit attached together;
     - the 32X's ROM-window mapping in Mode 2;
     - CD 32X disc detection;
     - sources: the 32X hardware manual's CD section, ares and PicoDrive for behaviour.
   - The only automatable oracle is PicoDrive (and possibly ares). This is the one platform in
     the request with no coverage today.
2. `--state-log` and `--dump-at` in `mesen-headless` (section 4).
3. A pad-poll flag per frame (for the lag sequence and poll-indexed input), and poll-indexed
   input in the runner.
4. `--md-mcd-swap` for multi-disc games.
5. `capture.py` additions: state log, VDP memory parsers for the GPGX and PicoDrive save-state
   layouts, core options from session settings. Check the clownmdemu core.
6. Converters: `.bk2`, `.gmv`, `.mmo` → session input.
7. Optional: `GEN` in `BizHawkMovie.cpp`; an ares headless host; MAME adapter.

## 11. Local → cloud

### 11.1 The runtime image

- **Image:** one OCI image built by Nix (`nix build .#md-farm-image`), x86-64 Linux, containing:
  - `mesen-headless` (Linux clang, already `-ffp-contract=off`, so captures are byte-identical
    to the Mac's, as checked by `test:md-xarch`);
  - the oracle cores, built from pinned sources (GPGX, PicoDrive, clownmdemu; BlastEm with
    nativekit's patches), plus MAME if used;
  - Python with numpy, and `tools/md-corpus`.
- **Pinning:** the flake already provides the toolchain. Pins live in `flake.lock`, so a result
  names exactly what produced it.
- **Equivalence:** the same image runs on the Mac through a Linux VM for spot checks. Mac-native
  runs stay the default locally, and their results are interchangeable (xarch).

### 11.2 ROMs and BIOS: a private vault

- **Storage:** a private, encrypted bucket you control (S3, R2 or B2), objects named by SHA-1.
  Workers fetch only the hashes their jobs need and delete them afterwards.
- **What stays out:** ROMs, BIOS dumps, save states and memory dumps never go into the
  repository, CI logs or public artifacts. They contain game code and data. Screenshots and
  hashes are fine in private reports.
- **Pushing changes:** sync from your machine with `md-corpus vault push` (uploads missing
  hashes only).
- **Your decision, not a technical one:** the legal position on copies of games you own, stored
  privately in the cloud. Keep the vault private and single-user.

### 11.3 Orchestration

- **Simplest that works:** one large spot or preemptible VM (e.g. 64 vCPUs) running the same
  `md-corpus run --jobs 64` as on the Mac, with results synced back to a bucket. Results are
  cached by key, so a pre-empted run resumes where it left off.
- **When that's not enough:** a queue (SQS, Redis or a bucket-backed one) with N workers, each
  running the image. Use a batch service (AWS Batch, GCP Batch, Modal, Fly Machines). The job
  is `(session hash, emulator, build)`, and the result goes to `results/<key>.json` in the
  bucket.
- **Per commit:**
  - CI (GitHub Actions) builds the image on pushes to the fork.
  - A scheduled workflow runs the goldens (fast) on every push.
  - The full oracle comparison runs nightly or on demand.
  - CI uses self-hosted or cloud-batch runners with vault credentials. GitHub's hosted runners
    must not see ROMs.
- **Reports:** `report.html` per run, published privately, plus a trend table (sessions passing
  per system per commit).
- **Fix agents:**
  - A scheduled cloud agent (Claude Code routine or GitHub Action) takes the top unassigned
    cluster each night, works it under section 6 on a branch, and opens a pull request or writes
    "candidate, no source".
  - Run several in parallel, one cluster each.
  - They need the vault, the image and the bundle. They never get merge rights.

### 11.4 Cost and throughput (estimates; measure on the first pass)

| Corpus slice | Sessions | Game time | MesenCE core-hours | Oracle core-hours |
|---|---|---|---|---|
| MD attract + walker (assumes about 1,000 titles; size depends on what you provide) | about 2,000 | about 4 min each | about 15 (at an assumed 500 fps; MD not measured yet) | about 5 |
| Sega CD attract + walker (31 discs) | about 60 | 5 min | under 1 (580 fps) | under 1 |
| 32X attract + walker (42 titles) | about 80 | 5 min | about 7 (≈ 1×) | about 1 |
| TAS replays (assumes 300 matched movies) | 300 | about 25 min | about 15 (MD rate) | about 5 |
| Explorer | 1 per title | — | — | 30 min/title ≈ 500 |
| L3 traces | per cluster | windows | small | small |

Without the explorer, a full pass is about 50-100 core-hours. That's 5-10 hours on the local
10-core Mac, or 1-2 hours on one 64-vCPU VM. The explorer dominates, and it's a one-time cost
per title: its output becomes sessions. At typical spot prices that's tens of dollars per full
pass. Check current prices; this is an estimate, not a quote.

## 12. Materials needed from you

Place them under `~/Games/ROMs/<system>/` as today (any naming; hashes do the matching), or tell
me the paths. Priorities: **P1** blocks the first pass, **P2** widens coverage, **P3** helps.

| # | Item | Format | Why | Priority |
|---|---|---|---|---|
| 1 | **Mega Drive / Genesis library**: US, EU and JP if you have them; include known-tricky titles (list below) | No-Intro set preferred: raw `.md`/`.bin`/`.gen`, or `.zip`/`.7z`, one game per archive | The local folder is empty. It's the largest part of the corpus and of the TAS matches | **P1** |
| 2 | **Sega CD 32X discs** (the handful of CD 32X titles, e.g. Fahrenheit, Night Trap, Corpse Killer, Slam City, Supreme Warrior) | Redump BIN/CUE (CHD needs converting: MesenCE reads CUE) | The platform isn't emulated yet (10.1); needed to build it and to test it | **P1** for CD 32X |
| 3 | **32X library**: verified dumps | No-Intro preferred; the current GoodTools set works, but `[f1]` fixed dumps are skipped | TAS movies need the exact No-Intro hash | P2 (current set is usable) |
| 4 | **Sega CD: more discs, especially JP and EU**, and multi-disc games | Redump BIN/CUE | Japanese region detection is open; multi-disc swap paths | P2 |
| 5 | **No-Intro DATs** (Mega Drive, 32X) and **Redump DATs** (Sega CD) | `.dat` (XML) from the DAT-o-MATIC and Redump sites (free accounts) | Hash → canonical name, verified-dump flag, TAS matching | **P1** |
| 6 | **Mega Drive TMSS boot ROM** | the 2 KB file | Only to compare with TMSS on; default runs keep it off | P3 |
| 7 | **BIOS:** already present (Sega CD US/EU/JP all models, 32X M/S/G in `~/mame/roms/segacd`) | as is | — | done |
| 8 | **Save files** for late-game content where useful (SRAM/backup RAM from your own play) | `.srm`/`.brm` | Sessions that start later in RPGs | P3 |
| 9 | **Oracle builds:** RetroArch GPGX and PicoDrive cores (present); the nativekit checkout (present at `~/src/nativekit`); MAME (`brew install mame` or Nix) | — | Third and fourth voters | P2 (MAME) |
| 10 | **BizHawk** on a Linux box or VM, for TASes that desync in libretro GPGX | release archive | TAS reference environment | P3 |
| 11 | **TAS movies** from TASVideos publications (Genesis, Sega CD, 32X), downloaded by hand: robots.txt disallows automated downloads | `.bk2`/`.gmv` files in `bin/md-corpus/movies` (any names, subfolders allowed); then `md_corpus.py tas import` | Whole-game sessions; the movie's ROM hash must match a dump in the library | P2 |
| 12 | **Cloud account and a private bucket** (S3, R2 or B2), and where you want runs (one VM vs batch); a budget cap | credentials kept outside the repo (mise `[env]` in `mise.local.toml` or a secrets manager) | Section 11 | P3 (after the local pass) |
| 13 | **Hardware** (optional): a Mega Drive (+ Sega CD, 32X) with a flash cart and a capture device | — | Ground truth for disputed clusters | optional |
| 14 | **Your priorities:** titles you care most about, and known-broken titles you've seen | a list | Ranking | P2 |

**Tricky-titles shortlist for the Mega Drive library** (accuracy-sensitive, worth having
first): Sonic 1-3 & Knuckles, Sonic 3D, Vectorman 1-2, Gunstar Heroes, Alien Soldier,
Contra: Hard Corps, Thunder Force IV, Castlevania: Bloodlines, Ranger X, Red Zone, Batman &
Robin, Overdrive and other demoscene ROMs, Virtua Racing (SVP: not emulated), Street Fighter II'
SCE (mapper), Phantasy Star IV, Lotus II, Mickey Mania, Road Rash II/3, Shining Force II,
World of Illusion, Pier Solar (homebrew, if owned). These lean on mid-frame VDP changes, DMA
timing, the Z80/68000 bus, interlace and mappers.

## 13. Phases and exit criteria

| Phase | Content | Exit criterion |
|---|---|---|
| P0 | Materials (12), inventory, `corpus.json` | Every file hashed and classified; matched to DATs |
| P1 | `--state-log`/`--dump-at`; `capture.py` state log; GPGX and PicoDrive state parsers; colour calibration cartridge; byte-order detection | A synthetic cartridge gives identical L2 logs in MesenCE, GPGX and PicoDrive (or each difference is explained and recorded in the findings) |
| P2 | Attract + walker over the corpus, L0-L2 + audio, consensus, `report.html` | First ranked cluster list; findings entry with pass rates per system |
| P3 | TASVideos import, `.bk2`/`.gmv` converters, TAS runs | Pass/desync table per movie; desyncs triaged by consensus |
| P4 | Goldens wired into a `mise` task (`test:md-corpus`) | Goldens run per commit locally; skip cleanly without ROMs |
| P5 | Bundles and the fix loop, started locally (one cluster at a time) | First fixes merged under the evidence rules |
| P6 | Explorer | Sessions for titles without a TAS |
| P7 | Cloud image, vault, orchestration, scheduled agents | Nightly full pass and per-commit goldens in the cloud |
| CD32X | Sega CD 32X support (own plan, like `32x-feasibility.md`) | CD 32X discs boot; they enter P2-P4 against PicoDrive |

P0-P2 are the expedient core: most bugs a library-wide sweep can find show up there. The
6 bugs found by hand in S5 are the kind P2 finds automatically.

## 14. Risks and limits

- **Oracles are wrong too.** Consensus helps, and agreement can still be a shared error.
  Oracles copy from each other, and MesenCE copies from ares. Sources decide (section 6).
- **Timing-sensitive games:** games that seed their RNG from the H/V counter or from loop
  counts diverge at any sub-frame timing difference. That is real information, but it hides
  later content. The lag sequence and jitter metrics show such games; give them functional (L1)
  verdicts.
- **TAS desyncs** come mostly from emulator-specific lag, not bugs. A desync is a lead, not a
  verdict.
- **Explorer reach:** RPGs and adventure games stay shallow without human or LLM play.
- **Sega CD lockstep** is limited (section 8).
- **Cost of strict L2:** state parsers are tied to oracle versions. Upgrading an oracle means
  updating its parser and re-running its results (they're keyed by oracle hash, so stale
  results can't be mixed in).
- **Privacy:** reports with screenshots and dumps of commercial games stay private (section 11.2).

## 15. Open questions (yours to decide)

1. Cloud provider and budget, and whether fix agents may open pull requests unattended.
2. Whether TASVideos fetching may run automatically (public site; rate-limited and cached) or
   you prefer to download the movie archive yourself.
3. Order of the work: start with the Sega CD 32X support or with the pipeline (P1-P2). The
   pipeline first is the expedient choice; CD 32X can follow in parallel.
