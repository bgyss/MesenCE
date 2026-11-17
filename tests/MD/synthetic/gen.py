#!/usr/bin/env python3
"""Build the synthetic Mega Drive cartridges from cases.py without an assembler.

  gen.py OUT_DIR [--check-vasm VASM_M68K]

For every case it writes OUT_DIR/<name>.md (the cartridge) and OUT_DIR/<name>.json (how to
run it: frames, region, model, pads, input script, capture kinds). The cartridge layout and
the bus-operation interpreter match nativekit's consoles/md/tests/rom/mdtest.py, so the same
ROM bytes come out; the 68000 and Z80 code is produced by the small encoders below.
--check-vasm assembles nativekit's interpreter source with vasm and requires identical bytes
(a one-off cross-check; vasm is not needed otherwise).
"""
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import cases  # noqa: E402
from cases import Operation, READ, WRITE, TAS  # noqa: E402

MAGIC = b"NMDX"
HEADER = struct.Struct(">4sHH")
Z80_HEADER = struct.Struct(">4sHHHH")
RECORD = struct.Struct(">BBHII")
ROM_SIZE = 32768


class M68kAsm:
    """Two-pass encoder for the handful of instructions the interpreter uses."""

    def __init__(self, origin):
        self.origin = origin
        self.items = []  # (label or None, encoder(pc, labels) -> bytes, size)

    def label(self, name):
        self.items.append((name, None, 0))

    def raw(self, *words):
        data = b"".join(struct.pack(">H", w & 0xFFFF) for w in words)
        self.items.append((None, lambda pc, labels, d=data: d, len(data)))

    def branch(self, cond, target):
        # Bcc.s: 0110 cccc dddddddd, displacement from PC+2.
        def enc(pc, labels):
            disp = labels[target] - (pc + 2)
            if not -128 <= disp <= 127 or disp == 0:
                raise ValueError("short branch out of range")
            return struct.pack(">BB", 0x60 | cond, disp & 0xFF)
        self.items.append((None, enc, 2))

    def lea_pc(self, target, areg):
        # LEA (d16,PC),An: 0100 aaa1 1111 1010, displacement from the extension word.
        def enc(pc, labels):
            return struct.pack(">Hh", 0x41FA | (areg << 9), labels[target] - (pc + 2))
        self.items.append((None, enc, 4))

    def assemble(self):
        labels, pc = {}, self.origin
        for name, _, size in self.items:
            if name:
                labels[name] = pc
            pc += size
        out, pc = bytearray(), self.origin
        for name, enc, size in self.items:
            if enc:
                chunk = enc(pc, labels)
                assert len(chunk) == size
                out += chunk
            pc += size
        return bytes(out)


BEQ, BNE, BRA = 7, 6, 0


