#pragma once
#include "pch.h"
#include "MD/Sh2/Sh2Core.h"
#include "Utilities/ISerializable.h"
#include "Debugger/DebugTypes.h"
#include "MD/MdTypes.h"

class MdConsole;
class Emulator;
class MdMemoryManager;
class MdVdp;
class M32xUnit;

// One of the 32X's two SH-2s (SH7604) on the unit's bus.
class M32xSh2 final : public Sh2Core, public ISerializable
{
private:
	M32xUnit* _unit = nullptr;
	uint8_t _index = 0; //0 = master, 1 = slave

public:
	void Init(M32xUnit* unit, uint8_t index)
	{
		_unit = unit;
		_index = index;
	}

	uint8_t BusRead8(uint32_t addr) override;
	uint16_t BusRead16(uint32_t addr) override;
	uint32_t BusRead32(uint32_t addr) override;
	void BusWrite8(uint32_t addr, uint8_t value) override;
	void BusWrite16(uint32_t addr, uint16_t value) override;
	void BusWrite32(uint32_t addr, uint32_t value) override;
	uint8_t Peek8(uint32_t addr) override;

	//Debugger
	void OnInstructionStart(uint32_t pc, uint16_t op) override;
	void OnDebugRead(uint32_t addr, uint32_t value, int size) override;
	void OnDebugWrite(uint32_t addr, uint32_t value, int size) override;
	void OnInterruptTaken(uint32_t returnPc, uint32_t handlerPc) override;
	M32xSh2State GetDebugState();
	void SetDebugState(M32xSh2State& state);
	uint32_t GetInstructionPc() { return _instPc; }

	void Serialize(Serializer& s) override { Sh2Core::Serialize(s); }
};

// Sega 32X / Mega 32X ("Mars"): two SH-2s, 256 KiB SDRAM, the 32X VDP (two 128 KiB frame
// buffers, 256-colour palette), PWM sound, the 68000-to-SH-2 DREQ FIFO and the communication
// registers. An optional unit inside MdConsole, like the Sega CD (Mcd/McdUnit.h).
//
// Sources (docs/research/32x-verification-findings.md): Sega, 32X Hardware Manual (US) for the
// memory maps, registers, interrupts, boot protocol and VDP/PWM behaviour; ares (ares/md/m32x,
// ISC, 140c0a3) as the reference implementation, cited where it is the only source.
//
// Timing: the SH-2s run at the Mega Drive master clock x 3/7 (findings S0-F2). They are kept in
// SH-2 cycles (Sh2Core::GetCycleCount) and catch up to the 68000 after every 68000 instruction
// and before every 68000 access to the unit. Between them, the CPU that is behind always runs
// next, one instruction at a time, so their accesses to SDRAM and the shared registers apply in
// time order (to within one instruction).
class M32xUnit final : public ISerializable
{
public:
	static constexpr uint32_t SdramSize = 0x40000;
	//The SH-2 address space the debugger shows (boot ROM to the end of SDRAM, physical addresses)
	static constexpr uint32_t Sh2DebugSpaceSize = 0x06040000;
	static constexpr uint32_t FrameBufferSize = 0x20000; //bytes, per buffer
	static constexpr uint32_t PaletteSize = 0x200; //bytes

	//Test hooks (mesen-headless --md-32x, --md-32x-bios): attach the unit to a cartridge that
	//doesn't say it is a 32X game, and use the real Sega boot ROMs from a folder
	static void SetAttachedOverride(bool attached) { _attachedOverride = attached; }
	static bool IsAttachedOverride() { return _attachedOverride; }
	static void SetBiosFolderOverride(string folder) { _biosFolderOverride = folder; }
	//The three Sega boot ROMs (32X_M_BIOS.bin, 32X_S_BIOS.bin, 32X_G_BIOS.bin); empty if missing
	static void LoadBios(Emulator* emu, vector<uint8_t>& master, vector<uint8_t>& slave, vector<uint8_t>& vectors);

	//A 32X cartridge: the "MARS" user header at $3C0 or "32X" in the system name at $100
	static bool Is32xRom(vector<uint8_t>& rom);

private:
	static inline bool _attachedOverride = false;
	static inline string _biosFolderOverride;

	MdConsole* _console = nullptr;
	MdMemoryManager* _memoryManager = nullptr;
	MdVdp* _vdp = nullptr;
	bool _isPal = false;

	M32xSh2 _sh2[2];
	vector<uint8_t> _bootRom[2]; //master 2 KiB, slave 1 KiB
	vector<uint8_t> _vectorRom; //68000 vectors, 256 bytes
	bool _replacementBios = false;

