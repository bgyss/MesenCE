#include "pch.h"
#include "MD/MdVdp.h"
#include "MD/MdConsole.h"
#include "MD/MdMemoryManager.h"
#include "MD/M32x/M32xUnit.h"
#include "MD/MdControlManager.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/MemoryType.h"
#include "Shared/RewindManager.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/NotificationManager.h"
#include "Shared/EventType.h"
#include "Debugger/Debugger.h"
#include "Debugger/PpuTools.h"
#include "Shared/RenderedFrame.h"
#include "Shared/BaseControlManager.h"
#include "Utilities/Serializer.h"

namespace
{
	//Master clocks from the V counter increment to the first active pixel (internal H = 0).
	constexpr uint32_t RenderOffsetH32 = 78 * 10;
	constexpr uint32_t RenderOffsetH40 = 90 * 3420 / 420;
	//The vertical interrupt is raised 4 pixels into the first VBlank line's active period.
	constexpr uint32_t VIntDelayH32 = 4 * 10;
	constexpr uint32_t VIntDelayH40 = 4 * 3420 / 420;

	constexpr uint8_t ShadeNormal = 0;
	constexpr uint8_t ShadeShadow = 1;
	constexpr uint8_t ShadeHighlight = 2;
}

MdVdp::MdVdp()
{
}

MdVdp::~MdVdp()
{
	delete[] _vram;
	delete[] _outputBuffers[0];
	delete[] _outputBuffers[1];
}

void MdVdp::Init(Emulator* emu, MdConsole* console, MdMemoryManager* memoryManager, bool isPal)
{
	_emu = emu;
	_console = console;
	_memoryManager = memoryManager;

	_vram = new uint8_t[0x10000];
	memset(_vram, 0, 0x10000);
	_outputBuffers[0] = new uint16_t[MaxWidth * MaxHeight];
	_outputBuffers[1] = new uint16_t[MaxWidth * MaxHeight];
	memset(_outputBuffers[0], 0, MaxWidth * MaxHeight * sizeof(uint16_t));
	memset(_outputBuffers[1], 0, MaxWidth * MaxHeight * sizeof(uint16_t));
	_currentBuffer = _outputBuffers[0];

	_emu->RegisterMemory(MemoryType::MdVideoRam, _vram, 0x10000);
	_emu->RegisterMemory(MemoryType::MdPaletteRam, _cram, sizeof(_cram));
	_emu->RegisterMemory(MemoryType::MdVScrollRam, _vsram, sizeof(_vsram));

	_state = {};
	SetRegion(isPal);
	_lineStartClock = 0;
	_state.Scanline = 0;
	ProcessLineStart();
}

void MdVdp::SetRegion(bool isPal)
{
	_isPal = isPal;
	_linesPerFrame = isPal ? MdTiming::PalLines : MdTiming::NtscLines;
}

uint16_t MdVdp::GetHCounterForClock(uint32_t hclock)
{
	if(IsH40()) {
		uint32_t dot = hclock * 420 / MdTiming::ClocksPerLine;
		uint32_t idx = (330 + dot) % 420;
		return idx < 0x16D ? idx : (idx - 0x16D + 0x1C9);
	} else {
		uint32_t dot = hclock / 10;
		uint32_t idx = (264 + dot) % 342;
		return idx < 0x128 ? idx : (idx - 0x128 + 0x1D2);
	}
}

bool MdVdp::IsHBlank(uint32_t hclock)
{
	uint16_t h = GetHCounterForClock(hclock);
	if(IsH40()) {
		return h >= 0x164 || h < 0x00C;
	} else {
		return h >= 0x126 || h < 0x00A;
	}
}

uint16_t MdVdp::GetVCounter()
{
	uint16_t line = _state.Scanline;
	uint16_t v;
	if(_isPal) {
		if(IsV30()) {
			v = line <= 0x10A ? line : (line - 0x10B + 0x1D2);
		} else {
			v = line <= 0x102 ? line : (line - 0x103 + 0x1CA);
		}
	} else {
		if(IsV30()) {
			v = line & 0xFF; //V30 on NTSC: the counter wraps without a jump
		} else {
			v = line <= 0xEA ? line : (line - 0xEB + 0x1E5);
		}
	}
	switch(GetInterlaceMode()) {
		case 1: return (v & 0xFE) | ((v >> 8) & 0x01);
		case 3: return ((v << 1) & 0xFE) | ((v >> 7) & 0x01);
		default: return v & 0xFF;
	}
}

bool MdVdp::IsInVBlank()
{
	return _state.Scanline >= GetVBlankStart() && _state.Scanline < _linesPerFrame - 1;
}

