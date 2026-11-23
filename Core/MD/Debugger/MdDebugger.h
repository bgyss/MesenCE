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
class MdM68k;
class MdZ80;
class MdVdp;
class MdMemoryManager;
class McdUnit;
class M68kCore;
class MdEventManager;
class MdM68kTraceLogger;
class MdZ80TraceLogger;
class MdVdpTools;
class IAssembler;

enum class MemoryOperationType;

// Debugger for one of the Mega Drive's CPUs (CpuType::MdM68k or CpuType::MdZ80): breakpoints,
// stepping, call stack, code/data logging, trace logging, events and interrupts.
class MdDebugger final : public IDebugger
{
	Debugger* _debugger;
	Emulator* _emu;
	EmuSettings* _settings;
	CpuType _cpuType;
	MemoryType _cpuMemType;
	MdConsole* _console;
	MdM68k* _m68k;
	MdZ80* _z80;
	MdVdp* _vdp;
	MdMemoryManager* _memoryManager;
	McdUnit* _mcd = nullptr; //Sega CD unit (for CpuType::McdM68k, its sub CPU)
	bool Is68k() { return _cpuType != CpuType::MdZ80; }
	M68kCore* Get68k();
	Disassembler* _disassembler;
	MemoryAccessCounter* _memoryAccessCounter;

	unique_ptr<MdEventManager> _eventManager;
	unique_ptr<MdVdpTools> _ppuTools;
	unique_ptr<CallstackManager> _callstackManager;
	unique_ptr<CodeDataLogger> _codeDataLogger;
	unique_ptr<BreakpointManager> _breakpointManager;
	//Concrete types: ITraceLogger has no virtual destructor, so deleting through it is undefined
	unique_ptr<MdM68kTraceLogger> _m68kTraceLogger;
	unique_ptr<MdZ80TraceLogger> _z80TraceLogger;
	ITraceLogger* _traceLogger = nullptr; //the one in use for this CPU

	MdM68kState _m68kState = {};

	uint16_t _prevOpCode = 0;
	uint32_t _prevStackPointer = 0;
	uint32_t _prevProgramCounter = 0;
	uint8_t _prevOpSize = 0;

	string _cdlFile;

	uint32_t GetCurrentPc();
	uint32_t GetStackPointer();
	uint8_t GetOpSize(uint32_t pc);
	uint16_t ReadOpCode(uint32_t pc);
	bool IsJumpToSub(uint16_t op);
	bool IsReturn(uint16_t op);
	void ProcessCallStackUpdates(AddressInfo& destAddr, uint32_t destPc, uint32_t sp);
	AddressInfo GetAbsoluteAddress(uint32_t addr);

public:
	MdDebugger(Debugger* debugger, CpuType cpuType);
	~MdDebugger();

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
	void SetM68kState(MdM68kState& state);
	void GetPpuState(BaseState& state) override;
	void SetPpuState(BaseState& state) override;
};
