// 관제 요약 띠(§3.5) 코드비하인드 — PTT 버튼의 누름/뗌만(카드의 PTT 버튼과 같은 규약: 누르는 동안 요청, 떼거나 벗어나면 해제).
using System.Windows.Controls;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class DispatchStripView : UserControl
{
    private MainViewModel? Vm => DataContext as MainViewModel;
    private bool _down;

    public DispatchStripView() { InitializeComponent(); }

    private void Ptt_Down(object sender, MouseButtonEventArgs e) { _down = true; Vm?.Summary.PttDown(); PttBtn.CaptureMouse(); e.Handled = true; }
    private void Ptt_Up(object sender, MouseButtonEventArgs e) { Release(); e.Handled = true; }
    private void Ptt_Leave(object sender, MouseEventArgs e) { if (_down && !PttBtn.IsMouseCaptured) Release(); }
    private void Release() { if (!_down) return; _down = false; PttBtn.ReleaseMouseCapture(); Vm?.Summary.PttUp(); }
}
