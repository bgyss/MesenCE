#include "pch.h"
#include "MD/Debugger/MdVdpTools.h"
#include "MD/MdTypes.h"
#include "MD/MdVdp.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryDumper.h"

static constexpr uint8_t _dacLevels[8] = { 0, 52, 87, 116, 144, 172, 206, 255 }; //as MdDefaultVideoFilter (normal shade)

MdVdpTools::MdVdpTools(Debugger* debugger, Emulator* emu, MdVdp* vdp) : PpuTools(debugger, emu)
{
	_vdp = vdp;
}

uint32_t MdVdpTools::CramToArgb(uint16_t cram)
{
	uint8_t r = _dacLevels[(cram >> 1) & 0x07];
	uint8_t g = _dacLevels[(cram >> 5) & 0x07];
	uint8_t b = _dacLevels[(cram >> 9) & 0x07];
	return 0xFF000000 | (r << 16) | (g << 8) | b;
}

void MdVdpTools::SetViewerUpdateTiming(uint32_t viewerId, uint16_t scanline, uint16_t cycle)
{
	PpuTools::SetViewerUpdateTiming(viewerId, scanline, 0);
}

uint32_t MdVdpTools::GetPlaneWidth(MdVdpState& state)
{
	static const uint8_t sizes[4] = { 32, 64, 64, 128 };
	return sizes[state.Regs[16] & 0x03];
}

uint32_t MdVdpTools::GetPlaneHeight(MdVdpState& state)
{
	static const uint8_t sizes[4] = { 32, 64, 64, 128 };
	uint32_t w = GetPlaneWidth(state);
	uint32_t h = sizes[(state.Regs[16] >> 4) & 0x03];
	//Same limits as the renderer: the name table holds at most 4096 entries
	if(w == 128 && h > 32) {
		h = 32;
	} else if(w == 64 && h > 64) {
		h = 64;
	}
	return h;
}

FrameInfo MdVdpTools::GetTilemapSize(GetTilemapOptions options, BaseState& baseState)
{
	MdVdpState& state = (MdVdpState&)baseState;
	uint32_t cellHeight = ((state.Regs[12] >> 1) & 0x03) == 3 ? 16 : 8;
	if(options.Layer == 2) {
		//Window: 64 cells wide in H40, 32 in H32; 32 rows
		bool h40 = (state.Regs[12] & 0x81) != 0;
		return { (h40 ? 64u : 32u) * 8, 32 * cellHeight };
	}
	return { GetPlaneWidth(state) * 8, GetPlaneHeight(state) * cellHeight };
}

