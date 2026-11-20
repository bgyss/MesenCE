#include "pch.h"
#include "MD/Debugger/Sh2TraceLogger.h"
#include "MD/MdVdp.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugTypes.h"

Sh2TraceLogger::Sh2TraceLogger(Debugger* debugger, IDebugger* cpuDebugger, MdVdp* vdp, CpuType cpuType) : BaseTraceLogger(debugger, cpuDebugger, cpuType)
{
	_vdp = vdp;
}

RowDataType Sh2TraceLogger::GetFormatTagType(string& tag)
{
	for(int i = 0; i < 16; i++) {
		if(tag == "R" + std::to_string(i)) {
			return (RowDataType)((int)RowDataType::R0 + i);
		}
	}
	if(tag == "SR") {
		return RowDataType::SR;
	} else if(tag == "SP") {
		return RowDataType::SP;
	} else if(tag == "PS") {
		return RowDataType::PS;
	}
	return RowDataType::Text;
}

void Sh2TraceLogger::GetTraceRow(string& output, M32xSh2State& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo)
{
	//SR flags: M Q S T
	constexpr char activeStatusLetters[4] = { 'M', 'Q', 'S', 'T' };
	constexpr char inactiveStatusLetters[4] = { 'm', 'q', 's', 't' };
	for(RowPart& rowPart : _rowParts) {
		int r = (int)rowPart.DataType - (int)RowDataType::R0;
		if(r >= 0 && r < 16) {
			WriteIntValue(output, cpuState.R[r], rowPart);
			continue;
		}
		switch(rowPart.DataType) {
			case RowDataType::SR: WriteIntValue(output, cpuState.SR, rowPart); break;
			case RowDataType::SP: WriteIntValue(output, cpuState.R[15], rowPart); break;
			case RowDataType::PS: {
				uint8_t flags = (uint8_t)(((cpuState.SR >> 9) & 1) << 3 | ((cpuState.SR >> 8) & 1) << 2 | ((cpuState.SR >> 1) & 1) << 1 | (cpuState.SR & 1));
				GetStatusFlag(activeStatusLetters, inactiveStatusLetters, output, flags, rowPart, 4);
				break;
			}
			default: ProcessSharedTag(rowPart, output, cpuState, ppuState, disassemblyInfo); break;
		}
	}
}

void Sh2TraceLogger::LogPpuState()
{
	MdVdpState& state = _vdp->GetState();
	_ppuState[_currentPos] = { state.HClock, state.HClock, state.Scanline, state.FrameCount };
}
