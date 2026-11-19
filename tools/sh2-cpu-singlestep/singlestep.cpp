// SH-2 (Core/MD/Sh2) vs the SingleStepTests sh4 suite, on the encodings the SH-2 shares with the
// SH-4. See docs/research/32x-verification-findings.md (S0-F3, S1).
//
// Each sh4 test runs four instructions: NOP, the instruction under test, ADD R1,R1 (the delay
// slot when the instruction branches) and NOP; a branch target holds ADD R2,R2. The suite was
// generated from Reicast's interpreter. What is compared:
//   - R0-R15, PC, GBR, VBR, MACH, MACL, PR, and SR masked to the SH-2 bits ($3F3);
//   - every instruction fetch address (in order), and every data read/write address, size and
//     written value (in order).
// What is left out, and why:
//   - SH-4-only encodings (the SH-2 core decodes them as illegal);
//   - TRAPA, RTE, SLEEP: the SH-4 exception model (TRA/EXPEVT, SSR/SPC) differs from the SH-2's
//     stack-based one; covered by scenarios instead;
//   - LDC/LDC.L to SR when the test flips SR.RB: the SH-4 switches register banks, the SH-2 has none;
//   - STC SR / STC.L SR store the full SH-4 SR: the stored value is compared masked to $3F3;
//   - timing (SH-4 timing differs).
#include "MD/Sh2/Sh2Core.h"
#include "scenarios.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>

namespace
{
	struct State
	{
		uint32_t R[16];
		uint32_t PC, GBR, SR, VBR, MACL, MACH, PR;
	};

	struct Access
	{
		bool write;
		uint32_t addr;
		uint64_t value;
		uint32_t size;
	};

	struct Test
	{
		State initial;
		State final;
		std::vector<std::pair<uint32_t, uint16_t>> fetches;
		std::vector<Access> accesses;
		uint16_t opcode;
		bool malformed; //a fetch value wider than 16 bits: a broken record in the suite
	};

	uint32_t Rd32(const uint8_t* p)
	{
		return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	}
	uint64_t Rd64(const uint8_t* p)
	{
		return (uint64_t)Rd32(p) | (uint64_t)Rd32(p + 4) << 32;
	}

	void LoadState(const uint8_t* p, State& s)
	{
		const uint8_t* v = p + 8;
		for(int i = 0; i < 16; i++) {
			s.R[i] = Rd32(v + i * 4);
		}
		s.PC = Rd32(v + 56 * 4);
		s.GBR = Rd32(v + 57 * 4);
		s.SR = Rd32(v + 58 * 4);
		s.VBR = Rd32(v + 61 * 4);
		s.MACL = Rd32(v + 64 * 4);
		s.MACH = Rd32(v + 65 * 4);
		s.PR = Rd32(v + 66 * 4);
	}

	bool LoadFile(const std::string& path, std::vector<Test>& tests)
	{
		FILE* f = fopen(path.c_str(), "rb");
		if(!f) {
			return false;
		}
		std::vector<uint8_t> buf;
		fseek(f, 0, SEEK_END);
		buf.resize((size_t)ftell(f));
		fseek(f, 0, SEEK_SET);
		size_t got = fread(buf.data(), 1, buf.size(), f);
		fclose(f);
		if(got != buf.size()) {
			return false;
		}
		size_t p = 0;
		while(p + 4 <= buf.size()) {
			uint32_t full = Rd32(&buf[p]);
			if(full == 0 || p + full > buf.size()) {
				break;
			}
			Test t = {};
			size_t q = p + 4;
			uint32_t sz = Rd32(&buf[q]);
			LoadState(&buf[q], t.initial);
			q += sz;
			sz = Rd32(&buf[q]);
			LoadState(&buf[q], t.final);
			q += sz;
			sz = Rd32(&buf[q]);
			const uint8_t* c = &buf[q + 12];
			for(int i = 0; i < 4; i++, c += 44) {
				uint32_t actions = Rd32(c);
				if(actions & 4) {
					uint32_t value = Rd32(c + 8);
					t.malformed |= value > 0xFFFF;
					t.fetches.push_back({ Rd32(c + 4), (uint16_t)value });
				}
				//A read-modify-write logs both in one cycle: the read comes first
				if(actions & 1) {
					t.accesses.push_back({ false, Rd32(c + 28), Rd64(c + 32), Rd32(c + 40) });
				}
				if(actions & 2) {
					t.accesses.push_back({ true, Rd32(c + 12), Rd64(c + 16), Rd32(c + 24) });
				}
			}
			q += sz;
			sz = Rd32(&buf[q]);
			t.opcode = (uint16_t)Rd32(&buf[q + 8 + 4]);
			tests.push_back(std::move(t));
			p += full;
		}
		return true;
	}

