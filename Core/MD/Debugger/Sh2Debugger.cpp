#include "pch.h"
#include "MD/Debugger/Sh2Debugger.h"
#include "MD/Debugger/Sh2DisUtils.h"
#include "MD/Debugger/Sh2TraceLogger.h"
#include "MD/Debugger/MdEventManager.h"
#include "MD/Debugger/MdVdpTools.h"
#include "MD/MdConsole.h"
#include "MD/MdVdp.h"
#include "MD/M32x/M32xUnit.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Disassembler.h"
#include "Debugger/CallstackManager.h"
#include "Debugger/BreakpointManager.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryAccessCounter.h"
#include "Debugger/CodeDataLogger.h"
#include "Debugger/StepBackManager.h"
#include "Utilities/HexUtilities.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Shared/MemoryOperationType.h"

Sh2Debugger::Sh2Debugger(Debugger* debugger, CpuType cpuType) : IDebugger(debugger->GetEmulator())
{
	_debugger = debugger;
	_emu = debugger->GetEmulator();
	_settings = _emu->GetSettings();
	_cpuType = cpuType;
	_cpuMemType = cpuType == CpuType::M32xMaster ? MemoryType::M32xMasterMemory : MemoryType::M32xSlaveMemory;
	_disassembler = debugger->GetDisassembler();
	_memoryAccessCounter = debugger->GetMemoryAccessCounter();

	_console = (MdConsole*)debugger->GetConsole();
	_vdp = _console->GetVdp();
	_unit = _console->GetM32x();
	_cpu = &_unit->GetSh2(cpuType == CpuType::M32xMaster ? 0 : 1);

	//The SH-2s run from SDRAM: code/data logging covers it (not saved to a file)
	_codeDataLogger.reset(new CodeDataLogger(debugger, MemoryType::M32xSdram, M32xUnit::SdramSize, cpuType, 0));
	_traceLogger.reset(new Sh2TraceLogger(debugger, this, _vdp, cpuType));
	_stepBackManager.reset(new StepBackManager(_emu, this));
	_eventManager.reset(new MdEventManager(debugger, _vdp, cpuType));
	_ppuTools.reset(new MdVdpTools(debugger, _emu, _vdp));
	_callstackManager.reset(new CallstackManager(debugger, this));
	_breakpointManager.reset(new BreakpointManager(debugger, this, cpuType, _eventManager.get()));
	_step.reset(new StepRequest());
}

Sh2Debugger::~Sh2Debugger()
{
}

void Sh2Debugger::Reset()
{
	_callstackManager->Clear();
	ResetPrevOpCode();
}

AddressInfo Sh2Debugger::GetAbsoluteAddress(uint32_t addr)
{
	AddressInfo rel = { (int32_t)addr, _cpuMemType };
	return _console->GetAbsoluteAddress(rel);
}

uint32_t Sh2Debugger::GetCurrentPc()
{
	return _cpu->GetInstructionPc() & 0x1FFFFFFF;
}

uint32_t Sh2Debugger::GetStackPointer()
{
	return _cpu->GetState().R[15];
}

uint16_t Sh2Debugger::ReadOpCode(uint32_t pc)
{
	return (uint16_t)(_unit->ShPeek8(pc) << 8 | _unit->ShPeek8(pc + 1));
}

