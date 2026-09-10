// ⑤ PTT 이벤트 — 행 클릭 = 그 채널 카드 포커스([답장] 버튼 클릭은 제외).
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using DispatchDesktop.Models;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class PttActivityPanel : UserControl
{
    public PttActivityPanel() { InitializeComponent(); }

    private void Row_Click(object sender, MouseButtonEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: ActivityRow r } || DataContext is not PttActivityViewModel vm) return;
        for (var d = e.OriginalSource as DependencyObject; d is not null && d != sender; d = VisualTreeHelper.GetParent(d))
            if (d is ButtonBase) return;
        vm.ChannelCommand.Execute(r);
    }
}
