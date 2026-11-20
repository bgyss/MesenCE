#pragma once
#include "pch.h"
#include "Debugger/BaseTraceLogger.h"
#include "MD/MdTypes.h"

class DisassemblyInfo;
class Debugger;
class MdVdp;

// Trace logger for the 32X SH-2s (CpuType::M32xMaster / M32xSlave)
class Sh2TraceLogger : public BaseTraceLogger<Sh2TraceLogger, M32xSh2State>
{
private:
	MdVdp* _vdp = nullptr;

protected:
	RowDataType GetFormatTagType(string& tag) override;

public:
	Sh2TraceLogger(Debugger* debugger, IDebugger* cpuDebugger, MdVdp* vdp, CpuType cpuType);

	void GetTraceRow(string& output, M32xSh2State& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo);
	void LogPpuState();

	__forceinline uint32_t GetProgramCounter(M32xSh2State& state) { return state.PC; }
	__forceinline uint64_t GetCycleCount(M32xSh2State& state) { return state.CycleCount; }
	__forceinline uint8_t GetStackPointer(M32xSh2State& state) { return (uint8_t)state.R[15]; }
};
