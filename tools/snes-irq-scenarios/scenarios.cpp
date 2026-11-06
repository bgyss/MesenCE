// scenarios.cpp - interrupt conformance checks for MesenCE's 65816 cores: the SNES
// main CPU (SnesCpu) and the SA-1 (Sa1Cpu).
//
// The SingleStepTests suite never takes an interrupt. This harness compiles the real
// CPU sources against a flat 16 MiB test bus (tools/snes-cpu-singlestep/TestCpus.h),
// raises IRQ/NMI through the CPUs' own entry points (SetIrqSource, SetNmiFlag), and
// checks each rule against its source:
//   WDC W65C816S datasheet: the interrupt sequence (8 cycles native, 7 emulation),
//     the pushed registers, I set / D cleared / PBR = 0, the emulation-mode B flag,
//     WAI with I = 1, and interruptible block moves;
//   ares (component/processor/wdc65816/instruction.cpp, interrupt()) as a second
//     source for the sequence and the pushed P.
// Every scenario runs on both CPUs. On the SA-1, NMI/IRQ vectors come from the SA-1
// registers (TestSa1::NmiVector/IrqVector), as on hardware.
//
// Usage: scenarios [--verbose]

#include "pch.h"
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include "../snes-cpu-singlestep/TestCpus.h"

namespace fs = std::filesystem;

// Handler addresses (bank 0) written into the vectors
static constexpr uint16_t NativeIrq = 0x9000, NativeNmi = 0x9100, NativeBrk = 0x9200, EmuIrqBrk = 0x9300, EmuNmi = 0x9400;
static constexpr uint32_t Prog = 0x008000;

enum Op : uint8_t
{
	NOP = 0xEA,
	WAI = 0xCB,
	RTI = 0x40,
	MVN = 0x54,
	BRK = 0x00,
	LDA_IMM = 0xA9,
};

static string Hex(uint32_t v, int width)
{
	std::ostringstream ss;
	ss << "$" << std::hex << std::uppercase << std::setw(width) << std::setfill('0') << v;
	return ss.str();
}

struct Machine
{
	TestBus Bus;
	TestSa1 Sa1;
	std::function<void()> Exec;
	std::function<SnesCpuState&()> State;
	std::function<void()> RaiseIrq;
	std::function<void()> ClearIrq;
	std::function<void()> RaiseNmi;
	std::function<void(bool)> SetWaiOver;

	void PokeWord(uint32_t addr, uint16_t v)
	{
		Bus.Poke(addr, (uint8_t)v);
		Bus.Poke(addr + 1, v >> 8);
	}

	// Native mode: SP $1FFF; emulation mode: SP $01FF, M/X set (as the CPU forces).
	void Reset(bool emulation, uint8_t ps)
	{
		Bus.Clear();
		PokeWord(0xFFEE, NativeIrq);
		PokeWord(0xFFEA, NativeNmi);
		PokeWord(0xFFE6, NativeBrk);
		PokeWord(0xFFFE, EmuIrqBrk);
		PokeWord(0xFFFA, EmuNmi);
		Sa1.IrqVector = NativeIrq;
		Sa1.NmiVector = NativeNmi;
		SnesCpuState& s = State();
		s = {};
		s.PC = (uint16_t)Prog;
		s.K = Prog >> 16;
		s.EmulationMode = emulation;
		s.SP = emulation ? 0x01FF : 0x1FFF;
		s.PS = emulation ? (ps | 0x30) : ps;
		s.StopState = SnesCpuStopState::Running;
		SetWaiOver(true);
		ClearIrq();
	}

	void Load(uint32_t addr, std::initializer_list<uint8_t> bytes)
	{
		for(uint8_t b : bytes) {
			Bus.Poke(addr++, b);
		}
	}

	uint64_t Step()
	{
		uint64_t start = State().CycleCount;
		Exec();
		return State().CycleCount - start;
	}

	uint8_t Peek(uint32_t addr) { return Bus.Ram[addr & 0xFFFFFF]; }
};

struct Scenario
{
	string Name;
	string Rule;
	std::function<string(Machine&)> Run; // "" if the documented behaviour holds, else what happened
	bool KnownBug = false;
};

