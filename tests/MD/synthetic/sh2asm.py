#!/usr/bin/env python3
"""A small two-pass SH-2 assembler (Hitachi syntax) for MesenCE's 32X test programs and the
replacement boot ROMs (tools/m32x-boot). Copyright notice and license: GPL-3.0.

Syntax: one instruction per line, "label:" definitions, ";" comments, registers r0-r15
(sp = r15), Hitachi operand forms (@rn, @rn+, @-rn, @(disp,rn), @(r0,rn), @(disp,gbr),
@(r0,gbr), @(disp,pc)). PC-relative loads take a label: "mov.l label,rn", "mov.w label,rn",
"mova label,r0". Directives: .align N, .long V, .word V, .byte V, .space N, .ascii "text".
Values are integers (0x.., $..) or labels, with + and - .

    from sh2asm import assemble
    code = assemble(source, origin=0x06000000)
"""
import re
import struct

REG = {("r%d" % i): i for i in range(16)}
REG["sp"] = 15
CTRL = ("sr", "gbr", "vbr")
SYS = ("mach", "macl", "pr")


class AsmError(Exception):
    pass


def _num(text, labels, pass2):
    text = text.strip()
    total, sign, pos = 0, 1, 0
    for tok in re.findall(r"[+-]|[^+-]+", text):
        tok = tok.strip()
        if tok == "+":
            sign = 1
            continue
        if tok == "-":
            sign = -1
            continue
        if re.fullmatch(r"0x[0-9a-fA-F]+", tok):
            v = int(tok, 16)
        elif re.fullmatch(r"\$[0-9a-fA-F]+", tok):
            v = int(tok[1:], 16)
        elif re.fullmatch(r"[0-9]+", tok):
            v = int(tok)
        elif re.fullmatch(r"'(.)'", tok):
            v = ord(tok[1])
        elif tok in labels:
            v = labels[tok]
        elif not pass2:
            v = 0
        else:
            raise AsmError("unknown symbol " + tok)
        total += sign * v
        sign = 1
        pos += 1
    return total


def _split_operands(text):
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def _operand(op, labels, pass2):
    """(kind, value...) for an operand."""
    o = op.lower().replace(" ", "")
    if o in REG:
        return ("R", REG[o])
    if o in CTRL or o in SYS:
        return (o.upper(),)
    if o.startswith("#"):
        return ("I", _num(op.strip()[1:], labels, pass2))
    m = re.fullmatch(r"@(r\d+|sp)\+", o)
    if m:
        return ("@R+", REG[m.group(1)])
    m = re.fullmatch(r"@-(r\d+|sp)", o)
    if m:
        return ("@-R", REG[m.group(1)])
    m = re.fullmatch(r"@(r\d+|sp)", o)
    if m:
        return ("@R", REG[m.group(1)])
    m = re.fullmatch(r"@\(r0,(r\d+|sp|gbr)\)", o)
    if m:
        return ("@(R0,GBR)",) if m.group(1) == "gbr" else ("@(R0,R)", REG[m.group(1)])
    m = re.fullmatch(r"@\((.+),(r\d+|sp|gbr|pc)\)", o)
    if m:
        inner = op.strip()[2:op.strip().rindex(",")]
        d = _num(inner, labels, pass2)
        if m.group(2) == "gbr":
            return ("@(D,GBR)", d)
        if m.group(2) == "pc":
            return ("L", d)
        return ("@(D,R)", d, REG[m.group(2)])
    return ("L", _num(op, labels, pass2))


