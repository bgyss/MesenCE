// scenarios.cpp - interrupt conformance checks for MesenCE's Z80 (SmsCpu: SMS, Game
// Gear, SG-1000, ColecoVision).
//
// The SingleStepTests suite never takes an interrupt. This harness compiles the real
// SmsCpu source against a flat test bus (tools/z80-cpu-singlestep/TestSmsCpu.h),
// raises IRQ/NMI through the CPU's own entry points (SetIrqSource, SetNmiLevel), and
// checks each rule against its source:
//   Zilog Z80 CPU User Manual (UM0080), "Interrupt Response" and the HALT/EI/DI/RETN
//     descriptions;
//   Sean Young, "The Undocumented Z80 Documented", section 5 (interrupts);
//   ares (component/processor/z80/z80.cpp, irq()/nmi()) as a second source for the
//     acceptance sequence and the NMOS LD A,I/R quirk.
// The SMS data bus reads $FF during an interrupt acknowledge, which this harness's
// TestBus also returns.
//
// Usage: scenarios [--verbose]

#include "pch.h"
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include "../z80-cpu-singlestep/TestSmsCpu.h"

namespace fs = std::filesystem;

static constexpr uint16_t Prog = 0x0100;

enum Op : uint8_t
{
	NOP = 0x00,
	HALT = 0x76,
	DI = 0xF3,
	EI = 0xFB,
	ED = 0xED,
	LD_A_I = 0x57, // ED 57
	RETN = 0x45, // ED 45
	IM2 = 0x5E, // ED 5E
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
	TestSmsCpu Cpu;

	void Reset(bool iff, uint8_t im = 1)
	{
		memset(Bus.Ram, 0, sizeof(Bus.Ram));
		Bus.Ops.clear();
		SmsCpuState& s = Cpu._state;
		s = {};
		s.PC = Prog;
		s.SP = 0xD000;
		s.IFF1 = s.IFF2 = iff;
		s.IM = im;
	}

	void Load(uint16_t addr, std::initializer_list<uint8_t> bytes)
	{
		for(uint8_t b : bytes) {
			Bus.Ram[addr++] = b;
		}
	}

	uint64_t Step()
	{
		uint64_t start = Cpu._state.CycleCount;
		Cpu.Exec();
		return Cpu._state.CycleCount - start;
	}

	SmsCpuState& S() { return Cpu._state; }
	uint16_t Pushed() { return Bus.Ram[S().SP] | (Bus.Ram[(uint16_t)(S().SP + 1)] << 8); }
	void Irq(bool on) { on ? Cpu.SetIrqSource(SmsIrqSource::Vdp) : Cpu.ClearIrqSource(SmsIrqSource::Vdp); }
};

struct Scenario
{
	string Name;
	string Rule;
	std::function<string(Machine&)> Run; // "" if the documented behaviour holds, else what happened
};

static string Expect(const char* what, uint32_t actual, uint32_t expected, int width = 4)
{
	if(actual == expected) {
		return "";
	}
	return string(" ") + what + "=" + Hex(actual, width) + " (expected " + Hex(expected, width) + ")";
}

