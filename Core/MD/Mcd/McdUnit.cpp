#include "pch.h"
#include "MD/Mcd/McdUnit.h"
#include "MD/MdConsole.h"
#include "Utilities/Serializer.h"

void McdM68k::Idle(uint32_t clocks)
{
	_unit->SubAdvance(clocks);
}

void McdM68k::Wait(uint32_t clocks)
{
	_unit->SubAdvance(clocks);
}

uint16_t McdM68k::BusRead(bool upper, bool lower, uint32_t address, M68kAccess /*access*/)
{
	return _unit->SubRead(upper, lower, address);
}

void McdM68k::BusWrite(bool upper, bool lower, uint32_t address, uint16_t data, M68kAccess /*access*/)
{
	if(!(upper && lower)) {
		//A byte write drives the byte on both data lanes (as MdM68k::BusWrite)
		uint8_t byte = upper ? (data >> 8) : (uint8_t)data;
		data = (byte << 8) | byte;
	}
	_unit->SubWrite(upper, lower, address, data);
}

uint16_t McdM68k::Peek(uint32_t address)
{
	return _unit->SubRead(true, true, address);
}

uint32_t McdM68k::AcknowledgeInterrupt(uint8_t level)
{
	_unit->SubAcknowledgeInterrupt(level);
	return M68kCore::AcknowledgeInterrupt(level);
}

McdUnit::~McdUnit()
{
	delete[] _prgRam;
}

void McdUnit::Init(MdConsole* console, uint32_t mclkRate)
{
	_console = console;
	_mclkRate = mclkRate;
	_prgRam = new uint8_t[PrgRamSize];
	console->InitializeRam(_prgRam, PrgRamSize);
	console->InitializeRam(_wordRam, WordRamSize);
	_cpu.Init(this);
	//Power-on: the sub CPU is held in reset; its reset vectors are fetched when it is released
	//(ares MCD::power: io.run = 0, io.halt = 1, irq.reset raised)
	_cpu.PowerOn();
	_run = false;
	_busRequest = false;
	_resetPending = true;
}

void McdUnit::AdvanceSub(uint32_t subCycles)
{
	_subCycles += subCycles;
	uint64_t num = (uint64_t)subCycles * _mclkRate + _subRemainder;
	_subClock += num / SubClockRate;
	_subRemainder = num % SubClockRate;
	//Timer and stopwatch: one count every 30.72 us = 384 sub cycles (Hardware Manual p. 30)
	_tickDivider += subCycles;
	while(_tickDivider >= 384) {
		_tickDivider -= 384;
		Tick();
	}
}

void McdUnit::Tick()
{
	//Stopwatch: 12 bits, counts 0-4095 (Hardware Manual p. 59)
	_stopwatch = (_stopwatch + 1) & 0xFFF;
	//Timer: counts down from n; reaching 0 raises level 3 and reloads n, so the interval is
	//(n + 1) counts; 0 disables it (Hardware Manual p. 30; ares MCD::Timer::clock)
	if(_timerReload) {
		if(_timerCounter == 0) {
			_timerCounter = _timerReload;
			RaiseInterrupt(3);
		} else {
			_timerCounter--;
		}
	}
}

void McdUnit::RaiseInterrupt(uint8_t level)
{
	//A source only becomes pending while its level is enabled in $FF8032 (ares MCD::IRQ::raise)
	if(_irqEnable & (1 << level)) {
		_irqPending |= 1 << level;
		UpdateInterruptLevel();
	}
}

void McdUnit::UpdateInterruptLevel()
{
	uint8_t level = 0;
	for(int i = 6; i >= 1; i--) {
		if(_irqPending & (1 << i)) {
			level = i;
			break;
		}
	}
	_cpu.SetInterruptLevel(level);
}

void McdUnit::SubAcknowledgeInterrupt(uint8_t level)
{
	_irqPending &= ~(1 << level);
	UpdateInterruptLevel();
}

