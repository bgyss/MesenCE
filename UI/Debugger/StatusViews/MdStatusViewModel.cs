using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using Mesen.Utilities;
using System;
using System.Text;

namespace Mesen.Debugger.StatusViews
{
	//Mega Drive 68000. A7 is the active stack pointer (SSP in supervisor mode, USP otherwise).
	public partial class MdStatusViewModel : BaseConsoleStatusViewModel
	{
		[ObservableProperty] public partial UInt32 RegD0 { get; set; }
		[ObservableProperty] public partial UInt32 RegD1 { get; set; }
		[ObservableProperty] public partial UInt32 RegD2 { get; set; }
		[ObservableProperty] public partial UInt32 RegD3 { get; set; }
		[ObservableProperty] public partial UInt32 RegD4 { get; set; }
		[ObservableProperty] public partial UInt32 RegD5 { get; set; }
		[ObservableProperty] public partial UInt32 RegD6 { get; set; }
		[ObservableProperty] public partial UInt32 RegD7 { get; set; }
		[ObservableProperty] public partial UInt32 RegA0 { get; set; }
		[ObservableProperty] public partial UInt32 RegA1 { get; set; }
		[ObservableProperty] public partial UInt32 RegA2 { get; set; }
		[ObservableProperty] public partial UInt32 RegA3 { get; set; }
		[ObservableProperty] public partial UInt32 RegA4 { get; set; }
		[ObservableProperty] public partial UInt32 RegA5 { get; set; }
		[ObservableProperty] public partial UInt32 RegA6 { get; set; }
		[ObservableProperty] public partial UInt32 RegA7 { get; set; }
		[ObservableProperty] public partial UInt32 RegPC { get; set; }
		[ObservableProperty] public partial UInt32 RegUsp { get; set; }
		[ObservableProperty] public partial UInt32 RegSsp { get; set; }
		[ObservableProperty] public partial UInt16 RegSR { get; set; }
		[ObservableProperty] public partial byte IntMask { get; set; }
		[ObservableProperty] public partial byte IrqLevel { get; set; }
		[ObservableProperty] public partial bool Stopped { get; set; }

		[ObservableProperty] public partial bool FlagCarry { get; set; }
		[ObservableProperty] public partial bool FlagOverflow { get; set; }
		[ObservableProperty] public partial bool FlagZero { get; set; }
		[ObservableProperty] public partial bool FlagNegative { get; set; }
		[ObservableProperty] public partial bool FlagExtend { get; set; }
		[ObservableProperty] public partial bool FlagSupervisor { get; set; }
		[ObservableProperty] public partial bool FlagTrace { get; set; }

		[ObservableProperty] public partial UInt16 Scanline { get; set; }
		[ObservableProperty] public partial UInt16 Cycle { get; set; }
		[ObservableProperty] public partial string StackPreview { get; private set; } = "";

		public MdStatusViewModel()
		{
			bool preventUpdate = false;
			this.ObserveProp([nameof(FlagCarry), nameof(FlagOverflow), nameof(FlagZero), nameof(FlagNegative), nameof(FlagExtend), nameof(FlagSupervisor), nameof(FlagTrace), nameof(IntMask)], () => {
				if(!preventUpdate) {
					RegSR = (UInt16)((RegSR & 0x58E0) | (FlagCarry ? 0x1 : 0) | (FlagOverflow ? 0x2 : 0) | (FlagZero ? 0x4 : 0) | (FlagNegative ? 0x8 : 0) | (FlagExtend ? 0x10 : 0) | (FlagSupervisor ? 0x2000 : 0) | (FlagTrace ? 0x8000 : 0) | ((IntMask & 7) << 8));
				}
			});
			this.ObserveProp(nameof(RegSR), () => {
				preventUpdate = true;
				FlagCarry = (RegSR & 0x1) != 0;
				FlagOverflow = (RegSR & 0x2) != 0;
				FlagZero = (RegSR & 0x4) != 0;
				FlagNegative = (RegSR & 0x8) != 0;
				FlagExtend = (RegSR & 0x10) != 0;
				FlagSupervisor = (RegSR & 0x2000) != 0;
				FlagTrace = (RegSR & 0x8000) != 0;
				IntMask = (byte)((RegSR >> 8) & 7);
				preventUpdate = false;
			});
		}

		protected override void InternalUpdateUiState()
		{
			MdM68kState cpu = DebugApi.GetCpuState<MdM68kState>(CpuType.MdM68k);
			MdVdpState vdp = DebugApi.GetPpuState<MdVdpState>(CpuType.MdM68k);
			UpdateCycleCount(cpu.CycleCount);

			RegD0 = cpu.D[0];
			RegD1 = cpu.D[1];
			RegD2 = cpu.D[2];
			RegD3 = cpu.D[3];
			RegD4 = cpu.D[4];
			RegD5 = cpu.D[5];
			RegD6 = cpu.D[6];
			RegD7 = cpu.D[7];
			RegA0 = cpu.A[0];
			RegA1 = cpu.A[1];
			RegA2 = cpu.A[2];
			RegA3 = cpu.A[3];
			RegA4 = cpu.A[4];
			RegA5 = cpu.A[5];
			RegA6 = cpu.A[6];
			RegA7 = cpu.A[7];
			RegPC = cpu.PC;
			RegUsp = cpu.Usp;
			RegSsp = cpu.Ssp;
			RegSR = cpu.SR;
			IrqLevel = cpu.IrqLevel;
			Stopped = cpu.Stopped;

			Scanline = vdp.Scanline;
			Cycle = vdp.HClock;

			StringBuilder sb = new StringBuilder();
			for(UInt32 i = 0; i < 32; i += 2) {
				UInt32 addr = (cpu.A[7] + i) & 0xFFFFFF;
				sb.Append($"${DebugApi.GetMemoryValue(MemoryType.MdMemory, addr):X2}{DebugApi.GetMemoryValue(MemoryType.MdMemory, addr + 1):X2} ");
			}
			StackPreview = sb.ToString();
		}

		protected override void InternalUpdateConsoleState()
		{
			MdM68kState cpu = DebugApi.GetCpuState<MdM68kState>(CpuType.MdM68k);
			cpu.D[0] = RegD0;
			cpu.D[1] = RegD1;
			cpu.D[2] = RegD2;
			cpu.D[3] = RegD3;
			cpu.D[4] = RegD4;
			cpu.D[5] = RegD5;
			cpu.D[6] = RegD6;
			cpu.D[7] = RegD7;
			cpu.A[0] = RegA0;
			cpu.A[1] = RegA1;
			cpu.A[2] = RegA2;
			cpu.A[3] = RegA3;
			cpu.A[4] = RegA4;
			cpu.A[5] = RegA5;
			cpu.A[6] = RegA6;
			cpu.A[7] = RegA7;
			cpu.PC = RegPC;
			cpu.Usp = RegUsp;
			cpu.Ssp = RegSsp;
			cpu.SR = RegSR;
			cpu.Stopped = Stopped;
			DebugApi.SetCpuState(cpu, CpuType.MdM68k);
		}
	}
}
