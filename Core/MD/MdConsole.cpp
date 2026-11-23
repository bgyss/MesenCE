#include "pch.h"
#include "Shared/CheatManager.h"
#include "MD/MdConsole.h"
#include "MD/MdM68k.h"
#include "MD/MdZ80.h"
#include "MD/MdVdp.h"
#include "MD/MdApu.h"
#include "MD/MdMemoryManager.h"
#include "MD/MdEeprom.h"
#include "MD/MdControlManager.h"
#include "MD/MdDefaultVideoFilter.h"
#include "MD/Mcd/McdUnit.h"
#include "MD/M32x/M32xUnit.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/BatteryManager.h"
#include "Shared/FirmwareHelper.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/Serializer.h"
#include "Utilities/CRC32.h"

MdConsole::MdConsole(Emulator* emu)
{
	_emu = emu;
}

MdConsole::~MdConsole()
{
}

bool MdConsole::HasSegaHeader(vector<uint8_t>& rom)
{
	if(rom.size() < 0x200) {
		return false;
	}
	//"SEGA" at $100 ("SEGA MEGA DRIVE", "SEGA GENESIS", " SEGA ..." variants)
	string sys((char*)&rom[0x100], 16);
	return sys.find("SEGA") != string::npos;
}

void MdConsole::Deinterleave(vector<uint8_t>& rom)
{
	//.smd: 512-byte header, then 16 KiB blocks with the odd bytes first and the even bytes second
	vector<uint8_t> out;
	size_t blocks = (rom.size() - 0x200) / 0x4000;
	out.resize(blocks * 0x4000);
	for(size_t b = 0; b < blocks; b++) {
		uint8_t* src = rom.data() + 0x200 + b * 0x4000;
		uint8_t* dst = out.data() + b * 0x4000;
		for(int i = 0; i < 0x2000; i++) {
			dst[i * 2 + 1] = src[i];
			dst[i * 2] = src[0x2000 + i];
		}
	}
	rom = out;
}

ConsoleRegion MdConsole::PickRegion(ConsoleRegion setting)
{
	if(setting != ConsoleRegion::Auto && setting != ConsoleRegion::Dendy) {
		return setting;
	}
	//Header $1F0: "J", "U", "E" letters, or a hex digit (bit 0 Japan, bit 2 Americas, bit 3 Europe)
	bool j = false, u = false, e = false;
	for(char c : _headerRegions) {
		switch(c) {
			case 'J': j = true; break;
			case 'U': u = true; break;
			case 'E': e = true; break;
			default:
				if(isxdigit((unsigned char)c) && _headerRegions.size() <= 3 && c != ' ') {
					int v = std::stoi(string(1, c), nullptr, 16);
					j |= v & 0x01;
					u |= (v & 0x04) != 0;
					e |= (v & 0x08) != 0;
				}
				break;
		}
	}
	if(u || (!j && !e)) {
		return ConsoleRegion::Ntsc;
	} else if(j) {
		return ConsoleRegion::NtscJapan;
	}
	return ConsoleRegion::Pal;
}

//The region a Sega CD disc boots in: the boot program at $200 of the first sector starts with
//the region's security code, which the BIOS compares against its own copy before booting the
//disc. The block describes its own length (lea; jsr; bra.w past the logo data: $0A + the
//displacement). Fingerprints (CRC32) taken from every local BIOS dump: Americas $584 bytes
//(Sega CD 1/2, LaserActive), Europe $56E bytes (Mega-CD 1/2, Multi-Mega). Japanese BIOSes don't
//hold a plain copy, so other discs keep the header field (findings S5: Snatcher (U) says "J").
const char* MdConsole::GetSecurityCodeRegion(vector<uint8_t>& sector)
{
	constexpr uint32_t ip = 0x200;
	if(sector.size() < ip + 0x0C || sector[ip + 8] != 0x60 || sector[ip + 9] != 0x00) {
		return nullptr;
	}
	uint32_t length = 0x0A + ((sector[ip + 10] << 8) | sector[ip + 11]);
	if(ip + length > sector.size()) {
		return nullptr;
	}
	switch(CRC32::GetCRC(sector.data() + ip, length)) {
		case 0xF361AB57: return "U  ";
		case 0x6FFB4732: return "E  ";
		default: return nullptr;
	}
}