	uint8_t _sdram[SdramSize] = {};
	uint8_t _frameBuffer[2][FrameBufferSize] = {};
	uint16_t _palette[256] = {};

	//$A15100 adapter control
	bool _adapterEnable = false; //ADEN
	bool _sh2Run = false; //RES: 0 = SH-2s held in reset
	bool _fm = false; //FM: VDP access, 0 = 68000, 1 = SH-2
	uint32_t _vectorLevel4 = 0; //68000 level 4 vector ($000070), writable while ADEN = 1 (ares)
	uint8_t _romBank = 0;
	bool _cartridgeMode = false; //$A1511A CM
	uint16_t _comm[8] = {};

	//SH-2 interrupt requests (per CPU) and masks
	enum IrqSource : uint8_t
	{
		IrqPwm = 0x01,
		IrqCmd = 0x02,
		IrqH = 0x04,
		IrqV = 0x08,
		IrqVres = 0x10,
	};
	uint8_t _irqActive[2] = {};
	uint8_t _irqMask[2] = {}; //interrupt mask register bits 0-3 (PWM, CMD, H, V)
	bool _hintInVblank = false; //HEN
	uint8_t _hCount = 0;
	uint8_t _hCounter = 0;

	//DREQ: 68000 -> SH-2 FIFO
	bool _dreqRv = false; //RV: the cartridge is in the 68000's original map (for VDP DMA)
	bool _dreq68s = false; //68S: the 68000 writes the FIFO
	bool _dreqDma = false; //DMA bit (68000 side)
	uint32_t _dreqSource = 0;
	uint32_t _dreqDest = 0;
	uint16_t _dreqLength = 0;
	static constexpr uint8_t FifoSize = 8;
	uint16_t _fifo[FifoSize] = {};
	uint8_t _fifoHead = 0;
	uint8_t _fifoCount = 0;

	//VDP
	uint8_t _vdpMode = 0; //M: 0 blank, 1 packed, 2 direct, 3 run length
	bool _vdpLines240 = false;
	bool _vdpPriority = false; //PRI: 1 = 32X in front
	bool _vdpDotShift = false;
	uint8_t _fillLength = 0;
	uint16_t _fillAddress = 0;
	uint16_t _fillData = 0;
	uint64_t _fillBusyUntil = 0; //SH-2 cycles
	bool _fbSelect = false; //FS as written
	bool _fbDisplay = false; //the buffer being displayed (FS swaps at VBlank)
	bool _hblank = false;
	bool _vblank = true;
	//Display settings latched at the start of a line
	uint8_t _lineMode = 0;
	bool _linePriority = false;
	bool _lineDotShift = false;
	bool _lineLines240 = false;

	//PWM
	uint8_t _pwmLMode = 0;
	uint8_t _pwmRMode = 0;
	bool _pwmMono = false;
	bool _pwmDreqEnable = false; //RTP: the timer requests DMA (DREQ1)
	uint8_t _pwmTimer = 0;
	uint16_t _pwmCycle = 0;
	uint8_t _pwmPeriods = 0;
	uint64_t _pwmNextTick = 0; //SH-2 cycle of the next PWM sample
	uint16_t _pwmFifo[2][3] = {};
	uint8_t _pwmFifoCount[2] = {};
	uint16_t _pwmOut[2] = {};
	uint16_t _pwmLatch[3] = {};
	//PWM output levels with the master clock of the PWM tick that produced them, consumed by
	//MdApu at each of its samples' clocks (the 32X runs ahead of the APU; mixing the current
	//output instead held one level per batch, i.e. per frame)
	struct PwmSample
	{
		uint64_t Clock;
		int16_t Left;
		int16_t Right;
	};
	std::vector<PwmSample> _pwmPending;
	PwmSample _pwmCurrent = {};
	void ComputePwmLevels(int32_t& left, int32_t& right);
	bool _dreq1Pulse = false;
	bool _syncingVdp = false;

	uint8_t& FbByte(bool back, uint32_t byteAddr) { return _frameBuffer[back ? !_fbDisplay : _fbDisplay][byteAddr & (FrameBufferSize - 1)]; }
	uint16_t FbWord(bool display, uint32_t wordIndex)
	{
		uint8_t* fb = _frameBuffer[display ? _fbDisplay : !_fbDisplay];
		uint32_t a = (wordIndex << 1) & (FrameBufferSize - 1);
		return (uint16_t)(fb[a] << 8 | fb[a + 1]);
	}
	bool InHBlank();
	bool FrameBufferBusy(uint64_t sh2Cycle);
	bool PaletteBusy();

