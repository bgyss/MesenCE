// singlestep.cpp - differential test of MesenCE's Game Boy CPU (GbCpu, SM83)
// against the SingleStepTests sm83 suite (https://github.com/SingleStepTests/sm83).
//
// Each test gives the initial CPU + RAM state, the expected final state, and one
// entry per M-cycle: [address, value, pins], where pins is "r-m" (read), "-wm"
// (write) or "---" (no memory access).  The suite assumes a flat 64 KiB RAM.
//
// Each test is executed twice, from the same initial state:
//   1. DummyGbCpu: the GbCpu source compiled with DUMMYCPU, which the debugger
//      uses.  Its memory accesses are logged in order and checked against the
//      r-m / -wm entries (address, value, direction).
//   2. The real GbCpu, with every memory page of a real Gameboy console pointed
//      at the flat RAM and all I/O-register pages disabled.  It gives the final
//      registers and RAM, and the M-cycle count (CycleCount advances 4 per
//      M-cycle), which is checked against the number of listed cycles.
//
// Usage: singlestep [--details N] <file.json | dir> ...

#include "pch.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <nlohmann/json.hpp>
// Test-tool-only access to GbMemoryManager page tables and GbCpu::_prevIrqVector.
// Standard headers are already included via pch.h, so only MesenCE headers see this.
#define private public
#include "Shared/Emulator.h"
#include "Gameboy/Gameboy.h"
#include "Gameboy/GbCpu.h"
#include "Gameboy/GbMemoryManager.h"
#include "Gameboy/Debugger/DummyGbCpu.h"
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
	return t == MemoryOperationType::Write || t == MemoryOperationType::DummyWrite || t == MemoryOperationType::DmaWrite;
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
	Gameboy* Gb = nullptr;
	GbMemoryManager* Mm = nullptr;
	GbCpu* Cpu = nullptr;
	DummyGbCpu Dummy;
	uint8_t Ram[0x10000] = {};

	void MapFlat()
	{
		for(int i = 0; i < 0x100; i++) {
			Mm->_reads[i] = Ram + i * 0x100;
			Mm->_writes[i] = Ram + i * 0x100;
			Mm->_state.IsReadRegister[i] = false;
			Mm->_state.IsWriteRegister[i] = false;
		}
		Mm->_state.IrqEnabled = 0;
		Mm->_state.IrqRequests = 0;
	}

	void LoadRam(const json& list)
	{
		memset(Ram, 0, sizeof(Ram));
		for(const auto& e : list) {
			Ram[e[0].get<uint16_t>()] = e[1].get<uint8_t>();
		}
	}

	static void LoadRegs(GbCpuState& st, const json& in)
	{
		st.PC = in["pc"].get<uint16_t>();
		st.SP = in["sp"].get<uint16_t>();
		st.A = in["a"].get<uint8_t>();
		st.Flags = in["f"].get<uint8_t>();
		st.B = in["b"].get<uint8_t>();
		st.C = in["c"].get<uint8_t>();
		st.D = in["d"].get<uint8_t>();
		st.E = in["e"].get<uint8_t>();
		st.H = in["h"].get<uint8_t>();
		st.L = in["l"].get<uint8_t>();
		st.IME = in["ime"].get<int>() != 0;
		// The suite's initial "ie" is the IE register (irrelevant here), not "EI pending".
		st.EiPending = false;
		st.HaltCounter = 0;
		st.HaltBug = false;
		st.Stopped = false;
	}

	int Run(const json& t, string& detail)
	{
		int kinds = 0;
		std::ostringstream d;
		const json& in = t["initial"];
		const json& fin = t["final"];
		const json& cycles = t["cycles"];

		// 1. Bus trace via DummyGbCpu.
		LoadRam(in["ram"]);
		MapFlat();
		GbCpuState st = {};
		LoadRegs(st, in);
		Dummy.SetDummyState(st);
		Dummy.Exec();
		vector<std::tuple<uint16_t, uint8_t, bool>> expectedOps;
		for(const auto& c : cycles) {
			string pins = c[2].get<string>();
			if(pins.size() >= 2 && (pins[0] == 'r' || pins[1] == 'w') && !c[1].is_null()) {
				expectedOps.emplace_back(c[0].get<uint16_t>(), c[1].get<uint8_t>(), pins[1] == 'w');
			}
		}
		string actualTrace, expectedTrace;
		uint32_t n = Dummy.GetOperationCount();
		for(uint32_t i = 0; i < n; i++) {
			MemoryOperationInfo op = Dummy.GetOperationInfo(i);
			actualTrace += (IsWrite(op.Type) ? " W" : " R") + Hex(op.Address, 4) + "=" + Hex((uint8_t)op.Value, 2);
		}
		for(auto& [a, v, w] : expectedOps) {
			expectedTrace += (w ? " W" : " R") + Hex(a, 4) + "=" + Hex(v, 2);
		}
		if(actualTrace != expectedTrace) {
			kinds |= FailBus;
		}

		// 2. Final state and timing via the real GbCpu.
		LoadRam(in["ram"]);
		MapFlat();
		GbCpuState& rs = Cpu->GetState();
		LoadRegs(rs, in);
		Cpu->_prevIrqVector = 0;
		uint64_t startCycle = rs.CycleCount;
		Cpu->Exec();
		uint64_t mcycles = (rs.CycleCount - startCycle) / 4;
		if(mcycles != cycles.size()) {
			kinds |= FailCycleCount;
			d << " mcycles=" << mcycles << "(exp " << cycles.size() << ")";
		}

		auto chk = [&](const char* name, uint32_t actual, uint32_t expected, int width) {
			if(actual != expected) {
				kinds |= FailRegs;
				d << " " << name << "=" << Hex(actual, width) << "(exp " << Hex(expected, width) << ")";
			}
		};
		chk("PC", rs.PC, fin["pc"].get<uint16_t>(), 4);
		chk("SP", rs.SP, fin["sp"].get<uint16_t>(), 4);
		chk("A", rs.A, fin["a"].get<uint8_t>(), 2);
		chk("F", rs.Flags, fin["f"].get<uint8_t>(), 2);
		chk("B", rs.B, fin["b"].get<uint8_t>(), 2);
		chk("C", rs.C, fin["c"].get<uint8_t>(), 2);
		chk("D", rs.D, fin["d"].get<uint8_t>(), 2);
		chk("E", rs.E, fin["e"].get<uint8_t>(), 2);
		chk("H", rs.H, fin["h"].get<uint8_t>(), 2);
		chk("L", rs.L, fin["l"].get<uint8_t>(), 2);
		chk("IME", rs.IME, fin["ime"].get<int>(), 1);
		if(fin.contains("ei")) {
			chk("EI", rs.EiPending, fin["ei"].get<int>(), 1);
		}
		for(const auto& e : fin["ram"]) {
			uint16_t a = e[0].get<uint16_t>();
			uint8_t v = e[1].get<uint8_t>();
			if(Ram[a] != v) {
				kinds |= FailRam;
				d << " [" << Hex(a, 4) << "]=" << Hex(Ram[a], 2) << "(exp " << Hex(v, 2) << ")";
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
// do not count as failures. See docs/research/gb-verification-findings.md.
static const std::map<string, string> KnownDivergences = {
	{ "10", "STOP: MesenCE implements the documented 2-byte form (Pan Docs); the suite models a 1-byte variant" },
	{ "76", "HALT: final state matches; MesenCE counts 1 M-cycle (halted cycles are accounted on later Exec calls), the suite 3" },
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

	fs::path home = fs::temp_directory_path() / "mesen-gb-singlestep-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);
	// The real GbCpu clocks the PPU; once a frame completes it is handed to the video
	// decoder, which needs a decode thread. Run-ahead mode emulates fully but skips
	// frame/audio output, which is what a headless harness needs.
	emu._isRunAheadFrame = true;

	// Minimal 32 KiB ROM-only cartridge; only the CPU and memory manager are used.
	vector<uint8_t> rom(0x8000, 0);
	memcpy(rom.data() + 0x134, "SINGLESTEP", 10);
	Gameboy gb(&emu);
	VirtualFile romFile(rom.data(), rom.size(), "singlestep.gb");
	if(gb.LoadRom(romFile) != LoadRomResult::Success) {
		std::cerr << "failed to set up Gameboy console\n";
		return 2;
	}
	Harness* h = new Harness();
	h->Gb = &gb;
	h->Mm = gb.GetMemoryManager();
	h->Cpu = gb.GetCpu();
	h->Dummy.Init(&emu, &gb, h->Mm);

	uint64_t total = 0, failed = 0, known = 0;
	for(const fs::path& f : files) {
		std::ifstream in(f);
		json tests = json::parse(in);
		int fail = 0, shown = 0;
		std::map<int, int> kindCounts;
		vector<string> details;
		for(const json& t : tests) {
			string detail;
			int k = h->Run(t, detail);
			total++;
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
		std::cout << "op " << op << ": " << (tests.size() - fail) << "/" << tests.size() << " pass";
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
				 << " known divergence\n";
	return failed ? 1 : 0;
}
