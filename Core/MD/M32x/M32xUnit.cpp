#include "pch.h"
#include "MD/M32x/M32xUnit.h"
#include "MD/M32x/M32xBootRom.h"
#include "MD/MdConsole.h"
#include "MD/MdMemoryManager.h"
#include "MD/MdVdp.h"
#include "Shared/Emulator.h"
#include "Shared/MemoryOperationType.h"
#include "Utilities/Serializer.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/FolderUtilities.h"
#include "Shared/FirmwareHelper.h"

// ---------------------------------------------------------------------------------------------
// SH-2 bus: 16-bit external bus, so long accesses are two word accesses (32X Hardware Manual,
// SH2 memory map; as ares)
// ---------------------------------------------------------------------------------------------

uint8_t M32xSh2::BusRead8(uint32_t addr)
{
	uint16_t w = _unit->ShRead16(_index, addr & ~1);
	return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

uint16_t M32xSh2::BusRead16(uint32_t addr)
{
	return _unit->ShRead16(_index, addr & ~1);
}

uint32_t M32xSh2::BusRead32(uint32_t addr)
{
	uint32_t hi = _unit->ShRead16(_index, addr & ~3);
	return hi << 16 | _unit->ShRead16(_index, (addr & ~3) | 2);
}

void M32xSh2::BusWrite8(uint32_t addr, uint8_t value)
{
	bool odd = addr & 1;
	_unit->ShWrite16(_index, addr & ~1, !odd, odd, (uint16_t)(value << 8 | value));
}

void M32xSh2::BusWrite16(uint32_t addr, uint16_t value)
{
	_unit->ShWrite16(_index, addr & ~1, true, true, value);
}

void M32xSh2::BusWrite32(uint32_t addr, uint32_t value)
{
	_unit->ShWrite16(_index, addr & ~3, true, true, (uint16_t)(value >> 16));
	_unit->ShWrite16(_index, (addr & ~3) | 2, true, true, (uint16_t)value);
}

uint8_t M32xSh2::Peek8(uint32_t addr)
{
	return _unit->ShPeek8(addr);
}

void M32xSh2::OnInstructionStart(uint32_t, uint16_t)
{
	Emulator* emu = _unit->GetEmulator();
	if(_index == 0) {
		emu->ProcessInstruction<CpuType::M32xMaster>();
	} else {
		emu->ProcessInstruction<CpuType::M32xSlave>();
	}
}

template<CpuType type>
static void ReportRead(Emulator* emu, uint32_t addr, uint32_t value, int size)
{
	if(size == 1) {
		uint8_t v = (uint8_t)value;
		emu->ProcessMemoryRead<type, 1>(addr, v, MemoryOperationType::Read);
	} else if(size == 2) {
		uint16_t v = (uint16_t)value;
		emu->ProcessMemoryRead<type, 2>(addr, v, MemoryOperationType::Read);
	} else {
		emu->ProcessMemoryRead<type, 4>(addr, value, MemoryOperationType::Read);
	}
}

template<CpuType type>
static void ReportWrite(Emulator* emu, uint32_t addr, uint32_t value, int size)
{
	if(size == 1) {
		uint8_t v = (uint8_t)value;
		emu->ProcessMemoryWrite<type, 1>(addr, v, MemoryOperationType::Write);
	} else if(size == 2) {
		uint16_t v = (uint16_t)value;
		emu->ProcessMemoryWrite<type, 2>(addr, v, MemoryOperationType::Write);
	} else {
		emu->ProcessMemoryWrite<type, 4>(addr, value, MemoryOperationType::Write);
	}
}

//Debugger addresses are physical (29 bits): cache-through addresses show as their cached ones
void M32xSh2::OnDebugRead(uint32_t addr, uint32_t value, int size)
{
	if(_index == 0) {
		ReportRead<CpuType::M32xMaster>(_unit->GetEmulator(), addr & 0x1FFFFFFF, value, size);
	} else {
		ReportRead<CpuType::M32xSlave>(_unit->GetEmulator(), addr & 0x1FFFFFFF, value, size);
	}
}

void M32xSh2::OnDebugWrite(uint32_t addr, uint32_t value, int size)
{
	if(_index == 0) {
		ReportWrite<CpuType::M32xMaster>(_unit->GetEmulator(), addr & 0x1FFFFFFF, value, size);
	} else {
		ReportWrite<CpuType::M32xSlave>(_unit->GetEmulator(), addr & 0x1FFFFFFF, value, size);
	}
}

void M32xSh2::OnInterruptTaken(uint32_t returnPc, uint32_t handlerPc)
{
	Emulator* emu = _unit->GetEmulator();
	if(_index == 0) {
		emu->ProcessInterrupt<CpuType::M32xMaster>(returnPc & 0x1FFFFFFF, handlerPc & 0x1FFFFFFF, false);
	} else {
		emu->ProcessInterrupt<CpuType::M32xSlave>(returnPc & 0x1FFFFFFF, handlerPc & 0x1FFFFFFF, false);
	}
}

M32xSh2State M32xSh2::GetDebugState()
{
	M32xSh2State s = {};
	s.CycleCount = _state.CycleCount;
	memcpy(s.R, _state.R, sizeof(s.R));
	s.PC = _state.PC;
	s.PR = _state.PR;
	s.GBR = _state.GBR;
	s.VBR = _state.VBR;
	s.MACH = _state.MACH;
	s.MACL = _state.MACL;
	s.SR = _state.SR;
	s.Sleeping = _state.Sleeping;
	return s;
}

void M32xSh2::SetDebugState(M32xSh2State& s)
{
	memcpy(_state.R, s.R, sizeof(s.R));
	if(s.PC != _state.PC) {
		SetPc(s.PC);
	}
	_state.PR = s.PR;
	_state.GBR = s.GBR;
	_state.VBR = s.VBR;
	_state.MACH = s.MACH;
	_state.MACL = s.MACL;
	_state.SR = s.SR & SrMask;
	_state.Sleeping = s.Sleeping;
}

// ---------------------------------------------------------------------------------------------

bool M32xUnit::Is32xRom(vector<uint8_t>& rom)
{
	if(rom.size() < 0x400) {
		return false;
	}
	string sys((char*)&rom[0x100], 16);
	return sys.find("32X") != string::npos || memcmp(&rom[0x3C0], "MARS", 4) == 0;
}

void M32xUnit::LoadBios(Emulator* emu, vector<uint8_t>& master, vector<uint8_t>& slave, vector<uint8_t>& vectors)
{
	(void)emu;
	if(_biosFolderOverride.empty()) {
		FirmwareHelper::Load32xBios(master, slave, vectors);
		return;
	}
	auto load = [](string path, vector<uint8_t>& out) {
		VirtualFile file(path);
		if(file.IsValid()) {
			file.ReadFile(out);
		}
	};
	load(FolderUtilities::CombinePath(_biosFolderOverride, "32X_M_BIOS.bin"), master);
	load(FolderUtilities::CombinePath(_biosFolderOverride, "32X_S_BIOS.bin"), slave);
	load(FolderUtilities::CombinePath(_biosFolderOverride, "32X_G_BIOS.bin"), vectors);
}

void M32xUnit::Init(MdConsole* console, MdMemoryManager* memoryManager, bool isPal, vector<uint8_t> masterBios, vector<uint8_t> slaveBios, vector<uint8_t> vectorBios)
{
	_console = console;
	_memoryManager = memoryManager;
	_vdp = console->GetVdp();
	_isPal = isPal;

	if(masterBios.size() >= 0x800 && slaveBios.size() >= 0x400 && vectorBios.size() >= 0x100) {
		_bootRom[0] = std::move(masterBios);
		_bootRom[1] = std::move(slaveBios);
		_vectorRom = std::move(vectorBios);
		_replacementBios = false;
	} else {
		_bootRom[0].assign(M32xBootRom::Master, M32xBootRom::Master + sizeof(M32xBootRom::Master));
		_bootRom[1].assign(M32xBootRom::Slave, M32xBootRom::Slave + sizeof(M32xBootRom::Slave));
		_vectorRom.assign(M32xBootRom::Vectors, M32xBootRom::Vectors + sizeof(M32xBootRom::Vectors));
		_replacementBios = true;
	}
	_vectorRom.resize(0x100, 0);
	_vectorLevel4 = (uint32_t)(_vectorRom[0x70] << 24 | _vectorRom[0x71] << 16 | _vectorRom[0x72] << 8 | _vectorRom[0x73]);

	for(int i = 0; i < 2; i++) {
		_sh2[i].Init(this, (uint8_t)i);
		_sh2[i].PowerOn();
	}
	Emulator* emu = console->GetEmulator();
	emu->RegisterMemory(MemoryType::M32xSdram, _sdram, SdramSize);
	emu->RegisterMemory(MemoryType::M32xFrameBuffer, &_frameBuffer[0][0], FrameBufferSize * 2);
	emu->RegisterMemory(MemoryType::M32xPalette, _palette, PaletteSize);
	emu->RegisterMemory(MemoryType::M32xBootRom, _bootRom[0].data(), (uint32_t)_bootRom[0].size());
	_sh2[0].LinkSci(&_sh2[1]);
	_sh2[1].LinkSci(&_sh2[0]);
}

// ---------------------------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------------------------

uint64_t M32xUnit::Sh2Target(uint64_t masterClock)
{
	//SH-2 clock = 68000 clock / 3 = master clock / 7 x 3 (32X Hardware Manual p. 52)
	return masterClock * 3 / 7;
}

void M32xUnit::Run(uint64_t masterClock)
{
	//The VDP first reaches `masterClock`: each line start it passes runs the SH-2s up to that line
	//(calling back here) before raising the line's H/V interrupts, so a VDP that catches up several
	//lines at once (after a long 68000 stall) still delivers every interrupt in time
	if(!_syncingVdp) {
		_syncingVdp = true;
		_vdp->Run(masterClock);
		_syncingVdp = false;
	}
	uint64_t target = Sh2Target(masterClock);
	//Debugger hooks only while a debugger is attached (Sh2Core::SetDebugHooks)
	bool debugging = _console->GetEmulator()->IsDebugging();
	_sh2[0].SetDebugHooks(debugging);
	_sh2[1].SetDebugHooks(debugging);
	if(!_sh2Run) {
		//RES = 0: both SH-2s are held in reset; time still passes
		for(int i = 0; i < 2; i++) {
			uint64_t c = _sh2[i].GetCycleCount();
			if(c < target) {
				_sh2[i].AddCycles((uint32_t)(target - c));
			}
		}
		RunPwm(target);
		return;
	}

	while(true) {
		//The CPU that is behind runs next
		int cpu = _sh2[1].GetCycleCount() < _sh2[0].GetCycleCount() ? 1 : 0;
		uint64_t now = _sh2[cpu].GetCycleCount();
		if(now >= target) {
			break;
		}
		RunPwm(now);
		_sh2[cpu].Step();
		if(_dreq1Pulse) {
			_dreq1Pulse = false;
			_sh2[0].SetDreq(1, false);
			_sh2[1].SetDreq(1, false);
		}
	}
	RunPwm(target);
}

// ---------------------------------------------------------------------------------------------
// Interrupts (32X Hardware Manual p. 69 and "Restrictions concerning SH2 interrupt"): VRES 14,
// V 12, H 10, CMD 8, PWM 6, on the SH-2's IRL inputs with auto-vectors 64 + level / 2.
// V, H and PWM requests latch only while enabled in the CPU's mask register and then stay
// asserted until the CPU clears them, even if masked; CMD is set by the 68000 and is gated by
// the mask (masking it negates the request, unmasking it asserts it again until cleared).
// ---------------------------------------------------------------------------------------------

void M32xUnit::UpdateIrl(int cpu)
{
	uint8_t active = _irqActive[cpu];
	uint8_t level = 0;
	if(active & IrqVres) {
		level = 14;
	} else if(active & IrqV) {
		level = 12;
	} else if(active & IrqH) {
		level = 10;
	} else if((active & IrqCmd) && (_irqMask[cpu] & IrqCmd)) {
		level = 8;
	} else if(active & IrqPwm) {
		level = 6;
	}
	_sh2[cpu].SetIrl(level, (uint8_t)(64 + level / 2));
}

void M32xUnit::UpdateIrls()
{
	UpdateIrl(0);
	UpdateIrl(1);
}

void M32xUnit::RaiseIrq(uint8_t source)
{
	for(int i = 0; i < 2; i++) {
		if(source == IrqVres || (_irqMask[i] & source)) {
			_irqActive[i] |= source;
		}
	}
	UpdateIrls();
}

void M32xUnit::OnResetButton()
{
	RaiseIrq(IrqVres);
}

// ---------------------------------------------------------------------------------------------
// VDP signals (from MdVdp)
// ---------------------------------------------------------------------------------------------

void M32xUnit::OnLineStart()
{
	//Display settings take effect at the next line (ares latches them at the end of HBlank)
	_lineMode = _vdpMode;
	_linePriority = _vdpPriority;
	_lineDotShift = _vdpDotShift;
	_lineLines240 = _vdpLines240;
}

void M32xUnit::OnHBlank(bool active)
{
	_hblank = active;
	if(active && (!_vblank || _hintInVblank)) {
		//H interrupt every HCount + 1 lines (ares)
		if(_hCounter++ >= _hCount) {
			_hCounter = 0;
			RaiseIrq(IrqH);
		}
	}
}

void M32xUnit::OnVBlank(bool active)
{
	_vblank = active;
	if(active) {
		//The frame buffer select written during the display takes effect here
		_fbDisplay = _fbSelect;
		RaiseIrq(IrqV);
	}
}

// ---------------------------------------------------------------------------------------------
// DREQ FIFO (68000 -> SH-2): as ares (a single 8-word queue; the hardware's two 4-word blocks are
// not modelled)
// ---------------------------------------------------------------------------------------------

void M32xUnit::UpdateDreq0()
{
	bool request = _fifoCount > 0;
	_sh2[0].SetDreq(0, request);
	_sh2[1].SetDreq(0, request);
}

uint16_t M32xUnit::FifoPop()
{
	if(_fifoCount == 0) {
		return _fifo[(_fifoHead + FifoSize - 1) % FifoSize];
	}
	uint16_t v = _fifo[_fifoHead];
	_fifoHead = (_fifoHead + 1) % FifoSize;
	_fifoCount--;
	UpdateDreq0();
	return v;
}

// ---------------------------------------------------------------------------------------------
// PWM (32X Hardware Manual, PWM; behaviour as ares): a sample every (cycle - 1) SH-2 clocks;
// three-word FIFOs per channel; the timer requests an interrupt (and DMA on DREQ1 when RTP is
// set) every TM samples (TM = 0 means 16)
// ---------------------------------------------------------------------------------------------

void M32xUnit::PwmFifoPush(int ch, uint16_t value)
{
	if(_pwmFifoCount[ch] == 3) {
		//Full: the oldest sample is dropped (ares)
		_pwmFifo[ch][0] = _pwmFifo[ch][1];
		_pwmFifo[ch][1] = _pwmFifo[ch][2];
		_pwmFifoCount[ch] = 2;
	}
	_pwmFifo[ch][_pwmFifoCount[ch]++] = value & 0xFFF;
}

void M32xUnit::RunPwm(uint64_t sh2Cycle)
{
	while(_pwmNextTick <= sh2Cycle) {
		uint32_t period = (_pwmCycle - 1) & 0xFFF;
		if(period == 0) {
			period = 1;
		}
		bool active = _pwmCycle != 1 && (((_pwmLMode ^ (_pwmLMode >> 1)) & 1) || ((_pwmRMode ^ (_pwmRMode >> 1)) & 1));
		if(active) {
			for(int ch = 0; ch < 2; ch++) {
				if(_pwmFifoCount[ch]) {
					_pwmOut[ch] = _pwmFifo[ch][0];
					_pwmFifo[ch][0] = _pwmFifo[ch][1];
					_pwmFifo[ch][1] = _pwmFifo[ch][2];
					_pwmFifoCount[ch]--;
				}
			}
			if(_pwmPeriods++ == ((_pwmTimer - 1) & 0x0F)) {
				_pwmPeriods = 0;
				RaiseIrq(IrqPwm);
				if(_pwmDreqEnable) {
					//A one-instruction pulse on DREQ1 (cleared by the scheduler; ares)
					_sh2[0].SetDreq(1, true);
					_sh2[1].SetDreq(1, true);
					_dreq1Pulse = true;
				}
			}
		}
		_pwmNextTick += period;
	}
}

void M32xUnit::GetAudioOutput(int32_t& left, int32_t& right)
{
	//Pulse width relative to the cycle, centred on half the cycle (judgment call: the analogue
	//output stage and its DC level are not modelled)
	uint32_t cycle = (_pwmCycle - 1) & 0xFFF;
	if(cycle < 2) {
		left = right = 0;
		return;
	}
	auto level = [&](uint8_t mode) -> int32_t {
		uint16_t sample = mode == 1 ? _pwmOut[0] : (mode == 2 ? _pwmOut[1] : (uint16_t)(cycle / 2));
		int32_t v = ((int32_t)sample * 2 - (int32_t)cycle) * 16383 / (int32_t)cycle;
		return std::clamp(v, -16384, 16383);
	};
	left = level(_pwmLMode);
	//R mode: 1 = R channel, 2 = L channel
	right = level(_pwmRMode == 1 ? 2 : (_pwmRMode == 2 ? 1 : 0));
}

void M32xUnit::WritePwmReg(uint32_t reg, bool upper, bool lower, uint16_t value, bool fromSh2)
{
	switch(reg & 0x0E) {
		case 0x00: //control
			if(lower) {
				_pwmLMode = value & 3;
				_pwmRMode = (value >> 2) & 3;
				_pwmMono = value & 0x10;
				if(fromSh2) {
					_pwmDreqEnable = value & 0x80;
					if(!_pwmDreqEnable) {
						_sh2[0].SetDreq(1, false);
						_sh2[1].SetDreq(1, false);
					}
				}
			}
			if(upper && fromSh2) {
				_pwmTimer = (value >> 8) & 0x0F;
				_pwmPeriods = 0;
			}
			break;
		case 0x02: //cycle
			if(lower) {
				_pwmCycle = (_pwmCycle & 0xF00) | (value & 0xFF);
			}
			if(upper) {
				_pwmCycle = (_pwmCycle & 0x0FF) | (value & 0xF00);
			}
			break;
		case 0x04: //L
		case 0x06: //R
		case 0x08: { //mono (both)
			int idx = ((reg & 0x0E) - 4) >> 1;
			if(upper) {
				_pwmLatch[idx] = (_pwmLatch[idx] & 0x0FF) | (value & 0xF00);
			}
			if(lower) {
				_pwmLatch[idx] = (_pwmLatch[idx] & 0xF00) | (value & 0xFF);
				if(idx == 2) {
					PwmFifoPush(0, _pwmLatch[2]);
					PwmFifoPush(1, _pwmLatch[2]);
				} else {
					PwmFifoPush(idx, _pwmLatch[idx]);
				}
			}
			break;
		}
	}
}

uint16_t M32xUnit::ReadPwmReg(uint32_t reg)
{
	switch(reg & 0x0E) {
		case 0x00: return (uint16_t)(_pwmTimer << 8 | (_pwmDreqEnable ? 0x80 : 0) | (_pwmMono ? 0x10 : 0) | _pwmRMode << 2 | _pwmLMode);
		case 0x02: return _pwmCycle;
		case 0x04:
		case 0x06: {
			int ch = ((reg & 0x0E) - 4) >> 1;
			return (uint16_t)((_pwmFifoCount[ch] == 3 ? 0x8000 : 0) | (_pwmFifoCount[ch] == 0 ? 0x4000 : 0));
		}
		case 0x08:
			return (uint16_t)((_pwmFifoCount[0] == 3 || _pwmFifoCount[1] == 3 ? 0x8000 : 0) | (_pwmFifoCount[0] == 0 && _pwmFifoCount[1] == 0 ? 0x4000 : 0));
	}
	return 0;
}

// ---------------------------------------------------------------------------------------------
// VDP registers and frame buffer
// ---------------------------------------------------------------------------------------------

bool M32xUnit::InHBlank()
{
	//HBLK from the VDP's line position at the SH-2s' current time
	uint64_t now = std::max(_sh2[0].GetCycleCount(), _sh2[1].GetCycleCount());
	return _vdp->IsHBlank(now * 7 / 3);
}

bool M32xUnit::FrameBufferBusy(uint64_t sh2Cycle)
{
	//FEN: a fill is running, or a buffer swap is pending until VBlank (as ares)
	return sh2Cycle < _fillBusyUntil || (_vdpMode != 0 && _fbSelect != _fbDisplay);
}

bool M32xUnit::PaletteBusy()
{
	//PEN: the palette is in use while the packed or run-length modes display a line (as ares)
	return !_vblank && !InHBlank() && (_lineMode & 1);
}

void M32xUnit::SelectFrameBuffer(bool select)
{
	_fbSelect = select;
	if(_vblank || _vdpMode == 0) {
		_fbDisplay = select;
	}
}

void M32xUnit::Fill()
{
	//Fills length + 1 words of the back buffer; the address's low byte wraps within its 256-word
	//row (ares). Busy for 7 + 3 x (length + 1) SH-2 cycles (ares, "according to official docs")
	uint64_t now = std::max(_sh2[0].GetCycleCount(), _sh2[1].GetCycleCount());
	uint8_t* fb = _frameBuffer[!_fbDisplay];
	uint16_t addr = _fillAddress;
	for(uint32_t i = 0; i <= _fillLength; i++) {
		uint32_t a = (uint32_t)addr << 1;
		fb[a] = (uint8_t)(_fillData >> 8);
		fb[a | 1] = (uint8_t)_fillData;
		addr = (uint16_t)((addr & 0xFF00) | ((addr + 1) & 0xFF));
	}
	_fillAddress = addr;
	_fillBusyUntil = now + 7 + 3 * ((uint64_t)_fillLength + 1);
}

uint16_t M32xUnit::ReadVdpReg(uint32_t reg)
{
	switch(reg & 0x0E) {
		case 0x00: return (uint16_t)((_isPal ? 0 : 0x8000) | (_vdpPriority ? 0x80 : 0) | (_vdpLines240 ? 0x40 : 0) | _vdpMode);
		case 0x02: return _vdpDotShift ? 1 : 0;
		case 0x04: return _fillLength;
		case 0x06: return _fillAddress;
		case 0x08: return _fillData;
		case 0x0A: {
			uint64_t now = std::max(_sh2[0].GetCycleCount(), _sh2[1].GetCycleCount());
			//FEN also reads 1 while the frame buffer DRAM is refreshed (Metal Head waits for it)
			bool fen = FrameBufferBusy(now) || _vdp->Is32xRefreshWindow(now * 7 / 3);
			return (uint16_t)((_vblank ? 0x8000 : 0) | (InHBlank() ? 0x4000 : 0) | (PaletteBusy() ? 0 : 0x2000) | (fen ? 2 : 0) | (_fbDisplay ? 1 : 0));
		}
	}
	return 0;
}

void M32xUnit::WriteVdpReg(uint32_t reg, bool upper, bool lower, uint16_t value)
{
	switch(reg & 0x0E) {
		case 0x00:
			if(lower) {
				_vdpMode = value & 3;
				_vdpLines240 = value & 0x40;
				_vdpPriority = value & 0x80;
			}
			break;
		case 0x02:
			if(lower) {
				_vdpDotShift = value & 1;
			}
			break;
		case 0x04:
			if(lower) {
				_fillLength = (uint8_t)value;
			}
			break;
		case 0x06:
			if(upper) {
				_fillAddress = (uint16_t)((_fillAddress & 0x00FF) | (value & 0xFF00));
			}
			if(lower) {
				_fillAddress = (uint16_t)((_fillAddress & 0xFF00) | (value & 0x00FF));
			}
			break;
		case 0x08:
			if(upper) {
				_fillData = (uint16_t)((_fillData & 0x00FF) | (value & 0xFF00));
			}
			if(lower) {
				_fillData = (uint16_t)((_fillData & 0xFF00) | (value & 0x00FF));
			}
			Fill();
			break;
		case 0x0A:
			if(lower) {
				SelectFrameBuffer(value & 1);
			}
			break;
	}
}

void M32xUnit::FrameBufferWrite(uint32_t offset, bool upper, bool lower, uint16_t value, bool overwrite)
{
	uint8_t* fb = _frameBuffer[!_fbDisplay];
	uint32_t a = offset & (FrameBufferSize - 2);
	if(overwrite) {
		//Overwrite image: zero bytes leave the pixel unchanged (32X Hardware Manual)
		if(upper && (value >> 8)) {
			fb[a] = (uint8_t)(value >> 8);
		}
		if(lower && (value & 0xFF)) {
			fb[a | 1] = (uint8_t)value;
		}
		return;
	}
	if(value == 0 && (!upper || !lower)) {
		//8-bit writes of $00 don't go through (ares)
		return;
	}
	if(upper) {
		fb[a] = (uint8_t)(value >> 8);
	}
	if(lower) {
		fb[a | 1] = (uint8_t)value;
	}
}

// ---------------------------------------------------------------------------------------------
// System registers: 68000 $A15100-$A1512F, SH-2 $4000-$402F
// ---------------------------------------------------------------------------------------------

uint16_t M32xUnit::ReadSysReg(int cpu, uint32_t addr)
{
	switch(addr & 0x3E) {
		case 0x00:
			return (uint16_t)((_fm ? 0x8000 : 0) | (_adapterEnable ? 0x200 : 0) | (_hintInVblank ? 0x80 : 0) | (_irqMask[cpu] & 0x0F));
		case 0x04: return _hCount;
		case 0x06: return (uint16_t)((_fifoCount == FifoSize ? 0x8000 : 0) | (_fifoCount == 0 ? 0x4000 : 0) | (_dreq68s ? 4 : 0) | (_dreqDma ? 2 : 0) | (_dreqRv ? 1 : 0));
		case 0x08: return (uint16_t)(_dreqSource >> 16);
		case 0x0A: return (uint16_t)_dreqSource;
		case 0x0C: return (uint16_t)(_dreqDest >> 16);
		case 0x0E: return (uint16_t)_dreqDest;
		case 0x10: return _dreqLength;
		case 0x12: return FifoPop();
		case 0x20:
		case 0x22:
		case 0x24:
		case 0x26:
		case 0x28:
		case 0x2A:
		case 0x2C:
		case 0x2E:
			return _comm[(addr >> 1) & 7];
		case 0x30:
		case 0x32:
		case 0x34:
		case 0x36:
		case 0x38:
			return ReadPwmReg(addr & 0x0E);
	}
	return 0;
}

void M32xUnit::WriteSysReg(int cpu, uint32_t addr, bool upper, bool lower, uint16_t value)
{
	switch(addr & 0x3E) {
		case 0x00:
			if(lower) {
				_irqMask[cpu] = value & 0x0F;
				_hintInVblank = value & 0x80;
				UpdateIrl(cpu);
			}
			if(upper) {
				_fm = value & 0x8000;
			}
			break;
		case 0x04:
			if(lower) {
				_hCount = (uint8_t)value;
				_hCounter = 0;
			}
			break;
		//Interrupt clear registers: each CPU clears its own request
		case 0x14:
			_irqActive[cpu] &= ~IrqVres;
			UpdateIrl(cpu);
			break;
		case 0x16:
			_irqActive[cpu] &= ~IrqV;
			UpdateIrl(cpu);
			break;
		case 0x18:
			_irqActive[cpu] &= ~IrqH;
			UpdateIrl(cpu);
			break;
		case 0x1A:
			_irqActive[cpu] &= ~IrqCmd;
			UpdateIrl(cpu);
			break;
		case 0x1C:
			_irqActive[cpu] &= ~IrqPwm;
			UpdateIrl(cpu);
			break;
		case 0x20:
		case 0x22:
		case 0x24:
		case 0x26:
		case 0x28:
		case 0x2A:
		case 0x2C:
		case 0x2E: {
			uint16_t& c = _comm[(addr >> 1) & 7];
			if(upper) {
				c = (uint16_t)((c & 0x00FF) | (value & 0xFF00));
			}
			if(lower) {
				c = (uint16_t)((c & 0xFF00) | (value & 0x00FF));
			}
			break;
		}
		case 0x30:
		case 0x32:
		case 0x34:
		case 0x36:
		case 0x38:
			WritePwmReg(addr & 0x0E, upper, lower, value, true);
			break;
	}
}

uint16_t M32xUnit::MainReadRegister(uint32_t addr, uint16_t openBus)
{
	if(addr >= 0xA130EC && addr < 0xA130F0) {
		return addr < 0xA130EE ? 0x4D41 : 0x5253; //"MARS"
	}
	if(addr >= 0xA15200) {
		//Palette (words only), while FM = 0
		if(_fm) {
			return openBus;
		}
		return _palette[(addr >> 1) & 0xFF];
	}
	if(addr >= 0xA15180) {
		if(_fm && (addr & 0x0E) != 0x0A) {
			return openBus;
		}
		return ReadVdpReg(addr & 0x0E);
	}
	switch(addr & 0x3E) {
		case 0x00:
			//REN reads 1 (reset enabled; judgment call)
			return (uint16_t)((_fm ? 0x8000 : 0) | 0x80 | (_sh2Run ? 2 : 0) | (_adapterEnable ? 1 : 0));
		case 0x02:
			return (uint16_t)(((_irqActive[1] & IrqCmd) ? 2 : 0) | ((_irqActive[0] & IrqCmd) ? 1 : 0));
		case 0x04: return _romBank;
		case 0x06: return (uint16_t)((_fifoCount == FifoSize ? 0x80 : 0) | (_dreq68s ? 4 : 0) | (_dreqDma ? 2 : 0) | (_dreqRv ? 1 : 0));
		case 0x08: return (uint16_t)((_dreqSource >> 16) & 0xFF);
		case 0x0A: return (uint16_t)_dreqSource;
		case 0x0C: return (uint16_t)((_dreqDest >> 16) & 0xFF);
		case 0x0E: return (uint16_t)_dreqDest;
		case 0x10: return _dreqLength;
		case 0x1A: return _cartridgeMode ? 1 : 0;
		case 0x20:
		case 0x22:
		case 0x24:
		case 0x26:
		case 0x28:
		case 0x2A:
		case 0x2C:
		case 0x2E:
			return _comm[(addr >> 1) & 7];
		case 0x30:
		case 0x32:
		case 0x34:
		case 0x36:
		case 0x38:
			return ReadPwmReg(addr & 0x0E);
	}
	return 0;
}

void M32xUnit::MainWriteRegister(uint32_t addr, bool upper, bool lower, uint16_t value)
{
	if(addr < 0xA15100) {
		return;
	}
	if(addr >= 0xA15200) {
		if(!_fm) {
			uint16_t& p = _palette[(addr >> 1) & 0xFF];
			if(upper) {
				p = (uint16_t)((p & 0x00FF) | (value & 0xFF00));
			}
			if(lower) {
				p = (uint16_t)((p & 0xFF00) | (value & 0x00FF));
			}
		}
		return;
	}
	if(addr >= 0xA15180) {
		if(!_fm) {
			WriteVdpReg(addr & 0x0E, upper, lower, value);
		}
		return;
	}
	switch(addr & 0x3E) {
		case 0x00:
			if(lower) {
				_adapterEnable = value & 1;
				bool run = value & 2;
				if(run && !_sh2Run) {
					//Leaving reset: both SH-2s start from their power-on vectors
					_sh2[0].Reset(false);
					_sh2[1].Reset(false);
				}
				_sh2Run = run;
			}
			if(upper) {
				_fm = value & 0x8000;
			}
			break;
		case 0x02:
			if(lower) {
				//INTM/INTS: CMD interrupt requests for the master and slave. Plain read/write bits:
				//Sega's Mars Check Program writes $02 after $01 and expects $02 back (findings S2)
				for(int i = 0; i < 2; i++) {
					if(value & (1 << i)) {
						_irqActive[i] |= IrqCmd;
					} else {
						_irqActive[i] &= ~IrqCmd;
					}
				}
				UpdateIrls();
			}
			break;
		case 0x04:
			if(lower) {
				_romBank = value & 3;
			}
			break;
		case 0x06:
			if(lower) {
				_dreqRv = value & 1;
				_dreqDma = value & 2;
				bool start = value & 4;
				if(_dreq68s && !start) {
					//Ending a transfer flushes the FIFO (ares)
					_fifoCount = 0;
					UpdateDreq0();
				}
				_dreq68s = start;
			}
			break;
		case 0x08:
			if(lower) {
				_dreqSource = (_dreqSource & 0x00FFFF) | ((uint32_t)(value & 0xFF) << 16);
			}
			break;
		case 0x0A:
			_dreqSource = (_dreqSource & 0xFF0000) | (upper ? (value & 0xFF00) : (_dreqSource & 0xFF00)) | (lower ? (value & 0x00FF) : (_dreqSource & 0x00FF));
			break;
		case 0x0C:
			if(lower) {
				_dreqDest = (_dreqDest & 0x00FFFF) | ((uint32_t)(value & 0xFF) << 16);
			}
			break;
		case 0x0E:
			_dreqDest = (_dreqDest & 0xFF0000) | (upper ? (value & 0xFF00) : (_dreqDest & 0xFF00)) | (lower ? (value & 0x00FF) : (_dreqDest & 0x00FF));
			break;
		case 0x10:
			if(upper) {
				_dreqLength = (uint16_t)((_dreqLength & 0x00FF) | (value & 0xFF00));
			}
			if(lower) {
				_dreqLength = (uint16_t)((_dreqLength & 0xFF00) | (value & 0x00FC));
			}
			break;
		case 0x12:
			//FIFO: words while 68S is set; the transfer ends after `length` words (ares)
			if(_dreq68s && _fifoCount < FifoSize) {
				_fifo[(_fifoHead + _fifoCount) % FifoSize] = value;
				_fifoCount++;
				if(--_dreqLength == 0) {
					_dreq68s = false;
				}
				UpdateDreq0();
			}
			break;
		case 0x1A:
			if(lower) {
				_cartridgeMode = value & 1;
			}
			break;
		case 0x20:
		case 0x22:
		case 0x24:
		case 0x26:
		case 0x28:
		case 0x2A:
		case 0x2C:
		case 0x2E: {
			uint16_t& c = _comm[(addr >> 1) & 7];
			if(upper) {
				c = (uint16_t)((c & 0x00FF) | (value & 0xFF00));
			}
			if(lower) {
				c = (uint16_t)((c & 0xFF00) | (value & 0x00FF));
			}
			break;
		}
		case 0x30:
		case 0x32:
		case 0x34:
		case 0x36:
		case 0x38:
			WritePwmReg(addr & 0x0E, upper, lower, value, false);
			break;
	}
}

// ---------------------------------------------------------------------------------------------
// 68000 memory ($000000-$0000FF vectors, $800000-$9FFFFF) while ADEN = 1
// ---------------------------------------------------------------------------------------------

uint16_t M32xUnit::ReadVector(uint32_t addr)
{
	addr &= 0xFE;
	if(addr == 0x70) {
		return (uint16_t)(_vectorLevel4 >> 16);
	} else if(addr == 0x72) {
		return (uint16_t)_vectorLevel4;
	}
	return (uint16_t)(_vectorRom[addr] << 8 | _vectorRom[addr + 1]);
}

void M32xUnit::WriteVector(uint32_t addr, bool upper, bool lower, uint16_t value)
{
	addr &= 0xFE;
	if(addr != 0x70 && addr != 0x72) {
		return;
	}
	bool high = addr == 0x70;
	uint16_t half = high ? (uint16_t)(_vectorLevel4 >> 16) : (uint16_t)_vectorLevel4;
	if(upper) {
		half = (uint16_t)((half & 0x00FF) | (value & 0xFF00));
	}
	if(lower) {
		half = (uint16_t)((half & 0xFF00) | (value & 0x00FF));
	}
	_vectorLevel4 = high ? ((_vectorLevel4 & 0x0000FFFF) | ((uint32_t)half << 16)) : ((_vectorLevel4 & 0xFFFF0000) | half);
}

uint16_t M32xUnit::ReadCart(uint32_t addr)
{
	return _memoryManager->ReadCartWord(addr & 0x3FFFFE);
}

uint16_t M32xUnit::MainRead(uint32_t addr, uint16_t openBus)
{
	if(addr >= 0x880000 && addr < 0x900000) {
		return _dreqRv ? openBus : ReadCart(addr & 0x7FFFF);
	} else if(addr >= 0x900000) {
		return _dreqRv ? openBus : ReadCart(((uint32_t)_romBank << 20) | (addr & 0xFFFFF));
	} else if(addr >= 0x840000 && addr < 0x880000) {
		if(_fm) {
			return openBus;
		}
		uint32_t a = addr & (FrameBufferSize - 2);
		uint8_t* fb = _frameBuffer[!_fbDisplay];
		return (uint16_t)(fb[a] << 8 | fb[a + 1]);
	}
	return openBus;
}

void M32xUnit::MainWrite(uint32_t addr, bool upper, bool lower, uint16_t value)
{
	if(addr >= 0x840000 && addr < 0x880000 && !_fm) {
		FrameBufferWrite(addr, upper, lower, value, addr >= 0x860000);
	}
}

uint16_t M32xUnit::PeekMain(uint32_t addr)
{
	if(addr >= 0x880000 && addr < 0x900000) {
		return ReadCart(addr & 0x7FFFF);
	} else if(addr >= 0x900000) {
		return ReadCart(((uint32_t)_romBank << 20) | (addr & 0xFFFFF));
	} else if(addr >= 0x840000 && addr < 0x880000) {
		uint32_t a = addr & (FrameBufferSize - 2);
		uint8_t* fb = _frameBuffer[!_fbDisplay];
		return (uint16_t)(fb[a] << 8 | fb[a + 1]);
	}
	return 0xFFFF;
}

// ---------------------------------------------------------------------------------------------
// SH-2 bus (32X Hardware Manual figure 3.2). Wait states are ares' figures (bus-internal.cpp),
// a judgment call until a hardware source is found; SDRAM costs 2 cycles per access (judgment
// call: ares charges a 12-cycle burst per cache line fill instead)
// ---------------------------------------------------------------------------------------------

uint16_t M32xUnit::ShRead16(int cpu, uint32_t addr)
{
	M32xSh2& sh2 = _sh2[cpu];
	addr &= 0x1FFFFFFE;
	switch(addr >> 24) {
		case 0x00:
			if(addr < 0x4000) {
				sh2.AddCycles(1);
				vector<uint8_t>& rom = _bootRom[cpu];
				uint32_t a = addr & (uint32_t)(rom.size() - 1);
				return (uint16_t)(rom[a] << 8 | rom[a + 1]);
			} else if(addr < 0x4100) {
				sh2.AddCycles(1);
				return ReadSysReg(cpu, addr);
			} else if(addr < 0x4200) {
				sh2.AddCycles(1);
				if((addr & 0x0E) != 0x0A && !_fm) {
					return 0;
				}
				return ReadVdpReg(addr & 0x0E);
			} else if(addr < 0x4400) {
				if(!_fm) {
					return 0;
				}
				sh2.AddCycles(4);
				return _palette[(addr >> 1) & 0xFF];
			}
			break;

		case 0x02:
		case 0x03:
			//Cartridge ($02000000-$023FFFFF)
			sh2.AddCycles(6);
			return ReadCart(addr & 0x3FFFFF);

		case 0x04:
		case 0x05:
			//Frame buffer and overwrite image read the same DRAM; SH-2 access needs FM = 1
			if(!_fm || FrameBufferBusy(sh2.GetCycleCount())) {
				return 0;
			}
			sh2.AddCycles(5);
			{
				uint32_t a = addr & (FrameBufferSize - 2);
				uint8_t* fb = _frameBuffer[!_fbDisplay];
				return (uint16_t)(fb[a] << 8 | fb[a + 1]);
			}

		case 0x06:
		case 0x07: {
			sh2.AddCycles(2);
			uint32_t a = addr & (SdramSize - 2);
			return (uint16_t)(_sdram[a] << 8 | _sdram[a + 1]);
		}
	}
	return 0;
}

void M32xUnit::ShWrite16(int cpu, uint32_t addr, bool upper, bool lower, uint16_t value)
{
	M32xSh2& sh2 = _sh2[cpu];
	addr &= 0x1FFFFFFE;
	switch(addr >> 24) {
		case 0x00:
			if(addr >= 0x4000 && addr < 0x4100) {
				sh2.AddCycles(1);
				WriteSysReg(cpu, addr, upper, lower, value);
			} else if(addr >= 0x4100 && addr < 0x4200) {
				sh2.AddCycles(1);
				if(_fm) {
					WriteVdpReg(addr & 0x0E, upper, lower, value);
				}
			} else if(addr >= 0x4200 && addr < 0x4400) {
				if(_fm) {
					sh2.AddCycles(4);
					uint16_t& p = _palette[(addr >> 1) & 0xFF];
					if(upper) {
						p = (uint16_t)((p & 0x00FF) | (value & 0xFF00));
					}
					if(lower) {
						p = (uint16_t)((p & 0xFF00) | (value & 0x00FF));
					}
				}
			}
			break;

		case 0x04:
		case 0x05:
			if(_fm && !FrameBufferBusy(sh2.GetCycleCount())) {
				sh2.AddCycles(4);
				FrameBufferWrite(addr, upper, lower, value, (addr & 0x20000) != 0);
			}
			break;

		case 0x06:
		case 0x07: {
			sh2.AddCycles(1);
			uint32_t a = addr & (SdramSize - 2);
			if(upper) {
				_sdram[a] = (uint8_t)(value >> 8);
			}
			if(lower) {
				_sdram[a + 1] = (uint8_t)value;
			}
			break;
		}
	}
}

uint8_t M32xUnit::ShPeek8(uint32_t addr)
{
	addr &= 0x1FFFFFFF;
	switch(addr >> 24) {
		case 0x00:
			if(addr < 0x4000) {
				vector<uint8_t>& rom = _bootRom[0];
				return rom[addr & (rom.size() - 1)];
			}
			break;
		case 0x02:
		case 0x03: {
			uint16_t w = ReadCart(addr & 0x3FFFFE);
			return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
		}
		case 0x04:
		case 0x05: return _frameBuffer[!_fbDisplay][addr & (FrameBufferSize - 1)];
		case 0x06:
		case 0x07: return _sdram[addr & (SdramSize - 1)];
	}
	return 0;
}

Emulator* M32xUnit::GetEmulator()
{
	return _console->GetEmulator();
}

AddressInfo M32xUnit::ShGetAbsoluteAddress(int cpu, uint32_t addr)
{
	addr &= 0x1FFFFFFF;
	if(addr < 0x4000) {
		return cpu == 0 && addr < _bootRom[0].size() ? AddressInfo { (int32_t)addr, MemoryType::M32xBootRom } : AddressInfo { -1, MemoryType::None };
	} else if(addr >= 0x4200 && addr < 0x4400) {
		return { (int32_t)(addr & 0x1FF), MemoryType::M32xPalette };
	} else if(addr >= 0x02000000 && addr < 0x02400000) {
		return { (int32_t)(addr & 0x3FFFFF), MemoryType::MdPrgRom };
	} else if(addr >= 0x04000000 && addr < 0x04040000) {
		return { (int32_t)((!_fbDisplay ? FrameBufferSize : 0) + (addr & (FrameBufferSize - 1))), MemoryType::M32xFrameBuffer };
	} else if(addr >= 0x06000000 && addr < 0x08000000) {
		return { (int32_t)(addr & (SdramSize - 1)), MemoryType::M32xSdram };
	}
	return { -1, MemoryType::None };
}

int32_t M32xUnit::ShGetRelativeAddress(AddressInfo& absAddress)
{
	switch(absAddress.Type) {
		case MemoryType::M32xSdram: return 0x06000000 | absAddress.Address;
		case MemoryType::M32xPalette: return 0x4200 | absAddress.Address;
		case MemoryType::M32xBootRom: return absAddress.Address;
		case MemoryType::MdPrgRom: return absAddress.Address < 0x400000 ? (0x02000000 | absAddress.Address) : -1;
		case MemoryType::M32xFrameBuffer: {
			bool back = (absAddress.Address >= (int32_t)FrameBufferSize) != _fbDisplay;
			return back ? (0x04000000 | (absAddress.Address & (FrameBufferSize - 1))) : -1;
		}
		default: return -1;
	}
}

void M32xUnit::ShDebugWrite(uint32_t addr, uint8_t value)
{
	addr &= 0x1FFFFFFF;
	if(addr >= 0x06000000 && addr < 0x08000000) {
		_sdram[addr & (SdramSize - 1)] = value;
	} else if(addr >= 0x04000000 && addr < 0x04040000) {
		_frameBuffer[!_fbDisplay][addr & (FrameBufferSize - 1)] = value;
	}
}

AddressInfo M32xUnit::MainGetAbsoluteAddress(uint32_t addr)
{
	if(addr >= 0x880000 && addr < 0x900000) {
		return { (int32_t)(addr & 0x7FFFF), MemoryType::MdPrgRom };
	} else if(addr >= 0x900000 && addr < 0xA00000) {
		return { (int32_t)(((uint32_t)_romBank << 20) | (addr & 0xFFFFF)), MemoryType::MdPrgRom };
	} else if(addr >= 0x840000 && addr < 0x880000) {
		return { (int32_t)((!_fbDisplay ? FrameBufferSize : 0) + (addr & (FrameBufferSize - 1))), MemoryType::M32xFrameBuffer };
	}
	return { -1, MemoryType::None };
}

// ---------------------------------------------------------------------------------------------
// Composition (32X Hardware Manual, bitmap modes and priority; as ares' VDP::scanline/plot)
// ---------------------------------------------------------------------------------------------

void M32xUnit::CompositeLine(uint16_t* out, uint32_t y, uint32_t width)
{
	if(_lineMode == 0 || y >= (_lineLines240 ? 240u : 224u)) {
		return;
	}
	auto plot = [&](uint32_t x, uint16_t color) {
		bool through = color & 0x8000;
		bool backdrop = out[x] & 0x800;
		//PRI = 0: the Mega Drive is in front, the 32X shows through its backdrop or where the
		//pixel's through bit is set; PRI = 1: the 32X is in front unless the through bit is set
		bool show = _linePriority ? (!through || backdrop) : (through || backdrop);
		if(show) {
			out[x] = (uint16_t)(0x8000 | (color & 0x7FFF));
		}
	};

	uint32_t w = std::min<uint32_t>(width, 320);
	uint16_t lineAddr = FbWord(true, y);
	switch(_lineMode) {
		case 1: {
			//Packed pixel: one byte per dot, palette colour; dot shift moves the line by one dot
			uint32_t shift = _lineDotShift ? 1 : 0;
			for(uint32_t x = 0; x < w; x++) {
				uint32_t dot = x + shift;
				uint16_t word = FbWord(true, (uint16_t)(lineAddr + (dot >> 1)));
				uint8_t index = (dot & 1) ? (uint8_t)word : (uint8_t)(word >> 8);
				plot(x, _palette[index]);
			}
			break;
		}
		case 2:
			//Direct colour: one word per dot
			for(uint32_t x = 0; x < w; x++) {
				plot(x, FbWord(true, (uint16_t)(lineAddr + x)));
			}
			break;
		case 3: {
			//Run length: (length - 1, colour index) words
			uint16_t addr = lineAddr;
			for(uint32_t x = 0; x < w;) {
				uint16_t word = FbWord(true, addr++);
				uint32_t count = (uint32_t)(word >> 8) + 1;
				uint16_t color = _palette[word & 0xFF];
				for(uint32_t i = 0; i < count && x < w; i++) {
					plot(x++, color);
				}
			}
			break;
		}
	}
}

void M32xUnit::Serialize(Serializer& s)
{
	s.Stream(_sh2[0], "sh2m", -1);
	s.Stream(_sh2[1], "sh2s", -1);
	SV(_dreq1Pulse);
	SVArray(_sdram, SdramSize);
	SVArray(_frameBuffer[0], FrameBufferSize);
	SVArray(_frameBuffer[1], FrameBufferSize);
	SVArray(_palette, 256);
	SV(_adapterEnable);
	SV(_sh2Run);
	SV(_fm);
	SV(_vectorLevel4);
	SV(_romBank);
	SV(_cartridgeMode);
	SVArray(_comm, 8);
	SVArray(_irqActive, 2);
	SVArray(_irqMask, 2);
	SV(_hintInVblank);
	SV(_hCount);
	SV(_hCounter);
	SV(_dreqRv);
	SV(_dreq68s);
	SV(_dreqDma);
	SV(_dreqSource);
	SV(_dreqDest);
	SV(_dreqLength);
	SVArray(_fifo, FifoSize);
	SV(_fifoHead);
	SV(_fifoCount);
	SV(_vdpMode);
	SV(_vdpLines240);
	SV(_vdpPriority);
	SV(_vdpDotShift);
	SV(_fillLength);
	SV(_fillAddress);
	SV(_fillData);
	SV(_fillBusyUntil);
	SV(_fbSelect);
	SV(_fbDisplay);
	SV(_hblank);
	SV(_vblank);
	SV(_lineMode);
	SV(_linePriority);
	SV(_lineDotShift);
	SV(_lineLines240);
	SV(_pwmLMode);
	SV(_pwmRMode);
	SV(_pwmMono);
	SV(_pwmDreqEnable);
	SV(_pwmTimer);
	SV(_pwmCycle);
	SV(_pwmPeriods);
	SV(_pwmNextTick);
	SVArray(&_pwmFifo[0][0], 6);
	SVArray(_pwmFifoCount, 2);
	SVArray(_pwmOut, 2);
	SVArray(_pwmLatch, 3);
	if(!s.IsSaving()) {
		UpdateIrls();
		UpdateDreq0();
	}
}
