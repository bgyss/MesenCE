#include "pch.h"
#include "MD/MdControlManager.h"
#include "MD/MdConsole.h"
#include "MD/Input/MdController.h"
#include "MD/Input/MdTeamPlayer.h"
#include "MD/Input/MdMouse.h"
#include "MD/Input/MdLightGun.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Utilities/Serializer.h"

MdControlManager::MdControlManager(Emulator* emu, MdConsole* console) : BaseControlManager(emu, CpuType::MdM68k)
{
	_console = console;
}

shared_ptr<BaseControlDevice> MdControlManager::CreateControllerDevice(ControllerType type, uint8_t port)
{
	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();
	KeyMappingSet keys = port == 0 ? cfg.Port1.Keys : cfg.Port2.Keys;
	switch(type) {
		case ControllerType::MdController3Buttons: return shared_ptr<BaseControlDevice>(new MdController(_emu, port, keys, false));
		case ControllerType::MdController6Buttons: return shared_ptr<BaseControlDevice>(new MdController(_emu, port, keys, true));
		case ControllerType::MdTeamPlayer: {
			ControllerConfig controllers[4];
			std::copy(port == 0 ? cfg.Port1SubPorts : cfg.Port2SubPorts, (port == 0 ? cfg.Port1SubPorts : cfg.Port2SubPorts) + 4, controllers);
			return shared_ptr<BaseControlDevice>(new MdTeamPlayer(_emu, port, controllers));
		}
		case ControllerType::MdMouse: return shared_ptr<BaseControlDevice>(new MdMouse(_emu, port, keys));
		case ControllerType::MdMenacer: return shared_ptr<BaseControlDevice>(new MdLightGun(_emu, port, keys, false));
		case ControllerType::MdJustifier: return shared_ptr<BaseControlDevice>(new MdLightGun(_emu, port, keys, true));
		default: return nullptr;
	}
}

//Compares only what defines the devices, member by member. A memcmp of MdConfig also compares its
//padding bytes (indeterminate in a copy), which made the pads look reconfigured and get recreated at
//every frame end, resetting the 6-button handshake (found by io_pad6_across_frame).
static bool SamePortConfig(const ControllerConfig& a, const ControllerConfig& b)
{
	const KeyMapping* ma[4] = { &a.Keys.Mapping1, &a.Keys.Mapping2, &a.Keys.Mapping3, &a.Keys.Mapping4 };
	const KeyMapping* mb[4] = { &b.Keys.Mapping1, &b.Keys.Mapping2, &b.Keys.Mapping3, &b.Keys.Mapping4 };
	for(int i = 0; i < 4; i++) {
		//KeyMapping holds only uint16_t members: no padding
		if(memcmp(ma[i], mb[i], sizeof(KeyMapping)) != 0) {
			return false;
		}
	}
	return a.Type == b.Type && a.Keys.TurboSpeed == b.Keys.TurboSpeed;
}

static bool SameSubPorts(const ControllerConfig* a, const ControllerConfig* b)
{
	for(int i = 0; i < 4; i++) {
		if(!SamePortConfig(a[i], b[i])) {
			return false;
		}
	}
	return true;
}

void MdControlManager::UpdateControlDevices()
{
	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();
	if(_controlDevices.size() > 0 && SamePortConfig(_prevConfig.Port1, cfg.Port1) && SamePortConfig(_prevConfig.Port2, cfg.Port2) &&
		SameSubPorts(_prevConfig.Port1SubPorts, cfg.Port1SubPorts) && SameSubPorts(_prevConfig.Port2SubPorts, cfg.Port2SubPorts)) {
		return;
	}

	auto lock = _deviceLock.AcquireSafe();
	ClearDevices();
	_portDevices[0] = _portDevices[1] = nullptr;
	_hasLightgun = false;
	for(int i = 0; i < 2; i++) {
		shared_ptr<BaseControlDevice> device = CreateControllerDevice(i == 0 ? cfg.Port1.Type : cfg.Port2.Type, i);
		if(device) {
			RegisterControlDevice(device);
			_portDevices[i] = dynamic_cast<IMdPortDevice*>(device.get());
			ControllerType type = device->GetControllerType();
			_hasLightgun |= type == ControllerType::MdMenacer || type == ControllerType::MdJustifier;
		}
	}
	_prevConfig = cfg;
}

uint8_t MdControlManager::ReadPort(uint8_t port, uint64_t masterClock)
{
	IMdPortDevice* device = _portDevices[port];
	if(!device) {
		return 0x7F;
	}
	SetInputReadFlag();
	return device->ReadPins(masterClock) & 0x7F;
}

void MdControlManager::WritePort(uint8_t port, uint8_t levels, uint8_t outputs, uint64_t masterClock)
{
	if(_portDevices[port]) {
		_portDevices[port]->WritePins(levels, outputs, masterClock);
	}
}

int32_t MdControlManager::GetLightgunX(uint16_t row, uint8_t& port)
{
	for(uint8_t i = 0; i < 2; i++) {
		if(_portDevices[i]) {
			int32_t x = _portDevices[i]->GetLightgunX(row);
			if(x >= 0) {
				port = i;
				return x;
			}
		}
	}
	return -1;
}

uint32_t MdControlManager::GetLightgunLatency(uint8_t port, bool h40)
{
	return _portDevices[port] ? _portDevices[port]->GetLightgunLatency(h40) : 0;
}

void MdControlManager::OnLightgunHit(uint8_t port, uint64_t masterClock)
{
	if(_portDevices[port]) {
		_portDevices[port]->OnLightgunHit(masterClock);
	}
}

void MdControlManager::Serialize(Serializer& s)
{
	BaseControlManager::Serialize(s);

	//The pads hold handshake state (6-button phase counter, TH level, time of the last TH change),
	//as the SMS/NES/PCE control managers do for their devices
	if(!s.IsSaving()) {
		UpdateControlDevices();
	}
	for(uint8_t i = 0; i < _controlDevices.size(); i++) {
		SVI(_controlDevices[i]);
	}
}
