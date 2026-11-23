#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "Shared/Audio/Sn76489.h"
#include "Utilities/Audio/ymfm/ymfm_opn.h"
#include "Utilities/Audio/NukedOpn2/ym3438.h"
#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;
class McdUnit;
class M32xUnit;

// Mega Drive sound: YM2612 (ymfm::ym2612, clocked at MCLK/7, one sample per 144 YM clocks =
// 1008 master clocks) and the VDP's SN76489-compatible PSG (MCLK/15, one tone step per 16 PSG
// clocks = 240 master clocks). Both run on the master clock; output is one stereo stream at the
// YM2612 sample rate with the PSG averaged over each sample period.
// The FM core is selectable (MdConfig::FmCore): ymfm applies register writes between samples;
// Nuked-OPN2 (die-shot model) is clocked per internal cycle (6 YM clocks = 42 master clocks, 24
// cycles per sample), so writes take effect at their exact cycle, and its timers, busy flag and
// status reads are the chip's own. Nuked costs about 10x ymfm's CPU time.
class MdApu final : public ymfm::ymfm_interface, public ISerializable
{
public:
	static constexpr uint32_t ClocksPerYmSample = 7 * 144;
	static constexpr uint32_t ClocksPerPsgStep = 15 * 16;

private:
	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;
	ymfm::ym2612 _ym;
	Sn76489 _psg;

	uint64_t _clock = 0; //master clock of the next YM sample boundary
	uint64_t _psgClock = 0; //master clock of the next PSG step
	uint64_t _now = 0; //time of the access being processed (for ymfm timer/busy callbacks)
	uint64_t _timerExpire[2] = {}; //0 = stopped
	uint64_t _busyEnd = 0;

	//Nuked-OPN2 core (allocated when selected)
	static constexpr uint32_t NukedCycleClocks = 42;
	static constexpr int32_t NukedGain = 7; //sum of 24 9-bit slot outputs -> about ymfm's level (approximate)
	unique_ptr<ym3438_t> _nuked;
	uint64_t _nukedClock = 0; //master clock of the next internal cycle
	uint8_t _nukedSlot = 0;
	int32_t _nukedAcc[2] = {};

	vector<int16_t> _samples;
	//Sega CD PCM and CD audio, mixed in when the unit is attached
	McdUnit* _mcd = nullptr;
	M32xUnit* _m32x = nullptr;
	std::function<void(int32_t ymL, int32_t ymR, int32_t psgL, int32_t psgR)> _chipObserver;
	std::function<void(int32_t pwmL, int32_t pwmR)> _pwmObserver;
	uint32_t _sampleRate = 53267;
	//32X output stage: AC coupling, a first-order DC blocker on the mix (fixed point, so the output
	//is the same on every architecture). The corner is rate / (2 pi 2^DcShift): about 16.6 Hz
	static constexpr int DcShift = 9;
	int32_t _dcIn[2] = {};
	int64_t _dcAcc[2] = {};
	int32_t DcBlock(int ch, int32_t x);

	void ProcessTimers(uint64_t until);
	void ComputePsg(int32_t& psgLeft, int32_t& psgRight);
	void GenerateSample();
	void FinishSample(int32_t ymLeft, int32_t ymRight);
	void RunNuked(uint64_t masterClock);
	void SelectCore(bool nuked);

public:
	MdApu();

	void Init(Emulator* emu, MdConsole* console, bool isPal);

	void Run(uint64_t masterClock);
	void EndFrame(uint64_t masterClock);

	uint8_t ReadYm(uint8_t port);
	void WriteYm(uint8_t port, uint8_t value);
	void ResetYm();
	void WritePsg(uint8_t value);

	SmsPsgState& GetPsgState() { return _psg.State; }
	bool IsNukedCore() { return _nuked != nullptr; }
	uint32_t GetSampleRate() { return _sampleRate; }
	void SetMcd(McdUnit* mcd) { _mcd = mcd; }
	void SetM32x(M32xUnit* m32x) { _m32x = m32x; }

	//Optional per-sample observer of the two chips before mixing (headless capture)
	void SetChipObserver(std::function<void(int32_t, int32_t, int32_t, int32_t)> observer) { _chipObserver = observer; }
	//32X PWM output as mixed, one call per YM2612 sample (test hook, like the chip observer)
	void SetPwmObserver(std::function<void(int32_t, int32_t)> observer) { _pwmObserver = observer; }

	//ymfm_interface
	void ymfm_set_timer(uint32_t tnum, int32_t durationInClocks) override;
	void ymfm_set_busy_end(uint32_t clocks) override;
	bool ymfm_is_busy() override;

	void Serialize(Serializer& s) override;
};
