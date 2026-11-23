#include "pch.h"
#include "MD/Debugger/Sh2DisUtils.h"
#include "MD/Sh2/Sh2Core.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/LabelManager.h"
#include "Shared/EmuSettings.h"

void Sh2DisUtils::GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings)
{
	(void)labelManager;
	uint8_t* byteCode = info.GetByteCode();
	uint16_t op = (uint16_t)(byteCode[0] << 8 | byteCode[1]);
	string text = Sh2Core::Disassemble(memoryAddr, op);
	if(settings->GetDebugConfig().UseLowerCaseDisassembly) {
		std::transform(text.begin(), text.end(), text.begin(), ::tolower);
	}
	out += text;
}

CdlFlags::CdlFlags Sh2DisUtils::GetOpFlags(uint16_t prevOp, uint32_t pc, uint32_t prevPc)
{
	if(pc != prevPc + 2) {
		if(IsJumpToSub(prevOp)) {
			return CdlFlags::SubEntryPoint;
		} else if(IsUnconditionalJump(prevOp) || IsConditionalJump(prevOp)) {
			return CdlFlags::JumpTarget;
		}
	}
	return CdlFlags::None;
}
