using System;
using System.Runtime.InteropServices;

namespace Mesen.Interop;

//Mirrors Core/MD/MdTypes.h (field order and types must match)
public struct MdM68kState : BaseState
{
	public UInt64 CycleCount;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public UInt32[] D;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public UInt32[] A;
	public UInt32 Usp;
	public UInt32 Ssp;
	public UInt32 PC;
	public UInt16 SR;
	public UInt16 Ir;
	public UInt16 Irc;
	public byte IrqLevel;
	[MarshalAs(UnmanagedType.I1)] public bool Stopped;
	[MarshalAs(UnmanagedType.I1)] public bool Halted;
}

public struct MdVdpState : BaseState
{
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 24)] public byte[] Regs;

	public UInt32 FrameCount;
	public UInt16 Scanline;
	public UInt16 HClock;

	public UInt16 Address;
	public byte Code;
	[MarshalAs(UnmanagedType.I1)] public bool CommandPending;
	public UInt16 FirstCommandWord;

	public byte HIntCounter;
	[MarshalAs(UnmanagedType.I1)] public bool HIntPending;
	[MarshalAs(UnmanagedType.I1)] public bool VIntPending;
	[MarshalAs(UnmanagedType.I1)] public bool Z80IntActive;

	[MarshalAs(UnmanagedType.I1)] public bool OddFrame;
	[MarshalAs(UnmanagedType.I1)] public bool SpriteOverflow;
	[MarshalAs(UnmanagedType.I1)] public bool SpriteCollision;

	[MarshalAs(UnmanagedType.I1)] public bool HvLatched;
	public UInt16 HvLatch;

	[MarshalAs(UnmanagedType.I1)] public bool DmaFillPending;
	public UInt16 DmaFillValue;
	public UInt64 DmaBusyUntil;

	public UInt16 ReadBuffer;
}

public struct MdIoState
{
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)] public byte[] Data;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)] public byte[] Ctrl;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)] public byte[] Serial;
}

public struct MdMemoryManagerState
{
	public UInt16 Z80Bank;
	[MarshalAs(UnmanagedType.I1)] public bool Z80BusRequested;
	[MarshalAs(UnmanagedType.I1)] public bool Z80Reset;
	[MarshalAs(UnmanagedType.I1)] public bool SramEnabled;
	[MarshalAs(UnmanagedType.I1)] public bool SramWriteProtected;
	[MarshalAs(UnmanagedType.I1)] public bool TmssUnlocked;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public byte[] Tmss;
	public MdIoState Io;
}

public struct MdState : BaseState
{
	public MdM68kState M68k;
	public SmsCpuState Z80;
	public MdVdpState Vdp;
	public MdMemoryManagerState MemoryManager;
}
