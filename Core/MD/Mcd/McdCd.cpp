#include "pch.h"
#include "MD/Mcd/McdCd.h"
#include "MD/Mcd/McdUnit.h"
#include "Utilities/Serializer.h"

void McdAudioQueue::Push(uint64_t clock, int16_t left, int16_t right)
{
	const McdTimedSample& last = _pending.empty() ? _current : _pending.back();
	if(left != last.Left || right != last.Right) {
		_pending.push_back({ clock, left, right });
	}
}

void McdAudioQueue::GetOutput(uint64_t clock, int32_t& left, int32_t& right)
{
	size_t used = 0;
	while(used < _pending.size() && _pending[used].Clock <= clock) {
		_current = _pending[used];
		used++;
	}
	if(used) {
		_pending.erase(_pending.begin(), _pending.begin() + used);
	}
	left = _current.Left;
	right = _current.Right;
}

void McdAudioQueue::Serialize(Serializer& s, const char* name)
{
	string prefix = name;
	s.Stream(_current.Clock, (prefix + ".currentClock").c_str());
	s.Stream(_current.Left, (prefix + ".currentLeft").c_str());
	s.Stream(_current.Right, (prefix + ".currentRight").c_str());
	uint32_t count = (uint32_t)_pending.size();
	s.Stream(count, (prefix + ".count").c_str());
	if(!s.IsSaving()) {
		_pending.resize(count);
	}
	for(uint32_t i = 0; i < count; i++) {
		s.Stream(_pending[i].Clock, (prefix + ".clock").c_str(), i);
		s.Stream(_pending[i].Left, (prefix + ".left").c_str(), i);
		s.Stream(_pending[i].Right, (prefix + ".right").c_str(), i);
	}
}

// ---------------------------------------------------------------------------------------------
// CDC

void McdCdc::UpdateIrq()
{
	bool pending = (_decoderIrq && (_ifctrl & 0x20)) || (_transferIrq && (_ifctrl & 0x40)) || (_commandIrq && (_ifctrl & 0x80));
	_unit->SetInterruptLine(5, pending);
}

void McdCdc::CompleteTransfer()
{
	_transfer.Active = false;
	_transfer.Busy = false;
	_transfer.Ready = false;
	_transfer.Completed = true;
	_transferIrq = true;
	UpdateIrq();
}

void McdCdc::StopTransfer()
{
	_transfer.Active = false;
	_transfer.Busy = false;
	_transfer.Ready = false;
}

uint8_t McdCdc::ReadRegister()
{
	uint8_t data = 0;
	switch(_address) {
		case 0x0: {
			//COMIN: nothing feeds the command FIFO on the Mega-CD
			if(_commandEmpty) {
				data = 0xFF;
				break;
			}
			data = _commandFifo[_commandRead++ & 7];
			if((_commandRead & 7) == (_commandWrite & 7)) {
				_commandEmpty = true;
				_commandIrq = false;
				UpdateIrq();
			}
			break;
		}
		case 0x1:
			//IFSTAT: active-low flags (STEN, DTEN, STBSY, DTBSY, -, DECI, DTEI, CMDI)
			data = 0x01 | (_transfer.Active ? 0 : 0x02) | 0x04 | (_transfer.Busy ? 0 : 0x08) | 0x10 | (_decoderIrq ? 0 : 0x20) | (_transferIrq ? 0 : 0x40) | (_commandIrq ? 0 : 0x80);
			break;
		case 0x2: data = (uint8_t)_transfer.Length; break;
		case 0x3: data = (_transfer.Length >> 8) & 0x0F; break;
		case 0x4:
		case 0x5:
		case 0x6:
		case 0x7: data = _headSelect ? _subheader[_address - 4] : _header[_address - 4]; break;
		case 0x8: data = (uint8_t)_transfer.Pointer; break;
		case 0x9: data = _transfer.Pointer >> 8; break;
		case 0xA: data = (uint8_t)_transfer.Target; break;
		case 0xB: data = _transfer.Target >> 8; break;
		case 0xC: data = _decoderEnable ? 0x80 : 0; break; //STAT0: CRCOK, no errors
		case 0xD: data = 0; break; //STAT1: no header errors
		case 0xE: data = (_form ? 0x04 : 0) | (_mode ? 0x08 : 0); break; //STAT2
		case 0xF:
			//STAT3: VALST (active low); reading it ends the decoder interrupt
			data = _decoderValid ? 0 : 0x80;
			_decoderValid = false;
			_decoderIrq = false;
			UpdateIrq();
			break;
	}
	//The address register increments after every access except to register 0
	if(_address) {
		_address = (_address + 1) & 0x0F;
	}
	return data;
}

