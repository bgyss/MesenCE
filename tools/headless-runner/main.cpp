// mesen-headless - native, deterministic, bounded MesenCE runner for CI and cloud workers.
//
// It links the emulation core (Core/, Utilities/, Lua, SevenZip) directly: no .NET UI, no SDL
// audio device, no window server, no desktop configuration. One process owns one Emulator and
// one isolated home folder inside the output directory.
//
//   mesen-headless run --rom ROM --frames N --out NEW_DIR [--input INPUT.json]
//                      [--lua SCRIPT.lua] [--region auto|ntsc|ntsc-j|pal]
//                      [--timeout SECONDS] [--allow-dirty]
//   mesen-headless validate OUT_DIR
//   mesen-headless doctor
//
// Contract (manifest schema "mesen-headless/1"), see docs/headless.md:
//   - the run emulates exactly N frames from power-on, unless a Lua script calls emu.stop()
//     (completion "script-stop", the frame in progress completes) or the wall-clock timeout
//     expires (completion "timeout": the manifest says incomplete and the exit code is 3);
//   - input.json: {"frames": {"<1-based frame>": {"<port 1-based>": ["A","START", ...]}}};
//     a frame's entry holds until the next entry ("hold" semantics), an empty list releases;
//   - artifacts: final.png (unfiltered frame buffer), audio.wav, final.mss (save state),
//     memory/<MemoryType>.bin, manifest.json; every artifact carries its SHA-256 and size;
//   - a build from a dirty tree is refused for qualified captures unless --allow-dirty is
//     given, in which case the manifest records "qualified": false.

#include "pch.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <nlohmann/json.hpp>
#include <thread>
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/MessageManager.h"
#include "Shared/SaveStateManager.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/Interfaces/IInputProvider.h"
#include "Shared/Interfaces/IConsole.h"
#include "Shared/Interfaces/INotificationListener.h"
#include "Shared/NotificationManager.h"
#include "Shared/Audio/SoundMixer.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/MemoryType.h"
#include "Debugger/Debugger.h"
#include "Debugger/ScriptManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/magic_enum.hpp"
#include "MD/MdConsole.h"
#include "MD/MdApu.h"
#include "MD/MdM68k.h"
#include "MD/MdEeprom.h"
#include "MD/Mcd/McdUnit.h"
#include "MD/M32x/M32xUnit.h"
#include "Shared/BaseControlManager.h"
#include "Utilities/Sha256.h"
#include "Utilities/CRC32.h"
#include "Debugger/DebugUtilities.h"

#ifndef MESEN_GIT_COMMIT
	#define MESEN_GIT_COMMIT "unknown"
#endif
#ifndef MESEN_GIT_DIRTY
	#define MESEN_GIT_DIRTY 1
#endif

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

static const char* SchemaVersion = "mesen-headless/1";

enum ExitCode
{
	ExitOk = 0,
	ExitUsage = 64,
	ExitFailure = 65,
	ExitIncomplete = 3,
};

static string FileSha256(const fs::path& p, uint64_t* size = nullptr)
{
	std::ifstream f(p, std::ios::binary);
	Sha256 h;
	char buf[1 << 16];
	uint64_t total = 0;
	while(f) {
		f.read(buf, sizeof(buf));
		std::streamsize n = f.gcount();
		if(n > 0) {
			h.Update(buf, (size_t)n);
			total += (uint64_t)n;
		}
	}
	if(size) {
		*size = total;
	}
	return h.HexDigest();
}

// Button names per console. Bits follow each controller's GetKeyNames() order.
static int ButtonBit(ConsoleType console, const string& rawName)
{
	string name = rawName;
	std::transform(name.begin(), name.end(), name.begin(), ::toupper);
	static const std::map<string, int> nes = { { "UP", 0 }, { "DOWN", 1 }, { "LEFT", 2 }, { "RIGHT", 3 }, { "START", 4 }, { "SELECT", 5 }, { "B", 6 }, { "A", 7 } };
	static const std::map<string, int> snes = { { "A", 0 }, { "B", 1 }, { "X", 2 }, { "Y", 3 }, { "L", 4 }, { "R", 5 }, { "SELECT", 6 }, { "START", 7 }, { "UP", 8 }, { "DOWN", 9 }, { "LEFT", 10 }, { "RIGHT", 11 } };
	static const std::map<string, int> sms = { { "UP", 0 }, { "DOWN", 1 }, { "LEFT", 2 }, { "RIGHT", 3 }, { "1", 4 }, { "2", 5 }, { "PAUSE", 6 } };
	static const std::map<string, int> pce = { { "UP", 0 }, { "DOWN", 1 }, { "LEFT", 2 }, { "RIGHT", 3 }, { "RUN", 4 }, { "SELECT", 5 }, { "I", 6 }, { "II", 7 } };
	static const std::map<string, int> md = { { "UP", 0 }, { "DOWN", 1 }, { "LEFT", 2 }, { "RIGHT", 3 }, { "START", 4 }, { "A", 5 }, { "B", 6 }, { "C", 7 }, { "X", 8 }, { "Y", 9 }, { "Z", 10 }, { "MODE", 11 } };
	const std::map<string, int>* table = nullptr;
	switch(console) {
		case ConsoleType::Nes:
		case ConsoleType::Gameboy: table = &nes; break;
		case ConsoleType::Snes: table = &snes; break;
		case ConsoleType::Sms: table = &sms; break;
		case ConsoleType::PcEngine: table = &pce; break;
		case ConsoleType::Md: table = &md; break;
		default: break;
	}
	if(table) {
		auto it = table->find(name);
		if(it != table->end()) {
			return it->second;
		}
	}
	if(name.size() > 1 && name[0] == '#') {
		return std::stoi(name.substr(1));
	}
	return -1;
}

// Feeds controller state from input.json. Frame numbers are 1-based: entry "1" applies to the
// first emulated frame. An entry holds until the next entry for the same port.
class JsonInputProvider : public IInputProvider
{
	Emulator* _emu;
	ConsoleType _console;
	// port -> (frame -> bits)
	std::map<int, std::map<uint32_t, vector<int>>> _events;

public:
	JsonInputProvider(Emulator* emu, ConsoleType console) : _emu(emu), _console(console) {}

