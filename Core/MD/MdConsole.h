#pragma once
#include "pch.h"
#include "Shared/Interfaces/IConsole.h"
#include "Shared/SettingTypes.h"
#include "MD/MdTypes.h"
#include "Shared/CdReader.h"

class Emulator;
class MdM68k;
class MdZ80;
class MdVdp;
class MdApu;
class MdMemoryManager;
class MdControlManager;
class McdUnit;
class M32xUnit;

// Sega Mega Drive / Genesis base console: 68000, Z80, VDP, YM2612 + PSG, two pad ports.
// See docs/research/mega-drive-genesis-feasibility.md and md-verification-findings.md.
class MdConsole final : public IConsole
{
private:
	Emulator* _emu = nullptr;
	unique_ptr<MdM68k> _m68k;
	unique_ptr<MdZ80> _z80;
	unique_ptr<MdVdp> _vdp;
	unique_ptr<MdApu> _apu;
	unique_ptr<MdMemoryManager> _memoryManager;
	unique_ptr<MdControlManager> _controlManager;
	unique_ptr<McdUnit> _mcd; //Sega CD unit, when attached (Mode 1)
	unique_ptr<M32xUnit> _m32x; //32X, for 32X cartridges

	ConsoleRegion _region = ConsoleRegion::Ntsc;
	std::function<void(const MdBusEvent&)> _busObserver;
	MdModel _model = MdModel::Md1Va3;
	string _headerRegions;

	static bool HasSegaHeader(vector<uint8_t>& rom);
	static void Deinterleave(vector<uint8_t>& rom);
	ConsoleRegion PickRegion(ConsoleRegion setting);

public:
	static vector<string> GetSupportedExtensions() { return { ".md", ".gen", ".smd", ".bin", ".cue", ".32x" }; }
	//A Sega CD / Mega-CD disc: "SEGADISCSYSTEM" at the start of the first data sector
	static bool IsSegaCdDisc(DiscInfo& disc);
	static const char* GetSecurityCodeRegion(vector<uint8_t>& sector);
	static vector<string> GetSupportedSignatures() { return {}; }

	MdConsole(Emulator* emu);
	~MdConsole();

	Emulator* GetEmulator() { return _emu; }
	MdM68k* GetM68k() { return _m68k.get(); }
	MdZ80* GetZ80() { return _z80.get(); }
	MdVdp* GetVdp() { return _vdp.get(); }
	MdApu* GetApu() { return _apu.get(); }
	MdMemoryManager* GetMemoryManager() { return _memoryManager.get(); }
	MdModel GetModel() { return _model; }
	McdUnit* GetMcd() { return _mcd.get(); }
	M32xUnit* GetM32x() { return _m32x.get(); }

	//Optional bus observer (headless trace capture); unset during normal emulation
	void SetBusObserver(std::function<void(const MdBusEvent&)> observer) { _busObserver = observer; }
	__forceinline bool HasBusObserver() { return (bool)_busObserver; }
	void NotifyBus(const MdBusEvent& evt) { _busObserver(evt); }

	LoadRomResult LoadRom(VirtualFile& romFile) override;
	void Reset() override;
	void RunFrame() override;
	void ProcessEndOfFrame();
	void SaveBattery() override;

	BaseControlManager* GetControlManager() override;
	ConsoleRegion GetRegion() override;
	ConsoleType GetConsoleType() override;
	vector<CpuType> GetCpuTypes() override;
	uint64_t GetMasterClock() override;
	uint32_t GetMasterClockRate() override;
	double GetFps() override;
	BaseVideoFilter* GetVideoFilter(bool getDefaultFilter) override;
	uint32_t GetFrameCount() override;
	PpuFrameInfo GetPpuFrame() override;
	RomFormat GetRomFormat() override;
	AudioTrackInfo GetAudioTrackInfo() override;
	void ProcessAudioPlayerAction(AudioPlayerActionParams p) override;

	AddressInfo GetAbsoluteAddress(AddressInfo& relAddress) override;
	AddressInfo GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType) override;

	MdState GetState();
	void GetConsoleState(BaseState& state, ConsoleType consoleType) override;

	void InitializeRam(void* data, uint32_t length);

	void Serialize(Serializer& s) override;
};
