// SH-2 disassembler (Hitachi mnemonics). Operand order follows the Hitachi programming manual.
#include "MD/Sh2/Sh2Core.h"
#include <cstdio>

namespace
{
	struct Pattern
	{
		uint16_t mask;
		uint16_t value;
		const char* format;
	};

	// Format codes (after %): n = Rn, m = Rm, i = #imm (signed 8), u = #imm (unsigned 8), 4 = disp4,
	// 8 = disp8, B = 8-bit branch target, C = 12-bit branch target, W/L = PC-relative word/long
	// address, A = MOVA address
	constexpr Pattern Patterns[] = {
		{ 0xFFFF, 0x0008, "CLRT" },
		{ 0xFFFF, 0x0009, "NOP" },
		{ 0xFFFF, 0x000B, "RTS" },
		{ 0xFFFF, 0x0018, "SETT" },
		{ 0xFFFF, 0x0019, "DIV0U" },
		{ 0xFFFF, 0x001B, "SLEEP" },
		{ 0xFFFF, 0x0028, "CLRMAC" },
		{ 0xFFFF, 0x002B, "RTE" },
		{ 0xF0FF, 0x0002, "STC SR,%n" },
		{ 0xF0FF, 0x0012, "STC GBR,%n" },
		{ 0xF0FF, 0x0022, "STC VBR,%n" },
		{ 0xF0FF, 0x000A, "STS MACH,%n" },
		{ 0xF0FF, 0x001A, "STS MACL,%n" },
		{ 0xF0FF, 0x002A, "STS PR,%n" },
		{ 0xF0FF, 0x0003, "BSRF %n" },
		{ 0xF0FF, 0x0023, "BRAF %n" },
		{ 0xF0FF, 0x0029, "MOVT %n" },
		{ 0xF00F, 0x0004, "MOV.B %m,@(R0,%n)" },
		{ 0xF00F, 0x0005, "MOV.W %m,@(R0,%n)" },
		{ 0xF00F, 0x0006, "MOV.L %m,@(R0,%n)" },
		{ 0xF00F, 0x0007, "MUL.L %m,%n" },
		{ 0xF00F, 0x000C, "MOV.B @(R0,%m),%n" },
		{ 0xF00F, 0x000D, "MOV.W @(R0,%m),%n" },
		{ 0xF00F, 0x000E, "MOV.L @(R0,%m),%n" },
		{ 0xF00F, 0x000F, "MAC.L @%m+,@%n+" },
		{ 0xF000, 0x1000, "MOV.L %m,@(%4,%n)" },
		{ 0xF00F, 0x2000, "MOV.B %m,@%n" },
		{ 0xF00F, 0x2001, "MOV.W %m,@%n" },
		{ 0xF00F, 0x2002, "MOV.L %m,@%n" },
		{ 0xF00F, 0x2004, "MOV.B %m,@-%n" },
		{ 0xF00F, 0x2005, "MOV.W %m,@-%n" },
		{ 0xF00F, 0x2006, "MOV.L %m,@-%n" },
		{ 0xF00F, 0x2007, "DIV0S %m,%n" },
		{ 0xF00F, 0x2008, "TST %m,%n" },
		{ 0xF00F, 0x2009, "AND %m,%n" },
		{ 0xF00F, 0x200A, "XOR %m,%n" },
		{ 0xF00F, 0x200B, "OR %m,%n" },
		{ 0xF00F, 0x200C, "CMP/STR %m,%n" },
		{ 0xF00F, 0x200D, "XTRCT %m,%n" },
		{ 0xF00F, 0x200E, "MULU.W %m,%n" },
		{ 0xF00F, 0x200F, "MULS.W %m,%n" },
		{ 0xF00F, 0x3000, "CMP/EQ %m,%n" },
		{ 0xF00F, 0x3002, "CMP/HS %m,%n" },
		{ 0xF00F, 0x3003, "CMP/GE %m,%n" },
		{ 0xF00F, 0x3004, "DIV1 %m,%n" },
		{ 0xF00F, 0x3005, "DMULU.L %m,%n" },
		{ 0xF00F, 0x3006, "CMP/HI %m,%n" },
		{ 0xF00F, 0x3007, "CMP/GT %m,%n" },
		{ 0xF00F, 0x3008, "SUB %m,%n" },
		{ 0xF00F, 0x300A, "SUBC %m,%n" },
		{ 0xF00F, 0x300B, "SUBV %m,%n" },
		{ 0xF00F, 0x300C, "ADD %m,%n" },
		{ 0xF00F, 0x300D, "DMULS.L %m,%n" },
		{ 0xF00F, 0x300E, "ADDC %m,%n" },
		{ 0xF00F, 0x300F, "ADDV %m,%n" },
		{ 0xF0FF, 0x4000, "SHLL %n" },
		{ 0xF0FF, 0x4001, "SHLR %n" },
		{ 0xF0FF, 0x4002, "STS.L MACH,@-%n" },
		{ 0xF0FF, 0x4003, "STC.L SR,@-%n" },
		{ 0xF0FF, 0x4004, "ROTL %n" },
		{ 0xF0FF, 0x4005, "ROTR %n" },
		{ 0xF0FF, 0x4006, "LDS.L @%n+,MACH" },
		{ 0xF0FF, 0x4007, "LDC.L @%n+,SR" },
		{ 0xF0FF, 0x4008, "SHLL2 %n" },
		{ 0xF0FF, 0x4009, "SHLR2 %n" },
		{ 0xF0FF, 0x400A, "LDS %n,MACH" },
		{ 0xF0FF, 0x400B, "JSR @%n" },
		{ 0xF0FF, 0x400E, "LDC %n,SR" },
		{ 0xF0FF, 0x4010, "DT %n" },
		{ 0xF0FF, 0x4011, "CMP/PZ %n" },
		{ 0xF0FF, 0x4012, "STS.L MACL,@-%n" },
		{ 0xF0FF, 0x4013, "STC.L GBR,@-%n" },
		{ 0xF0FF, 0x4015, "CMP/PL %n" },
		{ 0xF0FF, 0x4016, "LDS.L @%n+,MACL" },
		{ 0xF0FF, 0x4017, "LDC.L @%n+,GBR" },
		{ 0xF0FF, 0x4018, "SHLL8 %n" },
		{ 0xF0FF, 0x4019, "SHLR8 %n" },
		{ 0xF0FF, 0x401A, "LDS %n,MACL" },
		{ 0xF0FF, 0x401B, "TAS.B @%n" },
		{ 0xF0FF, 0x401E, "LDC %n,GBR" },
		{ 0xF0FF, 0x4020, "SHAL %n" },
		{ 0xF0FF, 0x4021, "SHAR %n" },
		{ 0xF0FF, 0x4022, "STS.L PR,@-%n" },
		{ 0xF0FF, 0x4023, "STC.L VBR,@-%n" },
		{ 0xF0FF, 0x4024, "ROTCL %n" },
		{ 0xF0FF, 0x4025, "ROTCR %n" },
		{ 0xF0FF, 0x4026, "LDS.L @%n+,PR" },
		{ 0xF0FF, 0x4027, "LDC.L @%n+,VBR" },
		{ 0xF0FF, 0x4028, "SHLL16 %n" },
		{ 0xF0FF, 0x4029, "SHLR16 %n" },
		{ 0xF0FF, 0x402A, "LDS %n,PR" },
		{ 0xF0FF, 0x402B, "JMP @%n" },
		{ 0xF0FF, 0x402E, "LDC %n,VBR" },
		{ 0xF00F, 0x400F, "MAC.W @%m+,@%n+" },
		{ 0xF000, 0x5000, "MOV.L @(%4,%m),%n" },
		{ 0xF00F, 0x6000, "MOV.B @%m,%n" },
		{ 0xF00F, 0x6001, "MOV.W @%m,%n" },
		{ 0xF00F, 0x6002, "MOV.L @%m,%n" },
		{ 0xF00F, 0x6003, "MOV %m,%n" },
		{ 0xF00F, 0x6004, "MOV.B @%m+,%n" },
		{ 0xF00F, 0x6005, "MOV.W @%m+,%n" },
		{ 0xF00F, 0x6006, "MOV.L @%m+,%n" },
		{ 0xF00F, 0x6007, "NOT %m,%n" },
		{ 0xF00F, 0x6008, "SWAP.B %m,%n" },
		{ 0xF00F, 0x6009, "SWAP.W %m,%n" },
		{ 0xF00F, 0x600A, "NEGC %m,%n" },
		{ 0xF00F, 0x600B, "NEG %m,%n" },
		{ 0xF00F, 0x600C, "EXTU.B %m,%n" },
		{ 0xF00F, 0x600D, "EXTU.W %m,%n" },
		{ 0xF00F, 0x600E, "EXTS.B %m,%n" },
		{ 0xF00F, 0x600F, "EXTS.W %m,%n" },
		{ 0xF000, 0x7000, "ADD %i,%n" },
		{ 0xFF00, 0x8000, "MOV.B R0,@(%1,%M)" },
		{ 0xFF00, 0x8100, "MOV.W R0,@(%2,%M)" },
		{ 0xFF00, 0x8400, "MOV.B @(%1,%M),R0" },
		{ 0xFF00, 0x8500, "MOV.W @(%2,%M),R0" },
		{ 0xFF00, 0x8800, "CMP/EQ %i,R0" },
		{ 0xFF00, 0x8900, "BT %B" },
		{ 0xFF00, 0x8B00, "BF %B" },
		{ 0xFF00, 0x8D00, "BT/S %B" },
		{ 0xFF00, 0x8F00, "BF/S %B" },
		{ 0xF000, 0x9000, "MOV.W %W,%n" },
		{ 0xF000, 0xA000, "BRA %C" },
		{ 0xF000, 0xB000, "BSR %C" },
		{ 0xFF00, 0xC000, "MOV.B R0,@(%g1)" },
		{ 0xFF00, 0xC100, "MOV.W R0,@(%g2)" },
		{ 0xFF00, 0xC200, "MOV.L R0,@(%g4)" },
		{ 0xFF00, 0xC300, "TRAPA %u" },
		{ 0xFF00, 0xC400, "MOV.B @(%g1),R0" },
		{ 0xFF00, 0xC500, "MOV.W @(%g2),R0" },
		{ 0xFF00, 0xC600, "MOV.L @(%g4),R0" },
		{ 0xFF00, 0xC700, "MOVA %A,R0" },
		{ 0xFF00, 0xC800, "TST %u,R0" },
		{ 0xFF00, 0xC900, "AND %u,R0" },
		{ 0xFF00, 0xCA00, "XOR %u,R0" },
		{ 0xFF00, 0xCB00, "OR %u,R0" },
		{ 0xFF00, 0xCC00, "TST.B %u,@(R0,GBR)" },
		{ 0xFF00, 0xCD00, "AND.B %u,@(R0,GBR)" },
		{ 0xFF00, 0xCE00, "XOR.B %u,@(R0,GBR)" },
		{ 0xFF00, 0xCF00, "OR.B %u,@(R0,GBR)" },
		{ 0xF000, 0xD000, "MOV.L %L,%n" },
		{ 0xF000, 0xE000, "MOV %i,%n" },
	};
}

