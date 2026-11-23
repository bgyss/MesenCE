// check.cpp - the Mega Drive debugger viewers (MdVdpTools) must decode VRAM/CRAM the way the VDP
// renders it. For each synthetic render cartridge, after N frames:
//   palette  every pixel of the rendered frame is one of the 64 CRAM colours the palette viewer
//            reports (cases without shadow/highlight);
//   tilemap  composing the plane A and plane B tilemap views (transparent background, line-0 scroll)
//            over the backdrop reproduces the rendered frame (cases whose planes use one scroll
//            value and no sprites, window or priority);
//   sprites  every opaque pixel of the sprite viewer's screen preview, inside the visible area,
//            equals the rendered pixel (sprite cases without per-line overflow, planes behind sprites).
//
// Usage: md-vdp-tools-check [--frames N] <case.md> ...   (exit 1 on any mismatch)

#include "pch.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>
#define private public
#include "Shared/Emulator.h"
#undef private
#include "Shared/EmuSettings.h"
#include "Shared/MessageManager.h"
#include "MD/MdConsole.h"
#include "MD/MdVdp.h"
#include "MD/MdTypes.h"
#include "MD/Debugger/MdVdpTools.h"
#include "Debugger/Debugger.h"
#include "Debugger/PpuTools.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/FolderUtilities.h"

namespace fs = std::filesystem;

static uint32_t FramePixelToArgb(uint16_t px)
{
	//Same DAC levels as MdDefaultVideoFilter / MdVdpTools (normal shade)
	uint16_t cram = ((px & 0x07) << 1) | (((px >> 3) & 0x07) << 5) | (((px >> 6) & 0x07) << 9);
	return MdVdpTools::CramToArgb(cram);
}

struct Result
{
	uint32_t checked = 0;
	uint32_t mismatched = 0;
};