uint64_t MdVdp::ConsumeSlots(uint64_t start, uint32_t slots)
{
	//Walk the external access slots from `start` on; the line structure is extrapolated from the
	//current line with the current display settings.
	uint64_t lineStart = _lineStartClock;
	uint32_t line = _state.Scanline;
	while(start >= lineStart + MdTiming::ClocksPerLine) {
		lineStart += MdTiming::ClocksPerLine;
		line = (line + 1) % _linesPerFrame;
	}
	uint64_t t = start;
	while(true) {
		bool blank = !IsDisplayEnabled() || line >= GetActiveLines();
		uint32_t perLine = blank ? (IsH40() ? 205 : 167) : (IsH40() ? 18 : 16);
		uint32_t spacing = MdTiming::ClocksPerLine / perLine;
		uint32_t offset = (uint32_t)(t - lineStart);
		uint32_t first = (offset + spacing - 1) / spacing;
		if(first < perLine) {
			uint32_t available = perLine - first;
			if(slots <= available) {
				return lineStart + (uint64_t)(first + slots - 1) * spacing + 1;
			}
			slots -= available;
		}
		lineStart += MdTiming::ClocksPerLine;
		line = (line + 1) % _linesPerFrame;
		t = lineStart;
	}
}

uint64_t MdVdp::GetNextSlot(uint64_t clock)
{
	return ConsumeSlots(clock, 1);
}

void MdVdp::RetireFifo(uint64_t clock)
{
	while(_fifoCount > 0 && _fifo[0] <= clock) {
		for(int i = 1; i < _fifoCount; i++) {
			_fifo[i - 1] = _fifo[i];
		}
		_fifoCount--;
	}
}

void MdVdp::PushFifo(uint8_t code)
{
	uint64_t now = _memoryManager->GetMasterClock();
	RetireFifo(now);
	if(_fifoCount == 4) {
		//The 68000 waits for the oldest entry to reach its slot
		uint64_t stall = _fifo[0] - now;
		_memoryManager->AddM68kStall(stall);
		now += stall;
		RetireFifo(now);
	}
	uint32_t slots = (code & 0x0F) == 0x01 ? 2 : 1;
	uint64_t start = std::max(now, _fifoLastCompletion);
	uint64_t done = ConsumeSlots(start, slots);
	_fifo[_fifoCount++] = done;
	_fifoLastCompletion = done;
}

void MdVdp::Run(uint64_t masterClock)
{
	while(true) {
		uint32_t renderOffset = IsH40() ? RenderOffsetH40 : RenderOffsetH32;
		if(!_lineRendered && masterClock >= _lineStartClock + renderOffset) {
			_lineRendered = true;
			if(_state.Scanline < GetActiveLines()) {
				BeginLine(_state.Scanline);
			}
		}

		if(_state.Scanline == GetVBlankStart() && !_vintRaisedThisFrame) {
			uint32_t delay = renderOffset + (IsH40() ? VIntDelayH40 : VIntDelayH32);
			if(masterClock >= _lineStartClock + delay) {
				_vintRaisedThisFrame = true;
				_state.VIntPending = true;
				_state.Z80IntActive = true;
				_z80IntEndClock = _lineStartClock + delay + MdTiming::ClocksPerLine;
			}
		}

		if(_state.Z80IntActive && masterClock >= _z80IntEndClock) {
			_state.Z80IntActive = false;
		}

		uint64_t lineEnd = _lineStartClock + MdTiming::ClocksPerLine;
		if(_lightgunClock <= masterClock && _lightgunClock < lineEnd) {
			uint64_t hit = _lightgunClock;
			_lightgunClock = UINT64_MAX;
			_memoryManager->OnLightgunHit(_lightgunPort, hit);
		}
		if(masterClock < lineEnd) {
			_state.HClock = (uint16_t)(masterClock - _lineStartClock);
			break;
		}

		if(!_lineRendered && _state.Scanline < GetActiveLines()) {
			BeginLine(_state.Scanline);
		}
		FinishLine();
		_lineStartClock = lineEnd;
		_state.Scanline++;
		if(_state.Scanline >= _linesPerFrame) {
			_state.Scanline = 0;
			EndFrame();
		}
		ProcessLineStart();
	}
}