def _encode(mn, ops, pc, labels, pass2):
    k = tuple(o[0] for o in ops)
    size = {".b": 0, ".w": 1, ".l": 2}.get(mn[-2:], None) if "." in mn else None
    base = mn.split(".")[0] if mn.startswith("mov") and mn not in ("mova", "movt") else mn

    def disp(value, scale, bits, signed=False):
        if value % scale:
            raise AsmError("%s: misaligned displacement %d" % (mn, value))
        v = value // scale
        lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
        if pass2 and not lo <= v <= hi:
            raise AsmError("%s: displacement %d out of range" % (mn, value))
        return v & ((1 << bits) - 1)

    def br(target, bits):
        return disp(target - (pc + 4), 2, bits, True)

    rr = {  # Rm,Rn two-register forms: base opcode
        "add": 0x300C, "addc": 0x300E, "addv": 0x300F, "and": 0x2009, "cmp/eq": 0x3000, "cmp/hs": 0x3002,
        "cmp/ge": 0x3003, "cmp/hi": 0x3006, "cmp/gt": 0x3007, "cmp/str": 0x200C, "div1": 0x3004, "div0s": 0x2007,
        "dmuls.l": 0x300D, "dmulu.l": 0x3005, "exts.b": 0x600E, "exts.w": 0x600F, "extu.b": 0x600C, "extu.w": 0x600D,
        "mul.l": 0x0007, "muls.w": 0x200F, "mulu.w": 0x200E, "muls": 0x200F, "mulu": 0x200E, "neg": 0x600B, "negc": 0x600A,
        "not": 0x6007, "or": 0x200B, "sub": 0x3008, "subc": 0x300A, "subv": 0x300B, "swap.b": 0x6008,
        "swap.w": 0x6009, "tst": 0x2008, "xor": 0x200A, "xtrct": 0x200D,
    }
    rn = {  # single Rn forms
        "dt": 0x4010, "cmp/pz": 0x4011, "cmp/pl": 0x4015, "movt": 0x0029, "rotl": 0x4004, "rotr": 0x4005,
        "rotcl": 0x4024, "rotcr": 0x4025, "shal": 0x4020, "shar": 0x4021, "shll": 0x4000, "shlr": 0x4001,
        "shll2": 0x4008, "shlr2": 0x4009, "shll8": 0x4018, "shlr8": 0x4019, "shll16": 0x4028, "shlr16": 0x4029,
    }
    none = {"nop": 0x0009, "rts": 0x000B, "rte": 0x002B, "clrt": 0x0008, "sett": 0x0018, "clrmac": 0x0028,
            "div0u": 0x0019, "sleep": 0x001B}
    ctl = {"SR": 0x00, "GBR": 0x10, "VBR": 0x20, "MACH": 0x00, "MACL": 0x10, "PR": 0x20}

    if mn in none and not ops:
        return none[mn]
    if mn in rr and k == ("R", "R"):
        return rr[mn] | ops[1][1] << 8 | ops[0][1] << 4
    if mn in rn and k == ("R",):
        return rn[mn] | ops[0][1] << 8
    if mn == "add" and k == ("I", "R"):
        return 0x7000 | ops[1][1] << 8 | (ops[0][1] & 0xFF)
    if mn == "cmp/eq" and k == ("I", "R") and ops[1][1] == 0:
        return 0x8800 | (ops[0][1] & 0xFF)
    if mn in ("and", "or", "xor", "tst") and k == ("I", "R") and ops[1][1] == 0:
        return {"tst": 0xC800, "and": 0xC900, "xor": 0xCA00, "or": 0xCB00}[mn] | (ops[0][1] & 0xFF)
    if mn in ("and.b", "or.b", "xor.b", "tst.b") and k == ("I", "@(R0,GBR)"):
        return {"tst.b": 0xCC00, "and.b": 0xCD00, "xor.b": 0xCE00, "or.b": 0xCF00}[mn] | (ops[0][1] & 0xFF)
    if mn == "tas.b" and k == ("@R",):
        return 0x401B | ops[0][1] << 8
    if mn in ("bra", "bsr") and k == ("L",):
        return (0xA000 if mn == "bra" else 0xB000) | br(ops[0][1], 12)
    if mn in ("bt", "bf", "bt/s", "bf/s") and k == ("L",):
        return {"bt": 0x8900, "bf": 0x8B00, "bt/s": 0x8D00, "bf/s": 0x8F00}[mn] | br(ops[0][1], 8)
    if mn in ("jmp", "jsr") and k == ("@R",):
        return (0x402B if mn == "jmp" else 0x400B) | ops[0][1] << 8
    if mn in ("braf", "bsrf") and k == ("R",):
        return (0x0023 if mn == "braf" else 0x0003) | ops[0][1] << 8
    if mn == "trapa" and k == ("I",):
        return 0xC300 | (ops[0][1] & 0xFF)
    if mn in ("ldc", "lds") and k[0] == "R" and len(k) == 2 and k[1] in ctl:
        return (0x400E if mn == "ldc" else 0x400A) | ops[0][1] << 8 | ctl[k[1]]
    if mn in ("ldc.l", "lds.l") and k[0] == "@R+" and k[1] in ctl:
        return (0x4007 if mn == "ldc.l" else 0x4006) | ops[0][1] << 8 | ctl[k[1]]
    if mn in ("stc", "sts") and k[1] == "R" and k[0] in ctl:
        return (0x0002 if mn == "stc" else 0x000A) | ops[1][1] << 8 | ctl[k[0]]
    if mn in ("stc.l", "sts.l") and k[1] == "@-R" and k[0] in ctl:
        return (0x4003 if mn == "stc.l" else 0x4002) | ops[1][1] << 8 | ctl[k[0]]
    if mn == "mova" and k == ("L", "R"):
        return 0xC700 | disp(ops[0][1] - ((pc + 4) & ~3), 4, 8)
    if mn in ("mac.w", "mac.l") and k == ("@R+", "@R+"):
        return (0x400F if mn == "mac.w" else 0x000F) | ops[1][1] << 8 | ops[0][1] << 4
    if base == "mov":
        if size is None:
            if k == ("R", "R"):
                return 0x6003 | ops[1][1] << 8 | ops[0][1] << 4
            if k == ("I", "R"):
                return 0xE000 | ops[1][1] << 8 | (ops[0][1] & 0xFF)
        else:
            sc = 1 << size
            if k == ("R", "@R"):
                return 0x2000 | size | ops[1][1] << 8 | ops[0][1] << 4
            if k == ("R", "@-R"):
                return 0x2004 | size | ops[1][1] << 8 | ops[0][1] << 4
            if k == ("R", "@(R0,R)"):
                return 0x0004 | size | ops[1][1] << 8 | ops[0][1] << 4
            if k == ("@R", "R"):
                return 0x6000 | size | ops[1][1] << 8 | ops[0][1] << 4
            if k == ("@R+", "R"):
                return 0x6004 | size | ops[1][1] << 8 | ops[0][1] << 4
            if k == ("@(R0,R)", "R"):
                return 0x000C | size | ops[1][1] << 8 | ops[0][1] << 4
            if k == ("R", "@(D,GBR)") and ops[0][1] == 0:
                return 0xC000 | size << 8 | disp(ops[1][1], sc, 8)
            if k == ("@(D,GBR)", "R") and ops[1][1] == 0:
                return 0xC400 | size << 8 | disp(ops[0][1], sc, 8)
            if k == ("R", "@(D,R)"):
                if size == 2:
                    return 0x1000 | ops[1][2] << 8 | ops[0][1] << 4 | disp(ops[1][1], 4, 4)
                if ops[0][1] == 0:
                    return (0x8000 if size == 0 else 0x8100) | ops[1][2] << 4 | disp(ops[1][1], sc, 4)
            if k == ("@(D,R)", "R"):
                if size == 2:
                    return 0x5000 | ops[1][1] << 8 | ops[0][2] << 4 | disp(ops[0][1], 4, 4)
                if ops[1][1] == 0:
                    return (0x8400 if size == 0 else 0x8500) | ops[0][2] << 4 | disp(ops[0][1], sc, 4)
            if k == ("L", "R") and size == 1:
                return 0x9000 | ops[1][1] << 8 | disp(ops[0][1] - (pc + 4), 2, 8)
            if k == ("L", "R") and size == 2:
                return 0xD000 | ops[1][1] << 8 | disp(ops[0][1] - ((pc + 4) & ~3), 4, 8)
    raise AsmError("unsupported instruction: %s %s" % (mn, k))


