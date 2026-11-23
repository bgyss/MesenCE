#pragma once
#include "pch.h"
#include "SMS/SmsTypes.h"

// SN76489-compatible PSG core (the Sega VDP variant: 16-bit noise LFSR tapping bits 0 and 3; the
// SG-1000/ColecoVision TI variant uses a 15-bit LFSR tapping bits 0 and 1). Shared by the SMS
// family (SmsPsg) and the Mega Drive (MdApu). Step() is one tone-counter clock (16 input clocks);
// the hosts own timing, mixing and output.
class Sn76489
{
public:
	SmsPsgState State = {};
	bool AltNoiseMode = false;

	static constexpr int16_t VolumeLut[16] = { 8191, 6507, 5168, 4105, 3261, 2590, 2058, 1642, 1298, 1031, 819, 651, 517, 410, 326, 0 };

	void Step()
	{
		for(int i = 0; i < 3; i++) {
			if(State.Tone[i].Timer == 0 || --State.Tone[i].Timer == 0) {
				State.Tone[i].Output ^= 1;
				State.Tone[i].Timer = State.Tone[i].ReloadValue;
			}
		}
		RunNoise(State.Noise);
	}

	void Write(uint8_t value)
	{
		if(value & 0x80) {
			State.SelectedReg = (value >> 4) & 0x07;
		}

		uint8_t channel = (State.SelectedReg >> 1) & 0x03;
		bool volReg = State.SelectedReg & 0x01;

		switch(channel) {
			case 0:
			case 1:
			case 2:
				if(volReg) {
					State.Tone[channel].Volume = value & 0x0F;
				} else {
					if(value & 0x80) {
						State.Tone[channel].ReloadValue = (State.Tone[channel].ReloadValue & 0x3F0) | (value & 0x0F);
					} else {
						State.Tone[channel].ReloadValue = (State.Tone[channel].ReloadValue & 0x0F) | ((value & 0x3F) << 4);
					}
				}
				break;

			case 3:
				if(volReg) {
					State.Noise.Volume = value & 0x0F;
				} else {
					State.Noise.Control = value & 0x07;
					State.Noise.Lfsr = 0x8000;
				}
				break;
		}
	}

private:
	void RunNoise(SmsNoiseChannelState& noise)
	{
		if(noise.Timer == 0 || --noise.Timer == 0) {
			noise.LfsrInputBit ^= 1;
			switch(noise.Control & 0x03) {
				case 0: noise.Timer = 0x10; break;
				case 1: noise.Timer = 0x20; break;
				case 2: noise.Timer = 0x40; break;
				case 3: noise.Timer = State.Tone[2].ReloadValue; break;
			}

			if(noise.LfsrInputBit) {
				if(AltNoiseMode) {
					bool useBit1 = noise.Control & 0x04;
					uint16_t newBit = (noise.Lfsr & 0x01) ^ (useBit1 & ((noise.Lfsr >> 1) & 0x01));
					noise.Lfsr = (newBit << 14) | (noise.Lfsr >> 1);
				} else {
					bool useBit3 = noise.Control & 0x04;
					uint16_t newBit = (noise.Lfsr & 0x01) ^ (useBit3 & ((noise.Lfsr >> 3) & 0x01));
					noise.Lfsr = (newBit << 15) | (noise.Lfsr >> 1);
				}
				noise.Output = noise.Lfsr & 0x01;
			}
		}
	}
};