void McdCdc::WriteRegister(uint8_t value)
{
	switch(_address) {
		case 0x0: break; //SBOUT: the status FIFO has no reader on the Mega-CD (not modelled)
		case 0x1:
			//IFCTRL: CMDIEN, DTEIEN, DECIEN, CMDBK, DTWAI, STWAI, DOUTEN, SOUTEN
			_ifctrl = value;
			_transfer.Enable = value & 0x02;
			if(!_transfer.Enable) {
				StopTransfer();
			}
			UpdateIrq();
			break;
		case 0x2: _transfer.Length = (_transfer.Length & 0xF00) | value; break;
		case 0x3: _transfer.Length = (_transfer.Length & 0x0FF) | ((value & 0x0F) << 8); break;
		case 0x4: _transfer.Source = (_transfer.Source & 0xFF00) | value; break;
		case 0x5: _transfer.Source = (_transfer.Source & 0x00FF) | (value << 8); break;
		case 0x6:
			//DTTRG: start a transfer
			if(_transfer.Enable) {
				_transfer.Active = true;
				_transfer.Busy = true;
				_transfer.Ready = _transfer.Destination == MainRead || _transfer.Destination == SubRead;
				_transfer.Completed = false;
				_transferIrq = false;
				UpdateIrq();
			}
			break;
		case 0x7:
			//DTACK
			_transferIrq = false;
			UpdateIrq();
			break;
		case 0x8: _transfer.Target = (_transfer.Target & 0xFF00) | value; break;
		case 0x9: _transfer.Target = (_transfer.Target & 0x00FF) | (value << 8); break;
		case 0xA:
			//CTRL0: DECEN (7), AUTORQ (4), WRRQ (2)
			_ctrl0 = value;
			_decoderEnable = value & 0x80;
			_autoCorrection = value & 0x10;
			_writeRequest = value & 0x04;
			break;
		case 0xB:
			//CTRL1: MODRQ (3), FORMRQ (2), SHDREN (0)
			_ctrl1 = value;
			_headSelect = value & 0x01;
			_mode = value & 0x08;
			_form = (value & 0x04) && _autoCorrection;
			break;
		case 0xC: _transfer.Pointer = (_transfer.Pointer & 0xFF00) | value; break;
		case 0xD: _transfer.Pointer = (_transfer.Pointer & 0x00FF) | (value << 8); break;
		case 0xE: _ctrl2 = value; break;
		case 0xF:
			//RESET
			_ifctrl = 0;
			_transfer.Enable = false;
			StopTransfer();
			_decoderIrq = _transferIrq = _commandIrq = false;
			_ctrl0 = _ctrl1 = _ctrl2 = 0;
			_decoderEnable = _writeRequest = _autoCorrection = _headSelect = _form = _mode = false;
			memset(_header, 0, sizeof(_header));
			memset(_subheader, 0, sizeof(_subheader));
			UpdateIrq();
			break;
	}
	if(_address) {
		_address = (_address + 1) & 0x0F;
	}
}

uint16_t McdCdc::ReadHostData()
{
	if(!_transfer.Ready) {
		return 0xFFFF;
	}
	uint16_t data = _ram[(_transfer.Source >> 1) & 0x1FFF];
	_transfer.Source += 2;
	//DBC counts bytes - 1; the transfer ends when it underflows (ares)
	for(int i = 0; i < 2; i++) {
		if(_transfer.Length == 0) {
			CompleteTransfer();
			_transfer.Length = 0xFFF;
		} else {
			_transfer.Length--;
		}
	}
	return data;
}

