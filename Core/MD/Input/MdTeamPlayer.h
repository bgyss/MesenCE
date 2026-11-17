#pragma once
#include "pch.h"
#include "Shared/ControllerHub.h"
#include "MD/Input/MdController.h"
#include "MD/Input/MdPortDevice.h"
#include "Utilities/Serializer.h"

// Sega Team Player (Sega Tap) in "Multi" mode: four 3- or 6-button pads on one control port.
//
// Protocol (MAME, src/devices/bus/sms_ctrl/teamplayer.cpp, BSD-3-Clause; MAME notes that it is
// derived from what software expects). TH high: idle, the adaptor drives TL=1, TR=1, data 0011.
// TH falling selects it (the pads are sampled then) and it answers data 1111. Each TR edge then
// moves to the next nibble, with TL following TR (low on a TR falling edge, high on a rising one):
//   nibbles 0-1 0000 0000, then the four port IDs (0 = 3-button, 1 = 6-button, F = nothing),
//   then for each connected pad in order: RLDU, SACB, and MXYZ for a 6-button pad.
// Pad data is active low, as on a pad. The Sega Mouse behind a Team Player is not supported (MAME
// has the same gap).
class MdTeamPlayer : public ControllerHub<4>, public IMdPortDevice
{
private:
	enum Phase : uint8_t
	{
		Start = 0,
		Unused,
		Ident1,
		Ident2,
		PortA,
		PortB,
		PortC,
		PortD,
		Final
	};

	bool _th = true;
	bool _tr = true;
	uint8_t _out = 0x33;
	uint8_t _phase = Phase::Start;
	uint8_t _nibble = 0;
	uint8_t _ident[4] = { 0x0F, 0x0F, 0x0F, 0x0F };
	uint8_t _data[12] = {}; //per port: RLDU, SACB, MXYZ

	void Select()
	{
		//The pads are sampled when TH falls
		for(int i = 0; i < 4; i++) {
			MdController* pad = dynamic_cast<MdController*>(_ports[i].get());
			if(pad) {
				_ident[i] = pad->IsSixButtons() ? 1 : 0;
				for(int n = 0; n < 3; n++) {
					_data[i * 3 + n] = pad->GetNibble(n);
				}
			} else {
				_ident[i] = 0x0F;
			}
		}
		_out = 0x3F;
		_phase = Phase::Start;
		_nibble = 0;
	}

	void NextPort(uint8_t tl)
	{
		_phase++;
		_nibble = 0;
		while(_phase <= Phase::PortD && _ident[_phase - Phase::PortA] == 0x0F) {
			_phase++;
		}
		if(_phase <= Phase::PortD) {
			_out = 0x20 | tl | _data[(_phase - Phase::PortA) * 3];
		} else {
			_phase = Phase::Final;
			_out = 0x20 | tl;
		}
	}

	void NextNibble(uint8_t tl)
	{
		uint8_t port = _phase - Phase::PortA;
		uint8_t count = _ident[port] == 1 ? 2 : 1;
		if(_nibble < count) {
			_nibble++;
			_out = 0x20 | tl | _data[port * 3 + _nibble];
		} else {
			NextPort(tl);
		}
	}

	void TrFalling()
	{
		switch(_phase) {
			case Phase::Start:
				_out = 0x20;
				_phase = Phase::Unused;
				break;
			case Phase::Unused:
			case Phase::Ident1:
				_phase++;
				_out = 0x20 | _ident[(_phase - Phase::Ident1) * 2];
				break;
			case Phase::Ident2: NextPort(0x00); break;
			case Phase::PortA:
			case Phase::PortB:
			case Phase::PortC:
			case Phase::PortD: NextNibble(0x00); break;
			default: _out = 0x20; break;
		}
	}

	void TrRising()
	{
		switch(_phase) {
			case Phase::Unused: _out = 0x30; break;
			case Phase::Ident1:
			case Phase::Ident2: _out = 0x30 | _ident[(_phase - Phase::Ident1) * 2 + 1]; break;
			case Phase::PortA:
			case Phase::PortB:
			case Phase::PortC:
			case Phase::PortD: NextNibble(0x10); break;
			default: _out = 0x30; break;
		}
	}

protected:
	void Serialize(Serializer& s) override
	{
		ControllerHub<4>::Serialize(s);
		SV(_th);
		SV(_tr);
		SV(_out);
		SV(_phase);
		SV(_nibble);
		SVArray(_ident, 4);
		SVArray(_data, 12);
	}

public:
	MdTeamPlayer(Emulator* emu, uint8_t port, ControllerConfig controllers[]) : ControllerHub(emu, ControllerType::MdTeamPlayer, port, controllers)
	{
	}

	uint8_t ReadRam(uint16_t addr) override { return 0; }
	void WriteRam(uint16_t addr, uint8_t value) override {}

	void WritePins(uint8_t levels, uint8_t outputs, uint64_t clock) override
	{
		bool th = (outputs & 0x40) ? (levels & 0x40) : true;
		bool tr = (outputs & 0x20) ? (levels & 0x20) : true;
		if(th != _th) {
			_th = th;
			_tr = tr;
			if(th) {
				_out = 0x33;
				_phase = Phase::Start;
				_nibble = 0;
			} else {
				Select();
				if(!tr) {
					TrFalling();
				}
			}
		} else if(!_th && tr != _tr) {
			_tr = tr;
			if(tr) {
				TrRising();
			} else {
				TrFalling();
			}
		}
	}

	uint8_t ReadPins(uint64_t clock) override { return 0x40 | _out; }
};
