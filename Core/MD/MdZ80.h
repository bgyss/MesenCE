#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "Shared/Processors/Z80/Z80Engine.h"
#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;
class MdMemoryManager;

// The Mega Drive's sound CPU: the shared Z80 engine with the MD's Z80 bus (8 KiB RAM, YM2612,
// bank register, PSG, 68000 bank window). One T-state is 15 master clocks. The Z80 runs in
// catch-up mode behind the 68000 and does not run while held in reset or while the 68000 owns
// its bus.
class MdZ80 final : public Z80Engine<MdZ80>, public ISerializable
{
	friend class Z80Engine<MdZ80>;

private:
	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;
	MdMemoryManager* _memoryManager = nullptr;
	uint64_t _masterClock = 0;

	// Z80Engine host interface
	template<MemoryOperationType type>
	__forceinline uint8_t BusRead(uint16_t addr);
	__forceinline void BusWrite(uint16_t addr, uint8_t value);
	uint8_t BusReadPort(uint8_t port);
	void BusWritePort(uint8_t port, uint8_t value);
	__forceinline void OnExecCycles(uint8_t cycles) { _masterClock += cycles * 15; }
	uint8_t GetInterruptDataBus() { return 0xFF; }
	bool HasNmosLdIrQuirk() { return true; }
	__forceinline void OnInstruction();
	void OnHaltedCycle();
	void OnHaltStarted();
	void OnHaltEnded();
	void OnInterruptAccepted(uint16_t originalPc, uint16_t handlerPc, bool nmi);

public:
	void Init(Emulator* emu, MdConsole* console, MdMemoryManager* memoryManager);
	void Reset();

	uint64_t GetMasterClock() { return _masterClock; }
	// Adds wait states (e.g. a bank-window access waiting for the 68000 bus).
	void AddWait(uint32_t masterClocks) { _masterClock += masterClocks; }

	// Runs instructions until the Z80 reaches targetClock, or skips to it while the Z80 is
	// held (reset asserted, or the 68000 owns the bus).
	void Run(uint64_t targetClock, bool held);

	void Serialize(Serializer& s) override;
};
