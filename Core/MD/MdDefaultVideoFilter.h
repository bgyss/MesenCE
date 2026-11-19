#pragma once
#include "pch.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"

// Converts the VDP's output (bits 0-8: 3-bit R, G, B; bits 9-10: 0 normal, 1 shadow,
// 2 highlight) to ARGB. The DAC levels are the commonly used measured values of a Model 1
// console (normal, shadow = half level, highlight = half level + half range); they are not
// qualified here against a measurement of our own.
class MdDefaultVideoFilter : public BaseVideoFilter
{
private:
	uint32_t _palette[0x800] = {};
	//32X pixels (bit 15 set): 15-bit BGR, 5 bits per channel
	uint32_t _palette32x[0x8000] = {};
	VideoConfig _videoConfig = {};

	void InitLookupTable()
	{
		static constexpr uint8_t normal[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
		static constexpr uint8_t shadow[8] = { 0, 29, 52, 70, 87, 101, 116, 130 };
		static constexpr uint8_t highlight[8] = { 130, 144, 158, 172, 187, 206, 228, 255 };

		VideoConfig config = _emu->GetSettings()->GetVideoConfig();
		InitConversionMatrix(config.Hue, config.Saturation);
		for(uint32_t i = 0; i < 0x800; i++) {
			uint8_t shade = (i >> 9) & 0x03;
			const uint8_t* levels = shade == 1 ? shadow : (shade == 2 ? highlight : normal);
			uint8_t r = levels[i & 0x07];
			uint8_t g = levels[(i >> 3) & 0x07];
			uint8_t b = levels[(i >> 6) & 0x07];
			if(config.Hue != 0 || config.Saturation != 0 || config.Brightness != 0 || config.Contrast != 0) {
				ApplyColorOptions(r, g, b, config.Brightness, config.Contrast);
			}
			_palette[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
		}
		for(uint32_t i = 0; i < 0x8000; i++) {
			uint8_t r = (uint8_t)((i & 0x1F) * 255 / 31);
			uint8_t g = (uint8_t)(((i >> 5) & 0x1F) * 255 / 31);
			uint8_t b = (uint8_t)(((i >> 10) & 0x1F) * 255 / 31);
			if(config.Hue != 0 || config.Saturation != 0 || config.Brightness != 0 || config.Contrast != 0) {
				ApplyColorOptions(r, g, b, config.Brightness, config.Contrast);
			}
			_palette32x[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
		}
		_videoConfig = config;
	}

protected:
	void OnBeforeApplyFilter() override
	{
		VideoConfig& config = _emu->GetSettings()->GetVideoConfig();
		if(_videoConfig.Hue != config.Hue || _videoConfig.Saturation != config.Saturation || _videoConfig.Contrast != config.Contrast || _videoConfig.Brightness != config.Brightness) {
			InitLookupTable();
		}
	}

public:
	MdDefaultVideoFilter(Emulator* emu) : BaseVideoFilter(emu)
	{
		InitLookupTable();
	}

	void ApplyFilter(uint16_t* ppuOutputBuffer) override
	{
		uint32_t* out = GetOutputBuffer();
		FrameInfo frame = _frameInfo;
		OverscanDimensions overscan = GetOverscan();
		for(uint32_t y = 0; y < frame.Height; y++) {
			for(uint32_t x = 0; x < frame.Width; x++) {
				uint16_t px = ppuOutputBuffer[(y + overscan.Top) * _baseFrameInfo.Width + x + overscan.Left];
				out[y * frame.Width + x] = (px & 0x8000) ? _palette32x[px & 0x7FFF] : _palette[px & 0x7FF];
			}
		}
	}
};
