// [PTT 그룹] 화면 본문(§4.7) 코드비하인드 — 확인 대화상자(소유 창 기준)·더블클릭(= 상세 카드 자리의 인라인 편집 폼)만.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class PttGroupsView : UserControl
{
    private GroupAdminViewModel? Vm => DataContext as GroupAdminViewModel;

    public PttGroupsView()
    {
        InitializeComponent();
        Loaded += (_, _) => { if (Vm is not null) Vm.Confirm = Confirm; };
    }

    private bool Confirm(string title, string text) =>
        MessageBox.Show(Window.GetWindow(this)!, text, title, MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes;

    private void Groups_DoubleClick(object sender, MouseButtonEventArgs e) { if (Vm?.Selected is not null) Vm.EditGroupCommand.Execute(null); }
}