	class TestCpu : public Sh2Core
	{
	public:
		const Test* test = nullptr;
		size_t fetchIndex = 0;
		size_t accessIndex = 0;
		std::string error;

		void Fail(const char* fmt, uint32_t a, uint64_t b = 0, uint32_t c = 0)
		{
			if(error.empty()) {
				char buf[160];
				snprintf(buf, sizeof(buf), fmt, a, (unsigned long long)b, c);
				error = buf;
			}
		}

		uint64_t Read(uint32_t addr, uint32_t size)
		{
			if(accessIndex >= test->accesses.size()) {
				Fail("unexpected read $%08X (size %llu)", addr, size);
				return 0;
			}
			const Access& a = test->accesses[accessIndex++];
			if(a.write || a.addr != addr || a.size != size) {
				Fail("read $%08X size %llu, expected access at $%08X", addr, size, a.addr);
			}
			return a.value;
		}

		void Write(uint32_t addr, uint64_t value, uint32_t size)
		{
			if(accessIndex >= test->accesses.size()) {
				Fail("unexpected write $%08X = $%llX", addr, value);
				return;
			}
			const Access& a = test->accesses[accessIndex++];
			uint64_t mask = size >= 8 ? ~0ull : ((1ull << (size * 8)) - 1);
			uint64_t expected = a.value & mask;
			//STC.L SR,@-Rn stores the SH-4 SR: compare the SH-2 bits
			if((test->opcode & 0xF0FF) == 0x4003) {
				expected &= SrMask;
				value &= SrMask;
			}
			if(!a.write || a.addr != addr || a.size != size || expected != (value & mask)) {
				Fail("write $%08X = $%llX (size %u)", addr, value, size);
				if(a.write) {
					char buf[96];
					snprintf(buf, sizeof(buf), "; expected $%08X = $%llX (size %u)", a.addr, (unsigned long long)expected, a.size);
					error += buf;
				}
			}
		}

		uint8_t BusRead8(uint32_t addr) override { return (uint8_t)Read(addr, 1); }
		uint16_t BusRead16(uint32_t addr) override { return (uint16_t)Read(addr, 2); }
		uint32_t BusRead32(uint32_t addr) override { return (uint32_t)Read(addr, 4); }
		void BusWrite8(uint32_t addr, uint8_t v) override { Write(addr, v, 1); }
		void BusWrite16(uint32_t addr, uint16_t v) override { Write(addr, v, 2); }
		void BusWrite32(uint32_t addr, uint32_t v) override { Write(addr, v, 4); }
		uint16_t BusFetch16(uint32_t addr) override
		{
			if(fetchIndex >= test->fetches.size()) {
				Fail("unexpected fetch $%08X", addr);
				return 9;
			}
			auto& f = test->fetches[fetchIndex++];
			if(f.first != addr) {
				Fail("fetch $%08X, expected $%08X", addr, f.first);
			}
			return f.second;
		}

		bool Run(const Test& t)
		{
			test = &t;
			fetchIndex = 0;
			accessIndex = 0;
			error.clear();
			PowerOn();
			_state.PendingReset = 0;
			SetFlatBus(true);
			for(int i = 0; i < 16; i++) {
				_state.R[i] = t.initial.R[i];
			}
			_state.PC = t.initial.PC;
			_state.GBR = t.initial.GBR;
			_state.SR = t.initial.SR & SrMask;
			_state.VBR = t.initial.VBR;
			_state.MACH = t.initial.MACH;
			_state.MACL = t.initial.MACL;
			_state.PR = t.initial.PR;
			//Keep interrupts out of the way (none are requested, but mask anyway)
			for(int i = 0; i < 4; i++) {
				Step();
			}
			if(accessIndex != t.accesses.size()) {
				Fail("%u data accesses, expected %llu", (uint32_t)accessIndex, t.accesses.size());
			}
			if(fetchIndex != t.fetches.size()) {
				Fail("%u fetches, expected %llu", (uint32_t)fetchIndex, t.fetches.size());
			}

			const State& e = t.final;
			bool stcSr = (t.opcode & 0xF0FF) == 0x0002;
			for(int i = 0; i < 16; i++) {
				uint32_t exp = e.R[i];
				uint32_t got = _state.R[i];
				if(stcSr && (uint32_t)i == ((t.opcode >> 8) & 15u)) {
					exp &= SrMask;
					got &= SrMask;
				}
				if(exp != got && error.empty()) {
					char buf[64];
					snprintf(buf, sizeof(buf), "R%d = $%08X, expected $%08X", i, got, exp);
					error = buf;
				}
			}
			auto cmp = [&](const char* name, uint32_t got, uint32_t exp) {
				if(got != exp && error.empty()) {
					char buf[96];
					snprintf(buf, sizeof(buf), "%s = $%08X, expected $%08X", name, got, exp);
					error = buf;
				}
			};
			cmp("PC", _state.PC, e.PC);
			cmp("GBR", _state.GBR, e.GBR);
			cmp("VBR", _state.VBR, e.VBR);
			cmp("MACH", _state.MACH, e.MACH);
			cmp("MACL", _state.MACL, e.MACL);
			cmp("PR", _state.PR, e.PR);
			cmp("SR", _state.SR & SrMask, e.SR & SrMask);
			return error.empty();
		}
	};

