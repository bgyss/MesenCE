#include "pch.h"
#include "MD/MdMemoryManager.h"
#include "MD/MdConsole.h"
#include "MD/MdM68k.h"
#include "MD/MdZ80.h"
#include "MD/MdVdp.h"
#include "MD/MdApu.h"
#include "MD/MdControlManager.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/BatteryManager.h"
#include "MD/MdEeprom.h"
#include "MD/Mcd/McdUnit.h"
#include "Utilities/Sha256.h"
#include "Shared/CheatManager.h"
#include "Shared/MemoryType.h"
#include "Utilities/Serializer.h"

MdMemoryManager::MdMemoryManager()
{
}

MdMemoryManager::~MdMemoryManager()
{
	delete[] _workRam;
	delete[] _z80Ram;
	delete[] _saveRam;
}

void MdMemoryManager::Init(Emulator* emu, MdConsole* console, vector<uint8_t>& romData, MdM68k* m68k, MdZ80* z80, MdVdp* vdp, MdApu* apu, MdControlManager* controlManager, bool isPal, bool isOverseas, bool hasTmss)
{
	_emu = emu;
	_console = console;
	_m68k = m68k;
	_z80 = z80;
	_vdp = vdp;
	_apu = apu;
	_controlManager = controlManager;
	_isPal = isPal;
	_isOverseas = isOverseas;
	_hasTmss = hasTmss;

	_rom = romData;
	InitEeprom(romData);
	_hasBankMapper = romData.size() > 0x400000 || (romData.size() > 0x110 && memcmp(&romData[0x100], "SEGA SSF", 8) == 0);
	uint32_t size = 1;
	while(size < _rom.size()) {
		size <<= 1;
	}
	_rom.resize(size, 0xFF);
	_romMask = size - 1;

	_workRam = new uint8_t[0x10000];
	_z80Ram = new uint8_t[0x2000];
	console->InitializeRam(_workRam, 0x10000);
	console->InitializeRam(_z80Ram, 0x2000);

	//Battery RAM header (Sega's "RA" convention): $1B0 "RA", $1B2 flags, $1B4 start, $1B8 end.
	//Flags bits 4-3: 10 = even bytes only, 11 = odd bytes only, 00 = both.
	if(_rom.size() > 0x1BC && _rom[0x1B0] == 'R' && _rom[0x1B1] == 'A') {
		uint32_t start = (_rom[0x1B4] << 24) | (_rom[0x1B5] << 16) | (_rom[0x1B6] << 8) | _rom[0x1B7];
		uint32_t end = (_rom[0x1B8] << 24) | (_rom[0x1B9] << 16) | (_rom[0x1BA] << 8) | _rom[0x1BB];
		start &= 0xFFFFFF;
		end &= 0xFFFFFF;
		if(end > start && end - start < 0x200000) {
			uint8_t lanes = (_rom[0x1B2] >> 3) & 0x03;
			_saveRamOddOnly = lanes == 3 || (lanes != 2 && (start & 1));
			_saveRamEvenOnly = lanes == 2;
			_saveRamStart = start & ~1;
			_saveRamEnd = end | 1;
			_saveRamSize = (_saveRamOddOnly || _saveRamEvenOnly) ? ((_saveRamEnd - _saveRamStart + 1) / 2) : (_saveRamEnd - _saveRamStart + 1);
		}
	}
	if(_saveRamSize) {
		_saveRam = new uint8_t[_saveRamSize];
		memset(_saveRam, 0xFF, _saveRamSize);
		_emu->GetBatteryManager()->LoadBattery(".srm", _saveRam, _saveRamSize);
		//Battery RAM above the ROM is enabled at power-on; RAM overlapping the ROM needs $A130F1.
		_state.SramEnabled = _saveRamStart >= _rom.size();
	}
	_hasCartSave = _saveRam || _eeprom;

	_emu->RegisterMemory(MemoryType::MdPrgRom, _rom.data(), (uint32_t)_rom.size());
	_emu->RegisterMemory(MemoryType::MdWorkRam, _workRam, 0x10000);
	_emu->RegisterMemory(MemoryType::MdZ80Ram, _z80Ram, 0x2000);
	if(_saveRam) {
		_emu->RegisterMemory(MemoryType::MdSaveRam, _saveRam, _saveRamSize);
	} else if(_eeprom) {
		_emu->RegisterMemory(MemoryType::MdSaveRam, _eeprom->GetMemory(), _eeprom->GetSize());
	}

	_state.Z80Reset = true;
	_state.Z80BusRequested = false;
	_state.Io.Data[0] = _state.Io.Data[1] = _state.Io.Data[2] = 0x00;
	_state.Io.Ctrl[0] = _state.Io.Ctrl[1] = _state.Io.Ctrl[2] = 0x00;
	_state.Io.Serial[0] = _state.Io.Serial[1] = _state.Io.Serial[2] = 0x00;
	_state.TmssUnlocked = !_hasTmss;
}

