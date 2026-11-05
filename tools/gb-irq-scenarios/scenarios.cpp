// scenarios.cpp - interrupt / HALT / EI conformance checks for MesenCE's Game Boy CPU.
//
// The SingleStepTests suite never takes an interrupt. This harness drives the real
// GbCpu (on a real Gameboy console whose memory pages are pointed at a flat 64 KiB
// RAM, with I/O-register pages disabled) through short programs with real IE/IF
// state, and checks each documented rule from Pan Docs:
//   https://gbdev.io/pandocs/Interrupts.html   https://gbdev.io/pandocs/halt.html
//
// Only the Serial ($58) and Joypad ($60) interrupts are used: neither the PPU nor
// the timer can raise them by themselves, so the harness fully controls IF.
//
// Usage: scenarios [--verbose]

#include "pch.h"
#include <filesystem>
#include <functional>
#include <iostream>
#include <sstream>
#define private public
#include "Shared/Emulator.h"
#include "Gameboy/Gameboy.h"
#include "Gameboy/GbCpu.h"
#include "Gameboy/GbMemoryManager.h"
#undef private
#include "Utilities/VirtualFile.h"
#include "Utilities/FolderUtilities.h"

namespace fs = std::filesystem;

// SM83 opcodes used by the scenarios
enum Op : uint8_t
{
	NOP = 0x00,
	INC_C = 0x0C,
	INC_D = 0x14,
	INC_E = 0x1C,
	HALT = 0x76,
	RETI = 0xD9,
	DI = 0xF3,
	EI = 0xFB,
	RST38 = 0xFF,
	JR = 0x18, // JR e8
};

static constexpr uint8_t Serial = 0x08; // IF/IE bit 3, vector $58
static constexpr uint8_t Joypad = 0x10; // IF/IE bit 4, vector $60
static constexpr uint16_t Prog = 0x0200;

struct Machine
{
	Gameboy* Gb = nullptr;
	GbCpu* Cpu = nullptr;
	GbMemoryManager* Mm = nullptr;
	uint8_t Ram[0x10000] = {};

	void Reset(uint8_t ie, uint8_t ifl, bool ime)
	{
		memset(Ram, 0, sizeof(Ram));
		for(int i = 0; i < 0x100; i++) {
			Mm->_reads[i] = Ram + i * 0x100;
			Mm->_writes[i] = Ram + i * 0x100;
			Mm->_state.IsReadRegister[i] = false;
			Mm->_state.IsWriteRegister[i] = false;
		}
		// Every handler counts its entries in D, then returns with RETI.
		for(uint16_t v : { 0x40, 0x48, 0x50, 0x58, 0x60 }) {
			Ram[v] = INC_D;
			Ram[v + 1] = RETI;
		}
		Ram[0x0000] = INC_E; // reached only via the IE-push quirk
		GbCpuState& s = Cpu->GetState();
		s.PC = Prog;
		s.SP = 0xD000;
		s.A = s.B = s.C = s.D = s.E = s.H = s.L = 0;
		s.Flags = 0;
		s.IME = ime;
		s.EiPending = false;
		s.HaltCounter = 0;
		s.HaltBug = false;
		s.Stopped = false;
		Mm->_state.IrqEnabled = ie;
		Mm->_state.IrqRequests = ifl;
		Cpu->PowerOn(); // samples the IRQ line for the first instruction, like a real boundary
	}

	void Load(std::initializer_list<uint8_t> bytes)
	{
		uint16_t a = Prog;
		for(uint8_t b : bytes) {
			Ram[a++] = b;
		}
	}

	uint64_t Step() // one GbCpu::Exec(); returns M-cycles consumed
	{
		uint64_t start = Cpu->GetState().CycleCount;
		Cpu->Exec();
		return (Cpu->GetState().CycleCount - start) / 4;
	}

	void Run(int steps)
	{
		for(int i = 0; i < steps; i++) {
			Step();
		}
	}

	uint16_t StackWord(uint16_t sp) { return Ram[sp] | (Ram[(uint16_t)(sp + 1)] << 8); }
	GbCpuState& S() { return Cpu->GetState(); }
};

struct Scenario
{
	string Name;
	string Rule;
	std::function<string(Machine&)> Run; // returns "" if the documented behaviour holds, else what happened
	bool KnownBug = false; // documented in docs/research/gb-verification-findings.md; does not fail the run
};

static string Fmt(const char* what, int actual, int expected)
{
	std::ostringstream ss;
	ss << what << " = " << actual << " (documented: " << expected << ")";
	return ss.str();
}

