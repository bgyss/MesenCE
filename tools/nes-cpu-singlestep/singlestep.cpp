// singlestep.cpp - differential test of MesenCE's NES CPU (NesCpu) against the
// SingleStepTests 65x02 "nes6502" suite (https://github.com/SingleStepTests/65x02).
//
// Each test gives an initial CPU + RAM state, the expected final state, and the
// expected bus operation for every cycle of one instruction.  The suite assumes
// a flat 64 KiB RAM, so a flat-RAM handler is mapped over the whole CPU address
// space of a real NesConsole, and the instruction is executed by DummyNesCpu:
// the same NesCpu source compiled with DUMMYCPU (what the debugger uses to
// predict accesses).  Its reads go through the memory handlers, and it records
// every bus operation in order.  It does not apply writes, so the harness
// applies them after the instruction; a read of an address written earlier in
// the same instruction is reported as a harness limitation, not an emulator
// mismatch.
//
// Usage: singlestep [--details N] [--json out.json] <file.json | dir> ...

#include "pch.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "Shared/Emulator.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/NesMemoryManager.h"
#include "NES/INesMemoryHandler.h"
#include "NES/Debugger/DummyNesCpu.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/FolderUtilities.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

class FlatRam : public INesMemoryHandler
{
public:
	uint8_t Ram[0x10000] = {};
	bool Listed[0x10000] = {};
	vector<uint16_t> ListedAddrs;
	vector<uint16_t> UnlistedAccesses;

	void GetMemoryRanges(MemoryRanges& ranges) override {}
	uint8_t ReadRam(uint16_t addr) override { return PeekRam(addr); }
	void WriteRam(uint16_t addr, uint8_t value) override { Ram[addr] = value; }
	uint8_t PeekRam(uint16_t addr) override
	{
		if(!Listed[addr]) {
			UnlistedAccesses.push_back(addr);
		}
		return Ram[addr];
	}

	void Load(const json& ramList)
	{
		for(uint16_t a : ListedAddrs) {
			Listed[a] = false;
			Ram[a] = 0;
		}
		ListedAddrs.clear();
		UnlistedAccesses.clear();
		for(const auto& e : ramList) {
			uint16_t a = e[0].get<uint16_t>();
			Ram[a] = e[1].get<uint8_t>();
			Listed[a] = true;
			ListedAddrs.push_back(a);
		}
	}
};

static bool IsWrite(MemoryOperationType t)
{
	return t == MemoryOperationType::Write || t == MemoryOperationType::DummyWrite || t == MemoryOperationType::DmaWrite;
}

static string Hex(uint32_t v, int width)
{
	std::ostringstream ss;
	ss << std::hex << std::uppercase << std::setw(width) << std::setfill('0') << v;
	return ss.str();
}

enum FailKind
{
	FailRegs = 1,
	FailRam = 2,
	FailCycleCount = 4,
	FailCycleContent = 8,
	FailUnlisted = 16,
	HarnessReadAfterWrite = 32,
};

struct Result
{
	int Kinds = 0;
	string Detail;
};

