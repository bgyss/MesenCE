#pragma once
#include <cstdint>
#include <string>
#include "MD/M68k/ares/m68000.h"

enum class M68kAccess : uint8_t
{
	Data,
	Program,
	Tas,
};

struct M68kRegisters
{
	uint32_t D[8];
	uint32_t A[7];
	uint32_t Usp;
	uint32_t Ssp;
	uint32_t Pc; // next prefetch address (instruction address + 4 at an instruction boundary)
	uint16_t Sr;
	uint16_t Ir; // opcode of the next instruction
	uint16_t Irc; // the word after it
	bool Stopped;
};

// Mesen-facing 68000: the ares interpreter plus an explicit bus interface. Every bus cycle
// reports its UDS/LDS strobes; Wait() precedes each bus cycle, Idle() is internal time.
// Odd word/long accesses raise a group 0 address error (see M68kCore.cpp).
class M68kCore : protected ares::M68000
{
public:
	M68kCore();
	virtual ~M68kCore() = default;

	virtual void Idle(uint32_t clocks) = 0;
	virtual void Wait(uint32_t clocks) = 0;
	virtual uint16_t BusRead(bool upper, bool lower, uint32_t address, M68kAccess access) = 0;
	virtual void BusWrite(bool upper, bool lower, uint32_t address, uint16_t data, M68kAccess access) = 0;
	virtual void AddressErrorCycle(bool write, uint32_t address, bool upper, bool lower) {}
	virtual uint16_t Peek(uint32_t address) = 0;
	// Called during interrupt acknowledge; return the level's autovector (default) or a vector number.
	virtual uint32_t AcknowledgeInterrupt(uint8_t level) { return ares::M68000::Vector::Level1 + level - 1; }
	// Debugger hooks: before an instruction executes (not while stopped), and after an
	// interrupt's exception processing has loaded the handler address.
	virtual void OnInstructionStart() {}
	virtual void OnInterruptTaken(uint32_t originalPc, uint32_t handlerPc) {}

	void PowerOn();
	// Fetches SSP and PC from vectors 0 and 1 and fills the prefetch queue.
	void Reset();
	// Executes one instruction, or one STOP wait cycle; services a pending interrupt first.
	void Step();

	// Interrupt priority level input (IPL0-2), 0 = none. Level 7 is edge-triggered.
	void SetInterruptLevel(uint8_t level);
	uint8_t GetInterruptLevel() const { return _irqLevel; }

	M68kRegisters GetRegisters() const;
	void SetRegisters(const M68kRegisters& regs);
	uint32_t GetInstructionPc() const { return (uint32_t)r.pc - 4; }
	bool IsStopped() const { return r.stop; }
	bool IsHalted() const { return _halted; }
	bool& RawHalted() { return _halted; }
	uint16_t GetSr() const { return (uint16_t)const_cast<M68kCore*>(this)->readSR(); }

	// Disassembles the instruction at pc; size receives its length in bytes.
	std::string Disassemble(uint32_t pc, uint32_t& size);

	// Raw state for serialization.
	ares::M68000::Registers& RawState() { return r; }
	uint8_t& RawIrqLevel() { return _irqLevel; }
	bool& RawNmiPending() { return _nmiPending; }

private:
	uint8_t _irqLevel = 0;
	bool _nmiPending = false;
	uint32_t _instructionStart = 0;
	bool _halted = false;
	uint8_t _ccrAtStart = 0;
	struct AddressErrorInfo
	{
		uint32_t Address;
		bool Write;
		bool Program;
	};

	void AddressErrorException(const AddressErrorInfo& info);

	using ares::M68000::read;
	using ares::M68000::write;

	using u32 = AresCompat::u32;
	using n1 = AresCompat::n1;
	using n16 = AresCompat::n16;
	using n24 = AresCompat::n24;
	using n32 = AresCompat::n32;
	auto idle(u32 clocks) -> void final { Idle(clocks); }
	auto wait(u32 clocks) -> void final { Wait(clocks); }
	auto read(n1 upper, n1 lower, n24 address, n16 data = 0) -> n16 final { return BusRead(upper, lower, address, (M68kAccess)access); }
	auto write(n1 upper, n1 lower, n24 address, n16 data) -> void final { BusWrite(upper, lower, address, data, (M68kAccess)access); }
	auto peek(n24 address) -> n16 final { return Peek(address); }
	auto addressError(bool write, n32 address, bool upper, bool lower) -> void final;
};
