using Avalonia.Controls;
using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Config;
using Mesen.Utilities;
using System;

namespace Mesen.ViewModels
{
	public partial class MdConfigViewModel : DisposableViewModel
	{
		[ObservableProperty] public partial MdConfig Config { get; set; }
		[ObservableProperty] public partial MdConfig OriginalConfig { get; set; }
		[ObservableProperty] public partial MdConfigTab SelectedTab { get; set; } = 0;

		public MdInputConfigViewModel Input { get; private set; }

		public Enum[] AvailableRegions => new Enum[] {
			ConsoleRegion.Auto,
			ConsoleRegion.Ntsc,
			ConsoleRegion.NtscJapan,
			ConsoleRegion.Pal
		};

		public MdConfigViewModel()
		{
			Config = ConfigManager.Config.Md;
			OriginalConfig = Config.Clone();
			Input = new MdInputConfigViewModel(Config);

			if(Design.IsDesignMode) {
				return;
			}

			AddDisposable(Input);
			AddDisposable(ReactiveHelper.RegisterRecursiveObserver(Config, (s, e) => {
				Config.ApplyConfig();
				ConfigManager.Config.Video.ApplyConfig();
			}));
		}
	}

	public enum MdConfigTab
	{
		General,
		Audio,
		Input,
		Video
	}
}