def assemble(source, origin=0):
    lines = []
    for raw in source.splitlines():
        line = raw.split(";")[0].strip()
        while ":" in line and not line.startswith(".") and re.match(r"^[A-Za-z_.][\w.]*:", line):
            name, line = line.split(":", 1)
            lines.append(("label", name.strip()))
            line = line.strip()
        if line:
            lines.append(("item", line))

    labels = {}
    for pass2 in (False, True):
        pc, out = origin, bytearray()
        for kind, text in lines:
            if kind == "label":
                if not pass2:
                    labels[text] = pc
                continue
            mn, _, rest = text.partition(" ")
            mn = mn.lower()
            if mn == ".align":
                n = _num(rest, labels, pass2)
                while (pc - origin) % n:
                    out += b"\x00"
                    pc += 1
                continue
            if mn in (".long", ".word", ".byte"):
                fmt, sz = {".long": (">I", 4), ".word": (">H", 2), ".byte": (">B", 1)}[mn]
                for v in _split_operands(rest):
                    out += struct.pack(fmt, _num(v, labels, pass2) & ((1 << (8 * sz)) - 1))
                    pc += sz
                continue
            if mn == ".space":
                n = _num(rest, labels, pass2)
                out += bytes(n)
                pc += n
                continue
            if mn == ".ascii":
                s = rest.strip()[1:-1].encode()
                out += s
                pc += len(s)
                continue
            ops = [_operand(o, labels, pass2) for o in _split_operands(rest)] if rest.strip() else []
            if pc & 1:
                raise AsmError("instruction at odd address: " + text)
            word = _encode(mn, ops, pc, labels, pass2)
            out += struct.pack(">H", word)
            pc += 2
    return bytes(out), labels


if __name__ == "__main__":
    import sys
    data, _ = assemble(open(sys.argv[1]).read(), int(sys.argv[2], 0) if len(sys.argv) > 2 else 0)
    sys.stdout.buffer.write(data)
