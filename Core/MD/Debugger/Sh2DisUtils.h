#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"

class LabelManager;
class EmuSettings;
class DisassemblyInfo;

// SH-2 (32X) disassembly and instruction classes for the debugger (Sh2Core::Disassemble).
// Every SH-2 instruction is 2 bytes; calls, returns and most jumps are delayed branches.
class Sh2DisUtils
{
public:
	static void GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings);
	static constexpr uint8_t GetOpSize() { return 2; }

	static bool IsJumpToSub(uint16_t op) { return (op & 0xF000) == 0xB000 || (op & 0xF0FF) == 0x400B || (op & 0xF0FF) == 0x0003; } //BSR, JSR, BSRF
	static bool IsReturnInstruction(uint16_t op) { return op == 0x000B || op == 0x002B; } //RTS, RTE
	static bool IsUnconditionalJump(uint16_t op) { return (op & 0xF000) == 0xA000 || (op & 0xF0FF) == 0x402B || (op & 0xF0FF) == 0x0023; } //BRA, JMP, BRAF
	static bool IsConditionalJump(uint16_t op) { return (op & 0xF900) == 0x8900; } //BT, BF, BT/S, BF/S
	static bool IsDelayedBranch(uint16_t op) { return IsJumpToSub(op) || IsReturnInstruction(op) || IsUnconditionalJump(op) || (op & 0xFD00) == 0x8D00; }
	static CdlFlags::CdlFlags GetOpFlags(uint16_t prevOp, uint32_t pc, uint32_t prevPc);
};
