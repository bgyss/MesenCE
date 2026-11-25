# Mega Drive / Genesis: feature parity with other emulators

Scope: cartridge games on the Mega Drive / Genesis. The Sega CD (Mega-CD) and 32X add-ons are
supported too, but are covered by their own plans and findings
([`sega-cd-feasibility.md`](sega-cd-feasibility.md), [`32x-feasibility.md`](32x-feasibility.md));
Sega CD 32X, Pico, and Master System mode through the Power Base Converter are not supported and
are out of scope here. Evidence for
each implemented item is in [`md-verification-findings.md`](md-verification-findings.md).

The comparison columns are what those emulators are generally known to offer (Genesis Plus GX,
BlastEm, ares). They are a summary for planning, not an audited feature list of each project; the
ares column for controllers was checked against its source tree (`md/controller`: pads and the Mega
Mouse; `md/cartridge/board`: J-Cart).

Legend: ✅ implemented and tested · ⚠️ implemented with a documented approximation · ❌ missing

## Emulation

| Feature | MesenCE | GPGX | BlastEm | ares | Notes |
|---|---|---|---|---|---|
| 68000 | ✅ | ✅ | ✅ | ✅ | ares-derived; SingleStepTests 317,242 pass |
| Z80 | ✅ | ✅ | ✅ | ✅ | shared with SMS; SingleStepTests 1,604,000 pass |
| VDP: modes, planes, window, sprites, shadow/highlight, interlace 2 | ✅ | ✅ | ✅ | ✅ | synthetic corpus 60/60 |
| VDP: FIFO, DMA timing, H/V counters, interrupts | ⚠️ | ✅ | ✅ | ✅ | slot model from the Sega manual |
| Mid-line raster effects | ⚠️ | ✅ | ✅ | ✅ | lines drawn in segments at register/VRAM/CRAM/VSRAM writes; no CRAM dots |
| YM2612 (fast core) | ✅ | ✅ | ✅ | ✅ | ymfm |
| YM2612 cycle-accurate core | ✅ | ✅ | — | — | Nuked-OPN2, selectable |
| PSG | ✅ | ✅ | ✅ | ✅ | |
| Model 1 audio low-pass filter | ❌ | ✅ | ✅ | ✅ | |
| DRAM refresh | ⚠️ | ✅ | ✅ | ✅ | BlastEm's model |
| Z80 bank-window 68000 stall | ❌ | ✅ | ✅ | ✅ | |
| TMSS (VA6+) | ✅ | ✅ | ✅ | ✅ | |
| NTSC / PAL / Japan regions, header auto-detect | ✅ | ✅ | ✅ | ✅ | |

## Cartridges and saves

| Feature | MesenCE | GPGX | BlastEm | ares | Notes |
|---|---|---|---|---|---|
| .md / .gen / .bin / .smd, zip / 7z | ✅ | ✅ | ✅ | ✅ | |
| Battery SRAM from the header (incl. odd/even lanes, $A130F1) | ✅ | ✅ | ✅ | ✅ | .srm |
| Sonic 3 & Knuckles lock-on image (SRAM overlapping ROM) | ⚠️ | ✅ | ✅ | ✅ | follows from $A130F1 support; not tested on the real image |
| Sega bank mapper (SSF2, >4 MiB) | ✅ | ✅ | ✅ | ✅ | |
| I²C EEPROM saves (35 ROMs) | ✅ | ✅ | ✅ | ✅ | ares' hash list, MAME's board wiring; `.eep` |
| Game-specific database (region fixes, special boards) | ❌ | ✅ | ✅ | ✅ | |
| Unlicensed / pirate mappers | ❌ | ✅ | some | some | |

## Input

| Feature | MesenCE | GPGX | BlastEm | ares | Notes |
|---|---|---|---|---|---|
| 3- and 6-button pads, key binding UI | ✅ | ✅ | ✅ | ✅ | |
| Team Player multitap | ✅ | ✅ | ✅ | ❌ | pads only (no mouse behind it) |
| EA 4-Way Play | ❌ | ✅ | ✅ | ❌ | no source under a usable license found |
| Sega Mouse | ✅ | ✅ | ✅ | ✅ | |
| Menacer / Justifier light guns | ⚠️ | ✅ | ❌ | ❌ | fixed latency per gun; blue Justifier only |
| J-Cart (extra ports on the cartridge) | ❌ | ✅ | ✅ | ✅ | Micro Machines 2/Military, Pete Sampras |

## Frontend and tools

| Feature | MesenCE | GPGX | BlastEm | ares | Notes |
|---|---|---|---|---|---|
| Save states, rewind, movies, netplay | ✅ | ✅ | some | some | MesenCE's shared infrastructure; save/replay tested |
| Cheats: Game Genie, Pro Action Replay | ✅ | ✅ | ❌ | ✅ | |
| Debugger for 68000 and Z80 (breakpoints, trace, CDL, labels) | ✅ | some | ✅ | some | |
| Tile, tilemap, sprite, palette, register, event viewers | ✅ | ❌ | ✅ | some | |
| Lua scripting | ✅ | ❌ | ❌ | ❌ | |
| Settings page (region, model, FM core, volumes, pads) | ✅ | ✅ | ✅ | ✅ | |
| Headless runner with bus trace | ✅ | ❌ | ❌ | ❌ | |

## Most useful next steps

1. **Testing with commercial cartridges:** EEPROM hash matches, gun calibration and Team Player
   games have only been tested with synthetic cartridges.
2. **J-Cart** (two extra pad ports on Codemasters cartridges) and the **EA 4-Way Play**.
3. **CRAM dots and DMA timing during active display**, for the remaining raster effects.
4. **Model 1 output low-pass filter**, as an option.
5. **Game database** (region fixes, special boards, unlicensed mappers).
