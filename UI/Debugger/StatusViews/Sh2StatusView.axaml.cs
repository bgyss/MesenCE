using Avalonia;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace Mesen.Debugger.StatusViews
{
	public class Sh2StatusView : UserControl
	{
		public Sh2StatusView()
		{
			InitializeComponent();
		}

		private void InitializeComponent()
		{
			AvaloniaXamlLoader.Load(this);
		}
	}
}
