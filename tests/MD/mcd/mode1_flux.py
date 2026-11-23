#!/usr/bin/env python3
"""Sega CD Mode 1 with the BIOS: Flux (Virgin, 1994), a Mega Drive cartridge that plays audio CDs
through the Mega-CD, run with a generated two-track audio CD (30 s of 440 Hz, then 30 s of 660 Hz).

Local only: needs the cartridge (MESEN_FLUX_ROM, a .gen or a .7z holding one; default
~/Documents/goodgen300/Flux for Mega-CD.7z) and the European BIOS (MESEN_MCD_BIOS_DIR, default
~/mame/roms/segacd, file "Mega CD (E) - M1 V1.00.bin"). Skips when either is missing.

Checks, from the CD audio stream (cdda.raw):
  - CD audio starts within 3 s of power-on (Flux asks the BIOS to play track 1);
  - at 18 s it is the 440 Hz track, at 36 s (50 Hz frames) the 660 Hz track;
  - without the BIOS (Mode 1 BIOS window empty) there is no CD audio: the BIOS is what plays.
"""
import array
import os
import pathlib
import shutil
import struct
import math
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
HEADLESS = ROOT / "bin/mesen-headless"


def make_disc(folder):
    data = bytearray()
    starts = []
    for freq in (440, 660):
        starts.append(len(data) // 2352)
        for i in range(44100 * 30):
            v = int(12000 * math.sin(2 * math.pi * freq * i / 44100))
            data += struct.pack("<hh", v, v)
        data += b"\0" * (-len(data) % 2352)
    (folder / "audio.bin").write_bytes(data)
    cue = 'FILE "audio.bin" BINARY\n'
    for i, s in enumerate(starts):
        cue += f"  TRACK {i + 1:02d} AUDIO\n    INDEX 01 {s // 4500:02d}:{s // 75 % 60:02d}:{s % 75:02d}\n"
    (folder / "audio.cue").write_text(cue)
    return folder / "audio.cue"


def find_rom(folder):
    src = pathlib.Path(os.environ.get("MESEN_FLUX_ROM", pathlib.Path.home() / "Documents/goodgen300/Flux for Mega-CD.7z"))
    if not src.exists():
        return None
    if src.suffix.lower() != ".7z":
        return src
    if not shutil.which("7z"):
        return None
    subprocess.run(["7z", "x", "-y", f"-o{folder}", str(src)], capture_output=True, check=True)
    roms = list(folder.glob("*.gen")) + list(folder.glob("*.md"))
    return roms[0] if roms else None


def run(rom, disc, frames, out, bios):
    args = [str(HEADLESS), "run", "--rom", str(rom), "--frames", str(frames), "--out", str(out), "--allow-dirty",
            "--md-mcd", "--md-mcd-disc", str(disc), "--chip-audio"]
    if bios:
        args += ["--md-mcd-bios", str(bios)]
    subprocess.run(args, capture_output=True, check=True)
    return array.array("h", (out / "cdda.raw").read_bytes())[::2]  # left channel


def freq(samples):
    zc = sum(1 for i in range(1, len(samples)) if (samples[i - 1] < 0) != (samples[i] < 0))
    return zc / 2 / (len(samples) / 44100)


def main():
    bios = pathlib.Path(os.environ.get("MESEN_MCD_BIOS_DIR", pathlib.Path.home() / "mame/roms/segacd")) / "Mega CD (E) - M1 V1.00.bin"
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        rom = find_rom(tmp)
        if rom is None or not bios.exists():
            print("skip mode1 flux (cartridge or European BIOS not found)")
            return
        disc = make_disc(tmp)
        failures = []
        left = run(rom, disc, 1800, tmp / "bios", bios)
        start = next((i for i, x in enumerate(left) if abs(x) > 100), None)
        if start is None or start > 3 * 44100:
            failures.append(f"CD audio did not start within 3 s (first sound at sample {start})")
        f18 = freq(left[18 * 44100 - 44100:18 * 44100])
        f36 = freq(left[-44100:])
        if abs(f18 - 440) > 5:
            failures.append(f"track 1 at 18 s: {f18:.1f} Hz, expected 440")
        if abs(f36 - 660) > 5:
            failures.append(f"track 2 at 36 s: {f36:.1f} Hz, expected 660")
        silent = run(rom, disc, 600, tmp / "nobios", None)
        if max(map(abs, silent), default=0) != 0:
            failures.append("CD audio without the BIOS")
        for f in failures:
            print("FAIL", f)
        if failures:
            sys.exit(1)
        print(f"pass mode1 flux (CD audio from {start / 44100:.2f} s, 440 Hz at 18 s, 660 Hz at 36 s, silent without BIOS)")


if __name__ == "__main__":
    main()
