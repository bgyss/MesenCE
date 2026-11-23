#pragma once
#include "pch.h"
#include "Shared/BaseState.h"
#include "SMS/SmsTypes.h"

// Master clock: NTSC 53.693175 MHz (15 x 3.579545 MHz colorburst), PAL 53.203424 MHz
// (12 x 4.43361875 MHz). 68000 = MCLK/7, Z80 = MCLK/15, one scanline = 3420 MCLK.
namespace MdTiming
{
	constexpr uint32_t NtscMasterClockRate = 53693175;
	constexpr uint32_t PalMasterClockRate = 53203424;
	constexpr uint32_t M68kDivider = 7;
	constexpr uint32_t Z80Divider = 15;
	constexpr uint32_t ClocksPerLine = 3420;
	constexpr uint32_t NtscLines = 262;
	constexpr uint32_t PalLines = 313;
}

struct MdM68kState : public BaseState
{
	uint64_t CycleCount; //68000 clocks since power-on
	uint32_t D[8];
	uint32_t A[8]; //A7 is the active stack pointer
	uint32_t Usp;
	uint32_t Ssp;
	uint32_t PC; //address of the next instruction
	uint16_t SR;
	uint16_t Ir;
	uint16_t Irc;
	uint8_t IrqLevel;
	bool Stopped;
	bool Halted;
};

//32X SH-2 (master or slave) registers for the debugger
struct M32xSh2State : public BaseState
{
	uint64_t CycleCount; //SH-2 clocks since power-on
	uint32_t R[16];
	uint32_t PC; //address of the next instruction
	uint32_t PR;
	uint32_t GBR;
	uint32_t VBR;
	uint32_t MACH;
	uint32_t MACL;
	uint32_t SR;
	bool Sleeping;
};

//The Mega Drive's Z80 runs the shared Z80 engine and uses the same register state.
using MdZ80State = SmsCpuState;

struct MdVdpState : public BaseState
{
	uint8_t Regs[24];

	uint32_t FrameCount;
	uint16_t Scanline;
	uint16_t HClock; //master clocks into the current line (the line starts where the V counter increments)

	uint16_t Address;
	uint8_t Code;
	bool CommandPending;
	uint16_t FirstCommandWord;

	uint8_t HIntCounter;
	bool HIntPending;
	bool VIntPending;
	bool Z80IntActive;

	bool OddFrame;
	bool SpriteOverflow;
	bool SpriteCollision;

	bool HvLatched;
	uint16_t HvLatch;

	bool DmaFillPending;
	uint16_t DmaFillValue;
	uint64_t DmaBusyUntil; //master clock at which a fill or copy completes

	uint16_t ReadBuffer;
};

struct MdIoState
{
	uint8_t Data[3];
	uint8_t Ctrl[3];
	uint8_t Serial[3];
};

struct MdMemoryManagerState
{
	uint16_t Z80Bank; //68000 address bits 23-15 of the Z80 bank window
	bool Z80BusRequested;
	bool Z80Reset;
	bool SramEnabled;
	bool SramWriteProtected;
	bool TmssUnlocked;
	uint8_t Tmss[4];
	MdIoState Io;
};

// Bus trace record (the NMDT equivalent; see docs/research/md-nativekit-trace-requirements.md).
enum class MdBusDevice : uint8_t
{
	M68k = 1,
	Z80 = 2,
	Vdp = 3,
	Ym2612 = 4,
	Psg = 5
};

enum class MdBusOp : uint8_t
{
	Fetch = 1,
	Read = 2,
	Write = 3,
	Dma = 4,
	Interrupt = 5
};

struct MdBusEvent
{
	uint64_t Clock; //master clock at the access
	MdBusDevice Device;
	MdBusOp Op;
	uint8_t Width; //bytes
	uint32_t Address;
	uint32_t Value;
};

struct MdState
{
	MdM68kState M68k;
	MdZ80State Z80;
	MdVdpState Vdp;
	MdMemoryManagerState MemoryManager;
};
