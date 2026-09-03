using System.Windows;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Shell;

public partial class LoginWindow : Window
{
    private readonly LoginViewModel _vm;

    public LoginWindow(LoginViewModel vm)
    {
        InitializeComponent();
        _vm = vm;
        DataContext = vm;
        vm.Succeeded += (_, _) => { DialogResult = true; Close(); };
        Loaded += (_, _) => { if (vm.LoginId.Length > 0) Pw.Focus(); };
    }

    private void Pw_Changed(object sender, RoutedEventArgs e) => _vm.Password = Pw.Password;
    private void Pw_KeyDown(object sender, KeyEventArgs e) { if (e.Key == Key.Enter && _vm.CanLogin) _vm.LoginCommand.Execute(null); }
}