void MdVdp::ProcessLineStart()
{
	_lineRendered = false;
	uint16_t line = _state.Scanline;
	if(_emu->IsDebugging()) {
		//Debugger viewers set to refresh at this line
		PpuTools* tools = _emu->InternalGetDebugger()->GetPpuTools(CpuType::MdM68k);
		if(tools && tools->HasOpenedViewer()) {
			tools->UpdateViewers(line, 0);
		}
	}
	if(_m32x) {
		//The SH-2s reach the line's start before its events: a long 68000 stall (VDP DMA) makes the
		//VDP catch up several lines at once, and each line's H interrupt must reach the SH-2s
		_m32x->Run(_lineStartClock);
		_m32x->OnLineStart();
		if(line == 0) {
			_m32x->OnVBlank(false);
		} else if(line == GetVBlankStart()) {
			_m32x->OnVBlank(true);
		}
		_m32x->OnHBlank(true);
	}
	if(line == 0) {
		_vintRaisedThisFrame = false;
		_frameWidth = IsH40() ? 320 : 256;
		_frameHeight = GetActiveLines() * (GetInterlaceMode() == 3 ? 2 : 1);
	}

	//Light gun: schedule the moment the gun pulls TH low for this line (a hit whose latency carried
	//it past the end of the previous line is still pending and fires first)
	if(_lightgunClock == UINT64_MAX && line < GetActiveLines()) {
		MdControlManager* controlManager = _memoryManager->GetControlManager();
		if(controlManager->HasLightgun()) {
			uint16_t row = GetInterlaceMode() == 3 ? (line * 2 + (_state.OddFrame ? 1 : 0)) : line;
			uint8_t port = 0;
			int32_t x = controlManager->GetLightgunX(row, port);
			if(x >= 0 && x < (IsH40() ? 320 : 256)) {
				uint32_t activeStart = IsH40() ? RenderOffsetH40 : RenderOffsetH32;
				_lightgunPort = port;
				_lightgunClock = _lineStartClock + activeStart + (uint64_t)x * (IsH40() ? 8 : 10) + controlManager->GetLightgunLatency(port, IsH40());
			}
		}
	}

	//The H interrupt counter counts down on lines 0 through the first VBlank line, and is
	//reloaded on every other line.
	if(line <= GetVBlankStart()) {
		if(_state.HIntCounter == 0) {
			_state.HIntCounter = _state.Regs[10];
			_state.HIntPending = true;
		} else {
			_state.HIntCounter--;
		}
	} else {
		_state.HIntCounter = _state.Regs[10];
	}
}

void MdVdp::EndFrame()
{
	_lastFrameWidth = _frameWidth;
	_lastFrameHeight = _frameHeight;
	_state.FrameCount++;
	if(GetInterlaceMode() & 0x01) {
		_state.OddFrame = !_state.OddFrame;
	} else {
		_state.OddFrame = false;
	}

	//Frame events for the debugger and Lua (emu.eventType.endFrame/startFrame) and the UI's frame
	//notification. MesenCE's MD frame boundary is the wrap to line 0, so both events fire here.
	_emu->ProcessEvent(EventType::EndFrame, CpuType::MdM68k);
	_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::PpuFrameDone);

	bool rewinding = _emu->GetRewindManager()->IsRewinding();
	RenderedFrame frame(_currentBuffer, _lastFrameWidth, _lastFrameHeight, 1.0, _state.FrameCount, _console->GetControlManager()->GetPortStates());
	_emu->GetVideoDecoder()->UpdateFrame(frame, rewinding, rewinding);

	_emu->ProcessEndOfFrame();
	_console->ProcessEndOfFrame();

	_currentBuffer = _currentBuffer == _outputBuffers[0] ? _outputBuffers[1] : _outputBuffers[0];
	_emu->ProcessEvent(EventType::StartFrame, CpuType::MdM68k);
}

uint8_t MdVdp::GetM68kInterruptLevel()
{
	if(_state.VIntPending && (_state.Regs[1] & 0x20)) {
		return 6;
	}
	if(_state.HIntPending && (_state.Regs[0] & 0x10)) {
		return 4;
	}
	if(_extIntPending && (_state.Regs[11] & 0x08)) {
		return 2;
	}
	return 0;
}

void MdVdp::TriggerExternalInterrupt(uint64_t clock)
{
	if(_state.Regs[11] & 0x08) {
		_extIntPending = true;
	}
	if(_state.Regs[0] & 0x02) {
		uint16_t h = GetHCounterForClock((uint32_t)(clock - _lineStartClock)) >> 1;
		_state.HvLatch = (GetVCounter() << 8) | (h & 0xFF);
		_state.HvLatched = true;
	}
}

void MdVdp::AcknowledgeInterrupt(uint8_t level)
{
	if(level == 6) {
		_state.VIntPending = false;
	} else if(level == 4) {
		_state.HIntPending = false;
	} else if(level == 2) {
		_extIntPending = false;
	}
}

uint16_t MdVdp::ReadPort(uint8_t port, uint16_t openBus)
{
	port &= 0x1F;
	if(port < 0x04) {
		_state.CommandPending = false;
		return ReadData();
	} else if(port < 0x08) {
		_state.CommandPending = false;
		uint64_t now = _memoryManager->GetMasterClock();
		RetireFifo(now);
		uint32_t hclock = (uint32_t)(now - _lineStartClock);
		//Bits 15-10 are not driven by the VDP: the 68000 sees its prefetched word there
		uint16_t status = openBus & 0xFC00;
		status |= _fifoCount == 0 ? 0x0200 : 0;
		status |= _fifoCount == 4 ? 0x0100 : 0;
		status |= _state.VIntPending ? 0x0080 : 0;
		status |= _state.SpriteOverflow ? 0x0040 : 0;
		status |= _state.SpriteCollision ? 0x0020 : 0;
		status |= _state.OddFrame ? 0x0010 : 0;
		status |= (IsInVBlank() || !IsDisplayEnabled()) ? 0x0008 : 0;
		status |= IsHBlank(hclock) ? 0x0004 : 0;
		status |= (now < _state.DmaBusyUntil) ? 0x0002 : 0;
		status |= _isPal ? 0x0001 : 0;
		_state.SpriteOverflow = false;
		_state.SpriteCollision = false;
		return status;
	} else if(port < 0x10) {
		if(_state.HvLatched) {
			return _state.HvLatch;
		}
		uint64_t now = _memoryManager->GetMasterClock();
		uint16_t h = GetHCounterForClock((uint32_t)(now - _lineStartClock)) >> 1;
		return (GetVCounter() << 8) | (h & 0xFF);
	}
	return 0xFFFF;
}

