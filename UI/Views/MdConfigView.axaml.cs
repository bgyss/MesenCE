using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace Mesen.Views;

public class MdConfigView : UserControl
{
	public MdConfigView()
	{
		InitializeComponent();
	}

	private void InitializeComponent()
	{
		AvaloniaXamlLoader.Load(this);
	}
}
