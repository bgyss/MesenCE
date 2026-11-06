#!/usr/bin/env python3
"""Fill in the SNES header checksum and complement of a LoROM-layout ROM (header at $7FC0)."""
import sys

path = sys.argv[1]
rom = bytearray(open(path, "rb").read())
rom[0x7FDC:0x7FE0] = b"\xff\xff\x00\x00"
total = sum(rom) & 0xFFFF
rom[0x7FDC] = (total ^ 0xFFFF) & 0xFF
rom[0x7FDD] = (total ^ 0xFFFF) >> 8
rom[0x7FDE] = total & 0xFF
rom[0x7FDF] = total >> 8
open(path, "wb").write(rom)