bool MdMemoryManager::IsZ80Held()
{
	return _state.Z80Reset || _state.Z80BusRequested;
}

void MdMemoryManager::SyncZ80()
{
	_z80->Run(_masterClock, IsZ80Held());
}

void MdMemoryManager::AddM68kStall(uint64_t masterClocks, bool dma)
{
	ApplyRefresh();
	_masterClock += masterClocks;
	if(dma) {
		_refreshCounter = 0;
	} else {
		_refreshCounter = (uint32_t)((_refreshCounter + masterClocks) % (128 * MdTiming::M68kDivider));
	}
	_refreshSyncClock = _masterClock;
}

void MdMemoryManager::SyncAfterM68kInstruction()
{
	ApplyRefresh();
	_vdp->Run(_masterClock);
	_z80->Run(_masterClock, IsZ80Held());
	if(_mcd) {
		_mcd->Run(_masterClock);
	}
	_m68k->SetInterruptLevel(_vdp->GetM68kInterruptLevel());
	if(_vdp->IsZ80InterruptActive()) {
		_z80->SetIrqSource(SmsIrqSource::Vdp);
	} else {
		_z80->ClearIrqSource(SmsIrqSource::Vdp);
	}
}

void MdMemoryManager::AcknowledgeM68kInterrupt(uint8_t level)
{
	_vdp->AcknowledgeInterrupt(level);
}

void MdMemoryManager::TraceDevice(uint64_t clock, MdBusDevice device, MdBusOp op, uint32_t address, uint32_t value)
{
	if(_console->HasBusObserver()) {
		_console->NotifyBus({ clock, device, op, (uint8_t)(device == MdBusDevice::Vdp ? 2 : 1), address, value });
	}
}

void MdMemoryManager::InitEeprom(vector<uint8_t>& romData)
{
	MdEepromInfo info = MdEepromDatabase::Find(Sha256::Of(romData.data(), romData.size()));
	if(info.Chip == MdEepromChip::None || info.Board == MdEepromBoard::None) {
		return;
	}

	//Board wiring: MAME, src/devices/bus/megadrive/eeprom.cpp and jcart.cpp (see MdEeprom.h)
	switch(info.Board) {
		default:
		case MdEepromBoard::Sega:
			_eepromSdaAddr = _eepromSclAddr = _eepromReadAddr = 0x200001;
			_eepromSdaBit = _eepromReadBit = 0;
			_eepromSclBit = 1;
			break;
		case MdEepromBoard::Ea:
			_eepromSdaAddr = _eepromSclAddr = _eepromReadAddr = 0x200001;
			_eepromSdaBit = _eepromReadBit = 7;
			_eepromSclBit = 6;
			break;
		case MdEepromBoard::Acclaim16:
			_eepromSdaAddr = _eepromSclAddr = _eepromReadAddr = 0x200001;
			_eepromSdaBit = 0;
			_eepromSclBit = 1;
			_eepromReadBit = 1;
			break;
		case MdEepromBoard::Acclaim32:
			_eepromSdaAddr = _eepromReadAddr = 0x200001;
			_eepromSclAddr = 0x200000;
			_eepromSdaBit = _eepromSclBit = _eepromReadBit = 0;
			break;
		case MdEepromBoard::Codemasters:
			_eepromSdaAddr = _eepromSclAddr = 0x300000;
			_eepromSdaBit = 0;
			_eepromSclBit = 1;
			_eepromReadAddr = 0x380001;
			_eepromReadBit = 7;
			break;
	}

	_eeprom.reset(new MdEepromDevice(info.Chip));
	_emu->GetBatteryManager()->LoadBattery(".eep", _eeprom->GetMemory(), _eeprom->GetSize());
}