void MdVdp::WritePort(uint8_t port, uint16_t value, bool upper, bool lower)
{
	port &= 0x1F;
	if(!(upper && lower)) {
		//Byte writes put the same byte on both halves of the data bus
		uint8_t b = upper ? (value >> 8) : (uint8_t)value;
		value = (b << 8) | b;
	}
	if(port < 0x04) {
		_state.CommandPending = false;
		WriteData(value);
	} else if(port < 0x08) {
		WriteControl(value);
	}
}

void MdVdp::WriteControl(uint16_t value)
{
	if(!_state.CommandPending) {
		if((value & 0xC000) == 0x8000) {
			WriteRegister((value >> 8) & 0x1F, value & 0xFF);
			return;
		}
		_state.CommandPending = true;
		_state.FirstCommandWord = value;
		_state.Address = (_state.Address & 0xC000) | (value & 0x3FFF);
		_state.Code = (_state.Code & 0x3C) | (value >> 14);
		return;
	}

	_state.CommandPending = false;
	_state.Address = (_state.Address & 0x3FFF) | ((value & 0x03) << 14);
	_state.Code = (_state.Code & 0x03) | ((value >> 2) & 0x3C);

	if((_state.Code & 0x20) && IsDmaEnabled()) {
		switch(_state.Regs[23] >> 6) {
			case 0:
			case 1: RunDma68k(); break;
			case 2: _state.DmaFillPending = true; break;
			case 3: RunDmaCopy(); break;
		}
	} else if(!(_state.Code & 0x01)) {
		//Read command: the VDP prefetches the first word
		_state.ReadBuffer = 0;
	}
}

void MdVdp::WriteRegister(uint8_t reg, uint8_t value)
{
	if(reg >= 24) {
		return;
	}
	CatchUp(_memoryManager->GetMasterClock());
	if(!(_state.Regs[1] & 0x04) && reg > 10) {
		//Mode 4 (SMS compatibility) only has registers 0-10
		return;
	}
	if(reg == 0 && (value & 0x02) && !(_state.Regs[0] & 0x02)) {
		//Latch the H/V counter
		uint64_t now = _memoryManager->GetMasterClock();
		uint16_t h = GetHCounterForClock((uint32_t)(now - _lineStartClock)) >> 1;
		_state.HvLatch = (GetVCounter() << 8) | (h & 0xFF);
		_state.HvLatched = true;
	} else if(reg == 0 && !(value & 0x02)) {
		_state.HvLatched = false;
	}
	_state.Regs[reg] = value;
}

void MdVdp::WriteMemory(uint8_t code, uint16_t address, uint16_t value)
{
	switch(code & 0x0F) {
		case 0x01: {
			//VRAM: a word written to an odd address has its bytes swapped
			uint16_t addr = address & 0xFFFE;
			if(address & 1) {
				value = (value >> 8) | (value << 8);
			}
			_vram[addr] = value >> 8;
			_vram[addr | 1] = (uint8_t)value;
			break;
		}

		case 0x03:
			_cram[(address >> 1) & 0x3F] = value & 0x0EEE;
			break;

		case 0x05: {
			uint8_t index = (address >> 1) & 0x3F;
			if(index < 40) {
				_vsram[index] = value & 0x07FF;
			}
			break;
		}
	}
}

void MdVdp::WriteData(uint16_t value)
{
	uint8_t code = _state.Code;
	if((code & 0x0F) == 0x01 || (code & 0x0F) == 0x03 || (code & 0x0F) == 0x05) {
		PushFifo(code);
		//The write lands when its FIFO entry reaches an access slot
		CatchUp(_fifoLastCompletion);
		WriteMemory(code, _state.Address, value);
	}
	_state.Address += _state.Regs[15];
	if(_state.DmaFillPending) {
		_state.DmaFillPending = false;
		RunDmaFill(value);
	}
}

uint16_t MdVdp::ReadData()
{
	uint16_t value = 0;
	switch(_state.Code & 0x0F) {
		case 0x00: value = (_vram[_state.Address & 0xFFFE] << 8) | _vram[(_state.Address & 0xFFFE) | 1]; break;
		case 0x04: {
			uint8_t index = (_state.Address >> 1) & 0x3F;
			value = index < 40 ? _vsram[index] : _vsram[0];
			break;
		}
		case 0x08: value = _cram[(_state.Address >> 1) & 0x3F]; break;
		case 0x0C: value = _vram[_state.Address ^ 1]; break; //undocumented 8-bit VRAM read
		default: return 0;
	}
	_state.Address += _state.Regs[15];
	return value;
}

