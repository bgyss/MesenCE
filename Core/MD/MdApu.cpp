#include "pch.h"
#include "MD/MdApu.h"
#include "MD/MdConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/Serializer.h"

MdApu::MdApu() : _ym(*this)
{
}

void MdApu::Init(Emulator* emu, MdConsole* console, bool isPal)
{
	_emu = emu;
	_console = console;
	uint32_t mclk = isPal ? MdTiming::PalMasterClockRate : MdTiming::NtscMasterClockRate;
	_sampleRate = (mclk + ClocksPerYmSample / 2) / ClocksPerYmSample;
	_psg.State = {};
	//Power-on: all PSG channels silent
	for(int i = 0; i < 3; i++) {
		_psg.State.Tone[i].Volume = 0x0F;
	}
	_psg.State.Noise.Volume = 0x0F;
	_psg.State.Noise.Lfsr = 0x8000;
	_ym.reset();
	_samples.reserve(2048);
}

void MdApu::ymfm_set_timer(uint32_t tnum, int32_t durationInClocks)
{
	if(tnum < 2) {
		_timerExpire[tnum] = durationInClocks < 0 ? 0 : (_now + (uint64_t)durationInClocks * MdTiming::M68kDivider);
	}
}

void MdApu::ymfm_set_busy_end(uint32_t clocks)
{
	_busyEnd = _now + (uint64_t)clocks * MdTiming::M68kDivider;
}

bool MdApu::ymfm_is_busy()
{
	return _now < _busyEnd;
}

void MdApu::ProcessTimers(uint64_t until)
{
	while(true) {
		int next = -1;
		for(int i = 0; i < 2; i++) {
			if(_timerExpire[i] && _timerExpire[i] <= until && (next < 0 || _timerExpire[i] < _timerExpire[next])) {
				next = i;
			}
		}
		if(next < 0) {
			break;
		}
		_now = _timerExpire[next];
		_timerExpire[next] = 0;
		//ymfm sets the flag and restarts the timer through ymfm_set_timer (relative to _now)
		m_engine->engine_timer_expired(next);
	}
}

void MdApu::GenerateSample()
{
	//PSG steps within this sample period, averaged (a box filter)
	int32_t psgLeft = 0;
	int32_t psgRight = 0;
	int32_t steps = 0;
	uint64_t sampleEnd = _clock + ClocksPerYmSample;
	while(_psgClock < sampleEnd) {
		_psg.Step();
		int32_t out = 0;
		for(int i = 0; i < 3; i++) {
			out += _psg.State.Tone[i].Output * Sn76489::VolumeLut[_psg.State.Tone[i].Volume];
		}
		out += _psg.State.Noise.Output * Sn76489::VolumeLut[_psg.State.Noise.Volume];
		psgLeft += out;
		psgRight += out;
		steps++;
		_psgClock += ClocksPerPsgStep;
	}
	if(steps) {
		psgLeft /= steps;
		psgRight /= steps;
	}

	ymfm::ym2612::output_data output;
	_ym.generate(&output, 1);

	if(_chipObserver) {
		_chipObserver(output.data[0], output.data[1], psgLeft, psgRight);
	}

	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();
	//The PSG sits about 6 dB below a full-scale FM channel mix (approximate, not hardware-qualified)
	int32_t left = output.data[0] * (int32_t)cfg.FmVolume / 100 + psgLeft * (int32_t)cfg.PsgVolume / 200;
	int32_t right = output.data[1] * (int32_t)cfg.FmVolume / 100 + psgRight * (int32_t)cfg.PsgVolume / 200;
	_samples.push_back((int16_t)std::clamp(left, -32768, 32767));
	_samples.push_back((int16_t)std::clamp(right, -32768, 32767));
	_clock = sampleEnd;
}

void MdApu::Run(uint64_t masterClock)
{
	while(_clock + ClocksPerYmSample <= masterClock) {
		ProcessTimers(_clock + ClocksPerYmSample);
		_now = _clock;
		GenerateSample();
	}
	ProcessTimers(masterClock);
	_now = masterClock;
}

void MdApu::EndFrame(uint64_t masterClock)
{
	Run(masterClock);
	if(!_samples.empty()) {
		_emu->GetSoundMixer()->PlayAudioBuffer(_samples.data(), (uint32_t)(_samples.size() / 2), _sampleRate);
		_samples.clear();
	}
}

uint8_t MdApu::ReadYm(uint8_t port)
{
	//The YM2612 returns its status on all four ports (Sega Genesis Software Manual, "FM sound")
	return _ym.read_status();
}

void MdApu::WriteYm(uint8_t port, uint8_t value)
{
	_ym.write(port & 0x03, value);
}

void MdApu::ResetYm()
{
	_ym.reset();
	_timerExpire[0] = _timerExpire[1] = 0;
	_busyEnd = 0;
}

void MdApu::WritePsg(uint8_t value)
{
	_psg.Write(value);
}

void MdApu::Serialize(Serializer& s)
{
	vector<uint8_t> ymData;
	ymfm::ymfm_saved_state state(ymData, s.IsSaving());
	if(s.IsSaving()) {
		_ym.save_restore(state);
	}
	SVVector(ymData);
	if(!s.IsSaving()) {
		ymfm::ymfm_saved_state loadState(ymData, false);
		_ym.save_restore(loadState);
		_samples.clear();
	}

	SmsPsgState& psg = _psg.State;
	SV(psg.SelectedReg);
	SV(psg.Noise.Timer);
	SV(psg.Noise.Lfsr);
	SV(psg.Noise.LfsrInputBit);
	SV(psg.Noise.Control);
	SV(psg.Noise.Output);
	SV(psg.Noise.Volume);
	for(int i = 0; i < 3; i++) {
		SVI(psg.Tone[i].ReloadValue);
		SVI(psg.Tone[i].Timer);
		SVI(psg.Tone[i].Output);
		SVI(psg.Tone[i].Volume);
	}
	SV(_clock);
	SV(_psgClock);
	SVArray(_timerExpire, 2);
	SV(_busyEnd);
}
