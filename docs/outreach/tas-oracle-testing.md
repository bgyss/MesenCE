# Speedruns as a test suite: what's different about MesenCE's Mega Drive checks

Background and a draft post for the follow-up to the intro post
([verification-posts.md](verification-posts.md)). The prior-work search was done 2026-11-21. It
was a web search, not a literature review, so the novelty claim is worded as "I couldn't
find", not "first".

**Before posting:**
- The tas batch (21 movies) was still finishing when this was written. Fill in
  `[batch results]` from `bin/md-corpus/report.html`, or drop that line.
- Keep the prior-work credits. People will name these projects in replies anyway.

---

## Prior work (sourced)

**Console verification of TAS movies (TASVideos, TASBot).** Published movies are played back
on real consoles through replay devices. A movie that syncs on hardware is "console verified".
That work has found emulator bugs. A BizHawk developer reported fixing subtle NES IRQ/NMI
handling bugs and adding a DMC IRQ delay that had never been tested before
([TASVideos forum](https://tasvideos.org/Forum/Posts/506887)). Some console resyncs had to
adjust input frames to make up for emulator inaccuracy
([TASVideos forum](https://tasvideos.org/Forum/Posts/515047)).
*Difference:* the reference there is real hardware, the strongest possible oracle, but it is
slow and done one movie at a time.

**Verified TASes as an emulator test suite (Game Boy).** TiKevin83's automation runs 46
console-verified Game Boy TASes against Gambatte Speedrun on every update. The page reports 41
of 46 passing. It describes "using known verified TASes as a test suite for emulation
accuracy" as a supplement to test ROMs
([gsr-automation-site](https://tikevin83.github.io/gsr-automation-site)). This is the closest
prior work. *Difference:* one emulator, checked against a console-synced input log; pass or fail
is judged on the screen at the end, not on game state every frame.

**Replaying recorded input to catch regressions.** Dolphin's FifoCI replays recorded GPU command
logs on every build and flags rendering changes
([Dolphin blog](https://dolphin-emu.org/blog/1X)). Mesen's own `.mtp` tests record input plus
per-frame hashes. *Difference:* these compare an emulator with an earlier version of itself.
They catch changes, not errors that were there from the start.

**Differential testing of emulators.** Comparing emulators against each other or against
hardware is an established research method for CPU emulators: EmuFuzzer
([TOSEM 2013](https://doi.org/10.1145/2522920.2522922)) and PokeEMU
([Berkeley, 2012](https://people.eecs.berkeley.edu/~dawnsong/papers/2012%20Path%20Exploration%20Lifting%20Hi%20Fi%20Tests%20for%20Lo%20Fi%20Emulators.pdf)).
Tom Harte's ProcessorTests and the SingleStepTests suites made per-instruction checks routine
for hobby emulators ([SingleStepTests](https://github.com/SingleStepTests)). This fork uses them
for every CPU core. *Difference:* single instructions, not whole games.

**LLM-written emulators checked against a reference.** Recent work fuzzes an LLM-written .NET VM
against the real runtime
([kant2002, May 2026](https://kant2002.github.io/en/llm/emulator/2026/05/14/generating-donet-emulator.html)).
Another project uses LLM-generated programs plus differential testing on gem5
([ASE 2025](https://link.springer.com/article/10.1007/s10515-025-00531-7)). *Difference:* those
target CPUs and VMs, not a whole console with video and audio chips.

**What I didn't find:** a Mega Drive project that compares several emulators automatically, game
state every frame, across a library and a set of TAS movies. The Genesis Plus GX / PicoDrive
results came back as reputation and ratings only, with no comparison tool.

---

## What is different here

1. **Two independent references, and a rule for blame.** Every session runs in MesenCE, Genesis
   Plus GX and PicoDrive. A difference counts against MesenCE only when the two references agree
   with each other and not with MesenCE. When one reference disagrees with everyone, that's
   recorded as "single oracle" and scored low.

2. **Game state every frame, not just "did it finish".** Each emulator logs 256 hashes of the
   68000's 64 KiB work RAM every frame. Each pair of emulators is lined up frame by frame. The
   comparison allows a few frames of lag drift and leaves out regions that are always noisy,
   such as the stack and tables caught mid-update. That gives the frame where two emulators
   stop agreeing, not just "desynced somewhere".

3. **Many movies run automatically.** Movies are made in BizHawk and Gens; both formats are
   imported, and each is matched to the exact ROM dump. Region and pad types come from the
   movie. Every movie runs in all three emulators, with no hand resyncing.

4. **Calibration without the emulator under test.** Libretro Genesis Plus GX acts on some games'
   pad reads two frames later than the others. Sonic 3 needs the 2 frames; Sonic 1 needs none.
   The pipeline picks a per-movie input offset for GPGX by matching it against PicoDrive only.
   MesenCE never takes part in the choice, so the check can't quietly be tuned to make it pass.

5. **TAS movies sit next to a library-wide pass.** The same pipeline runs the attract mode of
   1,130 Mega Drive titles plus the 32X and Sega CD libraries. Speedruns add what attract modes
   never reach: late levels, menus, saves and fast odd play.

6. **Evidence rules for an LLM-assisted codebase.** Every fix that comes out of it needs a
   reproduction, a source for the right behaviour, a regression test and a findings entry. That
   includes the results that turned out not to be MesenCE bugs.

---

## Results so far (be precise in the post)

- **Library pass (C1-C2):** it found and fixed real MesenCE bugs:
  - the Sonic & Knuckles lock-on window was mirrored;
  - interrupt-enable latency was wrong (Fatal Rewind, Sesame Street Counting Cafe);
  - battery RAM wasn't detected on several boards (HardBall '95 and III, Madden NFL 98, Buck
    Rogers).
- **TAS movies (C4):** the first three movies run through all three emulators showed no MesenCE
  divergence: Sonic 3, Sonic 1 100% (60,000 frames) and Phantasy Star II.
- **Found along the way:** Genesis Plus GX's game-dependent input lag in libretro. The checking
  method also tests the references.
- **Second batch: 59 movies, 6.9M frames, 91 min, no failed runs.**
  - In 41 movies all three emulators stay in step to the end; in 50, MesenCE and PicoDrive do.
  - Found on the way, not an emulation bug: emulators differ in when they hand a frame's input
    to the game. The Lion King reads the pad late in a long VBlank handler, and MesenCE served
    the next frame's input there. MesenCE now has a matching mode (`--md-input-latch vblank`)
    for movies recorded in BizHawk's Genplus-gx core.
  - MesenCE-only candidates still under investigation: Mega Turrican (looks like a stall),
    Streets of Rage 2, one Lion King movie and one Sonic 3 & Knuckles movie. Don't call them
    bugs in the post until each has a root cause.

**Caveats to keep:**
- Emulators agreeing is not hardware truth. Three emulators can share a bug, and only console
  verification settles that.
- Old Gens movies sometimes depend on Gens itself. One Sonic 1 movie desyncs the same way in all
  three emulators.

---

## Draft post (LinkedIn)

**Using speedruns as a test suite for an emulator**

In my last post I described checking MesenCE's CPU cores instruction by instruction. CPU tests
can't tell you whether a whole game plays correctly, though. For that I turned to tool-assisted
speedruns.

A TAS is a recorded input file that plays a game perfectly, frame by frame. If an emulator gets
anything wrong (a timing, an interrupt, a pad read), the run falls out of sync, often thousands
of frames later.

The idea isn't new, and credit goes where it's due:
• the TASVideos and TASBot communities verify runs on real consoles and have found emulator bugs
  doing it;
• TiKevin83 runs 46 console-verified Game Boy TASes against Gambatte Speedrun as a test suite.

What I added for the Mega Drive:
→ every run plays in three emulators: MesenCE, Genesis Plus GX and PicoDrive. A difference only
  counts against MesenCE when the other two agree with each other;
→ game memory is compared every frame, so I see the exact frame where emulators stop agreeing,
  not just "it desynced";
→ BizHawk and Gens movies are imported automatically and matched to the exact ROM dump;
→ one reference reads the pads two frames late in some games. The pipeline measures that per
  movie against the other reference, never against MesenCE, so the check can't be tuned to make
  my emulator pass.

The same pipeline runs across more than 1,100 Mega Drive games. It has already found real bugs:
a mirrored lock-on window in Sonic & Knuckles, interrupt timing, and save RAM detection.

The honest caveat: emulators agreeing isn't hardware truth. Console verification is still the
gold standard. But automated three-way comparison lets you check far more games far more often,
and it tells you where to point the hardware.

github.com/bgyss/MesenCE

#emulation #softwaretesting #retrogaming #AI

---

## Draft thread (X)

**1/** Follow-up: CPU tests can't tell you whether a whole game plays right. So I'm using
tool-assisted speedruns as a test suite for MesenCE's Mega Drive core. 🧵

**2/** A TAS replays exact inputs frame by frame. Get one timing wrong and the run desyncs. The
idea isn't new: TASVideos/TASBot console verification and TiKevin83's Game Boy TAS suite got
there first.

**3/** What's new here: every movie runs in three emulators (MesenCE, Genesis Plus GX,
PicoDrive), with game RAM compared every frame. A difference only counts against MesenCE when
the other two agree with each other.

**4/** Found on the way: Genesis Plus GX acts on some games' pad reads 2 frames late. The
pipeline calibrates that per movie against PicoDrive, never against MesenCE, so it can't be
tuned to make my emulator pass.

**5/** The same pipeline covers 1,100+ Mega Drive games and has already fixed real bugs (Sonic &
Knuckles lock-on, interrupt timing, save RAM). Emulators agreeing isn't hardware truth, but it
shows where to look.

github.com/bgyss/MesenCE
