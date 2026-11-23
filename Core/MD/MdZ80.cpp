#include "pch.h"
#include "MD/MdZ80.h"
#include "MD/MdConsole.h"
#include "MD/MdMemoryManager.h"
#include "Shared/Emulator.h"
#include "Shared/EventType.h"
#include "Shared/MemoryOperationType.h"
#include "Utilities/Serializer.h"
#include "Shared/Processors/Z80/Z80Engine.inl"

void MdZ80::Init(Emulator* emu, MdConsole* console, MdMemoryManager* memoryManager)
{
	_emu = emu;
	_console = console;
	_memoryManager = memoryManager;
	_state = {};
	_masterClock = 0;
}

void MdZ80::Reset()
{
	//Z80 /RESET: PC, I, R and the interrupt state are cleared; other registers keep their values.
	_state.PC = 0;
	_state.I = 0;
	_state.R = 0;
	_state.IFF1 = false;
	_state.IFF2 = false;
	_state.IM = 0;
	_state.Halted = false;
	_state.NmiPending = false;
	_state.FlagsChanged = 0;
}

void MdZ80::Run(uint64_t targetClock, bool held)
{
	if(held) {
		if(_masterClock < targetClock) {
			_masterClock = targetClock;
		}
		return;
	}
	while(_masterClock < targetClock) {
		Exec();
	}
}

template<MemoryOperationType type>
uint8_t MdZ80::BusRead(uint16_t addr)
{
	uint8_t value = _memoryManager->Z80Read(addr);
	if(_console->HasBusObserver()) {
		_console->NotifyBus({ _masterClock, MdBusDevice::Z80, type == MemoryOperationType::ExecOpCode ? MdBusOp::Fetch : MdBusOp::Read, 1, addr, value });
	}
	_emu->ProcessMemoryRead<CpuType::MdZ80>(addr, value, type);
	return value;
}

void MdZ80::BusWrite(uint16_t addr, uint8_t value)
{
	if(_console->HasBusObserver()) {
		_console->NotifyBus({ _masterClock, MdBusDevice::Z80, MdBusOp::Write, 1, addr, value });
	}
	if(_emu->ProcessMemoryWrite<CpuType::MdZ80>(addr, value, MemoryOperationType::Write)) {
		_memoryManager->Z80Write(addr, value);
	}
}

uint8_t MdZ80::BusReadPort(uint8_t port)
{
	//The Z80's I/O space is not decoded on the Mega Drive.
	return 0xFF;
}

void MdZ80::BusWritePort(uint8_t port, uint8_t value)
{
}

void MdZ80::OnInstruction()
{
	_emu->ProcessInstruction<CpuType::MdZ80>();
}

void MdZ80::OnHaltedCycle()
{
	_emu->ProcessHaltedCpu<CpuType::MdZ80>();
}

void MdZ80::OnHaltStarted()
{
	_emu->ProcessEvent(EventType::HaltStarted, CpuType::MdZ80);
}

void MdZ80::OnHaltEnded()
{
	_emu->ProcessEvent(EventType::HaltEnded, CpuType::MdZ80);
}

void MdZ80::OnInterruptAccepted(uint16_t originalPc, uint16_t handlerPc, bool nmi)
{
	_emu->ProcessInterrupt<CpuType::MdZ80>(originalPc, handlerPc, nmi);
}

void MdZ80::Serialize(Serializer& s)
{
	SV(_state.CycleCount);
	SV(_state.PC);
	SV(_state.SP);
	SV(_state.A);
	SV(_state.Flags);
	SV(_state.B);
	SV(_state.C);
	SV(_state.D);
	SV(_state.E);
	SV(_state.H);
	SV(_state.L);
	SV(_state.IXL);
	SV(_state.IXH);
	SV(_state.IYL);
	SV(_state.IYH);
	SV(_state.I);
	SV(_state.R);
	SV(_state.AltA);
	SV(_state.AltFlags);
	SV(_state.AltB);
	SV(_state.AltC);
	SV(_state.AltD);
	SV(_state.AltE);
	SV(_state.AltH);
	SV(_state.AltL);
	SV(_state.ActiveIrqs);
	SV(_state.NmiLevel);
	SV(_state.NmiPending);
	SV(_state.Halted);
	SV(_state.IFF1);
	SV(_state.IFF2);
	SV(_state.IM);
	SV(_state.FlagsChanged);
	SV(_state.WZ);
	SV(_cbAddress);
	SV(_ldIrEndCycle);
	SV(_masterClock);
}

template class Z80Engine<MdZ80>;
