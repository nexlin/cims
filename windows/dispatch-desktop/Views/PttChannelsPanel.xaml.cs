// ① 내 채널 — 카드 클릭 = 포커스(버튼·체크 위 클릭은 제외), 발언 바 PTT press/release(마우스·터치, 포인터가 벗어나도 release 를 놓치지 않는다),
// 빠른 발신 줄 Enter = 사설콜 발신 · 포커스 잃으면 제안 접기.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class PttChannelsPanel : UserControl
{
    private bool _pressed;

    public PttChannelsPanel() { InitializeComponent(); }

    private MainViewModel? Vm => DataContext as MainViewModel;

    private void Card_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: ChannelCard c }) return;
        // 체크·버튼 위 클릭은 그 컨트롤 몫 — 포커스를 바꾸지 않는다
        for (var d = e.OriginalSource as DependencyObject; d is not null && d != sender; d = VisualTreeHelper.GetParent(d))
            if (d is ButtonBase) return;
        Vm?.PttChannels.Select(c);
    }

    private void Ptt_Down(object sender, MouseButtonEventArgs e)
    {
        if (sender is not UIElement u || Vm is null) return;
        _pressed = true;
        Vm.TalkBar.PttDown();
        u.CaptureMouse();
        e.Handled = true;
    }
    private void Ptt_Up(object sender, MouseButtonEventArgs e) { Release(sender); e.Handled = true; }
    private void Ptt_Leave(object sender, MouseEventArgs e) { if (e.LeftButton != MouseButtonState.Pressed) Release(sender); }
    private void Ptt_TouchDown(object sender, TouchEventArgs e) { if (Vm is null) return; _pressed = true; Vm.TalkBar.PttDown(); e.Handled = true; }
    private void Ptt_TouchUp(object sender, TouchEventArgs e) { Release(sender); e.Handled = true; }

    private void Release(object sender)
    {
        if (sender is UIElement u && u.IsMouseCaptured) u.ReleaseMouseCapture();
        if (!_pressed) return;
        _pressed = false;
        Vm?.TalkBar.PttUp();
    }

    private void Target_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && Vm is not null) { Vm.PttOriginate.Mode = "private"; Vm.PttOriginate.StartCommand.Execute(null); e.Handled = true; }
        if (e.Key == Key.Escape && Vm is not null) { Vm.PttOriginate.ClearSuggestions(); e.Handled = true; }
    }
    private void Target_LostFocus(object sender, KeyboardFocusChangedEventArgs e) => Vm?.PttOriginate.ClearSuggestions();
}
