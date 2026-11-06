// singlestep.cpp - differential test of MesenCE's SNES main CPU (SnesCpu, 65816)
// against the SingleStepTests 65816 suite (https://github.com/SingleStepTests/65816).
//
// Each test gives the initial CPU + RAM state, the expected final state, and one
// entry per CPU cycle: [address, value, pins], pins = "dpvremxl" (VDA, VPA, VPB,
// r/w, E, M, X, MLB; '-' when inactive). A cycle with none of VDA/VPA/VPB is an
// internal operation (no valid address). The suite assumes a flat 16 MiB RAM.
// Native and emulation mode each have 10,000 tests per opcode ("xx.n.json",
// "xx.e.json").
//
// The harness compiles the real SnesCpu source (Core/SNES/SnesCpu.cpp, plus
// SnesCpu.Instructions.h and SnesCpu.Shared.h, which the SA-1 CPU shares) as
// "TestSnesCpu", with SnesMemoryManager replaced by TestBus (flat 16 MiB RAM) and
// SnesDmaController by TestDma (no transfers). So the code under test is the
// shipping instruction code. Checked:
//   - bus trace: the valid-address cycles (VDA/VPA/VPB) against TestBus's reads
//     and writes in order (address, value, direction); internal cycles must not
//     access the bus. Read values the suite leaves null are wildcards;
//   - cycle count (SnesCpuState::CycleCount, +1 per Read/Write/Idle);
//   - final registers (A, X, Y, S, D, DBR, PBR, P, E) and RAM.
//
// MVN/MVP: the suite runs the move to completion (the instruction re-executes
// until A wraps to $FFFF), so for those the harness calls Exec() until PC leaves
// the instruction.
//
// Usage: singlestep [--details N] <file.json | dir> ...

#include "pch.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <nlohmann/json.hpp>
#include "Shared/Emulator.h"
#include "Shared/CheatManager.h"
#include "Shared/MemoryOperationType.h"
#include "Shared/EventType.h"
#include "Utilities/Serializer.h"
#include "Utilities/FolderUtilities.h"
#include "SNES/SnesCpuTypes.h"
#include "SNES/SnesConsole.h"
#include "SNES/SnesMemoryManager.h"
#include "SNES/SnesDmaController.h"
#include "SNES/SnesCpu.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

struct BusOp
{
	bool Write;
	uint32_t Addr;
	uint8_t Value;
};

// Stands in for SnesMemoryManager: the members SnesCpu.cpp calls.
class TestBus
{
public:
	uint8_t* Ram = new uint8_t[0x1000000]();
	vector<uint32_t> Touched;
	vector<BusOp> Ops;

	void Poke(uint32_t addr, uint8_t value)
	{
		Ram[addr & 0xFFFFFF] = value;
		Touched.push_back(addr & 0xFFFFFF);
	}
	void Clear()
	{
		for(uint32_t a : Touched) {
			Ram[a] = 0;
		}
		Touched.clear();
		Ops.clear();
	}

	uint8_t Read(uint32_t addr, MemoryOperationType)
	{
		addr &= 0xFFFFFF;
		Ops.push_back({ false, addr, Ram[addr] });
		return Ram[addr];
	}
	void Write(uint32_t addr, uint8_t value, MemoryOperationType)
	{
		addr &= 0xFFFFFF;
		Ops.push_back({ true, addr, value });
		Poke(addr, value);
	}
	uint16_t PeekWord(uint32_t addr) { return Ram[addr & 0xFFFFFF] | (Ram[(addr + 1) & 0xFFFFFF] << 8); }
	uint8_t GetCpuSpeed(uint32_t) { return 8; }
	void SetCpuSpeed(uint8_t) {}
	void IncMasterClock4() {}
	void IncMasterClock6() {}
};

// Stands in for SnesDmaController: never has a pending transfer.
class TestDma
{
public:
	bool HasPendingTransfer() { return false; }
	bool ProcessPendingTransfers() { return false; }
};

// Stands in for SnesConsole in SnesCpu's constructor.
class Emulator;
class TestConsole
{
public:
	Emulator* Emu;
	TestBus* Bus;
	TestDma* Dma;
	Emulator* GetEmulator() { return Emu; }
	TestBus* GetMemoryManager() { return Bus; }
	TestDma* GetDmaController() { return Dma; }
};

// Compile the real SnesCpu against TestBus/TestDma. All headers SnesCpu.cpp
// includes were included above (they are guarded), so the renames only affect
// SnesCpu itself.
#undef __CPU__H
#define SnesCpu TestSnesCpu
#define SnesMemoryManager TestBus
#define SnesDmaController TestDma
#define SnesConsole TestConsole
#define private public
#include "SNES/SnesCpu.h"
#undef private
#include "SNES/SnesCpu.cpp"
#undef SnesConsole
#undef SnesDmaController
#undef SnesMemoryManager
#undef SnesCpu