int main(int argc, char** argv)
{
	uint32_t frames = 4;
	vector<string> roms;
	for(int i = 1; i < argc; i++) {
		string a = argv[i];
		if(a == "--frames" && i + 1 < argc) {
			frames = (uint32_t)std::stoul(argv[++i]);
		} else {
			roms.push_back(a);
		}
	}

	//Isolated home folder (no user configuration, nothing written next to the ROMs)
	fs::path home = fs::temp_directory_path() / ("md-vdp-tools-check-" + std::to_string(getpid()));
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());

	int failures = 0;
	for(string& rom : roms) {
		string name = fs::path(rom).stem().string();
		Emulator emu;
		emu.GetSettings()->SetFlag(EmulationFlags::TestMode);
		emu.GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);
		MessageManager::SetOptions(false, false);
		emu.Initialize(false);
		emu._isRunAheadFrame = true; //no video decoder thread
		if(!emu.LoadRom(VirtualFile(fs::absolute(rom).string()), VirtualFile(), false)) {
			std::cerr << "could not load " << rom << "\n";
			return 2;
		}
		emu.InitDebugger();
		emu.SetHeadlessMode(frames);
		emu.Run();

		MdConsole* console = dynamic_cast<MdConsole*>(emu.GetConsole().get());
		Debugger* dbg = emu.InternalGetDebugger();
		PpuTools* tools = dbg->GetPpuTools(CpuType::MdM68k);
		if(!console || !tools) {
			std::cerr << name << ": not a Mega Drive console or no VDP tools\n";
			return 2;
		}
		MdVdp* vdp = console->GetVdp();
		MdVdpState state = {};
		dbg->GetPpuState(state, CpuType::MdM68k);
		uint8_t* vram = vdp->GetVram();

		DebugPaletteInfo pal = tools->GetPaletteInfo({});
		uint16_t* frame = vdp->GetScreenBuffer(true);
		uint32_t width = vdp->GetFrameWidth();
		uint32_t height = vdp->GetFrameHeight();
		bool interlace2 = ((state.Regs[12] >> 1) & 0x03) == 3;
		bool shadowHighlight = state.Regs[12] & 0x08;
		bool display = state.Regs[1] & 0x40;

		auto at = [&](uint32_t x, uint32_t y) { return FramePixelToArgb(frame[y * width + x]); };

		//Palette
		Result palette;
		if(!shadowHighlight) {
			for(uint32_t y = 0; y < height; y++) {
				for(uint32_t x = 0; x < width; x++) {
					uint32_t c = at(x, y);
					palette.checked++;
					if(std::find(pal.RgbPalette, pal.RgbPalette + 64, c) == pal.RgbPalette + 64) {
						palette.mismatched++;
					}
				}
			}
		}

		//Tilemap: planes A and B composed over the backdrop
		Result tilemap;
		bool tilemapCase = display && !shadowHighlight && !interlace2 && (name.find("render_plane") == 0 || name == "render_tile_map" || name == "render_h40" || name == "render_h32_v30" || name == "render_h40_v30" || name == "render_scroll_vertical");
		if(tilemapCase && name != "render_plane_sprite_priority") {
			GetTilemapOptions opt = {};
			opt.Background = TilemapBackground::Transparent;
			vector<uint32_t> layers[2];
			DebugTilemapInfo info[2];
			for(int layer = 0; layer < 2; layer++) {
				opt.Layer = (uint8_t)layer;
				FrameInfo size = tools->GetTilemapSize(opt, state);
				layers[layer].resize(size.Width * size.Height);
				info[layer] = tools->GetTilemap(opt, state, state, vram, pal.RgbPalette, layers[layer].data());
			}
			uint32_t backdrop = pal.RgbPalette[state.Regs[7] & 0x3F];
			bool perColumnVScroll = state.Regs[11] & 0x04;
			bool fullScreenHScroll = (state.Regs[11] & 0x03) == 0;
			if(fullScreenHScroll && !perColumnVScroll) {
				for(uint32_t y = 0; y < height; y++) {
					for(uint32_t x = 0; x < width; x++) {
						uint32_t c = backdrop;
						for(int layer = 1; layer >= 0; layer--) {
							uint32_t w = info[layer].ColumnCount * 8;
							uint32_t h = info[layer].RowCount * 8;
							uint32_t px = layers[layer][((y + info[layer].ScrollY) % h) * w + (x + info[layer].ScrollX) % w];
							if(px) {
								c = px;
							}
						}
						tilemap.checked++;
						if(c != at(x, y)) {
							tilemap.mismatched++;
						}
					}
				}
			}
		}

		//Sprites
		Result sprites;
		//The viewer lists every linked sprite; the overflow cases rely on the per-line limit dropping one, so
		//their rendered frame intentionally lacks a sprite the viewer shows.
		if(display && !shadowHighlight && name.find("render_sprite") == 0 && name.find("overflow") == string::npos) {
			GetSpritePreviewOptions opt = {};
			opt.Background = SpriteBackground::Transparent;
			DebugSpritePreviewInfo info = tools->GetSpritePreviewInfo(opt, state, state);
			vector<DebugSpriteInfo> list(info.SpriteCount);
			vector<uint32_t> previews(info.SpriteCount * 128 * 128);
			vector<uint32_t> screen(info.Width * info.Height);
			tools->GetSpriteList(opt, state, state, vram, nullptr, pal.RgbPalette, list.data(), previews.data(), screen.data());
			for(uint32_t y = 0; y < std::min(height, info.VisibleHeight); y++) {
				for(uint32_t x = 0; x < std::min(width, info.VisibleWidth); x++) {
					uint32_t c = screen[(y + info.VisibleY) * info.Width + x + info.VisibleX];
					if(c) {
						sprites.checked++;
						if(c != at(x, y)) {
							sprites.mismatched++;
						}
					}
				}
			}
		}

		auto report = [&](const char* what, Result& r) {
			if(r.checked) {
				std::cout << "  " << what << ": " << (r.checked - r.mismatched) << "/" << r.checked << " pixels match\n";
			}
			if(r.mismatched) {
				failures++;
			}
		};
		std::cout << (palette.mismatched || tilemap.mismatched || sprites.mismatched ? "FAIL " : "ok   ") << name << " (" << width << "x" << height << ")\n";
		report("palette", palette);
		report("tilemap", tilemap);
		report("sprites", sprites);
		emu.Release();
	}
	std::error_code ec;
	fs::remove_all(home, ec);
	return failures ? 1 : 0;
}
