#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"

// Ricoh RF5C164 PCM sound source (the Sega CD's "SDC"): 8 channels playing 8-bit samples from
// 64 KiB of wave RAM, one output sample every 384 sub-CPU cycles (12.5 MHz / 384 = 32,552 Hz).
//
// Sources: Mega-CD Hardware Manual, appendix "RF5C164" pp. 6-12 (register map, control register,
// ST/LS/FD/PAN/ENV, channel address read-back, wave RAM banks). ares md/mcd/pcm.cpp (ISC) for what
// the manual leaves out: samples are sign-magnitude (bit 7 set = positive), $FF is the loop marker
// (playback continues from LS), the on/off register's bits are 0 = sounding, and the mix is clamped
// to 16 bits and reduced to the chip's 10-bit DAC resolution.
//
// The sub CPU sees the chip at $FF0000-$FF3FFF on the lower data byte: register n at $FF0000 + 2n.
class McdPcm final : public ISerializable
{
public:
	struct Sample
	{
		uint64_t Clock; //master clock at which the sample was produced
		int16_t Left;
		int16_t Right;
	};

private:
	struct Channel
	{
		uint8_t Envelope = 0;
		uint8_t Pan = 0xFF;
		uint16_t Step = 0; //FD: 5.11 fixed point address increment per sample
		uint16_t Loop = 0; //LS
		uint8_t Start = 0; //ST: high byte of the start address
		uint32_t Address = 0; //16.11 fixed point
		bool Enabled = false;
	};

	uint8_t _ram[0x10000] = {};
	Channel _channels[8];
	bool _soundOn = false;
	uint8_t _bank = 0;
	uint8_t _channel = 0;

	//Samples not yet taken by the mixer (MdApu), oldest first
	vector<Sample> _pending;
	Sample _current = {};
	std::function<void(int16_t, int16_t)> _observer;

public:
	uint8_t Read(uint16_t reg);
	void Write(uint16_t reg, uint8_t value);

	//One output sample (every 384 sub cycles), produced at master clock `clock`
	void Clock(uint64_t clock);

	//The PCM output in effect at master clock `clock` (the latest sample produced at or before it)
	void GetOutput(uint64_t clock, int32_t& left, int32_t& right);

	//Optional observer of each output sample (headless chip-audio capture)
	void SetObserver(std::function<void(int16_t, int16_t)> observer) { _observer = observer; }

	void Serialize(Serializer& s) override;
};
