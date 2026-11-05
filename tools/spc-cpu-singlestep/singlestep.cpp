// singlestep.cpp - differential test of MesenCE's SNES sound CPU (Spc, SPC700)
// against the SingleStepTests spc700 suite (https://github.com/SingleStepTests/spc700).
//
// Each test gives the initial CPU + RAM state, the expected final state, and one
// entry per SPC700 cycle: [address, value, "read" | "write" | "wait"].  A null
// address or value means the suite doesn't define it (e.g. a dummy read whose
// value is irrelevant).  The suite assumes a flat 64 KiB RAM.
//
// Each test runs on DummySpc: the Spc source (Spc.cpp + Spc.Instructions.cpp)
// compiled with DUMMYSPC, which the debugger uses for predictive breakpoints and
// effective addresses.  It runs the same instruction code as the real Spc; only
// Read/Write differ (the dummy logs writes instead of applying them, and has no
// DSP behind $F3).  Checked:
//   - bus trace: the logged reads/writes against the read/write entries;
//   - cycle count: SpcState::Cycle advances 2 per SPC cycle at the default speed;
//   - final registers, and final RAM (initial RAM + the logged writes).
// The real Spc can't be stopped at an instruction boundary from outside:
// Spc::Run() runs to a cycle target, and some instructions end with a step that
// takes no time (e.g. a branch that isn't taken).
//
// The SPC maps $F0-$FF to its I/O registers and $FFC0-$FFFF to the IPL ROM
// (disabled here).  Tests that access $F0-$FF assume plain RAM there, so they
// are skipped.
//
// The harness also loads an in-memory SNES cartridge (a VirtualFile with no
// folder), as a regression test for FolderUtilities::CombinePath("", ...).
//
// Usage: singlestep [--details N] <file.json | dir> ...

#include "pch.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <nlohmann/json.hpp>
// Test-tool-only access to Spc internals. Standard headers are already included
// via pch.h, so only MesenCE headers see this.
#define private public
#include "Shared/Emulator.h"
#include "SNES/SnesConsole.h"
#include "SNES/Spc.h"
#include "SNES/Debugger/DummySpc.h"
#undef private
#include "Utilities/VirtualFile.h"
#include "Utilities/FolderUtilities.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

static string Hex(uint32_t v, int width)
{
	std::ostringstream ss;
	ss << std::hex << std::uppercase << std::setw(width) << std::setfill('0') << v;
	return ss.str();
}

static bool IsWrite(MemoryOperationType t)
{
	return t == MemoryOperationType::Write || t == MemoryOperationType::DummyWrite;
}

enum FailKind
{
	FailRegs = 1,
	FailRam = 2,
	FailCycleCount = 4,
	FailBus = 8,
};

struct Harness
{
	uint8_t DummyRam[0x10000] = {};
	uint8_t FinalRam[0x10000] = {};
	DummySpc* Dummy = nullptr;

	static void LoadRam(uint8_t* ram, const json& list)
	{
		memset(ram, 0, 0x10000);
		for(const auto& e : list) {
			ram[e[0].get<uint16_t>()] = e[1].get<uint8_t>();
		}
	}

	static void LoadRegs(SpcState& st, const json& in)
	{
		st.PC = in["pc"].get<uint16_t>();
		st.A = in["a"].get<uint8_t>();
		st.X = in["x"].get<uint8_t>();
		st.Y = in["y"].get<uint8_t>();
		st.SP = in["sp"].get<uint8_t>();
		st.PS = in["psw"].get<uint8_t>();
		st.RomEnabled = false;
		st.WriteEnabled = true;
		st.InternalSpeed = 0;
		st.ExternalSpeed = 0;
		st.StopState = SnesCpuStopState::Running;
	}

	static bool TouchesIoPage(const json& t)
	{
		for(const auto& c : t["cycles"]) {
			if(!c[0].is_null()) {
				uint16_t a = c[0].get<uint16_t>();
				if(a >= 0xF0 && a <= 0xFF) {
					return true;
				}
			}
		}
		return false;
	}