def interpreter(header_size):
    a = M68kAsm(0x200)
    a.lea_pc("steps", 0)                 # lea steps(pc),a0
    if 1 <= header_size <= 8:
        a.raw(0x5048 | ((header_size & 7) << 9))  # addq.w #header,a0 (what vasm emits)
    else:
        a.raw(0x41E8, header_size)       # lea header(a0),a0 (what vasm emits for adda.w)
    a.label("next")
    a.raw(0x7E00)                        # moveq #0,d7
    a.raw(0x1E18)                        # move.b (a0)+,d7
    a.branch(BEQ, "done")
    a.raw(0x7C00)                        # moveq #0,d6
    a.raw(0x1C18)                        # move.b (a0)+,d6
    a.raw(0x3A18)                        # move.w (a0)+,d5
    a.raw(0x2258)                        # movea.l (a0)+,a1
    a.raw(0x2018)                        # move.l (a0)+,d0
    for code, target in ((1, "read_op"), (2, "write_op"), (3, "tas_op"), (4, "poll_op")):
        a.raw(0x0C07, code)              # cmpi.b #code,d7
        a.branch(BEQ, target)
    a.branch(BRA, "done")
    a.label("read_op")
    a.raw(0x0C06, 1)
    a.branch(BEQ, "read8")
    a.raw(0x0C06, 2)
    a.branch(BEQ, "read16")
    a.raw(0x2211)                        # move.l (a1),d1
    a.branch(BRA, "next")
    a.label("read8")
    a.raw(0x1211)                        # move.b (a1),d1
    a.branch(BRA, "next")
    a.label("read16")
    a.raw(0x3211)                        # move.w (a1),d1
    a.branch(BRA, "next")
    a.label("write_op")
    a.raw(0x0C06, 1)
    a.branch(BEQ, "write8")
    a.raw(0x0C06, 2)
    a.branch(BEQ, "write16")
    a.raw(0x2280)                        # move.l d0,(a1)
    a.branch(BRA, "next")
    a.label("write8")
    a.raw(0x1280)                        # move.b d0,(a1)
    a.branch(BRA, "next")
    a.label("write16")
    a.raw(0x3280)                        # move.w d0,(a1)
    a.branch(BRA, "next")
    a.label("tas_op")
    a.raw(0x4AD1)                        # tas (a1)
    a.branch(BRA, "next")
    a.label("poll_op")
    a.raw(0x0C06, 1)
    a.branch(BEQ, "poll8")
    a.label("poll_read")
    a.raw(0x3211)                        # move.w (a1),d1
    a.raw(0xC245)                        # and.w d5,d1
    a.raw(0xB240)                        # cmp.w d0,d1
    a.branch(BNE, "poll_read")
    a.branch(BRA, "next")
    a.label("poll8")
    a.raw(0x1211)                        # move.b (a1),d1
    a.raw(0xC205)                        # and.b d5,d1
    a.raw(0xB200)                        # cmp.b d0,d1
    a.branch(BNE, "poll8")
    a.branch(BRA, "next")
    a.label("done")
    a.branch(BRA, "done")
    a.label("steps")
    return a.assemble()


def direct_program(operations):
    out = bytearray()
    for op in operations:
        size = {1: 0x1000, 2: 0x3000, 4: 0x2000}.get(op.width)
        if op.code == WRITE:
            # move.<size> #imm,(xxx).l
            out += struct.pack(">H", size | 0x03FC)
            out += struct.pack(">I", op.value & 0xFFFFFFFF) if op.width == 4 else struct.pack(">H", op.value & (0xFF if op.width == 1 else 0xFFFF))
            out += struct.pack(">I", op.addr & 0xFFFFFFFF)
        elif op.code == READ:
            # move.<size> (xxx).l,d0
            out += struct.pack(">HI", size | 0x0039, op.addr & 0xFFFFFFFF)
        elif op.code == TAS and op.width == 1:
            out += struct.pack(">HI", 0x4AF9, op.addr & 0xFFFFFFFF)
        else:
            raise RuntimeError("direct case has an unsupported step")
    out += b"\x60\xFE"  # bra.s *
    return bytes(out)


def z80_assemble(lines):
    """The Z80 subset used by the fixtures: ld a,n / ld (nn),a / ld a,(nn) / xor a / jr label."""
    out, labels, fixups = bytearray(), {}, []
    for line in lines:
        text = line.strip()
        if ":" in text:
            name, text = text.split(":", 1)
            labels[name.strip()] = len(out)
            text = text.strip()
        if not text:
            continue
        mnemonic, _, args = text.partition(" ")
        args = args.replace(" ", "")
        num = lambda s: int(s[1:], 16) if s.startswith("$") else int(s, 0)
        if mnemonic == "ld" and args.startswith("a,("):
            out += bytes((0x3A,)) + struct.pack("<H", num(args[3:-1]))
        elif mnemonic == "ld" and args.startswith("a,"):
            out += bytes((0x3E, num(args[2:]) & 0xFF))
        elif mnemonic == "ld" and args.startswith("(") and args.endswith("),a"):
            out += bytes((0x32,)) + struct.pack("<H", num(args[1:-3]))
        elif mnemonic == "xor" and args == "a":
            out += b"\xAF"
        elif mnemonic == "jr":
            fixups.append((len(out), args))
            out += b"\x18\x00"
        else:
            raise RuntimeError("unsupported Z80 line: " + line)
    for pos, target in fixups:
        out[pos + 1] = (labels[target] - (pos + 2)) & 0xFF
    return bytes(out)