void McdUnit::Run(uint64_t masterClock)
{
	while(_subClock < masterClock) {
		if(IsHalted()) {
			//Reset or bus request: the CPU is stopped, the gate array (timer, stopwatch) keeps running
			uint64_t needed = (masterClock - _subClock) * SubClockRate - _subRemainder;
			uint64_t cycles = (needed + _mclkRate - 1) / _mclkRate;
			AdvanceSub((uint32_t)std::min<uint64_t>(cycles, 0x100000));
		} else if(_resetPending) {
			//Released from reset: fetch SSP and PC from program RAM $000000/$000004
			_resetPending = false;
			_cpu.PowerOn();
			_cpu.Reset();
			UpdateInterruptLevel();
		} else {
			_cpu.Step();
		}
	}
}

uint32_t McdUnit::CellImageToWordIndex(uint32_t offset)
{
	//1M mode, main CPU $220000-$23FFFF: cell-arranged views of the main CPU's word RAM bank, for
	//32, 16, 8 and 4 vertical cells (Hardware Manual p. 16; address formulas from ares
	//bus-external.cpp, which match the manual's dot/cell address table)
	if(offset < 0x230000) {
		return ((offset & 0x0FC00) >> 8) | ((offset & 0x003FC) << 6) | (offset & 0x10002);
	} else if(offset < 0x238000) {
		return ((offset & 0x07E00) >> 7) | ((offset & 0x001FC) << 6) | (offset & 0x18002);
	} else if(offset < 0x23C000) {
		return ((offset & 0x03F00) >> 6) | ((offset & 0x000FC) << 6) | (offset & 0x1C002);
	}
	return ((offset & 0x01F80) >> 5) | ((offset & 0x0007C) << 6) | (offset & 0x1E002);
}

uint16_t McdUnit::ReadWordRamMain(uint32_t offset)
{
	if(!_wordRam1M) {
		//2M mode: all 256 KiB, while the main CPU owns it (RET)
		return _wordRam[(offset & 0x3FFFF) >> 1];
	}
	//1M mode: the main CPU's 128 KiB bank (RET = 0: bank 0, RET = 1: bank 1; Hardware Manual
	//p. 24). The banks are interleaved by word in the 256 KiB array (ares bus-external.cpp).
	uint32_t byteOffset = offset >= 0x220000 ? CellImageToWordIndex(offset) : offset;
	return _wordRam[(byteOffset & 0x1FFFE) | (_ret ? 1 : 0)];
}

void McdUnit::WriteWordRamMain(uint32_t offset, bool upper, bool lower, uint16_t value)
{
	uint32_t index;
	if(!_wordRam1M) {
		index = (offset & 0x3FFFF) >> 1;
	} else {
		uint32_t byteOffset = offset >= 0x220000 ? CellImageToWordIndex(offset) : offset;
		index = (byteOffset & 0x1FFFE) | (_ret ? 1 : 0);
	}
	uint16_t& word = _wordRam[index];
	if(upper) {
		word = (word & 0x00FF) | (value & 0xFF00);
	}
	if(lower) {
		word = (word & 0xFF00) | (value & 0x00FF);
	}
}

uint16_t McdUnit::MainRead(uint32_t offset, uint16_t openBus)
{
	//$x40000-$x7FFFF etc. mirror $x00000-$x3FFFF (address bits 18-20 are not decoded; ares)
	offset &= 0x23FFFE;
	if(offset < 0x20000) {
		//Boot ROM: no BIOS in Mode 1 tests yet (S3)
		return openBus;
	} else if(offset < 0x40000) {
		//Program RAM window: 128 KiB of the 512 KiB, while the main CPU holds the sub CPU's bus or
		//keeps it in reset (Hardware Manual p. 6, switch A)
		if(!MainOwnsPrgRam()) {
			return openBus;
		}
		uint32_t addr = ((uint32_t)_prgBank << 17) | (offset & 0x1FFFE);
		return (_prgRam[addr] << 8) | _prgRam[addr | 1];
	} else if(offset >= 0x200000) {
		if(!_wordRam1M && _wordRamToSub) {
			return openBus;
		}
		return ReadWordRamMain(offset);
	}
	return openBus;
}

