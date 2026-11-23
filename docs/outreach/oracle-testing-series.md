# Social post series: three emulators, 59 speedruns, and what they found

These drafts follow the intro post ([verification-posts.md](verification-posts.md)) and replace the
draft in [tas-oracle-testing.md](tas-oracle-testing.md), which was written before the TAS batches
finished. Keep that file for its prior-work sources.

Figures are from [`md-verification-findings.md`](../research/md-verification-findings.md),
sections C1-C4, as of commit `10379284` (2026-11-22).

**Before posting:**
- **Links:** this work is on the `sega-cd` branch, not `master`. Link
  `github.com/bgyss/MesenCE/tree/sega-cd` until it's merged, then the repository root. The
  findings log is at `docs/research/md-verification-findings.md` on the same branch.
- **Novelty claim:** word it as "I couldn't find anyone doing this", never "first". The prior-work
  search was a web search, not a literature review. Credit TASVideos/TASBot console verification
  and TiKevin83's Game Boy TAS suite in any post that makes the claim; people will name them in
  replies anyway.
- **LLM disclosure:** the code was written with Claude (Claude Code). Every post that describes
  the code says so. Check each subreddit's rules on AI-generated content before posting.
- **"Bugs fixed" vs "differences":** only the items under "What it found" in post 2 are MesenCE
  bugs with a root cause. Don't promote the open movie divergences to bugs.
- **Agreement isn't hardware truth.** Three emulators can share a bug. Keep that caveat in every
  long post.

The series has six LinkedIn posts (one a week works), an X thread, a long Reddit post for
r/EmuDev, and a short Mastodon/Bluesky version.

---

## LinkedIn 1: the method

**How do you know an emulator plays a whole game correctly?**

CPU test suites check one instruction at a time. My fork of the Mesen2 emulator (MesenCE) passes
millions of those, but they can't tell you whether Streets of Rage 2 plays the way it does on a
real Mega Drive.

So I built a second line of checks: run the same game in three emulators and compare what the
game itself is thinking, every frame.

→ **Three emulators.** Every run plays in MesenCE, Genesis Plus GX and PicoDrive. They're
independent codebases written by different people over two decades.

→ **A rule for blame.** A difference counts against MesenCE only when the other two agree with
each other and not with MesenCE. If one reference disagrees with everyone, that's noted and scored
low.

→ **Game state, not screenshots.** Each emulator hashes the game's 64 KB of work RAM in 256 blocks
every frame. The comparison lines the runs up frame by frame and reports the exact frame where they
stop agreeing.

→ **Two kinds of input.** The game's attract mode, across 1,130 Mega Drive titles plus the 32X and
Sega CD libraries; and tool-assisted speedruns (TAS), recorded input files that play a game
perfectly from power-on to the credits.

Speedruns are the strict test. Get one timing wrong and the run falls out of step, often thousands
of frames later, because the game ends up in a slightly different state when the next input
arrives.

The idea of checking emulators against speedruns isn't new: the TASVideos and TASBot communities
verify runs on real consoles, and TiKevin83 runs console-verified Game Boy TASes as a test suite.
What I couldn't find anywhere was an automated three-way comparison of game state, every frame,
across a library and a set of movies.

Next post: what it found.

github.com/bgyss/MesenCE

#emulation #softwaretesting #retrogaming

---

## LinkedIn 2: what it found

**Bugs a three-emulator comparison found in my Mega Drive core**

Last week I described running games in three emulators and comparing game memory every frame.
Here's what it turned up in MesenCE, my fork of the Mesen2 emulator.

**The library pass (attract mode, 1,212 titles, 35 minutes on 10 cores):**
• Sonic & Knuckles showed its "NO WAY?" screen instead of the title: the lock-on cartridge slot was
  being mirrored from the game's own ROM.
• Fatal Rewind, The Killing Game Show and Sesame Street Counting Cafe crashed or hung: the 68000
  took an interrupt one instruction too early after enabling interrupts.
• HardBall III, HardBall '95, Madden NFL 98 and Buck Rogers failed or froze: save RAM wasn't
  detected on boards without a header entry.
• Star Odyssey and the Radica Arcade Legends units need cartridge boards MesenCE didn't emulate.
  It does now.

