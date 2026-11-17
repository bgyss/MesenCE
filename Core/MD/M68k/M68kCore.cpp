#include "MD/M68k/M68kCore.h"
#if defined(__clang__)
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wparentheses"
	#pragma clang diagnostic ignored "-Wbitwise-op-parentheses"
	#pragma clang diagnostic ignored "-Wlogical-op-parentheses"
	#pragma clang diagnostic ignored "-Wunused-variable"
#elif defined(__GNUC__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wparentheses"
	#pragma GCC diagnostic ignored "-Wunused-variable"
#elif defined(_MSC_VER)
	//Vendored ares code: no MSVC warnings in this region (it is checked by clang/GCC and the suite)
	#pragma warning(push, 0)
#endif
#include "MD/M68k/ares/m68000.inc"
#if defined(__clang__)
	#pragma clang diagnostic pop
#elif defined(__GNUC__)
	#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
	#pragma warning(pop)
#endif

namespace
{
	struct AddressErrorAbort
	{
		uint32_t Address;
		bool Write;
		bool Program;
	};
}

M68kCore::M68kCore()
{
	ares::M68000::power();
}

void M68kCore::PowerOn()
{
	ares::M68000::power();
	_irqLevel = 0;
	_nmiPending = false;
}

void M68kCore::Reset()
{
	_halted = false;
	r.stop = false;
	r.s = 1;
	r.t = 0;
	r.i = 7;
	uint32_t ssp = read<Long>(0);
	r.a[7] = ssp;
	r.pc = read<Long>(4);
	prefetch();
	prefetch();
}

void M68kCore::SetInterruptLevel(uint8_t level)
{
	level &= 7;
	if(level == 7 && _irqLevel != 7) {
		_nmiPending = true;
	}
	_irqLevel = level;
}

void M68kCore::Step()
{
	if(_halted) {
		Idle(4);
		return;
	}
	try {
		uint8_t level = 0;
		if(_nmiPending) {
			level = 7;
		} else if(_irqLevel > r.i) {
			level = _irqLevel;
		}

		if(level) {
			if(level == 7) {
				_nmiPending = false;
			}
			uint32_t vector = AcknowledgeInterrupt(level);
			if(r.stop) {
				// STOP left PC at its own opcode + 4; resume after the operand word.
				r.pc += 4;
				r.ir = r.irc = 0;
			}
			// exception() computes the stacked PC as r.pc - 4 (the next instruction's address).
			uint32_t originalPc = (uint32_t)r.pc - 4;
			interrupt(vector, level);
			OnInterruptTaken(originalPc, (uint32_t)r.pc - 4);
			return;
		}
		_instructionStart = (uint32_t)r.pc - 4;
		faultMode = 0;
		faultUpdate.pending = false;
		lowWordFirst = false;
		postIncFault = -1;
		irLatch = r.ir; // the opcode about to run (r.ird is loaded by instruction())
		fetchFault = false;
		_ccrAtStart = readCCR();
		if(!r.stop) {
			OnInstructionStart();
		}
		instruction();
	} catch(const AddressErrorAbort& ae) {
		try {
			AddressErrorException({ ae.Address, ae.Write, ae.Program });
		} catch(const AddressErrorAbort&) {
			// Address error while stacking a group 0 frame: double bus fault, the CPU halts
			// until reset (MC68000 UM 6.3.9).
			_halted = true;
		}
	}
}

auto M68kCore::addressError(bool write, n32 address, bool upper, bool lower) -> void
{
	AddressErrorCycle(write, address, upper, lower);
	throw AddressErrorAbort { (uint32_t)address, write, access == Access::Program };
}

