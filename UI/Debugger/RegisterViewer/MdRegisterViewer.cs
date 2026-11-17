using Mesen.Debugger.ViewModels;
using Mesen.Interop;
using System;
using System.Collections.Generic;
using static Mesen.Debugger.ViewModels.RegEntry;

namespace Mesen.Debugger.RegisterViewer;

//Mega Drive: VDP registers (Sega Genesis Software Manual, VDP register descriptions) and the system/I-O state
public class MdRegisterViewer
{
	public static List<RegisterViewerTab> GetTabs(ref MdState state)
	{
		return new List<RegisterViewerTab>() {
			GetVdpTab(ref state),
			GetSystemTab(ref state),
		};
	}

	private static RegisterViewerTab GetVdpTab(ref MdState state)
	{
		MdVdpState vdp = state.Vdp;
		byte[] r = vdp.Regs;
		bool h40 = (r[12] & 0x81) != 0;
		string[] planeSizes = { "32", "64", "invalid", "128" };

		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("", "State"),
			new RegEntry("", "Scanline", vdp.Scanline),
			new RegEntry("", "Master clocks into line", vdp.HClock),
			new RegEntry("", "Frame number", vdp.FrameCount),
			new RegEntry("", "Odd frame (interlace)", vdp.OddFrame),
			new RegEntry("--", "Address register", vdp.Address, Format.X16),
			new RegEntry("--", "Code register", vdp.Code, Format.X8),
			new RegEntry("--", "Command word pending", vdp.CommandPending),
			new RegEntry("--", "H interrupt counter", vdp.HIntCounter),
			new RegEntry("--", "H interrupt pending", vdp.HIntPending),
			new RegEntry("--", "V interrupt pending", vdp.VIntPending),
			new RegEntry("--", "Z80 interrupt active", vdp.Z80IntActive),
			new RegEntry("--", "Sprite overflow", vdp.SpriteOverflow),
			new RegEntry("--", "Sprite collision", vdp.SpriteCollision),
			new RegEntry("--", "H/V counter latched", vdp.HvLatched),
			new RegEntry("--", "H/V latch", vdp.HvLatch, Format.X16),
			new RegEntry("--", "DMA fill pending", vdp.DmaFillPending),

			new RegEntry("", "Registers"),
			new RegEntry("$00.1", "H/V counter latch enabled", (r[0] & 0x02) != 0),
			new RegEntry("$00.4", "H interrupt enabled", (r[0] & 0x10) != 0),
			new RegEntry("$00.5", "Blank leftmost 8 pixels", (r[0] & 0x20) != 0),
			new RegEntry("$01.2", "Mode 5", (r[1] & 0x04) != 0),
			new RegEntry("$01.3", "V30 (240 lines)", (r[1] & 0x08) != 0),
			new RegEntry("$01.4", "DMA enabled", (r[1] & 0x10) != 0),
			new RegEntry("$01.5", "V interrupt enabled", (r[1] & 0x20) != 0),
			new RegEntry("$01.6", "Display enabled", (r[1] & 0x40) != 0),
			new RegEntry("$02", "Plane A name table", (r[2] & 0x38) << 10, Format.X16),
			new RegEntry("$03", "Window name table", (r[3] & (h40 ? 0x3C : 0x3E)) << 10, Format.X16),
			new RegEntry("$04", "Plane B name table", (r[4] & 0x07) << 13, Format.X16),
			new RegEntry("$05", "Sprite attribute table", (r[5] & (h40 ? 0x7E : 0x7F)) << 9, Format.X16),
			new RegEntry("$07.0-5", "Background color index", r[7] & 0x3F),
			new RegEntry("$0A", "H interrupt counter reload", r[10]),
			new RegEntry("$0B.0-1", "Horizontal scroll mode", (r[11] & 0x03) switch { 0 => "Full screen", 1 => "Invalid (first 8 lines)", 2 => "Per cell", _ => "Per line" }, r[11] & 0x03),
			new RegEntry("$0B.2", "Vertical scroll per 2 cells", (r[11] & 0x04) != 0),
			new RegEntry("$0B.3", "External interrupt enabled", (r[11] & 0x08) != 0),
			new RegEntry("$0C.0+7", "H40 (320 pixels)", h40),
			new RegEntry("$0C.1-2", "Interlace mode", ((r[12] >> 1) & 0x03) switch { 0 => "Off", 1 => "Interlace", 2 => "Invalid", _ => "Interlace (double resolution)" }, (r[12] >> 1) & 0x03),
			new RegEntry("$0C.3", "Shadow/highlight", (r[12] & 0x08) != 0),
			new RegEntry("$0D", "H scroll table", (r[13] & 0x3F) << 10, Format.X16),
			new RegEntry("$0F", "Auto-increment", r[15]),
			new RegEntry("$10.0-1", "Plane width (cells)", planeSizes[r[16] & 0x03], r[16] & 0x03),
			new RegEntry("$10.4-5", "Plane height (cells)", planeSizes[(r[16] >> 4) & 0x03], (r[16] >> 4) & 0x03),
			new RegEntry("$11.0-4", "Window H position (2 cells)", r[17] & 0x1F),
			new RegEntry("$11.7", "Window right of position", (r[17] & 0x80) != 0),
			new RegEntry("$12.0-4", "Window V position (cells)", r[18] & 0x1F),
			new RegEntry("$12.7", "Window below position", (r[18] & 0x80) != 0),
			new RegEntry("$13-14", "DMA length (words)", r[19] | (r[20] << 8)),
			new RegEntry("$15-17", "DMA source", ((r[23] & 0x7F) << 17) | (r[22] << 9) | (r[21] << 1), Format.X24),
			new RegEntry("$17.6-7", "DMA mode", (r[23] >> 6) switch { 0 or 1 => "68000 to VDP", 2 => "Fill", _ => "Copy" }, r[23] >> 6),
		};

		for(int i = 0; i < 24; i++) {
			entries.Add(new RegEntry("$" + i.ToString("X2"), "Register value", r[i], Format.X8));
		}

		return new RegisterViewerTab("VDP", entries, CpuType.MdM68k);
	}

	private static RegisterViewerTab GetSystemTab(ref MdState state)
	{
		MdMemoryManagerState mm = state.MemoryManager;
		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("", "Z80"),
			new RegEntry("$A11100", "Bus requested", mm.Z80BusRequested),
			new RegEntry("$A11200", "Held in reset", mm.Z80Reset),
			new RegEntry("$A06000", "68000 bank window", mm.Z80Bank << 15, Format.X24),

			new RegEntry("", "Cartridge"),
			new RegEntry("$A130F1.0", "SRAM enabled", mm.SramEnabled),
			new RegEntry("$A130F1.1", "SRAM write-protected", mm.SramWriteProtected),
			new RegEntry("$A14000", "TMSS unlocked", mm.TmssUnlocked),

			new RegEntry("", "I/O ports"),
		};
		string[] names = { "Port 1", "Port 2", "Expansion" };
		for(int i = 0; i < 3; i++) {
			int addr = 0xA10003 + i * 2;
			entries.Add(new RegEntry("$" + addr.ToString("X6"), names[i] + " data", mm.Io.Data[i], Format.X8));
			entries.Add(new RegEntry("$" + (addr + 6).ToString("X6"), names[i] + " control", mm.Io.Ctrl[i], Format.X8));
		}
		return new RegisterViewerTab("System", entries, CpuType.MdM68k);
	}
}
