# Nuked-OPN2 (vendored)

Cycle-accurate YM3438/YM2612 emulator based on die-shot reverse engineering, by Alexey
Khokholov (Nuke.YKT). LGPL-2.1-or-later (see `LICENSE`), compatible with MesenCE's GPLv3.

- Source: https://github.com/nukeykt/Nuked-OPN2
- Commit: 335747d78cb0abbc3b55b004e62dad9763140115 (2023-08-11)
- Files: `ym3438.c`, `ym3438.h`, unmodified.

Used by the Mega Drive core as the optional "Nuked-OPN2" FM core (`MdConfig::FmCore`); the
default core is ymfm (`Utilities/Audio/ymfm`). See `Core/MD/MdApu.cpp` and
`docs/research/md-verification-findings.md`.
