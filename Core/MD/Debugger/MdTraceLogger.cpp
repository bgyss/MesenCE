#include "pch.h"
#include "MD/Debugger/MdTraceLogger.h"
#include "MD/MdVdp.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugTypes.h"
#include "Utilities/HexUtilities.h"

MdM68kTraceLogger::MdM68kTraceLogger(Debugger* debugger, IDebugger* cpuDebugger, MdVdp* vdp) : BaseTraceLogger(debugger, cpuDebugger, CpuType::MdM68k)
{
	_vdp = vdp;
}

RowDataType MdM68kTraceLogger::GetFormatTagType(string& tag)
{
	static const char* names[16] = { "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7" };
	for(int i = 0; i < 16; i++) {
		if(tag == names[i]) {
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

void MdM68kTraceLogger::GetTraceRow(string& output, MdM68kState& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo)
{
	constexpr char activeStatusLetters[5] = { 'X', 'N', 'Z', 'V', 'C' };
	constexpr char inactiveStatusLetters[5] = { 'x', 'n', 'z', 'v', 'c' };
	for(RowPart& rowPart : _rowParts) {
		int r = (int)rowPart.DataType - (int)RowDataType::R0;
		if(r >= 0 && r < 16) {
			WriteIntValue(output, r < 8 ? cpuState.D[r] : cpuState.A[r - 8], rowPart);
			continue;
		}
		switch(rowPart.DataType) {
			case RowDataType::SR: WriteIntValue(output, cpuState.SR, rowPart); break;
			case RowDataType::SP: WriteIntValue(output, cpuState.A[7], rowPart); break;
			case RowDataType::PS: GetStatusFlag(activeStatusLetters, inactiveStatusLetters, output, cpuState.SR & 0x1F, rowPart, 5); break;
			default: ProcessSharedTag(rowPart, output, cpuState, ppuState, disassemblyInfo); break;
		}
	}
}

void MdM68kTraceLogger::LogPpuState()
{
	MdVdpState& state = _vdp->GetState();
	_ppuState[_currentPos] = { state.HClock, state.HClock, state.Scanline, state.FrameCount };
}

MdZ80TraceLogger::MdZ80TraceLogger(Debugger* debugger, IDebugger* cpuDebugger, MdVdp* vdp) : BaseTraceLogger(debugger, cpuDebugger, CpuType::MdZ80)
{
	_vdp = vdp;
}

RowDataType MdZ80TraceLogger::GetFormatTagType(string& tag)
{
	static const std::pair<const char*, RowDataType> tags[] = {
		{ "A", RowDataType::A }, { "B", RowDataType::B }, { "C", RowDataType::C }, { "D", RowDataType::D }, { "E", RowDataType::E }, { "F", RowDataType::F }, { "H", RowDataType::H }, { "L", RowDataType::L }, { "A'", RowDataType::AltA }, { "B'", RowDataType::AltB }, { "C'", RowDataType::AltC }, { "D'", RowDataType::AltD }, { "E'", RowDataType::AltE }, { "F'", RowDataType::AltF }, { "H'", RowDataType::AltH }, { "L'", RowDataType::AltL }, { "I", RowDataType::I }, { "R", RowDataType::R }, { "IX", RowDataType::IX }, { "IY", RowDataType::IY }, { "PS", RowDataType::PS }, { "SP", RowDataType::SP }
	};
	for(auto& t : tags) {
		if(tag == t.first) {
			return t.second;
		}
	}
	return RowDataType::Text;
}

void MdZ80TraceLogger::GetTraceRow(string& output, MdZ80State& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo)
{
	constexpr char activeStatusLetters[8] = { 'S', 'Z', '5', 'H', '3', 'P', 'N', 'C' };
	constexpr char inactiveStatusLetters[8] = { 's', 'z', '-', 'h', '-', 'p', 'n', 'c' };
	for(RowPart& rowPart : _rowParts) {
		switch(rowPart.DataType) {
			case RowDataType::A: WriteIntValue(output, cpuState.A, rowPart); break;
			case RowDataType::B: WriteIntValue(output, cpuState.B, rowPart); break;
			case RowDataType::C: WriteIntValue(output, cpuState.C, rowPart); break;
			case RowDataType::D: WriteIntValue(output, cpuState.D, rowPart); break;
			case RowDataType::E: WriteIntValue(output, cpuState.E, rowPart); break;
			case RowDataType::F: WriteIntValue(output, cpuState.Flags, rowPart); break;
			case RowDataType::H: WriteIntValue(output, cpuState.H, rowPart); break;
			case RowDataType::L: WriteIntValue(output, cpuState.L, rowPart); break;
			case RowDataType::I: WriteIntValue(output, cpuState.I, rowPart); break;
			case RowDataType::R: WriteIntValue(output, cpuState.R, rowPart); break;
			case RowDataType::AltA: WriteIntValue(output, cpuState.AltA, rowPart); break;
			case RowDataType::AltB: WriteIntValue(output, cpuState.AltB, rowPart); break;
			case RowDataType::AltC: WriteIntValue(output, cpuState.AltC, rowPart); break;
			case RowDataType::AltD: WriteIntValue(output, cpuState.AltD, rowPart); break;
			case RowDataType::AltE: WriteIntValue(output, cpuState.AltE, rowPart); break;
			case RowDataType::AltF: WriteIntValue(output, cpuState.AltFlags, rowPart); break;
			case RowDataType::AltH: WriteIntValue(output, cpuState.AltH, rowPart); break;
			case RowDataType::AltL: WriteIntValue(output, cpuState.AltL, rowPart); break;
			case RowDataType::IX: WriteIntValue(output, (uint16_t)(cpuState.IXL | (cpuState.IXH << 8)), rowPart); break;
			case RowDataType::IY: WriteIntValue(output, (uint16_t)(cpuState.IYL | (cpuState.IYH << 8)), rowPart); break;
			case RowDataType::SP: WriteIntValue(output, cpuState.SP, rowPart); break;
			case RowDataType::PS: GetStatusFlag(activeStatusLetters, inactiveStatusLetters, output, cpuState.Flags, rowPart, 8); break;
			default: ProcessSharedTag(rowPart, output, cpuState, ppuState, disassemblyInfo); break;
		}
	}
}

void MdZ80TraceLogger::LogPpuState()
{
	MdVdpState& state = _vdp->GetState();
	_ppuState[_currentPos] = { state.HClock, state.HClock, state.Scanline, state.FrameCount };
}