**The speedrun batch (59 movies, 6.9 million frames, 91 minutes, no failed runs):**
• Mega Turrican froze for 19,000 frames. The game wasn't hung: it was paused. A one-frame Start
  press landed during gameplay instead of a screen transition, because MesenCE ran the game's
  logic slightly fast. Two timing fixes came out of it: a DMA crossing the end of the frame used
  the wrong transfer rate, and the 68000's accesses to the sound CPU's memory were a cycle too
  cheap.
• That second fix also fixed Streets of Rage 2, where MesenCE's player lost a life the other two
  emulators didn't.

Every fix comes with a test cartridge that measures the timing in all three emulators, so it
stays fixed.

None of these were exotic. They were the kind of thing nobody notices until something independent
checks every frame of every game.

The code was written with Claude (Claude Code), under one rule: nothing goes in without a
reproduction, a source, a regression test and a written finding.

github.com/bgyss/MesenCE

#emulation #softwaretesting #retrogaming #AI

---

## LinkedIn 3: the Mega Turrican story

**The bug that looked like a hang and turned out to be a pause button**

My favourite find so far, from running speedruns through three Mega Drive emulators.

In a Mega Turrican speedrun, MesenCE sat on one screen from frame 1,200 to 20,700 while the other
two emulators played through the first level.

It wasn't hung. Stepping through the 68000 code showed the game's main loop running fine, inside
its pause screen. The movie presses Start for exactly one frame around frame 912. In the other
emulators that frame fell in a screen transition, which ignores Start. In MesenCE it fell in
gameplay, so the game paused and waited for a Start press the movie never sent.

Why was the game a frame early? MesenCE finished decompressing a level one frame ahead of the
references, at frame 585. Tiny loop cartridges, built in Python and run in all three emulators,
found two reasons:

1. A graphics DMA that crossed the end of the frame was charged the fast blanking-time rate for
   the last line instead of the slower active-display rate.
2. When the 68000 talks to the sound CPU's memory, both references charge about one more cycle
   per access than MesenCE did. Mega Turrican feeds its music from the 68000, thousands of times
   a second.

Fixing both put the whole movie in step with the references. It also fixed Streets of Rage 2,
which uses the same kind of sound driver.

A one-cycle difference, multiplied by thousands of accesses, moved a single button press by a
frame and froze the game for five minutes of play. That's why speedruns make such a strict test.

github.com/bgyss/MesenCE

#emulation #debugging #retrogaming

---

## LinkedIn 4: when the test tests the references

**My test oracles had quirks too**

Comparing an emulator against two others only works if you understand the references. Some of
the most useful findings in this project were about Genesis Plus GX and PicoDrive, not MesenCE.

• **Input lag that depends on the game.** The libretro build of Genesis Plus GX acts on some
  games' pad reads two frames later than the other emulators. Sonic 3 needs the two-frame shift;
  Sonic 1 needs none. The pipeline now measures the shift for each movie, by comparing Genesis
  Plus GX against PicoDrive only. MesenCE never takes part in the choice, so the check can't be
  quietly tuned to make my emulator pass.

• **When "this frame's input" arrives.** The Lion King reads the pad late in a long VBlank
  handler. MesenCE handed the game the next frame's input there; the emulator the movie was
  recorded in hands it over at VBlank. Neither is a bug, but they play differently. MesenCE now
  has a matching mode for movies recorded in BizHawk's Genesis core.

• **A false alarm in my own comparison.** Comix Zone looked like a divergence at frame 3,149. The
  screenshots agreed, though: the frame-alignment tracker had wandered to a 60-frame offset
  through mostly static memory. It now always also tries the offsets near zero.

Each of these got written down as "not a MesenCE bug", with the evidence. A verification log that
only records wins can't be trusted.

github.com/bgyss/MesenCE

#emulation #softwaretesting

---

## LinkedIn 5: the limit of the method

**Where speedrun testing stops working, and how I found out**

The Lion King speedrun split from both reference emulators at frame 2,088 because MesenCE ran the
68000 slightly too fast. Its sound driver reads samples from the cartridge about 279 times a
frame, and each time the Z80 sound CPU borrows the 68000's bus. Both references slow the 68000 for
that; MesenCE didn't.

Kabuto's hardware measurements describe how the real arbiter works: the 68000 loses the bus for
about 11 of its cycles per access, but only stalls if it needs the bus during that window. I
implemented exactly that. On test cartridges, MesenCE now lands within about 1% of Genesis Plus GX,
where before it lost no time at all.

The Lion King movie now stays in step to the end. But across all 59 movies, the number in step
with both references went from 43 to 41. Shinobi III got worse, among others.

