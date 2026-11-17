#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "MD/M68k/M68kCore.h"
#include "Debugger/DebugTypes.h"
#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;
class MdM68k;
class MdZ80;
class MdVdp;
class MdApu;
class MdControlManager;
class MdEepromDevice;

// The Mega Drive bus (Sega Genesis Software Manual: memory map, system I/O, Z80 control):
//
//   68000: $000000-$3FFFFF cartridge ROM (and battery RAM per the header), $A00000-$A0FFFF
//          Z80 area (RAM, YM2612, bank register; only while the 68000 holds the Z80 bus),
//          $A10000-$A1001F I/O, $A11100 BUSREQ, $A11200 Z80 RESET, $A130F1 SRAM control,
//          $A14000 TMSS, $C00000-$DFFFFF VDP/PSG, $E00000-$FFFFFF 64 KiB work RAM (mirrored).
//   Z80:   $0000-$3FFF 8 KiB RAM (mirrored), $4000-$5FFF YM2612, $6000 bank register,
//          $7F00-$7F1F VDP/PSG, $8000-$FFFF 32 KiB window into the 68000 space.
//
// It owns the master clock. The 68000 drives it; after every 68000 instruction the VDP, the
// Z80 and the sound chips catch up (SyncAfterM68kInstruction), and any access to the Z80 side
// or the VDP first brings that device up to the access time.
class MdMemoryManager final : public ISerializable
{
private:
	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;
	MdM68k* _m68k = nullptr;
	MdZ80* _z80 = nullptr;
	MdVdp* _vdp = nullptr;
	MdApu* _apu = nullptr;
	MdControlManager* _controlManager = nullptr;

	vector<uint8_t> _rom;
	uint32_t _romMask = 0;