	int Run(const json& t, string& detail)
	{
		int kinds = 0;
		std::ostringstream d;
		const json& in = t["initial"];
		const json& fin = t["final"];
		const json& cycles = t["cycles"];

		// 1. Bus trace via DummySpc.
		LoadRam(DummyRam, in["ram"]);
		SpcState st = {};
		LoadRegs(st, in);
		st.Cycle = 0;
		Dummy->SetDummyState(st);
		Dummy->Step();
		uint32_t n = Dummy->GetOperationCount();
		string actualTrace, expectedTrace;
		for(uint32_t i = 0; i < n; i++) {
			MemoryOperationInfo op = Dummy->GetOperationInfo(i);
			actualTrace += (IsWrite(op.Type) ? " W" : " R") + Hex(op.Address, 4) + "=" + Hex((uint8_t)op.Value, 2);
		}
		bool busOk = true;
		uint32_t opIndex = 0;
		for(const auto& c : cycles) {
			string kind = c[2].get<string>();
			if(kind == "wait") {
				expectedTrace += " --";
				continue;
			}
			bool w = kind == "write";
			expectedTrace += (w ? " W" : " R") + (c[0].is_null() ? string("????") : Hex(c[0].get<uint16_t>(), 4)) + "=" +
				(c[1].is_null() ? string("??") : Hex(c[1].get<uint8_t>(), 2));
			if(opIndex >= n) {
				busOk = false;
				continue;
			}
			MemoryOperationInfo op = Dummy->GetOperationInfo(opIndex++);
			if(IsWrite(op.Type) != w || (!c[0].is_null() && op.Address != c[0].get<uint16_t>()) ||
				(!c[1].is_null() && (uint8_t)op.Value != c[1].get<uint8_t>())) {
				busOk = false;
			}
		}
		if(opIndex != n || !busOk) {
			kinds |= FailBus;
		}

		// 2. Final state and timing. DummySpc executes the same instruction code as the
		// real Spc (Spc.Instructions.cpp); only Read/Write differ, and only on the $F0-$FF
		// I/O page, which these tests avoid. Writes are logged rather than applied, so
		// the final RAM is the initial RAM with the logged writes applied in order.
		SpcState& rs = Dummy->_state;
		uint64_t spcCycles = rs.Cycle / 2;
		if(spcCycles != cycles.size()) {
			kinds |= FailCycleCount;
			d << " cycles=" << spcCycles << "(exp " << cycles.size() << ")";
		}
		uint8_t* ram = FinalRam;
		LoadRam(ram, in["ram"]);
		for(uint32_t i = 0; i < n; i++) {
			MemoryOperationInfo op = Dummy->GetOperationInfo(i);
			if(IsWrite(op.Type)) {
				ram[op.Address] = (uint8_t)op.Value;
			}
		}

		auto chk = [&](const char* name, uint32_t actual, uint32_t expected, int width) {
			if(actual != expected) {
				kinds |= FailRegs;
				d << " " << name << "=" << Hex(actual, width) << "(exp " << Hex(expected, width) << ")";
			}
		};
		chk("PC", rs.PC, fin["pc"].get<uint16_t>(), 4);
		chk("A", rs.A, fin["a"].get<uint8_t>(), 2);
		chk("X", rs.X, fin["x"].get<uint8_t>(), 2);
		chk("Y", rs.Y, fin["y"].get<uint8_t>(), 2);
		chk("SP", rs.SP, fin["sp"].get<uint8_t>(), 2);
		chk("PSW", rs.PS, fin["psw"].get<uint8_t>(), 2);
		for(const auto& e : fin["ram"]) {
			uint16_t a = e[0].get<uint16_t>();
			uint8_t v = e[1].get<uint8_t>();
			if(ram[a] != v) {
				kinds |= FailRam;
				d << " [" << Hex(a, 4) << "]=" << Hex(ram[a], 2) << "(exp " << Hex(v, 2) << ")";
			}
		}
		if(kinds & FailBus) {
			d << "\n      actual bus:  " << actualTrace << "\n      expected bus:" << expectedTrace;
		}
		detail = t["name"].get<string>() + ":" + d.str();
		return kinds;
	}
};