void MdMemoryManager::WriteEeprom(uint32_t addr, uint8_t value)
{
	if(addr == _eepromSclAddr) {
		_eepromScl = (value >> _eepromSclBit) & 0x01;
	}
	if(addr == _eepromSdaAddr) {
		_eepromSda = (value >> _eepromSdaBit) & 0x01;
	}
	if(addr == _eepromSclAddr || addr == _eepromSdaAddr) {
		_eeprom->SetLines(_eepromScl, _eepromSda);
	}
}

uint8_t MdMemoryManager::ReadCartByte(uint32_t addr)
{
	if(_hasCartSave) {
		if(_eeprom && addr == _eepromReadAddr) {
			//Only the SDA bit is driven (MAME returns the other bits as 0)
			return (_eeprom->ReadSda() ? 1 : 0) << _eepromReadBit;
		}
		if(_saveRam && _state.SramEnabled && IsSaveRamAddress(addr)) {
			return ReadSaveRam(addr);
		}
	}
	return _rom[MapRom(addr) & _romMask];
}

uint8_t MdMemoryManager::ReadCartBus(uint32_t addr)
{
	uint8_t value = ReadCartByte(addr);
	if(_emu->GetCheatManager()->HasCheats<CpuType::MdM68k>()) {
		_emu->GetCheatManager()->ApplyCheat<CpuType::MdM68k>(addr & 0x3FFFFF, value);
	}
	return value;
}

bool MdMemoryManager::IsSaveRamAddress(uint32_t addr)
{
	if(addr < _saveRamStart || addr > _saveRamEnd) {
		return false;
	}
	if(_saveRamOddOnly) {
		return addr & 1;
	} else if(_saveRamEvenOnly) {
		return !(addr & 1);
	}
	return true;
}

uint8_t MdMemoryManager::ReadSaveRam(uint32_t addr)
{
	uint32_t offset = addr - _saveRamStart;
	if(_saveRamOddOnly || _saveRamEvenOnly) {
		offset >>= 1;
	}
	return offset < _saveRamSize ? _saveRam[offset] : 0xFF;
}

void MdMemoryManager::WriteSaveRam(uint32_t addr, uint8_t value)
{
	uint32_t offset = addr - _saveRamStart;
	if(_saveRamOddOnly || _saveRamEvenOnly) {
		offset >>= 1;
	}
	if(offset < _saveRamSize && !_state.SramWriteProtected) {
		_saveRam[offset] = value;
	}
}

uint16_t MdMemoryManager::M68kRead(bool upper, bool lower, uint32_t addr, M68kAccess access)
{
	addr &= 0xFFFFFE;
	switch(addr >> 21) {
		case 0:
		case 1:
			if(_mcdMode2) {
				//Disc boot: the Sega CD unit (boot ROM, program RAM window, word RAM)
				_mcd->Run(_masterClock);
				return _mcd->MainRead(addr, _m68k->GetRegisters().Irc);
			}
			//Cartridge ($000000-$3FFFFF)
			return (ReadCartBus(addr) << 8) | ReadCartBus(addr | 1);

		case 5:
			if(addr < 0xA10000) {
				//Z80 area: 8-bit bus, the byte appears on both lanes
				uint8_t value = ReadZ80Area68k(addr | (lower && !upper ? 1 : 0));
				return (value << 8) | value;
			} else if(addr < 0xA10020) {
				uint8_t value = ReadIo(addr | 1);
				return (value << 8) | value;
			} else if(_mcd && (addr & 0xFFFF00) == 0xA12000) {
				_mcd->Run(_masterClock);
				return _mcd->MainReadRegister(addr);
			} else {
				return ReadControlRegister(addr, _m68k->GetRegisters().Irc);
			}

		case 6:
			//VDP ($C00000-$DFFFFF): A7-A5 and A18-A16 must be clear
			if((addr & 0xE700E0) == 0xC00000) {
				if(!_state.TmssUnlocked) {
					return 0;
				}
				_vdp->Run(_masterClock);
				uint16_t vdpValue = _vdp->ReadPort(addr & 0x1F, _m68k->GetRegisters().Irc);
				TraceDevice(_masterClock, MdBusDevice::Vdp, MdBusOp::Read, addr & 0x1C, vdpValue);
				return vdpValue;
			}
			return _m68k->GetRegisters().Irc;

		case 7:
			return (_workRam[addr & 0xFFFF] << 8) | _workRam[(addr & 0xFFFF) | 1];

		case 2:
		case 3:
			//$400000-$7FFFFF: the Sega CD unit in Mode 1 (nothing connected otherwise; in Mode 2 the
			//cartridge slot, empty)
			if(_mcd && !_mcdMode2) {
				_mcd->Run(_masterClock);
				return _mcd->MainRead(addr - 0x400000, _m68k->GetRegisters().Irc);
			}
			return _m68k->GetRegisters().Irc;

		default:
			//$800000-$9FFFFF: 32X space, nothing connected
			return _m68k->GetRegisters().Irc;
	}
}

