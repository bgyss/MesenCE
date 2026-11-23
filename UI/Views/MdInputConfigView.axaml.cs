using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace Mesen.Views
{
	public class MdInputConfigView : UserControl
	{
		public MdInputConfigView()
		{
			InitializeComponent();
		}

		private void InitializeComponent()
		{
			AvaloniaXamlLoader.Load(this);
		}
	}
}
