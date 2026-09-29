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

    // [일제 통화](임시 모드) — 누름 = 개시+발언, 뗌 = 끝(캡처로 포인터가 벗어나도 뗌을 받는다). 잠금 발언이면 ViewModel 이 뗌을 무시한다.
    private bool _bcPressed;
    private void Broadcast_Down(object sender, MouseButtonEventArgs e)
    {
        if (DataContext is not PttOriginateViewModel vm || sender is not UIElement u) return;
        _bcPressed = true;
        u.CaptureMouse();
        vm.BroadcastDown();
        e.Handled = true;
    }
    private void Broadcast_Up(object sender, MouseButtonEventArgs e) { BroadcastRelease(sender); e.Handled = true; }
    private void Broadcast_LostCapture(object sender, MouseEventArgs e) => BroadcastRelease(sender);
    private void Broadcast_TouchDown(object sender, TouchEventArgs e)
    {
        if (DataContext is not PttOriginateViewModel vm) return;
        _bcPressed = true;
        vm.BroadcastDown();
        e.Handled = true;
    }
    private void Broadcast_TouchUp(object sender, TouchEventArgs e) { BroadcastRelease(sender); e.Handled = true; }
    private void BroadcastRelease(object sender)
    {
        if (!_bcPressed) return;
        _bcPressed = false;
        if (sender is UIElement u && u.IsMouseCaptured) u.ReleaseMouseCapture();
        (DataContext as PttOriginateViewModel)?.BroadcastUp();
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
