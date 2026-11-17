#pragma once
#include "pch.h"
#include "Debugger/PpuTools.h"

class Debugger;
class Emulator;
class MdVdp;
struct MdVdpState;

// Tile, tilemap, sprite and palette viewers for the Mega Drive VDP (Mode 5). Decoding follows the
// renderer in MdVdp.cpp: 4bpp packed tiles (high nibble = left pixel, 32 bytes per 8x8 cell, 64 per
// 8x16 cell in interlace mode 2), name table entries (priority, palette, V/H flip, 11-bit tile),
// 8-byte sprite attribute entries in link order, CRAM words 0000BBB0GGG0RRR0.
class MdVdpTools final : public PpuTools
{
private:
	MdVdp* _vdp = nullptr;

	uint32_t GetPlaneWidth(MdVdpState& state);
	uint32_t GetPlaneHeight(MdVdpState& state);
	void GetSpriteInfo(DebugSpriteInfo& sprite, uint32_t* spritePreview, uint16_t index, MdVdpState& state, uint8_t* vram, uint32_t* palette);

public:
	MdVdpTools(Debugger* debugger, Emulator* emu, MdVdp* vdp);

	static uint32_t CramToArgb(uint16_t cram);

	//The renderer works per line: viewers refresh at the start of the selected line
	void SetViewerUpdateTiming(uint32_t viewerId, uint16_t scanline, uint16_t cycle) override;

	DebugTilemapInfo GetTilemap(GetTilemapOptions options, BaseState& state, BaseState& ppuToolsState, uint8_t* vram, uint32_t* palette, uint32_t* outBuffer) override;
	FrameInfo GetTilemapSize(GetTilemapOptions options, BaseState& state) override;
	DebugTilemapTileInfo GetTilemapTileInfo(uint32_t x, uint32_t y, uint8_t* vram, GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState) override;

	DebugSpritePreviewInfo GetSpritePreviewInfo(GetSpritePreviewOptions options, BaseState& state, BaseState& ppuToolsState) override;
	void GetSpriteList(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint8_t* oamRam, uint32_t* palette, DebugSpriteInfo outBuffer[], uint32_t* spritePreviews, uint32_t* screenPreview) override;

	DebugPaletteInfo GetPaletteInfo(GetPaletteInfoOptions options) override;
	void SetPaletteColor(int32_t colorIndex, uint32_t color) override;
};
