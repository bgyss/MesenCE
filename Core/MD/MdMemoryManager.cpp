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
#include "MD/M32x/M32xUnit.h"
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
	//The dump's own size: _rom is padded to a power of two below
	uint32_t dumpSize = (uint32_t)_rom.size();
	uint32_t size = 1;
	while(size < _rom.size()) {
		size <<= 1;
	}
	//Sonic & Knuckles on its own: $200000-$3FFFFF is its lock-on slot, not a mirror of its ROM. S&K
	//reads the header at $200100 to detect a cartridge on top, and a mirror made it find its own.
	//The empty slot reads $FF here, as GPGX (md_cart.c); the hardware's bus value isn't measured.
	//Padding at load keeps ROM reads branch-free. (Lock-on dumps are larger than 2 MiB.)
	if(_rom.size() == 0x200000 && memcmp(&_rom[0x150], "SONIC & KNUCKLES", 16) == 0) {
		size = 0x400000;
	}
	_rom.resize(size, 0xFF);
	_romMask = size - 1;
	InitMapper(dumpSize);

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
	} else if(dumpSize > 0x190) {
		//Cartridges with battery RAM but no "RA" header: by the overseas name at $150 (PicoDrive's
		//carthw.cfg "bad headers") or a product code within $180-$18D (GPGX sram.c, "autodetect
		//games with missing header infos")
		struct HeaderlessSram
		{
			uint16_t Field;
			const char* Text;
			uint32_t Start;
			uint32_t End;
			bool OddOnly;
		};
		static constexpr HeaderlessSram headerless[] = {
			{ 0x150, "  HardBall III", 0x200000, 0x20FFFF, false },
			{ 0x180, "T-50286", 0x200001, 0x203FFF, true }, //Buck Rogers - Countdown to Doomsday
			{ 0x180, "T-50086", 0x200001, 0x203FFF, true }, //PGA Tour Golf
			{ 0x180, "ACLD007", 0x200001, 0x200FFF, true }, //Winter Challenge
		};
		if(_mapper == CartMapper::Sf004) {
			//SF-004: 32 KiB on the odd bytes of $200000-$20FFFF, while register $D00 bit 7 is set (GPGX
			//sram.c and md_cart.c mapper_sf004_w; PicoDrive carthw.cfg). GPGX also mirrors it over
			//$200000-$2FFFFF; Star Odyssey only uses $200001 up (judgment call: no mirror here).
			_saveRamOddOnly = true;
			_saveRamStart = 0x200000;
			_saveRamEnd = 0x20FFFF;
			_saveRamSize = 0x8000;
		}
		for(size_t n = 0; n < sizeof(headerless) / sizeof(headerless[0]) && !_saveRamSize; n++) {
			const HeaderlessSram& entry = headerless[n];
			size_t len = strlen(entry.Text);
			//The name is matched at its offset; a product code anywhere in the 14-byte field
			size_t span = entry.Field == 0x180 ? 14 : len;
			bool found = false;
			for(size_t i = 0; i + len <= span && !found; i++) {
				found = memcmp(&_rom[entry.Field + i], entry.Text, len) == 0;
			}
			if(found) {
				_saveRamOddOnly = entry.OddOnly;
				_saveRamStart = entry.Start & ~1;
				_saveRamEnd = entry.End | 1;
				_saveRamSize = (_saveRamEnd - _saveRamStart + 1) / (entry.OddOnly ? 2 : 1);
				break;
			}
		}
		//Any other header-less ROM up to 2 MiB gets 64 KiB at $200000-$20FFFF, both lanes, as GPGX's
		//sram.c ("by default, enable backup RAM for ROM smaller than 2MB"; PicoDrive maps a header-less
		//default at $200000 too). Madden NFL 98 keeps its rosters there (odd bytes from $200001)
		//and looped forever without it. Not for Sonic & Knuckles, whose $200000 is its lock-on slot,
		//nor Pugsy (T-113016), which GPGX excludes.
		bool lockOn = memcmp(&_rom[0x150], "SONIC & KNUCKLES", 16) == 0;
		bool excluded = false;
		for(size_t i = 0; i + 8 <= 14 && !excluded; i++) {
			excluded = memcmp(&_rom[0x180 + i], "T-113016", 8) == 0;
		}
		if(!_saveRamSize && dumpSize <= 0x200000 && !lockOn && !excluded) {
			_saveRamStart = 0x200000;
			_saveRamEnd = 0x20FFFF;
			_saveRamSize = 0x10000;
		}
	}
	if(_saveRamSize) {
		_saveRam = new uint8_t[_saveRamSize];
		memset(_saveRam, 0xFF, _saveRamSize);
		_emu->GetBatteryManager()->LoadBattery(".srm", _saveRam, _saveRamSize);
		//Battery RAM above the ROM is enabled at power-on; RAM overlapping the ROM needs $A130F1.
		//(Against the dump's size: HardBall '95's RAM at $300001 is above its 3 MiB ROM but inside
		//the 4 MiB padding, and stayed disabled: "SRAM failure".)
		_state.SramEnabled = _saveRamStart >= dumpSize && _mapper != CartMapper::Sf004;
	}
	_hasCartSave = _saveRam || _eeprom;

	_emu->RegisterMemory(MemoryType::MdPrgRom, _rom.data(), _romMask + 1);
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

