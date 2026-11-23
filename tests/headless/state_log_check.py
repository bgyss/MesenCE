#!/usr/bin/env python3
"""test:headless: a mesen-headless run with --state-log state.log --state-blocks --dump-at 30,60
over 60 frames. Checks one log line and 512 block bytes per frame, and that the work RAM dumped at
frames 30 and 60 hashes to the CRC32 and the block hashes logged for those frames."""
import pathlib
import struct
import sys
import zlib

d = pathlib.Path(sys.argv[1])
lines = (d / "state.log").read_text().splitlines()
cols = lines[1][2:].split()
rows = [l.split() for l in lines[2:]]
assert lines[0] == "# mesen-state-log/1", lines[0]
assert [int(r[0]) for r in rows] == list(range(1, 61)), "one line per frame"
blocks = (d / "state.log.blocks").read_bytes()
assert len(blocks) == 60 * 512, len(blocks)
for frame in (30, 60):
    ram = (d / "dumps" / str(frame) / "MdWorkRam.bin").read_bytes()
    assert int(rows[frame - 1][cols.index("MdWorkRam")], 16) == zlib.crc32(ram), "CRC32 at %d" % frame
    got = struct.unpack_from("<256H", blocks, (frame - 1) * 512)
    assert list(got) == [zlib.crc32(ram[i * 256:(i + 1) * 256]) & 0xFFFF for i in range(256)], "blocks at %d" % frame
