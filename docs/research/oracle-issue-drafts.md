# Oracle issue drafts

Behaviour of the oracle emulators that md-corpus found and that looks wrong against the hardware
manuals. These are drafts for the oracles' own trackers: **not sent**. They are written so that
someone could file them as they are, after checking them against the current version of the
oracle. MesenCE itself is not changed for them; md-corpus records the affected sessions as
reviewed (`tools/md-corpus/compare.py`, `REVIEWED`).

Oracle version for both: PicoDrive libretro core `2.05-1890c29` (RetroArch, macOS arm64).

## PicoDrive: 32X master V interrupt taken many times per frame (36 Great Holes)

**Title:** 36 Great Holes Starring Fred Couples (32X) (JU) [!]: the master SH-2's V interrupt
handler runs about 100 times per frame in two windows of the attract mode.

**What we see:**
- The game's master V interrupt handler (`$06001210`, reached through the game's level table at
  `$06001424` for levels 12-13 only) adds 1 to a word at `$0600D24C`. Nothing else writes it.
- In PicoDrive the word grows by 60-450 per two frames between frames 2,948 and 3,000 of a
  power-on run, and again around frames 3,358-3,378; elsewhere by 2 per two frames.
- The 32X V interrupt is raised once per frame (32X Hardware Manual, interrupt control, pp.
  22-24). MesenCE adds 1 per frame throughout.

**How it was measured:** `tools/libretro-capture/capture.py --dump-at` (MesenCE repository) reads
the SDRAM from the core's exported `Pico32xMem` each frame. No input; NTSC (US); 3-button pads.

**Not established:** whether the handler is entered through a repeated V interrupt or another path
that reaches the same routine; the windows coincide with scene changes. Before filing, confirm
with PicoDrive's debugger or logging which interrupt is taken.

**Impact on the game:** none visible (the demo runs in both emulators since MesenCE's fix C8).

## PicoDrive: SH-2 cartridge reads faster than the manual's minimum wait (Darxide)

**Title:** Darxide (Europe): the attract intro, drawn by the master SH-2 from code in cartridge ROM,
runs faster than the 32X's documented cartridge wait states allow.

**What we see:**
- PicoDrive reaches the language screen at about frame 2,600-2,700 of a power-on run (PAL), MesenCE
  at about 3,300-3,400.
- The master SH-2 executes much of the intro from cartridge ROM (`$0204B1F0` is its hottest block).
  With cartridge reads at the manual's minimum of 6 waits, MesenCE's intro takes about 700 frames
  longer; with 1 wait (an experiment) it matches PicoDrive's pace.
- 32X Hardware Manual 4.4 (p. 77): "32X Mode and Cartridge ROM — SH2 (Read/Write): 6 wait (min) -
  15 wait (max)".

**Not established:** the real hardware's pace (no timed recording of the intro on hardware).
The manual's range bounds it from below; MesenCE uses the minimum.
