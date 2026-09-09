// ③ 일반통화 — 그룹원 칩 클릭: 대기 상태면 발신 필드에 채움, 우클릭: 사람 메뉴(§4.3·§4.1). 빠른 발신 줄 Enter = 발신, 문자 받는 사람 Enter = 스레드 열기.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class CallDeskPanel : UserControl
{
    public CallDeskPanel() { InitializeComponent(); }

    private MainViewModel? Vm => DataContext as MainViewModel;

    private void Chip_Click(object sender, MouseButtonEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: MemberChip chip } && chip.IsIdle && !chip.IsMe && chip.FillCommand.CanExecute(null))
            chip.FillCommand.Execute(null);
    }
    private void Chip_RightClick(object sender, MouseButtonEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: MemberChip chip } && !chip.IsMe) { chip.MenuCommand.Execute(null); e.Handled = true; }
    }

    private void Number_KeyDown(object sender, KeyEventArgs e)
    {
        if (Vm is null) return;
        if (e.Key == Key.Enter) { Vm.CallOriginate.DialCommand.Execute(null); e.Handled = true; }
        else if (e.Key == Key.Escape) { Vm.CallOriginate.ClearSuggestions(); e.Handled = true; }
    }
    private void Number_LostFocus(object sender, KeyboardFocusChangedEventArgs e) => Vm?.CallOriginate.ClearSuggestions();

    private void Recipient_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && Vm is not null && Vm.Sms.CanOpenRecipient) { Vm.Sms.OpenRecipientCommand.Execute(null); e.Handled = true; }
    }
}