So I looked at Shinobi III. Its idle loop rotates the game's random number once every 56 CPU
cycles until the next frame starts. The random value the game draws depends on exactly how much
spare time the previous frames had. Under any timing change of a few dozen cycles, whether the
movie stays in step is close to a coin flip.

What I took from it:
• Movie agreement is strong evidence for games that don't care about spare CPU time.
• For games that feed spare time into their random numbers, it's noise, and a fix can look like a
  regression.
• So the controlled measurements decide. I kept the hardware-based model and recorded why.

A metric you optimise blindly will eventually lie to you. Knowing where yours stops working is
part of the result.

github.com/bgyss/MesenCE

#emulation #softwaretesting #metrics

---

## LinkedIn 6: the LLM angle

**What I learned building an emulator core with an LLM: verification is the work now**

Over the last weeks I've added Mega Drive, Sega CD and 32X support to my fork of the Mesen2
emulator, written with Claude (Claude Code). I directed the work and set the rules. The rules
mattered more than the code.

The rule: nothing goes in without
• a reproduction (a failing test, a ROM, a model counterexample);
• a source for the correct behaviour (a manual, hardware notes, or two reference emulators that
  agree);
• a regression test wired into the build;
• a written finding, including results that turned out to be "not a bug".

What that made possible:
→ CPU cores checked against the SingleStepTests suites: millions of single-instruction cases for
  the 68000, Z80 and SH-2.
→ Game-level checks against two independent emulators, across 1,200 titles and 59 speedruns,
  every frame.
→ Small test cartridges generated for each timing question, run in all three emulators, so a fix
  is measured rather than argued.
→ A findings log that says where the evidence is weak: "judgment call", "two emulators agree, no
  hardware measurement".

LLMs make code cheap. They don't make it correct. The leverage comes from making verification
cheap too: when generating a test cartridge or a comparison tool takes minutes, you can afford to
check everything.

github.com/bgyss/MesenCE

#AI #softwareengineering #emulation #softwaretesting

---

## X thread

