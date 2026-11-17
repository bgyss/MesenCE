# Mega Drive / Genesis: feature parity with other emulators

Scope: cartridge games on the Mega Drive / Genesis. Sega CD (Mega-CD), 32X, Pico, and Master System
mode through the Power Base Converter are separate systems and out of scope here. Evidence for
each implemented item is in [`md-verification-findings.md`](md-verification-findings.md).

The comparison columns are what those emulators are generally known to offer (Genesis Plus GX,
BlastEm, ares). They are a summary for planning, not an audited feature list of each project.

Legend: ✅ implemented and tested · ⚠️ implemented with a documented approximation · ❌ missing

## Emulation

| Feature | MesenCE | GPGX | BlastEm | ares | Notes |
|---|---|---|---|---|---|
| 68000 | ✅ | ✅ | ✅ | ✅ | ares-derived; SingleStepTests 317,242 pass |
| Z80 | ✅ | ✅ | ✅ | ✅ | shared with SMS; SingleStepTests 1,604,000 pass |
| VDP: modes, planes, window, sprites, shadow/highlight, interlace 2 | ✅ | ✅ | ✅ | ✅ | synthetic corpus 60/60 |
| VDP: FIFO, DMA timing, H/V counters, interrupts | ⚠️ | ✅ | ✅ | ✅ | slot model from the Sega manual |
| Mid-line raster effects | ❌ | ✅ | ✅ | ✅ | renderer works per line |
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
| I²C EEPROM saves (≈30 titles) | ❌ | ✅ | ✅ | ✅ | needs a per-game chip/pin table |
| Game-specific database (region fixes, special boards) | ❌ | ✅ | ✅ | ✅ | |
| Unlicensed / pirate mappers | ❌ | ✅ | some | some | |

## Input

| Feature | MesenCE | GPGX | BlastEm | ares | Notes |
|---|---|---|---|---|---|
| 3- and 6-button pads, key binding UI | ✅ | ✅ | ✅ | ✅ | |
| Team Player / EA 4-Way Play multitaps | ❌ | ✅ | ✅ | ✅ | needed for 3-4 player sports games |
| Sega Mouse | ❌ | ✅ | ✅ | ✅ | |
| Menacer / Justifier light guns | ❌ | ✅ | ❌ | ✅ | |

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

1. **I²C EEPROM saves:** the chip types (24C01, 24C02, 24C08, 24C16, 24C65) and the cartridge
   pin wiring per title need a sourced table. Genesis Plus GX's table is under a non-commercial
   license, so it cannot be copied into this GPLv3 fork; the facts need an independent source.
2. **Multitaps** (Team Player, EA 4-Way Play): protocols are documented; many sports games use them.
3. **Mid-line rendering:** needed for raster effects in some games and demos.
4. **Model 1 output low-pass filter**, as an option.
5. **Sega Mouse and light guns.**