// Checks the interrupt sequence that follows a NOP at Prog. Returns "" or a description.
static string CheckEntry(Machine& m, uint64_t cycles, bool emulation, bool nmi, uint16_t vectorAddr, uint16_t handler, uint8_t psBefore, uint16_t spBefore)
{
	std::ostringstream err;
	SnesCpuState& s = m.State();
	uint64_t expectedCycles = 2 + (emulation ? 7 : 8); // NOP + interrupt sequence
	if(cycles != expectedCycles) {
		err << " cycles=" << cycles << " (expected " << expectedCycles << ")";
	}
	if(s.PC != handler || s.K != 0) {
		err << " PC=" << Hex((s.K << 16) | s.PC, 6) << " (expected " << Hex(handler, 6) << ")";
	}
	if(!(s.PS & ProcFlags::IrqDisable) || (s.PS & ProcFlags::Decimal)) {
		err << " P=" << Hex(s.PS, 2) << " (expected I=1, D=0)";
	}
	uint16_t ret = (uint16_t)(Prog + 1);
	uint16_t sp = spBefore;
	if(!emulation) {
		if(m.Peek(sp) != (Prog >> 16)) {
			err << " pushed PBR=" << Hex(m.Peek(sp), 2);
		}
		sp--;
	}
	uint16_t pushedPc = (m.Peek(sp) << 8) | m.Peek((uint16_t)(sp - 1));
	if(pushedPc != ret) {
		err << " pushed PC=" << Hex(pushedPc, 4) << " (expected " << Hex(ret, 4) << ")";
	}
	uint8_t pushedP = m.Peek((uint16_t)(sp - 2));
	uint8_t expectedP = emulation ? ((psBefore | 0x20) & ~0x10) : psBefore; // emulation: B = 0 for IRQ/NMI
	if(pushedP != expectedP) {
		err << " pushed P=" << Hex(pushedP, 2) << " (expected " << Hex(expectedP, 2) << ")";
	}
	uint16_t expectedSp = emulation ? (0x100 | (uint8_t)(spBefore - 3)) : (uint16_t)(spBefore - 4);
	if(s.SP != expectedSp) {
		err << " S=" << Hex(s.SP, 4) << " (expected " << Hex(expectedSp, 4) << ")";
	}
	// The last two bus accesses are the vector reads (on the SA-1 the NMI/IRQ vectors
	// come from its registers instead, so only the SNES CPU's vector reads are checked).
	(void)vectorAddr;
	(void)nmi;
	return err.str();
}

static string CheckVectorReads(Machine& m, uint16_t vectorAddr)
{
	size_t n = m.Bus.Ops.size();
	if(n < 2 || m.Bus.Ops[n - 2].Write || m.Bus.Ops[n - 2].Addr != vectorAddr || m.Bus.Ops[n - 1].Addr != (uint32_t)(vectorAddr + 1)) {
		return " vector not read from " + Hex(vectorAddr, 6);
	}
	return "";
}

