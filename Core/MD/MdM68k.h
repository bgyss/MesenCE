#pragma once
#include "pch.h"
#include "MD/M68k/M68kCore.h"
#include "MD/MdTypes.h"
#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;
class MdMemoryManager;
class MdVdp;

// The Mega Drive's 68000: the shared M68kCore with the console bus behind it. Every bus cycle
// goes through MdMemoryManager (which owns the master clock); CPU clocks are 7 master clocks.
class MdM68k final : public M68kCore, public ISerializable
{
private:
	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;
	MdMemoryManager* _memoryManager = nullptr;
	MdVdp* _vdp = nullptr;
	uint64_t _cycleCount = 0;

public:
	void Init(Emulator* emu, MdConsole* console, MdMemoryManager* memoryManager, MdVdp* vdp);
	void PowerOn();

	// Executes one instruction (or one STOP/halt step) and then lets the rest of the machine
	// catch up to the 68000.
	void Exec();

	uint64_t GetCycleCount() { return _cycleCount; }

	MdM68kState GetState();
	void SetState(MdM68kState& state);

	void Idle(uint32_t clocks) override;
	void Wait(uint32_t clocks) override;
	uint16_t BusRead(bool upper, bool lower, uint32_t address, M68kAccess accessType) override;
	void BusWrite(bool upper, bool lower, uint32_t address, uint16_t data, M68kAccess accessType) override;
	uint16_t Peek(uint32_t address) override;
	uint32_t AcknowledgeInterrupt(uint8_t level) override;
	void OnInstructionStart() override;
	void OnInterruptTaken(uint32_t originalPc, uint32_t handlerPc) override;

	void Serialize(Serializer& s) override;
};