void McdCdc::DmaStep()
{
	if(!_transfer.Active || (_transfer.Destination != PcmDma && _transfer.Destination != PrgRamDma && _transfer.Destination != WordRamDma)) {
		return;
	}
	uint16_t data = _ram[(_transfer.Source >> 1) & 0x1FFF];
	_unit->CdcDmaWrite(_transfer.Destination, _transfer.Address, data);
	//PCM RAM takes the word as two bytes (ares): the address moves 4 per word
	_transfer.Address += _transfer.Destination == PcmDma ? 4 : 2;
	_transfer.Source += 2;
	for(int i = 0; i < 2; i++) {
		if(_transfer.Length == 0) {
			CompleteTransfer();
			_transfer.Length = 0xFFF;
		} else {
			_transfer.Length--;
		}
	}
}

void McdCdc::Decode(int32_t lba, const uint8_t* raw)
{
	if(!_decoderEnable) {
		return;
	}
	int32_t msf = lba + 150;
	auto bcd = [](int32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
	_header[0] = bcd(msf / 75 / 60);
	_header[1] = bcd(msf / 75 % 60);
	_header[2] = bcd(msf % 75);
	_header[3] = 0x01;
	_decoderValid = true;
	_decoderIrq = true;
	UpdateIrq();
	if(_writeRequest) {
		//The block pointer moves to the new sector's header; the 12 sync bytes go to its tail
		_transfer.Pointer += 2352;
		_transfer.Target += 2352;
		for(uint32_t i = 0; i < 12; i += 2) {
			_ram[((_transfer.Pointer + i + 2340) >> 1) & 0x1FFF] = (raw[i] << 8) | raw[i + 1];
		}
		for(uint32_t i = 0; i < 2340; i += 2) {
			_ram[((_transfer.Pointer + i) >> 1) & 0x1FFF] = (raw[12 + i] << 8) | raw[13 + i];
		}
	}
}

void McdCdc::Serialize(Serializer& s)
{
	SVArray(_ram, 0x2000);
	SV(_address);
	SV(_transfer.Destination);
	SV(_transfer.Address);
	SV(_transfer.Source);
	SV(_transfer.Length);
	SV(_transfer.Target);
	SV(_transfer.Pointer);
	SV(_transfer.Enable);
	SV(_transfer.Active);
	SV(_transfer.Busy);
	SV(_transfer.Ready);
	SV(_transfer.Completed);
	SV(_decoderEnable);
	SV(_decoderValid);
	SV(_writeRequest);
	SV(_autoCorrection);
	SV(_headSelect);
	SV(_form);
	SV(_mode);
	SV(_ctrl0);
	SV(_ctrl1);
	SV(_ctrl2);
	SV(_ifctrl);
	SVArray(_header, 4);
	SVArray(_subheader, 4);
	SV(_decoderIrq);
	SV(_transferIrq);
	SV(_commandIrq);
	SVArray(_commandFifo, 8);
	SV(_commandRead);
	SV(_commandWrite);
	SV(_commandEmpty);
}

// ---------------------------------------------------------------------------------------------
// CDD

void McdCdd::Init(McdUnit* unit, McdCdc* cdc, DiscInfo* disc)
{
	_unit = unit;
	_cdc = cdc;
	_disc = disc;
	if(_disc && !_disc->Tracks.empty()) {
		//Power-on with a disc: the drive reads the TOC from the lead-in (2 seconds, ares)
		_driveStatus = ReadingToc;
		_sector = -150;
	} else {
		_driveStatus = NoDisc;
		_sector = 0;
	}
	_track = 0;
	SetStatusChecksum();
}

int32_t McdCdd::LeadOutLba()
{
	return _disc ? (int32_t)_disc->DiscSectorCount : 0;
}

int32_t McdCdd::TrackStartLba(uint8_t track)
{
	if(!_disc || track == 0 || track > _disc->Tracks.size()) {
		return LeadOutLba();
	}
	return (int32_t)_disc->Tracks[track - 1].FirstSector;
}

bool McdCdd::IsDataTrack(uint8_t track)
{
	return _disc && track >= 1 && track <= _disc->Tracks.size() && _disc->Tracks[track - 1].Format != TrackFormat::Audio;
}

uint8_t McdCdd::TrackAt(int32_t lba)
{
	if(!_disc || _disc->Tracks.empty() || lba < -150 || lba >= LeadOutLba()) {
		return 0;
	}
	if(lba < 0) {
		//The 2-second pregap before 00:02:00 is track 1's (index 0); the BIOS reads into it when it
		//seeks a few sectors ahead of the boot sector
		return 1;
	}
	//A track's pregap (INDEX 00) belongs to it
	uint8_t found = 0;
	for(size_t i = 0; i < _disc->Tracks.size(); i++) {
		TrackInfo& trk = _disc->Tracks[i];
		uint32_t start = trk.HasLeadIn ? trk.LeadInPosition.ToLba() : trk.FirstSector;
		if((uint32_t)lba >= start) {
			found = (uint8_t)(i + 1);
		}
	}
	return found;
}

void McdCdd::ReadRawSector(int32_t lba, uint8_t* out)
{
	memset(out, 0, 2352);
	int32_t index = _disc->GetTrack((uint32_t)lba);
	if(index < 0) {
		return;
	}
	TrackInfo& trk = _disc->Tracks[index];
	VirtualFile& file = _disc->Files[trk.FileIndex];
	vector<uint8_t> data;
	if(trk.Format == TrackFormat::Mode1_2048) {
		//Cooked image: rebuild sync and header around the 2048 bytes (EDC/ECC left at 0)
		static constexpr uint8_t sync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
		memcpy(out, sync, 12);
		int32_t msf = lba + 150;
		auto bcd = [](int32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
		out[12] = bcd(msf / 75 / 60);
		out[13] = bcd(msf / 75 % 60);
		out[14] = bcd(msf % 75);
		out[15] = 0x01;
		if(file.ReadChunk(data, trk.FileOffset + (lba - trk.FirstSector) * 2048, 2048)) {
			memcpy(out + 16, data.data(), 2048);
		}
	} else if(file.ReadChunk(data, trk.FileOffset + (lba - trk.FirstSector) * 2352, 2352)) {
		memcpy(out, data.data(), 2352);
	}
}

double McdCdd::DiscPosition(int32_t lba)
{
	//Normalized radius of a sector on the disc, for the seek time (ares CDD::position)
	constexpr double sectors = 7500.0 + 330000.0 + 6750.0;
	constexpr double radius = 0.058 - 0.024;
	constexpr double innerRadius = 0.024 * 0.024;
	constexpr double outerRadius = 0.058 * 0.058;
	double sector = (double)(lba + 150);
	return std::sqrt(sector / sectors * (outerRadius - innerRadius) + innerRadius) / radius;
}

void McdCdd::SeekTo(int32_t lba, uint8_t after)
{
	_counter = 0;
	_seekTarget = after;
	//Seek time in 75 Hz steps: 11 plus a part proportional to the radial distance (ares)
	_latency = (uint32_t)(11 + 112.5 * std::abs(DiscPosition(_sector) - DiscPosition(lba)));
	_driveStatus = Seeking;
	_sector = lba;
	_sampleOffset = 0;
	for(int i = 1; i <= 8; i++) {
		_status[i] = 0;
	}
	_status[1] = 0xF;
}

void McdCdd::Advance()
{
	uint8_t track = TrackAt(_sector + 1);
	if(track) {
		_track = track;
		_sector++;
		_sampleOffset = 0;
	} else if(_sector + 1 < 0) {
		_driveStatus = LeadIn;
		_track = 0;
	} else {
		_driveStatus = LeadOut;
		_track = 0xAA;
	}
}

void McdCdd::Tick()
{
	//The MCU's steps run at 75 Hz: 434 counts of 384 sub cycles = 13.33 ms (BIOS Manual p. 5)
	if(++_counter < 434) {
		return;
	}
	_counter = 0;
	Step75Hz();
}

void McdCdd::Step75Hz()
{
	if(!_hostClock) {
		return;
	}
	if(_statusPending) {
		_statusPending = false;
		_unit->SetInterruptLine(4, true);
	}
	switch(_driveStatus) {
		case Stopped:
			_driveStatus = (_disc && !_disc->Tracks.empty()) ? ReadingToc : NoDisc;
			_sector = 0;
			_sampleOffset = 0;
			_track = 0;
			break;

		case ReadingToc:
			_sector++;
			if(_sector >= 0) {
				_driveStatus = Paused;
				_track = TrackAt(_sector);
			}
			break;

		case Playing:
			if(!TrackAt(_sector)) {
				//Outside the disc (a seek past the last sector): the drive reports the lead-out
				_driveStatus = _sector < 0 ? LeadIn : LeadOut;
				_track = _sector < 0 ? 0 : 0xAA;
				break;
			}
			FillSubcode();
			if(IsDataTrack(_track)) {
				uint8_t raw[2352];
				ReadRawSector(_sector, raw);
				_cdc->Decode(_sector, raw);
				Advance();
			}
			//Audio tracks advance with the 44.1 kHz samples
			break;

		case Seeking:
		case Tracking:
			if(_latency && --_latency) {
				break;
			}
			_driveStatus = _seekTarget;
			_track = TrackAt(_sector);
			break;
	}
}

void McdCdd::Advance(uint32_t subCycles, uint64_t masterClock)
{
	_sampleAccumulator += (uint64_t)subCycles * 44100;
	while(_sampleAccumulator >= McdUnit::SubClockRate) {
		_sampleAccumulator -= McdUnit::SubClockRate;
		PlayAudioSample(masterClock);
	}
}

void McdCdd::PlayAudioSample(uint64_t clock)
{
	int32_t left = 0;
	int32_t right = 0;
	if(_driveStatus == Playing && _track && !IsDataTrack(_track)) {
		uint32_t sample = _sampleOffset / 4;
		left = _disc->ReadLeftSample((uint32_t)_sector, sample);
		right = _disc->ReadRightSample((uint32_t)_sector, sample);
		_sampleOffset += 4;
		if(_sampleOffset >= 2352) {
			Advance();
		}
	}
	//Fader: volume FD/$400, moving one step per sample towards the setting (ares; the manual gives
	//22 us per step). Output at half scale, as ares (judgment call: no hardware level measured).
	left = (left * (int32_t)_attenuated / 0x400) >> 1;
	right = (right * (int32_t)_attenuated / 0x400) >> 1;
	if(_attenuated < _attenuator) {
		_attenuated++;
	} else if(_attenuated > _attenuator) {
		_attenuated--;
	}
	_output.Push(clock, (int16_t)left, (int16_t)right);
	if(_observer) {
		_observer((int16_t)left, (int16_t)right);
	}
}

void McdCdd::FillSubcode()
{
	//Subcode for the current sector: P = 0, Q = mode 1 position (control, track, index 1, relative
	//and absolute time, CRC), R-W = 0, packed into the gate array's buffer as ares does
	//(CDD::readSubcode). Judgment call: the disc's own subchannel is not used.
	uint8_t q[12] = {};
	auto bcd = [](int32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
	int32_t rel = std::max<int32_t>(_sector - TrackStartLba(_track), 0);
	int32_t abs = _sector + 150;
	q[0] = (IsDataTrack(_track) ? 0x40 : 0x00) | 0x01;
	q[1] = bcd(_track);
	q[2] = 0x01;
	q[3] = bcd(rel / 75 / 60);
	q[4] = bcd(rel / 75 % 60);
	q[5] = bcd(rel % 75);
	q[7] = bcd(abs / 75 / 60);
	q[8] = bcd(abs / 75 % 60);
	q[9] = bcd(abs % 75);
	uint16_t crc = 0;
	for(int i = 0; i < 10; i++) {
		crc ^= q[i] << 8;
		for(int b = 0; b < 8; b++) {
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
		}
	}
	crc = ~crc;
	q[10] = crc >> 8;
	q[11] = (uint8_t)crc;

	_subcodePosition = (_subcodePosition + 49) & 0x7F;
	uint8_t index = _subcodePosition & 0x3F;
	for(int i = 0; i < 96; i += 2) {
		uint16_t data = 0;
		//Channels P (0) to W (7); only Q has data
		uint8_t input = q[i / 8];
		uint8_t bits = input >> (6 - (i & 6));
		data |= (bits & 1) << (7 - 1);
		data |= ((bits >> 1) & 1) << (15 - 1);
		_subcode[index++ & 0x3F] = data;
	}
	_unit->SetInterruptLine(6, true);
}

void McdCdd::SetStatusChecksum()
{
	uint8_t sum = 0;
	for(int i = 0; i < 9; i++) {
		sum += _status[i];
	}
	_status[9] = ~sum & 0x0F;
}

void McdCdd::ProcessCommand()
{
	uint8_t sum = 0;
	for(int i = 0; i < 9; i++) {
		sum += _command[i];
	}
	auto digits = [this](int32_t value, int first) {
		_status[first] = (uint8_t)(value / 10);
		_status[first + 1] = (uint8_t)(value % 10);
	};
	auto msf = [&](int32_t lba, int first) {
		int32_t v = lba + 150;
		digits(v / 75 / 60, first);
		digits(v / 75 % 60, first + 2);
		digits(v % 75, first + 4);
	};

	if((~sum & 0x0F) != _command[9]) {
		_driveStatus = ChecksumError;
	} else {
		switch(_command[0]) {
			case 0x0:
				//Idle: a pending "not ready" report becomes the track number (ares: fixes Lunar)
				if(!_latency && _status[1] == 0xF) {
					_status[1] = 0x2;
					digits(_track, 2);
				}
				break;

			case 0x1:
				//Stop
				_driveStatus = (_disc && !_disc->Tracks.empty()) ? Stopped : NoDisc;
				for(int i = 1; i <= 8; i++) {
					_status[i] = 0;
				}
				break;

			case 0x2:
				//Report request: command[3] selects the report
				switch(_command[3]) {
					case 0x0: //absolute time
						_status[1] = 0x0;
						msf(_sector, 2);
						_status[8] = IsDataTrack(_track) ? 0x4 : 0;
						break;
					case 0x1: { //relative time
						_status[1] = 0x1;
						int32_t rel = _sector - TrackStartLba(_track);
						digits(std::abs(rel) / 75 / 60, 2);
						digits(std::abs(rel) / 75 % 60, 4);
						digits(std::abs(rel) % 75, 6);
						_status[8] = IsDataTrack(_track) ? 0x4 : 0;
						break;
					}
					case 0x2: //track number
						_status[1] = 0x2;
						//The current track; 00 in the lead-in (before the 2-second pregap), AA in the
						//lead-out (ares: inLeadIn / inLeadOut)
						digits(TrackAt(_sector), 2);
						if(_sector < -150) {
							_status[2] = _status[3] = 0;
						} else if(_sector >= LeadOutLba()) {
							_status[2] = _status[3] = 0xA;
						}
						for(int i = 4; i <= 8; i++) {
							_status[i] = 0;
						}
						break;
					case 0x3: //disc length (lead-out time)
						_status[1] = 0x3;
						msf(LeadOutLba(), 2);
						_status[8] = 0;
						break;
					case 0x4: //first and last track
						_status[1] = 0x4;
						digits(1, 2);
						digits(_disc ? (int32_t)_disc->Tracks.size() : 0, 4);
						_status[6] = _status[7] = _status[8] = 0;
						break;
					case 0x5: { //start time of track command[4..5]
						if(_command[4] > 9 || _command[5] > 9) {
							break;
						}
						uint8_t track = _command[4] * 10 + _command[5];
						_status[1] = 0x5;
						msf(TrackStartLba(track), 2);
						if(IsDataTrack(track)) {
							_status[6] |= 0x8;
						}
						_status[8] = track % 10;
						break;
					}
					case 0x6: //error information: none
						_status[1] = 0x6;
						for(int i = 2; i <= 8; i++) {
							_status[i] = 0;
						}
						break;
				}
				break;

			case 0x3:
			case 0x4: {
				//Seek, then play (3) or pause (4): MSF in command[2..7]; the drive lands 3 sectors early
				int32_t m = _command[2] * 10 + _command[3];
				int32_t sec = _command[4] * 10 + _command[5];
				int32_t f = _command[6] * 10 + _command[7];
				SeekTo((m * 60 + sec) * 75 + f - 150 - 3, _command[0] == 0x3 ? Playing : Paused);
				break;
			}

			case 0x6:
				//Pause
				if(_driveStatus == Seeking) {
					_seekTarget = Paused;
				} else {
					_driveStatus = Paused;
				}
				break;

			case 0x7:
				//Play
				if(_driveStatus == Seeking) {
					_seekTarget = Playing;
				} else {
					_driveStatus = Playing;
				}
				break;

			case 0xA:
				//Track skip: treated as a seek in place (ares: the MCU's algorithm is unknown)
				_counter = 0;
				_driveStatus = Tracking;
				_seekTarget = Paused;
				_latency = 0;
				_sampleOffset = 0;
				for(int i = 2; i <= 8; i++) {
					_status[i] = 0;
				}
				_status[1] = 0xF;
				break;

			default:
				_driveStatus = CommandError;
				break;
		}
	}
	_status[0] = _driveStatus;
	SetStatusChecksum();
	_statusPending = true;
}

uint16_t McdCdd::ReadStatusRegister(uint32_t reg)
{
	//$FF8038-$FF8041: status nibbles 0-9, two per word (bits 11-8 and 3-0)
	uint32_t index = (reg - 0x38) & 0x0E;
	return (_status[index] << 8) | _status[index + 1];
}

uint16_t McdCdd::ReadCommandRegister(uint32_t reg)
{
	uint32_t index = (reg - 0x42) & 0x0E;
	return (_command[index] << 8) | _command[index + 1];
}

void McdCdd::WriteCommandRegister(uint32_t reg, bool upper, bool lower, uint16_t value)
{
	uint32_t index = (reg - 0x42) & 0x0E;
	if(upper) {
		_command[index] = (value >> 8) & 0x0F;
	}
	if(lower) {
		_command[index + 1] = value & 0x0F;
		//Writing command nibble 9 sends the command (Hardware Manual p. 33)
		if(index + 1 == 9) {
			ProcessCommand();
		}
	}
}

uint16_t McdCdd::ReadControl()
{
	//$FF8036: D/M (bit 8: 1 = data, including stop and pause), HOCK (bit 2); DTS/DRS read 0
	bool data = !(_driveStatus == Playing && _track && !IsDataTrack(_track));
	return (data ? 0x0100 : 0) | (_hostClock ? 0x04 : 0);
}

void McdCdd::WriteControl(uint16_t value)
{
	bool hock = value & 0x04;
	if(hock && !_hostClock) {
		//Starting the host clock starts communication with the drive (Hardware Manual p. 32)
		_unit->SetInterruptLine(4, true);
	}
	_hostClock = hock;
	_counter = 0;
}

void McdCdd::WriteFader(uint16_t value)
{
	//FD10-FD0 in bits 14-4 (Hardware Manual p. 31); de-emphasis is stored, not applied
	_attenuator = (value >> 4) & 0x7FF;
	if(_attenuator > 0x400) {
		_attenuator = 0x400;
	}
	_deemphasis = value & 0x03;
}

void McdCdd::Serialize(Serializer& s)
{
	SVArray(_status, 10);
	SVArray(_command, 10);
	SV(_hostClock);
	SV(_statusPending);
	SV(_counter);
	SV(_driveStatus);
	SV(_seekTarget);
	SV(_latency);
	SV(_sector);
	SV(_sampleOffset);
	SV(_track);
	SV(_attenuator);
	SV(_attenuated);
	SV(_deemphasis);
	SVArray(_subcode, 64);
	SV(_subcodePosition);
	SV(_sampleAccumulator);
	_output.Serialize(s, "cdda");
}
