#pragma once
#include "pch.h"
#include "Debugger/BaseTraceLogger.h"
#include "MD/MdTypes.h"

class DisassemblyInfo;
class Debugger;
class MdVdp;

class MdM68kTraceLogger : public BaseTraceLogger<MdM68kTraceLogger, MdM68kState>
{
private:
	MdVdp* _vdp = nullptr;

protected:
	RowDataType GetFormatTagType(string& tag) override;

public:
	MdM68kTraceLogger(Debugger* debugger, IDebugger* cpuDebugger, MdVdp* vdp);

	void GetTraceRow(string& output, MdM68kState& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo);
	void LogPpuState();

	__forceinline uint32_t GetProgramCounter(MdM68kState& state) { return state.PC; }
	__forceinline uint64_t GetCycleCount(MdM68kState& state) { return state.CycleCount; }
	__forceinline uint8_t GetStackPointer(MdM68kState& state) { return (uint8_t)state.A[7]; }
};

class MdZ80TraceLogger : public BaseTraceLogger<MdZ80TraceLogger, MdZ80State>
{
private:
	MdVdp* _vdp = nullptr;

protected:
	RowDataType GetFormatTagType(string& tag) override;

public:
	MdZ80TraceLogger(Debugger* debugger, IDebugger* cpuDebugger, MdVdp* vdp);

	void GetTraceRow(string& output, MdZ80State& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo);
	void LogPpuState();

	__forceinline uint32_t GetProgramCounter(MdZ80State& state) { return state.PC; }
	__forceinline uint64_t GetCycleCount(MdZ80State& state) { return state.CycleCount; }
	__forceinline uint8_t GetStackPointer(MdZ80State& state) { return (uint8_t)state.SP; }
};