	string Load(const json& doc)
	{
		if(!doc.is_object() || !doc.contains("frames") || !doc["frames"].is_object()) {
			return "input: expected an object with a \"frames\" object";
		}
		for(auto& [frameKey, ports] : doc["frames"].items()) {
			uint32_t frame = (uint32_t)std::stoul(frameKey);
			if(frame == 0) {
				return "input: frame numbers are 1-based";
			}
			if(!ports.is_object()) {
				return "input: frame " + frameKey + " must map ports to button lists";
			}
			for(auto& [portKey, buttons] : ports.items()) {
				int port = std::stoi(portKey);
				if(port < 1 || port > 8 || !buttons.is_array()) {
					return "input: bad port entry at frame " + frameKey;
				}
				vector<int> bits;
				for(auto& b : buttons) {
					int bit = ButtonBit(_console, b.get<string>());
					if(bit < 0) {
						return "input: unknown button \"" + b.get<string>() + "\" for this console";
					}
					bits.push_back(bit);
				}
				_events[port - 1][frame] = bits;
			}
		}
		return "";
	}

	bool SetInput(BaseControlDevice* device) override
	{
		uint8_t port = device->GetPort();
		auto pit = _events.find(port);
		if(pit == _events.end()) {
			return false;
		}
		uint32_t frame = _emu->GetFrameCount() + 1;
		auto it = pit->second.upper_bound(frame);
		if(it == pit->second.begin()) {
			return false;
		}
		--it;
		for(int bit : it->second) {
			device->SetBit((uint8_t)bit);
		}
		return true;
	}
};

static void ConfigureDeterministic(EmuSettings* settings, ConsoleRegion region)
{
	// TestMode is the CI runner's switch for otherwise-random power-on state.
	settings->SetFlag(EmulationFlags::TestMode);
	settings->SetFlag(EmulationFlags::MaximumSpeed);

	auto& nes = settings->GetNesConfig();
	nes.Region = region == ConsoleRegion::NtscJapan ? ConsoleRegion::Ntsc : region;
	nes.RamPowerOnState = RamState::AllZeros;
	nes.RandomizeCpuPpuAlignment = false;
	nes.RandomizeMapperPowerOnState = false;
	nes.EnableHdPacks = false;
	nes.DisableGameDatabase = true;
	nes.AutoConfigureInput = false;
	nes.Port1.Type = ControllerType::NesController;
	nes.Port2.Type = ControllerType::NesController;
	// The UI's default 2C02 palette (UI/Config/NesConfig.cs); headless there is no UI to push it.
	static const uint32_t palette[64] = {
		0xFF666666,
		0xFF002A88,
		0xFF1412A7,
		0xFF3B00A4,
		0xFF5C007E,
		0xFF6E0040,
		0xFF6C0600,
		0xFF561D00,
		0xFF333500,
		0xFF0B4800,
		0xFF005200,
		0xFF004F08,
		0xFF00404D,
		0xFF000000,
		0xFF000000,
		0xFF000000,
		0xFFADADAD,
		0xFF155FD9,
		0xFF4240FF,
		0xFF7527FE,
		0xFFA01ACC,
		0xFFB71E7B,
		0xFFB53120,
		0xFF994E00,
		0xFF6B6D00,
		0xFF388700,
		0xFF0C9300,
		0xFF008F32,
		0xFF007C8D,
		0xFF000000,
		0xFF000000,
		0xFF000000,
		0xFFFFFEFF,
		0xFF64B0FF,
		0xFF9290FF,
		0xFFC676FF,
		0xFFF36AFF,
		0xFFFE6ECC,
		0xFFFE8170,
		0xFFEA9E22,
		0xFFBCBE00,
		0xFF88D800,
		0xFF5CE430,
		0xFF45E082,
		0xFF48CDDE,
		0xFF4F4F4F,
		0xFF000000,
		0xFF000000,
		0xFFFFFEFF,
		0xFFC0DFFF,
		0xFFD3D2FF,
		0xFFE8C8FF,
		0xFFFBC2FF,
		0xFFFEC4EA,
		0xFFFECCC5,
		0xFFF7D8A5,
		0xFFE4E594,
		0xFFCFEF96,
		0xFFBDF4AB,
		0xFFB3F3CC,
		0xFFB5EBF2,
		0xFFB8B8B8,
		0xFF000000,
		0xFF000000,
	};
	memcpy(nes.UserPalette, palette, sizeof(palette));

	auto& snes = settings->GetSnesConfig();
	snes.Region = region == ConsoleRegion::NtscJapan ? ConsoleRegion::Ntsc : region;
	snes.RamPowerOnState = RamState::AllZeros;
	snes.Port1.Type = ControllerType::SnesController;
	snes.Port2.Type = ControllerType::SnesController;

	auto& sms = settings->GetSmsConfig();
	sms.Region = region == ConsoleRegion::NtscJapan ? ConsoleRegion::Ntsc : region;
	sms.RamPowerOnState = RamState::AllZeros;
	sms.Port1.Type = ControllerType::SmsController;
	sms.Port2.Type = ControllerType::SmsController;

	auto& pce = settings->GetPcEngineConfig();
	pce.RamPowerOnState = RamState::AllZeros;
	pce.Port1.Type = ControllerType::PceController;

	settings->GetGameboyConfig().RamPowerOnState = RamState::AllZeros;

	auto& md = settings->GetMdConfig();
	md.Region = region;
	md.RamPowerOnState = RamState::AllZeros;
	md.Port1.Type = ControllerType::MdController6Buttons;
	md.Port2.Type = ControllerType::MdController6Buttons;

	auto& prefs = settings->GetPreferences();
	prefs.DisableGameSelectionScreen = true;
	prefs.DisableOsd = true;
	prefs.AutoSaveStateDelay = 0;
	prefs.RewindBufferSize = 0;

	auto& debug = settings->GetDebugConfig();
	debug.ScriptAllowIoOsAccess = false;
	debug.ScriptAllowNetworkAccess = false;
	debug.ScriptTimeout = 5;

	settings->GetAudioConfig().DisableDynamicSampleRate = true;
}

// The console's current frame buffer through the default filter, as PNG (no renderer involved)
static string FramePng(Emulator& emu)
{
	PpuFrameInfo ppu = emu.GetPpuFrame();
	unique_ptr<BaseVideoFilter> filter(emu.GetVideoFilter(true));
	filter->SetBaseFrameInfo({ ppu.Width, ppu.Height });
	filter->SendFrame((uint16_t*)ppu.FrameBuffer, ppu.FrameCount, 0, nullptr);
	std::stringstream png;
	filter->TakeScreenshot(VideoFilterType::None, "", &png);
	return png.str();
}

