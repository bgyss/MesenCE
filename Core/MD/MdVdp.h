#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;
class MdMemoryManager;

// Mega Drive VDP (YM7101), Mode 5. Sources: Sega Genesis Software Manual (VDP registers, ports,
// DMA, interrupts, status, access slots per line), plus the H/V counter sequences documented in
// Charles MacDonald's "Sega Genesis VDP documentation". Timing model (hardware-unverified details
// are listed in docs/research/md-verification-findings.md):
//  - a line is 3420 master clocks; a "line" here starts where the V counter increments and the
//    H interrupt is raised (H counter $84 in H32, $A5 in H40);
//  - each visible line is rendered when its active display starts, from the registers and
//    memories at that time (line granularity);
//  - CPU writes go through a 4-entry FIFO drained at the external access slots of each line
//    (16/18 per active line in H32/H40, 167/205 per blank line); a VRAM word uses two slots,
//    CRAM/VSRAM words one. A write into a full FIFO stalls the 68000 until a slot frees it.
//  - 68000-to-VDP DMA holds the 68000 for the transfer at the same slot rate; fill and copy run
//    on VDP time with the DMA busy flag set.
class MdVdp final : public ISerializable
{
public:
	static constexpr uint32_t MaxWidth = 320;
	static constexpr uint32_t MaxHeight = 480; //interlace mode 2 doubles the 240 lines

private:
	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;
	MdMemoryManager* _memoryManager = nullptr;

	MdVdpState _state = {};
	uint8_t* _vram = nullptr;
	uint16_t _cram[64] = {};
	uint16_t _vsram[40] = {};

	uint16_t* _outputBuffers[2] = {};
	uint16_t* _currentBuffer = nullptr;
	uint32_t _frameWidth = 256;
	uint32_t _frameHeight = 224;
	uint32_t _lastFrameWidth = 256;
	uint32_t _lastFrameHeight = 224;

	bool _isPal = false;
	uint32_t _linesPerFrame = 262;

	uint64_t _lineStartClock = 0;
	uint64_t _z80IntEndClock = 0;
	bool _lineRendered = false;
	bool _vintRaisedThisFrame = false;

	//FIFO completion times, oldest first
	uint64_t _fifo[4] = {};
	uint8_t _fifoCount = 0;
	uint64_t _fifoLastCompletion = 0;

	//Per-line sprite evaluation results
	struct LineSprite
	{
		int16_t X;
		uint8_t Width; //in cells
		uint8_t Height; //in cells
		uint16_t Attr;
		uint16_t RowInSprite;
	};

	uint32_t GetDotsPerLine() { return IsH40() ? 420 : 342; }
	bool IsH40() { return (_state.Regs[12] & 0x81) != 0; }
	bool IsV30() { return (_state.Regs[1] & 0x08) != 0; }
	bool IsDisplayEnabled() { return (_state.Regs[1] & 0x40) != 0; }
	bool IsDmaEnabled() { return (_state.Regs[1] & 0x10) != 0; }
	uint8_t GetInterlaceMode() { return (_state.Regs[12] >> 1) & 0x03; }
	uint16_t GetActiveLines() { return IsV30() ? 240 : 224; }
	uint16_t GetVBlankStart() { return GetActiveLines(); }

	uint16_t GetHCounterForClock(uint32_t hclock);
	uint16_t GetVCounter();
	bool IsHBlank(uint32_t hclock);
	bool IsInVBlank();

	uint64_t GetNextSlot(uint64_t clock);
	uint64_t ConsumeSlots(uint64_t start, uint32_t slots);
	void PushFifo(uint8_t code);
	void RetireFifo(uint64_t clock);

	void ProcessLineStart();
	void RenderLine(uint16_t line);
	void RenderSprites(uint16_t line, uint8_t* spritePixels, uint8_t* spritePriority, bool interlace2);
	void EndFrame();

	void WriteData(uint16_t value);
	uint16_t ReadData();
	void WriteControl(uint16_t value);
	void WriteRegister(uint8_t reg, uint8_t value);
	void WriteMemory(uint8_t code, uint16_t address, uint16_t value);
	void RunDma68k();
	void RunDmaFill(uint16_t value);
	void RunDmaCopy();
	uint32_t GetDmaLength();
	void UpdateCachedState();

public:
	MdVdp();
	~MdVdp();

	void Init(Emulator* emu, MdConsole* console, MdMemoryManager* memoryManager, bool isPal);
	void SetRegion(bool isPal);

	void Run(uint64_t masterClock);

	//openBus: the value on the data bus lines the VDP does not drive (status bits 15-10)
	uint16_t ReadPort(uint8_t port, uint16_t openBus = 0);
	void WritePort(uint8_t port, uint16_t value, bool upper, bool lower);

	uint8_t GetM68kInterruptLevel();
	void AcknowledgeInterrupt(uint8_t level);
	bool IsZ80InterruptActive() { return _state.Z80IntActive; }

	uint32_t GetFrameCount() { return _state.FrameCount; }
	uint16_t GetScanline() { return _state.Scanline; }
	uint32_t GetLinesPerFrame() { return _linesPerFrame; }
	uint16_t* GetScreenBuffer(bool previousBuffer) { return previousBuffer ? (_currentBuffer == _outputBuffers[0] ? _outputBuffers[1] : _outputBuffers[0]) : _currentBuffer; }
	uint32_t GetFrameWidth() { return _lastFrameWidth; }
	uint32_t GetFrameHeight() { return _lastFrameHeight; }

	MdVdpState& GetState() { return _state; }
	uint8_t* GetVram() { return _vram; }
	uint16_t* GetCram() { return _cram; }
	uint16_t* GetVsram() { return _vsram; }

	void DebugWriteRegister(uint8_t reg, uint8_t value) { WriteRegister(reg, value); }

	void Serialize(Serializer& s) override;
};