	void UpdateIrl(int cpu);
	void UpdateIrls();
	void RaiseIrq(uint8_t source);
	uint64_t Sh2Target(uint64_t masterClock);
	void RunPwm(uint64_t sh2Cycle);
	void PwmFifoPush(int ch, uint16_t value);
	uint16_t FifoPop();
	void UpdateDreq0();

	uint16_t ReadCart(uint32_t addr);
	uint16_t ReadSysReg(int cpu, uint32_t addr);
	void WriteSysReg(int cpu, uint32_t addr, bool upper, bool lower, uint16_t value);
	uint16_t ReadVdpReg(uint32_t reg);
	void WriteVdpReg(uint32_t reg, bool upper, bool lower, uint16_t value);
	void WritePwmReg(uint32_t reg, bool upper, bool lower, uint16_t value, bool fromSh2);
	uint16_t ReadPwmReg(uint32_t reg);
	void FrameBufferWrite(uint32_t offset, bool upper, bool lower, uint16_t value, bool overwrite);
	void Fill();
	void SelectFrameBuffer(bool select);

public:
	M32xUnit() = default;

	//Boot ROMs (master, slave, 68000 vectors): the user's Sega files, or MesenCE's replacements
	//when any is missing (M32xBootRom.h)
	void Init(MdConsole* console, MdMemoryManager* memoryManager, bool isPal, vector<uint8_t> masterBios, vector<uint8_t> slaveBios, vector<uint8_t> vectorBios);
	bool UsesReplacementBios() { return _replacementBios; }

	//Runs both SH-2s and the PWM until `masterClock`
	void Run(uint64_t masterClock);

	//68000 side
	bool IsAdapterEnabled() { return _adapterEnable; }
	bool IsRv() { return _dreqRv; }
	//$000000-$0000FF while ADEN = 1: the vector ROM (the level 4 vector at $70 is writable)
	uint16_t ReadVector(uint32_t addr);
	void WriteVector(uint32_t addr, bool upper, bool lower, uint16_t value);
	//$800000-$9FFFFF while ADEN = 1
	uint16_t MainRead(uint32_t addr, uint16_t openBus);
	void MainWrite(uint32_t addr, bool upper, bool lower, uint16_t value);
	//$A15100-$A153FF registers and $A130EC "MARS"
	bool IsMainRegister(uint32_t addr) { return (addr >= 0xA15100 && addr < 0xA15400) || (addr >= 0xA130EC && addr < 0xA130F0); }
	uint16_t MainReadRegister(uint32_t addr, uint16_t openBus);
	void MainWriteRegister(uint32_t addr, bool upper, bool lower, uint16_t value);
	//Side-effect-free 68000 read of $800000-$9FFFFF (debugger, VDP DMA)
	uint16_t PeekMain(uint32_t addr);

	Emulator* GetEmulator();
	//Debugger: SH-2 addresses (`cpu` = 0 master, 1 slave) <-> memories
	AddressInfo ShGetAbsoluteAddress(int cpu, uint32_t addr);
	int32_t ShGetRelativeAddress(AddressInfo& absAddress);
	void ShDebugWrite(uint32_t addr, uint8_t value);
	//Debugger: the 68000's view of $880000-$9FFFFF and the frame buffer
	AddressInfo MainGetAbsoluteAddress(uint32_t addr);

	//SH-2 bus ($00000000-$1FFFFFFF physical)
	uint16_t ShRead16(int cpu, uint32_t addr);
	void ShWrite16(int cpu, uint32_t addr, bool upper, bool lower, uint16_t value);
	uint8_t ShPeek8(uint32_t addr);

	//Mega Drive VDP signals and composition
	void OnHBlank(bool active);
	void OnVBlank(bool active);
	void OnLineStart();
	//Overlays the 32X picture on the VDP's line (bit 11 of `line` marks the VDP's backdrop)
	void CompositeLine(uint16_t* line, uint32_t y, uint32_t width);
	void OnResetButton();

	//PWM output at master clock `clock` (MdApu), 16-bit
	void GetAudioOutput(uint64_t clock, int32_t& left, int32_t& right);

	M32xSh2& GetSh2(int i) { return _sh2[i]; }
	uint8_t* GetSdram() { return _sdram; }
	uint8_t* GetFrameBuffer(int i) { return _frameBuffer[i]; }
	uint16_t* GetPalette() { return _palette; }
	uint16_t* GetComm() { return _comm; }

	void Serialize(Serializer& s) override;
};
