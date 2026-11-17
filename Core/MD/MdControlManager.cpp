#include "pch.h"
#include "MD/MdControlManager.h"
#include "MD/MdConsole.h"
#include "MD/Input/MdController.h"
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
		default: return nullptr;
	}
}

void MdControlManager::UpdateControlDevices()
{
	MdConfig cfg = _emu->GetSettings()->GetMdConfig();
	if(_emu->GetSettings()->IsEqual(_prevConfig, cfg) && _controlDevices.size() > 0) {
		return;
	}

	auto lock = _deviceLock.AcquireSafe();
	ClearDevices();
	for(int i = 0; i < 2; i++) {
		shared_ptr<BaseControlDevice> device = CreateControllerDevice(i == 0 ? cfg.Port1.Type : cfg.Port2.Type, i);
		if(device) {
			RegisterControlDevice(device);
		}
	}
	_prevConfig = cfg;
}

uint8_t MdControlManager::ReadPad(uint8_t port, bool th, uint64_t masterClock)
{
	shared_ptr<MdController> pad = std::dynamic_pointer_cast<MdController>(GetControlDevice(port));
	if(!pad) {
		return 0x7F;
	}
	SetInputReadFlag();
	return pad->ReadPins(th, masterClock);
}

void MdControlManager::OnThChange(uint8_t port, bool th, uint64_t masterClock)
{
	shared_ptr<MdController> pad = std::dynamic_pointer_cast<MdController>(GetControlDevice(port));
	if(pad) {
		pad->OnThChange(th, masterClock);
	}
}

void MdControlManager::Serialize(Serializer& s)
{
	BaseControlManager::Serialize(s);
}