// --snap-every N: snaps/<frame>.png every N frames, for looking at a run without replaying it
class SnapListener final : public INotificationListener
{
	Emulator& _emu;
	fs::path _dir;
	uint32_t _every;

public:
	SnapListener(Emulator& emu, fs::path dir, uint32_t every) : _emu(emu), _dir(dir), _every(every) {}

	void ProcessNotification(ConsoleNotificationType type, void*) override
	{
		if(type != ConsoleNotificationType::PpuFrameDone) {
			return;
		}
		uint32_t frame = _emu.GetFrameCount();
		if(frame > 0 && frame % _every == 0) {
			char name[32];
			snprintf(name, sizeof(name), "%06u.png", frame);
			std::ofstream(_dir / name, std::ios::binary) << FramePng(_emu);
		}
	}
};

// --state-log FILE: one line per frame with CRC32s of every RAM-like memory and of the frame buffer,
// for comparing runs frame by frame (tools/md-corpus); --dump-at F1,F2,...: full copies of those
// memories at the end of the listed frames, in dumps/<frame>/<MemoryType>.bin.
// Format mesen-state-log/1: a "# mesen-state-log/1" line, a "#" header naming the columns, then
// "frame clock polled pc subpc <crc per region...> frame" in hex. polled = the game read its pads
// during the frame; pc/subpc = 68000 (and Sega CD sub 68000) PC at the end of the frame.
// --state-blocks (with --state-log, Mega Drive): FILE.blocks, binary, per frame the low 16 bits of
// the CRC32 of each 256-byte block of 68000 work RAM (256 little-endian uint16), so a comparison can
// tell which part of RAM differs and ignore blocks that only differ by timing (the stack).
static vector<MemoryType> StateLogRegions(Emulator& emu)
{
	vector<MemoryType> regions;
	for(auto type : magic_enum::enum_values<MemoryType>()) {
		string name(magic_enum::enum_name(type));
		if(name.find("Rom") != string::npos || name.find("Register") != string::npos || name.find("Bios") != string::npos || name == "None") {
			continue;
		}
		//CPU address spaces (MdMemory, McdMemory, ...) are views, not storage
		if(DebugUtilities::IsRelativeMemory(type)) {
			continue;
		}
		ConsoleMemoryInfo mem = emu.GetMemory(type);
		if(mem.Memory && mem.Size > 0) {
			regions.push_back(type);
		}
	}
	return regions;
}

class StateLogListener final : public INotificationListener
{
	Emulator& _emu;
	std::ofstream _log;
	vector<MemoryType> _regions;
	std::set<uint32_t> _dumpAt;
	fs::path _dumpDir;
	std::ofstream _blocks;

public:
	StateLogListener(Emulator& emu, const fs::path& logPath, bool blocks, std::set<uint32_t> dumpAt, const fs::path& dumpDir)
		: _emu(emu), _regions(StateLogRegions(emu)), _dumpAt(std::move(dumpAt)), _dumpDir(dumpDir)
	{
		if(blocks && !logPath.empty()) {
			_blocks.open(fs::path(logPath.string() + ".blocks"), std::ios::binary);
		}
		if(!logPath.empty()) {
			_log.open(logPath);
			_log << "# mesen-state-log/1\n# frame clock polled pc subpc";
			for(MemoryType type : _regions) {
				_log << " " << magic_enum::enum_name(type);
			}
			_log << " frame\n";
		}
	}

	void ProcessNotification(ConsoleNotificationType type, void*) override
	{
		if(type != ConsoleNotificationType::PpuFrameDone) {
			return;
		}
		uint32_t frame = _emu.GetFrameCount();
		if(_log.is_open()) {
			BaseControlManager* control = _emu.GetConsole()->GetControlManager();
			uint32_t pc = 0, subPc = 0;
			if(MdConsole* md = dynamic_cast<MdConsole*>(_emu.GetConsole().get())) {
				pc = md->GetM68k()->GetState().PC;
				if(md->GetMcd()) {
					subPc = md->GetMcd()->GetCpu().GetState().PC;
				}
			}
			char buf[64];
			snprintf(buf, sizeof(buf), "%u %llx %d %x %x", frame, (unsigned long long)_emu.GetMasterClock(), control && control->WasInputRead() ? 1 : 0, pc, subPc);
			_log << buf;
			for(MemoryType type : _regions) {
				ConsoleMemoryInfo mem = _emu.GetMemory(type);
				snprintf(buf, sizeof(buf), " %08x", CRC32::GetCRC((uint8_t*)mem.Memory, mem.Size));
				_log << buf;
			}
			PpuFrameInfo ppu = _emu.GetPpuFrame();
			snprintf(buf, sizeof(buf), " %08x\n", CRC32::GetCRC(ppu.FrameBuffer, ppu.FrameBufferSize));
			_log << buf;
		}
		if(_blocks.is_open()) {
			ConsoleMemoryInfo wram = _emu.GetMemory(MemoryType::MdWorkRam);
			uint8_t out[512] = {};
			for(uint32_t i = 0; i < 256 && (i + 1) * 256 <= wram.Size; i++) {
				uint16_t crc = (uint16_t)CRC32::GetCRC((uint8_t*)wram.Memory + i * 256, 256);
				out[i * 2] = crc & 0xFF;
				out[i * 2 + 1] = crc >> 8;
			}
			_blocks.write((const char*)out, sizeof(out));
		}
		if(_dumpAt.count(frame)) {
			fs::path dir = _dumpDir / std::to_string(frame);
			fs::create_directories(dir);
			for(MemoryType type : _regions) {
				ConsoleMemoryInfo mem = _emu.GetMemory(type);
				std::ofstream f(dir / (string(magic_enum::enum_name(type)) + ".bin"), std::ios::binary);
				f.write((const char*)mem.Memory, mem.Size);
			}
		}
	}

	void Close()
	{
		_log.close();
		_blocks.close();
	}
};

static bool ParseRegion(const string& s, ConsoleRegion& out)
{
	if(s == "auto") {
		out = ConsoleRegion::Auto;
	} else if(s == "ntsc" || s == "ntsc-u") {
		out = ConsoleRegion::Ntsc;
	} else if(s == "ntsc-j") {
		out = ConsoleRegion::NtscJapan;
	} else if(s == "pal") {
		out = ConsoleRegion::Pal;
	} else {
		return false;
	}
	return true;
}