bool MdConsole::IsSegaCdDisc(DiscInfo& disc)
{
	if(disc.Tracks.empty() || disc.Tracks[0].Format == TrackFormat::Audio) {
		return false;
	}
	vector<uint8_t> sector;
	disc.ReadDataSector(disc.Tracks[0].FirstSector, sector);
	return sector.size() >= 14 && memcmp(sector.data(), "SEGADISCSYSTEM", 14) == 0;
}

LoadRomResult MdConsole::LoadRom(VirtualFile& romFile)
{
	vector<uint8_t> romData;
	string ext = romFile.GetFileExtension();
	unique_ptr<DiscInfo> disc;
	bool mode2 = false;
	vector<uint8_t> bios;
	if(ext == ".cue") {
		//Sega CD disc: boot from the BIOS (Mode 2)
		disc.reset(new DiscInfo());
		if(!CdReader::LoadCue(romFile, *disc)) {
			return LoadRomResult::Failure;
		}
		if(!IsSegaCdDisc(*disc)) {
			return LoadRomResult::UnknownType;
		}
		vector<uint8_t> header;
		disc->ReadDataSector(disc->Tracks[0].FirstSector, header);
		_headerRegions = string((char*)&header[0x1F0], 3);
		if(const char* securityRegion = GetSecurityCodeRegion(header)) {
			_headerRegions = securityRegion;
		}
		MdConfig& discCfg = _emu->GetSettings()->GetMdConfig();
		if(!FirmwareHelper::LoadMcdBios(_emu, PickRegion(discCfg.Region), bios)) {
			return LoadRomResult::Failure;
		}
		mode2 = true;
		//No cartridge: the main CPU's $000000-$3FFFFF belongs to the unit
		romData.assign(0x400, 0xFF);
	} else {
		romFile.ReadFile(romData);
	}
	if(mode2) {
		//(the cartridge header checks below do not apply)
	} else if(ext == ".smd" || (romData.size() > 0x200 && (romData.size() % 0x4000) == 0x200 && !HasSegaHeader(romData))) {
		Deinterleave(romData);
	}
	if(!mode2) {
		if(romData.size() < 0x200) {
			return LoadRomResult::Failure;
		}
		if(!HasSegaHeader(romData)) {
			//.bin is shared with other systems: without the Sega header this is not a Mega Drive image
			return ext == ".bin" ? LoadRomResult::UnknownType : LoadRomResult::Failure;
		}
		_headerRegions = string((char*)&romData[0x1F0], 3);
	}
	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();
	_region = PickRegion(cfg.Region);
	_model = cfg.Model;
	bool isPal = _region == ConsoleRegion::Pal;

	_m68k.reset(new MdM68k());
	_z80.reset(new MdZ80());
	_vdp.reset(new MdVdp());
	_apu.reset(new MdApu());
	_memoryManager.reset(new MdMemoryManager());
	_controlManager.reset(new MdControlManager(_emu, this));

	_memoryManager->Init(_emu, this, romData, _m68k.get(), _z80.get(), _vdp.get(), _apu.get(), _controlManager.get(), isPal, _region != ConsoleRegion::NtscJapan, _model == MdModel::Md1Va6);
	_vdp->Init(_emu, this, _memoryManager.get(), isPal);
	_apu->Init(_emu, this, isPal);
	_z80->Init(_emu, this, _memoryManager.get());
	_m68k->Init(_emu, this, _memoryManager.get(), _vdp.get());
	if(mode2 || McdUnit::IsAttachedOverride()) {
		//Mode 1 (test hook): an optional disc in the drive (mesen-headless --md-mcd-disc)
		if(!mode2) {
			//Mode 1: the BIOS is visible at $400000 and Mode 1 software calls into it (Mega-CD
			//Hardware Manual, memory map). Optional: the synthetic Mode 1 cases run without one.
			FirmwareHelper::LoadMcdBios(_emu, _region, bios, false);
		}
		if(!mode2 && !McdUnit::GetDiscOverride().empty()) {
			disc.reset(new DiscInfo());
			VirtualFile discFile(McdUnit::GetDiscOverride());
			if(!CdReader::LoadCue(discFile, *disc)) {
				return LoadRomResult::Failure;
			}
		}
		_mcd.reset(new McdUnit());
		_mcd->Init(this, GetMasterClockRate(), mode2, std::move(bios), std::move(disc));
		_memoryManager->SetMcd(_mcd.get(), mode2);
		_apu->SetMcd(_mcd.get());
	}
	if(!mode2 && (ext == ".32x" || M32xUnit::Is32xRom(romData) || M32xUnit::IsAttachedOverride())) {
		//32X: the user's Sega boot ROMs when available, MesenCE's replacements otherwise
		vector<uint8_t> masterBios, slaveBios, vectorBios;
		M32xUnit::LoadBios(_emu, masterBios, slaveBios, vectorBios);
		_m32x.reset(new M32xUnit());
		_m32x->Init(this, _memoryManager.get(), isPal, std::move(masterBios), std::move(slaveBios), std::move(vectorBios));
		_memoryManager->SetM32x(_m32x.get());
		_vdp->SetM32x(_m32x.get());
		_apu->SetM32x(_m32x.get());
	}
	_m68k->PowerOn();
	_controlManager->UpdateControlDevices();
	return LoadRomResult::Success;
}

