// 주 창 — 고정 배치(모드마다 한 화면, §3.3 — 도킹·프리셋 없음, 창 위치와 칸 경계만 기억)·감청 창 관리(§5, [창으로] 로만)·
// 앱 포커스 핫키(§8: Ctrl+n·Ctrl+Shift+n·Ctrl+K·Ctrl+M·Esc·화면 전환 F1~F4)·화면 별창 관리(§3.4)·통합 검색 키 처리·레일 [설정]·트레이 최소화·종료 확인(§6).
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Shell;

public partial class MainWindow : Window
{
    private readonly MainViewModel _vm;
    private readonly LayoutStore _layout;
    private readonly Dictionary<int, MonitorWindow> _monitors = new();
    private readonly Dictionary<AppScreen, ScreenWindow> _screens = new();
    private bool _exitConfirmed;

    public MainWindow(MainViewModel vm, LayoutStore layout)
    {
        InitializeComponent();
        _vm = vm; _layout = layout;
        DataContext = vm;

        vm.MonitorWindowActivateRequested += (_, s) => { if (_monitors.TryGetValue(s.CallId, out var w)) { if (w.WindowState == WindowState.Minimized) w.WindowState = WindowState.Normal; w.Activate(); } else OpenMonitor(s); };
        vm.MonitorWindowCloseRequested += (_, s) => { if (_monitors.TryGetValue(s.CallId, out var w)) w.CloseFromSession(); };
        vm.Desk.SettingsRequested += (_, _) => OpenSettings();
        vm.ScreenPopOutRequested += (_, s) => OpenScreenWindow(s);
        vm.DispatchActivateRequested += (_, _) => { if (!IsActive) Activate(); };
        ScreenHost.ActivateFloatingRequested += (_, s) => { if (_screens.TryGetValue(s, out var w)) w.BringToFront(); };
        vm.GroupDeleteRequested += (_, g) => DeleteGroup(g);
        vm.Desk.LogoutRequested += (_, _) => { if (ConfirmLeave("로그아웃")) { _exitConfirmed = true; ((App)Application.Current).Logout(); } };
        vm.Desk.ExitRequested += (_, _) => { if (ConfirmLeave("종료")) { _exitConfirmed = true; ((App)Application.Current).ExitApp(); } };

        Loaded += (_, _) => { RestoreWindow(); _vm.RestoreFromSnapshot(); };
        Closing += OnClosing;
        PreviewKeyDown += OnKeyDown;
        PreviewKeyUp += OnKeyUp;
    }

    // ── 창 위치(layout.json) ──
    private void RestoreWindow()
    {
        var b = _layout.File.Window;
        if (b.Left is double wl && b.Top is double wt) { Left = wl; Top = wt; Width = b.Width; Height = b.Height; }
        WindowState = b.Maximized ? WindowState.Maximized : WindowState.Normal;
    }

    private void PersistWindow()
    {
        var r = RestoreBounds.IsEmpty || double.IsInfinity(RestoreBounds.Left) ? new Rect(Left, Top, Width, Height) : RestoreBounds;
        _layout.File.Window = new WindowBounds { Left = r.Left, Top = r.Top, Width = r.Width, Height = r.Height, Maximized = WindowState == WindowState.Maximized };
        _layout.Save();
    }

    /// <summary>드롭다운 항목 클릭 → 드롭다운 닫기(Command 는 그대로 실행된다).</summary>
    private void DropItem_Click(object sender, RoutedEventArgs e) { MonDrop.IsChecked = false; SessionDrop.IsChecked = false; }

    // ── 레일 [설정] — 화면이 아니라 설정 창을 연다. 누름이 남긴 켜짐 표시는 곧바로 내린다 ──
    private void RailSettings_Click(object sender, RoutedEventArgs e)
    {
        ((ToggleButton)sender).SetCurrentValue(ToggleButton.IsCheckedProperty, false);
        _vm.Desk.OpenSettingsCommand.Execute(null);
    }

    // ── 감청 창(§5) — [창으로] 를 눌렀을 때만 ──
    private void OpenMonitor(SessionItem s)
    {
        if (_monitors.ContainsKey(s.CallId)) return;
        var w = new MonitorWindow(new MonitorWindowViewModel(_vm.Session, s), _vm.Session, _layout) { Owner = null };
        w.Closed += (_, _) => _monitors.Remove(s.CallId);
        _monitors[s.CallId] = w;
        w.Show();                                     // 포커스를 훔치지 않는다(ShowActivated=false)
    }

    // ── 앱 포커스 핫키 (§8): 보류/음소거·Ctrl+1..9·화면 전환 F1~F4 ──
    // 화면 별창(ScreenWindow)도 같은 규칙으로 여기 라우팅한다 — 핫키는 창이 아니라 앱의 것이다.
    internal void RouteKeyDown(KeyEventArgs e) => OnKeyDown(this, e);
    internal void RouteKeyUp(KeyEventArgs e) => OnKeyUp(this, e);