	//Sega's bank-switching mapper (Super Street Fighter II, large homebrew): the 68000's cartridge
	//space is eight 512 KiB windows; $A130F3/5/7/9/B/D/F select the ROM bank shown in windows 1-7
	//(window 0 is fixed to bank 0). Enabled for ROMs over 4 MiB or a "SEGA SSF" system name.
	bool _hasBankMapper = false;
	uint8_t _romBank[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	__forceinline uint32_t MapRom(uint32_t addr)
	{
		return _hasBankMapper ? (((uint32_t)_romBank[(addr >> 19) & 0x07] << 19) | (addr & 0x7FFFF)) : addr;
	}
	//Serial EEPROM saves (MdEeprom.h): the chip, and where its lines sit on the cartridge bus
	unique_ptr<MdEepromDevice> _eeprom;
	uint32_t _eepromReadAddr = 0xFFFFFFFF;
	uint8_t _eepromReadBit = 0;
	uint32_t _eepromSdaAddr = 0;
	uint8_t _eepromSdaBit = 0;
	uint32_t _eepromSclAddr = 0;
	uint8_t _eepromSclBit = 0;
	bool _eepromScl = true;
	bool _eepromSda = true;
	//Set when the cartridge has battery RAM or an EEPROM, so ROM reads take one branch otherwise
	bool _hasCartSave = false;
	void InitEeprom(vector<uint8_t>& romData);
	void WriteEeprom(uint32_t addr, uint8_t value);

	uint8_t* _workRam = nullptr;
	uint8_t* _z80Ram = nullptr;
	uint8_t* _saveRam = nullptr;
	uint32_t _saveRamSize = 0;
	uint32_t _saveRamStart = 0;
	uint32_t _saveRamEnd = 0;
	bool _saveRamOddOnly = false;
	bool _saveRamEvenOnly = false;
	bool _isPal = false;
	bool _isOverseas = true;
	bool _hasTmss = false;

	uint64_t _masterClock = 0;

	//DRAM refresh (see ApplyRefresh): master clocks counted towards the next refresh, and the
	//master clock up to which the counter has been advanced
	uint32_t _refreshCounter = 0;
	uint64_t _refreshSyncClock = 0;
	MdMemoryManagerState _state = {};

	void SyncZ80();
	bool IsZ80Held();
	bool M68kOwnsZ80Bus() { return _state.Z80BusRequested; }

	uint8_t ReadZ80Area68k(uint32_t addr);
	void WriteZ80Area68k(uint32_t addr, uint8_t value);
	uint8_t ReadIo(uint32_t addr);
	void WriteIo(uint32_t addr, uint8_t value);
	uint16_t ReadControlRegister(uint32_t addr, uint16_t openBus);
	void WriteControlRegister(uint32_t addr, uint8_t value, bool upperLane);
	bool IsSaveRamAddress(uint32_t addr);
	uint8_t ReadSaveRam(uint32_t addr);
	void WriteSaveRam(uint32_t addr, uint8_t value);
	uint8_t ReadCartByte(uint32_t addr);
	//Cartridge bus read as the CPUs and DMA see it: ROM patch cheats (Game Genie, Action Replay) apply
	__forceinline uint8_t ReadCartBus(uint32_t addr);
	void TraceDevice(uint64_t clock, MdBusDevice device, MdBusOp op, uint32_t address, uint32_t value);

public:
	MdMemoryManager();
	~MdMemoryManager();

	void Init(Emulator* emu, MdConsole* console, vector<uint8_t>& romData, MdM68k* m68k, MdZ80* z80, MdVdp* vdp, MdApu* apu, MdControlManager* controlManager, bool isPal, bool isOverseas, bool hasTmss);

	MdMemoryManagerState& GetState() { return _state; }
	uint64_t GetMasterClock() { return _masterClock; }

	__forceinline void AdvanceM68k(uint32_t m68kClocks) { _masterClock += m68kClocks * MdTiming::M68kDivider; }
	//The 68000 waits for the VDP (FIFO full or 68000-to-VDP DMA). Refresh delays due before the wait
	//are applied first; refresh during the wait costs nothing extra, and a DMA restarts the counter.
	void AddM68kStall(uint64_t masterClocks, bool dma = false);
	void SyncAfterM68kInstruction();

	// DRAM refresh steals 2 68000 cycles every 128 68000 cycles. Model and figures are BlastEm's
	// (genesis.c, refresh_advance: REFRESH_INTERVAL 128, REFRESH_DELAY 2), the transitional
	// reference; hardware-unverified. The delay itself is time the counter keeps running in, so the
	// result does not depend on how often it is applied.
	__forceinline void ApplyRefresh()
	{
		constexpr uint32_t interval = 128 * MdTiming::M68kDivider;
		constexpr uint32_t delay = 2 * MdTiming::M68kDivider;
		uint64_t elapsed = _masterClock - _refreshSyncClock;
		if(_refreshCounter + elapsed >= interval) {
			uint64_t counter = _refreshCounter + elapsed;
			while(counter >= interval) {
				counter -= interval - delay;
				_masterClock += delay;
			}
			_refreshCounter = (uint32_t)counter;
		} else {
			_refreshCounter += (uint32_t)elapsed;
		}
		_refreshSyncClock = _masterClock;
	}
	void AcknowledgeM68kInterrupt(uint8_t level);
	//A light gun on `port` saw the beam (called by the VDP at the hit time)
	void OnLightgunHit(uint8_t port, uint64_t clock);
	MdControlManager* GetControlManager() { return _controlManager; }

	uint16_t M68kRead(bool upper, bool lower, uint32_t addr, M68kAccess access);
	void M68kWrite(bool upper, bool lower, uint32_t addr, uint16_t value, M68kAccess access);

	uint8_t Z80Read(uint16_t addr);
	void Z80Write(uint16_t addr, uint8_t value);

	//Side-effect-free accesses for the debugger and DMA source reads
	uint16_t M68kPeekWord(uint32_t addr);
	uint8_t DebugRead(uint32_t addr);
	void DebugWrite(uint32_t addr, uint8_t value);
	uint8_t DebugZ80Read(uint16_t addr);
	void DebugZ80Write(uint16_t addr, uint8_t value);

	//68000 bus word read used by 68000-to-VDP DMA (the VDP reads the bus while the 68000 waits)
	uint16_t DmaReadWord(uint32_t addr);

	AddressInfo GetAbsoluteAddress(uint32_t relAddr);
	int32_t GetRelativeAddress(AddressInfo& absAddress);
	AddressInfo GetZ80AbsoluteAddress(uint16_t relAddr);
	int32_t GetZ80RelativeAddress(AddressInfo& absAddress);

	uint8_t* GetSaveRam() { return _saveRam; }
	MdEepromDevice* GetEeprom() { return _eeprom.get(); }
	uint32_t GetSaveRamSize() { return _saveRamSize; }

	void Serialize(Serializer& s) override;
};