uint32_t MdVdp::GetDmaLength()
{
	uint32_t length = _state.Regs[19] | (_state.Regs[20] << 8);
	return length ? length : 0x10000;
}

void MdVdp::RunDma68k()
{
	CatchUp(_memoryManager->GetMasterClock());
	uint32_t length = GetDmaLength();
	uint32_t source = ((_state.Regs[23] & 0x7F) << 17) | (_state.Regs[22] << 9) | (_state.Regs[21] << 1);
	uint8_t code = _state.Code & 0x0F;
	uint64_t now = _memoryManager->GetMasterClock();
	uint32_t slotsPerWord = code == 0x01 ? 2 : 1;

	for(uint32_t i = 0; i < length; i++) {
		uint16_t value = _memoryManager->DmaReadWord(source);
		WriteMemory(code, _state.Address, value);
		_state.Address += _state.Regs[15];
		//The source address wraps within a 128 KiB block
		source = (source & 0xFE0000) | ((source + 2) & 0x1FFFF);
	}

	_state.Regs[19] = 0;
	_state.Regs[20] = 0;
	_state.Regs[21] = (source >> 1) & 0xFF;
	_state.Regs[22] = (source >> 9) & 0xFF;
	_state.Code &= ~0x20;

	//The 68000 is held until the last word has been written
	uint64_t done = ConsumeSlots(std::max(now, _fifoLastCompletion), length * slotsPerWord);
	_state.DmaBusyUntil = done;
	_fifoLastCompletion = done;
	_memoryManager->AddM68kStall(done - now, true);
}

void MdVdp::RunDmaFill(uint16_t value)
{
	CatchUp(_memoryManager->GetMasterClock());
	uint32_t length = GetDmaLength();
	uint8_t code = _state.Code & 0x0F;
	uint8_t fillByte = value >> 8;
	for(uint32_t i = 0; i < length; i++) {
		switch(code) {
			//VRAM fill writes the fill data's high byte to the byte address XOR 1, starting from the
			//address after the data-port write's increment (C. MacDonald, "Sega Genesis VDP
			//documentation", DMA fill)
			case 0x01: _vram[_state.Address ^ 1] = fillByte; break;
			case 0x03: _cram[(_state.Address >> 1) & 0x3F] = value & 0x0EEE; break;
			case 0x05: {
				uint8_t index = (_state.Address >> 1) & 0x3F;
				if(index < 40) {
					_vsram[index] = value & 0x07FF;
				}
				break;
			}
		}
		_state.Address += _state.Regs[15];
	}
	_state.Regs[19] = 0;
	_state.Regs[20] = 0;
	_state.Code &= ~0x20;
	uint64_t now = _memoryManager->GetMasterClock();
	_state.DmaBusyUntil = ConsumeSlots(std::max(now, _fifoLastCompletion), length);
}

void MdVdp::RunDmaCopy()
{
	CatchUp(_memoryManager->GetMasterClock());
	uint32_t length = GetDmaLength();
	uint16_t source = _state.Regs[21] | (_state.Regs[22] << 8);
	for(uint32_t i = 0; i < length; i++) {
		_vram[_state.Address ^ 1] = _vram[source ^ 1];
		source++;
		_state.Address += _state.Regs[15];
	}
	_state.Regs[19] = 0;
	_state.Regs[20] = 0;
	_state.Regs[21] = source & 0xFF;
	_state.Regs[22] = source >> 8;
	_state.Code &= ~0x20;
	uint64_t now = _memoryManager->GetMasterClock();
	_state.DmaBusyUntil = ConsumeSlots(std::max(now, _fifoLastCompletion), length * 2);
}

