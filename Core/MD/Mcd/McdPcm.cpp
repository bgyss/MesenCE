#include "pch.h"
#include "MD/Mcd/McdPcm.h"
#include "Utilities/Serializer.h"

uint8_t McdPcm::Read(uint16_t reg)
{
	reg &= 0x1FFF;
	if(reg >= 0x1000) {
		//Wave RAM window: 4 KiB of the 64 KiB, bank in the control register (WB3-0)
		return _ram[(_bank << 12) | (reg & 0xFFF)];
	} else if(reg >= 0x10 && reg < 0x20) {
		//Current wave RAM address of channel n: $10 + 2n = A7-A0, $11 + 2n = A15-A8
		uint16_t address = (uint16_t)(_channels[(reg >> 1) & 7].Address >> 11);
		return (reg & 1) ? (address >> 8) : (uint8_t)address;
	}
	//$00-$08 are write-only
	return 0xFF;
}

void McdPcm::Write(uint16_t reg, uint8_t value)
{
	reg &= 0x1FFF;
	if(reg >= 0x1000) {
		_ram[(_bank << 12) | (reg & 0xFFF)] = value;
		return;
	}
	Channel& ch = _channels[_channel];
	switch(reg) {
		case 0x00: ch.Envelope = value; break;
		case 0x01: ch.Pan = value; break;
		case 0x02: ch.Step = (ch.Step & 0xFF00) | value; break;
		case 0x03: ch.Step = (ch.Step & 0x00FF) | (value << 8); break;
		case 0x04: ch.Loop = (ch.Loop & 0xFF00) | value; break;
		case 0x05: ch.Loop = (ch.Loop & 0x00FF) | (value << 8); break;
		case 0x06:
			//ST: a channel that is not sounding restarts from here
			ch.Start = value;
			if(!ch.Enabled) {
				ch.Address = (uint32_t)value << 19;
			}
			break;

		case 0x07:
			//Control: bit 7 ON (the whole chip), bit 6 MOD: 1 = bits 2-0 select the channel whose
			//registers are written, 0 = bits 3-0 select the wave RAM bank
			if(value & 0x40) {
				_channel = value & 0x07;
			} else {
				_bank = value & 0x0F;
			}
			_soundOn = value & 0x80;
			break;

		case 0x08:
			//Channel on/off: bit n = channel n + 1, 0 = sounding. A stopped channel goes back to ST.
			for(int i = 0; i < 8; i++) {
				_channels[i].Enabled = !(value & (1 << i));
				if(!_channels[i].Enabled) {
					_channels[i].Address = (uint32_t)_channels[i].Start << 19;
				}
			}
			break;
	}
}

void McdPcm::Clock(uint64_t clock)
{
	int32_t left = 0;
	int32_t right = 0;
	if(_soundOn) {
		for(Channel& ch : _channels) {
			if(!ch.Enabled) {
				continue;
			}
			int32_t data = _ram[(ch.Address >> 11) & 0xFFFF];
			if(data == 0xFF) {
				//Loop marker: continue from LS
				ch.Address = (uint32_t)ch.Loop << 11;
				data = _ram[ch.Loop];
				if(data == 0xFF) {
					continue; //a loop onto a marker plays nothing
				}
			} else {
				ch.Address = (ch.Address + ch.Step) & 0x7FFFFFF;
			}
			data = (data & 0x80) ? (data & 0x7F) : -(data & 0x7F);
			left += (data * ch.Envelope * (ch.Pan & 0x0F)) >> 5;
			right += (data * ch.Envelope * (ch.Pan >> 4)) >> 5;
		}
	}
	//16-bit clamp, then the 10-bit DAC's resolution
	left = (std::clamp(left, -32768, 32767) >> 2) & ~15;
	right = (std::clamp(right, -32768, 32767) >> 2) & ~15;
	const Sample& last = _pending.empty() ? _current : _pending.back();
	if(left != last.Left || right != last.Right || _soundOn) {
		//(Silence while the chip is off and already silent changes nothing for the mixer)
		_pending.push_back({ clock, (int16_t)left, (int16_t)right });
	}
	if(_observer) {
		_observer((int16_t)left, (int16_t)right);
	}
}

void McdPcm::GetOutput(uint64_t clock, int32_t& left, int32_t& right)
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

void McdPcm::Serialize(Serializer& s)
{
	SVArray(_ram, 0x10000);
	for(int i = 0; i < 8; i++) {
		SVI(_channels[i].Envelope);
		SVI(_channels[i].Pan);
		SVI(_channels[i].Step);
		SVI(_channels[i].Loop);
		SVI(_channels[i].Start);
		SVI(_channels[i].Address);
		SVI(_channels[i].Enabled);
	}
	SV(_soundOn);
	SV(_bank);
	SV(_channel);
	SV(_current.Clock);
	SV(_current.Left);
	SV(_current.Right);
	uint32_t pendingCount = (uint32_t)_pending.size();
	SV(pendingCount);
	if(!s.IsSaving()) {
		_pending.resize(pendingCount);
	}
	for(uint32_t i = 0; i < pendingCount; i++) {
		SVI(_pending[i].Clock);
		SVI(_pending[i].Left);
		SVI(_pending[i].Right);
	}
}
