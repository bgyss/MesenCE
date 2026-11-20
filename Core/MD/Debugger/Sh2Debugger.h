#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/IDebugger.h"
#include "MD/MdTypes.h"

class Debugger;
class Emulator;
class EmuSettings;
class Disassembler;
class MemoryAccessCounter;
class CallstackManager;
class BreakpointManager;
class CodeDataLogger;
class ITraceLogger;
class MdConsole;
class MdVdp;
class M32xUnit;
class M32xSh2;
class MdEventManager;
class MdVdpTools;
class Sh2TraceLogger;
class IAssembler;

enum class MemoryOperationType;

// Debugger for one of the 32X's SH-2s (CpuType::M32xMaster or CpuType::M32xSlave): breakpoints,
// stepping, call stack (delayed branches: a call or return takes effect after its delay slot),
// code/data logging of SDRAM, trace logging and interrupts.
class Sh2Debugger final : public IDebugger
{
	Debugger* _debugger;
	Emulator* _emu;
	EmuSettings* _settings;
	CpuType _cpuType;
	MemoryType _cpuMemType;
	MdConsole* _console;
	MdVdp* _vdp;
	M32xUnit* _unit;
	M32xSh2* _cpu;
	Disassembler* _disassembler;
	MemoryAccessCounter* _memoryAccessCounter;

	unique_ptr<MdEventManager> _eventManager;
	unique_ptr<MdVdpTools> _ppuTools;
	unique_ptr<CallstackManager> _callstackManager;
	unique_ptr<CodeDataLogger> _codeDataLogger;
	unique_ptr<BreakpointManager> _breakpointManager;
	unique_ptr<Sh2TraceLogger> _traceLogger;

	M32xSh2State _state = {};

	uint16_t _prevOpCode = 0;
	uint32_t _prevProgramCounter = 0;
	uint32_t _prevStackPointer = 0;

	//A call or return whose delay slot is executing; resolved on the next instruction
	bool _callPending = false;
	uint32_t _callSource = 0;
	uint32_t _callReturn = 0;
	uint32_t _callStackPointer = 0;
	bool _returnPending = false;

	uint32_t GetCurrentPc();
	uint32_t GetStackPointer();
	uint16_t ReadOpCode(uint32_t pc);
	AddressInfo GetAbsoluteAddress(uint32_t addr);

public:
	Sh2Debugger(Debugger* debugger, CpuType cpuType);
	~Sh2Debugger();

	void Reset() override;

	void ProcessInstruction();
	template<typename T>
	void ProcessRead(uint32_t addr, T value, MemoryOperationType type);
	template<typename T>
	void ProcessWrite(uint32_t addr, T value, MemoryOperationType type);
	void ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi) override;

	void Run() override;
	void Step(int32_t stepCount, StepType type) override;

	void SetProgramCounter(uint32_t addr, bool updateDebuggerOnly = false) override;
	uint32_t GetProgramCounter(bool getInstPc) override;
	uint64_t GetCpuCycleCount(bool forProfiler) override;
	void ResetPrevOpCode() override;

	DebuggerFeatures GetSupportedFeatures() override;

	BaseEventManager* GetEventManager() override;
	PpuTools* GetPpuTools() override;
	IAssembler* GetAssembler() override { return nullptr; }
	CallstackManager* GetCallstackManager() override;
	BreakpointManager* GetBreakpointManager() override;
	ITraceLogger* GetTraceLogger() override;
	ISerializable* GetSerializableCpu() override;

	BaseState& GetState() override;
	void SetSh2State(M32xSh2State& state);
	void GetPpuState(BaseState& state) override;
	void SetPpuState(BaseState& state) override;
};