std::string Sh2Core::Disassemble(uint32_t pc, uint16_t op)
{
	for(const Pattern& p : Patterns) {
		if((op & p.mask) != p.value) {
			continue;
		}
		std::string out;
		char buf[32];
		uint32_t n = (op >> 8) & 15;
		uint32_t m = (op >> 4) & 15;
		for(const char* f = p.format; *f; f++) {
			if(*f != '%') {
				out += *f;
				continue;
			}
			f++;
			switch(*f) {
				case 'n':
					snprintf(buf, sizeof(buf), "R%u", n);
					out += buf;
					break;
				case 'm':
					snprintf(buf, sizeof(buf), "R%u", m);
					out += buf;
					break;
				case 'M':
					snprintf(buf, sizeof(buf), "R%u", m);
					out += buf;
					break;
				case 'i':
					snprintf(buf, sizeof(buf), "#%d", (int8_t)op);
					out += buf;
					break;
				case 'u':
					snprintf(buf, sizeof(buf), "#$%02X", op & 0xFF);
					out += buf;
					break;
				case '1':
				case '2':
				case '4': {
					//disp4 scaled by the size digit: "@(4,n)" style
					snprintf(buf, sizeof(buf), "%u", (op & 15) * (uint32_t)(*f - '0'));
					out += buf;
					break;
				}
				case 'g': {
					f++;
					snprintf(buf, sizeof(buf), "%u,GBR", (op & 0xFF) * (uint32_t)(*f - '0'));
					out += buf;
					break;
				}
				case 'B':
					snprintf(buf, sizeof(buf), "$%08X", pc + 4 + (int8_t)op * 2);
					out += buf;
					break;
				case 'C':
					snprintf(buf, sizeof(buf), "$%08X", pc + 4 + (uint32_t)((int32_t)((uint32_t)op << 20) >> 19));
					out += buf;
					break;
				case 'W':
					snprintf(buf, sizeof(buf), "@($%08X)", pc + 4 + (op & 0xFF) * 2);
					out += buf;
					break;
				case 'L':
					snprintf(buf, sizeof(buf), "@($%08X)", ((pc + 4) & ~3) + (op & 0xFF) * 4);
					out += buf;
					break;
				case 'A':
					snprintf(buf, sizeof(buf), "$%08X", ((pc + 4) & ~3) + (op & 0xFF) * 4);
					out += buf;
					break;
				default: out += *f; break;
			}
		}
		return out;
	}
	char buf[16];
	snprintf(buf, sizeof(buf), ".WORD $%04X", op);
	return buf;
}
