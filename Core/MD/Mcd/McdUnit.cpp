#include "pch.h"
#include "MD/Mcd/McdUnit.h"
#include "MD/MdConsole.h"
#include "Shared/Emulator.h"
#include "Shared/BatteryManager.h"
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

void McdUnit::Init(MdConsole* console, uint32_t mclkRate, bool mode2, vector<uint8_t> bios, unique_ptr<DiscInfo> disc)
{
	_console = console;
	_mclkRate = mclkRate;
	_mode2 = mode2;
	_bios = std::move(bios);
	_disc = std::move(disc);
	_cdc.Init(this);
	_cdd.Init(this, &_cdc, _disc.get());
	_prgRam = new uint8_t[PrgRamSize];
	console->InitializeRam(_prgRam, PrgRamSize);
	console->InitializeRam(_wordRam, WordRamSize);
	_cpu.Init(this);
	console->GetEmulator()->GetBatteryManager()->LoadBattery(".brm", _backupRam, BackupRamSize);
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
	if(_gfx.Active) {
		RunGraphics(subCycles);
	}
	//CDC DMA: one word every 6 sub cycles (ares MCD::step)
	if(_cdc.GetTransfer().Active) {
		_cdcDmaCounter += subCycles;
		while(_cdcDmaCounter >= 6) {
			_cdcDmaCounter -= 6;
			_cdc.DmaStep();
		}
	} else {
		_cdcDmaCounter = 0;
	}
	_cdd.Advance(subCycles, _subClock);
}

void McdUnit::SetInterruptLine(uint8_t level, bool active)
{
	if(active) {
		RaiseInterrupt(level);
	} else if(_irqPending & (1 << level)) {
		_irqPending &= ~(1 << level);
		UpdateInterruptLevel();
	}
}

void McdUnit::CdcDmaWrite(uint8_t destination, uint32_t address, uint16_t data)
{
	switch(destination) {
		case McdCdc::PcmDma:
			//PCM wave RAM through the current 4 KiB bank (Hardware Manual p. 27: A12 is the limit)
			_pcm.Write(0x1000 | ((address >> 1) & 0xFFF) | 1, data >> 8);
			_pcm.Write(0x1000 | ((address >> 1) & 0xFFF), (uint8_t)data);
			break;
		case McdCdc::PrgRamDma:
			SubWrite(true, true, address & 0x7FFFE, data);
			break;
		case McdCdc::WordRamDma:
			if(_wordRam1M) {
				SubWrite(true, true, 0x0C0000 | (address & 0x1FFFE), data);
			} else {
				SubWrite(true, true, 0x080000 | (address & 0x3FFFE), data);
			}
			break;
	}
}

