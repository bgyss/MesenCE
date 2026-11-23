#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/KeyManager.h"
#include "MD/Input/MdPortDevice.h"
#include "Utilities/Serializer.h"

// Sega Mouse / Mega Mouse.
//
// Protocol (MAME, src/devices/bus/sms_ctrl/mouse.cpp, BSD-3-Clause; also ares'
// md/controller/mega-mouse, ISC). TH high: idle, the mouse drives TL=1, TR=1, data 0000. TH falling
// selects it: data 1011 (the ID), TL=1. While TH is low, every TR edge asks for the next nibble:
// about 7.2 us after the edge the data lines change, about 4.8 us later TL follows TR (games wait
// for TL before reading). The nibbles after the ID, one per TR edge:
//   1111 (movement latched at this edge), 1111, flags (bit 3 Y overflow, 2 X overflow, 1 Y sign,
//   0 X sign), buttons (bit 3 Start, 2 middle, 1 right, 0 left; active high), X high, X low,
//   Y high, Y low.
// X grows to the right and Y grows upwards; each axis is a 9-bit two's complement count.
class MdMouse : public BaseControlDevice, public IMdPortDevice
{
private:
	//7.2 us and 12 us at the 53.69 MHz master clock
	static constexpr uint64_t DataDelay = 387;
	static constexpr uint64_t HandshakeDelay = 644;

	bool _th = true;
	bool _tr = true;
	uint8_t _step = 0; //nibbles sent since TH fell
	uint8_t _data = 0;
	bool _tl = true;
	uint8_t _pendingData = 0;
	bool _pendingTl = true;
	uint64_t _edgeClock = 0;
	bool _pending = false;
	int16_t _dx = 0;
	int16_t _dy = 0;

	void Settle(uint64_t clock)
	{
		if(_pending && clock >= _edgeClock + DataDelay) {
			_data = _pendingData;
			if(clock >= _edgeClock + HandshakeDelay) {
				_tl = _pendingTl;
				_pending = false;
			}
		}
	}

	uint8_t GetNibble(uint8_t step)
	{
		switch(step) {
			case 1:
			case 2: return 0x0F;
			case 3:
				//Counts are clamped to 9 bits, so the overflow flags stay clear
				return (_dy < 0 ? 0x02 : 0) | (_dx < 0 ? 0x01 : 0);
			case 4:
				return (IsPressed(Buttons::Start) ? 0x08 : 0) | (IsPressed(Buttons::Middle) ? 0x04 : 0) | (IsPressed(Buttons::Right) ? 0x02 : 0) | (IsPressed(Buttons::Left) ? 0x01 : 0);
			case 5: return (_dx >> 4) & 0x0F;
			case 6: return _dx & 0x0F;
			case 7: return (_dy >> 4) & 0x0F;
			default: return _dy & 0x0F;
		}
	}

	void OnTrEdge(bool tr, uint64_t clock)
	{
		Settle(UINT64_MAX); //a new edge completes the previous handshake
		//After the eighth nibble the sequence starts over (MAME)
		_step = (_step % 8) + 1;
		if(_step == 1) {
			//Movement since the previous read, clamped to the 9-bit two's complement range
			MouseMovement mov = GetMovement();
			_dx = (int16_t)std::clamp<int32_t>(mov.dx, -256, 255);
			_dy = (int16_t)std::clamp<int32_t>(-mov.dy, -256, 255);
		}
		_pendingData = GetNibble(_step);
		_pendingTl = tr;
		_edgeClock = clock;
		_pending = true;
	}

protected:
	string GetKeyNames() override { return "LRMS"; }
	bool HasCoordinates() override { return true; }

	void InternalSetStateFromInput() override
	{
		for(KeyMapping& keyMapping : _keyMappings) {
			SetPressedState(Buttons::Left, KeyManager::IsKeyPressed(keyMapping.CustomKeys[0]));
			SetPressedState(Buttons::Right, KeyManager::IsKeyPressed(keyMapping.CustomKeys[1]));
			SetPressedState(Buttons::Middle, KeyManager::IsKeyPressed(keyMapping.CustomKeys[2]));
			SetPressedState(Buttons::Start, KeyManager::IsKeyPressed(keyMapping.CustomKeys[3]));
		}
		SetMovement(KeyManager::GetMouseMovement(_emu, _emu->GetSettings()->GetInputConfig().MouseSensitivity));
	}

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_th);
		SV(_tr);
		SV(_step);
		SV(_data);
		SV(_tl);
		SV(_pendingData);
		SV(_pendingTl);
		SV(_edgeClock);
		SV(_pending);
		SV(_dx);
		SV(_dy);
	}

public:
	enum Buttons
	{
		Left = 0,
		Right,
		Middle,
		Start
	};

	MdMouse(Emulator* emu, uint8_t port, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::MdMouse, port, keyMappings)
	{
	}

	//Lua names; x/y set the movement since the last read (as GetMovement consumes it)
	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "x", BaseControlDevice::DeviceXCoordButtonId, true },
			{ "y", BaseControlDevice::DeviceYCoordButtonId, true },
			{ "left", Buttons::Left },
			{ "right", Buttons::Right },
			{ "middle", Buttons::Middle },
			{ "start", Buttons::Start },
		};
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
			_pending = false;
			_step = 0;
			_tl = true;
			_data = th ? 0x00 : 0x0B;
			if(!th && !tr) {
				OnTrEdge(false, clock);
			}
		} else if(!_th && tr != _tr) {
			_tr = tr;
			OnTrEdge(tr, clock);
		}
	}

	uint8_t ReadPins(uint64_t clock) override
	{
		Settle(clock);
		return 0x60 | (_tl ? 0x10 : 0) | _data;
	}
};
