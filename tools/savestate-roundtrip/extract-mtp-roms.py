#!/usr/bin/env python3
"""Extract the ROM embedded in each .mtp test (a zip with a TestRom.<ext> entry).

Usage: extract-mtp-roms.py <out dir> <tests dir> [<tests dir> ...]

Writes <out dir>/<top-level folder>/<test path with / replaced by _>.<ext>, so the
save-state round trip (tools/savestate-roundtrip) can run on every ROM that
MesenTests and the fork's tests/ already use. Identical ROMs are written once.
"""
import hashlib
import os
import sys
import zipfile


def main():
    out = sys.argv[1]
    seen = set()
    written = 0
    for root in sys.argv[2:]:
        for dirpath, _, files in os.walk(root):
            for f in sorted(files):
                if not f.endswith(".mtp"):
                    continue
                path = os.path.join(dirpath, f)
                rel = os.path.relpath(path, root)
                system = rel.split(os.sep)[0]
                try:
                    with zipfile.ZipFile(path) as z:
                        roms = [n for n in z.namelist() if n.startswith("TestRom")]
                        if not roms:
                            continue
                        data = z.read(roms[0])
                except zipfile.BadZipFile:
                    continue
                digest = hashlib.sha1(data).hexdigest()
                if digest in seen:
                    continue
                seen.add(digest)
                ext = os.path.splitext(roms[0])[1].lower()
                name = os.path.splitext(rel)[0].replace(os.sep, "_") + ext
                dest = os.path.join(out, system, name)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as fh:
                    fh.write(data)
                written += 1
    print(f"extracted {written} unique ROMs into {out}")


if __name__ == "__main__":
    main()