void MdMemoryManager::AddZ80AreaLatency()
{
	//A 68000 access to the Z80 area (Z80 RAM, YM2612, bank register) while it holds the Z80 bus
	//costs one more 68000 cycle, as in Genesis Plus GX ("Z80 bus access latency", which fixed
	//Pac-Man 2's sound engine and a Puyo Puyo 2 crash) and as PicoDrive measures. Judgment call:
	//no hardware measurement of it here (findings C4, Mega Turrican).
	AddM68kStall(MdTiming::M68kDivider, false);
}

void MdMemoryManager::AddZ80BusAccessWait()
{
	//A Z80 access to the 68000 bus (bank window, VDP and PSG at $7F00) goes through the bus
	//arbiter (Kabuto's hardware notes, "The bus system" and footnote 1):
	//- while a 68000->VDP DMA or a VDP FIFO stall holds the 68000 bus, the Z80 waits for it;
	//- the Z80 then waits about 3 of its cycles ("The average Z80 delay is around 3.3 Z80 cycles");
	//- the 68000 finishes the bus cycle it is in, gives the bus up, and gets it back after the
	//  Z80's access. It loses time only if it needs the bus meanwhile: its first access in the
	//  window waits for the window to end. Code that keeps the bus busy (a NOP loop) loses
	//  Kabuto's average of about 11 68000 cycles per Z80 access; code inside a long internal
	//  operation (MULU, DIVU, shifts) loses little or nothing.
	//The 68000's accesses during the current instruction are recorded (RecordM68kBusAccess): the
	//Z80 catches up after each instruction, so its accesses fall within the instruction just run.
	uint64_t z80Clock = _z80->GetMasterClock();
	if(z80Clock >= _m68kBusHeldFrom && z80Clock < _m68kBusHeldUntil) {
		_z80->AddWait((uint32_t)(_m68kBusHeldUntil - z80Clock));
		z80Clock = _m68kBusHeldUntil;
	}
	_z80->AddWait(3 * MdTiming::Z80Divider);

	//11 68000 cycles: Kabuto's average; GPGX matches it on the loop cartridges (findings C4)
	uint64_t windowEnd = z80Clock + 11 * MdTiming::M68kDivider;
	uint64_t access = std::max(_masterClock, z80Clock); //the next instruction's first access
	for(uint32_t i = 0; i < _m68kAccessCount; i++) {
		if(_m68kAccessClock[i] >= z80Clock) {
			access = _m68kAccessClock[i];
			break;
		}
	}
	if(access < windowEnd) {
		AddM68kStall(windowEnd - access, false);
	}
}