void M68kCore::AddressErrorException(const AddressErrorInfo& info)
{
	// Group 0 exception frame (MC68000 UM 6.3.9.1), lowest address first: special status
	// word, access address (long), instruction register, SR, PC (long). Write order, the
	// stacked PC and the status word's upper bits (IR) follow SingleStepTests/m68000.
	access = Access::Data;
	r.stop = false;
	if(faultUpdate.pending) {
		r.a[faultUpdate.reg & 7] = faultUpdate.value;
		faultUpdate.pending = false;
	}
	if((r.ird & 0xF000) == 0x2000 && faultWrite) {
		// A long MOVE whose write faults has set N and Z from the low word only, X is kept, V and C
		// are cleared (SingleStepTests/m68000).
		// With a register or immediate source the flags are not touched at all.
		uint16_t srcMode = (r.ird >> 3) & 7;
		if(srcMode <= 1 || (r.ird & 0x3F) == 0x3C) {
			writeCCR(_ccrAtStart);
		} else {
			uint16_t lo = (uint16_t)moveData;
			writeCCR((_ccrAtStart & 0x10) | (lo & 0x8000 ? 0x08 : 0) | (lo == 0 ? 0x04 : 0));
		}
	}
	uint16_t sr = readSR();
	uint16_t status = (irLatch & 0xFFE0) | (info.Write ? 0 : 0x10) | 0 /* I/N: MAME (and the suite) leave it clear */ | (r.s ? 4 : 0) | (info.Program ? 2 : 1);
	if(!r.s) {
		std::swap(r.a[7], r.sp);
	}
	r.s = 1;
	r.t = 0;
	uint32_t pc = r.pc - 2;
	if(fetchFault) {
		// Faulting program fetch (odd jump/branch/return target): the address after the
		// instruction; BSR stacks the target itself (SingleStepTests/m68000).
		if((r.ird & 0xFF00) == 0x6100) {
			pc = info.Address;
		} else if((r.ird & 0xFFC0) == 0x4EC0) {
			pc = _instructionStart + 2; // JMP
		} else {
			uint32_t length = 0;
			uint32_t savedPc = _pc;
			Disassemble(_instructionStart, length);
			_pc = savedPc;
			pc = _instructionStart + length;
		}
	} else if((r.ird & 0xFFF8) == 0x4E58 || (r.ird & 0xF138) == 0xB108 || (r.ird & 0xB130) == 0x9100) {
		pc = r.pc; // UNLK, CMPM, ADDX/SUBX
	} else if((r.ird & 0xC000) == 0 && (r.ird & 0x3000) != 0 && faultWrite) {
		// MOVE destination
		bool indexed = faultMode == AddressRegisterIndirectWithDisplacement || faultMode == AddressRegisterIndirectWithIndex || faultMode == AbsoluteShortIndirect || faultMode == AbsoluteLongIndirect;
		pc = (indexed || faultMode == AddressRegisterIndirectWithPreDecrement) ? (uint32_t)r.pc - 2 : (uint32_t)r.pc;
	} else if(faultMode == AddressRegisterIndirectWithPreDecrement && faultLong) {
		pc = r.pc - 2;
	} else if((r.ird & 0xFB80) == 0x4880) {
		// MOVEM: the register mask word is counted.
		pc = r.pc;
		if(faultMode == AddressRegisterIndirectWithDisplacement || faultMode == AddressRegisterIndirectWithIndex || faultMode == ProgramCounterIndirectWithDisplacement || faultMode == ProgramCounterIndirectWithIndex) {
			pc = r.pc - 2;
		}
	} else if(faultMode == AddressRegisterIndirectWithPreDecrement) {
		pc = r.pc;
	} else if(faultMode == AddressRegisterIndirectWithDisplacement || faultMode == AddressRegisterIndirectWithIndex || faultMode == ProgramCounterIndirectWithDisplacement || faultMode == ProgramCounterIndirectWithIndex) {
		pc = r.pc - 4;
	}
	uint32_t sp = r.a[7];
	idle(8);
	write<Word>(sp - 2, pc & 0xffff);
	write<Word>(sp - 6, sr);
	write<Word>(sp - 4, pc >> 16);
	write<Word>(sp - 8, irLatch);
	write<Word>(sp - 10, info.Address & 0xffff);
	write<Word>(sp - 14, status);
	write<Word>(sp - 12, info.Address >> 16);
	r.a[7] = sp - 14;
	r.pc = read<Long>(ares::M68000::Vector::AddressError << 2);
	prefetch();
	idle(2);
	prefetch();
}

M68kRegisters M68kCore::GetRegisters() const
{
	M68kRegisters st = {};
	for(int i = 0; i < 8; i++) {
		st.D[i] = r.d[i];
	}
	for(int i = 0; i < 7; i++) {
		st.A[i] = r.a[i];
	}
	if(r.s) {
		st.Ssp = r.a[7];
		st.Usp = r.sp;
	} else {
		st.Usp = r.a[7];
		st.Ssp = r.sp;
	}
	st.Pc = r.pc;
	st.Sr = GetSr();
	st.Ir = r.ir;
	st.Irc = r.irc;
	st.Stopped = r.stop;
	return st;
}

void M68kCore::SetRegisters(const M68kRegisters& st)
{
	for(int i = 0; i < 8; i++) {
		r.d[i] = st.D[i];
	}
	for(int i = 0; i < 7; i++) {
		r.a[i] = st.A[i];
	}
	n16 sr = st.Sr;
	r.c = sr.bit(0);
	r.v = sr.bit(1);
	r.z = sr.bit(2);
	r.n = sr.bit(3);
	r.x = sr.bit(4);
	r.i = sr.bit(8, 10);
	r.s = sr.bit(13);
	r.t = sr.bit(15);
	if(r.s) {
		r.a[7] = st.Ssp;
		r.sp = st.Usp;
	} else {
		r.a[7] = st.Usp;
		r.sp = st.Ssp;
	}
	r.pc = st.Pc;
	r.ir = st.Ir;
	r.ird = st.Ir;
	r.irc = st.Irc;
	r.stop = st.Stopped;
	_halted = false;
}

std::string M68kCore::Disassemble(uint32_t pc, uint32_t& size)
{
	_pc = pc;
	uint16_t opcode = Peek(pc & 0xFFFFFE);
	_pc += 2;
	std::string text = disassembleTable[opcode]();
	size = (uint32_t)_pc - pc;
	return text;
}