static vector<Scenario> Scenarios()
{
	vector<Scenario> list;

	list.push_back({ "IM 1 acceptance", "13 T-states; pushes the address of the next instruction; PC = $0038; IFF1 = IFF2 = 0; R + 1 for the acknowledge M1 (UM0080; ares irq())", [](Machine& m) {
							 m.Reset(true, 1);
							 m.Load(Prog, { NOP });
							 m.Irq(true);
							 uint64_t c = m.Step();
							 SmsCpuState& s = m.S();
							 return Expect("T", c, 4 + 13, 2) + Expect("PC", s.PC, 0x0038) + Expect("pushed", m.Pushed(), Prog + 1) +
								 Expect("IFF1", s.IFF1, 0, 1) + Expect("IFF2", s.IFF2, 0, 1) + Expect("R", s.R, 2, 2);
						 } });

	list.push_back({ "IM 2 acceptance", "19 T-states; vector read from (I << 8) | data bus ($FF on the SMS) (UM0080; ares irq())", [](Machine& m) {
							 m.Reset(true, 2);
							 m.S().I = 0x80;
							 m.Load(0x80FF, { 0x00, 0x40 });
							 m.Load(Prog, { NOP });
							 m.Irq(true);
							 uint64_t c = m.Step();
							 return Expect("T", c, 4 + 19, 2) + Expect("PC", m.S().PC, 0x4000) + Expect("pushed", m.Pushed(), Prog + 1);
						 } });

	list.push_back({ "IM 0 with $FF on the bus", "The acknowledge reads $FF = RST 38h: same as IM 1, 13 T-states (UM0080; Young section 5)", [](Machine& m) {
							 m.Reset(true, 0);
							 m.Load(Prog, { NOP });
							 m.Irq(true);
							 uint64_t c = m.Step();
							 return Expect("T", c, 4 + 13, 2) + Expect("PC", m.S().PC, 0x0038);
						 } });

	list.push_back({ "NMI acceptance", "11 T-states; PC = $0066; IFF1 = 0, IFF2 unchanged (UM0080; ares nmi())", [](Machine& m) {
							 m.Reset(true);
							 m.Load(Prog, { NOP });
							 m.Cpu.SetNmiLevel(true);
							 uint64_t c = m.Step();
							 SmsCpuState& s = m.S();
							 return Expect("T", c, 4 + 11, 2) + Expect("PC", s.PC, 0x0066) + Expect("pushed", m.Pushed(), Prog + 1) +
								 Expect("IFF1", s.IFF1, 0, 1) + Expect("IFF2", s.IFF2, 1, 1);
						 } });

	list.push_back({ "NMI is edge-triggered", "Holding NMI active gives one NMI, not one per instruction (UM0080)", [](Machine& m) {
							 m.Reset(false);
							 m.Load(Prog, { NOP });
							 m.Load(0x0066, { NOP, NOP, NOP });
							 m.Cpu.SetNmiLevel(true);
							 m.Step();
							 m.Cpu.SetNmiLevel(true);
							 m.Step();
							 m.Step();
							 return Expect("PC", m.S().PC, 0x0068);
						 } });

	list.push_back({ "EI delays IRQ by one instruction", "No maskable interrupt is accepted right after EI; it's accepted after the next instruction (UM0080, EI)", [](Machine& m) {
							 m.Reset(false);
							 m.Load(Prog, { EI, NOP, NOP });
							 m.Irq(true);
							 m.Step();
							 string err = Expect("PC after EI", m.S().PC, Prog + 1);
							 m.Step();
							 return err + Expect("PC after EI; NOP", m.S().PC, 0x0038) + Expect("pushed", m.Pushed(), Prog + 2);
						 } });

	list.push_back({ "EI; EI", "Each EI delays acceptance again: EI; EI; NOP takes the IRQ after the NOP (Young section 5)", [](Machine& m) {
							 m.Reset(false);
							 m.Load(Prog, { EI, EI, NOP });
							 m.Irq(true);
							 m.Step();
							 m.Step();
							 string err = Expect("PC after EI; EI", m.S().PC, Prog + 2);
							 m.Step();
							 return err + Expect("pushed", m.Pushed(), Prog + 3);
						 } });

	list.push_back({ "EI does not delay NMI", "An NMI is accepted right after EI (Young section 5; ares nmi() has no EI check)", [](Machine& m) {
							 m.Reset(false);
							 m.Load(Prog, { EI, NOP });
							 m.Cpu.SetNmiLevel(true);
							 m.Step();
							 return Expect("PC", m.S().PC, 0x0066) + Expect("pushed", m.Pushed(), Prog + 1);
						 } });

	list.push_back({ "DI masks IRQ", "With IFF1 = 0 no maskable interrupt is accepted (UM0080, DI)", [](Machine& m) {
							 m.Reset(true);
							 m.Load(Prog, { DI, NOP, NOP });
							 m.Irq(true);
							 m.Step();
							 m.Step();
							 return Expect("PC", m.S().PC, Prog + 2);
						 } });

	list.push_back({ "RETN restores IFF1", "After an NMI (IFF1 = 0, IFF2 = 1), RETN copies IFF2 to IFF1 (UM0080, RETN)", [](Machine& m) {
							 m.Reset(true);
							 m.Load(Prog, { NOP, NOP });
							 m.Load(0x0066, { ED, RETN });
							 m.Cpu.SetNmiLevel(true);
							 m.Step(); // NOP + NMI
							 m.Step(); // RETN
							 return Expect("PC", m.S().PC, Prog + 1) + Expect("IFF1", m.S().IFF1, 1, 1);
						 } });

	list.push_back({ "HALT, then IRQ", "HALT repeats 4 T-state NOP cycles; an IRQ ends it and pushes the address after HALT (UM0080, HALT)", [](Machine& m) {
							 m.Reset(true);
							 m.Load(Prog, { HALT, NOP });
							 m.Step(); // HALT
							 string err;
							 for(int i = 0; i < 3; i++) {
								 err += Expect("halted T", m.Step(), 4, 2);
							 }
							 m.Irq(true);
							 m.Step();
							 return err + Expect("PC", m.S().PC, 0x0038) + Expect("pushed", m.Pushed(), Prog + 1) + Expect("Halted", m.S().Halted, 0, 1);
						 } });

	list.push_back({ "R increments during HALT", "During HALT the CPU keeps executing NOP M1 cycles to refresh memory, so R increases by 1 per 4 T-states (UM0080, HALT; Young section 5). Judgment call: ares does not increment R while halted", [](Machine& m) {
							 m.Reset(false);
							 m.Load(Prog, { HALT });
							 m.Step(); // HALT: R = 1
							 for(int i = 0; i < 10; i++) {
								 m.Step();
							 }
							 return Expect("R", m.S().R, 11, 2);
						 } });

	list.push_back({ "LD A,I then IRQ (NMOS)", "On the NMOS Z80, if an interrupt is accepted right after LD A,I or LD A,R, the P/V flag reads 0 instead of IFF2 (Young section 5; ares irq()/nmi(): if(P) PF = 0; SingleStepTests' \"p\" field). The SMS uses an NMOS Z80", [](Machine& m) {
							 m.Reset(true);
							 m.Load(Prog, { ED, LD_A_I });
							 m.Load(0x0038, { NOP });
							 m.Irq(true);
							 m.Step(); // LD A,I (P/V = IFF2 = 1), then the IRQ is accepted
							 return Expect("PC", m.S().PC, 0x0038) + Expect("P/V", (m.S().Flags & SmsCpuFlags::Parity) != 0, 0, 1);
						 } });

	list.push_back({ "LD A,I without an interrupt", "Without an interrupt, P/V = IFF2 (UM0080, LD A,I)", [](Machine& m) {
							 m.Reset(true);
							 m.Load(Prog, { ED, LD_A_I, NOP });
							 m.Step();
							 return Expect("P/V", (m.S().Flags & SmsCpuFlags::Parity) != 0, 1, 1);
						 } });

	return list;
}

