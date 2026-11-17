#include "pch.h"
#include "Shared/CheatManager.h"
#include "MD/MdConsole.h"
#include "MD/MdM68k.h"
#include "MD/MdZ80.h"
#include "MD/MdVdp.h"
#include "MD/MdApu.h"
#include "MD/MdMemoryManager.h"
#include "MD/MdControlManager.h"
#include "MD/MdDefaultVideoFilter.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/BatteryManager.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/Serializer.h"

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

LoadRomResult MdConsole::LoadRom(VirtualFile& romFile)
{
	vector<uint8_t> romData;
	romFile.ReadFile(romData);
	string ext = romFile.GetFileExtension();
	if(ext == ".smd" || (romData.size() > 0x200 && (romData.size() % 0x4000) == 0x200 && !HasSegaHeader(romData))) {
		Deinterleave(romData);
	}
	if(romData.size() < 0x200) {
		return LoadRomResult::Failure;
	}
	if(!HasSegaHeader(romData)) {
		//.bin is shared with other systems: without the Sega header this is not a Mega Drive image
		return ext == ".bin" ? LoadRomResult::UnknownType : LoadRomResult::Failure;
	}

	_headerRegions = string((char*)&romData[0x1F0], 3);
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
	_controlManager->UpdateInputState();
}

void MdConsole::SaveBattery()
{
	if(_memoryManager && _memoryManager->GetSaveRamSize()) {
		_emu->GetBatteryManager()->SaveBattery(".srm", _memoryManager->GetSaveRam(), _memoryManager->GetSaveRamSize());
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
	}
	return _memoryManager->GetAbsoluteAddress(relAddress.Address);
}

AddressInfo MdConsole::GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType)
{
	if(cpuType == CpuType::MdZ80) {
		return { _memoryManager->GetZ80RelativeAddress(absAddress), MemoryType::MdZ80Memory };
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
}
