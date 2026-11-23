#include "pch.h"
#include "SMS/SmsPsg.h"
#include "SMS/SmsFmAudio.h"
#include "Utilities/Serializer.h"

SmsPsg::SmsPsg(Emulator* emu, SmsConsole* console)
{
	_console = console;
	_soundMixer = emu->GetSoundMixer();
	_settings = emu->GetSettings();
	_psg.AltNoiseMode = _console->GetModel() == SmsModel::ColecoVision || _console->GetModel() == SmsModel::Sg;

	_psg.State.Noise.Lfsr = 0x8000;
	_psg.State.Noise.Volume = 0x0F;
	_psg.State.Tone[0].Volume = 0x0F;
	_psg.State.Tone[1].Volume = 0x0F;
	_psg.State.Tone[2].Volume = 0x0F;

	_soundBuffer = new int16_t[SmsPsg::MaxSamples * 2];
	memset(_soundBuffer, 0, SmsPsg::MaxSamples * 2 * sizeof(int16_t));

	_leftChannel = blip_new(SmsPsg::MaxSamples);
	_rightChannel = blip_new(SmsPsg::MaxSamples);

	blip_clear(_leftChannel);
	blip_clear(_rightChannel);

	blip_set_rates(_leftChannel, _console->GetMasterClockRate(), SmsPsg::SampleRate);
	blip_set_rates(_rightChannel, _console->GetMasterClockRate(), SmsPsg::SampleRate);
}

void SmsPsg::SetRegion(ConsoleRegion region)
{
	blip_clear(_leftChannel);
	blip_clear(_rightChannel);

	blip_set_rates(_leftChannel, _console->GetMasterClockRate(), SmsPsg::SampleRate);
	blip_set_rates(_rightChannel, _console->GetMasterClockRate(), SmsPsg::SampleRate);
}

void SmsPsg::Run()
{
	uint64_t runTo = _console->GetMasterClock();
	uint32_t* volumes = _console->GetModel() == SmsModel::ColecoVision ? _settings->GetCvConfig().ChannelVolumes : _settings->GetSmsConfig().ChannelVolumes;

	while(_masterClock + 16 < runTo) {
		int16_t outputLeft = 0;
		int16_t outputRight = 0;
		int16_t channelOutput;
		_psg.Step();
		SmsPsgState& state = _psg.State;
		for(int i = 0; i < 3; i++) {
			channelOutput = state.Tone[i].Output * Sn76489::VolumeLut[state.Tone[i].Volume] * volumes[i] / 100;
			if(state.GameGearPanningReg & (0x01 << i)) {
				outputRight += channelOutput;
			}
			if(state.GameGearPanningReg & (0x10 << i)) {
				outputLeft += channelOutput;
			}
		}

		channelOutput = state.Noise.Output * Sn76489::VolumeLut[state.Noise.Volume] * volumes[3] / 100;
		if(state.GameGearPanningReg & 0x08) {
			outputRight += channelOutput;
		}
		if(state.GameGearPanningReg & 0x80) {
			outputLeft += channelOutput;
		}

		_clockCounter += 16;
		_masterClock += 16;

		if(_prevOutputLeft != outputLeft || _prevOutputRight != outputRight) {
			blip_add_delta(_leftChannel, _clockCounter, outputLeft - _prevOutputLeft);
			blip_add_delta(_rightChannel, _clockCounter, outputRight - _prevOutputRight);
			_prevOutputLeft = outputLeft;
			_prevOutputRight = outputRight;
		}
	}

	if(_clockCounter >= 20000) {
		PlayQueuedAudio();
	}
}

void SmsPsg::PlayQueuedAudio()
{
	blip_end_frame(_leftChannel, _clockCounter);
	blip_end_frame(_rightChannel, _clockCounter);

	uint32_t sampleCount = (uint32_t)blip_read_samples(_leftChannel, _soundBuffer, SmsPsg::MaxSamples, 1);
	blip_read_samples(_rightChannel, _soundBuffer + 1, SmsPsg::MaxSamples, 1);

	if(_console->IsPsgAudioMuted()) {
		memset(_soundBuffer, 0, SmsPsg::MaxSamples * 2 * sizeof(int16_t));
	}

	_soundMixer->PlayAudioBuffer(_soundBuffer, sampleCount, SmsPsg::SampleRate);
	_clockCounter = 0;
}

void SmsPsg::Write(uint8_t value)
{
	Run();
	_psg.Write(value);
}

void SmsPsg::WritePanningReg(uint8_t value)
{
	_psg.State.GameGearPanningReg = value;
}

void SmsPsg::Serialize(Serializer& s)
{
	//Keeps the save-state keys of the pre-Sn76489 layout ("_state.*")
	SmsPsgState& _state = _psg.State;
	if(s.IsSaving()) {
		Run();
	} else {
		_clockCounter = 0;
		blip_clear(_leftChannel);
		blip_clear(_rightChannel);
	}

	SV(_state.SelectedReg);
	SV(_state.GameGearPanningReg);

	SV(_state.Noise.Timer);
	SV(_state.Noise.Lfsr);
	SV(_state.Noise.LfsrInputBit);
	SV(_state.Noise.Control);
	SV(_state.Noise.Output);
	SV(_state.Noise.Volume);

	for(int i = 0; i < 3; i++) {
		SVI(_state.Tone[i].ReloadValue);
		SVI(_state.Tone[i].Timer);
		SVI(_state.Tone[i].Output);
		SVI(_state.Tone[i].Volume);
	}

	if(s.GetFormat() != SerializeFormat::Map) {
		//Hide these entries from the Lua API
		SV(_masterClock);
		SV(_clockCounter);
		SV(_prevOutputLeft);
		SV(_prevOutputRight);
	}
}
