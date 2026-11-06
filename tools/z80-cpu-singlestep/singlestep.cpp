// singlestep.cpp - differential test of MesenCE's Z80 (SmsCpu: SMS, Game Gear,
// SG-1000, ColecoVision) against the SingleStepTests z80 suite
// (https://github.com/SingleStepTests/z80).
//
// Each test gives the initial CPU + RAM state, the expected final state, one entry
// per T-state ([address, data, pins]; pins "rwmi" = RD, WR, MREQ, IORQ), and the
// I/O transactions ("ports").  The suite assumes a flat 64 KiB RAM.
//
// The harness compiles the real SmsCpu source (Core/SMS/SmsCpu.cpp, without
// DUMMYCPU) as "TestSmsCpu", with SmsMemoryManager replaced by TestBus: a flat
// 64 KiB RAM plus the test's port values. So the code under test is the shipping
// instruction code; only the memory manager behind it differs.  Checked:
//   - bus trace: memory and I/O accesses in order (address, value, direction).
//     SmsCpu passes 8-bit port numbers, so I/O addresses compare the low byte;
//   - T-state count (SmsCpuState::CycleCount);
//   - final registers (incl. WZ/MEMPTR, R, IFF1/2, IM, and Q via FlagsChanged)
//     and RAM.
// Not checked: "p" and "ei" (only used by interrupt acceptance, which a single
// instruction doesn't exercise).
//
// Usage: singlestep [--details N] <file.json | dir> ...

#include "pch.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <nlohmann/json.hpp>
#include "TestSmsCpu.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

static string Hex(uint32_t v, int width)
{
	std::ostringstream ss;
	ss << std::hex << std::uppercase << std::setw(width) << std::setfill('0') << v;
	return ss.str();
}

