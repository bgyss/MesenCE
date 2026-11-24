using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using System;
using System.Runtime.InteropServices;

namespace Mesen.Config;

public partial class MdConfig : BaseConfig<MdConfig>
{
	[ObservableProperty] public partial ConsoleOverrideConfig ConfigOverrides { get; set; } = new();

	[ObservableProperty] public partial MdControllerConfig Port1 { get; set; } = new();
	[ObservableProperty] public partial MdControllerConfig Port2 { get; set; } = new();

	//Team Player ports A-D (port A uses the key bindings of the port the Team Player is on)
	[ObservableProperty] public partial MdControllerConfig Port1A { get; set; } = new();
	[ObservableProperty] public partial MdControllerConfig Port1B { get; set; } = new();
	[ObservableProperty] public partial MdControllerConfig Port1C { get; set; } = new();
	[ObservableProperty] public partial MdControllerConfig Port1D { get; set; } = new();
	[ObservableProperty] public partial MdControllerConfig Port2A { get; set; } = new();
	[ObservableProperty] public partial MdControllerConfig Port2B { get; set; } = new();
	[ObservableProperty] public partial MdControllerConfig Port2C { get; set; } = new();
	[ObservableProperty] public partial MdControllerConfig Port2D { get; set; } = new();

	[ValidValues(ConsoleRegion.Auto, ConsoleRegion.Ntsc, ConsoleRegion.NtscJapan, ConsoleRegion.Pal)]
	[ObservableProperty] public partial ConsoleRegion Region { get; set; } = ConsoleRegion.Auto;
	[ObservableProperty] public partial MdModel Model { get; set; } = MdModel.Md1Va3;
	[ObservableProperty] public partial RamState RamPowerOnState { get; set; } = RamState.AllZeros;

	[ObservableProperty] public partial bool RemoveSpriteLimit { get; set; } = false;
	[ObservableProperty] public partial bool DisableSprites { get; set; } = false;
	[ObservableProperty] public partial bool HidePlaneA { get; set; } = false;
	[ObservableProperty] public partial bool HidePlaneB { get; set; } = false;
	[ObservableProperty] public partial bool HideWindow { get; set; } = false;

	[ObservableProperty][MinMax(0, 100)] public partial UInt32 FmVolume { get; set; } = 100;
	[ObservableProperty][MinMax(0, 100)] public partial UInt32 PsgVolume { get; set; } = 100;
	[ObservableProperty] public partial MdFmCore FmCore { get; set; } = MdFmCore.Nuked;

	public void ApplyConfig()
	{
		ConfigApi.SetMdConfig(new InteropMdConfig() {
			Port1 = Port1.ToInterop(),
			Port2 = Port2.ToInterop(),
			Region = Region,
			Model = Model,
			RamPowerOnState = RamPowerOnState,
			RemoveSpriteLimit = RemoveSpriteLimit,
			DisableSprites = DisableSprites,
			HidePlaneA = HidePlaneA,
			HidePlaneB = HidePlaneB,
			HideWindow = HideWindow,
			FmVolume = FmVolume,
			PsgVolume = PsgVolume,
			FmCore = FmCore,
			Port1A = Port1.ToInterop(Port1A.Type),
			Port1B = Port1B.ToInterop(),
			Port1C = Port1C.ToInterop(),
			Port1D = Port1D.ToInterop(),
			Port2A = Port2.ToInterop(Port2A.Type),
			Port2B = Port2B.ToInterop(),
			Port2C = Port2C.ToInterop(),
			Port2D = Port2D.ToInterop(),
		});
	}

	internal void InitializeDefaults(DefaultKeyMappingType defaultMappings)
	{
		Port1.InitDefaults<MdKeyMapping>(defaultMappings, ControllerType.MdController6Buttons);
		Port1.Type = ControllerType.MdController6Buttons;
		Port2.Type = ControllerType.MdController6Buttons;
		Port1A.Type = ControllerType.MdController6Buttons;
		Port2A.Type = ControllerType.MdController6Buttons;
	}
}

//Mirrors MdFmCore in Core/Shared/SettingTypes.h
public enum MdFmCore : byte
{
	Ymfm,
	Nuked
}

public enum MdModel : byte
{
	Md1Va3,
	Md1Va6
}

//Mirrors MdConfig in Core/Shared/SettingTypes.h
[StructLayout(LayoutKind.Sequential)]
public struct InteropMdConfig
{
	public InteropControllerConfig Port1;
	public InteropControllerConfig Port2;

	public ConsoleRegion Region;
	public MdModel Model;
	public RamState RamPowerOnState;

	[MarshalAs(UnmanagedType.I1)] public bool RemoveSpriteLimit;
	[MarshalAs(UnmanagedType.I1)] public bool DisableSprites;
	[MarshalAs(UnmanagedType.I1)] public bool HidePlaneA;
	[MarshalAs(UnmanagedType.I1)] public bool HidePlaneB;
	[MarshalAs(UnmanagedType.I1)] public bool HideWindow;

	public UInt32 FmVolume;
	public UInt32 PsgVolume;

	public MdFmCore FmCore;

	//ControllerConfig Port1SubPorts[4], Port2SubPorts[4]
	public InteropControllerConfig Port1A;
	public InteropControllerConfig Port1B;
	public InteropControllerConfig Port1C;
	public InteropControllerConfig Port1D;
	public InteropControllerConfig Port2A;
	public InteropControllerConfig Port2B;
	public InteropControllerConfig Port2C;
	public InteropControllerConfig Port2D;
}