void MdMemoryManager::M68kWrite(bool upper, bool lower, uint32_t addr, uint16_t value, M68kAccess access)
{
	addr &= 0xFFFFFE;
	switch(addr >> 21) {
		case 0:
		case 1:
			if(_mcdMode2) {
				_mcd->Run(_masterClock);
				_mcd->MainWrite(addr, upper, lower, value);
				break;
			}
			if(_eeprom) {
				if(upper) {
					WriteEeprom(addr, value >> 8);
				}
				if(lower) {
					WriteEeprom(addr | 1, (uint8_t)value);
				}
			}
			if(_saveRam && _state.SramEnabled) {
				if(upper && IsSaveRamAddress(addr)) {
					WriteSaveRam(addr, value >> 8);
				}
				if(lower && IsSaveRamAddress(addr | 1)) {
					WriteSaveRam(addr | 1, (uint8_t)value);
				}
			}
			break;

		case 5:
			if(addr < 0xA10000) {
				//A word write puts the upper byte on the Z80 bus at the even address
				if(upper) {
					WriteZ80Area68k(addr, value >> 8);
				} else {
					WriteZ80Area68k(addr | 1, (uint8_t)value);
				}
			} else if(addr < 0xA10020) {
				if(lower) {
					WriteIo(addr | 1, (uint8_t)value);
				}
			} else if(_mcd && (addr & 0xFFFF00) == 0xA12000) {
				_mcd->Run(_masterClock);
				_mcd->MainWriteRegister(addr, upper, lower, value);
			} else {
				if(upper) {
					WriteControlRegister(addr, value >> 8, true);
				}
				if(lower) {
					WriteControlRegister(addr | 1, (uint8_t)value, false);
				}
			}
			break;

		case 6:
			if((addr & 0xE700E0) == 0xC00000) {
				if(!_state.TmssUnlocked) {
					break;
				}
				uint8_t port = addr & 0x1F;
				if(port >= 0x10 && port < 0x18) {
					//PSG: byte writes on the odd address
					if(lower) {
						_apu->Run(_masterClock);
						TraceDevice(_masterClock, MdBusDevice::Psg, MdBusOp::Write, 0x11, (uint8_t)value);
						_apu->WritePsg((uint8_t)value);
					}
				} else {
					_vdp->Run(_masterClock);
					TraceDevice(_masterClock, MdBusDevice::Vdp, MdBusOp::Write, port & 0x04, value);
					_vdp->WritePort(port, value, upper, lower);
				}
			}
			break;

		case 7:
			if(upper) {
				_workRam[addr & 0xFFFF] = value >> 8;
			}
			if(lower) {
				_workRam[(addr & 0xFFFF) | 1] = (uint8_t)value;
			}
			break;

		case 2:
		case 3:
			if(_mcd && !_mcdMode2) {
				_mcd->Run(_masterClock);
				_mcd->MainWrite(addr - 0x400000, upper, lower, value);
			}
			break;

		default:
			break;
	}
}

uint8_t MdMemoryManager::ReadZ80Area68k(uint32_t addr)
{
	SyncZ80();
	if(!M68kOwnsZ80Bus()) {
		//The Z80 owns its bus; the 68000 reads nothing driven
		return 0xFF;
	}
	uint16_t z80Addr = addr & 0x7FFF;
	if(z80Addr < 0x4000) {
		return _z80Ram[z80Addr & 0x1FFF];
	} else if(z80Addr < 0x6000) {
		_apu->Run(_masterClock);
		return _apu->ReadYm(z80Addr & 0x03);
	}
	return 0xFF;
}

