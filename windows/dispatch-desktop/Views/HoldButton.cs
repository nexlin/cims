// 누르고 있는 동안 동작하는 버튼 — PTT(발언 바)·[일제 통화](채널 상세·사용자 패널). 누름 = DownCommand, 뗌 = UpCommand.
// 포인터가 버튼 밖으로 나가도 캡처로 뗌을 받고(LostMouseCapture 도 뗌), 터치도 같은 규칙이다 — 뗌을 놓치면 마이크가 열린 채 남는다.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace DispatchDesktop.Views;

public sealed class HoldButton : Button
{
    public static readonly DependencyProperty DownCommandProperty = DependencyProperty.Register(nameof(DownCommand), typeof(ICommand), typeof(HoldButton));
    public static readonly DependencyProperty UpCommandProperty = DependencyProperty.Register(nameof(UpCommand), typeof(ICommand), typeof(HoldButton));
    public static readonly DependencyProperty HoldParameterProperty = DependencyProperty.Register(nameof(HoldParameter), typeof(object), typeof(HoldButton));
    public ICommand? DownCommand { get => (ICommand?)GetValue(DownCommandProperty); set => SetValue(DownCommandProperty, value); }
    public ICommand? UpCommand { get => (ICommand?)GetValue(UpCommandProperty); set => SetValue(UpCommandProperty, value); }
    public object? HoldParameter { get => GetValue(HoldParameterProperty); set => SetValue(HoldParameterProperty, value); }

    private bool _pressed;

    private void Down()
    {
        if (_pressed) return;
        _pressed = true;
        if (DownCommand?.CanExecute(HoldParameter) == true) DownCommand.Execute(HoldParameter);
    }

    private void Up()
    {
        if (!_pressed) return;
        _pressed = false;                                   // 캡처 해제가 LostMouseCapture 로 다시 들어와도 한 번만
        if (IsMouseCaptured) ReleaseMouseCapture();
        if (UpCommand?.CanExecute(HoldParameter) == true) UpCommand.Execute(HoldParameter);
    }

    protected override void OnPreviewMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        base.OnPreviewMouseLeftButtonDown(e);
        CaptureMouse();
        Down();
        e.Handled = true;
    }
    protected override void OnPreviewMouseLeftButtonUp(MouseButtonEventArgs e) { base.OnPreviewMouseLeftButtonUp(e); Up(); e.Handled = true; }
    protected override void OnLostMouseCapture(MouseEventArgs e) { base.OnLostMouseCapture(e); Up(); }
    protected override void OnPreviewTouchDown(TouchEventArgs e) { base.OnPreviewTouchDown(e); CaptureTouch(e.TouchDevice); Down(); e.Handled = true; }
    protected override void OnPreviewTouchUp(TouchEventArgs e) { base.OnPreviewTouchUp(e); ReleaseTouchCapture(e.TouchDevice); Up(); e.Handled = true; }
}