void MdConsole::Reset()
{
	//The reset button resets the 68000 (and the Z80/YM2612 through the bus arbiter); a full
	//power cycle is the closest supported behaviour for now.
	_emu->ReloadRom(true);
}

void MdConsole::RunFrame()
{
	uint32_t frame = _vdp->GetFrameCount();
	while(frame == _vdp->GetFrameCount()) {
		_m68k->Exec();
	}
	_apu->EndFrame(_memoryManager->GetMasterClock());
}

void MdConsole::ProcessEndOfFrame()
{
	//Action Replay RAM codes are rewritten once per frame
	_emu->GetCheatManager()->RefreshRamCheats(CpuType::MdM68k);
	_controlManager->UpdateControlDevices();
	if(!_vblankInputLatch) {
		_controlManager->UpdateInputState();
	}
}

void MdConsole::ProcessVBlankStart()
{
	if(_vblankInputLatch) {
		//During frame N the input provider still serves frame N (the frame count moves at the wrap)
		_controlManager->UpdateInputState();
	}
}

void MdConsole::SaveBattery()
{
	if(_memoryManager && _memoryManager->GetSaveRamSize()) {
		_emu->GetBatteryManager()->SaveBattery(".srm", _memoryManager->GetSaveRam(), _memoryManager->GetSaveRamSize());
	}
	if(_memoryManager && _memoryManager->GetEeprom()) {
		MdEepromDevice* eeprom = _memoryManager->GetEeprom();
		_emu->GetBatteryManager()->SaveBattery(".eep", eeprom->GetMemory(), eeprom->GetSize());
	}
	if(_mcd) {
		_emu->GetBatteryManager()->SaveBattery(".brm", _mcd->GetBackupRam(), McdUnit::BackupRamSize);
	}
}

BaseControlManager* MdConsole::GetControlManager()
{
	return _controlManager.get();
}

ConsoleRegion MdConsole::GetRegion()
{
	return _region == ConsoleRegion::Pal ? ConsoleRegion::Pal : ConsoleRegion::Ntsc;
}

ConsoleType MdConsole::GetConsoleType()
{
	return ConsoleType::Md;
}

vector<CpuType> MdConsole::GetCpuTypes()
{
	if(_mcd) {
		return { CpuType::MdM68k, CpuType::MdZ80, CpuType::McdM68k };
	} else if(_m32x) {
		return { CpuType::MdM68k, CpuType::MdZ80, CpuType::M32xMaster, CpuType::M32xSlave };
	}
	return { CpuType::MdM68k, CpuType::MdZ80 };
}

uint64_t MdConsole::GetMasterClock()
{
	return _memoryManager->GetMasterClock();
}

uint32_t MdConsole::GetMasterClockRate()
{
	return _region == ConsoleRegion::Pal ? MdTiming::PalMasterClockRate : MdTiming::NtscMasterClockRate;
}