DebugTilemapInfo MdVdpTools::GetTilemap(GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint32_t* palette, uint32_t* outBuffer)
{
	MdVdpState& state = (MdVdpState&)baseState;
	bool h40 = (state.Regs[12] & 0x81) != 0;
	bool interlace2 = ((state.Regs[12] >> 1) & 0x03) == 3;
	uint32_t cellHeight = interlace2 ? 16 : 8;

	DebugTilemapInfo result = {};
	result.Bpp = 4;
	result.Format = TileFormat::WsBpp4Packed;
	result.TileWidth = 8;
	result.TileHeight = cellHeight;

	uint16_t base;
	if(options.Layer == 2) {
		base = (state.Regs[3] & (h40 ? 0x3C : 0x3E)) << 10;
		result.ColumnCount = h40 ? 64 : 32;
		result.RowCount = 32;
	} else {
		base = options.Layer == 0 ? ((state.Regs[2] & 0x38) << 10) : ((state.Regs[4] & 0x07) << 13);
		result.ColumnCount = GetPlaneWidth(state);
		result.RowCount = GetPlaneHeight(state);

		//Line 0 scroll values (full-screen scroll; per-line and per-column modes vary across the frame)
		uint16_t hsBase = (state.Regs[13] & 0x3F) << 10;
		uint16_t hs = (vram[(hsBase + options.Layer * 2) & 0xFFFF] << 8) | vram[(hsBase + options.Layer * 2 + 1) & 0xFFFF];
		uint32_t planeWidthPx = result.ColumnCount * 8;
		result.ScrollX = (planeWidthPx - (hs & 0x3FF) % planeWidthPx) % planeWidthPx;
		uint16_t* vsram = _vdp->GetVsram();
		result.ScrollY = (vsram[options.Layer] & 0x7FF) % (result.RowCount * cellHeight);
		result.ScrollWidth = h40 ? 320 : 256;
		result.ScrollHeight = (state.Regs[1] & 0x08) ? 240 : 224;
	}
	result.TilemapAddress = base;
	result.TilesetAddress = 0;

	uint32_t colorMask = 0x3F;
	const uint32_t* colors = palette;
	if(options.DisplayMode == TilemapDisplayMode::Grayscale) {
		colors = _grayscaleColorsBpp4;
		colorMask = 0x0F;
	}
	uint32_t bgColor = GetTilemapBackgroundColor(options.Background, palette[state.Regs[7] & 0x3F]);
	uint32_t widthPx = result.ColumnCount * 8;

	for(uint32_t row = 0; row < result.RowCount; row++) {
		for(uint32_t column = 0; column < result.ColumnCount; column++) {
			uint16_t entryAddr = (base + (row * result.ColumnCount + column) * 2) & 0xFFFF;
			uint16_t entry = (vram[entryAddr] << 8) | vram[(entryAddr + 1) & 0xFFFF];
			uint8_t paletteOffset = (entry >> 9) & 0x30;
			bool hflip = entry & 0x0800;
			bool vflip = entry & 0x1000;
			uint16_t tile = entry & 0x7FF;
			uint32_t tileSize = interlace2 ? 64 : 32;
			uint32_t tileAddr = interlace2 ? (tile & 0x3FF) * 64 : tile * 32;
			for(uint32_t y = 0; y < cellHeight; y++) {
				uint32_t tileRow = vflip ? (cellHeight - 1 - y) : y;
				uint32_t rowStart = (tileAddr + tileRow * 4) & 0xFFFF;
				for(uint32_t x = 0; x < 8; x++) {
					uint8_t tileColumn = hflip ? (7 - x) : x;
					uint8_t color = GetTilePixelColor<TileFormat::WsBpp4Packed>(vram, 0xFFFF, rowStart, tileColumn);
					uint32_t outPos = (row * cellHeight + y) * widthPx + column * 8 + x;
					outBuffer[outPos] = color == 0 ? bgColor : colors[(paletteOffset + color) & colorMask];
				}
			}
			(void)tileSize;
		}
	}
	return result;
}

DebugTilemapTileInfo MdVdpTools::GetTilemapTileInfo(uint32_t x, uint32_t y, uint8_t* vram, GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState)
{
	DebugTilemapTileInfo result = {};
	FrameInfo size = GetTilemapSize(options, baseState);
	if(x >= size.Width || y >= size.Height) {
		return result;
	}

	MdVdpState& state = (MdVdpState&)baseState;
	bool h40 = (state.Regs[12] & 0x81) != 0;
	bool interlace2 = ((state.Regs[12] >> 1) & 0x03) == 3;
	uint32_t cellHeight = interlace2 ? 16 : 8;
	uint32_t columns = size.Width / 8;
	uint16_t base;
	if(options.Layer == 2) {
		base = (state.Regs[3] & (h40 ? 0x3C : 0x3E)) << 10;
	} else {
		base = options.Layer == 0 ? ((state.Regs[2] & 0x38) << 10) : ((state.Regs[4] & 0x07) << 13);
	}

	uint32_t row = y / cellHeight;
	uint32_t column = x / 8;
	uint16_t entryAddr = (base + (row * columns + column) * 2) & 0xFFFF;
	uint16_t entry = (vram[entryAddr] << 8) | vram[(entryAddr + 1) & 0xFFFF];
	uint16_t tile = entry & 0x7FF;

	result.Row = row;
	result.Column = column;
	result.Width = 8;
	result.Height = cellHeight;
	result.TileMapAddress = entryAddr;
	result.TileIndex = tile;
	result.AddAddress(interlace2 ? (tile & 0x3FF) * 64 : tile * 32);
	result.PaletteIndex = (entry >> 13) & 0x03;
	result.PaletteAddress = result.PaletteIndex * 16 * 2;
	result.HighPriority = (entry & 0x8000) ? NullableBoolean::True : NullableBoolean::False;
	result.VerticalMirroring = (entry & 0x1000) ? NullableBoolean::True : NullableBoolean::False;
	result.HorizontalMirroring = (entry & 0x0800) ? NullableBoolean::True : NullableBoolean::False;
	return result;
}

