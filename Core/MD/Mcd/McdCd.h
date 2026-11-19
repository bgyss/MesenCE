#pragma once
#include "pch.h"
#include "Shared/CdReader.h"
#include "Utilities/ISerializable.h"

class McdUnit;

// A sample of an audio stream that the Mega Drive mixer picks up at its own sample times
struct McdTimedSample
{
	uint64_t Clock;
	int16_t Left;
	int16_t Right;
};

// Samples produced on the Sega CD's clock, taken by the mixer (MdApu) at master clock times:
// the latest sample produced at or before the time is the output (zero-order hold).
class McdAudioQueue
{
private:
	vector<McdTimedSample> _pending;
	McdTimedSample _current = {};

public:
	void Push(uint64_t clock, int16_t left, int16_t right);
	void GetOutput(uint64_t clock, int32_t& left, int32_t& right);
	void Serialize(Serializer& s, const char* name);
};

// Sanyo LC8951 CD data controller (the "CDC"): decodes sectors from the drive into its 16 KiB
// buffer and transfers them to a host (main or sub CPU reads through the gate array, or DMA to PCM
// RAM, program RAM or word RAM).
//
// Sources: Sanyo LC8950/LC8951 Design Manual (Sega Retro scan), section 11 / tables 7-8: register
// map, IFSTAT/IFCTRL, DBC, DAC, DTTRG, DTACK, WA, PT, CTRL0/1, STAT0-3, HEAD0-3, RESET. Mega-CD
// Hardware Manual pp. 26-27, 58: $FF8004/$A12004 (destination, DSR, EDT), host data, DMA address.
// ares md/mcd/cdc.cpp and cdc-transfer.cpp (ISC): an error-free decoder (STAT0 CRCOK, STAT3 VALST),
// the buffer layout (header first, sync at the tail), DMA of one word per 6 sub-CPU cycles and
// the transfer length bookkeeping.
class McdCdc final : public ISerializable
{
public:
	enum Destination : uint8_t
	{
		MainRead = 2,
		SubRead = 3,
		PcmDma = 4,
		PrgRamDma = 5,
		WordRamDma = 7,
	};

	struct Transfer
	{
		uint8_t Destination = 0;
		uint32_t Address = 0; //DMA destination address (bytes)
		uint16_t Source = 0; //DAC: buffer read address
		uint16_t Length = 0; //DBC: bytes - 1 (12 bits)
		uint16_t Target = 0; //WA: buffer write address
		uint16_t Pointer = 0; //PT: block pointer (header of the latest sector)
		bool Enable = false; //DOUTEN
		bool Active = false;
		bool Busy = false;
		bool Ready = false; //DSR
		bool Completed = false; //EDT
	};

private:
	McdUnit* _unit = nullptr;
	uint16_t _ram[0x2000] = {};
	uint8_t _address = 0;

	Transfer _transfer;
	bool _decoderEnable = false;
	bool _decoderValid = false;
	bool _writeRequest = false; //WRRQ
	bool _autoCorrection = false;
	bool _headSelect = false; //CTRL1 SHDREN: HEAD0-3 show the subheader
	bool _form = false;
	bool _mode = false;
	uint8_t _ctrl0 = 0;
	uint8_t _ctrl1 = 0;
	uint8_t _ctrl2 = 0;
	uint8_t _ifctrl = 0;
	uint8_t _header[4] = {};
	uint8_t _subheader[4] = {};
	bool _decoderIrq = false;
	bool _transferIrq = false;
	bool _commandIrq = false;
	uint8_t _commandFifo[8] = {};
	uint8_t _commandRead = 0;
	uint8_t _commandWrite = 0;
	bool _commandEmpty = true;

	void UpdateIrq();
	void CompleteTransfer();
	void StopTransfer();

public:
	void Init(McdUnit* unit) { _unit = unit; }

	uint8_t GetAddress() { return _address; }
	void SetAddress(uint8_t address) { _address = address & 0x0F; }
	uint8_t ReadRegister();
	void WriteRegister(uint8_t value);

	Transfer& GetTransfer() { return _transfer; }
	void SetDestination(uint8_t destination) { _transfer.Destination = destination & 0x07; }
	void SetDmaAddress(uint16_t value) { _transfer.Address = (uint32_t)value << 3; }
	//Host data ($FF8008 / $A12008): the next buffer word, for CPU-read destinations
	uint16_t ReadHostData();
	//One DMA word (every 6 sub-CPU cycles)
	void DmaStep();

