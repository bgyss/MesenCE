# Reference models for the Sega CD S2 cases (mcd_cases.py), written from the Mega-CD Hardware
# Manual's descriptions; where the manual is silent they follow ares (ISC) and say so.
# Copyright notice and license: GPL-3.0.


def font_words(color, bits):
    """$FF8050-$FF8056 for font colour `color` ($FF804C) and font bits `bits` ($FF804E).

    Manual p. 34: one 4-bit colour per font bit, SC1x where the bit is 1 and SC0x where it is 0;
    bit 15 (SBF) is the top nibble of $FF8050 and bit 0 (SB0) the bottom nibble of $FF8056.
    """
    words = []
    for k in range(4):
        word = 0
        for j in range(4):  # nibble j from the top
            bit = (bits >> (15 - 4 * k - j)) & 1
            word |= ((color >> 4) if bit else (color & 0x0F)) << (12 - 4 * j)
        words.append(word)
    return words


def pcm_stream(ram, channels, count):
    """RF5C164 output samples (manual appendix pp. 6-12: ST/LS/FD/ENV/PAN, FD as a 5.11 address
    step per sample, PAN high nibble = R, low = L). Sample format and the $FF loop marker, the
    16-bit clamp and the 10-bit DAC rounding follow ares md/mcd/pcm.cpp."""
    chans = [dict(c, addr=c["start"] << 19) for c in channels]
    out = []
    for _ in range(count):
        left = right = 0
        for ch in chans:
            data = ram[(ch["addr"] >> 11) & 0xFFFF]
            if data == 0xFF:
                ch["addr"] = ch["loop"] << 11
                data = ram[ch["loop"]]
                if data == 0xFF:
                    continue
            else:
                ch["addr"] = (ch["addr"] + ch["step"]) & 0x7FFFFFF
            value = (data & 0x7F) if data & 0x80 else -(data & 0x7F)
            left += (value * ch["env"] * (ch["pan"] & 0x0F)) >> 5
            right += (value * ch["env"] * (ch["pan"] >> 4)) >> 5
        # The top 10 bits at half scale (McdPcm, findings C12; was >> 2 & ~15, ares)
        left = (max(-32768, min(32767, left)) >> 1) & ~31
        right = (max(-32768, min(32767, right)) >> 1) & ~31
        out.append((left, right))
    return out


class GraphicsModel:
    """The graphics chip ("rotation scaling", manual pp. 35-53) on a 256 KiB word RAM image.

    - Trace vector per line: X start, Y start in 13.3 fixed point; dX, dY per dot in signed 5.11.
    - Stamp map entry: bit 15 HFLP, bit 14 RT1, bit 13 RT0, bits 10-0 stamp number (0 = no stamp);
      RT1:RT0 = 0, 90, 180, 270 degrees (p. 36; the figure on p. 42 turns counter-clockwise).
    - Stamps are stored like Mega Drive sprites: 8x8-dot cells arranged vertically, 4 bits a dot.
    - The image buffer is cells arranged vertically, `vcells` + 1 cells high; dots outside the map
      are 0 unless repeat is set.
    The order of HFLP and rotation and the address bit masks follow ares md/mcd/gpu.cpp.
    """

    def __init__(self, wram):
        self.wram = bytearray(wram)

    def nibble(self, n):
        b = self.wram[(n >> 1) & 0x3FFFF]
        return (b >> 4) if (n & 1) == 0 else (b & 0x0F)

    def set_nibble(self, n, v):
        i = (n >> 1) & 0x3FFFF
        if n & 1:
            self.wram[i] = (self.wram[i] & 0xF0) | v
        else:
            self.wram[i] = (self.wram[i] & 0x0F) | (v << 4)

    def word(self, byte_addr):
        return (self.wram[byte_addr & 0x3FFFE] << 8) | self.wram[(byte_addr | 1) & 0x3FFFF]

    def run(self, map_base, image_base, vcells, hdots, vdots, vector_base, stamp32=False, map16=False, repeat=False, offset=0):
        stamp_shift = 5 if stamp32 else 4
        map_shift = (8 if map16 else 4) - (1 if stamp32 else 0)
        index_mask = 0x7FC if stamp32 else 0x7FF
        pixel_mask = 0x1F if stamp32 else 0x0F
        map_mask = 0x7FFFFF if map16 else 0x07FFFF
        image_width = (vcells + 1) << 6
        line_addr = image_base * 2 + offset
        vec = vector_base
        for _ in range(vdots):
            x = (self.word(vec) << 8) & 0xFFFFFF
            y = (self.word(vec + 2) << 8) & 0xFFFFFF
            dx = self.word(vec + 4)
            dy = self.word(vec + 6)
            dx = dx - 0x10000 if dx & 0x8000 else dx
            dy = dy - 0x10000 if dy & 0x8000 else dy
            vec += 8
            addr = line_addr
            for _ in range(hdots):
                if repeat:
                    x &= map_mask
                    y &= map_mask
                out = 0
                if not ((x | y) & ~map_mask & 0xFFFFFF):
                    xd, yd = x >> 11, y >> 11
                    entry = self.word(map_base + 2 * (((yd >> stamp_shift) << map_shift) + (xd >> stamp_shift)))
                    index = entry & index_mask
                    if index:
                        if entry & 0x8000:
                            xd = ~xd
                        if entry & 0x4000:
                            xd, yd = ~xd, ~yd
                        if entry & 0x2000:
                            xd, yd = ~yd, xd
                        xp, yp = xd & pixel_mask, yd & pixel_mask
                        out = self.nibble((index << 8) | ((xp & ~7) << stamp_shift) | (yp << 3) | (xp & 7))
                self.set_nibble(addr, out)
                addr += 1
                if not (addr & 7):
                    addr += image_width - 8
                x = (x + dx) & 0xFFFFFF
                y = (y + dy) & 0xFFFFFF
            line_addr += 8
        return self.wram


def stamp_bytes(pixel):
    """A 16x16 stamp (128 bytes) from pixel(x, y): cells arranged vertically (manual p. 36)."""
    data = bytearray(128)
    for y in range(16):
        for x in range(16):
            offset = (64 if x >= 8 else 0) + y * 4 + (x & 7) // 2
            if x & 1:
                data[offset] |= pixel(x, y)
            else:
                data[offset] |= pixel(x, y) << 4
    return data