// Scenarios that fail because of a documented, not-yet-fixed MesenCE behaviour. They are
// reported but don't fail the run. See docs/research/sms-verification-findings.md.
static const std::set<string> KnownBugs = {};

int main(int argc, char* argv[])
{
	bool verbose = argc > 1 && string(argv[1]) == "--verbose";

	fs::path home = fs::temp_directory_path() / "mesen-z80-irq-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);
	// The IRQ path calls SmsConsole::RefreshRamCheats; an unloaded console is enough.
	SmsConsole console(&emu);

	Machine* m = new Machine();
	m->Cpu._emu = &emu;
	m->Cpu._console = &console;
	m->Cpu._memoryManager = &m->Bus;

	int failed = 0, known = 0, total = 0;
	for(Scenario& sc : Scenarios()) {
		string err = sc.Run(*m);
		total++;
		bool isKnown = KnownBugs.count(sc.Name) > 0;
		if(err.empty()) {
			std::cout << "PASS  " << sc.Name << "\n";
		} else if(isKnown) {
			known++;
			std::cout << "KNOWN " << sc.Name << ":" << err << "\n";
		} else {
			failed++;
			std::cout << "FAIL  " << sc.Name << ":" << err << "\n";
		}
		if(verbose || !err.empty()) {
			std::cout << "      rule: " << sc.Rule << "\n";
		}
	}
	std::cout << "TOTAL: " << (total - failed - known) << "/" << total << " pass, " << failed << " unexpected fail, " << known << " known\n";
	return failed ? 1 : 0;
}
