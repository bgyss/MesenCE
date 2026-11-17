# Mega Drive: trace and substitution fields MesenCE must expose for nativekit

Inspected 2026-11-16 from the sibling nativekit checkout (commit `6f21b99`):
`consoles/md/tests/oracle/TRACE_FORMAT.md` (NMDT v2 trace), `consoles/md/tests/oracle/decomp_abi.h`
(NMDH v1 decomp execution hooks), `tools/toolchains/README.md` (BlastEm patches 0001-0005) and
`consoles/md/docs/decomp/lockstep.md`. These describe nativekit's *pinned BlastEm candidate*;
they are requirements for a replacement, not hardware facts. Status refers to this repository.

## 1. Bus trace (NMDT v2 equivalent)

One record per event: 64-bit master clock, device, operation, width, address, value; clocks
nondecreasing per device; versioned header and a footer with the record count.

| Field / capability | Needed for | MesenCE status |
|---|---|---|
| 68000 byte/word reads and writes, prefetches tagged as fetches | decomp intake, synthetic comparison | M1: `M68kCore` reports every bus cycle with UDS/LDS and access kind (data, program, TAS) |
| Z80 one-byte reads/writes, opcode fetches tagged separately | Z80 sound-driver decomp | M2/M5: Z80 memory callbacks via the debugger's typed operations |
| VDP data/control port reads and writes (address 0 data, 4 control) | VDP command streams | M4 |
| 68K-to-VDP DMA source slots (source address, word, slot clock) | DMA timing | M4 |
| DMA fill and copy destination slots (address tagged `0x1000_0000`/`0x2000_0000`) | DMA verification | M4 |
| HINT pending events (V counter, reload value, line-boundary clock) | raster effects | M4 |
| YM2612 register commits and port writes, tagged by source CPU | sound driver capture | M5 |
| PSG writes | sound driver capture | M5 |
| Separate YM2612 and PSG PCM streams (source-rate and resampled) | audio comparison | M5 |
| Clock epoch: absolute master clock, no rebasing visible to consumers | long captures | MesenCE master clock is a 64-bit monotonic counter (`IConsole::GetMasterClock`) |

## 2. Decomp execution hooks (NMDH v1 equivalent)

| Capability bit | Content | MesenCE mapping |
|---|---|---|
| `M68K_INSTRUCTION`, `Z80_INSTRUCTION` | one event per executed instruction with PC and opcode bytes (up to 16 fetch bytes) | debugger `ProcessInstruction` hook |
| `M68K_STATE` | D0-7, A0-7, other SP, CCR flags, SR status, prefetch, STOP, interrupt priority/pending/acknowledge | `M68kRegisters` plus `MdM68k` interrupt state |
| `Z80_STATE` | main/alternate registers, IX/IY/SP/PC, I/R, WZ, IFF1/2, IM, BUSREQ/BUSACK, reset | `SmsCpuState`-style Z80 state plus MD bus arbitration state |
| `FLOW_EDGE` | previous PC, current PC, transfer kind (branch/call/return/exception), target known | debugger call-stack and branch tracking |
| `RESET_EXCEPTION_INTERRUPT` | vector, reason, phase, stack pointer | M68k exception entry |
| `FETCH_PROVENANCE` | every access tagged fetch / operand / data | `M68kAccess` (program vs data) |
| `BUS_INTERVALS` | start and completion clock of each access (stalls included) | requires per-access start/end clocks from `MdMemoryManager` |
| `MASTER_CLOCK` | absolute master clock on every event | `MdConsole` master clock |
| `SYNC_CHECKPOINT` | snapshot of both CPUs, work RAM, Z80 RAM, VRAM, CRAM, VSRAM, VDP registers at a synchronised point | save state (`Serialize`) plus memory dumps |
| `CONTROLLED_STOP` | stop at PC, return sentinel, interrupt, or instruction budget | debugger breakpoints and step counts |
| `REPLACE_HANDOFF` | run native code instead of a routine: make that CPU's bus accesses at given clocks and resume elsewhere, with refresh counter, next sync, interrupt and run target restored | not yet: needs a substitution API on `MdConsole` (M6 scope) |

## 3. Capture artifacts and identity

nativekit binds every capture to the reference core's SHA-256, the patch set, the ROM hash,
the input script and the frame count, and refuses stale toolchains. `mesen-headless/1`
records the emulator commit and dirty flag, ROM/input/Lua SHA-256 and every artifact's
SHA-256 (see [`../headless.md`](../headless.md)).

## 4. Synthetic cases

The 60 cartridges in `tests/MD/synthetic` (built by `gen.py`, byte-identical to nativekit's
vasm builds) and the recorded BlastEm reference (`tests/MD/synthetic/reference/`) cover the
bus map, Z80 bank window and bus request, PSG and YM2612 ports, timers, DAC and algorithms,
controller TH protocol, TMSS, VDP FIFO, DMA (memory, fill, copy, active-display rates),
rendering (planes, window, scroll, H40, V30, sprites, priorities, shadow/highlight,
interlace) and sprite status flags. Six cases are recorded in nativekit as reviewed
candidate-only differences (`consoles/md/tests/rom/KNOWN`); MesenCE must reach its own
source-backed verdict on each.
