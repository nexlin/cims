// 앱 진입 — 단일 인스턴스(명명 Mutex) · SynchronizationContext 캡처 · 전역 예외 · 테마 · 로그인→메인 · 1초 틱 · 네트워크 복귀 재등록 (§6).
using System.Net.NetworkInformation;
using System.Windows;
using System.Windows.Threading;
using CimsUe.Platform;
using DispatchDesktop.Services;
using DispatchDesktop.Shell;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop;

public partial class App : Application
{
    private SingleInstance? _instance;
    private AppLog? _log;
    private DispatchSession? _session;
    private HotKeyMap? _hotKeys;
    private LayoutStore? _layout;
    private MainWindow? _main;
    private DispatcherTimer? _tick;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        _instance = new SingleInstance(AppPaths.InstanceName);
        if (!_instance.IsFirst) { Shutdown(0); return; }
        _instance.ActivationRequested += (_, _) => _main?.ActivateFromSecondInstance();

        AppPaths.Ensure();
        _log = new AppLog();
        DispatcherUnhandledException += (_, ex) => { _log.Error("unhandled", ex.Exception); MessageBox.Show(ex.Exception.Message, "오류", MessageBoxButton.OK, MessageBoxImage.Error); ex.Handled = true; };
        AppDomain.CurrentDomain.UnhandledException += (_, ex) => _log.Error("fatal", ex.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, ex) => { _log.Error("task", ex.Exception); ex.SetObserved(); };

        var settings = new SettingsStore();
        settings.Load();
        _log.MinLevel = settings.Current.LogLevel;
        ApplyTheme(settings.Current.Theme);

        var directory = new DirectoryService();
        directory.Load(settings.Current.DirectoryCsv.Length > 0 ? settings.Current.DirectoryCsv : null);

        _session = new DispatchSession(settings, directory, _log);           // UI 스레드에서 생성 — SynchronizationContext.Current 캡처
        _hotKeys = new HotKeyMap();
        _layout = new LayoutStore();
        _layout.Load();

        _tick = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromSeconds(1) };
        NetworkChange.NetworkAvailabilityChanged += (_, a) => { if (a.IsAvailable) Dispatcher.BeginInvoke(() => _session?.RefreshRegistrations()); };

        _log.Info($"start {CimsUe.Engine.Version}");
        // --ui-preview: 로그인·엔진 없이 메인 화면만(화면 배치·바인딩 점검용 개발 스위치). 프로파일이 없으므로 소프트폰 모드 표시.
        if (e.Args.Contains("--ui-preview", StringComparer.OrdinalIgnoreCase)) { ShowMain(); return; }
        _ = RunLoginAsync();
    }

    private async Task RunLoginAsync()
    {
        var s = _session!;
        var login = new LoginViewModel(s);
        bool ok = false;
        if (s.HasSavedLogin) ok = await login.ResumeAsync();
        if (!ok)
        {
            var w = new LoginWindow(login);
            if (w.ShowDialog() != true) { ExitApp(); return; }
        }
        ShowMain();
    }

    private void ShowMain()
    {
        var s = _session!;
        var conflicts = _hotKeys!.Apply(s.Settings.Current.HotKeys);
        if (conflicts.Count > 0) s.Notify.Warn("핫키 충돌: " + string.Join(", ", conflicts), "다른 프로그램이 같은 키를 등록했습니다 — 설정에서 바꾸세요");
        var vm = new MainViewModel(s, _layout!, _hotKeys);
        _main = new MainWindow(vm, _layout!);
        _tick!.Tick += (_, _) => vm.Tick(DateTime.Now);
        _tick.Start();
        _main.Show();
        if (!s.HasDesk) s.Notify.Info("관제 데스크 미배정 — 일반 소프트폰 모드", "콘솔 관리 › 관제 그룹에서 배정하면 그룹원 띠·대기열·청취가 켜집니다");
    }

    public void ApplyTheme(string theme)
    {
        var dict = Resources.MergedDictionaries;
        var uri = new Uri(theme == "dark" ? "Themes/Dark.xaml" : "Themes/Light.xaml", UriKind.Relative);
        var current = dict.FirstOrDefault(d => d.Source is not null && d.Source.OriginalString.Contains("Themes/", StringComparison.OrdinalIgnoreCase) && !d.Source.OriginalString.Contains("Styles"));
        if (current is not null && current.Source!.OriginalString.EndsWith(uri.OriginalString, StringComparison.OrdinalIgnoreCase)) return;
        if (current is not null) dict.Remove(current);
        dict.Insert(0, new ResourceDictionary { Source = uri });
    }

    /// <summary>로그아웃 — 등록 해제·토큰 폐기 후 로그인 창으로.</summary>
    public void Logout()
    {
        _tick?.Stop();
        _main?.Close();
        _main = null;
        _session!.Logout();
        _ = RunLoginAsync();
    }

    public void ExitApp()
    {
        _tick?.Stop();
        try { _session?.Logout(); } catch (Exception ex) { _log?.Warn("logout on exit: " + ex.Message); }
        Shutdown(0);
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _hotKeys?.Dispose();
        _session?.Dispose();
        _instance?.Dispose();
        _log?.Info("exit");
        _log?.Dispose();
        base.OnExit(e);
    }
}
