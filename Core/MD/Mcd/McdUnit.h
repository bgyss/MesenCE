#pragma once
#include "pch.h"
#include "MD/M68k/M68kCore.h"
#include "MD/Mcd/McdPcm.h"
#include "MD/Mcd/McdCd.h"
#include "Utilities/ISerializable.h"
#include "Debugger/DebugTypes.h"
#include "MD/MdTypes.h"

class MdConsole;
class McdUnit;
class Emulator;

// The Sega CD's sub CPU: a 68000 at 12.5 MHz on the unit's own bus (McdUnit::SubRead/SubWrite).
// Not visible to the debugger yet (S4 in docs/research/sega-cd-feasibility.md).
class McdM68k final : public M68kCore
{
private:
	McdUnit* _unit = nullptr;
	Emulator* _emu = nullptr;

public:
	void Init(McdUnit* unit, Emulator* emu)
	{
		_unit = unit;
		_emu = emu;
	}

	MdM68kState GetState();
	void SetState(MdM68kState& state);

	void Idle(uint32_t clocks) override;
	void Wait(uint32_t clocks) override;
	uint16_t BusRead(bool upper, bool lower, uint32_t address, M68kAccess access) override;
	void BusWrite(bool upper, bool lower, uint32_t address, uint16_t data, M68kAccess access) override;
	uint16_t Peek(uint32_t address) override;
	uint32_t AcknowledgeInterrupt(uint8_t level) override;
	void OnInstructionStart() override;
	void OnInterruptTaken(uint32_t originalPc, uint32_t handlerPc) override;
};

// Sega CD / Mega-CD unit, Mode 1 (a cartridge in the Mega Drive, the unit attached): the sub CPU,
// its program RAM and word RAM, the gate array (registers, timer, interrupts, font registers,
// graphics chip, dot image), backup RAM, the PCM chip (McdPcm) and VDP DMA from word RAM.
// Also the CD drive and controller (McdCd.h), the BIOS, and both boot modes: Mode 1 (cartridge at
// $000000, unit at $400000) and Mode 2 (disc boot: unit at $000000, cartridge slot at $400000).
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
	static constexpr uint32_t BackupRamSize = 0x2000;

	//Test hooks (mesen-headless --md-mcd, --md-mcd-disc): attach the unit to cartridge games, with a
	//disc in the drive. The UI setting is S4.
	static void SetAttachedOverride(bool attached) { _attachedOverride = attached; }
	static bool IsAttachedOverride() { return _attachedOverride; }
	static void SetDiscOverride(string path) { _discOverride = path; }
	static string GetDiscOverride() { return _discOverride; }