static Result RunTest(const json& t, NesCpu* cpu, DummyNesCpu& dummy, FlatRam& flat)
{
	Result r;
	std::ostringstream d;
	const json& in = t["initial"];
	const json& fin = t["final"];

	flat.Load(in["ram"]);

	NesCpuState& st = cpu->GetState();
	st.PC = in["pc"].get<uint16_t>();
	st.SP = in["s"].get<uint8_t>();
	st.A = in["a"].get<uint8_t>();
	st.X = in["x"].get<uint8_t>();
	st.Y = in["y"].get<uint8_t>();
	st.PS = in["p"].get<uint8_t>();
	st.IrqFlag = 0;
	st.NmiFlag = false;

	dummy.SetDummyState(cpu);
	dummy.Exec();
	NesCpuState out = dummy.GetState();

	// Replay the bus trace: check reads/writes against the expected cycles,
	// apply writes to RAM in order, and detect read-after-write inside the instruction.
	uint32_t opCount = dummy.GetOperationCount();
	const json& cycles = t["cycles"];
	std::map<uint16_t, uint8_t> writtenSoFar;
	string actualTrace, expectedTrace;
	for(uint32_t i = 0; i < opCount; i++) {
		MemoryOperationInfo op = dummy.GetOperationInfo(i);
		bool w = IsWrite(op.Type);
		uint16_t addr = (uint16_t)op.Address;
		uint8_t value = (uint8_t)op.Value;
		if(!w && writtenSoFar.count(addr)) {
			r.Kinds |= HarnessReadAfterWrite;
			value = writtenSoFar[addr];
		}
		actualTrace += (w ? " W" : " R") + Hex(addr, 4) + "=" + Hex(value, 2);
		if(w) {
			flat.Ram[addr] = value;
			writtenSoFar[addr] = value;
		}
		if(i < cycles.size()) {
			const json& c = cycles[i];
			bool expW = c[2].get<string>() == "write";
			if(c[0].get<uint16_t>() != addr || c[1].get<uint8_t>() != value || expW != w) {
				r.Kinds |= FailCycleContent;
			}
		}
	}
	for(const auto& c : cycles) {
		expectedTrace += (c[2].get<string>() == "write" ? " W" : " R") + Hex(c[0].get<uint16_t>(), 4) + "=" + Hex(c[1].get<uint8_t>(), 2);
	}
	if(opCount != cycles.size()) {
		r.Kinds |= FailCycleCount;
	}

	auto chk = [&](const char* name, uint32_t actual, uint32_t expected, int width) {
		if(actual != expected) {
			r.Kinds |= FailRegs;
			d << " " << name << "=" << Hex(actual, width) << "(exp " << Hex(expected, width) << ")";
		}
	};
	chk("PC", out.PC, fin["pc"].get<uint16_t>(), 4);
	chk("S", out.SP, fin["s"].get<uint8_t>(), 2);
	chk("A", out.A, fin["a"].get<uint8_t>(), 2);
	chk("X", out.X, fin["x"].get<uint8_t>(), 2);
	chk("Y", out.Y, fin["y"].get<uint8_t>(), 2);
	// P bits 4 (B) and 5 (unused) do not exist in the 6502's flag register; they only
	// appear in the byte pushed by PHP/BRK/IRQ (which the bus trace checks). MesenCE
	// stores P with both cleared (SetPS masks with 0xCF), so compare without them.
	chk("P", out.PS & 0xCF, fin["p"].get<uint8_t>() & 0xCF, 2);

	for(const auto& e : fin["ram"]) {
		uint16_t a = e[0].get<uint16_t>();
		uint8_t v = e[1].get<uint8_t>();
		if(flat.Ram[a] != v) {
			r.Kinds |= FailRam;
			d << " [" << Hex(a, 4) << "]=" << Hex(flat.Ram[a], 2) << "(exp " << Hex(v, 2) << ")";
		}
	}
	if(!flat.UnlistedAccesses.empty()) {
		r.Kinds |= FailUnlisted;
		d << " unlisted-read=" << Hex(flat.UnlistedAccesses[0], 4);
	}

	if(r.Kinds & (FailCycleCount | FailCycleContent)) {
		d << "\n      actual:  " << actualTrace << "\n      expected:" << expectedTrace;
	}
	r.Detail = t["name"].get<string>() + ":" + d.str();
	return r;
}

// Opcodes where MesenCE deliberately differs from the suite. These are reported,
// but they do not count as failures. See docs/research/nes-verification-findings.md.
static const std::map<string, string> KnownDivergences = {
	{ "02", "STP/JAM: MesenCE freezes on the opcode (PC-=1); the suite models the jammed bus" },
	{ "12", "STP/JAM" },
	{ "22", "STP/JAM" },
	{ "32", "STP/JAM" },
	{ "42", "STP/JAM" },
	{ "52", "STP/JAM" },
	{ "62", "STP/JAM" },
	{ "72", "STP/JAM" },
	{ "92", "STP/JAM" },
	{ "b2", "STP/JAM" },
	{ "d2", "STP/JAM" },
	{ "f2", "STP/JAM" },
	{ "ab", "LXA #imm (unstable): MesenCE uses A=X=imm (magic $FF); the suite uses (A|$EE)&imm" },
};