void MdVdp::RenderSprites(uint16_t line, uint8_t* spritePixels, uint8_t* spritePriority, bool interlace2)
{
	bool h40 = IsH40();
	uint32_t width = h40 ? 320 : 256;
	uint32_t maxSprites = h40 ? 80 : 64;
	uint32_t maxPerLine = h40 ? 20 : 16;
	uint32_t maxDots = width;
	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();
	if(cfg.RemoveSpriteLimit) {
		maxPerLine = 80;
		maxDots = 80 * 32;
	}

	uint16_t satBase = (_state.Regs[5] & (h40 ? 0x7E : 0x7F)) << 9;
	uint8_t cellHeight = interlace2 ? 16 : 8;
	int32_t y = interlace2 ? (line * 2 + (_state.OddFrame ? 1 : 0)) : line;

	LineSprite sprites[80];
	uint32_t count = 0;
	uint8_t index = 0;
	for(uint32_t i = 0; i < maxSprites; i++) {
		uint16_t entry = satBase + index * 8;
		uint16_t w0 = (_vram[entry] << 8) | _vram[entry + 1];
		uint16_t w1 = (_vram[entry + 2] << 8) | _vram[entry + 3];
		uint16_t w2 = (_vram[(entry + 4) & 0xFFFF] << 8) | _vram[(entry + 5) & 0xFFFF];
		uint16_t w3 = (_vram[(entry + 6) & 0xFFFF] << 8) | _vram[(entry + 7) & 0xFFFF];
		int32_t sy = interlace2 ? ((w0 & 0x3FF) - 256) : ((w0 & 0x1FF) - 128);
		uint8_t hsize = ((w1 >> 10) & 0x03) + 1;
		uint8_t vsize = ((w1 >> 8) & 0x03) + 1;
		if(y >= sy && y < sy + vsize * cellHeight) {
			if(count >= maxPerLine) {
				_state.SpriteOverflow = true;
				break;
			}
			sprites[count++] = { (int16_t)((w3 & 0x1FF) - 128), hsize, vsize, w2, (uint16_t)(y - sy) };
		}
		index = w1 & 0x7F;
		if(index == 0 || index >= maxSprites) {
			break;
		}
	}

	uint32_t dots = 0;
	bool seenNonZeroX = false;
	bool masked = false;
	for(uint32_t i = 0; i < count && dots < maxDots; i++) {
		LineSprite& spr = sprites[i];
		if(spr.X == -128) {
			if(seenNonZeroX) {
				masked = true;
			}
		} else {
			seenNonZeroX = true;
		}

		uint16_t attr = spr.Attr;
		bool hflip = attr & 0x0800;
		bool vflip = attr & 0x1000;
		uint8_t palette = (attr >> 9) & 0x30;
		bool priority = attr & 0x8000;
		uint16_t tile = attr & 0x7FF;
		uint16_t row = spr.RowInSprite;
		if(vflip) {
			row = spr.Height * cellHeight - 1 - row;
		}
		uint16_t cellRow = row / cellHeight;
		uint16_t rowInCell = row % cellHeight;

		for(uint32_t px = 0; px < spr.Width * 8u; px++) {
			if(dots >= maxDots) {
				_state.SpriteOverflow = true;
				break;
			}
			dots++;
			if(masked) {
				continue;
			}
			int32_t x = spr.X + (int32_t)px;
			if(x < 0 || x >= (int32_t)width) {
				continue;
			}
			uint32_t sx = hflip ? (spr.Width * 8 - 1 - px) : px;
			uint16_t cellCol = sx / 8;
			uint16_t t = tile + cellCol * spr.Height + cellRow;
			uint32_t addr = interlace2 ? ((t & 0x3FF) * 64 + rowInCell * 4) : ((t & 0x7FF) * 32 + rowInCell * 4);
			uint8_t b = _vram[(addr + ((sx & 7) >> 1)) & 0xFFFF];
			uint8_t color = (sx & 1) ? (b & 0x0F) : (b >> 4);
			if(color == 0) {
				continue;
			}
			if(spritePixels[x]) {
				_state.SpriteCollision = true;
				continue;
			}
			spritePixels[x] = palette | color;
			spritePriority[x] = priority ? 1 : 0;
		}
	}
}

void MdVdp::BeginLine(uint16_t line)
{
	bool interlace2 = GetInterlaceMode() == 3;
	uint32_t row = interlace2 ? (line * 2 + (_state.OddFrame ? 1 : 0)) : line;
	if(row >= MaxHeight) {
		return;
	}
	_lineActive = true;
	_lineX = 0;
	//Rows use the frame's width (fixed at line 0) as their stride
	_lineWidth = (uint16_t)std::min<uint32_t>(IsH40() ? 320 : 256, _frameWidth);

	//Horizontal scroll is fetched during the preceding blanking period
	uint16_t hscrollBase = (_state.Regs[13] & 0x3F) << 10;
	uint16_t hsEntry = 0;
	switch(_state.Regs[11] & 0x03) {
		case 0: hsEntry = 0; break;
		case 1: hsEntry = (line & 0x07); break;
		case 2: hsEntry = (line & ~0x07); break;
		case 3: hsEntry = line; break;
	}
	uint16_t hsAddr = hscrollBase + hsEntry * 4;
	_lineHScrollA = ((_vram[hsAddr & 0xFFFF] << 8) | _vram[(hsAddr + 1) & 0xFFFF]) & 0x3FF;
	_lineHScrollB = ((_vram[(hsAddr + 2) & 0xFFFF] << 8) | _vram[(hsAddr + 3) & 0xFFFF]) & 0x3FF;

	//The sprite line is evaluated before the line starts
	memset(_lineSpritePixels, 0, sizeof(_lineSpritePixels));
	memset(_lineSpritePriority, 0, sizeof(_lineSpritePriority));
	if(IsDisplayEnabled() && !_emu->GetSettings()->GetMdConfig().DisableSprites) {
		RenderSprites(line, _lineSpritePixels, _lineSpritePriority, interlace2);
	}
}

void MdVdp::CatchUpLine(uint64_t clock)
{
	uint64_t activeStart = _lineStartClock + (IsH40() ? RenderOffsetH40 : RenderOffsetH32);
	if(clock <= activeStart) {
		return;
	}
	uint64_t x = (clock - activeStart) / (IsH40() ? 8 : 10);
	RenderPixels((uint16_t)std::min<uint64_t>(x, _lineWidth));
}