    private void OnKeyDown(object sender, KeyEventArgs e)
    {
        // 입력란에 포커스가 있으면 "글자를 넣는 키"만 양보한다 — 관리 화면은 입력 폼투성이라 여기서 다 버리면 폴백 PTT·화면 전환이 죽는다
        if (Keyboard.FocusedElement is System.Windows.Controls.Primitives.TextBoxBase or System.Windows.Controls.PasswordBox && IsTypingKey(e)) return;
        var map = _vm.Session.Settings.Current.HotKeys;
        // Ctrl+Shift+n = 발언 대상 토글(다중) · Ctrl+n = 발언 대상을 그 채널 하나로(+ 메시지 따라가기)
        if (Keyboard.Modifiers == (ModifierKeys.Control | ModifierKeys.Shift) && e.Key >= Key.D1 && e.Key <= Key.D9) { _vm.ToggleChannel(e.Key - Key.D0); e.Handled = true; return; }
        if (Keyboard.Modifiers == ModifierKeys.Control && e.Key >= Key.D1 && e.Key <= Key.D9) { _vm.SelectChannel(e.Key - Key.D0); e.Handled = true; return; }
        // Ctrl+K 통합 검색 · Ctrl+M [통화] (기록 — 문자)
        if (Keyboard.Modifiers == ModifierKeys.Control && e.Key == Key.K && !e.IsRepeat) { _vm.People.SearchOpen = !_vm.People.SearchOpen; if (_vm.People.SearchOpen && !IsActive) Activate(); e.Handled = true; return; }
        if (Keyboard.Modifiers == ModifierKeys.Control && e.Key == Key.M && !e.IsRepeat) { _vm.SetModeCommand.Execute("call"); e.Handled = true; return; }
        // Esc = 메뉴·키패드·(고정하지 않은) 패널 닫기
        if (Keyboard.Modifiers == ModifierKeys.None && e.Key == Key.Escape && CloseTransients()) { e.Handled = true; return; }
        foreach (var name in HotKeyMap.LocalNames)
            if (map.TryGetValue(name, out var t) && CimsUe.Platform.HotKey.TryParse(t, out var hk) && Matches(hk, e)) { _vm.OnHotKey(name, true); e.Handled = true; return; }
        // 전역 핫키 등록에 실패한 키(충돌)는 앱 포커스에서라도 동작
        foreach (var name in _vm.HotKeys.Conflicts)
            if (map.TryGetValue(name, out var t) && CimsUe.Platform.HotKey.TryParse(t, out var hk) && Matches(hk, e) && !e.IsRepeat) { _vm.OnHotKey(name, true); e.Handled = true; return; }
        // 화면 전환 F1~F4 — 설정 핫키가 같은 키를 쓰면 위에서 먼저 잡힌다
        if (Keyboard.Modifiers == ModifierKeys.None && AppScreens.OfFunctionKey(e.Key) is { } screen && !e.IsRepeat)
        {
            _vm.ShowScreenCommand.Execute(screen); e.Handled = true;
            // 별창에서 눌렀을 때 결과가 보이게 — 그 화면이 별창이면 그 별창을, 아니면 주 창을 앞으로
            if (_screens.TryGetValue(screen, out var sw)) sw.BringToFront();
            else if (!IsActive) Activate();
        }
    }

    /// <summary>입력란이 소비할 키인가 — 수식키(Ctrl/Alt/Win) 없는, F키·Esc 가 아닌 키.</summary>
    private static bool IsTypingKey(KeyEventArgs e)
    {
        if ((Keyboard.Modifiers & (ModifierKeys.Control | ModifierKeys.Alt | ModifierKeys.Windows)) != 0) return false;
        var k = e.Key == Key.System ? e.SystemKey : e.Key;
        if (k == Key.Escape) return false;
        return k is < Key.F1 or > Key.F24;
    }

    /// <summary>열린 메뉴·키패드·패널을 닫는다(안쪽 것부터 하나) — 닫았으면 true.</summary>
    private bool CloseTransients()
    {
        if (_vm.People.MenuOpen) { _vm.People.MenuOpen = false; return true; }
        if (_vm.People.SearchOpen) { _vm.People.SearchOpen = false; return true; }
        if (_vm.KeypadOpen) { _vm.KeypadOpen = false; return true; }
        if (_vm.Panel.IsOpen && !_vm.Panel.Pinned && !_vm.Panel.IsGroup) { _vm.Panel.View = PanelView.None; return true; }
        return false;
    }

