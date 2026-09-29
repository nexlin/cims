using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class PttOriginateView : UserControl
{
    public PttOriginateView() { InitializeComponent(); }

    /// <summary>대상 필드 Enter = 개인 통화 발신(옆 [개인 통화 발신]·패드 📞 와 같은 동작 — 반이중/전이중 선택을 따른다).</summary>
    private void Target_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && DataContext is PttOriginateViewModel vm && vm.CanStart) { vm.StartCommand.Execute(null); e.Handled = true; }
    }

    /// <summary>주소록 사용자 행 클릭 — 모드별 행동(RowCommand). 버튼·체크 위 클릭은 그 컨트롤 몫이라 건너뛴다.</summary>
    private void UserRow_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: PttUserRow u } || DataContext is not PttOriginateViewModel vm) return;
        for (var d = e.OriginalSource as DependencyObject; d is not null && d != sender; d = VisualTreeHelper.GetParent(d))
            if (d is ButtonBase) return;
        vm.RowCommand.Execute(u);
        e.Handled = true;
    }
}
