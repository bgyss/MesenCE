#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "Shared/Audio/Sn76489.h"
#include "Utilities/Audio/ymfm/ymfm_opn.h"
#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;

// Mega Drive sound: YM2612 (ymfm::ym2612, clocked at MCLK/7, one sample per 144 YM clocks =
// 1008 master clocks) and the VDP's SN76489-compatible PSG (MCLK/15, one tone step per 16 PSG
// clocks = 240 master clocks). Both run on the master clock; output is one stereo stream at the
// YM2612 sample rate with the PSG averaged over each sample period.
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

	vector<int16_t> _samples;
	std::function<void(int32_t ymL, int32_t ymR, int32_t psgL, int32_t psgR)> _chipObserver;
	uint32_t _sampleRate = 53267;

	void ProcessTimers(uint64_t until);
	void GenerateSample();

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
	uint32_t GetSampleRate() { return _sampleRate; }

	//Optional per-sample observer of the two chips before mixing (headless capture)
	void SetChipObserver(std::function<void(int32_t, int32_t, int32_t, int32_t)> observer) { _chipObserver = observer; }

	//ymfm_interface
	void ymfm_set_timer(uint32_t tnum, int32_t durationInClocks) override;
	void ymfm_set_busy_end(uint32_t clocks) override;
	bool ymfm_is_busy() override;

	void Serialize(Serializer& s) override;
};
