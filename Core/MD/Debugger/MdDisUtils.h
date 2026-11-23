#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"

class LabelManager;
class EmuSettings;
class MemoryDumper;
class DisassemblyInfo;

// 68000 disassembly for the debugger, using the ares-derived M68kCore disassembler.
class MdDisUtils
{
public:
	static void GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings);
	static uint8_t GetOpSize(uint32_t cpuAddress, MemoryType memType, MemoryDumper* memoryDumper);

	static bool IsJumpToSub(uint16_t opCode);
	static bool IsReturnInstruction(uint16_t opCode);
	static bool IsUnconditionalJump(uint16_t opCode);
	static bool IsConditionalJump(uint16_t opCode);
	static CdlFlags::CdlFlags GetOpFlags(uint16_t opCode, uint32_t pc, uint32_t prevPc, uint8_t prevOpSize);
};
