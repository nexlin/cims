// ① PTT 채널 — 카드 클릭 선택, PTT 버튼 press/release(마우스·터치). 포인터가 버튼을 벗어나도 release 를 놓치지 않는다.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class PttChannelsPanel : UserControl
{
    private ChannelCard? _pressed;

    public PttChannelsPanel() { InitializeComponent(); }

    private MainViewModel? Vm => DataContext as MainViewModel;

    private void Card_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: ChannelCard c }) Vm?.PttChannels.Select(c);
    }

    private void Ptt_Down(object sender, MouseButtonEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: ChannelCard c }) return;
        Vm?.PttChannels.Select(c);
        _pressed = c;
        c.PttDown();
        ((UIElement)sender).CaptureMouse();
        e.Handled = true;
    }

    private void Ptt_Up(object sender, MouseButtonEventArgs e) { Release(sender); e.Handled = true; }
    private void Ptt_Leave(object sender, MouseEventArgs e) { if (e.LeftButton != MouseButtonState.Pressed) Release(sender); }
    private void Ptt_TouchDown(object sender, TouchEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: ChannelCard c }) return;
        Vm?.PttChannels.Select(c);
        _pressed = c;
        c.PttDown();
        e.Handled = true;
    }
    private void Ptt_TouchUp(object sender, TouchEventArgs e) { Release(sender); e.Handled = true; }

    private void Release(object sender)
    {
        if (sender is UIElement u && u.IsMouseCaptured) u.ReleaseMouseCapture();
        _pressed?.PttUp();
        _pressed = null;
    }
}