static int Usage()
{
	std::cerr << "usage:\n"
					 "  mesen-headless run --rom ROM --frames N --out NEW_DIR [--input INPUT.json] [--lua SCRIPT.lua]\n"
					 "                     [--region auto|ntsc|ntsc-j|pal] [--timeout SECONDS] [--allow-dirty]\n"
					 "                     [--snap-every N] [--state-log FILE [--state-blocks]] [--dump-at F1,F2,...]\n"
					 "  mesen-headless validate OUT_DIR\n"
					 "  mesen-headless doctor\n";
	return ExitUsage;
}

static int Run(const vector<string>& args, const string& commandLine)
{
	string rom, out, inputPath, luaPath, traceProfile, regionName = "auto";
	uint64_t traceLimit = 5000000;
	string mdModel = "md1va3";
	string mdInputLatch = "frame";
	string loadStatePath;
	bool chipAudio = false;
	string mdPads = "6,6";
	string mdFmCore = "nuked"; //the MdConfig default (findings C15)
	string mdEeprom;
	uint32_t frames = 0;
	uint32_t snapEvery = 0;
	double timeout = 0;
	bool allowDirty = false;
	bool mdMcd = false;
	bool md32x = false;
	string md32xBios;
	vector<string> batteryPaths;
	string mdMcdDisc, mdMcdBios;
	bool saveBattery = false;
	string stateLogPath;
	bool stateBlocks = false;
	std::set<uint32_t> dumpAt;
	for(size_t i = 0; i < args.size(); i++) {
		auto next = [&]() -> string {
			if(i + 1 >= args.size()) {
				throw std::runtime_error("missing value for " + args[i]);
			}
			return args[++i];
		};
		if(args[i] == "--rom") {
			rom = next();
		} else if(args[i] == "--frames") {
			frames = (uint32_t)std::stoul(next());
		} else if(args[i] == "--out") {
			out = next();
		} else if(args[i] == "--input") {
			inputPath = next();
		} else if(args[i] == "--lua") {
			luaPath = next();
		} else if(args[i] == "--region") {
			regionName = next();
		} else if(args[i] == "--timeout") {
			timeout = std::stod(next());
		} else if(args[i] == "--trace") {
			traceProfile = next();
		} else if(args[i] == "--chip-audio") {
			chipAudio = true;
		} else if(args[i] == "--load-state") {
			loadStatePath = next();
		} else if(args[i] == "--md-model") {
			mdModel = next();
		} else if(args[i] == "--md-pads") {
			mdPads = next();
		} else if(args[i] == "--md-fm-core") {
			mdFmCore = next();
		} else if(args[i] == "--md-eeprom") {
			mdEeprom = next();
		} else if(args[i] == "--md-mcd") {
			mdMcd = true;
		} else if(args[i] == "--md-mcd-disc") {
			mdMcdDisc = next();
		} else if(args[i] == "--md-mcd-bios") {
			mdMcdBios = next();
		} else if(args[i] == "--md-input-latch") {
			mdInputLatch = next();
		} else if(args[i] == "--md-32x") {
			md32x = true;
		} else if(args[i] == "--md-32x-bios") {
			md32xBios = next();
		} else if(args[i] == "--snap-every") {
			snapEvery = (uint32_t)std::stoul(next());
		} else if(args[i] == "--battery") {
			batteryPaths.push_back(next());
		} else if(args[i] == "--save-battery") {
			saveBattery = true;
		} else if(args[i] == "--trace-limit") {
			traceLimit = std::stoull(next());
		} else if(args[i] == "--state-log") {
			stateLogPath = next();
		} else if(args[i] == "--state-blocks") {
			stateBlocks = true;
		} else if(args[i] == "--dump-at") {
			std::stringstream list(next());
			string item;
			while(std::getline(list, item, ',')) {
				dumpAt.insert((uint32_t)std::stoul(item));
			}
		} else if(args[i] == "--allow-dirty") {
			allowDirty = true;
		} else {
			std::cerr << "unknown option " << args[i] << "\n";
			return Usage();
		}
	}
	ConsoleRegion region;
	if(rom.empty() || out.empty() || frames == 0 || !ParseRegion(regionName, region) || (!traceProfile.empty() && traceProfile != "bus")) {
		return Usage();
	}
	if(MESEN_GIT_DIRTY && !allowDirty) {
		std::cerr << "refusing a qualified capture from a dirty or unknown build (commit " << MESEN_GIT_COMMIT << "); pass --allow-dirty for an unqualified run\n";
		return ExitFailure;
	}
	fs::path outDir = fs::absolute(out);
	if(fs::exists(outDir) && !fs::is_empty(outDir)) {
		std::cerr << "output directory must be new or empty: " << outDir << "\n";
		return ExitUsage;
	}
	fs::create_directories(outDir / "memory");
	fs::path home = outDir / ".home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());

	json manifest;
	manifest["schema"] = SchemaVersion;
	manifest["emulator"] = { { "name", "MesenCE" }, { "commit", MESEN_GIT_COMMIT }, { "dirty", (bool)MESEN_GIT_DIRTY } };
	manifest["qualified"] = !MESEN_GIT_DIRTY;
	manifest["command"] = commandLine;

	uint64_t romSize = 0;
	manifest["rom"] = { { "sha256", FileSha256(rom, &romSize) }, { "size", romSize }, { "name", fs::path(rom).filename().string() } };

	json inputDoc;
	if(!inputPath.empty()) {
		std::ifstream f(inputPath);
		if(!f) {
			std::cerr << "cannot read " << inputPath << "\n";
			return ExitUsage;
		}
		try {
			f >> inputDoc;
		} catch(std::exception& e) {
			std::cerr << "input: " << e.what() << "\n";
			return ExitUsage;
		}
		manifest["input"] = { { "sha256", FileSha256(inputPath) } };
	}
	string luaCode;
	if(!luaPath.empty()) {
		std::ifstream f(luaPath, std::ios::binary);
		if(!f) {
			std::cerr << "cannot read " << luaPath << "\n";
			return ExitUsage;
		}
		luaCode.assign((std::istreambuf_iterator<char>(f)), {});
		manifest["lua"] = { { "sha256", Sha256::Of(luaCode) } };
	}

	int exitCode = ExitOk;
	{
		Emulator emu;
		ConfigureDeterministic(emu.GetSettings(), region);
		{
			MdConfig& md = emu.GetSettings()->GetMdConfig();
			if(mdModel != "md1va3" && mdModel != "md1va6") {
				throw std::runtime_error("--md-model must be md1va3 or md1va6");
			}
			md.Model = mdModel == "md1va6" ? MdModel::Md1Va6 : MdModel::Md1Va3;
			if(mdFmCore != "ymfm" && mdFmCore != "nuked") {
				throw std::runtime_error("--md-fm-core must be ymfm or nuked");
			}
			md.FmCore = mdFmCore == "nuked" ? MdFmCore::Nuked : MdFmCore::Ymfm;
			//Each port: 3 or 6 (pads), n (nothing), m (Sega Mouse), g (Menacer), j (Justifier), or
			//t:ABCD (Team Player with four of 3/6/n on its ports A-D)
			size_t comma = mdPads.find(',');
			if(comma == string::npos) {
				throw std::runtime_error("--md-pads takes two port devices, e.g. 6,3 or t:63n6,m");
			}
			auto parsePort = [](const string& spec, ControllerConfig& port, ControllerConfig* subPorts) {
				auto padType = [](char c) {
					switch(c) {
						case '3': return ControllerType::MdController3Buttons;
						case '6': return ControllerType::MdController6Buttons;
						case 'n': return ControllerType::None;
						default: throw std::runtime_error("--md-pads: unknown pad type");
					}
				};
				if(spec.size() == 6 && spec.compare(0, 2, "t:") == 0) {
					port.Type = ControllerType::MdTeamPlayer;
					for(int i = 0; i < 4; i++) {
						subPorts[i].Type = padType(spec[2 + i]);
					}
					return;
				}
				if(spec.size() != 1) {
					throw std::runtime_error("--md-pads: bad port device " + spec);
				}
				switch(spec[0]) {
					case 'm': port.Type = ControllerType::MdMouse; break;
					case 'g': port.Type = ControllerType::MdMenacer; break;
					case 'j': port.Type = ControllerType::MdJustifier; break;
					default: port.Type = padType(spec[0]); break;
				}
			};
			parsePort(mdPads.substr(0, comma), md.Port1, md.Port1SubPorts);
			parsePort(mdPads.substr(comma + 1), md.Port2, md.Port2SubPorts);
		}
		//Test hook: force an EEPROM chip and board (the database is keyed by commercial ROM hashes)
		MdEepromInfo eepromInfo = {};
		if(!mdEeprom.empty()) {
			size_t comma = mdEeprom.find(',');
			auto chip = magic_enum::enum_cast<MdEepromChip>(mdEeprom.substr(0, comma));
			auto board = comma == string::npos ? std::nullopt : magic_enum::enum_cast<MdEepromBoard>(mdEeprom.substr(comma + 1));
			if(!chip || !board || *chip == MdEepromChip::None || *board == MdEepromBoard::None) {
				throw std::runtime_error("--md-eeprom takes CHIP,BOARD, e.g. X24C01,Sega");
			}
			eepromInfo = { *chip, *board };
		}
		MdEepromDatabase::SetOverride(eepromInfo);
		//Test hook: attach the Sega CD unit (Mode 1; no BIOS, discs or UI setting yet)
		McdUnit::SetAttachedOverride(mdMcd);
		McdUnit::SetDiscOverride(mdMcdDisc.empty() ? string() : fs::absolute(mdMcdDisc).string());
		//Test hooks: attach the 32X to any cartridge; --md-32x-bios DIR: Sega's three boot ROMs
		//(32X_M_BIOS.bin, 32X_S_BIOS.bin, 32X_G_BIOS.bin) instead of MesenCE's replacements
		M32xUnit::SetAttachedOverride(md32x);
		//Test hook: when the pads take each frame's input (frame: the wrap to line 0; vblank: the
		//first VBlank line, as Genesis Plus GX and BizHawk's Genplus-gx core do)
		if(mdInputLatch != "frame" && mdInputLatch != "vblank") {
			throw std::runtime_error("--md-input-latch must be frame or vblank");
		}
		MdConsole::SetVBlankInputLatchOverride(mdInputLatch == "vblank");
		M32xUnit::SetBiosFolderOverride(md32xBios.empty() ? string() : fs::absolute(md32xBios).string());
		MessageManager::SetOptions(false, false);
		emu.Initialize(false);
		//--md-mcd-bios FILE: the Sega CD BIOS for disc boot, under every region's file name
		if(!mdMcdBios.empty()) {
			fs::path firmware = FolderUtilities::GetFirmwareFolder();
			fs::create_directories(firmware);
			for(const char* name : { "bios_CD_U.bin", "bios_CD_E.bin", "bios_CD_J.bin" }) {
				fs::copy_file(mdMcdBios, firmware / name, fs::copy_options::overwrite_existing);
			}
		}
		//--battery FILE: start from this save file (it becomes <rom name><FILE's extension>)
		for(const string& battery : batteryPaths) {
			fs::path saves = FolderUtilities::GetSaveFolder();
			fs::create_directories(saves);
			fs::copy_file(battery, saves / (FolderUtilities::GetFilename(rom, false) + fs::path(battery).extension().string()), fs::copy_options::overwrite_existing);
		}
		if(!emu.LoadRom(VirtualFile(fs::absolute(rom).string()), VirtualFile(), false)) {
			std::cerr << "could not load " << rom << "\n"
						 << MessageManager::GetLog();
			emu.Release();
			return ExitFailure;
		}
		// Loading resets the rewind manager, which clears MaximumSpeed.
		emu.GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);
		emu.GetSettings()->GetEmulationConfig().EmulationSpeed = 0;
		ConsoleType console = emu.GetConsoleType();
		manifest["console"] = string(magic_enum::enum_name(console));
		manifest["region"] = { { "requested", regionName }, { "effective", string(magic_enum::enum_name(emu.GetRegion())) } };
		manifest["settings"] = {
			{ "power_on", "TestMode, RAM all zeros" },
			{ "controllers", "standard pads on ports 1-2" },
			{ "md_model", mdModel },
			{ "md_input_latch", mdInputLatch },
			{ "md_mcd", mdMcd },
			{ "md_32x", md32x },
			{ "md_32x_bios", md32xBios.empty() ? "replacement" : "user" },
			{ "md_pads", mdPads },
			{ "md_fm_core", mdFmCore },
			{ "md_eeprom", mdEeprom.empty() ? "database" : mdEeprom },
			{ "audio_sample_rate", emu.GetSettings()->GetAudioConfig().SampleRate },
			{ "dynamic_sample_rate", false }
		};

		JsonInputProvider input(&emu, console);
		if(!inputDoc.is_null()) {
			string err = input.Load(inputDoc);
			if(!err.empty()) {
				std::cerr << err << "\n";
				emu.Release();
				return ExitUsage;
			}
		}
		if(!loadStatePath.empty()) {
			//Resume from a save state (checkpoint replay): --frames counts frames after the state
			std::ifstream stateFile(loadStatePath, std::ios::binary);
			if(!stateFile || !emu.GetSaveStateManager()->LoadState(stateFile)) {
				std::cerr << "could not load state " << loadStatePath << "\n";
				emu.Release();
				return ExitFailure;
			}
			manifest["load_state"] = { { "sha256", FileSha256(loadStatePath) }, { "frame", emu.GetFrameCount() } };
			frames += emu.GetFrameCount();
		}
		//Headless mode first: a Lua error while the script loads must fail the run too (stop code 65).
		//The script is loaded before the input refresh below so that its inputPolled callbacks take
		//part in it: after --load-state, a refresh without them replaced the input the script had set
		//in the saved run with no input (io_menacer_hl diverged on replay).
		emu.SetHeadlessMode(frames);
		int scriptId = -1;
		if(!luaCode.empty()) {
			emu.InitDebugger();
			scriptId = emu.InternalGetDebugger()->GetScriptManager()->LoadScript(fs::path(luaPath).filename().string(), fs::absolute(luaPath).parent_path().string() + "/", luaCode, -1);
		}

		emu.RegisterInputProvider(&input);
		//Apply the first frame's input before the first frame runs (frame 1 is 1-based)
		if(BaseControlManager* controlManager = emu.GetConsole()->GetControlManager()) {
			controlManager->UpdateInputState();
		}

		//Trace profile "bus" (Mega Drive): every CPU bus access plus VDP, YM2612 and PSG port events, one
		//text line each: "<master clock> <device> <op> <width> <address> <value>" (hex fields).
		std::ofstream traceFile;
		uint64_t traceEvents = 0;
		bool traceTruncated = false;
		if(!traceProfile.empty()) {
			MdConsole* md = dynamic_cast<MdConsole*>(emu.GetConsole().get());
			if(!md) {
				std::cerr << "--trace bus is only implemented for the Mega Drive\n";
				emu.Release();
				return ExitUsage;
			}
			traceFile.open(outDir / "trace.txt");
			traceFile << "# mesen-md-bus-trace/1 clock device(1=68k 2=z80 3=vdp 4=ym2612 5=psg) op(1=fetch 2=read 3=write) width address value\n";
			md->SetBusObserver([&](const MdBusEvent& e) {
				if(traceEvents >= traceLimit) {
					traceTruncated = true;
					return;
				}
				traceEvents++;
				char line[96];
				snprintf(line, sizeof(line), "%llx %d %d %d %x %x\n", (unsigned long long)e.Clock, (int)e.Device, (int)e.Op, (int)e.Width, e.Address, e.Value);
				traceFile << line;
			});
		}

		//--chip-audio (Mega Drive): YM2612 and PSG streams before mixing, s16 little-endian stereo at the
		//YM2612 sample rate (master clock / 1008)
		std::ofstream ymFile, psgFile, pcmFile, cddaFile, pwmFile;
		if(chipAudio) {
			MdConsole* md = dynamic_cast<MdConsole*>(emu.GetConsole().get());
			if(!md) {
				std::cerr << "--chip-audio is only implemented for the Mega Drive\n";
				emu.Release();
				return ExitUsage;
			}
			ymFile.open(outDir / "ym2612.raw", std::ios::binary);
			psgFile.open(outDir / "psg.raw", std::ios::binary);
			md->GetApu()->SetChipObserver([&](int32_t yl, int32_t yr, int32_t pl, int32_t pr) {
				int16_t ym[2] = { (int16_t)std::clamp(yl, -32768, 32767), (int16_t)std::clamp(yr, -32768, 32767) };
				int16_t psg[2] = { (int16_t)std::clamp(pl, -32768, 32767), (int16_t)std::clamp(pr, -32768, 32767) };
				ymFile.write((const char*)ym, sizeof(ym));
				psgFile.write((const char*)psg, sizeof(psg));
			});
			manifest["chip_audio"] = { { "format", "s16le-stereo" }, { "sample_rate", md->GetApu()->GetSampleRate() }, { "files", { "ym2612.raw", "psg.raw" } } };
			if(md->GetM32x()) {
				//32X PWM as mixed (after the PWM level conversion), one sample per YM2612 sample
				pwmFile.open(outDir / "pwm.raw", std::ios::binary);
				md->GetApu()->SetPwmObserver([&](int32_t l, int32_t r) {
					int16_t pwm[2] = { (int16_t)std::clamp(l, -32768, 32767), (int16_t)std::clamp(r, -32768, 32767) };
					pwmFile.write((const char*)pwm, sizeof(pwm));
				});
				manifest["chip_audio"]["pwm"] = { { "file", "pwm.raw" }, { "format", "s16le-stereo" }, { "sample_rate", md->GetApu()->GetSampleRate() } };
			}
			if(md->GetMcd()) {
				//Sega CD PCM: its own stream at 12.5 MHz / 384 (one sample per McdPcm::Clock)
				pcmFile.open(outDir / "pcm.raw", std::ios::binary);
				md->GetMcd()->GetPcm().SetObserver([&](int16_t l, int16_t r) {
					int16_t pcm[2] = { l, r };
					pcmFile.write((const char*)pcm, sizeof(pcm));
				});
				manifest["chip_audio"]["pcm"] = { { "file", "pcm.raw" }, { "format", "s16le-stereo" }, { "sample_rate_hz", 12500000.0 / 384 } };
				//CD audio after the fader, at 44.1 kHz
				cddaFile.open(outDir / "cdda.raw", std::ios::binary);
				md->GetMcd()->GetCdd().SetObserver([&](int16_t l, int16_t r) {
					int16_t cd[2] = { l, r };
					cddaFile.write((const char*)cd, sizeof(cd));
				});
				manifest["chip_audio"]["cdda"] = { { "file", "cdda.raw" }, { "format", "s16le-stereo" }, { "sample_rate_hz", 44100 } };
			}
		}

		std::atomic<bool> finished = false;
		std::atomic<bool> timedOut = false;
		std::thread watchdog;
		if(timeout > 0) {
			watchdog = std::thread([&]() {
				auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
				while(!finished) {
					if(std::chrono::steady_clock::now() >= deadline) {
						timedOut = true;
						emu.SetStopCode(0);
						return;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
			});
		}

		shared_ptr<SnapListener> snaps;
		if(snapEvery > 0) {
			fs::create_directories(outDir / "snaps");
			snaps.reset(new SnapListener(emu, outDir / "snaps", snapEvery));
			emu.GetNotificationManager()->RegisterNotificationListener(snaps);
		}

		shared_ptr<StateLogListener> stateLog;
		if(!stateLogPath.empty() || !dumpAt.empty()) {
			//The log goes inside the output directory (listed and hashed like every artifact)
			fs::path logPath = stateLogPath.empty() ? fs::path() : outDir / fs::path(stateLogPath).filename();
			stateLog.reset(new StateLogListener(emu, logPath, stateBlocks, dumpAt, outDir / "dumps"));
			emu.GetNotificationManager()->RegisterNotificationListener(stateLog);
			if(!stateLogPath.empty()) {
				manifest["state_log"] = { { "file", logPath.filename().string() }, { "format", "mesen-state-log/1" }, { "blocks", stateBlocks } };
			}
		}

		fs::path wav = outDir / "audio.wav";
		emu.GetSoundMixer()->StartRecording(wav.string());
		auto start = std::chrono::steady_clock::now();
		emu.Run();
		double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		emu.GetSoundMixer()->StopRecording();
		finished = true;
		if(watchdog.joinable()) {
			watchdog.join();
		}
		emu.UnregisterInputProvider(&input);

		uint32_t frameCount = emu.GetFrameCount();
		string completion = "frame-limit";
		bool complete = true;
		if(timedOut) {
			completion = "timeout";
			complete = false;
			exitCode = ExitIncomplete;
		} else if(emu.HeadlessStopRequested()) {
			completion = "script-stop";
			if(emu.GetStopCode() != 0) {
				// emu.stop(code) with a nonzero code, or a Lua error (65).
				completion = "script-failure";
				exitCode = ExitFailure;
			}
		}
		manifest["completion"] = {
			{ "reason", completion },
			{ "complete", complete },
			{ "frames", frameCount },
			{ "frames_requested", frames },
			{ "master_clock", emu.GetMasterClock() },
			{ "stop_code", emu.GetStopCode() },
			{ "wall_seconds", elapsed }
		};
		if(chipAudio) {
			((MdConsole*)emu.GetConsole().get())->GetApu()->SetChipObserver(nullptr);
			((MdConsole*)emu.GetConsole().get())->GetApu()->SetPwmObserver(nullptr);
			ymFile.close();
			pwmFile.close();
			psgFile.close();
		}
		if(!traceProfile.empty()) {
			((MdConsole*)emu.GetConsole().get())->SetBusObserver(nullptr);
			traceFile.close();
			manifest["trace"] = { { "profile", traceProfile }, { "format", "mesen-md-bus-trace/1" }, { "events", traceEvents }, { "limit", traceLimit }, { "truncated", traceTruncated } };
			if(traceTruncated) {
				//A truncated trace is not a qualified capture
				manifest["qualified"] = false;
			}
		}
		if(stateLog) {
			stateLog->Close();
		}
		if(scriptId >= 0) {
			string log = emu.InternalGetDebugger()->GetScriptManager()->GetScriptLog(scriptId);
			std::ofstream(outDir / "lua.log") << log;
		}

		// Screenshot from the core's frame buffer (no renderer or display involved).
		std::ofstream(outDir / "final.png", std::ios::binary) << FramePng(emu);

		{
			std::ofstream state(outDir / "final.mss", std::ios::binary);
			emu.GetSaveStateManager()->SaveState(state);
		}

		for(auto type : magic_enum::enum_values<MemoryType>()) {
			string name(magic_enum::enum_name(type));
			if(name.find("Rom") != string::npos || name.find("Register") != string::npos || name == "None") {
				continue;
			}
			ConsoleMemoryInfo mem = emu.GetMemory(type);
			if(!mem.Memory || mem.Size == 0) {
				continue;
			}
			std::ofstream f(outDir / "memory" / (name + ".bin"), std::ios::binary);
			f.write((const char*)mem.Memory, mem.Size);
		}

		//--save-battery: write the battery files on exit and keep them in OUT/saves
		emu.Stop(false, true, saveBattery);
		emu.Release();
		if(saveBattery && fs::exists(FolderUtilities::GetSaveFolder())) {
			fs::create_directories(outDir / "saves");
			for(auto& e : fs::directory_iterator(FolderUtilities::GetSaveFolder())) {
				if(e.is_regular_file()) {
					fs::copy_file(e.path(), outDir / "saves" / e.path().filename(), fs::copy_options::overwrite_existing);
				}
			}
		}
	}

	fs::remove_all(home);

	json artifacts = json::array();
	vector<fs::path> files;
	for(auto& e : fs::recursive_directory_iterator(outDir)) {
		if(e.is_regular_file() && e.path().filename() != "manifest.json") {
			files.push_back(e.path());
		}
	}
	std::sort(files.begin(), files.end());
	for(auto& p : files) {
		uint64_t size = 0;
		string hash = FileSha256(p, &size);
		artifacts.push_back({ { "path", fs::relative(p, outDir).generic_string() }, { "sha256", hash }, { "size", size } });
	}
	manifest["artifacts"] = artifacts;
	manifest["limits"] = { { "frames", frames }, { "timeout_seconds", timeout }, { "truncated", exitCode == ExitIncomplete } };
	std::ofstream(outDir / "manifest.json") << manifest.dump(2) << "\n";
	return exitCode;
}

static int Validate(const string& dir)
{
	fs::path outDir = fs::absolute(dir);
	std::ifstream f(outDir / "manifest.json");
	if(!f) {
		std::cerr << "no manifest.json in " << outDir << "\n";
		return ExitFailure;
	}
	json m;
	try {
		f >> m;
	} catch(std::exception& e) {
		std::cerr << "manifest: " << e.what() << "\n";
		return ExitFailure;
	}
	int errors = 0;
	auto fail = [&](const string& msg) {
		std::cerr << "invalid: " << msg << "\n";
		errors++;
	};
	if(m.value("schema", "") != SchemaVersion) {
		fail("schema is not " + string(SchemaVersion));
	}
	for(const char* key : { "emulator", "rom", "console", "region", "completion", "artifacts", "limits" }) {
		if(!m.contains(key)) {
			fail(string("missing ") + key);
		}
	}
	if(errors) {
		return ExitFailure;
	}
	if(!m["completion"].value("complete", false)) {
		fail("run is incomplete (" + m["completion"].value("reason", string("?")) + ")");
	}
	std::set<string> listed;
	for(auto& a : m["artifacts"]) {
		string path = a["path"];
		listed.insert(path);
		uint64_t size = 0;
		if(!fs::exists(outDir / path)) {
			fail("missing artifact " + path);
			continue;
		}
		string hash = FileSha256(outDir / path, &size);
		if(hash != a["sha256"].get<string>() || size != a["size"].get<uint64_t>()) {
			fail("artifact changed: " + path);
		}
	}
	for(const char* required : { "final.png", "final.mss", "audio.wav" }) {
		if(!listed.count(required)) {
			fail(string("missing required artifact ") + required);
		}
	}
	for(auto& e : fs::recursive_directory_iterator(outDir)) {
		if(e.is_regular_file()) {
			string rel = fs::relative(e.path(), outDir).generic_string();
			if(rel != "manifest.json" && !listed.count(rel)) {
				fail("unlisted file " + rel);
			}
		}
	}
	if(!m.value("qualified", false)) {
		std::cerr << "note: unqualified capture (dirty or unknown build)\n";
	}
	if(errors) {
		return ExitFailure;
	}
	std::cout << "valid: " << outDir.string() << " (" << m["console"].get<string>() << ", " << m["completion"]["frames"] << " frames)\n";
	return ExitOk;
}

// nativekit NES capture host contract (consoles/nes/tools/capture/mesen_host.cpp):
//   mesen-headless nnes-compat ROM HOME LUA RESULT WAV
// The collector must call emu.stop(code); artifacts are written next to WAV and RESULT gets
// {"version":1,"stop_code","frames","master_clock"}. Difference from nativekit's patched
// host: a stop requested mid-frame takes effect at the end of that frame.
static int NnesCompat(const vector<string>& a)
{
	if(a.size() != 5) {
		std::cerr << "usage: mesen-headless nnes-compat ROM HOME LUA RESULT WAV\n";
		return ExitUsage;
	}
	FolderUtilities::SetHomeFolder(a[1]);
	std::ifstream luaFile(a[2], std::ios::binary);
	if(!luaFile) {
		std::cerr << "cannot read Lua script\n";
		return ExitFailure;
	}
	string code((std::istreambuf_iterator<char>(luaFile)), {});
	fs::path artifacts = fs::absolute(a[4]).parent_path();
	int result = 0;
	uint32_t frames = 0;
	uint64_t clock = 0;
	{
		Emulator emu;
		ConfigureDeterministic(emu.GetSettings(), ConsoleRegion::Ntsc);
		emu.GetSettings()->GetDebugConfig().ScriptAllowIoOsAccess = true;
		MessageManager::SetOptions(false, false);
		emu.Initialize(false);
		if(!emu.LoadRom(VirtualFile(fs::absolute(a[0]).string()), VirtualFile(), false) || emu.GetConsoleType() != ConsoleType::Nes) {
			std::cerr << "could not load NES image\n"
						 << MessageManager::GetLog();
			emu.Release();
			return ExitFailure;
		}
		emu.GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);
		emu.InitDebugger();
		//Headless mode before loading, so a stop or error during load is kept (not reset afterwards)
		emu.SetHeadlessMode(0);
		auto scripts = emu.InternalGetDebugger()->GetScriptManager();
		int id = scripts->LoadScript("nnes-capture.lua", fs::absolute(a[2]).parent_path().string() + "/", code, -1);
		if(!emu.HeadlessStopRequested()) {
			emu.GetSoundMixer()->StartRecording(a[4]);
			emu.Run();
			emu.GetSoundMixer()->StopRecording();
		}
		result = emu.GetStopCode();
		frames = emu.GetFrameCount();
		clock = emu.GetMasterClock();
		if(result == 0) {
			PpuFrameInfo ppu = emu.GetPpuFrame();
			unique_ptr<BaseVideoFilter> filter(emu.GetVideoFilter(true));
			filter->SetBaseFrameInfo({ ppu.Width, ppu.Height });
			filter->SendFrame((uint16_t*)ppu.FrameBuffer, ppu.FrameCount, 0, nullptr);
			std::stringstream png;
			filter->TakeScreenshot(VideoFilterType::None, "", &png);
			std::ofstream(artifacts / "final.png", std::ios::binary) << png.str();
			for(auto item : { std::make_pair(MemoryType::NesInternalRam, "ram.bin"), std::make_pair(MemoryType::NesSpriteRam, "oam.bin"), std::make_pair(MemoryType::NesNametableRam, "vram.bin") }) {
				ConsoleMemoryInfo mem = emu.GetMemory(item.first);
				std::ofstream(artifacts / item.second, std::ios::binary).write((const char*)mem.Memory, mem.Size);
			}
			std::ofstream state(artifacts / "final.mss", std::ios::binary);
			emu.GetSaveStateManager()->SaveState(state);
		}
		std::cerr << scripts->GetScriptLog(id) << "\n";
		emu.Stop(false, true, false);
		emu.Release();
	}
	std::ofstream(a[3]) << "{\"version\":1,\"stop_code\":" << result << ",\"frames\":" << frames << ",\"master_clock\":" << clock << "}\n";
	return result == 0 ? ExitOk : ExitFailure;
}

