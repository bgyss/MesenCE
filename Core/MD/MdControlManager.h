#pragma once
#include "pch.h"
#include "Shared/BaseControlManager.h"
#include "Shared/SettingTypes.h"

class MdConsole;
class IMdPortDevice;

class MdControlManager final : public BaseControlManager
{
private:
	MdConsole* _console = nullptr;
	MdConfig _prevConfig = {};
	//The device on each port, cached when the devices are (re)created
	IMdPortDevice* _portDevices[2] = {};
	bool _hasLightgun = false;

public:
	MdControlManager(Emulator* emu, MdConsole* console);

	shared_ptr<BaseControlDevice> CreateControllerDevice(ControllerType type, uint8_t port) override;
	void UpdateControlDevices() override;

	//Pin levels driven by the device on `port` (bits 6-0; 0x7F when nothing is connected)
	uint8_t ReadPort(uint8_t port, uint64_t masterClock);
	//The console's data/control register for `port` changed
	void WritePort(uint8_t port, uint8_t levels, uint8_t outputs, uint64_t masterClock);

	//Light gun on either port: the pixel column it sees on frame row `row`, or -1
	int32_t GetLightgunX(uint16_t row, uint8_t& port);
	uint32_t GetLightgunLatency(uint8_t port, bool h40);
	//The beam reached the gun's spot (the gun pulls TH low)
	void OnLightgunHit(uint8_t port, uint64_t masterClock);
	bool HasLightgun() { return _hasLightgun; }

	void Serialize(Serializer& s) override;
};
