#include "pch.h"
#include "MD/Debugger/MdDebugger.h"
#include "MD/Debugger/MdDisUtils.h"
#include "MD/Debugger/MdEventManager.h"
#include "MD/Debugger/MdVdpTools.h"
#include "MD/Debugger/MdTraceLogger.h"
#include "MD/MdConsole.h"
#include "MD/MdM68k.h"
#include "MD/MdZ80.h"
#include "MD/MdVdp.h"
#include "MD/MdMemoryManager.h"
#include "SMS/Debugger/SmsDisUtils.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Disassembler.h"
#include "Debugger/CallstackManager.h"
#include "Debugger/BreakpointManager.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryAccessCounter.h"
#include "Debugger/MemoryDumper.h"
#include "Debugger/CodeDataLogger.h"
#include "Debugger/StepBackManager.h"
#include "Utilities/HexUtilities.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Shared/MemoryOperationType.h"

MdDebugger::MdDebugger(Debugger* debugger, CpuType cpuType) : IDebugger(debugger->GetEmulator())
{
	_debugger = debugger;
	_emu = debugger->GetEmulator();
	_settings = _emu->GetSettings();
	_cpuType = cpuType;
	_cpuMemType = cpuType == CpuType::MdM68k ? MemoryType::MdMemory : MemoryType::MdZ80Memory;
	_disassembler = debugger->GetDisassembler();
	_memoryAccessCounter = debugger->GetMemoryAccessCounter();

	_console = (MdConsole*)debugger->GetConsole();
	_m68k = _console->GetM68k();
	_z80 = _console->GetZ80();
	_vdp = _console->GetVdp();
	_memoryManager = _console->GetMemoryManager();

	_codeDataLogger.reset(new CodeDataLogger(debugger, MemoryType::MdPrgRom, _emu->GetMemory(MemoryType::MdPrgRom).Size, cpuType, _emu->GetCrc32()));
	if(cpuType == CpuType::MdM68k) {
		_cdlFile = _codeDataLogger->GetCdlFilePath(_emu->GetRomInfo().RomFile.GetFileName());
		_codeDataLogger->LoadCdlFile(_cdlFile, _settings->GetDebugConfig().AutoResetCdl);
		_traceLogger.reset(new MdM68kTraceLogger(debugger, this, _vdp));
	} else {
		_traceLogger.reset(new MdZ80TraceLogger(debugger, this, _vdp));
	}

	_stepBackManager.reset(new StepBackManager(_emu, this));
	_eventManager.reset(new MdEventManager(debugger, _vdp, cpuType));
	_ppuTools.reset(new MdVdpTools(debugger, debugger->GetEmulator(), _vdp));
	_callstackManager.reset(new CallstackManager(debugger, this));
	_breakpointManager.reset(new BreakpointManager(debugger, this, cpuType, _eventManager.get()));
	_step.reset(new StepRequest());
}

MdDebugger::~MdDebugger()
{
	if(!_cdlFile.empty()) {
		_codeDataLogger->SaveCdlFile(_cdlFile);
	}
}

void MdDebugger::Reset()
{
	_callstackManager->Clear();
	ResetPrevOpCode();
}

AddressInfo MdDebugger::GetAbsoluteAddress(uint32_t addr)
{
	AddressInfo rel = { (int32_t)addr, _cpuMemType };
	return _console->GetAbsoluteAddress(rel);
}

uint32_t MdDebugger::GetCurrentPc()
{
	return _cpuType == CpuType::MdM68k ? _m68k->GetInstructionPc() : _z80->GetState().PC;
}

uint32_t MdDebugger::GetStackPointer()
{
	if(_cpuType == CpuType::MdM68k) {
		M68kRegisters regs = _m68k->GetRegisters();
		return (regs.Sr & 0x2000) ? regs.Ssp : regs.Usp;
	}
	return _z80->GetState().SP;
}

uint16_t MdDebugger::ReadOpCode(uint32_t pc)
{
	if(_cpuType == CpuType::MdM68k) {
		return _memoryManager->M68kPeekWord(pc);
	}
	uint8_t op = _memoryManager->DebugZ80Read((uint16_t)pc);
	return op == 0xED ? (0xED | (_memoryManager->DebugZ80Read((uint16_t)(pc + 1)) << 8)) : op;
}