static int Doctor()
{
	std::cout << "mesen-headless " << SchemaVersion << "\n";
	std::cout << "commit: " << MESEN_GIT_COMMIT << (MESEN_GIT_DIRTY ? " (dirty)" : "") << "\n";
	std::cout << "audio device: none (WAV capture only); video: core frame buffer only\n";
	fs::path home = fs::temp_directory_path() / ("mesen-headless-doctor-" + std::to_string((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	{
		Emulator emu;
		emu.Initialize(false);
		std::cout << "emulator init: ok\n";
		emu.Release();
	}
	fs::remove_all(home);
	return ExitOk;
}

int main(int argc, char** argv)
{
	if(argc < 2) {
		return Usage();
	}
	string cmd = argv[1];
	vector<string> args(argv + 2, argv + argc);
	string commandLine;
	for(int i = 0; i < argc; i++) {
		commandLine += (i ? " " : "") + string(argv[i]);
	}
	try {
		if(cmd == "run") {
			return Run(args, commandLine);
		} else if(cmd == "validate" && args.size() == 1) {
			return Validate(args[0]);
		} else if(cmd == "nnes-compat") {
			return NnesCompat(args);
		} else if(cmd == "doctor") {
			return Doctor();
		}
	} catch(std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return ExitFailure;
	}
	return Usage();
}
