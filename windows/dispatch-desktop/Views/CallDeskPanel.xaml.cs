// ③ 일반통화 — 그룹원 칩 클릭: 대기 상태면 발신 필드에 채움(§4.3).
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class CallDeskPanel : UserControl
{
    public CallDeskPanel() { InitializeComponent(); }

    private void Chip_Click(object sender, MouseButtonEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: MemberChip chip } && chip.IsIdle && !chip.IsMe && chip.FillCommand.CanExecute(null))
            chip.FillCommand.Execute(null);
    }
}
