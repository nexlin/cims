// 감청 창 — [창으로] 로 연 별창(기본 표면은 인라인 행·타 채널 행). 창 닫기 = 종료(설정 "닫기 전 확인"), 원 세션 종료 → "통화 종료됨" 3초 후 자동 닫힘, 위치 기억(layout.json Monitor).
using System.ComponentModel;
using System.Windows;
using DispatchDesktop.Services;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Shell;

public partial class MonitorWindow : Window
{
    private readonly MonitorWindowViewModel _vm;
    private readonly DispatchSession _s;
    private readonly LayoutStore _layout;
    private bool _sessionEnded;

    public MonitorWindow(MonitorWindowViewModel vm, DispatchSession s, LayoutStore layout)
    {
        InitializeComponent();
        _vm = vm; _s = s; _layout = layout;
        DataContext = vm;
        var b = layout.File.Monitor;
        if (b.Left is double bl && b.Top is double bt) { Left = bl; Top = bt; }
        if (b.Width > 0) Width = b.Width;
        if (b.Height > 0) Height = b.Height;
        Closing += OnClosing;
        LocationChanged += (_, _) => Remember();
        SizeChanged += (_, _) => Remember();
    }

    private void Remember()
    {
        if (WindowState != WindowState.Normal) return;
        var m = _layout.File.Monitor;
        m.Left = Left; m.Top = Top; m.Width = Width; m.Height = Height;
    }

    /// <summary>세션이 끝나 창을 닫는다 — 3초 안내 후.</summary>
    public async void CloseFromSession()
    {
        _sessionEnded = true;
        _vm.ClosingNote = _vm.IsVolte ? "통화 종료됨 — 창을 닫습니다" : "세션 종료됨 — 창을 닫습니다";
        await Task.Delay(3000);
        Close();
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        _layout.Save();
        if (_sessionEnded || ((App)Application.Current).IsExiting) return;   // 앱 종료 — 확인은 주 창이 했고 호 정리는 ExitApp 의 Logout
        if (_s.Settings.Current.ConfirmCloseMonitor
            && !ConfirmWindow.Ask(this, "청취 종료", "창을 닫으면 청취가 종료됩니다. 계속할까요?", "청취 종료"))
        { e.Cancel = true; return; }
        _sessionEnded = true;                       // 종료 이벤트가 다시 닫지 않도록
        _vm.StopCommand.Execute(null);
    }
}
