using System.Windows;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Shell;

public partial class SettingsWindow : Window
{
    public SettingsWindow(SettingsViewModel vm)
    {
        InitializeComponent();
        DataContext = vm;
        vm.Saved += (_, _) => Close();
    }
}