void MdVdp::FinishLine()
{
	if(_lineActive) {
		RenderPixels(_lineWidth);
		_lineActive = false;
		if(_m32x && GetInterlaceMode() != 3) {
			_m32x->CompositeLine(_currentBuffer + _state.Scanline * _frameWidth, _state.Scanline, _lineWidth);
		}
	}
}

bool MdVdp::IsHBlank(uint64_t clock)
{
	uint64_t pos = clock >= _lineStartClock ? (clock - _lineStartClock) % MdTiming::ClocksPerLine : (MdTiming::ClocksPerLine - (_lineStartClock - clock) % MdTiming::ClocksPerLine) % MdTiming::ClocksPerLine;
	uint32_t start = IsH40() ? RenderOffsetH40 : RenderOffsetH32;
	return pos < start || pos >= start + 2560;
}

void MdVdp::RenderPixels(uint16_t endX)
{
	if(endX <= _lineX) {
		return;
	}
	uint32_t startX = _lineX;
	_lineX = endX;

	uint16_t line = _state.Scanline;
	bool interlace2 = GetInterlaceMode() == 3;
	uint32_t row = interlace2 ? (line * 2 + (_state.OddFrame ? 1 : 0)) : line;
	bool h40 = IsH40();
	uint16_t* out = _currentBuffer + row * _frameWidth;

	auto toPixel = [this](uint8_t colorIndex, uint8_t shade) -> uint16_t {
		uint16_t c = _cram[colorIndex & 0x3F];
		uint16_t rgb = ((c >> 1) & 0x07) | (((c >> 5) & 0x07) << 3) | (((c >> 9) & 0x07) << 6);
		return rgb | (shade << 9);
	};

	uint8_t bgIndex = _state.Regs[7] & 0x3F;
	if(!IsDisplayEnabled()) {
		uint16_t bg = toPixel(bgIndex, ShadeNormal) | _backdropFlag;
		for(uint32_t x = startX; x < endX; x++) {
			out[x] = bg;
		}
		return;
	}

	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();
	uint8_t cellHeight = interlace2 ? 16 : 8;
	int32_t y = interlace2 ? (line * 2 + (_state.OddFrame ? 1 : 0)) : line;

	static const uint8_t planeSizes[4] = { 32, 64, 64, 128 };
	uint32_t planeW = planeSizes[_state.Regs[16] & 0x03];
	uint32_t planeH = planeSizes[(_state.Regs[16] >> 4) & 0x03];
	if(planeW == 128 && planeH > 32) {
		planeH = 32;
	} else if(planeW == 64 && planeH > 64) {
		planeH = 64;
	}

	uint16_t hscrollA = _lineHScrollA;
	uint16_t hscrollB = _lineHScrollB;

	uint16_t planeABase = (_state.Regs[2] & 0x38) << 10;
	uint16_t planeBBase = (_state.Regs[4] & 0x07) << 13;
	uint16_t windowBase = (_state.Regs[3] & (h40 ? 0x3C : 0x3E)) << 10;
	bool perColumnVScroll = _state.Regs[11] & 0x04;

	//Window region on this line
	uint8_t winH = _state.Regs[17] & 0x1F;
	bool winRight = _state.Regs[17] & 0x80;
	uint8_t winV = _state.Regs[18] & 0x1F;
	bool winDown = _state.Regs[18] & 0x80;
	bool lineInWindow = winDown ? (line >= winV * 8) : (line < winV * 8);

	uint8_t* spritePixels = _lineSpritePixels;
	uint8_t* spritePriority = _lineSpritePriority;

	bool shadowHighlight = _state.Regs[12] & 0x08;
	bool blankLeftColumn = _state.Regs[0] & 0x20;

	auto fetchPixel = [&](uint16_t base, uint32_t w, uint32_t h, uint32_t px, uint32_t py, uint8_t& colorOut, bool& priOut) {
		uint32_t cellX = (px / 8) & (w - 1);
		uint32_t cellY = (py / cellHeight) & (h - 1);
		uint16_t ntAddr = base + (cellY * w + cellX) * 2;
		uint16_t entry = (_vram[ntAddr & 0xFFFF] << 8) | _vram[(ntAddr + 1) & 0xFFFF];
		uint32_t tx = px & 7;
		uint32_t ty = py % cellHeight;
		if(entry & 0x0800) {
			tx = 7 - tx;
		}
		if(entry & 0x1000) {
			ty = cellHeight - 1 - ty;
		}
		uint32_t tile = entry & 0x7FF;
		uint32_t addr = interlace2 ? ((tile & 0x3FF) * 64 + ty * 4) : (tile * 32 + ty * 4);
		uint8_t b = _vram[(addr + (tx >> 1)) & 0xFFFF];
		uint8_t color = (tx & 1) ? (b & 0x0F) : (b >> 4);
		colorOut = color ? (((entry >> 9) & 0x30) | color) : 0;
		priOut = entry & 0x8000;
	};

	for(uint32_t x = startX; x < endX; x++) {
		//Vertical scroll: full screen, or per 16-pixel column
		uint32_t column = (x >> 4) & 0x1F;
		uint16_t vscrollA = perColumnVScroll ? _vsram[(column * 2) % 40] : _vsram[0];
		uint16_t vscrollB = perColumnVScroll ? _vsram[(column * 2 + 1) % 40] : _vsram[1];
		if(!interlace2) {
			vscrollA &= 0x3FF;
			vscrollB &= 0x3FF;
		}

		uint8_t colorB = 0;
		bool priB = false;
		if(!cfg.HidePlaneB) {
			fetchPixel(planeBBase, planeW, planeH, (x - hscrollB) & (planeW * 8 - 1), (y + vscrollB) & (planeH * cellHeight - 1), colorB, priB);
		}

		uint8_t colorA = 0;
		bool priA = false;
		bool inWindow = lineInWindow || (winRight ? (x >= winH * 16u) : (x < winH * 16u));
		if(inWindow) {
			if(!cfg.HideWindow) {
				uint32_t ww = h40 ? 64 : 32;
				fetchPixel(windowBase, ww, 32, x, y, colorA, priA);
			}
		} else if(!cfg.HidePlaneA) {
			fetchPixel(planeABase, planeW, planeH, (x - hscrollA) & (planeW * 8 - 1), (y + vscrollA) & (planeH * cellHeight - 1), colorA, priA);
		}

		uint8_t colorS = spritePixels[x];
		bool priS = spritePriority[x];

		if(blankLeftColumn && x < 8) {
			out[x] = toPixel(bgIndex, ShadeNormal) | _backdropFlag;
			continue;
		}

		uint8_t shade = ShadeNormal;
		if(shadowHighlight && !priA && !priB) {
			shade = ShadeShadow;
		}

		bool spriteIsOperator = shadowHighlight && (colorS == 0x3E || colorS == 0x3F);
		uint8_t result = bgIndex;
		bool resultIsSprite = false;
		//Priority order: high sprite, high A, high B, low sprite, low A, low B, background
		if(colorS && priS && !spriteIsOperator) {
			result = colorS;
			resultIsSprite = true;
		} else if(colorA && priA) {
			result = colorA;
		} else if(colorB && priB) {
			result = colorB;
		} else if(colorS && !priS && !spriteIsOperator) {
			result = colorS;
			resultIsSprite = true;
		} else if(colorA) {
			result = colorA;
		} else if(colorB) {
			result = colorB;
		}

		if(shadowHighlight) {
			if(spriteIsOperator) {
				bool operatorVisible = priS || !((colorA && priA) || (colorB && priB));
				if(operatorVisible) {
					if(colorS == 0x3E) {
						shade = shade == ShadeShadow ? ShadeNormal : ShadeHighlight;
					} else {
						shade = ShadeShadow;
					}
				}
			} else if(resultIsSprite && priS) {
				shade = ShadeNormal;
			} else if(resultIsSprite && ((result & 0x0F) == 0x0E)) {
				//Sprite colours 14 of palettes 0-2 are never shadowed
				shade = ShadeNormal;
			}
		}

		//Backdrop: no sprite (other than a shadow/highlight operator), plane A or plane B pixel
		bool backdrop = !(colorS && !spriteIsOperator) && !colorA && !colorB;
		out[x] = toPixel(result, shade) | (backdrop ? _backdropFlag : 0);
	}
}