static string Hex(uint32_t v, int width)
{
	std::ostringstream ss;
	ss << std::hex << std::uppercase << std::setw(width) << std::setfill('0') << v;
	return ss.str();
}

static string OpStr(const BusOp& op)
{
	return string(op.Write ? " W" : " R") + Hex(op.Addr, 6) + "=" + Hex(op.Value, 2);
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
	TestBus Bus;
	TestDma Dma;
	TestSnesCpu* Cpu = nullptr;

	static void LoadRegs(SnesCpuState& st, const json& in)
	{
		st = {};
		st.PC = in["pc"].get<uint16_t>();
		st.SP = in["s"].get<uint16_t>();
		st.PS = in["p"].get<uint8_t>();
		st.A = in["a"].get<uint16_t>();
		st.X = in["x"].get<uint16_t>();
		st.Y = in["y"].get<uint16_t>();
		st.DBR = in["dbr"].get<uint8_t>();
		st.D = in["d"].get<uint16_t>();
		st.K = in["pbr"].get<uint8_t>();
		st.EmulationMode = in["e"].get<int>() != 0;
		if(st.EmulationMode) {
			// The suite's emulation-mode tests start with a random S high byte. The CPU
			// holds it at $01 in emulation mode (and so does SnesCpu::SetSP), so the
			// expected results all assume $01xx.
			st.SP = 0x100 | (st.SP & 0xFF);
		}
		st.StopState = SnesCpuStopState::Running;
	}

	int Run(const json& t, bool isBlockMove, string& detail)
	{
		int kinds = 0;
		std::ostringstream d;
		const json& in = t["initial"];
		const json& fin = t["final"];
		const json& cycles = t["cycles"];

		Bus.Clear();
		for(const auto& e : in["ram"]) {
			Bus.Poke(e[0].get<uint32_t>(), e[1].get<uint8_t>());
		}
		SnesCpuState& st = Cpu->_state;
		LoadRegs(st, in);
		Cpu->_waiOver = true;

		uint16_t startPc = st.PC;
		uint8_t startK = st.K;
		Cpu->Exec();
		if(isBlockMove) {
			while(st.PC == startPc && st.K == startK && st.CycleCount < cycles.size() + 8) {
				Cpu->Exec();
			}
		}

		// Bus trace
		string actualTrace, expectedTrace;
		for(const BusOp& op : Bus.Ops) {
			actualTrace += OpStr(op);
		}
		bool busOk = true;
		size_t opIndex = 0;
		for(const auto& c : cycles) {
			string pins = c[2].get<string>();
			// Emulation-mode read-modify-write: the modify cycle drives RWB low with the
			// unmodified value but VDA/VPA low. MesenCE performs it as a write
			// (upstream ef64e869), so a write strobe counts as a bus access.
			bool valid = pins[0] == 'd' || pins[1] == 'p' || pins[2] == 'v' || pins[3] == 'w';
			if(!valid) {
				expectedTrace += " --";
				continue;
			}
			bool w = pins[3] == 'w';
			expectedTrace += (w ? " W" : " R") + (c[0].is_null() ? string("??????") : Hex(c[0].get<uint32_t>(), 6)) + "=" +
				(c[1].is_null() ? string("??") : Hex(c[1].get<uint8_t>(), 2));
			if(opIndex >= Bus.Ops.size()) {
				busOk = false;
				continue;
			}
			const BusOp& op = Bus.Ops[opIndex++];
			if(op.Write != w || (!c[0].is_null() && op.Addr != c[0].get<uint32_t>()) ||
				(!c[1].is_null() && op.Value != c[1].get<uint8_t>())) {
				busOk = false;
			}
		}
		if(opIndex != Bus.Ops.size() || !busOk) {
			kinds |= FailBus;
		}

		if(st.CycleCount != cycles.size()) {
			kinds |= FailCycleCount;
			d << " cycles=" << st.CycleCount << "(exp " << cycles.size() << ")";
		}

		auto chk = [&](const char* name, uint32_t actual, uint32_t expected, int width) {
			if(actual != expected) {
				kinds |= FailRegs;
				d << " " << name << "=" << Hex(actual, width) << "(exp " << Hex(expected, width) << ")";
			}
		};
		chk("PC", st.PC, fin["pc"].get<uint16_t>(), 4);
		chk("A", st.A, fin["a"].get<uint16_t>(), 4);
		chk("X", st.X, fin["x"].get<uint16_t>(), 4);
		chk("Y", st.Y, fin["y"].get<uint16_t>(), 4);
		chk("S", st.SP, fin["s"].get<uint16_t>(), 4);
		chk("D", st.D, fin["d"].get<uint16_t>(), 4);
		chk("DBR", st.DBR, fin["dbr"].get<uint8_t>(), 2);
		chk("PBR", st.K, fin["pbr"].get<uint8_t>(), 2);
		chk("P", st.PS, fin["p"].get<uint8_t>(), 2);
		chk("E", st.EmulationMode ? 1 : 0, fin["e"].get<int>(), 1);
		for(const auto& e : fin["ram"]) {
			uint32_t a = e[0].get<uint32_t>();
			uint8_t v = e[1].get<uint8_t>();
			if(Bus.Ram[a] != v) {
				kinds |= FailRam;
				d << " [" << Hex(a, 6) << "]=" << Hex(Bus.Ram[a], 2) << "(exp " << Hex(v, 2) << ")";
			}
		}
		if(kinds & FailBus) {
			d << "\n      actual bus:  " << actualTrace << "\n      expected bus:" << expectedTrace;
		}
		detail = t["name"].get<string>() + ":" + d.str();
		return kinds;
	}
};