static vector<Scenario> Scenarios(bool isSa1)
{
	vector<Scenario> list;

	for(bool emulation : { false, true }) {
		string mode = emulation ? "emulation" : "native";
		uint16_t irqVec = emulation ? 0xFFFE : 0xFFEE;
		uint16_t irqHandler = emulation ? EmuIrqBrk : NativeIrq;
		list.push_back({ "IRQ entry, " + mode,
			string(emulation ? "7" : "8") + " cycles; pushes " + (emulation ? "" : "PBR, ") +
				"PC (next instruction), P" + (emulation ? " with B = 0" : "") + "; I = 1, D = 0, PBR = 0; PC = (" + Hex(irqVec, 4) +
				") (WDC datasheet; ares interrupt())",
			[=](Machine& m) {
				uint8_t ps = ProcFlags::Decimal | ProcFlags::Carry;
				m.Reset(emulation, ps);
				m.Load(Prog, { NOP, NOP });
				m.RaiseIrq();
				uint16_t sp = m.State().SP;
				uint64_t c = m.Step();
				string err = CheckEntry(m, c, emulation, false, irqVec, irqHandler, m.State().EmulationMode ? (ps | 0x30) : ps, sp);
				if(!isSa1 || emulation) {
					err += CheckVectorReads(m, irqVec);
				}
				return err;
			} });

		uint16_t nmiVec = emulation ? 0xFFFA : 0xFFEA;
		uint16_t nmiHandler = emulation ? EmuNmi : NativeNmi;
		list.push_back({ "NMI entry with I = 1, " + mode,
			"NMI is not masked by I; same sequence as IRQ; PC = (" + Hex(nmiVec, 4) + ") (WDC datasheet; ares interrupt())",
			[=](Machine& m) {
				uint8_t ps = ProcFlags::IrqDisable;
				m.Reset(emulation, ps);
				m.Load(Prog, { NOP, NOP });
				m.RaiseNmi();
				uint16_t sp = m.State().SP;
				uint64_t c = m.Step();
				string err = CheckEntry(m, c, emulation, true, nmiVec, nmiHandler, emulation ? (ps | 0x30) : ps, sp);
				if(!isSa1 || emulation) {
					err += CheckVectorReads(m, nmiVec);
				}
				return err;
			} });

		list.push_back({ "IRQ masked by I, " + mode, "With I = 1 a pending IRQ is not taken (WDC datasheet)", [=](Machine& m) {
								 m.Reset(emulation, ProcFlags::IrqDisable);
								 m.Load(Prog, { NOP, NOP });
								 m.RaiseIrq();
								 m.Step();
								 m.Step();
								 SnesCpuState& s = m.State();
								 return s.PC == (uint16_t)(Prog + 2) && s.K == 0 ? string() : " PC=" + Hex((s.K << 16) | s.PC, 6) + " (expected " + Hex(Prog + 2, 6) + ")";
							 } });

		list.push_back({ "WAI with I = 1, " + mode, "An IRQ ends WAI; with I = 1 execution resumes at the next instruction, without the interrupt sequence (WDC datasheet, WAI)", [=](Machine& m) {
								 m.Reset(emulation, ProcFlags::IrqDisable | ProcFlags::MemoryMode8); // 8-bit A for LDA #$42
								 m.Load(Prog, { WAI, LDA_IMM, 0x42, NOP });
								 uint16_t sp = m.State().SP;
								 m.Step(); // WAI
								 for(int i = 0; i < 4; i++) {
									 m.Step(); // waiting
								 }
								 m.RaiseIrq();
								 int guard = 0;
								 while(m.State().StopState != SnesCpuStopState::Running && guard++ < 16) {
									 m.Step();
								 }
								 m.ClearIrq();
								 m.Step(); // LDA #$42
								 SnesCpuState& s = m.State();
								 std::ostringstream err;
								 if(guard >= 16) {
									 err << " WAI never ended";
								 }
								 if((uint8_t)s.A != 0x42 || s.PC != (uint16_t)(Prog + 3)) {
									 err << " A=" << Hex(s.A & 0xFF, 2) << " PC=" << Hex(s.PC, 4) << " (expected the LDA after WAI to run)";
								 }
								 if(s.SP != sp) {
									 err << " S=" << Hex(s.SP, 4) << " (something was pushed)";
								 }
								 return err.str();
							 } });

		list.push_back({ "WAI with I = 0, " + mode, "An IRQ ends WAI and is taken; the pushed PC is the instruction after WAI (WDC datasheet, WAI)", [=](Machine& m) {
								 m.Reset(emulation, 0);
								 m.Load(Prog, { WAI, NOP });
								 m.Step(); // WAI
								 m.Step();
								 m.RaiseIrq();
								 int guard = 0;
								 while(m.State().PC != irqHandler && guard++ < 16) {
									 m.Step();
								 }
								 SnesCpuState& s = m.State();
								 uint16_t sp = emulation ? 0x01FF : 0x1FFF;
								 if(!emulation) {
									 sp--;
								 }
								 uint16_t pushedPc = (m.Peek(sp) << 8) | m.Peek((uint16_t)(sp - 1));
								 std::ostringstream err;
								 if(s.PC != irqHandler) {
									 err << " IRQ not taken";
								 } else if(pushedPc != (uint16_t)(Prog + 1)) {
									 err << " pushed PC=" << Hex(pushedPc, 4) << " (expected " << Hex((uint16_t)(Prog + 1), 4) << ")";
								 }
								 return err.str();
							 } });
	}

	list.push_back({ "MVN is interruptible, native", "An IRQ during a block move pushes the MVN's own address; after RTI the move resumes and completes (WDC datasheet, MVN/MVP)", [=](Machine& m) {
							 m.Reset(false, 0); // 16-bit index registers
							 m.State().A = 3; // move 4 bytes
							 m.State().X = 0x2000;
							 m.State().Y = 0x3000;
							 m.Load(Prog, { MVN, 0x00, 0x00, NOP });
							 m.Load(NativeIrq, { RTI });
							 m.Load(0x2000, { 0x11, 0x22, 0x33, 0x44 });
							 m.RaiseIrq();
							 m.Step(); // one byte moved, then the IRQ
							 SnesCpuState& s = m.State();
							 std::ostringstream err;
							 uint16_t pushedPc = (m.Peek(0x1FFE) << 8) | m.Peek(0x1FFD);
							 if(s.PC != NativeIrq) {
								 err << " IRQ not taken after the first byte (PC=" << Hex(s.PC, 4) << ")";
							 } else if(pushedPc != (uint16_t)Prog) {
								 err << " pushed PC=" << Hex(pushedPc, 4) << " (expected " << Hex((uint16_t)Prog, 4) << ", the MVN)";
							 }
							 m.ClearIrq();
							 int guard = 0;
							 while(!(m.State().PC == (uint16_t)(Prog + 3) && m.State().K == 0) && guard++ < 32) {
								 m.Step();
							 }
							 for(int i = 0; i < 4; i++) {
								 if(m.Peek(0x3000 + i) != 0x11 * (i + 1)) {
									 err << " [" << Hex(0x3000 + i, 4) << "]=" << Hex(m.Peek(0x3000 + i), 2);
								 }
							 }
							 if(m.State().A != 0xFFFF) {
								 err << " A=" << Hex(m.State().A, 4) << " (expected $FFFF)";
							 }
							 return err.str();
						 } });

	list.push_back({ "BRK in emulation mode pushes B = 1", "BRK pushes P with B (bit 4) = 1, so a handler on the shared $FFFE vector can tell it from an IRQ (WDC datasheet)", [=](Machine& m) {
							 m.Reset(true, 0);
							 m.Load(Prog, { BRK, 0x00 });
							 m.Step();
							 uint8_t pushedP = m.Peek(0x01FD);
							 return (pushedP & 0x10) ? string() : " pushed P=" + Hex(pushedP, 2) + " (B clear)";
						 } });

	return list;
}

