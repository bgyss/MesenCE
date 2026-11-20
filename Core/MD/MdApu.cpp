#include "pch.h"
#include "MD/MdApu.h"
#include "MD/MdConsole.h"
#include "MD/Mcd/McdUnit.h"
#include "MD/M32x/M32xUnit.h"
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
	SelectCore(_emu->GetSettings()->GetMdConfig().FmCore == MdFmCore::Nuked);
	_samples.reserve(2048);
}

void MdApu::SelectCore(bool nuked)
{
	if(nuked) {
		if(!_nuked) {
			_nuked.reset(new ym3438_t());
		}
		//MD1 VA3/VA6: discrete YM2612 (status only on port 0, DAC ladder effect)
		OPN2_SetChipType(ym3438_mode_ym2612);
		OPN2_Reset(_nuked.get());
		_nukedClock = _clock;
		_nukedSlot = 0;
		_nukedAcc[0] = _nukedAcc[1] = 0;
	} else {
		_nuked.reset();
	}
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

void MdApu::ComputePsg(int32_t& psgLeft, int32_t& psgRight)
{
	//PSG steps within this sample period, averaged (a box filter)
	psgLeft = 0;
	psgRight = 0;
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
}

void MdApu::GenerateSample()
{
	ymfm::ym2612::output_data output;
	_ym.generate(&output, 1);
	FinishSample(output.data[0], output.data[1]);
}

void MdApu::FinishSample(int32_t ymLeft, int32_t ymRight)
{
	int32_t psgLeft, psgRight;
	ComputePsg(psgLeft, psgRight);
	if(_chipObserver) {
		_chipObserver(ymLeft, ymRight, psgLeft, psgRight);
	}

	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();
	//The PSG sits about 6 dB below a full-scale FM channel mix (approximate, not hardware-qualified)
	int32_t left = ymLeft * (int32_t)cfg.FmVolume / 100 + psgLeft * (int32_t)cfg.PsgVolume / 200;
	int32_t right = ymRight * (int32_t)cfg.FmVolume / 100 + psgRight * (int32_t)cfg.PsgVolume / 200;
	if(_mcd) {
		//Sega CD PCM (DAC level) and CD audio (fader, half scale), unscaled (judgment call: no
		//hardware mix levels measured yet)
		int32_t mcdLeft, mcdRight;
		_mcd->GetAudioOutput(_clock, mcdLeft, mcdRight);
		left += mcdLeft;
		right += mcdRight;
	}
	if(_m32x) {
		//32X PWM, at half scale (judgment call: no hardware mix level measured)
		int32_t pwmLeft, pwmRight;
		_m32x->GetAudioOutput(_clock, pwmLeft, pwmRight);
		if(_pwmObserver) {
			_pwmObserver(pwmLeft, pwmRight);
		}
		left += pwmLeft;
		right += pwmRight;
	}
	_samples.push_back((int16_t)std::clamp(left, -32768, 32767));
	_samples.push_back((int16_t)std::clamp(right, -32768, 32767));
	_clock += ClocksPerYmSample;
}

void MdApu::RunNuked(uint64_t masterClock)
{
	ym3438_t* chip = _nuked.get();
	while(_nukedClock + NukedCycleClocks <= masterClock) {
		Bit16s out[2];
		OPN2_Clock(chip, out);
		_nukedAcc[0] += out[0];
		_nukedAcc[1] += out[1];
		_nukedClock += NukedCycleClocks;
		if(++_nukedSlot == 24) {
			//One sample: the sum of the 24 time-multiplexed DAC outputs
			int32_t left = _nukedAcc[0] * NukedGain;
			int32_t right = _nukedAcc[1] * NukedGain;
			_nukedSlot = 0;
			_nukedAcc[0] = _nukedAcc[1] = 0;
			FinishSample(left, right);
		}
	}
}

void MdApu::Run(uint64_t masterClock)
{
	if(_nuked) {
		RunNuked(masterClock);
		return;
	}
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
	if(_nuked) {
		//Discrete YM2612: port 0 returns the status; ports 1-3 return the last latched value
		return OPN2_Read(_nuked.get(), port & 0x03);
	}
	//ymfm: the status on all four ports (Sega Genesis Software Manual, "FM sound")
	return _ym.read_status();
}

void MdApu::WriteYm(uint8_t port, uint8_t value)
{
	if(_nuked) {
		//Takes effect on the chip's next internal cycles (writes while busy behave as on the die)
		OPN2_Write(_nuked.get(), port & 0x03, value);
		return;
	}
	_ym.write(port & 0x03, value);
}

void MdApu::ResetYm()
{
	if(_nuked) {
		OPN2_Reset(_nuked.get());
	}
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
	//The state carries its FM core; loading switches to it
	bool nukedCore = _nuked != nullptr;
	SV(nukedCore);
	if(!s.IsSaving() && nukedCore != (_nuked != nullptr)) {
		if(nukedCore) {
			_nuked.reset(new ym3438_t());
			OPN2_SetChipType(ym3438_mode_ym2612);
		} else {
			_nuked.reset();
		}
	}
	if(_nuked) {
		//Plain integer arrays, no pointers (Nuked-OPN2's ym3438_t)
		uint8_t* nukedChip = (uint8_t*)_nuked.get();
		SVArray(nukedChip, (uint32_t)sizeof(ym3438_t));
		SV(_nukedClock);
		SV(_nukedSlot);
		SVArray(_nukedAcc, 2);
	}

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