void MdMemoryManager::WriteZ80Area68k(uint32_t addr, uint8_t value)
{
	SyncZ80();
	if(!M68kOwnsZ80Bus()) {
		return;
	}
	uint16_t z80Addr = addr & 0x7FFF;
	if(z80Addr < 0x4000) {
		_z80Ram[z80Addr & 0x1FFF] = value;
	} else if(z80Addr < 0x6000) {
		_apu->Run(_masterClock);
		TraceDevice(_masterClock, MdBusDevice::Ym2612, MdBusOp::Write, 0x80000000 | (z80Addr & 0x03), value);
		_apu->WriteYm(z80Addr & 0x03, value);
	} else if(z80Addr < 0x6100) {
		_state.Z80Bank = ((_state.Z80Bank >> 1) | ((value & 0x01) << 8)) & 0x1FF;
	}
}

uint8_t MdMemoryManager::ReadIo(uint32_t addr)
{
	switch(addr & 0x1F) {
		case 0x01:
			//Version: bit 7 overseas, bit 6 PAL, bit 5 no expansion unit, bits 3-0 version (1 = TMSS)
			return (_isOverseas ? 0x80 : 0) | (_isPal ? 0x40 : 0) | (_mcd ? 0 : 0x20) | (_hasTmss ? 0x01 : 0x00);

		case 0x03:
		case 0x05:
		case 0x07: {
			uint8_t port = ((addr & 0x1F) - 3) >> 1;
			uint8_t ctrl = _state.Io.Ctrl[port];
			uint8_t data = _state.Io.Data[port];
			uint8_t pins = port < 2 ? _controlManager->ReadPort(port, _masterClock) : 0x7F;
			return (data & 0x80) | (data & ctrl & 0x7F) | (pins & ~ctrl & 0x7F);
		}

		case 0x09:
		case 0x0B:
		case 0x0D:
			return _state.Io.Ctrl[((addr & 0x1F) - 9) >> 1];

		case 0x0F:
		case 0x15:
		case 0x1B:
			return 0xFF; //serial TxData

		case 0x11:
		case 0x17:
		case 0x1D:
			return 0x00; //serial RxData

		case 0x13:
		case 0x19:
		case 0x1F:
			return _state.Io.Serial[((addr & 0x1F) - 0x13) / 6];

		default:
			return 0xFF;
	}
}

void MdMemoryManager::OnLightgunHit(uint8_t port, uint64_t clock)
{
	_controlManager->OnLightgunHit(port, clock);
	//TH falling on a port whose control register has bit 7 set (and TH as an input) is the VDP's
	//HL input (Eke-Eke, "Sega Genesis Lightguns")
	uint8_t ctrl = _state.Io.Ctrl[port];
	if((ctrl & 0x80) && !(ctrl & 0x40)) {
		_vdp->TriggerExternalInterrupt(clock);
	}
}

void MdMemoryManager::WriteIo(uint32_t addr, uint8_t value)
{
	switch(addr & 0x1F) {
		case 0x03:
		case 0x05:
		case 0x07: {
			uint8_t port = ((addr & 0x1F) - 3) >> 1;
			_state.Io.Data[port] = value;
			if(port < 2) {
				_controlManager->WritePort(port, value & 0x7F, _state.Io.Ctrl[port] & 0x7F, _masterClock);
			}
			break;
		}

		case 0x09:
		case 0x0B:
		case 0x0D: {
			uint8_t port = ((addr & 0x1F) - 9) >> 1;
			_state.Io.Ctrl[port] = value;
			if(port < 2) {
				_controlManager->WritePort(port, _state.Io.Data[port] & 0x7F, value & 0x7F, _masterClock);
			}
			break;
		}

		case 0x13:
		case 0x19:
		case 0x1F:
			_state.Io.Serial[((addr & 0x1F) - 0x13) / 6] = value & 0xF8;
			break;

		default:
			break;
	}
}

uint16_t MdMemoryManager::ReadControlRegister(uint32_t addr, uint16_t openBus)
{
	if(addr >= 0xA11100 && addr < 0xA11200) {
		//BUSREQ: bit 8 (bit 0 of the even byte) reads 0 while the 68000 owns the Z80 bus
		SyncZ80();
		return (openBus & 0xFEFF) | (_state.Z80BusRequested ? 0 : 0x0100);
	} else if(addr >= 0xA14000 && addr < 0xA14004 && _hasTmss) {
		return (_state.Tmss[addr & 0x02] << 8) | _state.Tmss[(addr & 0x02) | 1];
	}
	return openBus;
}

