using System.Windows;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Shell;

public partial class GroupEditWindow : Window
{
    public GroupEditWindow(GroupEditViewModel vm)
    {
        InitializeComponent();
        DataContext = vm;
        vm.Saved += (_, _) => { DialogResult = true; Close(); };
    }
}