    // ── 통합 검색 팝업(Ctrl+K) — 열리면 입력란 포커스, ↑↓ 이동 · Enter 첫 행동 ──
    private void SearchPop_Opened(object sender, EventArgs e) => Dispatcher.BeginInvoke(() => { SearchBox.Focus(); SearchBox.SelectAll(); });
    private void SearchBox_KeyDown(object sender, KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Down: _vm.People.Move(+1); e.Handled = true; break;
            case Key.Up: _vm.People.Move(-1); e.Handled = true; break;
            case Key.Enter: _vm.People.Enter(); e.Handled = true; break;
            case Key.Escape: _vm.People.SearchOpen = false; e.Handled = true; break;
        }
    }

    private void OnKeyUp(object sender, KeyEventArgs e)
    {
        var map = _vm.Session.Settings.Current.HotKeys;
        if (_vm.HotKeys.Conflicts.Contains("ptt") && map.TryGetValue("ptt", out var t) && CimsUe.Platform.HotKey.TryParse(t, out var hk)
            && KeyInterop.VirtualKeyFromKey(e.Key == Key.System ? e.SystemKey : e.Key) == hk.VirtualKey) _vm.OnHotKey("ptt", false);
    }

    private static bool Matches(CimsUe.Platform.HotKey hk, KeyEventArgs e)
    {
        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        if (KeyInterop.VirtualKeyFromKey(key) != hk.VirtualKey) return false;
        var m = CimsUe.Platform.HotKeyModifiers.None;
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Control)) m |= CimsUe.Platform.HotKeyModifiers.Control;
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Shift)) m |= CimsUe.Platform.HotKeyModifiers.Shift;
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Alt)) m |= CimsUe.Platform.HotKeyModifiers.Alt;
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Windows)) m |= CimsUe.Platform.HotKeyModifiers.Win;
        return m == hk.Modifiers;
    }

    // ── PTT 그룹 삭제 확인 (GMS DELETE — 본인 소유만) ──
    private async void DeleteGroup(GroupInfo g)
    {
        var live = _vm.Session.SessionOfGroup(g.Id) ?? _vm.Session.ListenOfGroup(g.Id);
        string extra = live is not null ? "\n진행 중인 세션이 있습니다 — 삭제하면 서버가 세션을 정리합니다." : "";
        if (!ConfirmWindow.Ask(this, "그룹 삭제", $"그룹 '{g.Name}' ({g.Id}) 을 삭제할까요?\n멤버 {g.MemberCount}명의 단말에서도 사라집니다.{extra}", "삭제", danger: true)) return;
        await _vm.Session.DeleteGroupAsync(g);
    }

    // ── 화면 별창(§3.4) — 화면당 하나. 닫히면 주 창 화면으로 돌아온다 ──
    private void OpenScreenWindow(AppScreen s)
    {
        if (_screens.TryGetValue(s, out var existing)) { existing.BringToFront(); return; }
        var w = new ScreenWindow(_vm, s) { Owner = this };
        w.Closed += (_, _) => _screens.Remove(s);
        _screens[s] = w;
        w.Show();
    }

    // ── 설정·종료 ──
    private void OpenSettings()
    {
        var w = new SettingsWindow(new SettingsViewModel(_vm.Session, _vm.HotKeys)) { Owner = this };
        w.ShowDialog();
        _vm.Desk.RefreshIdentity();
        ((App)Application.Current).ApplyTheme(_vm.Session.Settings.Current.Theme);
    }

    private bool ConfirmLeave(string what)
    {
        int live = _vm.Session.Sessions.Count(s => s.IsLive);
        if (live == 0) return true;
        return ConfirmWindow.Ask(this, what, $"진행 중인 세션·감청이 {live}개 있습니다. {what}할까요?", what);
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        PersistWindow();
        if (_exitConfirmed || ((App)Application.Current).IsExiting) return;
        if (_vm.Session.Settings.Current.MinimizeToTray) { e.Cancel = true; WindowState = WindowState.Minimized; ShowInTaskbar = true; return; }
        if (!ConfirmLeave("종료")) { e.Cancel = true; return; }
        _exitConfirmed = true;
        ((App)Application.Current).ExitApp();
    }

    public void ActivateFromSecondInstance()
    {
        if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
        Show(); Activate();
    }

    /// <summary>로그아웃 — 창·ViewModel 은 앱 수명 동안 하나만 두고(세션 이벤트 구독이 로그인마다 쌓이지 않게) 숨긴다. 감청 창은 닫는다.</summary>
    public void HideForLogout()
    {
        PersistWindow();
        foreach (var w in _monitors.Values.ToList()) w.CloseFromSession();
        foreach (var w in _screens.Values.ToList()) w.Close();
        _exitConfirmed = false;
        Hide();
    }

    /// <summary>재로그인 — 숨긴 창을 다시 보이고 스냅샷에서 화면을 재구성한다.</summary>
    public void ShowAfterLogin()
    {
        _vm.RestoreFromSnapshot();
        if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
        Show(); Activate();
    }
}
