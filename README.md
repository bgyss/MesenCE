# Mesen Community Edition: LLM + formal verification fork

> **This is an independent fork** of [MesenCE](https://github.com/nesdev-org/MesenCE), run as
> its own project. It's an example of developing an emulator with LLM assistance, backed by
> automated differential testing, sanitizers and formal verification (TLA+). Fixes are applied
> here directly. The release and build links below still point at upstream MesenCE, which does
> not include the Mega Drive core.
>
> What this fork has found and fixed so far:
>
> | System | Area | Result |
> |---|---|---|
> | NES | OAM DMA scheduler (TLA+ model + test ROM) | **Fixed:** OAM DMA restarted when a duplicated DMC DMA landed inside it |
> | NES | PPU (UndefinedBehaviorSanitizer) | **Fixed:** sprite index −1 led to out-of-bounds reads in the HD Pack Builder |
> | NES | CPU vs 2.56M SingleStepTests | Clean (divergences documented) |
> | Game Boy | Interrupts (TLA+ model + scenarios) | **Fixed:** `EI; HALT` with an interrupt pending returned past the HALT |
> | Game Boy | SM83 CPU vs 500k SingleStepTests | Clean (divergences documented) |
> | NES + GB | Save states (509 test ROMs) | Clean |
> | Mega Drive | New core: 68000 vs 317k SingleStepTests, 73 synthetic cartridges vs BlastEm and expected values | See below |
>
> Details: [docs/research/](docs/research/). Start with
> [formal-verification-tla-lean.md](docs/research/formal-verification-tla-lean.md),
> [nes-verification-findings.md](docs/research/nes-verification-findings.md) and
> [gb-verification-findings.md](docs/research/gb-verification-findings.md).
> To build and test, see [CLAUDE.md](CLAUDE.md) (Nix dev shell + `mise` tasks); to contribute, see
> [CONTRIBUTING.md](CONTRIBUTING.md).

Mesen is a multi-system emulator for Windows, Linux, and macOS. It supports NES, SNES, Game Boy (GB/SGB/GBC), Game Boy Advance, PC Engine, SMS/Game Gear, WonderSwan (WS/WSC) and, in this fork, **Mega Drive / Genesis**.

## Mega Drive / Genesis (this fork)

Cartridge games (`.md`, `.gen`, `.bin`, `.smd`, also in `.zip`/`.7z`). Sega CD, 32X and Pico are
not supported.

- **Emulation:** 68000 (derived from ares), Z80, VDP (all display modes, shadow/highlight,
  interlace, DMA and FIFO timing, mid-line palette/scroll/register changes), YM2612 (the
  cycle-accurate Nuked-OPN2 core by default, or the faster ymfm) and PSG; NTSC, PAL and Japanese
  consoles, with or without TMSS.
- **Cartridges:** battery RAM, serial EEPROM saves (35 known ROMs), the Sega bank-switching mapper
  (Super Street Fighter II).
- **Input:** 3- and 6-button pads, Sega Team Player (4 pads per port), Sega Mouse, Sega Menacer and
  Konami Justifier (aimed with the mouse).
- **Tools:** Game Genie and Pro Action Replay cheats, save states, rewind, movies, netplay, the
  debugger for the 68000 and Z80 (breakpoints, trace logger, code/data logger, memory, tile, tilemap,
  sprite, palette and register viewers, event viewer), Lua scripting, and a headless runner with a
  bus trace (`docs/headless.md`).
- **Settings:** Options → Mega Drive (region, console model, FM core, volumes, controllers, key
  bindings, video).

How it was built and checked: [mega-drive-genesis-feasibility.md](docs/research/mega-drive-genesis-feasibility.md)
(the plan), [md-verification-findings.md](docs/research/md-verification-findings.md) (every test
and fix, with sources), and [md-feature-parity.md](docs/research/md-feature-parity.md) (what is
still missing compared with Genesis Plus GX, BlastEm and ares).

MesenCE is a community-managed fork, created to maintain and expand this emulator into the future.

## Releases

The latest stable version is available from the [releases page on GitHub](https://github.com/nesdev-org/MesenCE/releases).

## Development Builds

[![Mesen](https://github.com/nesdev-org/MesenCE/actions/workflows/build.yml/badge.svg)](https://github.com/nesdev-org/MesenCE/actions/workflows/build.yml?query=branch%3Amaster)

* [Windows](https://nightly.link/nesdev-org/MesenCE/workflows/build/master/Mesen%20%28Windows%20-%20net10.0%20-%20AoT%29.zip)
  * Windows 7 or higher is required. Windows 7 users must use SP1 and have all updates installed.
* [Linux x64](https://nightly.link/nesdev-org/MesenCE/workflows/build/master/Mesen%20%28Linux%20-%20ubuntu-22.04%20-%20clang_aot%29.zip)  (requires **SDL2**)  
* [Linux ARM64](https://nightly.link/nesdev-org/MesenCE/workflows/build/master/Mesen%20%28Linux%20-%20ubuntu-22.04-arm%20-%20clang_aot%29.zip)  (requires **SDL2**)  
* [macOS - Intel](https://nightly.link/nesdev-org/MesenCE/workflows/build/master/Mesen%20%28macOS%20-%20macos-15-intel%20-%20clang_aot%29.zip)  (requires **SDL2**)  
* [macOS - Apple Silicon](https://nightly.link/nesdev-org/MesenCE/workflows/build/master/Mesen%20%28macOS%20-%20macos-15%20-%20clang_aot%29.zip)  (requires **SDL2**)  

#### <ins>Notes</ins> ####

* Other builds are also available in the [Actions](https://github.com/nesdev-org/MesenCE/actions/workflows/build.yml?query=branch%3Amaster) tab.
* **macOS**: Builds are self-signed and will require approval via Gatekeeper before they are able to be run.  
* **SteamOS**: See [SteamOS.md](SteamOS.md)  

## Compiling

See [COMPILING.md](COMPILING.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md)

## License

Mesen is available under the GPL V3 license.  Full text here: <http://www.gnu.org/licenses/gpl-3.0.en.html>

Copyright (C) 2014-2026 Sour, 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