void MdMemoryManager::WriteControlRegister(uint32_t addr, uint8_t value, bool upperLane)
{
	if(addr >= 0xA11100 && addr < 0xA11200) {
		if(upperLane) {
			SyncZ80();
			_state.Z80BusRequested = value & 0x01;
		}
	} else if(addr >= 0xA11200 && addr < 0xA11300) {
		if(upperLane) {
			SyncZ80();
			bool reset = !(value & 0x01);
			if(reset && !_state.Z80Reset) {
				///RESET also resets the YM2612
				_apu->Run(_masterClock);
				_apu->ResetYm();
			}
			if(!reset && _state.Z80Reset) {
				_z80->Reset();
			}
			_state.Z80Reset = reset;
		}
	} else if(addr == 0xA130F1) {
		_state.SramEnabled = value & 0x01;
		_state.SramWriteProtected = value & 0x02;
	} else if(_hasBankMapper && addr >= 0xA130F3 && addr <= 0xA130FF && (addr & 1)) {
		_romBank[(addr - 0xA130F1) >> 1] = value & 0x3F;
	} else if(addr >= 0xA14000 && addr < 0xA14004) {
		if(_hasTmss) {
			_state.Tmss[addr & 0x03] = value;
			_state.TmssUnlocked = _state.Tmss[0] == 'S' && _state.Tmss[1] == 'E' && _state.Tmss[2] == 'G' && _state.Tmss[3] == 'A';
		}
	}
}

uint8_t MdMemoryManager::Z80Read(uint16_t addr)
{
	if(addr < 0x4000) {
		return _z80Ram[addr & 0x1FFF];
	} else if(addr < 0x6000) {
		_apu->Run(_z80->GetMasterClock());
		return _apu->ReadYm(addr & 0x03);
	} else if(addr < 0x7F00) {
		return 0xFF;
	} else if(addr < 0x7F20) {
		//VDP through the 68000 bus
		uint8_t port = addr & 0x1F;
		if(port >= 0x10) {
			return 0xFF;
		}
		_vdp->Run(_z80->GetMasterClock());
		uint16_t value = _vdp->ReadPort(port & 0x1E, 0xFFFF);
		return (port & 1) ? (uint8_t)value : (value >> 8);
	} else if(addr < 0x8000) {
		return 0xFF;
	} else {
		//68000 bank window: the Z80 waits for the 68000 bus
		_z80->AddWait(3 * MdTiming::Z80Divider);
		uint32_t m68kAddr = ((uint32_t)_state.Z80Bank << 15) | (addr & 0x7FFF);
		if(m68kAddr >= 0xA00000 && m68kAddr < 0xA10000) {
			return 0xFF; //the Z80 cannot reach its own bus through the window
		}
		if((m68kAddr & 0xE00000) == 0xC00000) {
			return 0xFF; //VDP through the window locks the machine; not modelled
		}
		return m68kAddr < 0x400000 ? ReadCartBus(m68kAddr) : DebugRead(m68kAddr);
	}
}

void MdMemoryManager::Z80Write(uint16_t addr, uint8_t value)
{
	if(addr < 0x4000) {
		_z80Ram[addr & 0x1FFF] = value;
	} else if(addr < 0x6000) {
		_apu->Run(_z80->GetMasterClock());
		TraceDevice(_z80->GetMasterClock(), MdBusDevice::Ym2612, MdBusOp::Write, 0x40000000 | (addr & 0x03), value);
		_apu->WriteYm(addr & 0x03, value);
	} else if(addr < 0x6100) {
		_state.Z80Bank = ((_state.Z80Bank >> 1) | ((value & 0x01) << 8)) & 0x1FF;
	} else if(addr >= 0x7F00 && addr < 0x7F20) {
		uint8_t port = addr & 0x1F;
		if(port >= 0x10 && port < 0x18) {
			if(port & 1) {
				_apu->Run(_z80->GetMasterClock());
				TraceDevice(_z80->GetMasterClock(), MdBusDevice::Psg, MdBusOp::Write, 0x7F11, value);
				_apu->WritePsg(value);
			}
		} else if(port < 0x10) {
			_vdp->Run(_z80->GetMasterClock());
			_vdp->WritePort(port & 0x1E, (value << 8) | value, true, true);
		}
	} else if(addr >= 0x8000) {
		_z80->AddWait(3 * MdTiming::Z80Divider);
		uint32_t m68kAddr = ((uint32_t)_state.Z80Bank << 15) | (addr & 0x7FFF);
		if(m68kAddr >= 0xE00000) {
			_workRam[m68kAddr & 0xFFFF] = value;
		} else if(m68kAddr < 0x400000 && _saveRam && _state.SramEnabled && IsSaveRamAddress(m68kAddr)) {
			WriteSaveRam(m68kAddr, value);
		}
	}
}