def encode_steps(case, operations):
    if case.z80_fixture_id:
        data = bytearray(Z80_HEADER.pack(MAGIC, 2, len(operations), case.z80_fixture_id, case.z80_entry))
    else:
        data = bytearray(HEADER.pack(MAGIC, 1, len(operations)))
    for op in operations:
        data += RECORD.pack(op.code, op.width, op.compare_mask & 0xFFFF, op.addr & 0xFFFFFFFF, op.value & 0xFFFFFFFF)
    return bytes(data)


def build(case):
    operations = list(case.operations)
    if case.z80_program:
        code = z80_assemble(case.z80_program)
        copy = [Operation(WRITE, 1, 0xA00000 + i, b) for i, b in enumerate(code)]
        operations[case.z80_load_index:case.z80_load_index] = copy
    if case.direct:
        program = direct_program(operations)
    else:
        header = Z80_HEADER.size if case.z80_fixture_id else HEADER.size
        program = interpreter(header) + encode_steps(case, operations)
    if len(program) + 0x200 > 0x4000:
        raise RuntimeError(case.name + ": program overlaps the ROM data fixture")
    rom = bytearray(case.rom_size or ROM_SIZE)
    rom[0:4] = (0x00FFFF00).to_bytes(4, "big")
    rom[4:8] = (0x00000200).to_bytes(4, "big")
    rom[0x100:0x110] = b"SEGA GENESIS    "
    rom[0x120:0x150] = b"NATIVEKIT BUS TEST".ljust(48, b" ")
    rom[0x150:0x180] = b"NATIVEKIT BUS TEST".ljust(48, b" ")
    rom[0x1F0:0x200] = b"JUE             "
    for offset, value in case.rom_data:
        rom[offset] = value
    rom[0x200:0x200 + len(program)] = program
    rom[0x4000:0x4008] = bytes((0x01, 0x23, 0x04, 0x56, 0x07, 0x89, 0x0A, 0xBC))
    return bytes(rom)


def run_spec(case):
    script = {}
    for frame, (p1, p2) in enumerate(case.input_frames, start=1):
        ports = {}
        if p1:
            ports["1"] = list(p1)
        if p2:
            ports["2"] = list(p2)
        if ports:
            script[str(frame)] = ports
    capture = [k for k, v in (("frames", case.render), ("psg", case.capture_audio),
                              ("ym", case.capture_ym_audio), ("mixed", case.capture_mixed_audio)) if v]
    return {"name": case.name, "frames": case.capture_frames, "region": case.region,
            "model": case.model, "pad1": case.pad1, "pad2": case.pad2,
            "input": {"frames": script}, "capture": capture,
            "weave_fields": case.weave_fields}


def check_vasm(vasm):
    sys.path.insert(0, str(pathlib.Path.home() / "src/nativekit/consoles/md/tests/rom"))
    import mdtest  # nativekit's builder, used only for this cross-check
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "i.s"
        out = pathlib.Path(tmp) / "i.bin"
        for header in (8, 12):
            src.write_text("\n".join(l.replace("#8,a0", f"#{header},a0") for l in mdtest.INTERPRETER) + "\n")
            subprocess.run([vasm, "-Fbin", "-quiet", "-o", str(out), str(src)], check=True, capture_output=True)
            want = out.read_bytes()
            got = interpreter(header)
            if want != got:
                raise SystemExit(f"interpreter mismatch (header {header}):\n vasm {want.hex()}\n  gen {got.hex()}")
    print("interpreter bytes match vasm")


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    if "--check-vasm" in args:
        i = args.index("--check-vasm")
        check_vasm(args[i + 1])
        del args[i:i + 2]
    out = pathlib.Path(args[0])
    out.mkdir(parents=True, exist_ok=True)
    all_cases = cases.all_cases()
    for case in all_cases:
        (out / (case.name + ".md")).write_bytes(build(case))
        (out / (case.name + ".json")).write_text(json.dumps(run_spec(case), indent=1) + "\n")
    print(f"{len(all_cases)} cartridges in {out}")


if __name__ == "__main__":
    main()
