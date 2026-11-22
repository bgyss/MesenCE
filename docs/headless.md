# Native headless runner (`mesen-headless`)

`bin/mesen-headless` runs any MesenCE console without the .NET UI, an SDL audio device, a
window server or desktop configuration. It links only the emulation core (`Core/`,
`Utilities/`, Lua, SevenZip); `InteropDLL/`, `Sdl/`, `Linux/` and `MacOS/` are not part of it.
One process owns one `Emulator` and an isolated home folder inside the output directory.

```sh
mise run headless                       # make headless -> bin/mesen-headless
mise run test:headless                  # self-test (determinism, validation, Lua, timeout)

mesen-headless run --rom ROM --frames N --out NEW_DIR [--input INPUT.json] [--lua SCRIPT.lua]
                   [--region auto|ntsc|ntsc-j|pal] [--timeout SECONDS] [--allow-dirty]
                   [--load-state STATE.mss]
                   [--snap-every N]           # snaps/<frame>.png every N frames
                   [--state-log FILE]         # per-frame CRC32s of every RAM-like memory and the frame (OUT/FILE)
                   [--state-blocks]           # with --state-log: OUT/FILE.blocks, per-256-byte-block work RAM CRCs
                   [--dump-at F1,F2,...]      # dumps/<frame>/<MemoryType>.bin at the end of those frames
                   [--trace bus] [--trace-limit EVENTS] [--chip-audio]     # Mega Drive only
                   [--md-model md1va3|md1va6] [--md-pads 6,6]              # Mega Drive only
                   [--md-input-latch frame|vblank]  # Mega Drive test hook: when the pads take each frame's input
                   [--md-fm-core ymfm|nuked]                               # Mega Drive only
                   [--md-eeprom CHIP,BOARD]   # Mega Drive test hook, e.g. X24C01,Sega
                   [--md-mcd]                 # Mega Drive test hook: attach the Sega CD unit (Mode 1)
                   [--md-mcd-disc FILE.cue]   # with --md-mcd: a disc in the drive (Mode 1)
                   [--md-mcd-bios FILE]       # Sega CD BIOS for disc boot (ROM = a Sega CD .cue)
                   [--md-32x]                 # Mega Drive test hook: attach the 32X to any cartridge
                   [--md-32x-bios DIR]        # 32X: Sega's boot ROMs (32X_M/S/G_BIOS.bin) instead of MesenCE's replacements
                   [--battery FILE]...        # start from a save file (named <rom><FILE extension>)
                   [--save-battery]           # write battery files on exit into OUT/saves
mesen-headless validate OUT_DIR
mesen-headless doctor
mesen-headless nnes-compat ROM HOME LUA RESULT WAV   # nativekit NES capture host contract
```

## Run semantics

- Power-on state is deterministic: `EmulationFlags::TestMode` (the CI runner's switch for
  otherwise random power-on state), RAM initialised to zero, standard pads on ports 1-2, no
  game database or auto-configured input, the UI's default NES palette, a fixed 48 kHz WAV
  sample rate with dynamic rate control off.
- `Emulator::Run()` executes on the runner's thread (`Emulator::SetHeadlessMode`). It returns
  at a frame boundary: after exactly `--frames` frames, or at the end of the frame in which a
  Lua script called `emu.stop(code)`.
- A Lua error stops a headless run with stop code 65 (`ScriptingContext::ProcessLuaError`).
- `--timeout` is wall-clock. When it expires the run ends at the next frame boundary, the
  manifest says `"complete": false, "reason": "timeout"`, `limits.truncated` is true, the exit
  code is 3 and `validate` rejects the directory. A truncated capture is never presented as a
  qualified one.
