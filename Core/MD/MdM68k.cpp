#include "pch.h"
#include "MD/MdM68k.h"
#include "MD/MdConsole.h"
#include "MD/MdMemoryManager.h"
#include "MD/MdVdp.h"
#include "Shared/Emulator.h"
#include "Shared/MemoryOperationType.h"
#include "Utilities/Serializer.h"

void MdM68k::Init(Emulator* emu, MdConsole* console, MdMemoryManager* memoryManager, MdVdp* vdp)
{
	_emu = emu;
	_console = console;
	_memoryManager = memoryManager;
	_vdp = vdp;
}

void MdM68k::PowerOn()
{
	_cycleCount = 0;
	M68kCore::PowerOn();
	M68kCore::Reset();
}

void MdM68k::Exec()
{
	Step();
	_memoryManager->SyncAfterM68kInstruction();
}

void MdM68k::Idle(uint32_t clocks)
{
	_cycleCount += clocks;
	_memoryManager->AdvanceM68k(clocks);
}

void MdM68k::Wait(uint32_t clocks)
{
	_cycleCount += clocks;
	_memoryManager->AdvanceM68k(clocks);
}

uint16_t MdM68k::BusRead(bool upper, bool lower, uint32_t address, M68kAccess accessType)
{
	uint16_t value = _memoryManager->M68kRead(upper, lower, address, accessType);
	if(_console->HasBusObserver()) {
		uint8_t width = (upper && lower) ? 2 : 1;
		uint32_t addr = (address & 0xFFFFFE) | (upper ? 0 : 1);
		uint32_t v = width == 2 ? value : (upper ? (value >> 8) : (value & 0xFF));
		_console->NotifyBus({ _memoryManager->GetMasterClock(), MdBusDevice::M68k, accessType == M68kAccess::Program ? MdBusOp::Fetch : MdBusOp::Read, width, addr, v });
	}
	MemoryOperationType opType = accessType == M68kAccess::Program ? MemoryOperationType::ExecOperand : MemoryOperationType::Read;
	if(upper && lower) {
		_emu->ProcessMemoryRead<CpuType::MdM68k, 2>(address & 0xFFFFFE, value, opType);
	} else {
		uint8_t byte = upper ? (value >> 8) : (uint8_t)value;
		_emu->ProcessMemoryRead<CpuType::MdM68k, 1>((address & 0xFFFFFE) | (lower ? 1 : 0), byte, opType);
	}
	return value;
}

void MdM68k::BusWrite(bool upper, bool lower, uint32_t address, uint16_t data, M68kAccess accessType)
{
	if(upper && lower) {
		if(!_emu->ProcessMemoryWrite<CpuType::MdM68k, 2>(address & 0xFFFFFE, data, MemoryOperationType::Write)) {
			return;
		}
	} else {
		uint8_t byte = upper ? (data >> 8) : (uint8_t)data;
		if(!_emu->ProcessMemoryWrite<CpuType::MdM68k, 1>((address & 0xFFFFFE) | (lower ? 1 : 0), byte, MemoryOperationType::Write)) {
			return;
		}
		data = (byte << 8) | byte;
	}
	if(_console->HasBusObserver()) {
		uint8_t width = (upper && lower) ? 2 : 1;
		uint32_t addr = (address & 0xFFFFFE) | (upper ? 0 : 1);
		uint32_t v = width == 2 ? data : (upper ? (data >> 8) : (data & 0xFF));
		_console->NotifyBus({ _memoryManager->GetMasterClock(), MdBusDevice::M68k, MdBusOp::Write, width, addr, v });
	}
	_memoryManager->M68kWrite(upper, lower, address, data, accessType);
}

uint16_t MdM68k::Peek(uint32_t address)
{
	return _memoryManager->M68kPeekWord(address);
}

uint32_t MdM68k::AcknowledgeInterrupt(uint8_t level)
{
	_memoryManager->AcknowledgeM68kInterrupt(level);
	return M68kCore::AcknowledgeInterrupt(level);
}

void MdM68k::OnInstructionStart()
{
	_emu->ProcessInstruction<CpuType::MdM68k>();
}

void MdM68k::OnInterruptTaken(uint32_t originalPc, uint32_t handlerPc)
{
	_emu->ProcessInterrupt<CpuType::MdM68k>(originalPc, handlerPc, false);
}

MdM68kState MdM68k::GetState()
{
	M68kRegisters regs = GetRegisters();
	MdM68kState state = {};
	state.CycleCount = _cycleCount;
	for(int i = 0; i < 8; i++) {
		state.D[i] = regs.D[i];
	}
	for(int i = 0; i < 7; i++) {
		state.A[i] = regs.A[i];
	}
	state.Usp = regs.Usp;
	state.Ssp = regs.Ssp;
	state.A[7] = (regs.Sr & 0x2000) ? regs.Ssp : regs.Usp;
	state.PC = GetInstructionPc();
	state.SR = regs.Sr;
	state.Ir = regs.Ir;
	state.Irc = regs.Irc;
	state.IrqLevel = GetInterruptLevel();
	state.Stopped = regs.Stopped;
	state.Halted = IsHalted();
	return state;
}

void MdM68k::SetState(MdM68kState& state)
{
	M68kRegisters regs = GetRegisters();
	for(int i = 0; i < 8; i++) {
		regs.D[i] = state.D[i];
	}
	for(int i = 0; i < 7; i++) {
		regs.A[i] = state.A[i];
	}
	if(state.SR & 0x2000) {
		regs.Ssp = state.A[7];
		regs.Usp = state.Usp;
	} else {
		regs.Usp = state.A[7];
		regs.Ssp = state.Ssp;
	}
	regs.Sr = state.SR;
	if(state.PC != GetInstructionPc()) {
		// Moving the PC refills the prefetch queue from the new address.
		regs.Pc = state.PC + 4;
		regs.Ir = Peek(state.PC);
		regs.Irc = Peek(state.PC + 2);
	}
	SetRegisters(regs);
}

void MdM68k::Serialize(Serializer& s)
{
	ares::M68000::Registers& reg = RawState();
	for(int i = 0; i < 8; i++) {
		uint32_t d = reg.d[i];
		uint32_t a = reg.a[i];
		SVI(d);
		SVI(a);
		if(!s.IsSaving()) {
			reg.d[i] = d;
			reg.a[i] = a;
		}
	}
	uint32_t sp = reg.sp;
	uint32_t pc = reg.pc;
	uint8_t mask = reg.i;
	uint16_t irc = reg.irc;
	uint16_t ir = reg.ir;
	uint16_t ird = reg.ird;
	SV(sp);
	SV(pc);
	SV(reg.c);
	SV(reg.v);
	SV(reg.z);
	SV(reg.n);
	SV(reg.x);
	SV(mask);
	SV(reg.s);
	SV(reg.t);
	SV(irc);
	SV(ir);
	SV(ird);
	SV(reg.stop);
	SV(reg.reset);
	SV(RawIrqLevel());
	SV(RawNmiPending());
	SV(RawHalted());
	SV(_cycleCount);
	if(!s.IsSaving()) {
		reg.sp = sp;
		reg.pc = pc;
		reg.i = mask;
		reg.irc = irc;
		reg.ir = ir;
		reg.ird = ird;
	}
}
