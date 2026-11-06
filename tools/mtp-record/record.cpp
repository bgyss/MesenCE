// record.cpp - record a MesenTests-style .mtp regression test without the UI.
//
// Loads a ROM, records it from power-on (RecordedRomTest::Record with reset, the
// same call as the UI's test menu) for a fixed number of frames with no input, and
// writes the .mtp. It then replays the new file with RunRecordedTest, the function
// the CI runner uses, and fails unless it passes. So a test recorded here is known
// to be deterministic under CI conditions.
//
// Use it for self-checking test ROMs that need no input and end on a stable
// result screen. The recording only pins down MesenCE's current output: check the
// result screen (written next to the .mtp as <name>.png) before committing.
// See docs/research/mesentests-improvement-plan.md.
//
// Usage: mtp-record <rom> <out.mtp> [--frames N]   (default 600 frames)

#include "pch.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/RecordedRomTest.h"
#include "Shared/Video/VideoDecoder.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"

namespace fs = std::filesystem;

extern "C" RomTestResult RunRecordedTest(const char* filename, bool inBackground);

int main(int argc, char* argv[])
{
	vector<string> args;
	uint32_t frames = 600;
	for(int i = 1; i < argc; i++) {
		string a = argv[i];
		if(a == "--frames" && i + 1 < argc) {
			frames = (uint32_t)std::stoul(argv[++i]);
		} else {
			args.push_back(a);
		}
	}
	if(args.size() != 2 || frames == 0) {
		std::cerr << "usage: mtp-record <rom> <out.mtp> [--frames N]\n";
		return 2;
	}
	string romPath = fs::absolute(args[0]).string();
	string outPath = fs::absolute(args[1]).string();
	if(!fs::exists(romPath)) {
		std::cerr << "ROM not found: " << romPath << "\n";
		return 2;
	}
	fs::create_directories(fs::path(outPath).parent_path());
	fs::remove(outPath);

	// A scratch home folder, so no saves, settings or firmware from the user's own
	// home folder affect the recording (the CI runner uses <tests>/MesenHomeFolder).
	fs::path home = fs::temp_directory_path() / "mesen-mtp-record-home";
	fs::remove_all(home);
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());

	{
		unique_ptr<Emulator> emu(new Emulator());
		emu->Initialize(false);
		emu->GetSettings()->SetFlag(EmulationFlags::TestMode);
		if(!emu->LoadRom(VirtualFile(romPath), VirtualFile(""))) {
			std::cerr << "failed to load " << romPath << "\n";
			return 1;
		}

		shared_ptr<RecordedRomTest> test(new RecordedRomTest(emu.get(), true));
		test->Record(outPath, true); // power cycle, then record from power-on
		emu->GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);
		emu->Resume();

		while(emu->GetFrameCount() < frames) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		test->Stop(); // waits for the next new frame hash, then writes the .mtp

		std::stringstream png;
		emu->GetVideoDecoder()->TakeScreenshot(png);
		std::ofstream(fs::path(outPath).replace_extension(".png"), std::ios::binary) << png.rdbuf();
		emu->Stop(false);
		emu->Release();
	}

	if(!fs::exists(outPath)) {
		std::cerr << "recording failed: " << outPath << " was not written\n";
		return 1;
	}

	// Replay it the way RunCiTests does.
	RomTestResult result = RunRecordedTest(outPath.c_str(), true);
	const char* state = "FAILED";
	if(result.State == RomTestState::Passed) {
		state = "passed";
	} else if(result.State == RomTestState::PassedWithWarnings) {
		state = "passed with warnings";
	}
	std::cout << "recorded " << outPath << " (" << frames << "+ frames), replay " << state;
	if(result.State != RomTestState::Passed) {
		std::cout << " (error/bad frame count " << result.ErrorCode << ")";
	}
	std::cout << "\n";
	return result.State == RomTestState::Passed ? 0 : 1;
}
