// dma_regression.cpp - regression test for the OAM DMA restart bug (NES).
//
// Runs docs/research/tla/rom/dmc_oam_overlap.nes headlessly with the
// "DMC sample duplication glitch" option enabled (the only configuration in
// which the bug could occur), then reads the ROM's own results from CPU RAM:
//   $10-$11 iterations, $12-$13 failures (OAM[0] != 0 after an OAM DMA).
// See docs/research/nes-verification-findings.md, finding 1.
//
// Usage: dma_regression <dmc_oam_overlap.nes> [frames]

#include "pch.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#define private public
#include "Shared/Emulator.h"
#undef private
#include "Shared/EmuSettings.h"
#include "NES/NesConsole.h"
#include "NES/NesMemoryManager.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/FolderUtilities.h"

namespace fs = std::filesystem;

int main(int argc, char* argv[])
{
	if(argc < 2) {
		std::cerr << "usage: dma_regression <dmc_oam_overlap.nes> [frames]\n";
		return 2;
	}
	int frames = argc > 2 ? std::stoi(argv[2]) : 600;

	fs::path home = fs::temp_directory_path() / "mesen-dma-regression-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);
	emu._isRunAheadFrame = true; // headless: emulate frames without sending them to the video decoder
	emu.GetSettings()->GetNesConfig().EnableDmcSampleDuplicationGlitch = true;

	std::ifstream in(argv[1], std::ios::binary);
	vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), {});
	VirtualFile file(rom.data(), rom.size(), "dmc_oam_overlap.nes");
	shared_ptr<NesConsole> console = std::make_shared<NesConsole>(&emu);
	if(console->LoadRom(file) != LoadRomResult::Success) {
		std::cerr << "failed to load " << argv[1] << "\n";
		return 2;
	}
	emu._console.reset(std::static_pointer_cast<IConsole>(console));
	for(int i = 0; i < frames; i++) {
		console->RunFrame();
	}

	NesMemoryManager* mm = console->GetMemoryManager();
	uint32_t iterations = mm->DebugRead(0x10) | (mm->DebugRead(0x11) << 8);
	uint32_t failures = mm->DebugRead(0x12) | (mm->DebugRead(0x13) << 8);
	std::cout << "frames=" << frames << " iterations=" << iterations << " failures=" << failures << "\n";
	if(iterations == 0) {
		std::cout << "FAIL: the ROM did not run\n";
		return 1;
	}
	std::cout << (failures ? "FAIL: OAM DMA wrote the wrong bytes (finding 1 regressed)\n" : "PASS\n");
	return failures ? 1 : 0;
}