void Sh2Debugger::ProcessInstruction()
{
	uint32_t pc = GetCurrentPc();
	AddressInfo addressInfo = GetAbsoluteAddress(pc);
	uint16_t opCode = ReadOpCode(pc);
	MemoryOperationInfo operation(pc, opCode >> 8, MemoryOperationType::ExecOpCode, _cpuMemType);
	InstructionProgress.LastMemOperation = operation;
	InstructionProgress.StartCycle = GetCpuCycleCount(false);

	if(addressInfo.Address >= 0) {
		if(addressInfo.Type == MemoryType::M32xSdram) {
			_codeDataLogger->SetCode(addressInfo.Address, Sh2DisUtils::GetOpFlags(_prevOpCode, pc, _prevProgramCounter));
		}
		_disassembler->BuildCache(addressInfo, 0, _cpuType);
	}

	//Calls and returns take effect after their delay slot
	uint32_t sp = GetStackPointer();
	if(_callPending) {
		_callPending = false;
		if(pc != _callReturn) {
			AddressInfo src = GetAbsoluteAddress(_callSource);
			AddressInfo ret = GetAbsoluteAddress(_callReturn);
			_callstackManager->Push(src, _callSource, addressInfo, pc, ret, _callReturn, _callStackPointer, StackFrameFlags::None);
		}
	}
	if(_returnPending) {
		_returnPending = false;
		_callstackManager->Pop(addressInfo, pc, sp);
		if(_step->BreakAddress == (int32_t)pc && _step->BreakStackPointer == sp) {
			_step->Break(BreakSource::CpuStep);
		}
	}
	if(Sh2DisUtils::IsJumpToSub(_prevOpCode)) {
		_callPending = true;
		_callSource = _prevProgramCounter;
		_callReturn = _prevProgramCounter + 4;
		_callStackPointer = _prevStackPointer;
	} else if(Sh2DisUtils::IsReturnInstruction(_prevOpCode)) {
		_returnPending = true;
	}

	if(_traceLogger->IsEnabled()) {
		DisassemblyInfo disInfo = _disassembler->GetDisassemblyInfo(addressInfo, pc, 0, _cpuType);
		_state = _cpu->GetDebugState();
		_traceLogger->Log(_state, disInfo, operation, addressInfo);
	}
	_memoryAccessCounter->ProcessMemoryExec(addressInfo, _console->GetMasterClock());

	_prevOpCode = opCode;
	_prevProgramCounter = pc;
	_prevStackPointer = sp;

	_step->ProcessCpuExec();
	_debugger->ProcessBreakConditions(_cpuType, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

template<typename T>
void Sh2Debugger::ProcessRead(uint32_t addr, T value, MemoryOperationType type)
{
	AddressInfo addressInfo = GetAbsoluteAddress(addr);
	MemoryOperationInfo operation(addr, value, type, _cpuMemType);
	InstructionProgress.LastMemOperation = operation;

	if(addressInfo.Address >= 0 && addressInfo.Type == MemoryType::M32xSdram) {
		_codeDataLogger->SetData(addressInfo.Address);
	}
	if(_traceLogger->IsEnabled()) {
		_traceLogger->LogNonExec(operation, addressInfo);
	}
	ReadResult result = _memoryAccessCounter->ProcessMemoryRead(addressInfo, _console->GetMasterClock());
	if(result == ReadResult::FirstUninitRead) {
		_debugger->Log(string(_cpuType == CpuType::M32xMaster ? "[SH2M]" : "[SH2S]") + " Uninitialized memory read: $" + HexUtilities::ToHex32(addr));
	}
	_step->ProcessCpuCycle();
	_debugger->ProcessBreakConditions(_cpuType, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

template<typename T>
void Sh2Debugger::ProcessWrite(uint32_t addr, T value, MemoryOperationType type)
{
	AddressInfo addressInfo = GetAbsoluteAddress(addr);
	MemoryOperationInfo operation(addr, value, type, _cpuMemType);
	InstructionProgress.LastMemOperation = operation;

	if(addressInfo.Type == MemoryType::M32xSdram) {
		_disassembler->InvalidateCache(addressInfo, _cpuType);
	}
	if(_traceLogger->IsEnabled()) {
		_traceLogger->LogNonExec(operation, addressInfo);
	}
	_memoryAccessCounter->ProcessMemoryWrite(addressInfo, _console->GetMasterClock());
	_step->ProcessCpuCycle();
	_debugger->ProcessBreakConditions(_cpuType, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void Sh2Debugger::ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi)
{
	AddressInfo ret = GetAbsoluteAddress(originalPc);
	AddressInfo dest = GetAbsoluteAddress(currentPc);
	if(dest.Type == MemoryType::M32xSdram && dest.Address >= 0) {
		_codeDataLogger->SetCode(dest.Address, CdlFlags::SubEntryPoint);
	}
	//SR and PC were pushed
	uint32_t originalSp = GetStackPointer() + 8;
	_callPending = false;
	_returnPending = false;
	ResetPrevOpCode();
	_debugger->InternalProcessInterrupt(_cpuType, *this, *_step.get(), ret, originalPc, dest, currentPc, ret, originalPc, originalSp, forNmi);
}

void Sh2Debugger::Run()
{
	_step.reset(new StepRequest());
}

void Sh2Debugger::Step(int32_t stepCount, StepType type)
{
	StepRequest step(type);
	switch(type) {
		case StepType::Step: step.StepCount = stepCount; break;
		case StepType::StepOut:
			step.BreakAddress = _callstackManager->GetReturnAddress();
			step.BreakStackPointer = _callstackManager->GetReturnStackPointer();
			break;
		case StepType::StepOver:
			if(Sh2DisUtils::IsJumpToSub(_prevOpCode)) {
				//Past the call and its delay slot
				step.BreakAddress = _prevProgramCounter + 4;
				step.BreakStackPointer = _prevStackPointer;
			} else {
				step.StepCount = 1;
			}
			break;
		case StepType::CpuCycleStep: step.CpuCycleStepCount = stepCount; break;
		default: step.StepCount = 1; break;
	}
	_step.reset(new StepRequest(step));
}

void Sh2Debugger::SetProgramCounter(uint32_t addr, bool updateDebuggerOnly)
{
	if(!updateDebuggerOnly) {
		_cpu->SetPc(addr & ~1u);
	}
	_prevOpCode = ReadOpCode(addr);
	_prevProgramCounter = addr;
	_prevStackPointer = GetStackPointer();
}

uint32_t Sh2Debugger::GetProgramCounter(bool getInstPc)
{
	return getInstPc ? _prevProgramCounter : (_cpu->GetState().PC & 0x1FFFFFFF);
}

uint64_t Sh2Debugger::GetCpuCycleCount(bool forProfiler)
{
	(void)forProfiler;
	return _cpu->GetCycleCount();
}

void Sh2Debugger::ResetPrevOpCode()
{
	_prevOpCode = 0;
}

DebuggerFeatures Sh2Debugger::GetSupportedFeatures()
{
	DebuggerFeatures features = {};
	features.RunToIrq = true;
	features.StepOver = true;
	features.StepOut = true;
	features.CallStack = true;
	features.CpuCycleStep = true;
	features.ChangeProgramCounter = AllowChangeProgramCounter;
	features.CpuVectors[0] = { "Reset", 0x00, VectorType::Indirect };
	features.CpuVectorCount = 1;
	return features;
}

BaseEventManager* Sh2Debugger::GetEventManager()
{
	return _eventManager.get();
}

PpuTools* Sh2Debugger::GetPpuTools()
{
	return _ppuTools.get();
}

CallstackManager* Sh2Debugger::GetCallstackManager()
{
	return _callstackManager.get();
}

BreakpointManager* Sh2Debugger::GetBreakpointManager()
{
	return _breakpointManager.get();
}

ITraceLogger* Sh2Debugger::GetTraceLogger()
{
	return _traceLogger.get();
}

ISerializable* Sh2Debugger::GetSerializableCpu()
{
	return _cpu;
}

BaseState& Sh2Debugger::GetState()
{
	_state = _cpu->GetDebugState();
	return _state;
}

void Sh2Debugger::SetSh2State(M32xSh2State& state)
{
	_cpu->SetDebugState(state);
}

void Sh2Debugger::GetPpuState(BaseState& state)
{
	(MdVdpState&)state = _vdp->GetState();
}

void Sh2Debugger::SetPpuState(BaseState& srcState)
{
	MdVdpState& src = (MdVdpState&)srcState;
	for(int i = 0; i < 24; i++) {
		_vdp->DebugWriteRegister(i, src.Regs[i]);
	}
}

template void Sh2Debugger::ProcessRead<uint8_t>(uint32_t addr, uint8_t value, MemoryOperationType type);
template void Sh2Debugger::ProcessRead<uint16_t>(uint32_t addr, uint16_t value, MemoryOperationType type);
template void Sh2Debugger::ProcessRead<uint32_t>(uint32_t addr, uint32_t value, MemoryOperationType type);
template void Sh2Debugger::ProcessWrite<uint8_t>(uint32_t addr, uint8_t value, MemoryOperationType type);
template void Sh2Debugger::ProcessWrite<uint16_t>(uint32_t addr, uint16_t value, MemoryOperationType type);
template void Sh2Debugger::ProcessWrite<uint32_t>(uint32_t addr, uint32_t value, MemoryOperationType type);
