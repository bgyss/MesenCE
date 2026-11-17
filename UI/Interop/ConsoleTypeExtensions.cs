using System;

namespace Mesen.Interop
{
	public static class ConsoleTypeExtensions
	{
		public static CpuType GetMainCpuType(this ConsoleType type)
		{
			return type switch {
				ConsoleType.Snes => CpuType.Snes,
				ConsoleType.Nes => CpuType.Nes,
				ConsoleType.Gameboy => CpuType.Gameboy,
				ConsoleType.PcEngine => CpuType.Pce,
				ConsoleType.Sms => CpuType.Sms,
				ConsoleType.Gba => CpuType.Gba,
				ConsoleType.Ws => CpuType.Ws,
				ConsoleType.Md => CpuType.MdM68k,
				_ => throw new Exception("Invalid type")
			};
		}

		//Tile/tilemap/sprite/palette/register/event viewers (not implemented for the Mega Drive yet)
		public static bool SupportsPpuTools(this ConsoleType type)
		{
			return type != ConsoleType.Md;
		}

		public static bool SupportsCheats(this ConsoleType type)
		{
			return type switch {
				ConsoleType.Gba => false,
				ConsoleType.Ws => false,
				ConsoleType.Md => false,
				_ => true
			};
		}
	}
}
