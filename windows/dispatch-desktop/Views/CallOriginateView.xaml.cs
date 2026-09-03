using System.Windows.Controls;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class CallOriginateView : UserControl
{
    public CallOriginateView() { InitializeComponent(); }

    private void Number_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && DataContext is CallOriginateViewModel vm) { vm.DialCommand.Execute(null); e.Handled = true; }
    }
}
