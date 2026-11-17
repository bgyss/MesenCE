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
#include "Shared/Audio/SoundMixer.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/MemoryType.h"
#include "Debugger/Debugger.h"
#include "Debugger/ScriptManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/magic_enum.hpp"
#include "sha256.h"

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
	[[maybe_unused]] static const std::map<string, int> md = { { "UP", 0 }, { "DOWN", 1 }, { "LEFT", 2 }, { "RIGHT", 3 }, { "START", 4 }, { "A", 5 }, { "B", 6 }, { "C", 7 }, { "X", 8 }, { "Y", 9 }, { "Z", 10 }, { "MODE", 11 } };
	const std::map<string, int>* table = nullptr;
	switch(console) {
		case ConsoleType::Nes:
		case ConsoleType::Gameboy: table = &nes; break;
		case ConsoleType::Snes: table = &snes; break;
		case ConsoleType::Sms: table = &sms; break;
		case ConsoleType::PcEngine: table = &pce; break;
		// MD_SUPPORT_CONSOLE
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
	settings->SetFlag(EmulationFlags::ConsoleMode);
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

	// MD_SUPPORT_SETTINGS

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
					 "  mesen-headless validate OUT_DIR\n"
					 "  mesen-headless doctor\n";
	return ExitUsage;
}

static int Run(const vector<string>& args, const string& commandLine)
{
	string rom, out, inputPath, luaPath, regionName = "auto";
	uint32_t frames = 0;
	double timeout = 0;
	bool allowDirty = false;
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
		} else if(args[i] == "--allow-dirty") {
			allowDirty = true;
		} else {
			std::cerr << "unknown option " << args[i] << "\n";
			return Usage();
		}
	}
	ConsoleRegion region;
	if(rom.empty() || out.empty() || frames == 0 || !ParseRegion(regionName, region)) {
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
		MessageManager::SetOptions(false, false);
		emu.Initialize(false);
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
		emu.RegisterInputProvider(&input);

		int scriptId = -1;
		if(!luaCode.empty()) {
			emu.InitDebugger();
			scriptId = emu.InternalGetDebugger()->GetScriptManager()->LoadScript(fs::path(luaPath).filename().string(), fs::absolute(luaPath).parent_path().string() + "/", luaCode, -1);
		}

		emu.SetHeadlessMode(frames);
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
		if(scriptId >= 0) {
			string log = emu.InternalGetDebugger()->GetScriptManager()->GetScriptLog(scriptId);
			std::ofstream(outDir / "lua.log") << log;
		}

		// Screenshot from the core's frame buffer (no renderer or display involved).
		PpuFrameInfo ppu = emu.GetPpuFrame();
		unique_ptr<BaseVideoFilter> filter(emu.GetVideoFilter(true));
		filter->SetBaseFrameInfo({ ppu.Width, ppu.Height });
		filter->SendFrame((uint16_t*)ppu.FrameBuffer, ppu.FrameCount, 0, nullptr);
		std::stringstream png;
		filter->TakeScreenshot(VideoFilterType::None, "", &png);
		std::ofstream(outDir / "final.png", std::ios::binary) << png.str();

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

		emu.Stop(false, true, false);
		emu.Release();
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
		auto scripts = emu.InternalGetDebugger()->GetScriptManager();
		int id = scripts->LoadScript("nnes-capture.lua", fs::absolute(a[2]).parent_path().string() + "/", code, -1);
		emu.SetHeadlessMode(0);
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
