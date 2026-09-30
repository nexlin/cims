// 오른쪽 패널 — 채널 상세 ⋮ 메뉴 항목을 누르면 메뉴를 닫는다(명령은 그대로 실행된다).
using System.Windows;
using System.Windows.Controls;

namespace DispatchDesktop.Views;

public partial class SidePanelView : UserControl
{
    public SidePanelView() { InitializeComponent(); }

    private void More_Click(object sender, RoutedEventArgs e) => More.IsChecked = false;
}