uint8_t MdDebugger::GetOpSize(uint32_t pc)
{
	if(_cpuType == CpuType::MdM68k) {
		return MdDisUtils::GetOpSize(pc, MemoryType::MdMemory, _debugger->GetMemoryDumper());
	}
	return SmsDisUtils::GetOpSize((uint8_t)_prevOpCode, pc, MemoryType::MdZ80Memory, _debugger->GetMemoryDumper());
}

bool MdDebugger::IsJumpToSub(uint16_t op)
{
	return _cpuType == CpuType::MdM68k ? MdDisUtils::IsJumpToSub(op) : SmsDisUtils::IsJumpToSub((uint8_t)op);
}

bool MdDebugger::IsReturn(uint16_t op)
{
	return _cpuType == CpuType::MdM68k ? MdDisUtils::IsReturnInstruction(op) : SmsDisUtils::IsReturnInstruction(op);
}

void MdDebugger::ProcessInstruction()
{
	uint32_t pc = GetCurrentPc();
	AddressInfo addressInfo = GetAbsoluteAddress(pc);
	uint16_t opCode = ReadOpCode(pc);
	MemoryOperationInfo operation(pc, opCode & 0xFF, MemoryOperationType::ExecOpCode, _cpuMemType);
	InstructionProgress.LastMemOperation = operation;
	InstructionProgress.StartCycle = GetCpuCycleCount(false);

	if(addressInfo.Address >= 0) {
		if(addressInfo.Type == MemoryType::MdPrgRom) {
			CdlFlags::CdlFlags flags = _cpuType == CpuType::MdM68k ? MdDisUtils::GetOpFlags(_prevOpCode, pc, _prevProgramCounter, _prevOpSize) : SmsDisUtils::GetOpFlags((uint8_t)_prevOpCode, (uint16_t)pc, (uint16_t)_prevProgramCounter);
			_codeDataLogger->SetCode(addressInfo.Address, flags);
		}
		_disassembler->BuildCache(addressInfo, 0, _cpuType);
	}

	ProcessCallStackUpdates(addressInfo, pc, GetStackPointer());

	if(_traceLogger->IsEnabled()) {
		DisassemblyInfo disInfo = _disassembler->GetDisassemblyInfo(addressInfo, pc, 0, _cpuType);
		if(_cpuType == CpuType::MdM68k) {
			_m68kState = _m68k->GetState();
			((MdM68kTraceLogger*)_traceLogger.get())->Log(_m68kState, disInfo, operation, addressInfo);
		} else {
			((MdZ80TraceLogger*)_traceLogger.get())->Log(_z80->GetState(), disInfo, operation, addressInfo);
		}
	}
	_memoryAccessCounter->ProcessMemoryExec(addressInfo, _console->GetMasterClock());

	_prevOpCode = opCode;
	_prevProgramCounter = pc;
	_prevStackPointer = GetStackPointer();
	_prevOpSize = GetOpSize(pc);

	_step->ProcessCpuExec();
	_debugger->ProcessBreakConditions(_cpuType, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

template<typename T>
void MdDebugger::ProcessRead(uint32_t addr, T value, MemoryOperationType type)
{
	AddressInfo addressInfo = GetAbsoluteAddress(addr);
	MemoryOperationInfo operation(addr, value, type, _cpuMemType);
	InstructionProgress.LastMemOperation = operation;

	if(addressInfo.Address >= 0 && addressInfo.Type == MemoryType::MdPrgRom) {
		if(type == MemoryOperationType::ExecOperand) {
			_codeDataLogger->SetCode(addressInfo.Address);
		} else {
			_codeDataLogger->SetData(addressInfo.Address);
		}
	}
	if(_traceLogger->IsEnabled()) {
		if(_cpuType == CpuType::MdM68k) {
			((MdM68kTraceLogger*)_traceLogger.get())->LogNonExec(operation, addressInfo);
		} else {
			((MdZ80TraceLogger*)_traceLogger.get())->LogNonExec(operation, addressInfo);
		}
	}
	if(type == MemoryOperationType::ExecOperand) {
		_memoryAccessCounter->ProcessMemoryExec(addressInfo, _console->GetMasterClock());
	} else {
		ReadResult result = _memoryAccessCounter->ProcessMemoryRead(addressInfo, _console->GetMasterClock());
		if(result == ReadResult::FirstUninitRead) {
			_debugger->Log(string(_cpuType == CpuType::MdM68k ? "[68K]" : "[Z80]") + " Uninitialized memory read: $" + HexUtilities::ToHex24(addr));
		}
		if(_cpuType == CpuType::MdM68k && (addr & 0xE700E0) == 0xC00000) {
			_eventManager->AddEvent(DebugEventType::Register, operation);
		}
	}
	_step->ProcessCpuCycle();
	_debugger->ProcessBreakConditions(_cpuType, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

template<typename T>
void MdDebugger::ProcessWrite(uint32_t addr, T value, MemoryOperationType type)
{
	AddressInfo addressInfo = GetAbsoluteAddress(addr);
	MemoryOperationInfo operation(addr, value, type, _cpuMemType);
	InstructionProgress.LastMemOperation = operation;

	if(addressInfo.Type == MemoryType::MdWorkRam || addressInfo.Type == MemoryType::MdZ80Ram || addressInfo.Type == MemoryType::MdSaveRam) {
		_disassembler->InvalidateCache(addressInfo, _cpuType);
	}
	if(_traceLogger->IsEnabled()) {
		if(_cpuType == CpuType::MdM68k) {
			((MdM68kTraceLogger*)_traceLogger.get())->LogNonExec(operation, addressInfo);
		} else {
			((MdZ80TraceLogger*)_traceLogger.get())->LogNonExec(operation, addressInfo);
		}
	}
	if(_cpuType == CpuType::MdM68k && (addr & 0xE700E0) == 0xC00000) {
		_eventManager->AddEvent(DebugEventType::Register, operation);
	}
	_memoryAccessCounter->ProcessMemoryWrite(addressInfo, _console->GetMasterClock());
	_step->ProcessCpuCycle();
	_debugger->ProcessBreakConditions(_cpuType, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void MdDebugger::ProcessCallStackUpdates(AddressInfo& destAddr, uint32_t destPc, uint32_t sp)
{
	if(IsJumpToSub(_prevOpCode) && destPc != _prevProgramCounter + _prevOpSize) {
		uint32_t returnPc = _prevProgramCounter + _prevOpSize;
		AddressInfo src = GetAbsoluteAddress(_prevProgramCounter);
		AddressInfo ret = GetAbsoluteAddress(returnPc);
		_callstackManager->Push(src, _prevProgramCounter, destAddr, destPc, ret, returnPc, _prevStackPointer, StackFrameFlags::None);
	} else if(IsReturn(_prevOpCode)) {
		if(destPc != _prevProgramCounter + _prevOpSize) {
			_callstackManager->Pop(destAddr, destPc, sp);
		}
		if(_step->BreakAddress == (int32_t)destPc && _step->BreakStackPointer == sp) {
			_step->Break(BreakSource::CpuStep);
		}
	}
}

void MdDebugger::ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi)
{
	AddressInfo ret = GetAbsoluteAddress(originalPc);
	AddressInfo dest = GetAbsoluteAddress(currentPc);
	if(dest.Type == MemoryType::MdPrgRom && dest.Address >= 0) {
		_codeDataLogger->SetCode(dest.Address, CdlFlags::SubEntryPoint);
	}
	uint32_t sp = GetStackPointer();
	uint32_t originalSp = _cpuType == CpuType::MdM68k ? sp + 6 : sp + 2;
	_prevStackPointer = originalSp;
	ProcessCallStackUpdates(ret, originalPc, originalSp);
	ResetPrevOpCode();
	_debugger->InternalProcessInterrupt(_cpuType, *this, *_step.get(), ret, originalPc, dest, currentPc, ret, originalPc, originalSp, forNmi);
}

void MdDebugger::Run()
{
	_step.reset(new StepRequest());
}

void MdDebugger::Step(int32_t stepCount, StepType type)
{
	StepRequest step(type);
	switch(type) {
		case StepType::Step: step.StepCount = stepCount; break;
		case StepType::StepOut:
			step.BreakAddress = _callstackManager->GetReturnAddress();
			step.BreakStackPointer = _callstackManager->GetReturnStackPointer();
			break;
		case StepType::StepOver:
			if(IsJumpToSub(_prevOpCode)) {
				step.BreakAddress = _prevProgramCounter + _prevOpSize;
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

void MdDebugger::SetProgramCounter(uint32_t addr, bool updateDebuggerOnly)
{
	if(!updateDebuggerOnly) {
		if(_cpuType == CpuType::MdM68k) {
			MdM68kState state = _m68k->GetState();
			state.PC = addr & 0xFFFFFE;
			_m68k->SetState(state);
		} else {
			_z80->GetState().PC = (uint16_t)addr;
		}
	}
	_prevOpCode = ReadOpCode(addr);
	_prevProgramCounter = addr;
	_prevStackPointer = GetStackPointer();
	_prevOpSize = GetOpSize(addr);
}

uint32_t MdDebugger::GetProgramCounter(bool getInstPc)
{
	return getInstPc ? _prevProgramCounter : GetCurrentPc();
}

uint64_t MdDebugger::GetCpuCycleCount(bool forProfiler)
{
	return _cpuType == CpuType::MdM68k ? _m68k->GetCycleCount() : _z80->GetCycleCount();
}

void MdDebugger::ResetPrevOpCode()
{
	_prevOpCode = 0;
	_prevOpSize = 0;
}

DebuggerFeatures MdDebugger::GetSupportedFeatures()
{
	DebuggerFeatures features = {};
	features.RunToIrq = true;
	features.StepOver = true;
	features.StepOut = true;
	features.CallStack = true;
	features.CpuCycleStep = true;
	features.ChangeProgramCounter = AllowChangeProgramCounter;
	if(_cpuType == CpuType::MdM68k) {
		features.CpuVectors[0] = { "Reset", 0x04, VectorType::Indirect };
		features.CpuVectors[1] = { "HInt", 0x70, VectorType::Indirect };
		features.CpuVectors[2] = { "VInt", 0x78, VectorType::Indirect };
		features.CpuVectorCount = 3;
	} else {
		features.RunToNmi = true;
		features.CpuVectors[0] = { "IRQ", 0x38, VectorType::Direct };
		features.CpuVectors[1] = { "NMI", 0x66, VectorType::Direct };
		features.CpuVectorCount = 2;
	}
	return features;
}

BaseEventManager* MdDebugger::GetEventManager()
{
	return _eventManager.get();
}

PpuTools* MdDebugger::GetPpuTools()
{
	return _ppuTools.get();
}

CallstackManager* MdDebugger::GetCallstackManager()
{
	return _callstackManager.get();
}

BreakpointManager* MdDebugger::GetBreakpointManager()
{
	return _breakpointManager.get();
}

ITraceLogger* MdDebugger::GetTraceLogger()
{
	return _traceLogger.get();
}

ISerializable* MdDebugger::GetSerializableCpu()
{
	return _cpuType == CpuType::MdM68k ? (ISerializable*)_m68k : (ISerializable*)_z80;
}

BaseState& MdDebugger::GetState()
{
	if(_cpuType == CpuType::MdM68k) {
		_m68kState = _m68k->GetState();
		return _m68kState;
	}
	return _z80->GetState();
}

void MdDebugger::SetM68kState(MdM68kState& state)
{
	_m68k->SetState(state);
}

void MdDebugger::GetPpuState(BaseState& state)
{
	(MdVdpState&)state = _vdp->GetState();
}

void MdDebugger::SetPpuState(BaseState& srcState)
{
	MdVdpState& src = (MdVdpState&)srcState;
	for(int i = 0; i < 24; i++) {
		_vdp->DebugWriteRegister(i, src.Regs[i]);
	}
}

template void MdDebugger::ProcessRead<uint8_t>(uint32_t addr, uint8_t value, MemoryOperationType type);
template void MdDebugger::ProcessRead<uint16_t>(uint32_t addr, uint16_t value, MemoryOperationType type);
template void MdDebugger::ProcessRead<uint32_t>(uint32_t addr, uint32_t value, MemoryOperationType type);
template void MdDebugger::ProcessWrite<uint8_t>(uint32_t addr, uint8_t value, MemoryOperationType type);
template void MdDebugger::ProcessWrite<uint16_t>(uint32_t addr, uint16_t value, MemoryOperationType type);
template void MdDebugger::ProcessWrite<uint32_t>(uint32_t addr, uint32_t value, MemoryOperationType type);
