#include "pch.h"
#include "MD/Debugger/MdDisUtils.h"
#include "MD/M68k/M68kCore.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/MemoryDumper.h"
#include "Debugger/LabelManager.h"
#include "Shared/EmuSettings.h"
#include "Shared/MemoryType.h"

namespace
{
	// An M68kCore whose only use is the disassembler; its "bus" is a byte provider.
	class DisassemblerCpu final : public M68kCore
	{
	public:
		std::function<uint8_t(uint32_t)> ReadByte;

		void Idle(uint32_t) override {}
		void Wait(uint32_t) override {}
		uint16_t BusRead(bool, bool, uint32_t, M68kAccess) override { return 0; }
		void BusWrite(bool, bool, uint32_t, uint16_t, M68kAccess) override {}
		uint16_t Peek(uint32_t address) override { return (ReadByte(address & 0xFFFFFE) << 8) | ReadByte((address & 0xFFFFFE) | 1); }
	};

	DisassemblerCpu& GetDisassembler()
	{
		//Building the 64K-entry decode tables is costly; one instance per thread is kept
		thread_local DisassemblerCpu cpu;
		return cpu;
	}
}

void MdDisUtils::GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings)
{
	DisassemblerCpu& cpu = GetDisassembler();
	uint8_t* byteCode = info.GetByteCode();
	uint8_t size = info.GetOpSize();
	cpu.ReadByte = [=](uint32_t addr) -> uint8_t {
		uint32_t offset = (addr - memoryAddr) & 0xFFFFFF;
		return offset < size && offset < 10 ? byteCode[offset] : 0;
	};
	uint32_t length = 0;
	string text = cpu.Disassemble(memoryAddr, length);
	//The ares disassembler pads mnemonics with spaces; collapse them to Mesen's single-space style
	size_t space = text.find(' ');
	if(space != string::npos) {
		size_t operands = text.find_first_not_of(' ', space);
		text = operands == string::npos ? text.substr(0, space) : text.substr(0, space) + " " + text.substr(operands);
	}
	if(!settings->GetDebugConfig().UseLowerCaseDisassembly) {
		std::transform(text.begin(), text.end(), text.begin(), ::toupper);
	}
	out += text;
}

uint8_t MdDisUtils::GetOpSize(uint32_t cpuAddress, MemoryType memType, MemoryDumper* memoryDumper)
{
	DisassemblerCpu& cpu = GetDisassembler();
	cpu.ReadByte = [=](uint32_t addr) -> uint8_t { return memoryDumper->GetMemoryValue(memType, addr); };
	uint32_t length = 0;
	cpu.Disassemble(cpuAddress, length);
	return (uint8_t)std::clamp<uint32_t>(length, 2, 10);
}

bool MdDisUtils::IsJumpToSub(uint16_t opCode)
{
	return (opCode & 0xFFC0) == 0x4E80 || (opCode & 0xFF00) == 0x6100; //JSR, BSR
}

bool MdDisUtils::IsReturnInstruction(uint16_t opCode)
{
	return opCode == 0x4E75 || opCode == 0x4E73 || opCode == 0x4E77; //RTS, RTE, RTR
}

bool MdDisUtils::IsUnconditionalJump(uint16_t opCode)
{
	return (opCode & 0xFFC0) == 0x4EC0 || (opCode & 0xFF00) == 0x6000; //JMP, BRA
}

bool MdDisUtils::IsConditionalJump(uint16_t opCode)
{
	return ((opCode & 0xF000) == 0x6000 && (opCode & 0x0F00) >= 0x0200) || (opCode & 0xF0F8) == 0x50C8; //Bcc, DBcc
}

CdlFlags::CdlFlags MdDisUtils::GetOpFlags(uint16_t opCode, uint32_t pc, uint32_t prevPc, uint8_t prevOpSize)
{
	if(pc != prevPc + prevOpSize) {
		if(IsJumpToSub(opCode)) {
			return CdlFlags::SubEntryPoint;
		} else if(IsUnconditionalJump(opCode) || IsConditionalJump(opCode)) {
			return CdlFlags::JumpTarget;
		}
	}
	return CdlFlags::None;
}
