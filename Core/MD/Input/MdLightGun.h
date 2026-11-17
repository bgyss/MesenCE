#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/Emulator.h"
#include "Shared/KeyManager.h"
#include "Shared/InputHud.h"
#include "MD/Input/MdPortDevice.h"
#include "Utilities/Serializer.h"

// Sega Menacer and Konami Justifier ("blue" gun only).
//
// Source: Eke-Eke, "Sega Genesis Lightguns" (2008), notes from disassembling the gun games (the
// author lists them as untested on hardware). A gun pulls TH low when its sensor sees the beam;
// with the port's control register bit 7 set, that edge is the VDP's HL input: a level 2
// interrupt when VDP register 11 bit 3 is set, and an H/V counter latch when register 0 bit 1 is
// set (MdVdp::TriggerExternalInterrupt). Here the gun "sees" the beam at the mouse position,
// whenever the position is on screen; TH goes low there plus a fixed latency, and stays low for
// the rest of that line.
//
// Menacer: data bits 3-0 are Start, C, A, B, active high; TR/TL are commands to the receiver
// (not needed here). Latency: 1344 master clocks (about 165 H40 pixels), which latches H counter $52
// for pixel 0 in H40 as the Menacer 6-in-1 cartridge expects (Eke-Eke: 6-in-1 offset $52, Body
// Count $48, T2 $84 without latching) - a judgment call; other games may be a few pixels off.
//
// Justifier: TR selects the gun (0 = blue), TL = 0 enables it. With TH as an output (device ID
// probe): TH=1 reads 1110000, TH=0 reads 01100 + Start, Trigger. With TH as an input: TL and TR
// read 1, bits 3-2 read 0, bit 1 Start and bit 0 Trigger (active low). Latency 0 (Lethal Enforcers
// uses H counter $00 for the left edge; Lethal Enforcers II adds $18 and offers calibration).
class MdLightGun : public BaseControlDevice, public IMdPortDevice
{
private:
	bool _justifier = false;
	uint8_t _levels = 0x7F;
	uint8_t _outputs = 0;
	uint64_t _thLowStart = 0;
	uint64_t _thLowEnd = 0;

	bool IsThLow(uint64_t clock) { return clock >= _thLowStart && clock < _thLowEnd; }
	bool IsBlueGunEnabled() { return !(_levels & 0x20) && !(_levels & 0x10); }

protected:
	string GetKeyNames() override { return _justifier ? "TS" : "TABCS"; }
	bool HasCoordinates() override { return true; }

	void InternalSetStateFromInput() override
	{
		for(KeyMapping& keyMapping : _keyMappings) {
			SetPressedState(Buttons::Trigger, KeyManager::IsKeyPressed(keyMapping.CustomKeys[0]));
			SetPressedState(Buttons::Start, KeyManager::IsKeyPressed(keyMapping.CustomKeys[1]));
			if(!_justifier) {
				SetPressedState(Buttons::A, KeyManager::IsKeyPressed(keyMapping.CustomKeys[2]));
				SetPressedState(Buttons::C, KeyManager::IsKeyPressed(keyMapping.CustomKeys[3]));
			}
		}
		SetCoordinates(KeyManager::GetMousePosition());
	}

	void InternalDrawController(InputHud& hud) override
	{
		hud.DrawOutline(11, 14);
		hud.DrawButton(1, 1, 3, 5, IsPressed(Buttons::Trigger));
		hud.DrawButton(5, 1, 2, 4, IsPressed(Buttons::Start));
		hud.DrawMousePosition(GetCoordinates());
	}

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_levels);
		SV(_outputs);
		SV(_thLowStart);
		SV(_thLowEnd);
	}

public:
	enum Buttons
	{
		Trigger = 0, //Menacer: B (the trigger)
		Start,
		A,
		C
	};

	MdLightGun(Emulator* emu, uint8_t port, KeyMappingSet keyMappings, bool justifier)
		: BaseControlDevice(emu, justifier ? ControllerType::MdJustifier : ControllerType::MdMenacer, port, keyMappings)
	{
		_justifier = justifier;
	}

	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "x", BaseControlDevice::DeviceXCoordButtonId, true },
			{ "y", BaseControlDevice::DeviceYCoordButtonId, true },
			{ "trigger", Buttons::Trigger },
			{ "start", Buttons::Start },
			{ "a", Buttons::A },
			{ "c", Buttons::C },
		};
	}

	uint8_t ReadRam(uint16_t addr) override { return 0; }
	void WriteRam(uint16_t addr, uint8_t value) override {}

	void WritePins(uint8_t levels, uint8_t outputs, uint64_t clock) override
	{
		_levels = levels;
		_outputs = outputs;
	}

	uint8_t ReadPins(uint64_t clock) override
	{
		uint8_t th = IsThLow(clock) ? 0 : 0x40;
		if(!_justifier) {
			return th | 0x30 | (IsPressed(Buttons::Start) ? 0x08 : 0) | (IsPressed(Buttons::C) ? 0x04 : 0) | (IsPressed(Buttons::A) ? 0x02 : 0) | (IsPressed(Buttons::Trigger) ? 0x01 : 0);
		}

		uint8_t buttons = 0x03;
		if(!(_levels & 0x20)) {
			//Only the blue gun is connected; the pink gun reads as released
			buttons = (IsPressed(Buttons::Start) ? 0 : 0x02) | (IsPressed(Buttons::Trigger) ? 0 : 0x01);
		}
		if(_outputs & 0x40) {
			//Device ID probe (ID 1): TH=1 -> 1110000, TH=0 -> 01100xx
			return (_levels & 0x40) ? 0x70 : (0x30 | buttons);
		}
		return th | 0x30 | buttons;
	}

	int32_t GetLightgunX(uint16_t line) override
	{
		if(_justifier && !IsBlueGunEnabled()) {
			return -1;
		}
		MousePosition pos = GetCoordinates();
		if(pos.X < 0 || pos.Y < 0 || pos.Y != (int16_t)line) {
			return -1;
		}
		return pos.X;
	}

	uint32_t GetLightgunLatency(bool h40) override
	{
		if(_justifier) {
			return 0;
		}
		//1344 master clocks: pixel 0 latches H counter $52 in H40, the Menacer 6-in-1 offset
		return 1344;
	}

	void OnLightgunHit(uint64_t clock) override
	{
		_thLowStart = clock;
		_thLowEnd = clock + 3420; //one line
	}
};
