#pragma once
#include "pch.h"
#include "MD/M68k/M68kCore.h"
#include "Utilities/ISerializable.h"

class MdConsole;
class McdUnit;

// The Sega CD's sub CPU: a 68000 at 12.5 MHz on the unit's own bus (McdUnit::SubRead/SubWrite).
// Not visible to the debugger yet (S4 in docs/research/sega-cd-feasibility.md).
class McdM68k final : public M68kCore
{
private:
	McdUnit* _unit = nullptr;

public:
	void Init(McdUnit* unit) { _unit = unit; }

	void Idle(uint32_t clocks) override;
	void Wait(uint32_t clocks) override;
	uint16_t BusRead(bool upper, bool lower, uint32_t address, M68kAccess access) override;
	void BusWrite(bool upper, bool lower, uint32_t address, uint16_t data, M68kAccess access) override;
	uint16_t Peek(uint32_t address) override;
	uint32_t AcknowledgeInterrupt(uint8_t level) override;
};

// Sega CD / Mega-CD unit, Mode 1 (a cartridge in the Mega Drive, the unit attached): the sub CPU,
// its program RAM and word RAM, and the gate array's CPU-facing registers, timer and interrupts.
// Not yet: the BIOS, the CD drive and controller, PCM, the graphics chip, backup RAM, the dot-image
// view and VDP DMA from the unit (sega-cd-feasibility.md, S2/S3).
//
// Sources (docs/research/mcd-verification-findings.md): Sega, Mega-CD Hardware Manual (1991) for
// the memory maps, registers, interrupt levels, word RAM switching and the timer; ares
// (ares/md/mcd, ISC, 65c6fc2f) as the reference implementation, cited where it is the only source.
//
// Timing: the sub CPU runs from its own 12.5 MHz crystal. Its position is kept in Mega Drive
// master clocks plus an exact remainder (findings F3). It catches up to the main CPU after every
// main instruction and before every main-CPU access to the unit, so the sub CPU can be up to one
// of its instructions ahead of an access. Memory contention (refresh, CD DMA, the graphics chip)
// is not modelled: the manual describes uncontended accesses as wait-free and gives no figures.
class McdUnit final : public ISerializable
{
public:
	static constexpr uint32_t SubClockRate = 12500000;
	static constexpr uint32_t PrgRamSize = 0x80000;
	static constexpr uint32_t WordRamSize = 0x40000;

	//Test hook (mesen-headless --md-mcd): attach the unit to cartridge games. The UI setting is S4.
	static void SetAttachedOverride(bool attached) { _attachedOverride = attached; }
	static bool IsAttachedOverride() { return _attachedOverride; }

private:
	static inline bool _attachedOverride = false;

	MdConsole* _console = nullptr;
	McdM68k _cpu;
	uint32_t _mclkRate = 0;

	uint8_t* _prgRam = nullptr;
	uint16_t _wordRam[WordRamSize / 2] = {};

	//Sub CPU position: master clocks, plus a remainder in units of 1/SubClockRate master clocks
	uint64_t _subClock = 0;
	uint64_t _subRemainder = 0;
	uint64_t _subCycles = 0;
	//Sub cycles towards the next 30.72 us tick (384 sub cycles: timer and stopwatch)
	uint32_t _tickDivider = 0;

	//$A12001 / gate array: sub CPU reset and bus request (ares: io.run, io.request, io.halt)
	bool _run = false;
	bool _busRequest = false;
	bool _resetPending = true;

	//Memory mode ($A12002 / $FF8002)
	uint8_t _prgProtect = 0;
	uint8_t _prgBank = 0;
	bool _wordRam1M = false;
	bool _wordRamToSub = false; //2M mode: DMNA side (1 = the sub CPU owns word RAM)
	bool _swapRequest = false; //1M mode: DMNA as read (ares: wramSwitchRequest)
	bool _ret = false; //sub CPU's RET bit (ares: wramSelect); in 1M mode, the main CPU's bank
	uint8_t _priorityMode = 0;

	uint16_t _hIntVector = 0xFFFF;
	uint8_t _commFlagMain = 0;
	uint8_t _commFlagSub = 0;
	uint16_t _commCommand[8] = {};
	uint16_t _commStatus[8] = {};
	uint16_t _stopwatch = 0;
	uint8_t _timerReload = 0;
	uint8_t _timerCounter = 0;
	uint8_t _irqEnable = 0; //$FF8032 bits 1-6 = levels 1-6
	uint8_t _irqPending = 0; //bit n = level n
	uint8_t _led = 0;

	bool IsHalted() { return !_run || _busRequest; }
	bool MainOwnsPrgRam() { return _busRequest || !_run; }
	void AdvanceSub(uint32_t subCycles);
	void Tick();
	void RaiseInterrupt(uint8_t level);
	void UpdateInterruptLevel();

	uint16_t ReadWordRamMain(uint32_t offset);
	void WriteWordRamMain(uint32_t offset, bool upper, bool lower, uint16_t value);
	static uint32_t CellImageToWordIndex(uint32_t offset);
	uint16_t ReadRegister(uint32_t reg, bool fromSub);
	void WriteMainRegister(uint32_t reg, bool upper, bool lower, uint16_t value);
	void WriteSubRegister(uint32_t reg, bool upper, bool lower, uint16_t value);

public:
	McdUnit() = default;
	~McdUnit();

	void Init(MdConsole* console, uint32_t mclkRate);

	//Runs the sub CPU until its position reaches `masterClock`
	void Run(uint64_t masterClock);

	//Main CPU side: `offset` is the address within the expansion area ($000000-$3FFFFF)
	uint16_t MainRead(uint32_t offset, uint16_t openBus);
	void MainWrite(uint32_t offset, bool upper, bool lower, uint16_t value);
	//Main CPU registers $A12000-$A120FF ($A12040-$A120FF mirror $A12000-$A1203F)
	uint16_t MainReadRegister(uint32_t addr);
	void MainWriteRegister(uint32_t addr, bool upper, bool lower, uint16_t value);
	uint8_t DebugMainRead(uint32_t offset);

	//Sub CPU bus
	void SubAdvance(uint32_t subCycles) { AdvanceSub(subCycles); }
	uint16_t SubRead(bool upper, bool lower, uint32_t address);
	void SubWrite(bool upper, bool lower, uint32_t address, uint16_t value);
	void SubAcknowledgeInterrupt(uint8_t level);

	McdM68k& GetCpu() { return _cpu; }
	uint64_t GetSubCycles() { return _subCycles; }

	void Serialize(Serializer& s) override;
};
