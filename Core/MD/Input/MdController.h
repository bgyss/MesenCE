#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Utilities/Serializer.h"
#include "MD/Input/MdPortDevice.h"

// Mega Drive 3- and 6-button pads (Sega Genesis Software Manual, "Control Pad"). Pins are active
// low. TH high: ?1CBRLDU. TH low: ?0SA00DU. The 6-button pad counts TH high-to-low edges: on the
// third low phase it reports ?0SA0000 (the 6-button ID), on the high phase after it ?1CBMXYZ,
// on the fourth low phase ?0SA1111; it returns to the 3-button sequence when TH stays put for
// about 1.5 ms (the exact timeout is a hardware-unverified choice).
class MdController : public BaseControlDevice, public IMdPortDevice
{
private:
	bool _sixButtons = false;
	uint8_t _phase = 0;
	bool _th = true;
	uint64_t _lastThChange = 0;
	uint32_t _turboSpeed = 0;

	static constexpr uint64_t PhaseTimeout = 80000; //master clocks (~1.5 ms)

protected:
	string GetKeyNames() override { return "UDLRSABCXYZM"; }

	void InternalSetStateFromInput() override
	{
		bool turboOn = IsTurboOn(_turboSpeed);
		for(KeyMapping& keyMapping : _keyMappings) {
			SetPressedState(Buttons::Up, keyMapping.Up);
			SetPressedState(Buttons::Down, keyMapping.Down);
			SetPressedState(Buttons::Left, keyMapping.Left);
			SetPressedState(Buttons::Right, keyMapping.Right);
			SetPressedState(Buttons::Start, keyMapping.Start);
			SetPressedState(Buttons::A, keyMapping.A);
			SetPressedState(Buttons::B, keyMapping.B);
			SetPressedState(Buttons::C, keyMapping.X);
			if(_sixButtons) {
				SetPressedState(Buttons::X, keyMapping.Y);
				SetPressedState(Buttons::Y, keyMapping.L);
				SetPressedState(Buttons::Z, keyMapping.R);
				SetPressedState(Buttons::Mode, keyMapping.Select);
			}
			if(turboOn) {
				SetPressedState(Buttons::A, keyMapping.TurboA);
				SetPressedState(Buttons::B, keyMapping.TurboB);
				SetPressedState(Buttons::C, keyMapping.TurboX);
			}
		}
	}

	void RefreshStateBuffer() override {}

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_phase);
		SV(_th);
		SV(_lastThChange);
	}

public:
	enum Buttons
	{
		Up = 0,
		Down,
		Left,
		Right,
		Start,
		A,
		B,
		C,
		X,
		Y,
		Z,
		Mode
	};

	MdController(Emulator* emu, uint8_t port, KeyMappingSet keyMappings, bool sixButtons)
		: BaseControlDevice(emu, sixButtons ? ControllerType::MdController6Buttons : ControllerType::MdController3Buttons, port, keyMappings)
	{
		_sixButtons = sixButtons;
		_turboSpeed = keyMappings.TurboSpeed;
	}

	//The pad is read through MdMemoryManager/MdControlManager (TH protocol), not a RAM-mapped port
	uint8_t ReadRam(uint16_t addr) override { return 0; }
	void WriteRam(uint16_t addr, uint8_t value) override {}

	void WritePins(uint8_t levels, uint8_t outputs, uint64_t clock) override
	{
		//TH as the pad sees it: the driven level when it is an output, pulled high otherwise
		bool th = (outputs & 0x40) ? (levels & 0x40) : true;
		if(th != _th) {
			OnThChange(th, clock);
		}
	}

	uint8_t ReadPins(uint64_t clock) override { return ReadPins(_th, clock); }

	bool IsSixButtons() { return _sixButtons; }

	//Lua (emu.getInput/emu.setInput) button names
	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "up", Buttons::Up },
			{ "down", Buttons::Down },
			{ "left", Buttons::Left },
			{ "right", Buttons::Right },
			{ "start", Buttons::Start },
			{ "a", Buttons::A },
			{ "b", Buttons::B },
			{ "c", Buttons::C },
			{ "x", Buttons::X },
			{ "y", Buttons::Y },
			{ "z", Buttons::Z },
			{ "mode", Buttons::Mode },
		};
	}

	//Active-low nibbles as the Team Player reports them (MdTeamPlayer.h): 0 = RLDU, 1 = SACB, 2 = MXYZ
	uint8_t GetNibble(int index)
	{
		auto bit = [this](Buttons b, uint8_t mask) -> uint8_t { return IsPressed(b) ? 0 : mask; };
		switch(index) {
			case 0: return bit(Right, 0x08) | bit(Left, 0x04) | bit(Down, 0x02) | bit(Up, 0x01);
			case 1: return bit(Start, 0x08) | bit(A, 0x04) | bit(C, 0x02) | bit(B, 0x01);
			default: return bit(Mode, 0x08) | bit(X, 0x04) | bit(Y, 0x02) | bit(Z, 0x01);
		}
	}

	void OnThChange(bool th, uint64_t clock)
	{
		if(clock - _lastThChange > PhaseTimeout) {
			_phase = 0;
		}
		if(_th && !th) {
			_phase++;
		}
		_th = th;
		_lastThChange = clock;
	}

	//Returns the 7 pin levels (bits 6-0, bit 6 = TH as seen by the pad side, i.e. ignored)
	uint8_t ReadPins(bool th, uint64_t clock)
	{
		if(_sixButtons && clock - _lastThChange > PhaseTimeout) {
			_phase = 0;
		}
		auto bit = [this](Buttons b, uint8_t mask) -> uint8_t { return IsPressed(b) ? 0 : mask; };
		uint8_t value;
		if(th) {
			if(_sixButtons && _phase == 3) {
				value = 0x40 | bit(C, 0x20) | bit(B, 0x10) | bit(Mode, 0x08) | bit(X, 0x04) | bit(Y, 0x02) | bit(Z, 0x01);
			} else {
				value = 0x40 | bit(C, 0x20) | bit(B, 0x10) | bit(Right, 0x08) | bit(Left, 0x04) | bit(Down, 0x02) | bit(Up, 0x01);
			}
		} else {
			if(_sixButtons && _phase == 3) {
				value = bit(Start, 0x20) | bit(A, 0x10);
			} else if(_sixButtons && _phase == 4) {
				value = bit(Start, 0x20) | bit(A, 0x10) | 0x0F;
			} else {
				value = bit(Start, 0x20) | bit(A, 0x10) | bit(Down, 0x02) | bit(Up, 0x01);
			}
		}
		return value;
	}
};