uint16_t MdMemoryManager::M68kPeekWord(uint32_t addr)
{
	addr &= 0xFFFFFE;
	return (DebugRead(addr) << 8) | DebugRead(addr | 1);
}

uint16_t MdMemoryManager::DmaReadWord(uint32_t addr)
{
	addr &= 0xFFFFFE;
	if(addr >= 0xE00000) {
		return (_workRam[addr & 0xFFFF] << 8) | _workRam[(addr & 0xFFFF) | 1];
	} else if(_mcdMode2 && addr < 0x400000) {
		return _mcd->DmaMainRead(addr);
	} else if(addr < 0x400000) {
		return (ReadCartBus(addr) << 8) | ReadCartBus(addr | 1);
	} else if(_mcd && !_mcdMode2 && addr < 0x800000) {
		return _mcd->DmaMainRead(addr - 0x400000);
	}
	//DMA from the Z80 area, I/O or the VDP itself returns undefined data; use the 68000 view
	return M68kPeekWord(addr);
}

uint8_t MdMemoryManager::DebugRead(uint32_t addr)
{
	addr &= 0xFFFFFF;
	if(_mcdMode2 && addr < 0x400000) {
		return _mcd->DebugMainRead(addr);
	} else if(addr < 0x400000) {
		return ReadCartByte(addr);
	} else if(addr >= 0xE00000) {
		return _workRam[addr & 0xFFFF];
	} else if(_mcd && !_mcdMode2 && addr < 0x800000) {
		return _mcd->DebugMainRead(addr - 0x400000);
	} else if(addr >= 0xA00000 && addr < 0xA04000) {
		return _z80Ram[addr & 0x1FFF];
	} else if(addr >= 0xA10000 && addr < 0xA10020) {
		if(addr & 1) {
			switch(addr & 0x1F) {
				case 0x01: return (_isOverseas ? 0x80 : 0) | (_isPal ? 0x40 : 0) | (_mcd ? 0 : 0x20) | (_hasTmss ? 0x01 : 0x00);
				case 0x03: return _state.Io.Data[0];
				case 0x05: return _state.Io.Data[1];
				case 0x07: return _state.Io.Data[2];
				case 0x09: return _state.Io.Ctrl[0];
				case 0x0B: return _state.Io.Ctrl[1];
				case 0x0D: return _state.Io.Ctrl[2];
			}
		}
		return 0xFF;
	} else if(addr == 0xA11100) {
		return _state.Z80BusRequested ? 0 : 1;
	}
	return 0xFF;
}

void MdMemoryManager::DebugWrite(uint32_t addr, uint8_t value)
{
	addr &= 0xFFFFFF;
	if(addr < 0x400000) {
		if(_saveRam && _state.SramEnabled && IsSaveRamAddress(addr)) {
			WriteSaveRam(addr, value);
		} else {
			_rom[addr & _romMask] = value;
		}
	} else if(addr >= 0xE00000) {
		_workRam[addr & 0xFFFF] = value;
	} else if(addr >= 0xA00000 && addr < 0xA04000) {
		_z80Ram[addr & 0x1FFF] = value;
	}
}

uint8_t MdMemoryManager::DebugZ80Read(uint16_t addr)
{
	if(addr < 0x4000) {
		return _z80Ram[addr & 0x1FFF];
	} else if(addr >= 0x8000) {
		return DebugRead(((uint32_t)_state.Z80Bank << 15) | (addr & 0x7FFF));
	}
	return 0xFF;
}

