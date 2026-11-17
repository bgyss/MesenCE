#include "pch.h"
#include "SMS/SmsCpu.h"
#include "SMS/SmsConsole.h"
#include "SMS/SmsTypes.h"
#include "SMS/SmsMemoryManager.h"
#include "Shared/Emulator.h"
#include "Shared/MemoryOperationType.h"
#include "Shared/EventType.h"
#include "Utilities/Serializer.h"
#include "Shared/Processors/Z80/Z80Engine.inl"

void SmsCpu::Init(Emulator* emu, SmsConsole* console, SmsMemoryManager* memoryManager)
{
	_emu = emu;
	_console = console;
	_memoryManager = memoryManager;

	_state = {};

	_state.PC = 0;

	if(!_console->HasBios()) {
		InitPostBiosState();
	}
}

template<MemoryOperationType type>
uint8_t SmsCpu::BusRead(uint16_t addr)
{
#ifdef DUMMYCPU
	uint8_t value = _memoryManager->DebugRead(addr);
	LogMemoryOperation(addr, value, type, MemoryType::SmsMemory);
	return value;
#else
	return _memoryManager->Read(addr, type);
#endif
}

void SmsCpu::BusWrite(uint16_t addr, uint8_t value)
{
#ifdef DUMMYCPU
	LogMemoryOperation(addr, value, MemoryOperationType::Write, MemoryType::SmsMemory);
#else
	_memoryManager->Write(addr, value);
#endif
}

uint8_t SmsCpu::BusReadPort(uint8_t port)
{
#ifdef DUMMYCPU
	uint8_t value = _memoryManager->DebugReadPort(port);
	LogMemoryOperation(port, value, MemoryOperationType::Read, MemoryType::SmsPort);
	return value;
#else
	return _memoryManager->ReadPort(port);
#endif
}

void SmsCpu::BusWritePort(uint8_t port, uint8_t value)
{
#ifdef DUMMYCPU
	LogMemoryOperation(port, value, MemoryOperationType::Write, MemoryType::SmsPort);
#else
	_memoryManager->WritePort(port, value);
#endif
}

void SmsCpu::OnExecCycles(uint8_t cycles)
{
#ifndef DUMMYCPU
	_memoryManager->Exec(cycles * 3);
#endif
}

uint8_t SmsCpu::GetInterruptDataBus()
{
	return _memoryManager->GetOpenBus();
}

bool SmsCpu::HasNmosLdIrQuirk()
{
	//The Game Gear's CMOS Z80 doesn't have the LD A,I / LD A,R P/V quirk.
	return _console->GetModel() != SmsModel::GameGear;
}

void SmsCpu::OnInstruction()
{
#ifndef DUMMYCPU
	_emu->ProcessInstruction<CpuType::Sms>();
#endif
}

void SmsCpu::OnHaltedCycle()
{
	_emu->ProcessHaltedCpu<CpuType::Sms>();
}

void SmsCpu::OnHaltStarted()
{
#ifndef DUMMYCPU
	_emu->ProcessEvent(EventType::HaltStarted, CpuType::Sms);
#endif
}

void SmsCpu::OnHaltEnded()
{
#ifndef DUMMYCPU
	_emu->ProcessEvent(EventType::HaltEnded, CpuType::Sms);
#endif
}

void SmsCpu::OnInterruptAccepted(uint16_t originalPc, uint16_t handlerPc, bool nmi)
{
#ifndef DUMMYCPU
	if(!nmi) {
		_console->RefreshRamCheats();
	}
	_emu->ProcessInterrupt<CpuType::Sms>(originalPc, handlerPc, nmi);
#endif
}

void SmsCpu::InitPostBiosState()
{
	_state.SP = 0xDFF0;
	_regAF.Write(0xAB42);
	_regBC.Write(0x0002);
	_regDE.Write(0xA51B);
	_regHL.Write(0x0000);
	_state.AltB = 0x02;
	_state.AltD = 0xC7;
	_state.AltH = 0xC7;
	_state.AltL = 0x5F;
	_state.IM = 1;
	_state.R = 29;
}

void SmsCpu::Serialize(Serializer& s)
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
}

template class Z80Engine<SmsCpu>;