	bool IsSh2Encoding(uint16_t op)
	{
		return Sh2Core::Disassemble(0, op).rfind(".WORD", 0) != 0;
	}

	bool IsExcluded(uint16_t op)
	{
		return (op & 0xFF00) == 0xC300 //TRAPA
			|| op == 0x002B //RTE
			|| op == 0x001B //SLEEP
			|| (op & 0xF00F) == 0x000F //MAC.L (not in the suite)
			|| (op & 0xF00F) == 0x400F; //MAC.W (not in the suite)
	}
}

int main(int argc, char** argv)
{
	int details = 3;
	std::vector<std::string> paths;
	for(int i = 1; i < argc; i++) {
		if(!strcmp(argv[i], "--scenarios")) {
			return Scenarios::Run();
		}
		if(!strcmp(argv[i], "--details") && i + 1 < argc) {
			details = atoi(argv[++i]);
		} else {
			paths.push_back(argv[i]);
		}
	}
	if(paths.empty()) {
		fprintf(stderr, "usage: sh2-cpu-singlestep --scenarios | [--details N] <sh4 suite dir | files...>\n");
		return 2;
	}

	std::vector<std::string> files;
	for(auto& p : paths) {
		DIR* d = opendir(p.c_str());
		if(d) {
			while(dirent* e = readdir(d)) {
				std::string name = e->d_name;
				if(name.size() > 9 && name.compare(name.size() - 9, 9, ".json.bin") == 0) {
					files.push_back(p + "/" + name);
				}
			}
			closedir(d);
		} else {
			files.push_back(p);
		}
	}
	std::sort(files.begin(), files.end());

	TestCpu cpu;
	uint64_t passed = 0, failed = 0, skipped = 0, rbSkipped = 0, malformed = 0;
	int filesRun = 0, filesSkipped = 0, filesFailed = 0;
	for(auto& file : files) {
		std::vector<Test> tests;
		if(!LoadFile(file, tests) || tests.empty()) {
			fprintf(stderr, "cannot read %s\n", file.c_str());
			return 2;
		}
		uint16_t op = tests[0].opcode;
		std::string base = file.substr(file.find_last_of('/') + 1);
		//Only the integer instructions: the FPU mode variants (sz1/pr1) repeat them
		if(!IsSh2Encoding(op) || IsExcluded(op) || base.find("_sz0_pr0") == std::string::npos) {
			filesSkipped++;
			skipped += tests.size();
			continue;
		}
		filesRun++;
		int shown = 0;
		uint64_t fileFailed = 0;
		for(auto& t : tests) {
			if(!IsSh2Encoding(t.opcode)) {
				skipped++;
				continue;
			}
			bool ldcSr = (t.opcode & 0xF0FF) == 0x400E || (t.opcode & 0xF0FF) == 0x4007;
			if(ldcSr && ((t.initial.SR ^ t.final.SR) & 0x20000000)) {
				rbSkipped++;
				continue;
			}
			if(t.malformed) {
				malformed++;
				continue;
			}
			bool pass = cpu.Run(t);
			if(pass) {
				passed++;
			} else {
				failed++;
				fileFailed++;
				if(shown++ < details) {
					printf("FAIL %s [%04X %s]: %s\n", base.c_str(), t.opcode, Sh2Core::Disassemble(t.initial.PC + 2, t.opcode).c_str(), cpu.error.c_str());
				}
			}
		}
		if(fileFailed) {
			filesFailed++;
			printf("     %s: %llu/%zu failed\n", base.c_str(), (unsigned long long)fileFailed, tests.size());
		}
	}

	printf("sh2-cpu: %llu passed, %llu failed; %d files run (%d with failures); %d files (%llu tests) skipped as SH-4-only or excluded;\n"
			 "         skipped: %llu LDC SR tests with an SR.RB bank switch, %llu malformed records\n",
		(unsigned long long)passed,
		(unsigned long long)failed,
		filesRun,
		filesFailed,
		filesSkipped,
		(unsigned long long)skipped,
		(unsigned long long)rbSkipped,
		(unsigned long long)malformed);
	return failed ? 1 : 0;
}