template<typename CpuT>
static void Bind(Machine& m, CpuT* cpu)
{
	m.Exec = [cpu]() { cpu->Exec(); };
	m.State = [cpu]() -> SnesCpuState& { return cpu->_state; };
	m.RaiseIrq = [cpu]() { cpu->SetIrqSource(SnesIrqSource::Ppu); };
	m.ClearIrq = [cpu]() { cpu->ClearIrqSource(SnesIrqSource::Ppu); };
	m.RaiseNmi = [cpu]() { cpu->SetNmiFlag(1); };
	m.SetWaiOver = [cpu](bool v) { cpu->_waiOver = v; };
}

// Scenarios that fail because of a documented, not-yet-fixed MesenCE bug. Reported, but
// they don't fail the run. See docs/research/snes-verification-findings.md.
static const std::set<string> KnownBugs = {};

int main(int argc, char* argv[])
{
	bool verbose = argc > 1 && string(argv[1]) == "--verbose";

	fs::path home = fs::temp_directory_path() / "mesen-snes-irq-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);

	int failed = 0, known = 0, total = 0;
	for(bool isSa1 : { false, true }) {
		Machine* m = new Machine();
		TestDma dma;
		TestConsole console = { &emu, &m->Bus, &dma };
		m->Sa1.Bus = &m->Bus;
		if(isSa1) {
			Bind(*m, new TestSa1Cpu(&m->Sa1, &emu));
		} else {
			Bind(*m, new TestSnesCpu(&console));
		}
		string cpuName = isSa1 ? "SA-1" : "SNES";
		for(Scenario& sc : Scenarios(isSa1)) {
			string err = sc.Run(*m);
			total++;
			string key = cpuName + ": " + sc.Name;
			bool isKnown = KnownBugs.count(key) > 0;
			if(err.empty()) {
				std::cout << "PASS  " << key << "\n";
			} else if(isKnown) {
				known++;
				std::cout << "KNOWN " << key << ":" << err << "\n";
			} else {
				failed++;
				std::cout << "FAIL  " << key << ":" << err << "\n";
			}
			if(verbose || !err.empty()) {
				std::cout << "      rule: " << sc.Rule << "\n";
			}
		}
	}
	std::cout << "TOTAL: " << (total - failed - known) << "/" << total << " pass, " << failed << " unexpected fail, " << known << " known bug\n";
	return failed ? 1 : 0;
}