double MdConsole::GetFps()
{
	double clocksPerFrame = (double)MdTiming::ClocksPerLine * _vdp->GetLinesPerFrame();
	return GetMasterClockRate() / clocksPerFrame;
}

BaseVideoFilter* MdConsole::GetVideoFilter(bool getDefaultFilter)
{
	return new MdDefaultVideoFilter(_emu);
}

uint32_t MdConsole::GetFrameCount()
{
	return _vdp->GetFrameCount();
}

PpuFrameInfo MdConsole::GetPpuFrame()
{
	PpuFrameInfo frame = {};
	frame.FirstScanline = 0;
	frame.FrameCount = _vdp->GetFrameCount();
	frame.Width = _vdp->GetFrameWidth();
	frame.Height = _vdp->GetFrameHeight();
	frame.ScanlineCount = _vdp->GetLinesPerFrame();
	frame.CycleCount = MdTiming::ClocksPerLine;
	frame.FrameBufferSize = frame.Width * frame.Height * sizeof(uint16_t);
	frame.FrameBuffer = (uint8_t*)_vdp->GetScreenBuffer(true);
	return frame;
}

RomFormat MdConsole::GetRomFormat()
{
	return RomFormat::Md;
}

AudioTrackInfo MdConsole::GetAudioTrackInfo()
{
	return AudioTrackInfo();
}

void MdConsole::ProcessAudioPlayerAction(AudioPlayerActionParams p)
{
}

AddressInfo MdConsole::GetAbsoluteAddress(AddressInfo& relAddress)
{
	if(relAddress.Type == MemoryType::MdZ80Memory) {
		return _memoryManager->GetZ80AbsoluteAddress((uint16_t)relAddress.Address);
	} else if(relAddress.Type == MemoryType::McdMemory) {
		return _mcd ? _mcd->SubGetAbsoluteAddress(relAddress.Address) : AddressInfo { -1, MemoryType::None };
	} else if(relAddress.Type == MemoryType::M32xMasterMemory || relAddress.Type == MemoryType::M32xSlaveMemory) {
		return _m32x ? _m32x->ShGetAbsoluteAddress(relAddress.Type == MemoryType::M32xSlaveMemory, relAddress.Address) : AddressInfo { -1, MemoryType::None };
	}
	return _memoryManager->GetAbsoluteAddress(relAddress.Address);
}

AddressInfo MdConsole::GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType)
{
	if(cpuType == CpuType::MdZ80) {
		return { _memoryManager->GetZ80RelativeAddress(absAddress), MemoryType::MdZ80Memory };
	} else if(cpuType == CpuType::McdM68k) {
		return { _mcd ? _mcd->SubGetRelativeAddress(absAddress) : -1, MemoryType::McdMemory };
	} else if(cpuType == CpuType::M32xMaster || cpuType == CpuType::M32xSlave) {
		return { _m32x ? _m32x->ShGetRelativeAddress(absAddress) : -1, cpuType == CpuType::M32xMaster ? MemoryType::M32xMasterMemory : MemoryType::M32xSlaveMemory };
	}
	return { _memoryManager->GetRelativeAddress(absAddress), MemoryType::MdMemory };
}

MdState MdConsole::GetState()
{
	MdState state = {};
	state.M68k = _m68k->GetState();
	state.Z80 = _z80->GetState();
	state.Vdp = _vdp->GetState();
	state.MemoryManager = _memoryManager->GetState();
	return state;
}

void MdConsole::GetConsoleState(BaseState& state, ConsoleType consoleType)
{
	(MdState&)state = GetState();
}

void MdConsole::InitializeRam(void* data, uint32_t length)
{
	_emu->GetSettings()->InitializeRam(_emu->GetSettings()->GetMdConfig().RamPowerOnState, data, length);
}

void MdConsole::Serialize(Serializer& s)
{
	SV(_m68k);
	SV(_z80);
	SV(_vdp);
	SV(_apu);
	SV(_memoryManager);
	SV(_controlManager);
	if(_mcd) {
		SV(_mcd);
	}
	if(_m32x) {
		SV(_m32x);
	}
}