int main(int argc, char* argv[])
{
	int maxDetails = 3;
	string jsonOut;
	vector<fs::path> files;
	for(int i = 1; i < argc; i++) {
		string a = argv[i];
		if(a == "--details" && i + 1 < argc) {
			maxDetails = std::stoi(argv[++i]);
		} else if(a == "--json" && i + 1 < argc) {
			jsonOut = argv[++i];
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
		std::cerr << "usage: singlestep [--details N] [--json out.json] <file.json | dir> ...\n";
		return 2;
	}

	// A real NesConsole around a blank NROM image; only its memory manager and CPU are used.
	fs::path home = fs::temp_directory_path() / "mesen-singlestep-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);
	vector<uint8_t> rom(16 + 0x4000 + 0x2000, 0);
	memcpy(rom.data(), "NES\x1A\x01\x01", 6);
	VirtualFile romFile(rom.data(), rom.size(), "singlestep.nes");
	NesConsole console(&emu);
	if(console.LoadRom(romFile) != LoadRomResult::Success) {
		std::cerr << "failed to set up NesConsole\n";
		return 2;
	}
	FlatRam* flat = new FlatRam();
	console.GetMemoryManager()->RegisterReadHandler(flat, 0, 0xFFFF);
	console.GetMemoryManager()->RegisterWriteHandler(flat, 0, 0xFFFF);
	NesCpu* cpu = console.GetCpu();
	DummyNesCpu dummy(&console);

	json report = json::object();
	uint64_t total = 0, failed = 0, known = 0, harnessOnly = 0;
	for(const fs::path& f : files) {
		std::ifstream in(f);
		json tests = json::parse(in);
		int fail = 0, kinds = 0, shown = 0, rawOnly = 0;
		std::map<int, int> kindCounts;
		vector<string> details;
		for(const json& t : tests) {
			Result r = RunTest(t, cpu, dummy, *flat);
			total++;
			int emuKinds = r.Kinds & ~HarnessReadAfterWrite;
			if(emuKinds) {
				fail++;
				kinds |= emuKinds;
				kindCounts[emuKinds]++;
				if(r.Kinds & HarnessReadAfterWrite) {
					rawOnly++;
				}
				if(shown < maxDetails) {
					details.push_back(r.Detail);
					shown++;
				}
			} else if(r.Kinds & HarnessReadAfterWrite) {
				harnessOnly++;
			}
		}
		string op = f.stem().string();
		auto knownIt = KnownDivergences.find(op);
		bool isKnown = knownIt != KnownDivergences.end();
		if(isKnown) {
			known += fail;
		} else {
			failed += fail;
		}
		std::cout << "op " << op << ": " << (tests.size() - fail) << "/" << tests.size() << " pass";
		if(fail && isKnown) {
			std::cout << "  KNOWN DIVERGENCE: " << knownIt->second;
		}
		if(fail) {
			std::cout << "  (";
			bool first = true;
			for(auto& [k, n] : kindCounts) {
				std::cout << (first ? "" : ", ") << n << "x";
				if(k & FailRegs) {
					std::cout << " regs";
				}
				if(k & FailRam) {
					std::cout << " ram";
				}
				if(k & FailCycleCount) {
					std::cout << " cycle-count";
				}
				if(k & FailCycleContent) {
					std::cout << " bus";
				}
				if(k & FailUnlisted) {
					std::cout << " unlisted";
				}
				first = false;
			}
			std::cout << ")";
			if(rawOnly) {
				std::cout << " [" << rawOnly << " also read-after-write]";
			}
		}
		std::cout << "\n";
		if(!isKnown) {
			for(const string& s : details) {
				std::cout << "    " << s << "\n";
			}
		}
		report[op] = { { "tests", tests.size() }, { "failed", fail }, { "known", isKnown }, { "details", details } };
	}
	std::cout << "TOTAL: " << (total - failed - known) << "/" << total << " pass, " << failed << " unexpected fail, " << known
				 << " known divergence";
	if(harnessOnly) {
		std::cout << " (" << harnessOnly << " passing tests needed read-after-write replay)";
	}
	std::cout << "\n";
	if(!jsonOut.empty()) {
		std::ofstream(jsonOut) << report.dump(1);
	}
	return failed ? 1 : 0;
}
