#pragma once
#include "pch.h"
#include "Shared/BaseControlManager.h"
#include "Shared/SettingTypes.h"

class MdConsole;

class MdControlManager final : public BaseControlManager
{
private:
	MdConsole* _console = nullptr;
	MdConfig _prevConfig = {};

public:
	MdControlManager(Emulator* emu, MdConsole* console);

	shared_ptr<BaseControlDevice> CreateControllerDevice(ControllerType type, uint8_t port) override;
	void UpdateControlDevices() override;

	uint8_t ReadPad(uint8_t port, bool th, uint64_t masterClock);
	void OnThChange(uint8_t port, bool th, uint64_t masterClock);

	void Serialize(Serializer& s) override;
};