// Opcode files where MesenCE deliberately differs from the suite. They are reported
// but do not count as failures. See docs/research/snes-verification-findings.md.
static const std::map<string, string> KnownDivergences = {
	{ "42.n", "WDM: MesenCE reads the signature byte (VPA); the suite has an internal cycle. ares also fetches it" },
	{ "42.e", "WDM: same as 42.n" },
	{ "cb.n", "WAI: MesenCE spends the 2 internal cycles in the halted state; the suite counts them in the instruction" },
	{ "cb.e", "WAI: same as cb.n" },
	{ "db.n", "STP: same as WAI; only reset leaves this state" },
	{ "db.e", "STP: same as db.n" },
};

// (dp,X) in emulation mode with DL != 0, when the pointer's low byte is at $xxFF:
// MesenCE (upstream c0ea1245) and ares wrap the high-byte fetch within the page; the
// suite doesn't. See the findings log. Only these tests are excluded.
static bool IsDirIdxIndXWrapCase(const string& opcode, const json& t)
{
	static const char* ops[] = { "01", "21", "41", "61", "81", "a1", "c1", "e1" };
	if(std::find(std::begin(ops), std::end(ops), opcode) == std::end(ops)) {
		return false;
	}
	const json& in = t["initial"];
	uint16_t d = in["d"].get<uint16_t>();
	if(!in["e"].get<int>() || (d & 0xFF) == 0) {
		return false;
	}
	// Operand byte is at PBR:PC+1
	uint32_t opAddr = (in["pbr"].get<uint32_t>() << 16) | (uint16_t)(in["pc"].get<uint16_t>() + 1);
	uint8_t operand = 0;
	for(const auto& e : in["ram"]) {
		if(e[0].get<uint32_t>() == opAddr) {
			operand = e[1].get<uint8_t>();
		}
	}
	uint16_t hiAddr = (uint16_t)(d + operand + (in["x"].get<uint16_t>() & 0xFF) + 1);
	return (hiAddr & 0xFF) == 0;
}

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

	fs::path home = fs::temp_directory_path() / "mesen-snes-singlestep-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);

	Harness* h = new Harness();
	TestConsole console = { &emu, &h->Bus, &h->Dma };
	h->Cpu = new TestSnesCpu(&console);

	uint64_t total = 0, failed = 0, known = 0;
	int failedFiles = 0;
	for(const fs::path& f : files) {
		std::ifstream in(f);
		json tests = json::parse(in);
		string op = f.stem().string(); // e.g. "a9.n"
		string opcode = op.substr(0, 2);
		bool isBlockMove = opcode == "44" || opcode == "54";
		int fail = 0, shown = 0, knownCases = 0;
		std::map<int, int> kindCounts;
		vector<string> details;
		for(const json& t : tests) {
			string detail;
			int k = h->Run(t, isBlockMove, detail);
			total++;
			if(k && IsDirIdxIndXWrapCase(opcode, t)) {
				knownCases++;
				continue;
			}
			if(k) {
				fail++;
				kindCounts[k]++;
				if(shown++ < maxDetails) {
					details.push_back(detail);
				}
			}
		}
		auto knownIt = KnownDivergences.find(op);
		bool isKnown = knownIt != KnownDivergences.end();
		(isKnown ? known : failed) += fail;
		known += knownCases;
		if(knownCases) {
			std::cout << "op " << op << ": " << knownCases
						 << " KNOWN DIVERGENCE: (dp,X) in emulation mode with DL != 0, pointer high byte at a page start\n";
		}
		if(!fail) {
			continue;
		}
		if(!isKnown) {
			failedFiles++;
		}
		std::cout << "op " << op << ": " << (tests.size() - fail - knownCases) << "/" << (tests.size() - knownCases) << " pass";
		if(isKnown) {
			std::cout << "  KNOWN DIVERGENCE: " << knownIt->second;
		}
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
		std::cout << ")\n";
		if(!isKnown) {
			for(const string& s : details) {
				std::cout << "    " << s << "\n";
			}
		}
	}
	std::cout << "TOTAL: " << (total - failed - known) << "/" << total << " pass, " << failed << " unexpected fail ("
				 << failedFiles << " of " << files.size() << " files), " << known << " known divergence\n";
	return failed ? 1 : 0;
}