- `--load-state` loads a save state (from a previous run's `final.mss`) after the ROM and before
  the first frame; `--frames` then counts from that point.
- `--md-input-latch` (Mega Drive): `frame` (default) applies frame N's `--input` from the wrap
  to line 0; `vblank` applies it from frame N's first VBlank line until frame N+1's, as Genesis
  Plus GX and BizHawk's Genplus-gx core do. It matters for TAS movies when a game reads the
  pads after the wrap in a long VBlank handler (The Lion King, findings C4). The manifest
  records it as `md_input_latch`.
- Builds use `-ffp-contract=off` (no fused multiply-add), so floating-point host code (audio
  resampler, filters) rounds identically on arm64 and x86-64 and captures are byte-identical
  across those hosts (`mise run test:md-xarch`).
- A build from a dirty tree (or without a known commit) refuses to run unless `--allow-dirty`
  is given; the manifest then records `"qualified": false`.

Exit codes: 0 success; 3 incomplete (timeout); 64 usage; 65 failure (load error, script
failure, nonzero `emu.stop` code).

## Input

```json
{"frames": {"1": {"1": ["START"]}, "10": {"1": [], "2": ["A", "B"]}}}
```

Frame numbers are 1-based (entry `"1"` applies to the first emulated frame); ports are
1-based. An entry holds until the next entry for the same port. Button names per console:

| Console | Names |
|---|---|
| NES, Game Boy | UP DOWN LEFT RIGHT START SELECT B A |
| SNES | A B X Y L R SELECT START UP DOWN LEFT RIGHT |
| SMS | UP DOWN LEFT RIGHT 1 2 PAUSE |
| PC Engine | UP DOWN LEFT RIGHT RUN SELECT I II |
| Mega Drive | UP DOWN LEFT RIGHT START A B C X Y Z MODE |

`#n` sets controller bit `n` directly for any device.

## Output directory and manifest (`mesen-headless/1`)

| File | Contents |
|---|---|
| `manifest.json` | provenance and completion record (below) |
| `final.png` | the console's last frame buffer through the default filter, no renderer involved |
| `audio.wav` | the mixed audio of the whole run |
| `final.mss` | a save state at the end of the run |
| `memory/<MemoryType>.bin` | every registered RAM-like memory (ROM and register views excluded) |
| `lua.log` | the script log, when `--lua` is given |
| `snaps/<frame>.png` | the frame buffer every N frames (`--snap-every N`), as `final.png` |
| `<FILE>` (`--state-log FILE`) | `mesen-state-log/1`, below |
| `dumps/<frame>/<MemoryType>.bin` | the state log's memories at the end of the listed frames (`--dump-at`) |
| `trace.txt` | Mega Drive bus trace (`--trace bus`), below |
| `ym2612.raw`, `psg.raw` | Mega Drive per-chip audio before mixing (`--chip-audio`): s16le stereo at the YM2612 sample rate (master clock / 1008); the manifest's `chip_audio` gives format and rate |
| `pwm.raw` | 32X PWM as mixed (`--chip-audio` with a 32X attached): s16le stereo, one sample per YM2612 sample |
| `pcm.raw`, `cdda.raw` | Sega CD PCM (12.5 MHz / 384) and CD audio after the fader (44.1 kHz), with `--chip-audio` and the unit attached |

`trace.txt` (`mesen-md-bus-trace/1`) has one event per line,
`<master clock> <device> <op> <width> <address> <value>` in hex, with device 1 = 68000,
2 = Z80, 3 = VDP, 4 = YM2612, 5 = PSG and op 1 = fetch, 2 = read, 3 = write. At most
`--trace-limit` events are written (default 5,000,000); the manifest's `trace` record gives
the event count, the limit and `truncated`, and a truncated trace makes the capture
`"qualified": false`.

`mesen-state-log/1` (`--state-log`, any console) is text: a `# mesen-state-log/1` line, a `#`
line naming the columns, then one line per frame: `frame clock polled pc subpc <CRC32 per
memory> frame`, all hex except the decimal frame number. `polled` is 1 when the game read its
controllers during the frame (the lag flag), `pc`/`subpc` the Mega Drive 68000 and Sega CD sub
68000 PC at the end of the frame (0 elsewhere). The memories are every RAM-like `MemoryType` the
console registers (ROM, BIOS, register views and CPU address spaces excluded), in enum order;
`frame` is the CRC32 of the console's raw frame buffer. With `--state-blocks` (Mega Drive),
`FILE.blocks` holds, per frame, 256 little-endian uint16 values: the low 16 bits of the CRC32 of
each 256-byte block of 68000 work RAM, in 68000 byte order. `tools/md-corpus` compares these logs
between runs and with the oracles' (`tools/libretro-capture/capture.py --state-log`).

The manifest binds: schema version; emulator commit and dirty flag; `qualified`; the exact
command; ROM SHA-256 and size (the ROM is not copied); input and Lua SHA-256; console and
requested/effective region; the deterministic settings; completion reason, frames, requested
frames, master clock, stop code and wall time; every artifact's path, SHA-256 and size; and
the limits (`frames`, `timeout_seconds`, `truncated`). `validate` re-hashes every artifact,
requires `final.png`, `final.mss` and `audio.wav`, rejects unlisted files and incomplete runs.

## nativekit compatibility

`nnes-compat` implements the private CLI of nativekit's out-of-tree NES capture host
(`consoles/nes/tools/capture/mesen_host.cpp`): it loads the collector Lua before the first
opcode, runs until the script calls `emu.stop`, and writes `final.png`, `ram.bin`, `oam.bin`,
`vram.bin` and `final.mss` next to the WAV plus the `{"version":1,...}` result file. Two of
nativekit's local MesenCE patches are now upstream in this fork: synchronous script stop
(`Emulator::SetHeadlessMode`) and the `emu.setInput` port fix (`Core/Debugger/LuaApi.cpp`).
One difference remains: a stop requested mid-frame takes effect at the end of that frame,
where nativekit's patched NES loop stops at the end of the current instruction.

The trace and checkpoint fields nativekit's Mega Drive workflow needs from MesenCE are listed
in [`research/md-nativekit-trace-requirements.md`](research/md-nativekit-trace-requirements.md).