void MdMemoryManager::AddM68kStall(uint64_t masterClocks, bool dma)
{
	ApplyRefresh();
	if(dma) {
		HoldM68kBus(_masterClock, _masterClock + masterClocks);
	}
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
	_m68kAccessCount = 0;
	if(_mcd) {
		_mcd->Run(_masterClock);
	}
	if(_m32x) {
		_m32x->Run(_masterClock);
	}
	_m68k->SetInterruptLevel(_vdp->GetM68kInterruptLevel(_masterClock));
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

void MdMemoryManager::InitMapper(uint32_t dumpSize)
{
	auto productHas = [this](const char* text) {
		size_t len = strlen(text);
		for(size_t i = 0; i + len <= 12; i++) {
			if(memcmp(&_rom[0x182 + i], text, len) == 0) {
				return true;
			}
		}
		return false;
	};

	_mapper = CartMapper::None;
	if(dumpSize > 0x400000 || (dumpSize > 0x110 && memcmp(&_rom[0x100], "SEGA SSF", 8) == 0)) {
		_mapper = CartMapper::Sega;
	} else if(dumpSize > 0x200 && _rom[0x180] == 'S' && _rom[0x181] == 'F' && productHas("004")) {
		//Super Fighter Team's boards name themselves in the type field: "SF-004" (GPGX md_cart.c)
		_mapper = CartMapper::Sf004;
	} else if(dumpSize == 0x400000) {
		//Radica units, by header checksum and the sum of the words from $200 (GPGX md_cart.c's
		//rom_database, mapper_64k_radica_r). Sensible Soccer Plus (128 KiB banks, battery RAM) and
		//the bad Volume 1 dump are left out: neither is in the test library.
		static constexpr uint16_t radica[][2] = {
			{ 0x24F4, 0xFC84 }, //Volume 1
			{ 0xD951, 0x78D0 }, //Volume 2
			{ 0x0000, 0x1F25 }, //Volume 3 - Super Sonic Gold edition
			{ 0x1ADD, 0xA838 }, //Street Fighter II CE edition
			{ 0x104F, 0x32E9 }, //Street Fighter II CE edition (PAL)
		};
		uint16_t checksum = (_rom[0x18E] << 8) | _rom[0x18F];
		uint16_t sum = 0;
		for(uint32_t i = 0x200; i + 1 < dumpSize; i += 2) {
			sum += (_rom[i] << 8) | _rom[i + 1];
		}
		for(auto& entry : radica) {
			if(checksum == entry[0] && sum == entry[1]) {
				_mapper = CartMapper::Radica;
			}
		}
	}

	for(int i = 0; i < 8; i++) {
		_romBank[i] = i;
	}
	if(_mapper == CartMapper::Sf004) {
		//An empty page past the ROM for what the board leaves unmapped
		_emptyPage = (uint16_t)((_romMask + 1) >> 16);
		_rom.resize(_rom.size() + 0x10000, 0xFF);
		_sf004Locked = false;
		_sf004Regs[0] = 0;
		_sf004Regs[1] = 0x80;
		_sf004Regs[2] = 0;
	}
	UpdateRomPages();
}

void MdMemoryManager::UpdateRomPages()
{
	uint16_t pageMask = (uint16_t)(_romMask >> 16);
	for(int i = 0; i < 64; i++) {
		_romPage[i] = i & pageMask;
	}
	switch(_mapper) {
		case CartMapper::Sega:
			for(int i = 0; i < 64; i++) {
				_romPage[i] = (uint16_t)(((_romBank[i >> 3] << 3) | (i & 0x07)) & pageMask);
			}
			break;

		case CartMapper::Sf004: {
			//GPGX md_cart.c mapper_sf004_w and PicoDrive carthw.c carthw_sf004_write8 agree on the
			//registers and on the mirrored mode. Where they differ, this follows GPGX (judgment call):
			//ROM off ($E00 bit 5) unmaps $000000-$1FFFFF, and the "in a row" mode maps five 256 KiB
			//banks to $000000-$13FFFF and nothing above (PicoDrive: eight banks, ROM off ignored).
			//$200000-$3FFFFF shows nothing but the battery RAM.
			uint8_t mode = _sf004Regs[1];
			uint16_t first = ((_sf004Regs[2] >> 4) & 0x07) << 2;
			for(int i = 0; i < 64; i++) {
				uint16_t page = _emptyPage;
				if(i < 0x20 && !(mode & 0x20)) {
					if(mode & 0x40) {
						page = i < 0x14 ? ((first + i) & 0x1F & pageMask) : _emptyPage;
					} else {
						page = (first + (i & 0x03)) & pageMask;
					}
				}
				_romPage[i] = page;
			}
			break;
		}

		case CartMapper::Radica:
			//_romBank[0] holds the selected 64 KiB bank: page i shows bank (index | i), the board
			//ORs the index into VA21-VA16 (GPGX mapper_64k_radica_r)
			for(int i = 0; i < 64; i++) {
				_romPage[i] = (_romBank[0] | i) & pageMask;
			}
			break;

		default:
			break;
	}
}

void MdMemoryManager::WriteSf004(uint32_t addr, uint8_t value)
{
	//Writes to $000000-$00FFFF; A11-A8 select the register. Once $E00 bit 7 is written as 0, the
	//registers ignore writes until the next power-on (GPGX, PicoDrive).
	uint8_t reg = (addr >> 8) & 0x0F;
	if(_sf004Locked || reg < 0x0D) {
		return;
	}
	_sf004Regs[reg - 0x0D] = value;
	if(reg == 0x0D) {
		_state.SramEnabled = value & 0x80;
		return;
	}
	if(reg == 0x0E && !(value & 0x80)) {
		_sf004Locked = true;
	}
	UpdateRomPages();
}

void MdMemoryManager::ReadRadica(uint32_t addr)
{
	//Any read of $A13000-$A130FF selects the bank from A6-A1 (GPGX: !TIME read16 handler)
	_romBank[0] = (addr >> 1) & 0x3F;
	UpdateRomPages();
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
	return _rom[MapRom(addr)];
}

uint8_t MdMemoryManager::ReadCartBus(uint32_t addr)
{
	uint8_t value = ReadCartByte(addr);
	if(_emu->GetCheatManager()->HasCheats<CpuType::MdM68k>()) {
		_emu->GetCheatManager()->ApplyCheat<CpuType::MdM68k>(addr & 0x3FFFFF, value);
	}
	return value;
}

// A 68000 write to the cartridge ($000000-$3FFFFF; with the 32X, also its $900000 bank window):
// the SF-004 board, the EEPROM, and the battery RAM. The ROM itself ignores writes.
void MdMemoryManager::WriteCartWord(uint32_t addr, bool upper, bool lower, uint16_t value)
{
	if(_mapper == CartMapper::Sf004 && addr < 0x10000) {
		//A word write's low byte reaches the board (GPGX, PicoDrive)
		WriteSf004(addr, lower ? (uint8_t)value : (uint8_t)(value >> 8));
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
}

uint16_t MdMemoryManager::ReadCartWord(uint32_t addr)
{
	return (uint16_t)((ReadCartBus(addr) << 8) | ReadCartBus(addr | 1));
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
	RecordM68kBusAccess();
	addr &= 0xFFFFFE;
	switch(addr >> 21) {
		case 0:
		case 1:
			if(_mcdMode2) {
				//Disc boot: the Sega CD unit (boot ROM, program RAM window, word RAM)
				_mcd->Run(_masterClock);
				return _mcd->MainRead(addr, _m68k->GetRegisters().Irc);
			}
			if(_m32x && _m32x->IsAdapterEnabled()) {
				//32X (ADEN = 1): the vector ROM at $000000-$0000FF; the cartridge only while RV = 1
				if(addr < 0x100) {
					return _m32x->ReadVector(addr);
				} else if(!_m32x->IsRv()) {
					return _m68k->GetRegisters().Irc;
				}
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
			} else if(_m32x && _m32x->IsMainRegister(addr)) {
				_m32x->Run(_masterClock);
				return _m32x->MainReadRegister(addr, _m68k->GetRegisters().Irc);
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
			//$800000-$9FFFFF: the 32X while ADEN = 1, nothing connected otherwise
			if(_m32x && _m32x->IsAdapterEnabled()) {
				_m32x->Run(_masterClock);
				return _m32x->MainRead(addr, _m68k->GetRegisters().Irc);
			}
			return _m68k->GetRegisters().Irc;
	}
}

void MdMemoryManager::M68kWrite(bool upper, bool lower, uint32_t addr, uint16_t value, M68kAccess access)
{
	RecordM68kBusAccess();
	addr &= 0xFFFFFE;
	switch(addr >> 21) {
		case 0:
		case 1:
			if(_mcdMode2) {
				_mcd->Run(_masterClock);
				_mcd->MainWrite(addr, upper, lower, value);
				break;
			}
			if(_m32x && _m32x->IsAdapterEnabled()) {
				if(addr < 0x100) {
					_m32x->WriteVector(addr, upper, lower, value);
					break;
				} else if(!_m32x->IsRv()) {
					break;
				}
			}
			WriteCartWord(addr, upper, lower, value);
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
			} else if(_m32x && _m32x->IsMainRegister(addr)) {
				_m32x->Run(_masterClock);
				_m32x->MainWriteRegister(addr, upper, lower, value);
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
			if(_m32x && _m32x->IsAdapterEnabled()) {
				_m32x->Run(_masterClock);
				_m32x->MainWrite(addr, upper, lower, value);
			}
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
	AddZ80AreaLatency();
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
	AddZ80AreaLatency();
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
		//BUSREQ: bit 8 (bit 0 of the even byte) reads 0 while the 68000 owns the Z80 bus. A Z80
		//held in reset never acknowledges: the bit reads 1 (MAME megadriv.cpp, BlastEm; The
		//Terminator (Sega CD) waits for it. Findings: md-verification, Sega CD S5)
		SyncZ80();
		return (openBus & 0xFEFF) | (_state.Z80BusRequested && !_state.Z80Reset ? 0 : 0x0100);
	} else if(addr >= 0xA14000 && addr < 0xA14004 && _hasTmss) {
		return (_state.Tmss[addr & 0x02] << 8) | _state.Tmss[(addr & 0x02) | 1];
	} else if((addr & 0xFFFF00) == 0xA13000) {
		if(_mapper == CartMapper::Sf004) {
			//The bank at $000000 ($00, $10 ... $70) on the low byte (GPGX mapper_sf004_r)
			return ((_romPage[0] >> 2) & 0x07) << 4;
		} else if(_mapper == CartMapper::Radica) {
			//GPGX returns $FFFF, PicoDrive 0 (the menu doesn't use the value)
			ReadRadica(addr);
			return 0xFFFF;
		}
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
	} else if(addr == 0xA130F1 && _mapper != CartMapper::Sf004) {
		//(The SF-004 board ignores $A130F1: register $D00 switches its battery RAM)
		_state.SramEnabled = value & 0x01;
		_state.SramWriteProtected = value & 0x02;
	} else if(_mapper == CartMapper::Sega && addr >= 0xA130F3 && addr <= 0xA130FF && (addr & 1)) {
		_romBank[(addr - 0xA130F1) >> 1] = value & 0x3F;
		UpdateRomPages();
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
	} else if(addr < 0x8000) {
		//VDP through the 68000 bus: every access to $7F00-$7FFF goes through the bus arbiter
		AddZ80BusAccessWait();
		uint8_t port = addr & 0x1F;
		if(addr >= 0x7F20 || port >= 0x10) {
			return 0xFF;
		}
		_vdp->Run(_z80->GetMasterClock());
		uint16_t value = _vdp->ReadPort(port & 0x1E, 0xFFFF);
		return (port & 1) ? (uint8_t)value : (value >> 8);
	} else {
		//68000 bank window: the Z80 waits for the 68000 bus
		AddZ80BusAccessWait();
		uint32_t m68kAddr = ((uint32_t)_state.Z80Bank << 15) | (addr & 0x7FFF);
		if(m68kAddr >= 0xA00000 && m68kAddr < 0xA10000) {
			return 0xFF; //the Z80 cannot reach its own bus through the window
		}
		if((m68kAddr & 0xE00000) == 0xC00000) {
			return 0xFF; //VDP through the window locks the machine; not modelled
		}
		if(_m32x && _m32x->IsAdapterEnabled() && _m32x->IsMainRegister(m68kAddr)) {
			//32X registers as a 68000 byte read (Blackthorne, Toughman Contest, T-MEK, World Series
			//Baseball '95: their Z80 drivers poll INTM/INTS and raise CMD interrupts; PicoDrive)
			_m32x->Run(_z80->GetMasterClock());
			uint16_t value = _m32x->MainReadRegister(m68kAddr & ~1, 0xFFFF);
			return (m68kAddr & 1) ? (uint8_t)value : (uint8_t)(value >> 8);
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
	} else if(addr >= 0x7F00 && addr < 0x8000) {
		//VDP and PSG through the 68000 bus: every access goes through the bus arbiter
		AddZ80BusAccessWait();
		uint8_t port = addr & 0x1F;
		if(addr >= 0x7F20) {
			//unused
		} else if(port >= 0x10 && port < 0x18) {
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
		AddZ80BusAccessWait();
		uint32_t m68kAddr = ((uint32_t)_state.Z80Bank << 15) | (addr & 0x7FFF);
		if(m68kAddr >= 0xE00000) {
			_workRam[m68kAddr & 0xFFFF] = value;
		} else if(_m32x && _m32x->IsAdapterEnabled() && _m32x->IsMainRegister(m68kAddr)) {
			//32X registers as a 68000 byte write (see Z80Read)
			_m32x->Run(_z80->GetMasterClock());
			bool odd = m68kAddr & 1;
			_m32x->MainWriteRegister(m68kAddr & ~1, !odd, odd, (uint16_t)((value << 8) | value));
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
	} else if(_m32x && _m32x->IsAdapterEnabled() && (addr < 0x100 || addr >= 0x800000) && addr < 0xA00000) {
		return addr < 0x100 ? _m32x->ReadVector(addr) : _m32x->PeekMain(addr);
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
	} else if(_m32x && _m32x->IsAdapterEnabled() && (addr < 0x100 || (addr >= 0x800000 && addr < 0xA00000))) {
		uint16_t w = addr < 0x100 ? _m32x->ReadVector(addr) : _m32x->PeekMain(addr & ~1);
		return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
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
		return _state.Z80BusRequested && !_state.Z80Reset ? 0 : 1;
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
			uint32_t romAddr = MapRom(addr);
			if(romAddr <= _romMask) {
				_rom[romAddr] = value;
			}
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
	if(_mcd && _mcdMode2 && relAddr < 0x400000) {
		return _mcd->MainGetAbsoluteAddress(relAddr);
	} else if(_mcd && !_mcdMode2 && relAddr >= 0x400000 && relAddr < 0x800000) {
		return _mcd->MainGetAbsoluteAddress(relAddr - 0x400000);
	} else if(_m32x && _m32x->IsAdapterEnabled() && relAddr >= 0x800000 && relAddr < 0xA00000) {
		return _m32x->MainGetAbsoluteAddress(relAddr);
	} else if(_m32x && _m32x->IsAdapterEnabled() && !_m32x->IsRv() && relAddr < 0x400000) {
		return { -1, MemoryType::None }; //the vector ROM, or nothing (the cartridge is at $880000)
	} else if(relAddr < 0x400000) {
		if(_saveRam && _state.SramEnabled && IsSaveRamAddress(relAddr)) {
			uint32_t offset = relAddr - _saveRamStart;
			if(_saveRamOddOnly || _saveRamEvenOnly) {
				offset >>= 1;
			}
			return { (int32_t)offset, MemoryType::MdSaveRam };
		}
		uint32_t romAddr = MapRom(relAddr);
		return romAddr <= _romMask ? AddressInfo { (int32_t)romAddr, MemoryType::MdPrgRom } : AddressInfo { -1, MemoryType::None };
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
			if(_mapper != CartMapper::None) {
				//The first page currently showing that part of the ROM, if any
				for(int i = 0; i < 64; i++) {
					if(_romPage[i] == (uint32_t)absAddress.Address >> 16) {
						return (i << 16) | (absAddress.Address & 0xFFFF);
					}
				}
				return -1;
			}
			if(_m32x && _m32x->IsAdapterEnabled() && !_m32x->IsRv()) {
				//32X: the cartridge's first 512 KiB at $880000
				return absAddress.Address < 0x80000 ? (0x880000 | absAddress.Address) : -1;
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
	SV(_m68kBusHeldFrom);
	SV(_m68kBusHeldUntil);
	SV(_refreshSyncClock);
	SV(_state.Z80Bank);
	SV(_state.Z80BusRequested);
	SV(_state.Z80Reset);
	SV(_state.SramEnabled);
	SV(_state.SramWriteProtected);
	SV(_state.TmssUnlocked);
	SVArray(_romBank, 8);
	SVArray(_romPage, 64);
	SVArray(_sf004Regs, 3);
	SV(_sf004Locked);
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