void McdUnit::MainWrite(uint32_t offset, bool upper, bool lower, uint16_t value)
{
	offset &= 0x23FFFE;
	if(offset >= 0x20000 && offset < 0x40000) {
		if(!MainOwnsPrgRam()) {
			return;
		}
		uint32_t addr = ((uint32_t)_prgBank << 17) | (offset & 0x1FFFE);
		if(upper) {
			_prgRam[addr] = value >> 8;
		}
		if(lower) {
			_prgRam[addr | 1] = (uint8_t)value;
		}
	} else if(offset >= 0x200000) {
		if(!_wordRam1M && _wordRamToSub) {
			return;
		}
		WriteWordRamMain(offset, upper, lower, value);
	}
}

uint8_t McdUnit::DebugMainRead(uint32_t offset)
{
	uint16_t word = MainRead(offset & ~1, 0xFFFF);
	return (offset & 1) ? (uint8_t)word : (word >> 8);
}

uint16_t McdUnit::ReadRegister(uint32_t reg, bool fromSub)
{
	switch(reg) {
		case 0x02: {
			//Memory mode: 2M mode RET/DMNA show who owns word RAM; 1M mode RET is the bank
			//assignment and DMNA a pending swap request (Hardware Manual pp. 22-25, 57; ares)
			bool ret = _wordRam1M ? _ret : !_wordRamToSub;
			bool dmna = _wordRam1M ? _swapRequest : _wordRamToSub;
			uint16_t value = (_prgProtect << 8) | (_wordRam1M ? 0x04 : 0) | (dmna ? 0x02 : 0) | (ret ? 0x01 : 0);
			if(fromSub) {
				value |= _priorityMode << 3;
			} else {
				value |= _prgBank << 6;
			}
			return value;
		}
		case 0x0C: return _stopwatch;
		case 0x0E: return (_commFlagMain << 8) | _commFlagSub;
		default:
			if(reg >= 0x10 && reg < 0x20) {
				return _commCommand[(reg - 0x10) >> 1];
			} else if(reg >= 0x20 && reg < 0x30) {
				return _commStatus[(reg - 0x20) >> 1];
			}
			return 0;
	}
}

uint16_t McdUnit::MainReadRegister(uint32_t addr)
{
	uint32_t reg = addr & 0x3E;
	switch(reg) {
		case 0x00:
			//IEN2 (bit 15: the sub CPU's level 2 enable), IFL2 (bit 8: level 2 not yet taken),
			//SBRQ, SRES (Hardware Manual p. 56)
			return ((_irqEnable & 0x04) ? 0x8000 : 0) | ((_irqPending & 0x04) ? 0x0100 : 0) | (_busRequest ? 0x02 : 0) | (_run ? 0x01 : 0);
		case 0x04:
		case 0x08:
			return 0; //CDC (S3)
		case 0x06:
			return _hIntVector;
		default:
			return ReadRegister(reg, false);
	}
}

