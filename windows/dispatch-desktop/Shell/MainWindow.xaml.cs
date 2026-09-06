// 주 창 — 도킹 배치 잠금·프리셋(AvalonDock 직렬화)·감청 창 관리(§5)·앱 포커스 핫키(§8)·트레이 최소화·종료 확인(§6).
using System.ComponentModel;
using System.IO;
using System.Windows;
using System.Windows.Input;
using AvalonDock.Layout;
using AvalonDock.Layout.Serialization;
using DispatchDesktop.Models;
using DispatchDesktop.Services;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Shell;

public partial class MainWindow : Window
{
    private readonly MainViewModel _vm;
    private readonly LayoutStore _layout;
    private readonly Dictionary<int, MonitorWindow> _monitors = new();
    private bool _exitConfirmed;

    public MainWindow(MainViewModel vm, LayoutStore layout)
    {
        InitializeComponent();
        _vm = vm; _layout = layout;
        DataContext = vm;

        vm.MonitorWindowRequested += (_, s) => OpenMonitor(s);
        vm.MonitorWindowActivateRequested += (_, s) => { if (_monitors.TryGetValue(s.CallId, out var w)) { if (w.WindowState == WindowState.Minimized) w.WindowState = WindowState.Normal; w.Activate(); } else OpenMonitor(s); };
        vm.MonitorWindowCloseRequested += (_, s) => { if (_monitors.TryGetValue(s.CallId, out var w)) w.CloseFromSession(); };
        vm.Desk.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(DeskViewModel.LayoutLocked)) ApplyLock(vm.Desk.LayoutLocked); };
        vm.Desk.PresetApplyRequested += (_, name) => ApplyPreset(name);
        vm.Desk.PresetSaveRequested += (_, name) => SavePreset(name);
        vm.Desk.SettingsRequested += (_, _) => OpenSettings();
        vm.GroupEditRequested += (_, g) => { var w = new GroupEditWindow(g) { Owner = this }; w.ShowDialog(); };
        vm.GroupDeleteRequested += (_, g) => DeleteGroup(g);
        vm.Desk.LogoutRequested += (_, _) => { if (ConfirmLeave("로그아웃")) { _exitConfirmed = true; ((App)Application.Current).Logout(); } };
        vm.Desk.ExitRequested += (_, _) => { if (ConfirmLeave("종료")) { _exitConfirmed = true; ((App)Application.Current).ExitApp(); } };

        ApplyDockTheme(vm.Session.Settings.Current.Theme);
        Loaded += (_, _) => { ApplyPreset(_layout.File.Current, restoreWindow: true); ApplyLock(vm.Desk.LayoutLocked); _vm.RestoreFromSnapshot(); };
        Closing += OnClosing;
        PreviewKeyDown += OnKeyDown;
        PreviewKeyUp += OnKeyUp;
    }

    // ── 도킹 배치 (§3.3) ──
    private IEnumerable<LayoutAnchorable> Anchorables => Dock.Layout.Descendents().OfType<LayoutAnchorable>();

    private void ApplyLock(bool locked)
    {
        foreach (var a in Anchorables) { a.CanFloat = !locked; a.CanMove = !locked; a.CanDockAsTabbedDocument = false; a.CanClose = false; a.CanHide = false; }
    }

    private string SerializeDock()
    {
        using var sw = new StringWriter();
        new XmlLayoutSerializer(Dock).Serialize(sw);
        return sw.ToString();
    }

    private void DeserializeDock(string xml)
    {
        if (xml.Length == 0) return;
        var contents = Anchorables.ToDictionary(a => a.ContentId, a => a.Content);
        var ser = new XmlLayoutSerializer(Dock);
        ser.LayoutSerializationCallback += (_, e) => { if (e.Model.ContentId is { } id && contents.TryGetValue(id, out var c)) e.Content = c; else e.Cancel = true; };
        using var sr = new StringReader(xml);
        try { ser.Deserialize(sr); }
        catch (Exception ex) { _vm.Session.Log.Warn("layout deserialize: " + ex.Message); }
        // 저장 XML 에 없던 패널은 사라지지 않도록 확인 — 없으면 왼쪽 열 끝에 붙인다
        var present = Anchorables.Select(a => a.ContentId).ToHashSet();
        foreach (var (id, content) in contents)
            if (!present.Contains(id)) Dock.Layout.RootPanel.Children.Add(new LayoutAnchorablePane(new LayoutAnchorable { ContentId = id, Title = id, Content = content, CanClose = false, CanHide = false }));
    }

    private void ApplyPreset(string name, bool restoreWindow = false)
    {
        var p = _layout.Get(name);
        if (p is null) return;
        _defaultXml ??= SerializeDock();                       // 기본 배치 = XAML 초기 상태 — 프리셋을 적용하기 전에 찍어 둔다
        if (name == LayoutStore.DefaultName) DeserializeDock(_defaultXml);
        else DeserializeDock(p.DockXml);
        if (restoreWindow && p.Window.Left is double wl && p.Window.Top is double wt)
        {
            Left = wl; Top = wt; Width = p.Window.Width; Height = p.Window.Height;
            WindowState = p.Window.Maximized ? WindowState.Maximized : WindowState.Normal;
        }
        _layout.SetCurrent(name);
        _vm.Desk.LayoutLocked = p.Locked;
        _vm.Desk.RefreshPresets();
        ApplyLock(p.Locked);
    }
    private string? _defaultXml;

    private WindowBounds CurrentBounds()
    {
        var r = RestoreBounds.IsEmpty || double.IsInfinity(RestoreBounds.Left) ? new Rect(Left, Top, Width, Height) : RestoreBounds;
        return new WindowBounds { Left = r.Left, Top = r.Top, Width = r.Width, Height = r.Height, Maximized = WindowState == WindowState.Maximized };
    }

    private void SavePreset(string name)
    {
        _layout.SaveAs(name, SerializeDock(), _vm.Desk.LayoutLocked, CurrentBounds());
        _vm.Desk.RefreshPresets();
    }

    /// <summary>드롭다운 팝업 항목 클릭 → 팝업 닫기(Command 는 그대로 실행된다).</summary>
    private void DropItem_Click(object sender, RoutedEventArgs e)
    {
        MonDrop.IsChecked = false; PresetDrop.IsChecked = false; GearDrop.IsChecked = false;
    }

    /// <summary>도킹 크롬(패널 제목줄·탭·스플리터)을 앱 테마에 맞춘다 — AvalonDock VS2013 테마.</summary>
    public void ApplyDockTheme(string theme)
    {
        Dock.Theme = theme == "dark" ? new AvalonDock.Themes.Vs2013DarkTheme() : new AvalonDock.Themes.Vs2013LightTheme();
    }

    private void SavePreset_Click(object sender, RoutedEventArgs e)
    {
        DropItem_Click(sender, e);
        var dlg = new PromptWindow("배치 프리셋 저장", "프리셋 이름", _layout.File.Current == LayoutStore.DefaultName ? "" : _layout.File.Current) { Owner = this };
        if (dlg.ShowDialog() == true && dlg.Value.Trim().Length > 0) SavePreset(dlg.Value.Trim());
    }

    private void DeletePreset_Click(object sender, RoutedEventArgs e)
    {
        DropItem_Click(sender, e);
        string cur = _layout.File.Current;
        if (cur == LayoutStore.DefaultName) return;
        if (MessageBox.Show(this, $"프리셋 '{cur}' 을 삭제할까요?", "프리셋 삭제", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
        _layout.Delete(cur);
        ApplyPreset(LayoutStore.DefaultName);
    }

    /// <summary>현재 프리셋에 창 위치·잠금만 갱신(배치 XML 은 명시 저장 때만).</summary>
    private void PersistWindow()
    {
        var p = _layout.Current;
        p.Window = CurrentBounds();
        p.Locked = _vm.Desk.LayoutLocked;
        _layout.Save();
    }

    // ── 감청 창 (§5) ──
    private void OpenMonitor(SessionItem s)
    {
        if (_monitors.ContainsKey(s.CallId)) return;
        var w = new MonitorWindow(new MonitorWindowViewModel(_vm.Session, s), _vm.Session, _layout) { Owner = null };
        w.Closed += (_, _) => { _monitors.Remove(s.CallId); _vm.Desk.SyncMonitors(_vm.Session.Sessions); };
        _monitors[s.CallId] = w;
        w.Show();                                     // 포커스를 훔치지 않는다(ShowActivated=false)
    }

    // ── 앱 포커스 핫키 (§8): 보류/음소거·Ctrl+1..9 ──
    private void OnKeyDown(object sender, KeyEventArgs e)
    {
        if (Keyboard.FocusedElement is System.Windows.Controls.TextBox) return;
        var map = _vm.Session.Settings.Current.HotKeys;
        if (Keyboard.Modifiers == ModifierKeys.Control && e.Key >= Key.D1 && e.Key <= Key.D9) { _vm.SelectChannel(e.Key - Key.D0); e.Handled = true; return; }
        foreach (var name in HotKeyMap.LocalNames)
            if (map.TryGetValue(name, out var t) && CimsUe.Platform.HotKey.TryParse(t, out var hk) && Matches(hk, e)) { _vm.OnHotKey(name, true); e.Handled = true; return; }
        // 전역 핫키 등록에 실패한 키(충돌)는 앱 포커스에서라도 동작
        foreach (var name in _vm.HotKeys.Conflicts)
            if (map.TryGetValue(name, out var t) && CimsUe.Platform.HotKey.TryParse(t, out var hk) && Matches(hk, e) && !e.IsRepeat) { _vm.OnHotKey(name, true); e.Handled = true; return; }
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
        if (MessageBox.Show(this, $"그룹 '{g.Name}' ({g.Id}) 을 삭제할까요?\n멤버 {g.MemberCount}명의 단말에서도 사라집니다.{extra}", "그룹 삭제",
                            MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
        await _vm.Session.DeleteGroupAsync(g);
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
        return MessageBox.Show(this, $"진행 중인 세션·감청 창이 {live}개 있습니다. {what}할까요?", what, MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes;
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        PersistWindow();
        if (_exitConfirmed) return;
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
