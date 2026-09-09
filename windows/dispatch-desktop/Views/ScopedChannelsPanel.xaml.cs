// ② 범위 채널 — 카드 클릭 = 선택(조작 노출), 버튼 위 클릭은 제외.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class ScopedChannelsPanel : UserControl
{
    public ScopedChannelsPanel() { InitializeComponent(); }

    private void Card_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: ScopedCard c } || DataContext is not ScopedChannelsViewModel vm) return;
        for (var d = e.OriginalSource as DependencyObject; d is not null && d != sender; d = VisualTreeHelper.GetParent(d))
            if (d is ButtonBase) return;
        vm.SelectCardCommand.Execute(c);
    }
}