	//The drive delivers sector `lba` (2352 raw bytes)
	void Decode(int32_t lba, const uint8_t* raw);

	void Serialize(Serializer& s) override;
};

// The CD drive's controller as the gate array sees it (a NEC uPD75006 4-bit MCU; "CDD"): commands
// and status as 10 nibbles each, a status interrupt (level 4) at 75 Hz, seeking, playing data
// sectors into the CDC and audio samples into the mixer, the subcode buffer (level 6), and the
// fader. High-level emulation of the MCU's behaviour.
//
// Sources: the Mega-CD Hardware Manual documents only the gate array side (pp. 31-33: $FF8034
// fader, $FF8036 HOCK/DM, status/command registers; the protocol itself is blacked out, "direct
// access prohibited"); BIOS Manual p. 5 (13.3 ms CDD interrupt), p. 20-21 (status codes and
// report types as the BIOS presents them). The command/status protocol, report formats, seek
// latency and TOC timing follow ares md/mcd/cdd.cpp (ISC); MAME src/mame/shared/megacdcd.cpp
// (BSD-3) implements the same command set and checksum.
class McdCdd final : public ISerializable
{
public:
	enum Status : uint8_t
	{
		Stopped = 0x0,
		Playing = 0x1,
		Seeking = 0x2,
		Paused = 0x4,
		DoorOpened = 0x5,
		ChecksumError = 0x6,
		CommandError = 0x7,
		ReadingToc = 0x9,
		Tracking = 0xA,
		NoDisc = 0xB,
		LeadOut = 0xC,
		LeadIn = 0xD,
	};

private:
	McdUnit* _unit = nullptr;
	McdCdc* _cdc = nullptr;
	DiscInfo* _disc = nullptr;

	uint8_t _status[10] = {};
	uint8_t _command[10] = {};
	bool _hostClock = false; //HOCK
	bool _statusPending = false;
	uint16_t _counter = 0; //384-cycle ticks towards the next 75 Hz step

	uint8_t _driveStatus = NoDisc;
	uint8_t _seekTarget = Paused; //status after the seek
	uint32_t _latency = 0; //75 Hz steps left in a seek
	int32_t _sector = 0; //LBA (0 = 00:02:00)
	uint32_t _sampleOffset = 0; //byte offset of the next audio sample in the sector
	uint8_t _track = 0; //track number (1-based), 0 = lead-in, 0xAA = lead-out

	//Fader ($FF8034): FD10-FD0 = bits 14-4, $400 = full volume
	uint16_t _attenuator = 0x400;
	uint16_t _attenuated = 0x400;
	uint8_t _deemphasis = 0;

	//Subcode buffer ($FF8100-$FF817F) and its position
	uint16_t _subcode[64] = {};
	uint8_t _subcodePosition = 0;

	//44.1 kHz sample timing from the 12.5 MHz clock: sub cycles x 44100, remainder kept
	uint64_t _sampleAccumulator = 0;
	McdAudioQueue _output;
	std::function<void(int16_t, int16_t)> _observer;

	int32_t LeadOutLba();
	int32_t TrackStartLba(uint8_t track);
	bool IsDataTrack(uint8_t track);
	uint8_t TrackAt(int32_t lba); //0 if none
	void ReadRawSector(int32_t lba, uint8_t* out);
	double DiscPosition(int32_t lba);
	void Advance();
	void Step75Hz();
	void PlayAudioSample(uint64_t clock);
	void FillSubcode();
	void ProcessCommand();
	void SetStatusChecksum();
	void SeekTo(int32_t lba, uint8_t after);

public:
	void Init(McdUnit* unit, McdCdc* cdc, DiscInfo* disc);

	//Every 384 sub-CPU cycles
	void Tick();
	//Sub-CPU cycles elapsed (for the 44.1 kHz audio), with the master clock now
	void Advance(uint32_t subCycles, uint64_t masterClock);

	uint16_t ReadStatusRegister(uint32_t reg);
	uint16_t ReadCommandRegister(uint32_t reg);
	void WriteCommandRegister(uint32_t reg, bool upper, bool lower, uint16_t value);
	uint16_t ReadControl();
	void WriteControl(uint16_t value);
	void WriteFader(uint16_t value);
	uint16_t ReadSubcode(uint32_t index) { return _subcode[index & 0x3F]; }
	uint8_t GetSubcodePosition() { return _subcodePosition; }

	McdAudioQueue& GetOutput() { return _output; }
	void SetObserver(std::function<void(int16_t, int16_t)> observer) { _observer = observer; }

	void Serialize(Serializer& s) override;
};