void MdVdp::Serialize(Serializer& s)
{
	SVArray(_vram, 0x10000);
	SVArray(_cram, 64);
	SVArray(_vsram, 40);
	SVArray(_state.Regs, 24);
	SV(_state.FrameCount);
	SV(_state.Scanline);
	SV(_state.Address);
	SV(_state.Code);
	SV(_state.CommandPending);
	SV(_state.FirstCommandWord);
	SV(_state.HIntCounter);
	SV(_state.HIntPending);
	SV(_state.VIntPending);
	SV(_state.Z80IntActive);
	SV(_state.OddFrame);
	SV(_state.SpriteOverflow);
	SV(_state.SpriteCollision);
	SV(_state.HvLatched);
	SV(_state.HvLatch);
	SV(_state.DmaFillPending);
	SV(_state.DmaFillValue);
	SV(_state.DmaBusyUntil);
	SV(_state.ReadBuffer);
	SV(_lineStartClock);
	SV(_z80IntEndClock);
	SV(_lineRendered);
	SV(_lineActive);
	SV(_lineX);
	SV(_lineWidth);
	SV(_lineHScrollA);
	SV(_lineHScrollB);
	SVArray(_lineSpritePixels, 320);
	SVArray(_lineSpritePriority, 320);
	SV(_vintRaisedThisFrame);
	SV(_extIntPending);
	SV(_lightgunClock);
	SV(_lightgunPort);
	SVArray(_fifo, 4);
	SV(_fifoCount);
	SV(_fifoLastCompletion);
	SV(_frameWidth);
	SV(_frameHeight);
	SV(_lastFrameWidth);
	SV(_lastFrameHeight);
	SV(_isPal);
	SV(_linesPerFrame);
}
