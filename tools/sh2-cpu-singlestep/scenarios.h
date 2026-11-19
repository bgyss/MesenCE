// SH-2 scenarios the sh4 suite can't cover: resets, exceptions (TRAPA, RTE, illegal and slot
// illegal instructions), interrupts (IRL, on-chip, priority, masking rules), SLEEP, MAC.W/MAC.L,
// the DIVU, FRT, DMAC and cache, and the instruction timing table. Expected values are worked out
// from the Hitachi SH7604 hardware manual and SH-1/SH-2 programming manual definitions (see
// docs/research/32x-verification-findings.md, S1); where the core follows ares on a point the
// manuals were not checked for, the scenario says so.
#pragma once
#include "MD/Sh2/Sh2Core.h"
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace Scenarios
{
	//1 MiB of RAM at physical $00000000, mirrored through the 29-bit space
	class RamCpu : public Sh2Core
	{
	public:
		std::vector<uint8_t> ram = std::vector<uint8_t>(1 << 20);
		int busReads = 0;
		int busWrites = 0;
		std::vector<uint8_t> acceptedLevels;

		uint32_t Mask(uint32_t a) { return a & 0xFFFFF; }
		uint8_t BusRead8(uint32_t a) override
		{
			busReads++;
			return ram[Mask(a)];
		}
		uint16_t BusRead16(uint32_t a) override
		{
			busReads++;
			a = Mask(a);
			return (uint16_t)(ram[a] << 8 | ram[a + 1]);
		}
		uint32_t BusRead32(uint32_t a) override
		{
			busReads++;
			a = Mask(a);
			return (uint32_t)ram[a] << 24 | (uint32_t)ram[a + 1] << 16 | (uint32_t)ram[a + 2] << 8 | ram[a + 3];
		}
		void BusWrite8(uint32_t a, uint8_t v) override
		{
			busWrites++;
			ram[Mask(a)] = v;
		}
		void BusWrite16(uint32_t a, uint16_t v) override
		{
			busWrites++;
			a = Mask(a);
			ram[a] = (uint8_t)(v >> 8);
			ram[a + 1] = (uint8_t)v;
		}
		void BusWrite32(uint32_t a, uint32_t v) override
		{
			busWrites++;
			a = Mask(a);
			ram[a] = (uint8_t)(v >> 24);
			ram[a + 1] = (uint8_t)(v >> 16);
			ram[a + 2] = (uint8_t)(v >> 8);
			ram[a + 3] = (uint8_t)v;
		}
		void OnExternalInterruptAccepted(uint8_t level) override { acceptedLevels.push_back(level); }

		uint32_t Long(uint32_t a)
		{
			a = Mask(a);
			return (uint32_t)ram[a] << 24 | (uint32_t)ram[a + 1] << 16 | (uint32_t)ram[a + 2] << 8 | ram[a + 3];
		}
		void SetLong(uint32_t a, uint32_t v)
		{
			a = Mask(a);
			ram[a] = (uint8_t)(v >> 24);
			ram[a + 1] = (uint8_t)(v >> 16);
			ram[a + 2] = (uint8_t)(v >> 8);
			ram[a + 3] = (uint8_t)v;
		}
		void SetWords(uint32_t a, std::initializer_list<uint16_t> words)
		{
			for(uint16_t w : words) {
				ram[Mask(a)] = (uint8_t)(w >> 8);
				ram[Mask(a + 1)] = (uint8_t)w;
				a += 2;
			}
		}
		//Boots through the power-on reset vectors: PC = pc, SP = sp
		void Boot(uint32_t pc, uint32_t sp = 0x8000)
		{
			std::fill(ram.begin(), ram.end(), 0);
			SetLong(0, pc);
			SetLong(4, sp);
			PowerOn();
			Step();
		}
		//On-chip register access through the CPU's own address decoding
		void Io8(uint32_t a, uint8_t v) { WriteByte(a, v); }
		void Io16(uint32_t a, uint16_t v) { WriteWord(a, v); }
		void Io32(uint32_t a, uint32_t v) { WriteLong(a, v); }
		uint32_t RdIo32(uint32_t a) { return ReadLong(a); }
		uint32_t RdIo8(uint32_t a) { return ReadByte(a); }
		uint32_t Rd32(uint32_t a) { return ReadLong(a); }
		uint64_t Cycles() { return GetCycleCount(); }
	};

	struct Runner
	{
		int failures = 0;
		int checks = 0;
		std::string current;

		void Check(bool ok, const char* what, uint64_t got, uint64_t expected)
		{
			checks++;
			if(!ok) {
				failures++;
				printf("FAIL %s: %s = $%llX, expected $%llX\n", current.c_str(), what, (unsigned long long)got, (unsigned long long)expected);
			}
		}
		void Eq(const char* what, uint64_t got, uint64_t expected) { Check(got == expected, what, got, expected); }
	};

	inline int Run()
	{
		Runner r;
		RamCpu cpu;
		auto& s = cpu.GetState();
		auto scenario = [&](const char* name, std::function<void()> body) {
			r.current = name;
			body();
		};
		constexpr uint32_t Code = 0x1000;

		scenario("power-on reset", [&] {
			cpu.Boot(Code, 0x7FF0);
			r.Eq("PC", s.PC, Code);
			r.Eq("R15", s.R[15], 0x7FF0);
			r.Eq("VBR", s.VBR, 0);
			r.Eq("SR.I", (s.SR >> 4) & 15, 15);
		});

		scenario("manual reset uses vectors 2/3", [&] {
			cpu.Boot(Code);
			cpu.SetLong(8, 0x2000);
			cpu.SetLong(12, 0x6000);
			cpu.Reset(true);
			cpu.Step();
			r.Eq("PC", s.PC, 0x2000);
			r.Eq("R15", s.R[15], 0x6000);
		});

		scenario("TRAPA pushes SR and the next PC", [&] {
			cpu.Boot(Code, 0x8000);
			cpu.SetWords(Code, { 0xC320 }); //TRAPA #$20
			cpu.SetLong(0x20 * 4, 0x3000);
			s.SR = 0x301; //M, Q, T
			uint64_t c0 = cpu.Cycles();
			cpu.Step();
			r.Eq("PC", s.PC, 0x3000);
			r.Eq("R15", s.R[15], 0x8000 - 8);
			r.Eq("saved PC", cpu.Long(0x8000 - 8), Code + 2);
			r.Eq("saved SR", cpu.Long(0x8000 - 4), 0x301);
			r.Eq("cycles (8)", cpu.Cycles() - c0, 8);
		});

		scenario("RTE restores PC and SR after its delay slot", [&] {
			cpu.Boot(Code, 0x7FF8);
			cpu.SetLong(0x7FF8, 0x3000);
			cpu.SetLong(0x7FFC, 0x0F1); //I = 15, T = 1
			cpu.SetWords(Code, { 0x002B, 0x7105 }); //RTE; ADD #5,R1
			s.R[1] = 10;
			s.SR = 0;
			uint64_t c0 = cpu.Cycles();
			cpu.Step();
			r.Eq("SR after RTE", s.SR, 0x0F1);
			r.Eq("cycles (4)", cpu.Cycles() - c0, 4);
			cpu.Step();
			r.Eq("slot executed", s.R[1], 15);
			r.Eq("PC", s.PC, 0x3000);
			r.Eq("R15", s.R[15], 0x8000);
		});

		scenario("delayed branch: slot runs, then the target", [&] {
			cpu.Boot(Code);
			cpu.SetWords(Code, { 0xA002, 0x7101 }); //BRA +4 (target Code+8); ADD #1,R1
			s.R[1] = 0;
			uint64_t c0 = cpu.Cycles();
			cpu.Step();
			r.Eq("BRA cycles (2)", cpu.Cycles() - c0, 2);
			r.Eq("PC after BRA (slot)", s.PC, Code + 2);
			cpu.Step();
			r.Eq("R1", s.R[1], 1);
			r.Eq("PC", s.PC, Code + 8);
		});

		scenario("MOVA in a delay slot uses the branch destination + 2", [&] {
			cpu.Boot(Code);
			cpu.SetWords(Code, { 0xA00E, 0xC701 }); //BRA Code+$20; MOVA @(4,PC),R0
			cpu.Step();
			cpu.Step();
			r.Eq("R0", s.R[0], ((Code + 0x20 + 2) & ~3u) + 4);
		});

		scenario("BT/BF timing: 3 taken, 1 not taken; BT/S 2 taken", [&] {
			cpu.Boot(Code);
			cpu.SetWords(Code, { 0x8901, 0x0009, 0x0009, 0x8B01 }); //BT +2; NOP; NOP; BF +2
			s.SR |= 1;
			uint64_t c0 = cpu.Cycles();
			cpu.Step();
			r.Eq("BT taken cycles", cpu.Cycles() - c0, 3);
			r.Eq("BT target", s.PC, Code + 6);
			c0 = cpu.Cycles();
			cpu.Step();
			r.Eq("BF not taken cycles", cpu.Cycles() - c0, 1);
			cpu.SetWords(Code + 8, { 0x8D01, 0x0009 }); //BT/S
			s.PC = Code + 8;
			c0 = cpu.Cycles();
			cpu.Step();
			r.Eq("BT/S taken cycles", cpu.Cycles() - c0, 2);
		});

		scenario("illegal instruction: vector 4, saved PC = the instruction", [&] {
			cpu.Boot(Code, 0x8000);
			cpu.SetWords(Code, { 0xFFFF });
			cpu.SetLong(4 * 4, 0x3000);
			cpu.Step();
			r.Eq("PC", s.PC, 0x3000);
			r.Eq("saved PC", cpu.Long(0x8000 - 8), Code);
		});

		scenario("slot illegal instruction: vector 6", [&] {
			cpu.Boot(Code, 0x8000);
			cpu.SetWords(Code, { 0xA010, 0xA010 }); //BRA; BRA in the slot
			cpu.SetLong(6 * 4, 0x3000);
			cpu.Step();
			cpu.Step();
			r.Eq("PC", s.PC, 0x3000);
			//Saved PC: the jump destination of the delayed branch (SH7604 manual table 4.11)
			r.Eq("saved PC", cpu.Long(0x8000 - 8), Code + 4 + 0x20);
		});

		scenario("IRL interrupt: accepted above the mask, SR.I = level", [&] {
			cpu.Boot(Code, 0x8000);
			cpu.SetWords(Code, { 0x0009, 0x0009 });
			cpu.SetLong(70 * 4, 0x3000);
			s.SR = 0x30; //I = 3
			cpu.acceptedLevels.clear();
			cpu.SetIrl(12, 70);
			cpu.Step();
			r.Eq("PC", s.PC, 0x3000);
			r.Eq("SR.I", (s.SR >> 4) & 15, 12);
			r.Eq("saved PC", cpu.Long(0x8000 - 8), Code);
			r.Eq("saved SR", cpu.Long(0x8000 - 4), 0x30);
			r.Eq("acknowledged level", cpu.acceptedLevels.size() == 1 ? cpu.acceptedLevels[0] : 0, 12);
			cpu.SetIrl(0, 0);
		});

		scenario("IRL interrupt: not accepted at or below the mask", [&] {
			cpu.Boot(Code);
			cpu.SetWords(Code, { 0x0009 });
			s.SR = 0xC0; //I = 12
			cpu.SetIrl(12, 70);
			cpu.Step();
			r.Eq("PC", s.PC, Code + 2);
			cpu.SetIrl(0, 0);
		});

		scenario("no interrupt between a delayed branch and its slot", [&] {
			cpu.Boot(Code, 0x8000);
			cpu.SetWords(Code, { 0xA002, 0x0009 }); //BRA; NOP
			cpu.SetLong(70 * 4, 0x3000);
			s.SR = 0;
			cpu.Step();
			cpu.SetIrl(12, 70);
			cpu.Step(); //slot: no interrupt yet
			r.Eq("PC after slot", s.PC, Code + 8);
			cpu.Step();
			r.Eq("PC after interrupt", s.PC, 0x3000);
			r.Eq("saved PC = branch target", cpu.Long(0x8000 - 8), Code + 8);
			cpu.SetIrl(0, 0);
		});

		scenario("no interrupt right after LDC", [&] {
			cpu.Boot(Code, 0x8000);
			cpu.SetWords(Code, { 0x401E, 0x0009, 0x0009 }); //LDC R0,GBR; NOP; NOP
			cpu.SetLong(70 * 4, 0x3000);
			s.SR = 0;
			cpu.SetIrl(12, 70);
			cpu.Step(); //accepted before LDC? yes: the check happens before the instruction
			r.Eq("taken before LDC", s.PC, 0x3000);
			s.PC = Code;
			s.SR = 0xF0;
			cpu.Step(); //LDC with I = 15
			s.SR = 0; //lower the mask by hand: the interrupt must still wait one instruction
			cpu.Step();
			r.Eq("PC after the instruction following LDC", s.PC, Code + 4);
			cpu.Step();
			r.Eq("then taken", s.PC, 0x3000);
			cpu.SetIrl(0, 0);
		});

		scenario("priority: DIVU at 12 beats IRL 10; IRL beats DMAC at equal level", [&] {
			cpu.Boot(Code, 0x8000);
			cpu.SetWords(Code, { 0x0009, 0x0009 });
			cpu.SetLong(0x50 * 4, 0x3000); //DIVU vector $50
			cpu.SetLong(69 * 4, 0x4000);
			s.SR = 0;
			cpu.Io8(0xFFFFFEE2, 0xC0); //IPRA: DIVU = 12, DMAC = 0
			cpu.Io32(0xFFFFFF0C, 0x50); //VCRDIV
			cpu.Io32(0xFFFFFF08, 0x2); //DVCR: OVFIE
			cpu.Io32(0xFFFFFF00, 0); //DVSR = 0
			cpu.Io32(0xFFFFFF04, 5); //divide by zero -> OVF
			cpu.SetIrl(10, 69);
			cpu.Step();
			r.Eq("DIVU first", s.PC, 0x3000);
			cpu.Io32(0xFFFFFF08, 0); //clear OVF/OVFIE
			cpu.Io8(0xFFFFFEE2, 0x0A); //DMAC = 10
			s.SR = 0;
			s.PC = Code;
			s.R[15] = 0x8000;
			//DMAC channel 0 TE + IE pending at level 10 vs IRL 10
			cpu.Io32(0xFFFFFFA0, 0x51);
			cpu.Io32(0xFFFFFF80, 0x100);
			cpu.Io32(0xFFFFFF84, 0x200);
			cpu.Io32(0xFFFFFF88, 1);
			cpu.Io32(0xFFFFFFB0, 1); //DME
			cpu.Io32(0xFFFFFF8C, 0x5207); //DM=1 SM=1 TS=0 (byte), AR, IE, DE -> one transfer, TE
			cpu.SetIrl(0, 0);
			cpu.Step(); //runs the DMA transfer
			cpu.SetIrl(10, 69);
			cpu.Step();
			r.Eq("IRL before DMAC at equal level", s.PC, 0x4000);
			cpu.SetIrl(0, 0);
			cpu.Io32(0xFFFFFF8C, 0);
			cpu.Io32(0xFFFFFFB0, 0);
		});

		scenario("SLEEP waits; an interrupt resumes after it", [&] {
			cpu.Boot(Code, 0x8000);
			cpu.SetWords(Code, { 0x001B, 0x0009 });
			cpu.SetLong(70 * 4, 0x3000);
			s.SR = 0;
			uint64_t c0 = cpu.Cycles();
			cpu.Step();
			r.Eq("SLEEP cycles (3)", cpu.Cycles() - c0, 3);
			cpu.Step();
			cpu.Step();
			r.Eq("sleeping", cpu.IsSleeping(), 1);
			cpu.SetIrl(12, 70);
			cpu.Step();
			r.Eq("woken", cpu.IsSleeping(), 0);
			r.Eq("PC", s.PC, 0x3000);
			r.Eq("saved PC = after SLEEP", cpu.Long(0x8000 - 8), Code + 2);
			cpu.SetIrl(0, 0);
		});

		scenario("MAC.W: 64-bit accumulate (S = 0) and 32-bit saturation (S = 1)", [&] {
			cpu.Boot(Code);
			cpu.SetWords(0x2000, { 0x8000, 0x7FFF }); //-32768, 32767
			cpu.SetWords(0x2100, { 0x8000, 0x7FFF });
			cpu.SetWords(Code, { 0x432F, 0x432F }); //MAC.W @R2+,@R3+ twice
			s.R[2] = 0x2000;
			s.R[3] = 0x2100;
			s.MACH = 0;
			s.MACL = 0;
			s.SR = 0;
			cpu.Step();
			cpu.Step();
			uint64_t expected = (uint64_t)(32768LL * 32768 + 32767LL * 32767);
			r.Eq("MAC", (uint64_t)s.MACH << 32 | s.MACL, expected);
			r.Eq("R2", s.R[2], 0x2004);
			//S = 1: MACL = $7FFFFFF0 + 1073741824 saturates to $7FFFFFFF; MACH unchanged (SH-2)
			s.PC = Code;
			s.R[2] = 0x2000;
			s.R[3] = 0x2100;
			s.MACH = 0x1234;
			s.MACL = 0x7FFFFFF0;
			s.SR = Sh2Core::SrS;
			cpu.Step();
			r.Eq("MACL saturated", s.MACL, 0x7FFFFFFF);
			r.Eq("MACH unchanged on saturation", s.MACH, 0x1234);
			//S = 1, negative accumulator: -16 + 1073741824 fits, no saturation
			s.PC = Code;
			s.R[2] = 0x2000;
			s.R[3] = 0x2100;
			s.MACH = 0;
			s.MACL = (uint32_t)-16;
			cpu.Step();
			r.Eq("MACL signed sum", s.MACL, (uint32_t)(1073741824 - 16));
			r.Eq("MACH unchanged", s.MACH, 0);
		});

		scenario("MAC.L: 64-bit accumulate and 48-bit saturation", [&] {
			cpu.Boot(Code);
			cpu.SetLong(0x2000, 0x40000000);
			cpu.SetLong(0x2100, 0x40000000);
			cpu.SetWords(Code, { 0x032F }); //MAC.L @R2+,@R3+
			s.R[2] = 0x2000;
			s.R[3] = 0x2100;
			s.MACH = 0;
			s.MACL = 0;
			s.SR = 0;
			uint64_t c0 = cpu.Cycles();
			cpu.Step();
			r.Eq("MAC", (uint64_t)s.MACH << 32 | s.MACL, 0x1000000000000000ull);
			r.Eq("cycles (3)", cpu.Cycles() - c0, 3);
			s.PC = Code;
			s.R[2] = 0x2000;
			s.R[3] = 0x2100;
			s.MACH = 0;
			s.MACL = 0;
			s.SR = Sh2Core::SrS;
			cpu.Step();
			r.Eq("MAC saturated to 48 bits", (uint64_t)s.MACH << 32 | s.MACL, 0x00007FFFFFFFFFFFull);
		});

		scenario("DIVU 32/32 and 64/32", [&] {
			cpu.Boot(Code);
			cpu.Io32(0xFFFFFF00, 7);
			cpu.Io32(0xFFFFFF04, (uint32_t)-100);
			r.Eq("quotient", cpu.RdIo32(0xFFFFFF04), (uint32_t)-14);
			r.Eq("remainder", cpu.RdIo32(0xFFFFFF10), (uint32_t)-2);
			r.Eq("DVDNTL mirror", cpu.RdIo32(0xFFFFFF14), (uint32_t)-14);
			cpu.Io32(0xFFFFFF00, 1000);
			cpu.Io32(0xFFFFFF10, 1); //DVDNTH
			cpu.Io32(0xFFFFFF14, 0); //DVDNTL: dividend $1_00000000
			r.Eq("64/32 quotient", cpu.RdIo32(0xFFFFFF14), 4294967);
			r.Eq("64/32 remainder", cpu.RdIo32(0xFFFFFF10), 296);
			r.Eq("no overflow", cpu.RdIo32(0xFFFFFF08) & 1, 0);
			//$80000000 / -1 overflows (no crash, OVF set)
			cpu.Io32(0xFFFFFF00, 0xFFFFFFFF);
			cpu.Io32(0xFFFFFF04, 0x80000000);
			r.Eq("overflow flag", cpu.RdIo32(0xFFFFFF08) & 1, 1);
			cpu.Io32(0xFFFFFF08, 0);
			cpu.Io32(0xFFFFFF00, 2);
			cpu.Io32(0xFFFFFF10, 0);
			cpu.Io32(0xFFFFFF14, 0x80000000);
			r.Eq("64/32 no overflow for $80000000 / 2", cpu.RdIo32(0xFFFFFF08) & 1, 0);
			r.Eq("64/32 $80000000 / 2", cpu.RdIo32(0xFFFFFF14), 0x40000000);
			//$FFFFFFFE / 2 = $7FFFFFFF is the largest quotient that fits (ares overflows above $7FFF7FFF)
			cpu.Io32(0xFFFFFF14, 0xFFFFFFFE);
			r.Eq("64/32 $FFFFFFFE / 2", cpu.RdIo32(0xFFFFFF14), 0x7FFFFFFF);
			r.Eq("64/32 no overflow at $7FFFFFFF", cpu.RdIo32(0xFFFFFF08) & 1, 0);
			cpu.Io32(0xFFFFFF10, 1);
			cpu.Io32(0xFFFFFF14, 0); //$1_00000000 / 2 = $80000000: overflow
			r.Eq("64/32 overflow at $80000000", cpu.RdIo32(0xFFFFFF08) & 1, 1);
		});

		scenario("DIVU: 39 cycles per division, 6 on overflow; accesses wait for the end", [&] {
			cpu.Boot(Code);
			cpu.Io32(0xFFFFFF00, 3);
			uint64_t c0 = cpu.Cycles();
			cpu.Io32(0xFFFFFF04, 100);
			r.Eq("quotient", cpu.RdIo32(0xFFFFFF04), 33);
			r.Eq("read waited 39 cycles", cpu.Cycles() - c0, 39);
			cpu.Io32(0xFFFFFF00, 0);
			c0 = cpu.Cycles();
			cpu.Io32(0xFFFFFF04, 100);
			cpu.RdIo32(0xFFFFFF04);
			r.Eq("overflow ends after 6 cycles", cpu.Cycles() - c0, 6);
			//Writing DVDNT sign-extends into DVDNTH: -7 / 0 overflows negative
			cpu.Io32(0xFFFFFF04, (uint32_t)-7);
			r.Eq("negative overflow", cpu.RdIo32(0xFFFFFF14), 0x80000000);
			cpu.Io32(0xFFFFFF08, 0);
		});

		scenario("FRT: 16-bit registers through TEMP", [&] {
			cpu.Boot(Code);
			cpu.Io8(0xFFFFFE12, 0x12); //upper byte goes to TEMP
			cpu.Io8(0xFFFFFE13, 0x34); //lower byte write stores both
			r.Eq("FRC upper", cpu.RdIo8(0xFFFFFE12), 0x12);
			cpu.Io8(0xFFFFFE12, 0x56); //TEMP = $56, FRC still $1234
			r.Eq("upper write alone doesn't change FRC (reads TEMP back as lower after an upper read)", cpu.RdIo8(0xFFFFFE12), 0x12);
			r.Eq("lower read returns the TEMP latched by the upper read", cpu.RdIo8(0xFFFFFE13), 0x34);
			cpu.Io16(0xFFFFFE12, 0xABCD);
			r.Eq("word write", cpu.RdIo32(0xFFFFFE10) & 0xFFFF, 0xABCD);
		});

		scenario("DMAC: round-robin alternates channels; fixed mode runs channel 0 first", [&] {
			for(int pr = 0; pr < 2; pr++) {
				cpu.Boot(Code);
				for(uint32_t a = Code; a < Code + 0x40; a += 2) {
					cpu.SetWords(a, { 0x0009 });
				}
				for(int ch = 0; ch < 2; ch++) {
					uint32_t base = 0xFFFFFF80 + ch * 0x10;
					cpu.Io32(base + 0, 0x20004000);
					cpu.Io32(base + 4, 0x20005000 + ch * 0x100);
					cpu.Io32(base + 8, 4);
				}
				cpu.Io32(0xFFFFFFB0, pr ? 9 : 1); //DME (+ PR)
				cpu.Io32(0xFFFFFF8C, 0x5A01);
				cpu.Io32(0xFFFFFF9C, 0x5A01);
				s.SR = 0xF0;
				cpu.Step();
				cpu.Step();
				uint32_t tcr0 = cpu.RdIo32(0xFFFFFF88);
				uint32_t tcr1 = cpu.RdIo32(0xFFFFFF98);
				if(pr) {
					r.Eq("round-robin: channel 0 count", tcr0, 3);
					r.Eq("round-robin: channel 1 count", tcr1, 3);
				} else {
					r.Eq("fixed: channel 0 count", tcr0, 2);
					r.Eq("fixed: channel 1 count", tcr1, 4);
				}
				cpu.Io32(0xFFFFFF8C, 0);
				cpu.Io32(0xFFFFFF9C, 0);
				cpu.Io32(0xFFFFFFB0, 0);
			}
		});

		scenario("FRT: compare match A sets OCFA and clears FRC (CCLRA); OCI interrupt", [&] {
			cpu.Boot(Code, 0x8000);
			for(uint32_t a = Code; a < Code + 0x200; a += 2) {
				cpu.SetWords(a, { 0x0009 });
			}
			cpu.SetLong(0x60 * 4, 0x3000);
			s.SR = 0;
			cpu.Io8(0xFFFFFE16, 0); //TCR: /8
			cpu.Io8(0xFFFFFE17, 0); //TOCR: OCRA selected
			cpu.Io8(0xFFFFFE14, 0);
			cpu.Io8(0xFFFFFE15, 4); //OCRA = 4
			cpu.Io8(0xFFFFFE11, 1); //FTCSR: CCLRA
			cpu.Io8(0xFFFFFE12, 0);
			cpu.Io8(0xFFFFFE13, 0); //FRC = 0
			cpu.Io8(0xFFFFFE60, 0x05); //IPRB: FRT = 5
			cpu.Io8(0xFFFFFE67, 0x60); //VCRC: OCI vector
			cpu.Io8(0xFFFFFE10, 0x08); //TIER: OCIAE
			s.PC = Code;
			int steps = 0;
			while(s.PC != 0x3000 && steps < 100) {
				cpu.Step();
				steps++;
			}
			r.Eq("interrupt taken", s.PC, 0x3000);
			r.Eq("OCFA", (cpu.RdIo8(0xFFFFFE11) >> 3) & 1, 1);
			//4 counts at /8 = 32 cycles; 1 cycle per NOP
			r.Check(steps >= 30 && steps <= 34, "NOPs before the match", steps, 32);
			cpu.Io8(0xFFFFFE10, 0);
		});

		scenario("DMAC auto-request: count, address modes, TE", [&] {
			cpu.Boot(Code);
			for(uint32_t a = Code; a < Code + 0x100; a += 2) {
				cpu.SetWords(a, { 0x0009 });
			}
			for(uint32_t i = 0; i < 4; i++) {
				cpu.SetLong(0x4000 + i * 4, 0x11111111 * (i + 1));
			}
			cpu.Io32(0xFFFFFF80, 0x20004000); //SAR (cache-through)
			cpu.Io32(0xFFFFFF84, 0x20005000); //DAR
			cpu.Io32(0xFFFFFF88, 4); //TCR
			cpu.Io32(0xFFFFFFB0, 1); //DMAOR: DME
			cpu.Io32(0xFFFFFF8C, 0x5A01); //DM=1 SM=1 TS=2 (long), AR, DE
			s.SR = 0xF0;
			for(int i = 0; i < 8; i++) {
				cpu.Step();
			}
			for(uint32_t i = 0; i < 4; i++) {
				r.Eq("copied", cpu.Long(0x5000 + i * 4), 0x11111111 * (i + 1));
			}
			r.Eq("TE", (cpu.RdIo32(0xFFFFFF8C) >> 1) & 1, 1);
			r.Eq("TCR", cpu.RdIo32(0xFFFFFF88), 0);
			r.Eq("SAR", cpu.RdIo32(0xFFFFFF80), 0x20004010);
			cpu.Io32(0xFFFFFF8C, 0);
			cpu.Io32(0xFFFFFFB0, 0);
		});

		scenario("cache: line fill, hit, write-through, purge", [&] {
			cpu.Boot(Code);
			cpu.SetLong(0x4000, 0xAABBCCDD);
			cpu.Io8(0xFFFFFE92, 0x10); //purge
			cpu.Io8(0xFFFFFE92, 0x01); //enable
			cpu.busReads = 0;
			r.Eq("read", cpu.Rd32(0x4000), 0xAABBCCDD);
			r.Eq("line fill: 4 long reads", cpu.busReads, 4);
			cpu.busReads = 0;
			cpu.SetLong(0x4000, 0x12345678); //changed behind the cache's back
			r.Eq("hit returns the cached value", cpu.Rd32(0x4004 - 4), 0xAABBCCDD);
			r.Eq("no bus read on a hit", cpu.busReads, 0);
			r.Eq("cache-through sees memory", cpu.Rd32(0x20004000), 0x12345678);
			cpu.Io32(0x40004000, 0); //associative purge of the line
			r.Eq("after purge", cpu.Rd32(0x4000), 0x12345678);
			cpu.Io8(0xFFFFFE92, 0x00);
		});

		printf("sh2 scenarios: %d checks, %d failed\n", r.checks, r.failures);
		return r.failures ? 1 : 0;
	}
}
