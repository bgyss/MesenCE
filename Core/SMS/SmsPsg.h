#pragma once
#include "pch.h"
#include "SMS/SmsConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/ISerializable.h"
#include "Utilities/Audio/blip_buf.h"
#include "Shared/Audio/Sn76489.h"

class SmsPsg final : public ISerializable
{
private:
	static constexpr int SampleRate = 96000;
	static constexpr int MaxSamples = 4000;

	int16_t* _soundBuffer = nullptr;
	blip_t* _leftChannel = nullptr;
	blip_t* _rightChannel = nullptr;

	SoundMixer* _soundMixer = nullptr;
	EmuSettings* _settings = nullptr;
	SmsConsole* _console = nullptr;

	Sn76489 _psg;
	uint64_t _masterClock = 0;
	uint64_t _clockCounter = 0;
	int16_t _prevOutputLeft = 0;
	int16_t _prevOutputRight = 0;

public:
	SmsPsg(Emulator* emu, SmsConsole* console);

	SmsPsgState& GetState() { return _psg.State; }

	void SetRegion(ConsoleRegion region);

	void Run();
	void PlayQueuedAudio();

	void Write(uint8_t value);
	void WritePanningReg(uint8_t value);

	void Serialize(Serializer& s) override;
};