int main(int argc, char* argv[])
{
	bool verbose = argc > 1 && string(argv[1]) == "--verbose";
	fs::path home = fs::temp_directory_path() / "mesen-gb-irq-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);
	emu._isRunAheadFrame = true;

	vector<uint8_t> rom(0x8000, 0);
	Gameboy gb(&emu);
	VirtualFile romFile(rom.data(), rom.size(), "irq.gb");
	if(gb.LoadRom(romFile) != LoadRomResult::Success) {
		std::cerr << "failed to set up Gameboy console\n";
		return 2;
	}
	Machine m;
	m.Gb = &gb;
	m.Cpu = gb.GetCpu();
	m.Mm = gb.GetMemoryManager();

	vector<Scenario> scenarios = {
		{ "dispatch", "IME=1, IE&IF!=0: handler runs, IF bit and IME cleared, return address pushed (Interrupts: dispatch)", [](Machine& m) -> string {
			 m.Reset(Serial, Serial, true);
			 m.Load({ NOP, NOP });
			 m.Step();
			 if(m.S().D != 1) {
				 return Fmt("handler entries (D)", m.S().D, 1);
			 }
			 if(m.Mm->_state.IrqRequests & Serial) {
				 return "IF.serial still set after dispatch";
			 }
			 if(m.StackWord(0xCFFE) != Prog) {
				 return Fmt("pushed return address", m.StackWord(0xCFFE), Prog);
			 }
			 return "";
		 } },
		{ "dispatch-cycles", "Dispatch takes 5 M-cycles (Interrupts: 2 wait + 2 push + 1 jump)", [](Machine& m) -> string {
			 m.Reset(Serial, 0, true);
			 m.Load({ NOP, NOP });
			 uint64_t nop = m.Step(); // baseline: NOP without an interrupt
			 m.Reset(Serial, Serial, true);
			 m.Load({ NOP, NOP });
			 uint64_t withIrq = m.Step(); // dispatch + the handler's first instruction (INC D, 1 M-cycle)
			 if(withIrq - nop != 5) {
				 return Fmt("dispatch M-cycles", (int)(withIrq - nop), 5);
			 }
			 return "";
		 } },
		{ "priority", "Lowest IF bit wins: Serial ($58) before Joypad ($60) (Interrupts: priority)", [](Machine& m) -> string {
			 m.Reset(Serial | Joypad, Serial | Joypad, true);
			 m.Load({ NOP, NOP });
			 m.Step();
			 if(m.Mm->_state.IrqRequests != Joypad) {
				 return Fmt("IF after first dispatch", m.Mm->_state.IrqRequests, Joypad);
			 }
			 return "";
		 } },
		{ "ei-delay", "EI's effect is delayed by one instruction: the instruction after EI runs before the handler", [](Machine& m) -> string {
			 m.Reset(Serial, Serial, false);
			 m.Load({ EI, INC_C, NOP, NOP });
			 m.Step(); // EI
			 m.Step(); // INC C (must run before the interrupt is taken)
			 if(m.S().C != 1 || m.S().D != 0) {
				 return Fmt("INC C before handler (C)", m.S().C, 1) + ", D=" + std::to_string(m.S().D);
			 }
			 m.Step(); // now the interrupt is taken
			 if(m.S().D != 1) {
				 return Fmt("handler entries after the following instruction (D)", m.S().D, 1);
			 }
			 return "";
		 } },
		{ "ei-di", "EI immediately followed by DI allows no interrupt between them, and IME ends up 0", [](Machine& m) -> string {
			 m.Reset(Serial, Serial, false);
			 m.Load({ EI, DI, NOP, NOP, NOP, NOP });
			 m.Run(5);
			 if(m.S().D != 0) {
				 return Fmt("handler entries (D)", m.S().D, 0);
			 }
			 if(m.S().IME) {
				 return "IME=1 after EI;DI";
			 }
			 return "";
		 } },
		{ "halt-ime1", "HALT with IME=1: wakes on interrupt, handler runs, returns after HALT", [](Machine& m) -> string {
			 m.Reset(Serial, 0, true);
			 m.Load({ HALT, INC_C, NOP, NOP });
			 m.Run(3); // halted
			 if(!m.S().HaltCounter) {
				 return "CPU did not halt";
			 }
			 m.Mm->_state.IrqRequests = Serial; // raise the interrupt while halted
			 m.Run(6);
			 if(m.S().D != 1) {
				 return Fmt("handler entries (D)", m.S().D, 1);
			 }
			 if(m.S().C != 1) {
				 return Fmt("instruction after HALT executed (C)", m.S().C, 1);
			 }
			 return "";
		 } },
		{ "halt-ime0-wake", "HALT with IME=0 and nothing pending: wakes when IE&IF!=0, interrupt NOT handled", [](Machine& m) -> string {
			 m.Reset(Serial, 0, false);
			 m.Load({ HALT, INC_C, NOP, NOP });
			 m.Run(3);
			 if(!m.S().HaltCounter) {
				 return "CPU did not halt";
			 }
			 m.Mm->_state.IrqRequests = Serial;
			 m.Run(4);
			 if(m.S().D != 0) {
				 return Fmt("handler entries (D)", m.S().D, 0);
			 }
			 if(m.S().C != 1) {
				 return Fmt("execution resumed after HALT (C)", m.S().C, 1);
			 }
			 return "";
		 } },
		{ "halt-bug", "HALT with IME=0 and IE&IF!=0: halt bug, the byte after HALT is read twice", [](Machine& m) -> string {
			 m.Reset(Serial, Serial, false);
			 m.Load({ HALT, INC_C, NOP, NOP });
			 m.Run(4);
			 if(m.S().C != 2) {
				 return Fmt("INC C executions (C)", m.S().C, 2);
			 }
			 return "";
		 } },
		{ "halt-bug-rst", "Halt bug followed by RST: the pushed return address points at the RST itself", [](Machine& m) -> string {
			 m.Reset(Serial, Serial, false);
			 m.Load({ HALT, RST38 });
			 m.Ram[0x38] = NOP;
			 m.Step(); // HALT (bug triggered)
			 m.Step(); // RST $38 (executed via the bug)
			 uint16_t ret = m.StackWord(m.S().SP);
			 if(m.S().PC != 0x38) {
				 return Fmt("PC after RST", m.S().PC, 0x38);
			 }
			 if(ret != Prog + 1) {
				 return Fmt("pushed return address", ret, Prog + 1);
			 }
			 return "";
		 } },
		{ "ei-halt", "EI immediately before HALT with an interrupt pending: serviced, but the handler returns to the HALT (halt.html)", [](Machine& m) -> string {
			 m.Reset(Serial, Serial, false);
			 m.Load({ EI, HALT, INC_C, NOP, NOP, NOP });
			 m.Step(); // EI
			 m.Step(); // HALT
			 uint16_t sp = m.S().SP;
			 m.Run(4); // interrupt taken, handler (INC D; RETI) runs and returns
			 std::ostringstream ss;
			 if(m.S().D != 1) {
				 return Fmt("handler entries (D)", m.S().D, 1);
			 }
			 uint16_t ret = m.StackWord((uint16_t)(sp - 2));
			 if(ret != Prog + 1) {
				 ss << "handler returned to $" << std::hex << std::uppercase << ret << " (documented: $" << (Prog + 1)
					 << ", the HALT); the instruction after HALT ran " << std::dec << (int)m.S().C << "x and the CPU is "
					 << (m.S().HaltCounter ? "halted" : "running") << " (documented: halted again)";
				 return ss.str();
			 }
			 if(!m.S().HaltCounter || m.S().C != 0) {
				 ss << "returned to HALT but CPU " << (m.S().HaltCounter ? "halted" : "running") << ", C=" << (int)m.S().C;
				 return ss.str();
			 }
			 return "";
		 },
			true },
		{ "reti-ime", "RETI enables IME (Interrupts: 'same as EI immediately followed by RET'; timing not specified)", [](Machine& m) -> string {
			 // Informational: report whether an interrupt pending at RETI is taken before the next instruction.
			 m.Reset(Serial, 0, false);
			 m.Load({ RETI });
			 m.Ram[0xD000] = 0x00; // RETI pops $0300
			 m.Ram[0xD001] = 0x03;
			 m.Ram[0x0300] = INC_C;
			 m.Ram[0x0301] = NOP;
			 m.Mm->_state.IrqRequests = Serial;
			 m.Step(); // RETI
			 m.Step();
			 std::cout << "      (info) after RETI with an interrupt pending: "
						  << (m.S().D ? "taken before the next instruction (immediate IME)" : "next instruction ran first (delayed IME)") << "\n";
			 return "";
		 } },
	};

	int failed = 0, known = 0;
	for(Scenario& s : scenarios) {
		string result = s.Run(m);
		bool ok = result.empty();
		const char* tag = ok ? (s.KnownBug ? "PASS (known bug no longer reproduces) " : "PASS ") : (s.KnownBug ? "KNOWN BUG " : "FAIL ");
		if(!ok) {
			(s.KnownBug ? known : failed)++;
		}
		std::cout << tag << s.Name << (verbose || !ok ? "  -- " + s.Rule : "") << "\n";
		if(!ok) {
			std::cout << "      " << result << "\n";
		}
	}
	std::cout << "TOTAL: " << (scenarios.size() - failed - known) << "/" << scenarios.size() << " documented behaviours hold, " << failed
				 << " unexpected failure(s), " << known << " known bug(s)\n";
	return failed ? 1 : 0;
}