void McdUnit::MainWriteRegister(uint32_t addr, bool upper, bool lower, uint16_t value)
{
	uint32_t reg = addr & 0x3E;
	switch(reg) {
		case 0x00:
			if(lower) {
				//SRES = 0 resets the sub CPU, which also forces the bus request (ares, confirmed by
				//Eke-Eke per its comment). Not modelled: the gate array's forced reset sequence.
				bool run = value & 0x01;
				if(_run && !run) {
					_resetPending = true;
				}
				_run = run;
				_busRequest = run ? (value & 0x02) != 0 : true;
			}
			if(upper && (value & 0x0100)) {
				//IFL2: level 2 interrupt to the sub CPU, if it has level 2 enabled
				RaiseInterrupt(2);
			}
			break;

		case 0x02:
			if(lower) {
				//DMNA (ares MCD::writeExternalIO): 1 hands 2M word RAM to the sub CPU; 0 sets the 1M
				//swap request flag. The manual describes the 1M request as writing 1 instead; this
				//disagreement is open (findings S1), and the tests do not depend on it.
				if(value & 0x02) {
					_wordRamToSub = true;
				} else {
					_swapRequest = true;
				}
				_prgBank = (value >> 6) & 0x03;
			}
			if(upper) {
				_prgProtect = value >> 8;
			}
			break;

		case 0x06:
			if(upper) {
				_hIntVector = (_hIntVector & 0x00FF) | (value & 0xFF00);
			}
			if(lower) {
				_hIntVector = (_hIntVector & 0xFF00) | (value & 0x00FF);
			}
			break;

		case 0x0E:
			//The main CPU's flag byte is the upper byte; a write to either byte lands there (ares)
			_commFlagMain = value >> 8;
			break;

		default:
			if(reg >= 0x10 && reg < 0x20) {
				uint16_t& word = _commCommand[(reg - 0x10) >> 1];
				if(upper) {
					word = (word & 0x00FF) | (value & 0xFF00);
				}
				if(lower) {
					word = (word & 0xFF00) | (value & 0x00FF);
				}
			}
			break;
	}
}

void McdUnit::WriteSubRegister(uint32_t reg, bool upper, bool lower, uint16_t value)
{
	switch(reg) {
		case 0x00:
			if(upper) {
				_led = (value >> 8) & 0x03;
			}
			//Bit 0 = 0 resets the CD peripherals (S3)
			break;

		case 0x02: {
			//Writes land in the low byte (ares): RET, MODE, PM0-1
			bool ret = value & 0x01;
			if(ret != _ret) {
				_swapRequest = false;
			}
			_ret = ret;
			_wordRam1M = value & 0x04;
			_priorityMode = (value >> 3) & 0x03;
			if(ret) {
				//2M mode: RET = 1 returns word RAM to the main CPU
				_wordRamToSub = false;
			}
			break;
		}

		case 0x0C:
			_stopwatch = 0;
			break;

		case 0x0E:
			_commFlagSub = (uint8_t)value;
			break;

		case 0x30:
			_timerReload = _timerCounter = (uint8_t)value;
			break;

		case 0x32:
			if(lower) {
				//Level 1-6 enables. Disabling does not clear a pending level (ares leaves it open too).
				_irqEnable = value & 0x7E;
			}
			break;

		default:
			if(reg >= 0x20 && reg < 0x30) {
				uint16_t& word = _commStatus[(reg - 0x20) >> 1];
				if(upper) {
					word = (word & 0x00FF) | (value & 0xFF00);
				}
				if(lower) {
					word = (word & 0xFF00) | (value & 0x00FF);
				}
			}
			break;
	}
}

uint16_t McdUnit::SubRead(bool /*upper*/, bool /*lower*/, uint32_t address)
{
	address &= 0xFFFFFE;
	if(address < 0x80000) {
		return (_prgRam[address] << 8) | _prgRam[address | 1];
	} else if(address < 0xC0000) {
		//2M mode word RAM while the sub CPU owns it. 1M mode: the dot image (S2).
		if(!_wordRam1M && _wordRamToSub) {
			return _wordRam[(address & 0x3FFFF) >> 1];
		}
		return 0;
	} else if(address < 0xE0000) {
		//1M mode: the sub CPU's bank (the other one from the main CPU's)
		if(_wordRam1M) {
			return _wordRam[(address & 0x1FFFE) | (_ret ? 0 : 1)];
		}
		return 0;
	} else if(address >= 0xFF8000) {
		uint32_t reg = address & 0x1FE;
		switch(reg) {
			case 0x00: return (_led << 8) | 0x01; //bit 0: peripherals ready; version 0
			case 0x30: return _timerReload;
			case 0x32: return _irqEnable;
			default: return reg < 0x30 ? ReadRegister(reg, true) : 0;
		}
	}
	//Backup RAM, PCM (S2) and reserved areas
	return 0;
}