**1/** I've been testing my Mega Drive emulator by playing speedruns in three emulators at once
and comparing the game's memory every frame. 59 movies, 6.9M frames, 91 minutes. Here's what it
found 🧵

**2/** Setup: MesenCE (my Mesen2 fork), Genesis Plus GX and PicoDrive. A difference only counts
against MesenCE when the other two agree. Work RAM is hashed in 256 blocks per frame, so I get the
exact frame where they split.

**3/** Credit where due: TASVideos/TASBot verify runs on real consoles, and TiKevin83 runs Game Boy
TASes as a test suite. What I couldn't find was an automated three-way, every-frame comparison
across a whole library and a set of movies.

**4/** Library pass (1,212 titles): Sonic & Knuckles' lock-on slot was mirrored, three games
crashed on interrupt timing, four lost their saves, and two cartridge boards were missing. All
fixed.

**5/** Best find: Mega Turrican "froze" for 19,000 frames. It was paused. The game ran a frame
fast, so a one-frame Start press hit gameplay instead of a transition. Cause: a DMA rate on the
frame's last line, and sound-CPU memory accesses 1 cycle too cheap.

**6/** The method tests the references too: libretro Genesis Plus GX reads some games' pads 2
frames late. The pipeline calibrates that per movie against PicoDrive only, never against MesenCE,
so it can't be tuned to make my emulator pass.

**7/** And its limit: Shinobi III rotates its RNG every 56 cycles of idle time. A correct
bus-timing fix (within 1% of GPGX on test carts) made it desync. For games like that, movie
agreement is a coin flip, so controlled measurements decide.

**8/** Caveat: emulators agreeing isn't hardware truth. Console verification is still the gold
standard. This tells you where to point the hardware.

Written with Claude Code; every fix has a repro, a source and a regression test.
github.com/bgyss/MesenCE

---

## Reddit (r/EmuDev)

**Title:** Differential testing a Mega Drive core against Genesis Plus GX and PicoDrive with 59
TAS movies: per-frame work RAM comparison, what it found, and where it stops working

**Body:**

I maintain an independent fork of Mesen2 (MesenCE) that adds Mega Drive, Sega CD and 32X support.
The code is LLM-written (Claude Code) under a strict evidence rule: every fix needs a repro, a
source for the intended behaviour, a regression test and a findings entry, including "not a bug"
results. This post is about the game-level verification, since CPU suites (SingleStepTests 68000,
Z80, SH-2) only get you so far.

**Method**
- Every session runs in MesenCE, Genesis Plus GX (libretro, `60bee4c`) and PicoDrive.
- Each logs 256 CRCs of 256-byte work RAM blocks per frame. GPGX and PicoDrive keep work RAM
  word-swapped on little-endian hosts, so the capture swaps it back.
- Whole-RAM CRCs are useless: Sonic 2 stops matching after ~150 frames while staying in step,
  because each emulator ends its frame at a different point in the game's code (stack, tables
  mid-update). Per-block hashes, a mask for always-noisy blocks and offset tracking fix that.
- Blame rule: a divergence counts against MesenCE only when the two oracles agree with each other.
- Two inputs: attract mode across the library (1,130 MD titles, 40 32X, 30 Sega CD), and imported
  BizHawk/Gens movies matched to ROMs by MD5 or by hand.
- Libretro GPGX applies pad input 2 frames late in some games (Sonic 3: +2, Sonic 1: 0). The
  offset is calibrated per movie against PicoDrive only, so MesenCE can't influence its own test.

**Bugs found and fixed**
- S&K lock-on window was mirrored from the game ROM.
- Interrupt enable latency and acknowledge priority (Fatal Rewind, Killing Game Show, Sesame Street
  Counting Cafe).
- Battery RAM detection on header-less boards (HardBall III/'95, Madden 98, Buck Rogers).
- SF-004 (Star Odyssey) and Radica boards.
- A 68000→VDP DMA running on the frame's last line got the VBlank rate (167/205 bytes per line)
  instead of 16/18. The VDP status flag already cleared there.
- 68000 accesses to `$A00000-$A0FFFF` were a cycle too cheap. Both oracles charge +1; GPGX
  documents it in `mem68k.c` (Pacman 2, Puyo Puyo 2). Together these took Mega Turrican from a
  19,000-frame "freeze" (really the pause screen) to in step for the whole movie, and fixed
  Streets of Rage 2.

**Z80 → 68000 bus arbitration, and the limit of movie testing**

The Lion King streams DAC samples through the Z80 bank window (~279 reads per frame). GPGX loses
about 10.5 68000 cycles per Z80 access; MesenCE lost nothing. Following Kabuto's notes, I modelled
the arbiter: the 68000 finishes its bus cycle, loses the bus for about 11 cycles, and stalls only if
it needs the bus inside that window. The Z80 waits out DMA and FIFO stalls. Loop carts:

| Loop | before | now | GPGX | PicoDrive |
|---|---|---|---|---|
| NOP; Z80 bank reads back to back | 5,723 | 4,662 | 4,673 | 4,950 |
| RAM read; Z80 bank reads | 3,702 | 3,024 | 3,023 | 3,204 |
| NOP; Z80 PSG writes | 5,722 | 5,106 | 5,148 | 5,719 |
| Z80 bank reads during a 4 KB DMA | +119 | +0 | +0 | +119 |

Lion King is now in step to the end, but the batch went from 43 to 41 movies in step with both
oracles. Shinobi III explains it: its idle loop does `ROL.W $FF001C` once per ~56 spare cycles
until VBlank, so its RNG is a function of exact spare time. From the seed at frame 428 that word
differs between all four runs almost every frame, yet three of them had stayed in step for 46,000
frames. Under a timing change of a few dozen cycles, whether such a movie survives is a draw. I
kept the model on the strength of the hardware notes and the carts, and documented the limit.

**Caveats**
- Oracle agreement isn't hardware truth. Every timing fix above is labelled "judgment call: two
  emulators agree, no hardware measurement" in the log. If anyone has hardware and wants to run
  the loop carts, they're generated by `tests/MD/timing/check_bus_timing.py` and
  `tests/MD/dma/check_dma_timing.py`.
- Gens movies sometimes depend on Gens itself.
- I'm not distributing movies or ROMs. Movies are downloaded by hand (TASVideos' robots.txt).

Findings log, with method, repros and sources: `docs/research/md-verification-findings.md`,
sections C1-C4. [link]

Thanks to the TASVideos authors, the Genesis Plus GX and PicoDrive developers, and Kabuto for the
arbitration measurements.

---

## Mastodon / Bluesky (short)

Testing my Mega Drive emulator: 59 speedruns played in it, Genesis Plus GX and PicoDrive, game RAM
compared every frame. It found real timing and save bugs, and its own limit: Shinobi III's RNG
makes movie sync a coin flip. [link] #emulation
