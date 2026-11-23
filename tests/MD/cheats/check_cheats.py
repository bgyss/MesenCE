#!/usr/bin/env python3
"""Mega Drive cheat codes (Game Genie, Pro Action Replay) on the synthetic bus_map cartridge.

  check_cheats.py ROM_DIR [--runner bin/mesen-headless]

bus_map reads the word at $004000 (the ROM data fixture, $0123). Checks:
  - Action Replay ROM patch "004000:BEEF": the 68000 reads $BEEF there (bus trace);
  - Game Genie ROM patch for $004000 = $CAFE: the 68000 reads $CAFE. The code is produced by
    encode_game_genie() below, written from the documented bit layout string, independently
    of the C++ decoder (CheatManager::ConvertFromMdGameGenie);
  - Action Replay RAM code "FF0100:1234": work RAM holds $12 $34 at $0100 at the end of the run;
  - no cheat: the original value is read (control);
  - invalid codes are rejected (the Lua call fails, the run stops with code 65).
"""
import argparse, json, pathlib, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ALPHABET = "ABCDEFGHJKLMNPRSTVWXYZ0123456789"
#Bit names of the 8 characters (5 bits each, MSB first): address = ABCDEFGH IJKLMNOP QRSTUVWX,
#value = abcdefgh ijklmnop (Charles MacDonald, Genesis Game Genie code format)
LAYOUT = "ijklm nopIJ KLMNO PABCD EFGHd efgha bcQRS TUVWX"


def encode_game_genie(address, value):
    bits = {}
    for i, name in enumerate("ABCDEFGHIJKLMNOPQRSTUVWX"):
        bits[name] = (address >> (23 - i)) & 1
    for i, name in enumerate("abcdefghijklmnop"):
        bits[name] = (value >> (15 - i)) & 1
    chars = []
    for group in LAYOUT.split():
        n = 0
        for name in group:
            n = (n << 1) | bits[name]
        chars.append(ALPHABET[n])
    return "".join(chars[:4]) + "-" + "".join(chars[4:])


def run(runner, rom, out, lua):
    script = out.parent / (out.name + ".lua")
    script.write_text(lua)
    r = subprocess.run([str(runner), "run", "--rom", str(rom), "--frames", "4", "--out", str(out), "--trace", "bus",
                        "--lua", str(script), "--allow-dirty"], capture_output=True, text=True)
    return r.returncode


def read_at(out, addr):
    for line in (out / "trace.txt").read_text().splitlines():
        if line.startswith("#"):
            continue
        clock, dev, op, width, a, value = line.split()
        if dev == "1" and op == "2" and int(a, 16) == addr and width == "2":
            return int(value, 16)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roms", type=pathlib.Path)
    ap.add_argument("--runner", type=pathlib.Path, default=ROOT / "bin/mesen-headless")
    args = ap.parse_args()
    rom = args.roms / "bus_map.md"
    gg = encode_game_genie(0x004000, 0xCAFE)
    checks = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)

        def case(name, lua):
            out = tmp / name
            return run(args.runner, rom, out, lua), out

        code, out = case("none", "")
        checks.append(("no cheat: ROM word $004000 reads $0123", code == 0 and read_at(out, 0x4000) == 0x0123))

        code, out = case("par_rom", 'emu.addCheat(emu.cheatType.mdProActionReplay, "004000:BEEF")\n')
        checks.append(("Action Replay 004000:BEEF patches the ROM read", code == 0 and read_at(out, 0x4000) == 0xBEEF))

        code, out = case("gg_rom", 'emu.addCheat(emu.cheatType.mdGameGenie, "%s")\n' % gg)
        checks.append(("Game Genie %s ($004000 = $CAFE) patches the ROM read" % gg, code == 0 and read_at(out, 0x4000) == 0xCAFE))

        code, out = case("par_ram", 'emu.addCheat(emu.cheatType.mdProActionReplay, "FF0100:1234")\n')
        ram = (out / "memory" / "MdWorkRam.bin").read_bytes() if code == 0 else b""
        checks.append(("Action Replay FF0100:1234 writes work RAM every frame", ram[0x100:0x102] == b"\x12\x34"))

        for i, bad in enumerate(("ABCD-EFGI", "IIII-IIII", "123")):
            code, out = case("bad_gg_%d" % i, 'emu.addCheat(emu.cheatType.mdGameGenie, "%s")\n' % bad)
            checks.append(("invalid Game Genie code %r rejected" % bad, code == 65))
        code, out = case("bad_par", 'emu.addCheat(emu.cheatType.mdProActionReplay, "A10000:1234")\n')
        checks.append(("Action Replay code on the I/O area rejected", code == 65))

    bad = 0
    for name, ok in checks:
        print("%-4s %s" % ("pass" if ok else "FAIL", name))
        bad += not ok
    print("\n%d of %d cheat checks pass" % (len(checks) - bad, len(checks)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
