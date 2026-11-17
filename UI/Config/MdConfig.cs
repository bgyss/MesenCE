using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using System;
using System.Runtime.InteropServices;

namespace Mesen.Config;

public partial class MdConfig : BaseConfig<MdConfig>
{
	[ObservableProperty] public partial ConsoleOverrideConfig ConfigOverrides { get; set; } = new();

	[ObservableProperty] public partial ControllerConfig Port1 { get; set; } = new();
	[ObservableProperty] public partial ControllerConfig Port2 { get; set; } = new();

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
	[ObservableProperty] public partial MdFmCore FmCore { get; set; } = MdFmCore.Ymfm;

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
		});
	}

	internal void InitializeDefaults(DefaultKeyMappingType defaultMappings)
	{
		Port1.InitDefaults(defaultMappings, ControllerType.MdController6Buttons);
		Port1.Type = ControllerType.MdController6Buttons;
		Port2.Type = ControllerType.MdController6Buttons;
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
}