void MdMemoryManager::DebugZ80Write(uint16_t addr, uint8_t value)
{
	if(addr < 0x4000) {
		_z80Ram[addr & 0x1FFF] = value;
	} else if(addr >= 0x8000) {
		DebugWrite(((uint32_t)_state.Z80Bank << 15) | (addr & 0x7FFF), value);
	}
}

AddressInfo MdMemoryManager::GetAbsoluteAddress(uint32_t relAddr)
{
	relAddr &= 0xFFFFFF;
	if(relAddr < 0x400000) {
		if(_saveRam && _state.SramEnabled && IsSaveRamAddress(relAddr)) {
			uint32_t offset = relAddr - _saveRamStart;
			if(_saveRamOddOnly || _saveRamEvenOnly) {
				offset >>= 1;
			}
			return { (int32_t)offset, MemoryType::MdSaveRam };
		}
		return { (int32_t)(MapRom(relAddr) & _romMask), MemoryType::MdPrgRom };
	} else if(relAddr >= 0xE00000) {
		return { (int32_t)(relAddr & 0xFFFF), MemoryType::MdWorkRam };
	} else if(relAddr >= 0xA00000 && relAddr < 0xA04000) {
		return { (int32_t)(relAddr & 0x1FFF), MemoryType::MdZ80Ram };
	}
	return { -1, MemoryType::None };
}

int32_t MdMemoryManager::GetRelativeAddress(AddressInfo& absAddress)
{
	switch(absAddress.Type) {
		case MemoryType::MdPrgRom:
			if(_hasBankMapper) {
				//The window currently showing that bank, if any
				for(int i = 0; i < 8; i++) {
					if(_romBank[i] == (uint32_t)absAddress.Address >> 19) {
						return (i << 19) | (absAddress.Address & 0x7FFFF);
					}
				}
				return -1;
			}
			return absAddress.Address < 0x400000 ? absAddress.Address : -1;
		case MemoryType::MdWorkRam: return 0xFF0000 | (absAddress.Address & 0xFFFF);
		case MemoryType::MdZ80Ram: return 0xA00000 | (absAddress.Address & 0x1FFF);
		case MemoryType::MdSaveRam:
			if(_saveRamOddOnly) {
				return _saveRamStart + absAddress.Address * 2 + 1;
			} else if(_saveRamEvenOnly) {
				return _saveRamStart + absAddress.Address * 2;
			}
			return _saveRamStart + absAddress.Address;
		default: return -1;
	}
}

AddressInfo MdMemoryManager::GetZ80AbsoluteAddress(uint16_t relAddr)
{
	if(relAddr < 0x4000) {
		return { (int32_t)(relAddr & 0x1FFF), MemoryType::MdZ80Ram };
	} else if(relAddr >= 0x8000) {
		return GetAbsoluteAddress(((uint32_t)_state.Z80Bank << 15) | (relAddr & 0x7FFF));
	}
	return { -1, MemoryType::None };
}

int32_t MdMemoryManager::GetZ80RelativeAddress(AddressInfo& absAddress)
{
	if(absAddress.Type == MemoryType::MdZ80Ram) {
		return absAddress.Address & 0x1FFF;
	}
	int32_t m68kAddr = GetRelativeAddress(absAddress);
	if(m68kAddr >= 0 && ((uint32_t)m68kAddr >> 15) == _state.Z80Bank) {
		return 0x8000 | (m68kAddr & 0x7FFF);
	}
	return -1;
}

void MdMemoryManager::Serialize(Serializer& s)
{
	SVArray(_workRam, 0x10000);
	SVArray(_z80Ram, 0x2000);
	if(_saveRam) {
		SVArray(_saveRam, _saveRamSize);
	}
	SV(_masterClock);
	SV(_refreshCounter);
	SV(_refreshSyncClock);
	SV(_state.Z80Bank);
	SV(_state.Z80BusRequested);
	SV(_state.Z80Reset);
	SV(_state.SramEnabled);
	SV(_state.SramWriteProtected);
	SV(_state.TmssUnlocked);
	SV(_hasBankMapper);
	SVArray(_romBank, 8);
	SVArray(_state.Tmss, 4);
	SVArray(_state.Io.Data, 3);
	SVArray(_state.Io.Ctrl, 3);
	SVArray(_state.Io.Serial, 3);
	if(_eeprom) {
		SV(_eepromScl);
		SV(_eepromSda);
		SV(_eeprom);
	}
}
