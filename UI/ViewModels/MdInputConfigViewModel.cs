using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Config;
using System;

namespace Mesen.ViewModels
{
	public partial class MdInputConfigViewModel : DisposableViewModel
	{
		[ObservableProperty] public partial MdConfig Config { get; set; }

		public Enum[] AvailableControllerTypes => new Enum[] {
			ControllerType.None,
			ControllerType.MdController3Buttons,
			ControllerType.MdController6Buttons,
			ControllerType.MdTeamPlayer,
			ControllerType.MdMouse,
			ControllerType.MdMenacer,
			ControllerType.MdJustifier,
		};

		public Enum[] AvailableControllerTypesTeamPlayer => new Enum[] {
			ControllerType.None,
			ControllerType.MdController3Buttons,
			ControllerType.MdController6Buttons,
		};

		[Obsolete("For designer only")]
		public MdInputConfigViewModel() : this(new MdConfig()) { }

		public MdInputConfigViewModel(MdConfig config)
		{
			Config = config;
		}
	}
}
