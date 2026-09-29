// ① 내 채널 — 카드 클릭 = 포커스(버튼·체크 위 클릭은 제외), 발언 바 PTT press/release(마우스·터치, 포인터가 벗어나도 release 를 놓치지 않는다),
// 빠른 발신 줄 Enter = 개인 통화 발신(반이중 — 팝오버 모드와 무관) · 포커스 잃으면 제안 접기.
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

    // [일제 통화] — 누름 = 개시+발언, 뗌 = 끝(포인터가 벗어나도 캡처로 뗌을 받는다). 잠금 발언이면 뗌은 무시되고 다시 누르면 끝난다(ViewModel).
    private bool _bcPressed;
    private void Broadcast_Down(object sender, MouseButtonEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: ChannelCard c } fe || Vm is null) return;
        _bcPressed = true;
        fe.CaptureMouse();
        Vm.PttChannels.BroadcastDown(c);
        e.Handled = true;
    }
    private void Broadcast_Up(object sender, MouseButtonEventArgs e) { BroadcastRelease(sender); e.Handled = true; }
    private void Broadcast_LostCapture(object sender, MouseEventArgs e) => BroadcastRelease(sender);
    private void Broadcast_TouchDown(object sender, TouchEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: ChannelCard c } || Vm is null) return;
        _bcPressed = true;
        Vm.PttChannels.BroadcastDown(c);
        e.Handled = true;
    }
    private void Broadcast_TouchUp(object sender, TouchEventArgs e) { BroadcastRelease(sender); e.Handled = true; }
    private void BroadcastRelease(object sender)
    {
        if (!_bcPressed) return;
        _bcPressed = false;
        if (sender is UIElement u && u.IsMouseCaptured) u.ReleaseMouseCapture();
        Vm?.PttChannels.BroadcastUp();
    }

    private void Target_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && Vm is not null) { Vm.PttOriginate.StartPrivateFromTarget(); e.Handled = true; }
        if (e.Key == Key.Escape && Vm is not null) { Vm.PttOriginate.ClearSuggestions(); e.Handled = true; }
    }
    private void Target_LostFocus(object sender, KeyboardFocusChangedEventArgs e) => Vm?.PttOriginate.ClearSuggestions();
}
