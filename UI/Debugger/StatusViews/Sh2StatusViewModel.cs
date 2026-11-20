using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using Mesen.Utilities;
using System;
using System.Text;

namespace Mesen.Debugger.StatusViews
{
	//32X SH-2 (master or slave). R15 is the stack pointer.
	public partial class Sh2StatusViewModel : BaseConsoleStatusViewModel
	{
		[ObservableProperty] public partial UInt32 RegR0 { get; set; }
		[ObservableProperty] public partial UInt32 RegR1 { get; set; }
		[ObservableProperty] public partial UInt32 RegR2 { get; set; }
		[ObservableProperty] public partial UInt32 RegR3 { get; set; }
		[ObservableProperty] public partial UInt32 RegR4 { get; set; }
		[ObservableProperty] public partial UInt32 RegR5 { get; set; }
		[ObservableProperty] public partial UInt32 RegR6 { get; set; }
		[ObservableProperty] public partial UInt32 RegR7 { get; set; }
		[ObservableProperty] public partial UInt32 RegR8 { get; set; }
		[ObservableProperty] public partial UInt32 RegR9 { get; set; }
		[ObservableProperty] public partial UInt32 RegR10 { get; set; }
		[ObservableProperty] public partial UInt32 RegR11 { get; set; }
		[ObservableProperty] public partial UInt32 RegR12 { get; set; }
		[ObservableProperty] public partial UInt32 RegR13 { get; set; }
		[ObservableProperty] public partial UInt32 RegR14 { get; set; }
		[ObservableProperty] public partial UInt32 RegR15 { get; set; }
		[ObservableProperty] public partial UInt32 RegPC { get; set; }
		[ObservableProperty] public partial UInt32 RegPR { get; set; }
		[ObservableProperty] public partial UInt32 RegGBR { get; set; }
		[ObservableProperty] public partial UInt32 RegVBR { get; set; }
		[ObservableProperty] public partial UInt32 RegMACH { get; set; }
		[ObservableProperty] public partial UInt32 RegMACL { get; set; }
		[ObservableProperty] public partial UInt32 RegSR { get; set; }
		[ObservableProperty] public partial byte IntMask { get; set; }
		[ObservableProperty] public partial bool FlagT { get; set; }
		[ObservableProperty] public partial bool FlagS { get; set; }
		[ObservableProperty] public partial bool FlagQ { get; set; }
		[ObservableProperty] public partial bool FlagM { get; set; }
		[ObservableProperty] public partial bool Sleeping { get; set; }

		[ObservableProperty] public partial UInt16 Scanline { get; set; }
		[ObservableProperty] public partial UInt16 Cycle { get; set; }
		[ObservableProperty] public partial string StackPreview { get; private set; } = "";

		private CpuType _cpuType = CpuType.M32xMaster;

		public Sh2StatusViewModel(CpuType cpuType) : this()
		{
			_cpuType = cpuType;
		}

		public Sh2StatusViewModel()
		{
			bool preventUpdate = false;
			this.ObserveProp([nameof(FlagT), nameof(FlagS), nameof(FlagQ), nameof(FlagM), nameof(IntMask)], () => {
				if(!preventUpdate) {
					RegSR = (UInt32)((FlagT ? 0x1 : 0) | (FlagS ? 0x2 : 0) | ((IntMask & 0xF) << 4) | (FlagQ ? 0x100 : 0) | (FlagM ? 0x200 : 0));
				}
			});
			this.ObserveProp(nameof(RegSR), () => {
				preventUpdate = true;
				FlagT = (RegSR & 0x1) != 0;
				FlagS = (RegSR & 0x2) != 0;
				FlagQ = (RegSR & 0x100) != 0;
				FlagM = (RegSR & 0x200) != 0;
				IntMask = (byte)((RegSR >> 4) & 0xF);
				preventUpdate = false;
			});
		}

		protected override void InternalUpdateUiState()
		{
			M32xSh2State cpu = DebugApi.GetCpuState<M32xSh2State>(_cpuType);
			MdVdpState vdp = DebugApi.GetPpuState<MdVdpState>(CpuType.MdM68k);
			UpdateCycleCount(cpu.CycleCount);
			RegR0 = cpu.R[0];
			RegR1 = cpu.R[1];
			RegR2 = cpu.R[2];
			RegR3 = cpu.R[3];
			RegR4 = cpu.R[4];
			RegR5 = cpu.R[5];
			RegR6 = cpu.R[6];
			RegR7 = cpu.R[7];
			RegR8 = cpu.R[8];
			RegR9 = cpu.R[9];
			RegR10 = cpu.R[10];
			RegR11 = cpu.R[11];
			RegR12 = cpu.R[12];
			RegR13 = cpu.R[13];
			RegR14 = cpu.R[14];
			RegR15 = cpu.R[15];
			RegPC = cpu.PC;
			RegPR = cpu.PR;
			RegGBR = cpu.GBR;
			RegVBR = cpu.VBR;
			RegMACH = cpu.MACH;
			RegMACL = cpu.MACL;
			RegSR = cpu.SR;
			Sleeping = cpu.Sleeping;
			Scanline = vdp.Scanline;
			Cycle = vdp.HClock;

			MemoryType memType = _cpuType.ToMemoryType();
			StringBuilder sb = new StringBuilder();
			for(UInt32 i = 0; i < 32; i += 4) {
				UInt32 addr = (cpu.R[15] + i) & 0x1FFFFFFF;
				sb.Append($"${DebugApi.GetMemoryValue(memType, addr):X2}{DebugApi.GetMemoryValue(memType, addr + 1):X2}{DebugApi.GetMemoryValue(memType, addr + 2):X2}{DebugApi.GetMemoryValue(memType, addr + 3):X2} ");
			}
			StackPreview = sb.ToString();
		}

		protected override void InternalUpdateConsoleState()
		{
			M32xSh2State cpu = DebugApi.GetCpuState<M32xSh2State>(_cpuType);
			cpu.R[0] = RegR0;
			cpu.R[1] = RegR1;
			cpu.R[2] = RegR2;
			cpu.R[3] = RegR3;
			cpu.R[4] = RegR4;
			cpu.R[5] = RegR5;
			cpu.R[6] = RegR6;
			cpu.R[7] = RegR7;
			cpu.R[8] = RegR8;
			cpu.R[9] = RegR9;
			cpu.R[10] = RegR10;
			cpu.R[11] = RegR11;
			cpu.R[12] = RegR12;
			cpu.R[13] = RegR13;
			cpu.R[14] = RegR14;
			cpu.R[15] = RegR15;
			cpu.PC = RegPC;
			cpu.PR = RegPR;
			cpu.GBR = RegGBR;
			cpu.VBR = RegVBR;
			cpu.MACH = RegMACH;
			cpu.MACL = RegMACL;
			cpu.SR = RegSR;
			cpu.Sleeping = Sleeping;
			DebugApi.SetCpuState(cpu, _cpuType);
		}
	}
}