void McdUnit::SubWrite(bool upper, bool lower, uint32_t address, uint16_t value)
{
	address &= 0xFFFFFE;
	if(address < 0x80000) {
		//Write protection: $000000 up to WP x $200 (Hardware Manual pp. 14, 57)
		if(address < ((uint32_t)_prgProtect << 9)) {
			return;
		}
		if(upper) {
			_prgRam[address] = value >> 8;
		}
		if(lower) {
			_prgRam[address | 1] = (uint8_t)value;
		}
	} else if(address < 0xC0000) {
		//2M mode while the main CPU owns word RAM: the manual says the sub CPU hangs until a
		//forced reset; ares holds the bus until the RAM is handed over. Judgment call: the write is
		//dropped (findings S1).
		if(!_wordRam1M && _wordRamToSub) {
			uint16_t& word = _wordRam[(address & 0x3FFFF) >> 1];
			if(upper) {
				word = (word & 0x00FF) | (value & 0xFF00);
			}
			if(lower) {
				word = (word & 0xFF00) | (value & 0x00FF);
			}
		}
	} else if(address < 0xE0000) {
		if(_wordRam1M) {
			uint16_t& word = _wordRam[(address & 0x1FFFE) | (_ret ? 0 : 1)];
			if(upper) {
				word = (word & 0x00FF) | (value & 0xFF00);
			}
			if(lower) {
				word = (word & 0xFF00) | (value & 0x00FF);
			}
		}
	} else if(address >= 0xFF8000) {
		WriteSubRegister(address & 0x1FE, upper, lower, value);
	}
}

void McdUnit::Serialize(Serializer& s)
{
	//Sub CPU registers, in the same layout as MdM68k::Serialize (kept separate so the main CPU's
	//save-state keys do not change)
	ares::M68000::Registers& reg = _cpu.RawState();
	for(int i = 0; i < 8; i++) {
		uint32_t d = reg.d[i];
		uint32_t a = reg.a[i];
		SVI(d);
		SVI(a);
		if(!s.IsSaving()) {
			reg.d[i] = d;
			reg.a[i] = a;
		}
	}
	uint32_t sp = reg.sp;
	uint32_t pc = reg.pc;
	uint8_t mask = reg.i;
	uint16_t irc = reg.irc;
	uint16_t ir = reg.ir;
	uint16_t ird = reg.ird;
	SV(sp);
	SV(pc);
	SV(reg.c);
	SV(reg.v);
	SV(reg.z);
	SV(reg.n);
	SV(reg.x);
	SV(mask);
	SV(reg.s);
	SV(reg.t);
	SV(irc);
	SV(ir);
	SV(ird);
	SV(reg.stop);
	SV(reg.reset);
	SV(_cpu.RawIrqLevel());
	SV(_cpu.RawNmiPending());
	SV(_cpu.RawHalted());
	if(!s.IsSaving()) {
		reg.sp = sp;
		reg.pc = pc;
		reg.i = mask;
		reg.irc = irc;
		reg.ir = ir;
		reg.ird = ird;
	}

	SVArray(_prgRam, PrgRamSize);
	SVArray(_wordRam, WordRamSize / 2);
	SV(_subClock);
	SV(_subRemainder);
	SV(_subCycles);
	SV(_tickDivider);
	SV(_run);
	SV(_busRequest);
	SV(_resetPending);
	SV(_prgProtect);
	SV(_prgBank);
	SV(_wordRam1M);
	SV(_wordRamToSub);
	SV(_swapRequest);
	SV(_ret);
	SV(_priorityMode);
	SV(_hIntVector);
	SV(_commFlagMain);
	SV(_commFlagSub);
	SVArray(_commCommand, 8);
	SVArray(_commStatus, 8);
	SV(_stopwatch);
	SV(_timerReload);
	SV(_timerCounter);
	SV(_irqEnable);
	SV(_irqPending);
	SV(_led);
}