DebugSpritePreviewInfo MdVdpTools::GetSpritePreviewInfo(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState)
{
	MdVdpState& state = (MdVdpState&)baseState;
	bool h40 = (state.Regs[12] & 0x81) != 0;
	bool interlace2 = ((state.Regs[12] >> 1) & 0x03) == 3;

	//Sprite coordinates are 9 bits (10 vertically in interlace mode 2); the screen starts at 128,128
	DebugSpritePreviewInfo info = {};
	info.Width = 512;
	info.Height = interlace2 ? 1024 : 512;
	info.SpriteCount = h40 ? 80 : 64;
	info.CoordOffsetX = 0;
	info.CoordOffsetY = 0;
	info.VisibleX = 128;
	info.VisibleY = interlace2 ? 256 : 128;
	info.VisibleWidth = h40 ? 320 : 256;
	info.VisibleHeight = ((state.Regs[1] & 0x08) ? 240 : 224) * (interlace2 ? 2 : 1);
	return info;
}

void MdVdpTools::GetSpriteInfo(DebugSpriteInfo& sprite, uint32_t* spritePreview, uint16_t index, MdVdpState& state, uint8_t* vram, uint32_t* palette)
{
	bool h40 = (state.Regs[12] & 0x81) != 0;
	bool interlace2 = ((state.Regs[12] >> 1) & 0x03) == 3;
	uint16_t satBase = (state.Regs[5] & (h40 ? 0x7E : 0x7F)) << 9;
	uint16_t entry = (satBase + index * 8) & 0xFFFF;
	auto word = [&](int offset) -> uint16_t {
		return (vram[(entry + offset) & 0xFFFF] << 8) | vram[(entry + offset + 1) & 0xFFFF];
	};
	uint16_t w0 = word(0);
	uint16_t w1 = word(2);
	uint16_t w2 = word(4);
	uint16_t w3 = word(6);

	uint32_t cellHeight = interlace2 ? 16 : 8;
	uint8_t hsize = ((w1 >> 10) & 0x03) + 1;
	uint8_t vsize = ((w1 >> 8) & 0x03) + 1;
	uint16_t tile = w2 & 0x7FF;
	bool hflip = w2 & 0x0800;
	bool vflip = w2 & 0x1000;
	uint8_t paletteOffset = (w2 >> 9) & 0x30;

	sprite.SpriteIndex = index;
	sprite.Bpp = 4;
	sprite.Format = TileFormat::WsBpp4Packed;
	sprite.RawX = w3 & 0x1FF;
	sprite.RawY = w0 & (interlace2 ? 0x3FF : 0x1FF);
	sprite.X = sprite.RawX;
	sprite.Y = sprite.RawY;
	sprite.Width = hsize * 8;
	sprite.Height = vsize * cellHeight;
	sprite.TileIndex = tile;
	sprite.Palette = paletteOffset >> 4;
	sprite.PaletteAddress = paletteOffset * 2;
	sprite.Priority = (w2 & 0x8000) ? DebugSpritePriority::Foreground : DebugSpritePriority::Background;
	sprite.HorizontalMirror = hflip ? NullableBoolean::True : NullableBoolean::False;
	sprite.VerticalMirror = vflip ? NullableBoolean::True : NullableBoolean::False;

	uint32_t visibleY = interlace2 ? 256 : 128;
	uint32_t visibleHeight = ((state.Regs[1] & 0x08) ? 240 : 224) * (interlace2 ? 2 : 1);
	uint32_t visibleWidth = h40 ? 320 : 256;
	bool onScreen = sprite.X + sprite.Width > 128 && sprite.X < 128 + (int)visibleWidth && sprite.Y + sprite.Height > (int)visibleY && sprite.Y < (int)(visibleY + visibleHeight);
	sprite.Visibility = onScreen ? SpriteVisibility::Visible : SpriteVisibility::Offscreen;

	//Cells are stored column by column
	uint32_t tileSize = interlace2 ? 64 : 32;
	sprite.TileCount = 0;
	for(uint32_t col = 0; col < hsize; col++) {
		for(uint32_t row = 0; row < vsize; row++) {
			uint16_t t = tile + col * vsize + row;
			sprite.TileAddresses[sprite.TileCount++] = interlace2 ? (t & 0x3FF) * tileSize : (t & 0x7FF) * tileSize;
		}
	}
	sprite.TileAddress = sprite.TileAddresses[0];

	for(uint32_t y = 0; y < sprite.Height; y++) {
		uint32_t sy = vflip ? (sprite.Height - 1 - y) : y;
		for(uint32_t x = 0; x < sprite.Width; x++) {
			uint32_t sx = hflip ? (sprite.Width - 1 - x) : x;
			uint16_t t = tile + (sx / 8) * vsize + (sy / cellHeight);
			uint32_t addr = interlace2 ? ((t & 0x3FF) * 64 + (sy % cellHeight) * 4) : ((t & 0x7FF) * 32 + (sy % cellHeight) * 4);
			uint8_t color = GetTilePixelColor<TileFormat::WsBpp4Packed>(vram, 0xFFFF, addr & 0xFFFF, sx & 0x07);
			spritePreview[y * sprite.Width + x] = color ? palette[(paletteOffset | color) & 0x3F] : 0;
		}
	}
}