void McdUnit::GetAudioOutput(uint64_t clock, int32_t& left, int32_t& right)
{
	int32_t pcmLeft, pcmRight, cdLeft, cdRight;
	_pcm.GetOutput(clock, pcmLeft, pcmRight);
	_cdd.GetOutput().GetOutput(clock, cdLeft, cdRight);
	left = pcmLeft + cdLeft;
	right = pcmRight + cdRight;
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
	//The PCM chip and the drive's MCU step on the same clock (ares MCD::step)
	_pcm.Clock(_subClock);
	_cdd.Tick();
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
		//Boot ROM. In Mode 2 the H-INT vector's low word ($000072) comes from $A12006 (Hardware
		//Manual p. 58); Mode 1 tests run without a BIOS.
		if(_mode2 && offset == 0x72) {
			return _hIntVector;
		}
		if(offset + 1 < _bios.size()) {
			return (_bios[offset] << 8) | _bios[offset + 1];
		}
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

uint16_t McdUnit::DmaMainRead(uint32_t offset)
{
	//VDP DMA from word RAM: each read returns the word of the previous read (the data is output
	//one cycle late; Hardware Manual p. 61, which tells programs to start the DMA one word later;
	//ares wramLatch). The first word of a transfer is the stale latch.
	offset &= 0x23FFFE;
	if(offset >= 0x200000 && (_wordRam1M || !_wordRamToSub)) {
		uint16_t value = _dmaLatch;
		_dmaLatch = ReadWordRamMain(offset);
		return value;
	}
	return MainRead(offset, 0xFFFF);
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
		case 0x04: {
			//CDC mode as the main CPU sees it: EDT, DSR, destination (Hardware Manual p. 58)
			McdCdc::Transfer& t = _cdc.GetTransfer();
			return (t.Completed ? 0x8000 : 0) | (t.Ready ? 0x4000 : 0) | (t.Destination << 8);
		}
		case 0x08:
			return _cdc.GetTransfer().Destination == McdCdc::MainRead ? _cdc.ReadHostData() : 0xFFFF;
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

		case 0x04:
			//CDC mode: destination (upper byte), register address (lower byte)
			if(upper) {
				_cdc.SetDestination((value >> 8) & 0x07);
			}
			if(lower) {
				_cdc.SetAddress(value & 0x1F);
			}
			break;

		case 0x06:
			if(lower) {
				_cdc.WriteRegister((uint8_t)value);
			}
			break;

		case 0x0A:
			_cdc.SetDmaAddress(value);
			break;

		case 0x0C:
			_stopwatch = 0;
			break;

		case 0x0E:
			_commFlagSub = (uint8_t)value;
			break;

		case 0x34:
			_cdd.WriteFader(value);
			break;

		case 0x36:
			if(lower) {
				_cdd.WriteControl(value);
			}
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
			} else if(reg >= 0x42 && reg < 0x4C) {
				_cdd.WriteCommandRegister(reg, upper, lower, value);
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
		//2M mode: word RAM while the sub CPU owns it. 1M mode: the dot image of its bank.
		if(_wordRam1M) {
			return ReadDotImage(address);
		} else if(_wordRamToSub) {
			if(_gfx.Active) {
				//A sub CPU access to word RAM delays the graphics chip by 3 clocks (Hardware Manual p. 53)
				_gfx.Counter += 3;
			}
			return _wordRam[(address & 0x3FFFF) >> 1];
		}
		return 0;
	} else if(address < 0xE0000) {
		//1M mode: the sub CPU's bank (the other one from the main CPU's)
		if(_wordRam1M) {
			return _wordRam[(address & 0x1FFFE) | (_ret ? 0 : 1)];
		}
		return 0;
	} else if(address >= 0xFE0000 && address < 0xFF0000) {
		//Backup RAM on the lower byte (Hardware Manual p. 15)
		return _backupRam[(address >> 1) & (BackupRamSize - 1)];
	} else if(address >= 0xFF0000 && address < 0xFF8000) {
		//PCM on the lower byte
		return _pcm.Read((uint16_t)(address >> 1));
	} else if(address >= 0xFF8000) {
		uint32_t reg = address & 0x1FE;
		switch(reg) {
			case 0x00: return (_led << 8) | 0x01; //bit 0: peripherals ready; version 0
			case 0x04: {
				McdCdc::Transfer& t = _cdc.GetTransfer();
				return (t.Completed ? 0x8000 : 0) | (t.Ready ? 0x4000 : 0) | (t.Destination << 8) | _cdc.GetAddress();
			}
			case 0x06: return _cdc.ReadRegister();
			case 0x08: return _cdc.GetTransfer().Destination == McdCdc::SubRead ? _cdc.ReadHostData() : 0xFFFF;
			case 0x0A: return (_cdc.GetTransfer().Address >> 3) & 0xFFFF;
			case 0x30: return _timerReload;
			case 0x32: return _irqEnable;
			case 0x34: return 0; //fader: EFDT = 0 (ready)
			case 0x36: return _cdd.ReadControl();
			case 0x68: return (_cdd.GetSubcodePosition() & 0x7F) << 1;
			default:
				if(reg >= 0x38 && reg < 0x42) {
					return _cdd.ReadStatusRegister(reg);
				} else if(reg >= 0x42 && reg < 0x4C) {
					return _cdd.ReadCommandRegister(reg);
				} else if(reg >= 0x4C && reg < 0x68) {
					return ReadGraphicsRegister(reg);
				} else if(reg >= 0x100) {
					return _cdd.ReadSubcode((reg - 0x100) >> 1);
				}
				return reg < 0x30 ? ReadRegister(reg, true) : 0;
		}
	}
	//Reserved areas
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
		if(_wordRam1M) {
			WriteDotImage(address, upper, lower, value);
		} else if(_wordRamToSub) {
			if(_gfx.Active) {
				_gfx.Counter += 3;
			}
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
	} else if(address >= 0xFE0000 && address < 0xFF0000) {
		if(lower) {
			_backupRam[(address >> 1) & (BackupRamSize - 1)] = (uint8_t)value;
		}
	} else if(address >= 0xFF0000 && address < 0xFF8000) {
		if(lower) {
			_pcm.Write((uint16_t)(address >> 1), (uint8_t)value);
		}
	} else if(address >= 0xFF8000) {
		uint32_t reg = address & 0x1FE;
		if(reg >= 0x4C && reg < 0x68) {
			WriteGraphicsRegister(reg, upper, lower, value);
		} else {
			WriteSubRegister(reg, upper, lower, value);
		}
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
	SVArray(_backupRam, BackupRamSize);
	SV(_fontColor);
	SV(_fontBits);
	SV(_gfx.Repeat);
	SV(_gfx.Stamp32);
	SV(_gfx.Map16);
	SV(_gfx.MapBase);
	SV(_gfx.VCells);
	SV(_gfx.ImageBase);
	SV(_gfx.ImageOffset);
	SV(_gfx.HDots);
	SV(_gfx.VDots);
	SV(_gfx.VectorBase);
	SV(_gfx.Active);
	SV(_gfx.ImageAddress);
	SV(_gfx.VectorAddress);
	SV(_gfx.MapAddress);
	SV(_gfx.LineClocks);
	SV(_gfx.Counter);
	SV(_dmaLatch);
	SV(_pcm);
	SV(_cdc);
	SV(_cdd);
	SV(_cdcDmaCounter);
}

uint8_t McdUnit::ReadNibble(uint32_t address)
{
	//Graphics chip view of 2M word RAM: one 4-bit dot per nibble, high nibble first
	uint16_t word = _wordRam[(address >> 2) & 0x1FFFF];
	return (word >> (12 - ((address & 3) << 2))) & 0x0F;
}

void McdUnit::WriteNibble(uint32_t address, uint8_t value)
{
	uint16_t& word = _wordRam[(address >> 2) & 0x1FFFF];
	uint8_t shift = 12 - ((address & 3) << 2);
	word = (word & ~(0x0F << shift)) | ((value & 0x0F) << shift);
}

uint16_t McdUnit::ReadDotImage(uint32_t address)
{
	//1M mode, sub CPU $080000-$0BFFFF: the sub CPU's bank, one 4-bit dot per byte (upper four bits
	//read 0). Each dot-image word is one byte of the bank (Hardware Manual pp. 14, 54; byte and bank
	//arrangement from ares bus-internal.cpp).
	uint32_t index = (((address >> 1) & 0x1FFFF) & ~1) | (_ret ? 0 : 1);
	uint8_t byte = (address & 2) ? (uint8_t)_wordRam[index] : (_wordRam[index] >> 8);
	return ((byte >> 4) << 8) | (byte & 0x0F);
}

void McdUnit::WriteDotImage(uint32_t address, bool upper, bool lower, uint16_t value)
{
	uint32_t index = (((address >> 1) & 0x1FFFF) & ~1) | (_ret ? 0 : 1);
	uint8_t shift = (address & 2) ? 0 : 8;
	uint8_t byte = (_wordRam[index] >> shift) & 0xFF;
	uint8_t hi = byte >> 4;
	uint8_t lo = byte & 0x0F;
	uint8_t newHi = (value >> 8) & 0x0F;
	uint8_t newLo = value & 0x0F;
	//Write priority (Hardware Manual pp. 23, 54): 0 = normal, 1 = underwrite (only where the old
	//dot is 0), 2 = overwrite (only non-zero new dots), 3 = prohibited (treated as no write, ares)
	switch(_priorityMode) {
		case 0:
			hi = upper ? newHi : hi;
			lo = lower ? newLo : lo;
			break;
		case 1:
			hi = (upper && !hi) ? newHi : hi;
			lo = (lower && !lo) ? newLo : lo;
			break;
		case 2:
			hi = (upper && newHi) ? newHi : hi;
			lo = (lower && newLo) ? newLo : lo;
			break;
		default:
			break;
	}
	_wordRam[index] = (_wordRam[index] & ~(0xFF << shift)) | (((hi << 4) | lo) << shift);
}

uint16_t McdUnit::ReadGraphicsRegister(uint32_t reg)
{
	switch(reg) {
		case 0x4C: return _fontColor;
		case 0x4E: return _fontBits;
		case 0x50:
		case 0x52:
		case 0x54:
		case 0x56: {
			//Font data: one nibble per font bit, $FF8050 = bits 15-12 ... $FF8056 = bits 3-0, the
			//most significant bit in the highest nibble; colour SC1 for a 1 bit, SC0 for a 0 bit
			//(Hardware Manual p. 34)
			uint32_t first = 12 - ((reg - 0x50) << 1);
			uint16_t value = 0;
			for(int i = 0; i < 4; i++) {
				bool bit = (_fontBits >> (first + i)) & 1;
				value |= (bit ? (_fontColor >> 4) : (_fontColor & 0x0F)) << (i * 4);
			}
			return value;
		}
		case 0x58: return (_gfx.Active ? 0x8000 : 0) | (_gfx.Map16 ? 0x04 : 0) | (_gfx.Stamp32 ? 0x02 : 0) | (_gfx.Repeat ? 0x01 : 0);
		case 0x5A: return (_gfx.MapBase >> 2) & 0xFFE0;
		case 0x5C: return _gfx.VCells;
		case 0x5E: return (_gfx.ImageBase >> 2) & 0xFFF8;
		case 0x60: return _gfx.ImageOffset;
		case 0x62: return _gfx.HDots;
		case 0x64: return _gfx.VDots;
		default: return 0; //$FF8066 is write-only
	}
}

void McdUnit::WriteGraphicsRegister(uint32_t reg, bool upper, bool lower, uint16_t value)
{
	switch(reg) {
		case 0x4C: _fontColor = (uint8_t)value; break;
		case 0x4E:
			if(upper) {
				_fontBits = (_fontBits & 0x00FF) | (value & 0xFF00);
			}
			if(lower) {
				_fontBits = (_fontBits & 0xFF00) | (value & 0x00FF);
			}
			break;
		case 0x58:
			if(lower) {
				_gfx.Repeat = value & 0x01;
				_gfx.Stamp32 = value & 0x02;
				_gfx.Map16 = value & 0x04;
			}
			break;
		case 0x5A: _gfx.MapBase = (value & 0xFFE0) << 2; break;
		case 0x5C: _gfx.VCells = value & 0x1F; break;
		case 0x5E: _gfx.ImageBase = (value & 0xFFF8) << 2; break;
		case 0x60: _gfx.ImageOffset = value & 0x3F; break;
		case 0x62: _gfx.HDots = value & 0x1FF; break;
		case 0x64: _gfx.VDots = (uint8_t)value; break;
		case 0x66:
			//Trace vector base address: writing it starts the operation
			_gfx.VectorBase = (value & 0xFFFE) << 2;
			StartGraphics();
			break;
	}
}

void McdUnit::StartGraphics()
{
	//Only in 2M mode (Hardware Manual p. 36: the stamp generator is the start of the 2M RAM)
	if(_wordRam1M) {
		return;
	}
	_gfx.Active = true;
	_gfx.ImageAddress = (_gfx.ImageBase << 1) + _gfx.ImageOffset;
	_gfx.VectorAddress = _gfx.VectorBase >> 1;
	//Stamp map base: the address bits that exist for each map/stamp size (Hardware Manual pp. 35-36)
	uint32_t mapAddress = _gfx.MapBase >> 1;
	if(!_gfx.Map16) {
		mapAddress &= _gfx.Stamp32 ? 0x1FFC0 : 0x1FF00;
	} else {
		mapAddress &= _gfx.Stamp32 ? 0x1C000 : 0x10000;
	}
	_gfx.MapAddress = mapAddress;
	//Time per line (Hardware Manual p. 53, 1 clock = 80 ns): 13 + 2 x (offset AND 3) + 9 x ((H dots
	//+ offset - 1) >> 2), plus 3 refresh clocks per 192. Judgment call: the scan of this formula is
	//partly illegible, "offset" is read as the dot offset and the bracket as a shift by 2.
	int32_t dotOffset = _gfx.ImageOffset & 7;
	int32_t clocks = 13 + 2 * (dotOffset & 3) + 9 * (std::max<int32_t>(_gfx.HDots + dotOffset - 1, 0) >> 2);
	_gfx.LineClocks = clocks + 3 * clocks / 192;
	_gfx.Counter = _gfx.LineClocks;
	if(_gfx.VDots == 0) {
		_gfx.Active = false;
		RaiseInterrupt(1);
	}
}

void McdUnit::RunGraphics(uint32_t subCycles)
{
	_gfx.Counter -= (int32_t)subCycles;
	while(_gfx.Active && _gfx.Counter <= 0) {
		RenderGraphicsLine();
		_gfx.ImageAddress += 8;
		_gfx.VDots--;
		if(_gfx.VDots == 0) {
			_gfx.Active = false;
			//Level 1 when the last dot is written (Hardware Manual p. 41)
			RaiseInterrupt(1);
		} else {
			_gfx.Counter += _gfx.LineClocks;
		}
	}
}

void McdUnit::RenderGraphicsLine()
{
	//One image buffer line from one trace vector (Hardware Manual pp. 39-41): start X/Y in 13.3
	//fixed point, then dX/dY per dot in 5.11; every dot samples the stamp map at (X, Y).
	//Implementation follows ares md/mcd/gpu.cpp (coordinate masks, stamp/map shifts, flips).
	uint32_t stampShift = _gfx.Stamp32 ? 5 : 4;
	uint32_t mapShift = (_gfx.Map16 ? 8 : 4) - (_gfx.Stamp32 ? 1 : 0);
	uint32_t indexMask = _gfx.Stamp32 ? 0x7FC : 0x7FF;
	uint32_t pixelMask = _gfx.Stamp32 ? 0x1F : 0x0F;
	uint32_t imageWidth = ((uint32_t)_gfx.VCells + 1) << 6;
	uint32_t mapMask = _gfx.Map16 ? 0x7FFFFF : 0x07FFFF;

	uint32_t x = ((uint32_t)_wordRam[_gfx.VectorAddress++ & 0x1FFFF] << 8) & 0xFFFFFF;
	uint32_t y = ((uint32_t)_wordRam[_gfx.VectorAddress++ & 0x1FFFF] << 8) & 0xFFFFFF;
	int32_t xStep = (int16_t)_wordRam[_gfx.VectorAddress++ & 0x1FFFF];
	int32_t yStep = (int16_t)_wordRam[_gfx.VectorAddress++ & 0x1FFFF];

	uint32_t address = _gfx.ImageAddress;
	for(uint32_t i = 0; i < _gfx.HDots; i++) {
		if(_gfx.Repeat) {
			x &= mapMask;
			y &= mapMask;
		}
		uint8_t output = 0;
		if(!((x | y) & ~mapMask)) {
			uint32_t xDot = x >> 11;
			uint32_t yDot = y >> 11;
			uint16_t entry = _wordRam[(_gfx.MapAddress + ((yDot >> stampShift) << mapShift) + (xDot >> stampShift)) & 0x1FFFF];
			uint32_t index = entry & indexMask;
			if(index) {
				//HFLP (bit 15), RT1 (bit 14: 180 degrees), RT0 (bit 13: 90 degrees)
				if(entry & 0x8000) {
					xDot = ~xDot;
				}
				if(entry & 0x4000) {
					xDot = ~xDot;
					yDot = ~yDot;
				}
				if(entry & 0x2000) {
					uint32_t t = xDot;
					xDot = ~yDot;
					yDot = t;
				}
				uint32_t xPixel = xDot & pixelMask;
				uint32_t yPixel = yDot & pixelMask;
				//Stamps are stored as cells arranged vertically, like Mega Drive sprites
				output = ReadNibble((index << 8) | ((xPixel & ~7) << stampShift) | (yPixel << 3) | (xPixel & 7));
			}
		}
		if(_priorityMode) {
			uint8_t input = ReadNibble(address);
			switch(_priorityMode) {
				case 1: output = input ? input : output; break; //underwrite
				case 2: output = output ? output : input; break; //overwrite
				default: output = input; break;
			}
		}
		WriteNibble(address, output);
		address++;
		if(!(address & 7)) {
			address += imageWidth - 8;
		}
		x = (x + xStep) & 0xFFFFFF;
		y = (y + yStep) & 0xFFFFFF;
	}
}