private:
	static inline bool _attachedOverride = false;
	static inline string _discOverride;

	bool _mode2 = false;
	vector<uint8_t> _bios;
	unique_ptr<DiscInfo> _disc;
	McdCdc _cdc;
	McdCdd _cdd;
	uint32_t _cdcDmaCounter = 0;

	MdConsole* _console = nullptr;
	McdM68k _cpu;
	uint32_t _mclkRate = 0;

	uint8_t* _prgRam = nullptr;
	//Word RAM, big-endian bytes (as the memory viewer and Lua see it); "index" below is a word index
	uint8_t _wordRam[WordRamSize] = {};
	__forceinline uint16_t Wram(uint32_t index)
	{
		index = (index & 0x1FFFF) << 1;
		return (_wordRam[index] << 8) | _wordRam[index | 1];
	}
	__forceinline void WriteWram(uint32_t index, bool upper, bool lower, uint16_t value)
	{
		index = (index & 0x1FFFF) << 1;
		if(upper) {
			_wordRam[index] = value >> 8;
		}
		if(lower) {
			_wordRam[index | 1] = (uint8_t)value;
		}
	}

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

	//Backup RAM: 8 KiB on the odd bytes of $FE0000-$FE3FFF (battery file .brm)
	uint8_t _backupRam[BackupRamSize] = {};

	//Font registers $FF804C-$FF8057 (Hardware Manual p. 34)
	uint8_t _fontColor = 0;
	uint16_t _fontBits = 0;

	//Graphics chip ("rotation scaling", $FF8058-$FF8066, Hardware Manual pp. 35-53)
	struct Graphics
	{
		bool Repeat = false;
		bool Stamp32 = false; //STS: 32x32 dot stamps
		bool Map16 = false; //SMS: 16x16 screen (4096x4096 dot) map
		uint32_t MapBase = 0; //byte address in word RAM
		uint8_t VCells = 0; //image buffer V cell size - 1
		uint32_t ImageBase = 0; //byte address
		uint8_t ImageOffset = 0; //LN2-0, dot2-0
		uint16_t HDots = 0;
		uint8_t VDots = 0;
		uint32_t VectorBase = 0; //byte address
		bool Active = false;
		uint32_t ImageAddress = 0; //nibble address of the current line's first dot
		uint32_t VectorAddress = 0; //word index of the next trace vector
		uint32_t MapAddress = 0; //word index
		int32_t LineClocks = 0; //sub cycles per line
		int32_t Counter = 0; //sub cycles left in the current line
	} _gfx;

	//VDP DMA from word RAM: the data comes out one read late (Hardware Manual p. 61; ares wramLatch)
	uint16_t _dmaLatch = 0;

	McdPcm _pcm;

	bool IsHalted() { return !_run || _busRequest; }
	bool MainOwnsPrgRam() { return _busRequest || !_run; }
	void AdvanceSub(uint32_t subCycles);
	void Tick();
	void RaiseInterrupt(uint8_t level);
	void UpdateInterruptLevel();

	uint16_t ReadWordRamMain(uint32_t offset);
	void WriteWordRamMain(uint32_t offset, bool upper, bool lower, uint16_t value);
	static uint32_t CellImageToWordIndex(uint32_t offset);
	uint8_t ReadNibble(uint32_t address);
	void WriteNibble(uint32_t address, uint8_t value);
	uint16_t ReadDotImage(uint32_t address);
	void WriteDotImage(uint32_t address, bool upper, bool lower, uint16_t value);
	void StartGraphics();
	void RenderGraphicsLine();
	void RunGraphics(uint32_t subCycles);
	uint16_t ReadGraphicsRegister(uint32_t reg);
	void WriteGraphicsRegister(uint32_t reg, bool upper, bool lower, uint16_t value);
	uint16_t ReadRegister(uint32_t reg, bool fromSub);
	void WriteMainRegister(uint32_t reg, bool upper, bool lower, uint16_t value);
	void WriteSubRegister(uint32_t reg, bool upper, bool lower, uint16_t value);

public:
	McdUnit() = default;
	~McdUnit();

	void Init(MdConsole* console, uint32_t mclkRate, bool mode2, vector<uint8_t> bios, unique_ptr<DiscInfo> disc);
	bool IsMode2() { return _mode2; }

	//For the CD controller and drive
	void SetInterruptLine(uint8_t level, bool active);
	void CdcDmaWrite(uint8_t destination, uint32_t address, uint16_t data);
	//PCM and CD audio output at master clock `clock` (MdApu)
	void GetAudioOutput(uint64_t clock, int32_t& left, int32_t& right);
	McdCdd& GetCdd() { return _cdd; }

	//Runs the sub CPU until its position reaches `masterClock`
	void Run(uint64_t masterClock);

	//Main CPU side: `offset` is the address within the expansion area ($000000-$3FFFFF)
	uint16_t MainRead(uint32_t offset, uint16_t openBus);
	void MainWrite(uint32_t offset, bool upper, bool lower, uint16_t value);
	//Main CPU registers $A12000-$A120FF ($A12040-$A120FF mirror $A12000-$A1203F)
	uint16_t MainReadRegister(uint32_t addr);
	void MainWriteRegister(uint32_t addr, bool upper, bool lower, uint16_t value);
	uint8_t DebugMainRead(uint32_t offset);
	//68000-to-VDP DMA source read
	uint16_t DmaMainRead(uint32_t offset);

	//Sub CPU bus
	void SubAdvance(uint32_t subCycles) { AdvanceSub(subCycles); }
	uint16_t SubRead(bool upper, bool lower, uint32_t address);
	void SubWrite(bool upper, bool lower, uint32_t address, uint16_t value);
	void SubAcknowledgeInterrupt(uint8_t level);

	McdM68k& GetCpu() { return _cpu; }

	//Debugger: side-effect-free sub CPU accesses, and addresses in the unit's memories
	uint8_t SubDebugRead(uint32_t address);
	void SubDebugWrite(uint32_t address, uint8_t value);
	AddressInfo SubGetAbsoluteAddress(uint32_t address);
	int32_t SubGetRelativeAddress(AddressInfo& absAddress);
	AddressInfo MainGetAbsoluteAddress(uint32_t offset);
	McdPcm& GetPcm() { return _pcm; }
	uint8_t* GetBackupRam() { return _backupRam; }
	uint8_t* GetWordRam() { return _wordRam; }
	uint8_t* GetPrgRam() { return _prgRam; }
	uint64_t GetSubCycles() { return _subCycles; }

	void Serialize(Serializer& s) override;
};