static string OpStr(const BusOp& op)
{
	return string(" ") + (op.Io ? (op.Write ? "OUT" : "IN") : (op.Write ? "W" : "R")) + Hex(op.Addr, op.Io ? 2 : 4) + "=" + Hex(op.Value, 2);
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
	TestSmsCpu Cpu;
	Emulator* Emu = nullptr;

	void LoadRegs(SmsCpuState& st, const json& in)
	{
		auto pair = [](uint8_t& hi, uint8_t& lo, uint16_t v) {
			hi = v >> 8;
			lo = (uint8_t)v;
		};
		st = {};
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
		st.I = in["i"].get<uint8_t>();
		st.R = in["r"].get<uint8_t>();
		pair(st.IXH, st.IXL, in["ix"].get<uint16_t>());
		pair(st.IYH, st.IYL, in["iy"].get<uint16_t>());
		pair(st.AltA, st.AltFlags, in["af_"].get<uint16_t>());
		pair(st.AltB, st.AltC, in["bc_"].get<uint16_t>());
		pair(st.AltD, st.AltE, in["de_"].get<uint16_t>());
		pair(st.AltH, st.AltL, in["hl_"].get<uint16_t>());
		st.WZ = in["wz"].get<uint16_t>();
		st.IM = in["im"].get<uint8_t>();
		st.IFF1 = in["iff1"].get<int>() != 0;
		st.IFF2 = in["iff2"].get<int>() != 0;
		// q is F if the previous instruction changed the flags, else 0. SmsCpu keeps a
		// shift register instead; Exec() shifts it left before the instruction runs.
		st.FlagsChanged = in["q"].get<uint8_t>() != 0 ? 1 : 0;
	}

	int Run(const json& t, string& detail)
	{
		int kinds = 0;
		std::ostringstream d;
		const json& in = t["initial"];
		const json& fin = t["final"];
		const json& cycles = t["cycles"];

		memset(Bus.Ram, 0, sizeof(Bus.Ram));
		for(const auto& e : in["ram"]) {
			Bus.Ram[e[0].get<uint16_t>()] = e[1].get<uint8_t>();
		}
		Bus.Ops.clear();
		Bus.PortReads.clear();
		Bus.PortReadIndex = 0;
		if(t.contains("ports")) {
			for(const auto& p : t["ports"]) {
				if(p[2].get<string>() == "r") {
					Bus.PortReads.push_back(p[1].get<uint8_t>());
				}
			}
		}

		SmsCpuState& st = Cpu._state;
		LoadRegs(st, in);
		Cpu.Exec();

		// Expected accesses: RD/WR together with MREQ or IORQ. Read data is on the bus
		// in the following T-state; write data in the same one.
		vector<BusOp> expected;
		for(size_t i = 0; i < cycles.size(); i++) {
			string pins = cycles[i][2].get<string>();
			bool rd = pins[0] == 'r', wr = pins[1] == 'w', mreq = pins[2] == 'm', iorq = pins[3] == 'i';
			if(!(rd || wr) || !(mreq || iorq)) {
				continue;
			}
			const json& dataEntry = wr ? cycles[i] : (i + 1 < cycles.size() ? cycles[i + 1] : cycles[i]);
			uint16_t addr = cycles[i][0].get<uint16_t>();
			expected.push_back({ iorq, wr, (uint16_t)(iorq ? (addr & 0xFF) : addr), dataEntry[1].is_null() ? (uint8_t)0 : dataEntry[1].get<uint8_t>() });
		}
		string actualTrace, expectedTrace;
		for(const BusOp& op : Bus.Ops) {
			actualTrace += OpStr(op);
		}
		for(const BusOp& op : expected) {
			expectedTrace += OpStr(op);
		}
		if(actualTrace != expectedTrace) {
			kinds |= FailBus;
		}

		if(st.CycleCount != cycles.size()) {
			kinds |= FailCycleCount;
			d << " T=" << st.CycleCount << "(exp " << cycles.size() << ")";
		}

		auto chk = [&](const char* name, uint32_t actual, uint32_t expectedValue, int width) {
			if(actual != expectedValue) {
				kinds |= FailRegs;
				d << " " << name << "=" << Hex(actual, width) << "(exp " << Hex(expectedValue, width) << ")";
			}
		};
		auto w = [](uint8_t hi, uint8_t lo) { return (uint32_t)((hi << 8) | lo); };
		chk("PC", st.PC, fin["pc"].get<uint16_t>(), 4);
		chk("SP", st.SP, fin["sp"].get<uint16_t>(), 4);
		chk("A", st.A, fin["a"].get<uint8_t>(), 2);
		chk("F", st.Flags, fin["f"].get<uint8_t>(), 2);
		chk("B", st.B, fin["b"].get<uint8_t>(), 2);
		chk("C", st.C, fin["c"].get<uint8_t>(), 2);
		chk("D", st.D, fin["d"].get<uint8_t>(), 2);
		chk("E", st.E, fin["e"].get<uint8_t>(), 2);
		chk("H", st.H, fin["h"].get<uint8_t>(), 2);
		chk("L", st.L, fin["l"].get<uint8_t>(), 2);
		chk("I", st.I, fin["i"].get<uint8_t>(), 2);
		chk("R", st.R, fin["r"].get<uint8_t>(), 2);
		chk("IX", w(st.IXH, st.IXL), fin["ix"].get<uint16_t>(), 4);
		chk("IY", w(st.IYH, st.IYL), fin["iy"].get<uint16_t>(), 4);
		chk("AF'", w(st.AltA, st.AltFlags), fin["af_"].get<uint16_t>(), 4);
		chk("BC'", w(st.AltB, st.AltC), fin["bc_"].get<uint16_t>(), 4);
		chk("DE'", w(st.AltD, st.AltE), fin["de_"].get<uint16_t>(), 4);
		chk("HL'", w(st.AltH, st.AltL), fin["hl_"].get<uint16_t>(), 4);
		chk("WZ", st.WZ, fin["wz"].get<uint16_t>(), 4);
		chk("IM", st.IM, fin["im"].get<uint8_t>(), 1);
		chk("IFF1", st.IFF1, fin["iff1"].get<int>(), 1);
		chk("IFF2", st.IFF2, fin["iff2"].get<int>(), 1);
		chk("Q", (st.FlagsChanged & 1) ? st.Flags : 0, fin["q"].get<uint8_t>(), 2);
		for(const auto& e : fin["ram"]) {
			uint16_t a = e[0].get<uint16_t>();
			uint8_t v = e[1].get<uint8_t>();
			if(Bus.Ram[a] != v) {
				kinds |= FailRam;
				d << " [" << Hex(a, 4) << "]=" << Hex(Bus.Ram[a], 2) << "(exp " << Hex(v, 2) << ")";
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
// do not count as failures. See docs/research/sms-verification-findings.md.
static const std::map<string, string> KnownDivergences = {};

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

	fs::path home = fs::temp_directory_path() / "mesen-z80-singlestep-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);

	Harness* h = new Harness();
	h->Emu = &emu;
	h->Cpu._emu = &emu;
	h->Cpu._memoryManager = &h->Bus;

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
		if(fail == 0) {
			continue; // 1604 files: only print the ones that need attention
		}
		std::cout << "op " << op << ": " << (tests.size() - fail) << "/" << tests.size() << " pass";
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
	std::cout << "TOTAL: " << (total - failed - known) << "/" << total << " pass (" << files.size() << " opcode files), " << failed
				 << " unexpected fail, " << known << " known divergence\n";
	return failed ? 1 : 0;
}