// Opcodes where MesenCE deliberately differs from the suite. They are reported but
// do not count as failures. See docs/research/snes-verification-findings.md.
static const std::map<string, string> KnownDivergences = {
	{ "ef", "SLEEP: the suite records 3 iterations of ares' endless read(PC+1)/idle loop; MesenCE re-executes the opcode (PC stays on it, 1 cycle per pass). Only reset leaves this state" },
	{ "ff", "STOP: same as SLEEP (EF)" },
};

int main(int argc, char* argv[])
{
	int maxDetails = 3;
	vector<fs::path> files;
	for(int i = 1; i < argc; i++) {
		string a = argv[i];
		if(a == "--details" && i + 1 < argc) {
			maxDetails = std::stoi(argv[++i]);
		} else if(fs::is_directory(a)) {
			for(const auto& e : fs::directory_iterator(a)) {
				if(e.path().extension() == ".json") {
					files.push_back(e.path());
				}
			}
		} else {
			files.push_back(a);
		}
	}
	std::sort(files.begin(), files.end());
	if(files.empty()) {
		std::cerr << "usage: singlestep [--details N] <file.json | dir> ...\n";
		return 2;
	}

	fs::path home = fs::temp_directory_path() / "mesen-spc-singlestep-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);
	emu._isRunAheadFrame = true;

	// Regression check: loading a cartridge from memory (no folder) used to read
	// folder[-1] in FolderUtilities::CombinePath (called by Msu1::Init).
	// See docs/research/snes-verification-findings.md.
	vector<uint8_t> rom(0x8000, 0);
	memcpy(rom.data() + 0x7FC0, "SINGLESTEP           ", 21);
	rom[0x7FD5] = 0x20; // LoROM
	rom[0x7FD7] = 0x08; // 256 KiB (header value; only used for detection)
	rom[0x7FDC] = 0xFF;
	rom[0x7FDD] = 0xFF; // checksum complement
	rom[0x7FFC] = 0x00;
	rom[0x7FFD] = 0x80; // reset vector
	shared_ptr<SnesConsole> console(new SnesConsole(&emu));
	VirtualFile romFile(rom.data(), rom.size(), "singlestep.sfc");
	if(console->LoadRom(romFile) != LoadRomResult::Success) {
		std::cerr << "failed to set up SNES console\n";
		return 2;
	}
	emu._console.reset(console);

	Harness* h = new Harness();
	h->Dummy = new DummySpc(h->DummyRam);

	uint64_t total = 0, failed = 0, known = 0, skipped = 0;
	for(const fs::path& f : files) {
		std::ifstream in(f);
		json tests = json::parse(in);
		int fail = 0, shown = 0, run = 0;
		std::map<int, int> kindCounts;
		vector<string> details;
		for(const json& t : tests) {
			if(Harness::TouchesIoPage(t)) {
				skipped++;
				continue;
			}
			string detail;
			int k = h->Run(t, detail);
			total++;
			run++;
			if(k) {
				fail++;
				kindCounts[k]++;
				if(shown++ < maxDetails) {
					details.push_back(detail);
				}
			}
		}
		string op = f.stem().string();
		auto knownIt = KnownDivergences.find(op);
		bool isKnown = knownIt != KnownDivergences.end();
		(isKnown ? known : failed) += fail;
		std::cout << "op " << op << ": " << (run - fail) << "/" << run << " pass";
		if(fail && isKnown) {
			std::cout << "  KNOWN DIVERGENCE: " << knownIt->second;
		}
		if(fail) {
			std::cout << "  (";
			bool first = true;
			for(auto& [k, cnt] : kindCounts) {
				std::cout << (first ? "" : ", ") << cnt << "x";
				if(k & FailRegs) {
					std::cout << " regs";
				}
				if(k & FailRam) {
					std::cout << " ram";
				}
				if(k & FailCycleCount) {
					std::cout << " cycles";
				}
				if(k & FailBus) {
					std::cout << " bus";
				}
				first = false;
			}
			std::cout << ")";
		}
		std::cout << "\n";
		if(!isKnown) {
			for(const string& s : details) {
				std::cout << "    " << s << "\n";
			}
		}
	}
	std::cout << "TOTAL: " << (total - failed - known) << "/" << total << " pass, " << failed << " unexpected fail, " << known
				 << " known divergence, " << skipped << " skipped (access $F0-$FF I/O registers)\n";
	return failed ? 1 : 0;
}