void MdVdpTools::GetSpriteList(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint8_t* oamRam, uint32_t* palette, DebugSpriteInfo outBuffer[], uint32_t* spritePreviews, uint32_t* screenPreview)
{
	MdVdpState& state = (MdVdpState&)baseState;
	DebugSpritePreviewInfo info = GetSpritePreviewInfo(options, baseState, ppuToolsState);
	bool h40 = (state.Regs[12] & 0x81) != 0;
	uint16_t satBase = (state.Regs[5] & (h40 ? 0x7E : 0x7F)) << 9;

	for(uint32_t i = 0; i < info.SpriteCount; i++) {
		outBuffer[i].Init();
		GetSpriteInfo(outBuffer[i], spritePreviews + i * _spritePreviewSize, i, state, vram, palette);
	}

	//Sprites not reached by the link list (starting at sprite 0) are not displayed
	bool linked[80] = {};
	uint8_t index = 0;
	for(uint32_t n = 0; n < info.SpriteCount; n++) {
		if(linked[index]) {
			break;
		}
		linked[index] = true;
		uint8_t link = vram[(satBase + index * 8 + 3) & 0xFFFF] & 0x7F;
		if(link == 0 || link >= info.SpriteCount) {
			break;
		}
		index = link;
	}

	uint32_t bgColor = GetSpriteBackgroundColor(options.Background, palette, false);
	uint32_t offscreenColor = GetSpriteBackgroundColor(options.Background, palette, true);
	for(uint32_t y = 0; y < info.Height; y++) {
		for(uint32_t x = 0; x < info.Width; x++) {
			bool visible = x >= info.VisibleX && x < info.VisibleX + info.VisibleWidth && y >= info.VisibleY && y < info.VisibleY + info.VisibleHeight;
			screenPreview[y * info.Width + x] = visible ? bgColor : offscreenColor;
		}
	}

	//Draw in reverse link order so that earlier sprites end up on top
	for(int i = (int)info.SpriteCount - 1; i >= 0; i--) {
		DebugSpriteInfo& sprite = outBuffer[i];
		if(!linked[i]) {
			sprite.Visibility = SpriteVisibility::Disabled;
			continue;
		}
		uint32_t* preview = spritePreviews + i * _spritePreviewSize;
		for(int y = 0; y < sprite.Height; y++) {
			for(int x = 0; x < sprite.Width; x++) {
				uint32_t color = preview[y * sprite.Width + x];
				int px = sprite.X + x;
				int py = sprite.Y + y;
				if(color && px >= 0 && px < (int)info.Width && py >= 0 && py < (int)info.Height) {
					screenPreview[py * info.Width + px] = color;
				} else if(!color) {
					preview[y * sprite.Width + x] = bgColor;
				}
			}
		}
	}
}

DebugPaletteInfo MdVdpTools::GetPaletteInfo(GetPaletteInfoOptions options)
{
	DebugPaletteInfo info = {};
	info.PaletteMemType = MemoryType::MdPaletteRam;
	info.HasMemType = true;
	info.ColorCount = 64;
	info.BgColorCount = 64;
	info.SpriteColorCount = 64;
	info.SpritePaletteOffset = 0;
	info.ColorsPerPalette = 16;
	info.RawFormat = RawPaletteFormat::MdCram;

	uint16_t* cram = _vdp->GetCram();
	for(int i = 0; i < 64; i++) {
		info.RawPalette[i] = cram[i];
		info.RgbPalette[i] = CramToArgb(cram[i]);
	}
	return info;
}

void MdVdpTools::SetPaletteColor(int32_t colorIndex, uint32_t color)
{
	//Nearest 3-bit level per channel
	auto toLevel = [](uint8_t v) -> uint16_t {
		int best = 0;
		for(int i = 1; i < 8; i++) {
			if(std::abs(_dacLevels[i] - v) < std::abs(_dacLevels[best] - v)) {
				best = i;
			}
		}
		return (uint16_t)best;
	};
	uint16_t value = (toLevel((color >> 16) & 0xFF) << 1) | (toLevel((color >> 8) & 0xFF) << 5) | (toLevel(color & 0xFF) << 9);
	if(colorIndex >= 0 && colorIndex < 64) {
		_vdp->GetCram()[colorIndex] = value;
	}
}
