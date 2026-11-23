using Avalonia.Media;
using Mesen.Interop;
using Mesen.ViewModels;
using CommunityToolkit.Mvvm.ComponentModel;

namespace Mesen.Config
{
	public partial class MdEventViewerConfig : ViewModelBase
	{
		[ObservableProperty] public partial EventViewerCategoryCfg VdpWrite { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xB4, 0x7A, 0xDA));
		[ObservableProperty] public partial EventViewerCategoryCfg VdpRead { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0x53, 0xD7, 0x44));

		[ObservableProperty] public partial EventViewerCategoryCfg Irq { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xC4, 0xF4, 0x7A));
		[ObservableProperty] public partial EventViewerCategoryCfg Nmi { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xF4, 0xF4, 0x7A));
		[ObservableProperty] public partial EventViewerCategoryCfg MarkedBreakpoints { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0x18, 0x98, 0xE4));

		[ObservableProperty] public partial bool ShowPreviousFrameEvents { get; set; } = true;

		public InteropMdEventViewerConfig ToInterop()
		{
			return new InteropMdEventViewerConfig() {
				Irq = this.Irq,
				Nmi = this.Nmi,
				MarkedBreakpoints = this.MarkedBreakpoints,
				VdpWrite = this.VdpWrite,
				VdpRead = this.VdpRead,
				ShowPreviousFrameEvents = this.ShowPreviousFrameEvents
			};
		}
	}
}
