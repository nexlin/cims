// 앱 진입 — 단일 인스턴스(명명 Mutex) · SynchronizationContext 캡처 · 전역 예외 · 테마 · 로그인→메인 · 1초 틱 · 망 전환 재등록(코어 handleNetworkChange, §6).
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

    private MainViewModel? _mainVm;
    private bool _started;

    /// <summary>종료가 정해졌다(ExitApp·개발 스위치) — 창들의 Closing 은 다시 묻지 않는다. Shutdown 중 WPF 는 취소를 무시하므로 물어도 창은 닫히고
    /// 모달 질문은 종료만 붙잡는다. 진행 중 세션·감청 창은 주 창 [종료] 확인(ConfirmLeave)이 이미 센다.</summary>
    public bool IsExiting { get; private set; }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        // --ui-preview(개발 스위치)는 실제 앱과 다른 인스턴스 이름 — 실행 중인 관제 앱 옆에서 화면 점검용으로 띄울 수 있게.
        bool preview = e.Args.Contains("--ui-preview", StringComparer.OrdinalIgnoreCase);
        _instance = new SingleInstance(preview ? AppPaths.InstanceName + ".preview" : AppPaths.InstanceName);
        if (!_instance.IsFirst) { Shutdown(0); return; }
        _instance.ActivationRequested += (_, _) => _main?.ActivateFromSecondInstance();

        // 기동 중 예외는 창 없는 프로세스로 남지 않게 종료한다(ShutdownMode 가 명시 종료라 스스로 끝나지 않는다).
        DispatcherUnhandledException += (_, ex) =>
        {
            _log?.Error("unhandled", ex.Exception);
            MessageBox.Show(ex.Exception.Message, _started ? "오류" : "기동 실패", MessageBoxButton.OK, MessageBoxImage.Error);
            ex.Handled = true;
            if (!_started) Shutdown(1);
        };
        AppDomain.CurrentDomain.UnhandledException += (_, ex) => _log?.Error("fatal", ex.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, ex) => { _log?.Error("task", ex.Exception); ex.SetObserved(); };
        try { StartCore(e); }
        catch (Exception ex)
        {
            _log?.Error("startup", ex);
            MessageBox.Show($"앱을 시작할 수 없습니다.\n{ex.Message}\n\n데이터 폴더: {AppPaths.Root}", "기동 실패", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown(1);
        }
    }

    private void StartCore(StartupEventArgs e)
    {
        AppPaths.Ensure();
        _log = new AppLog();

        var settings = new SettingsStore();
        settings.Load();
        _log.MinLevel = settings.Current.LogLevel;
        ApplyTheme(settings.Current.Theme);

        var directory = new DirectoryService();
        directory.Load(settings.Current.DirectoryCsv.Length > 0 ? settings.Current.DirectoryCsv : null);

        _session = new DispatchSession(settings, directory, _log);           // UI 스레드에서 생성 — SynchronizationContext.Current 캡처
        // 토큰 자격이 되살릴 수 없게 끝났다(refresh 폐기·회전 실패) — 통화만 살아 있는 반쯤 로그인된 상태를 두지 않는다(§6 세션 수명).
        _session.CredentialsEnded += (_, why) => { _log!.Warn("session ended: " + why); _pendingLoginError = why; Logout(); };
        _hotKeys = new HotKeyMap();
        _layout = new LayoutStore();
        _layout.Load();

        _tick = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromSeconds(1) };
        // 망 전환·복귀 — 세션이 합쳐서(2 초) 주소 지문이 바뀐 때만 코어에 알린다(TCP/TLS 연결 종료 + 계정별 재등록)
        NetworkChange.NetworkAvailabilityChanged += (_, _) => _session?.NoteNetworkChange();
        NetworkChange.NetworkAddressChanged += (_, _) => _session?.NoteNetworkChange();

        _log.Info($"start {CimsUe.Engine.Version}");
        _started = true;
        // --ui-preview: 로그인·엔진 없이 메인 화면만(화면 배치·바인딩 점검용 개발 스위치). 프로파일이 없으므로 소프트폰 모드 표시.
        if (e.Args.Contains("--ui-preview", StringComparer.OrdinalIgnoreCase))
        {
            ShowMain();
            // --ui-preview-screen=history|groups|admin: 관제 외 화면(§3.4)으로 열어 서버 없이 XAML 자원·바인딩 점검(목록은 "로그인 전" 오류로 비어 있다).
            // 관리 화면은 관리 범위 검사를 건너뛴다(프로파일이 없다). 구 스위치 --ui-preview-management = admin.
            string? screenArg = e.Args.FirstOrDefault(a => a.StartsWith("--ui-preview-screen=", StringComparison.OrdinalIgnoreCase))?.Split('=', 2)[1]
                                ?? (e.Args.Contains("--ui-preview-management", StringComparer.OrdinalIgnoreCase) ? "admin" : null);
            // 창의 Loaded(스냅샷 재구성 — 화면을 관제로 되돌린다) 뒤에 적용해야 그 화면이 찍힌다
            if (screenArg is not null && _mainVm is not null)
            {
                var screen = screenArg.ToLowerInvariant() switch { "history" => Models.AppScreen.History, "groups" => Models.AppScreen.PttGroups, _ => Models.AppScreen.Admin };
                var vmS = _mainVm;
                _main!.Dispatcher.BeginInvoke(DispatcherPriority.ContextIdle, () => vmS.Screen = screen);
            }
            // --ui-preview-canvas: 관제 두 화면(§3.1)에 표본 채널·세션·대기열·기록·메시지를 심는다. --ui-preview-banner=alerts|incoming|none 으로 배너 층을 고른다.
            string Arg(string name) => e.Args.FirstOrDefault(a => a.StartsWith(name + "=", StringComparison.OrdinalIgnoreCase))?.Split('=', 2)[1] ?? "";
            if (e.Args.Contains("--ui-preview-canvas", StringComparer.OrdinalIgnoreCase) && _mainVm is not null)
            {
                _mainVm.SeedCanvasPreview(Arg("--ui-preview-banner") is { Length: > 0 } banner ? banner : "alerts");
                // --ui-preview-mode=ptt|call · --ui-preview-panel=channel|other|users|group|event|dir · --ui-preview-keypad — 창이 뜬 뒤(RestoreFromSnapshot 다음) 적용
                string mode = Arg("--ui-preview-mode"), panel = Arg("--ui-preview-panel");
                bool keypad = e.Args.Contains("--ui-preview-keypad", StringComparer.OrdinalIgnoreCase);
                var vm0 = _mainVm;
                _main!.Dispatcher.BeginInvoke(DispatcherPriority.ContextIdle, () => vm0.ApplyPreview(mode, panel, keypad));
            }
            // --ui-preview-history=call|ptt: 이력 화면(§4.6)에 표본 하루를 심어(시간대 밴드·표/카드·선택 세션 패널) 서버 없이 그려 본다.
            if (e.Args.FirstOrDefault(a => a.StartsWith("--ui-preview-history=", StringComparison.OrdinalIgnoreCase))?.Split('=', 2)[1] is { Length: > 0 } histKind && _mainVm is not null)
                _mainVm.HistoryScreen.SeedPreview(histKind.Equals("ptt", StringComparison.OrdinalIgnoreCase) ? Models.HistoryKind.Ptt : Models.HistoryKind.Call);
            // --ui-preview-zoom=<배율>: 발언 타임라인 확대 상태로 그려 본다(눈금·트랙 폭·가로 스크롤).
            if (e.Args.FirstOrDefault(a => a.StartsWith("--ui-preview-zoom=", StringComparison.OrdinalIgnoreCase))?.Split('=', 2)[1] is { Length: > 0 } zoomArg && _mainVm is not null
                && double.TryParse(zoomArg, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out double z))
                _mainVm.HistoryScreen.TalkZoom = Math.Clamp(z, 1, ViewModels.SessionHistoryViewModel.TalkZoomMax);
            // --ui-preview-shot=<png>: 주 창을 그려 PNG 로 저장하고 종료 — 화면 잠금·원격 세션에서도 XAML 점검이 되게(화면 캡처가 아니라 WPF 렌더).
            if (e.Args.FirstOrDefault(a => a.StartsWith("--ui-preview-shot=", StringComparison.OrdinalIgnoreCase))?.Split('=', 2)[1] is { Length: > 0 } shot && _main is not null)
            {
                var t = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
                t.Tick += (_, _) =>
                {
                    t.Stop();
                    try
                    {
                        var w = _main; int pw = (int)Math.Ceiling(w.ActualWidth), ph = (int)Math.Ceiling(w.ActualHeight);
                        var rtb = new System.Windows.Media.Imaging.RenderTargetBitmap(pw, ph, 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
                        rtb.Render(w);
                        var enc = new System.Windows.Media.Imaging.PngBitmapEncoder();
                        enc.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(rtb));
                        using var fs = System.IO.File.Create(shot);
                        enc.Save(fs);
                        _log?.Info($"preview shot {pw}x{ph} → {shot}");
                    }
                    catch (Exception ex) { _log?.Error("preview shot", ex); }
                    IsExiting = true;                       // 표본 세션·감청 창의 종료 확인을 띄우지 않는다. ExitApp(Logout)은 저장된 로그인을 지우므로 쓰지 않는다
                    Shutdown(0);
                };
                t.Start();
            }
            return;
        }
        _ = RunLoginAsync();
    }

    private async Task RunLoginAsync()
    {
        try
        {
            var s = _session!;
            var login = new LoginViewModel(s);
            if (_pendingLoginError.Length > 0) { login.Error = _pendingLoginError; _pendingLoginError = ""; }
            bool ok = false;
            if (s.HasSavedLogin) ok = await login.ResumeAsync();
            if (!ok)
            {
                var w = new LoginWindow(login);
                if (w.ShowDialog() != true) { ExitApp(); return; }
            }
            ShowMain();
        }
        catch (Exception ex)
        {
            _log?.Error("login flow", ex);
            MessageBox.Show(ex.Message, "로그인 실패", MessageBoxButton.OK, MessageBoxImage.Error);
            ExitApp();
        }
    }

    /// <summary>메인 창·ViewModel 은 앱 수명 동안 하나 — 세션·핫키·틱은 싱글턴이라 로그인마다 새로 만들면 이벤트 구독이 쌓인다(SDS 이중 저장·PTT 이중 요청).
    /// 재로그인은 숨겨 둔 창을 다시 보이고 스냅샷에서 재구성한다.</summary>
    private void ShowMain()
    {
        var s = _session!;
        var conflicts = _hotKeys!.Apply(s.Settings.Current.HotKeys);
        if (conflicts.Count > 0) s.Notify.Warn("핫키 충돌: " + string.Join(", ", conflicts), "다른 프로그램이 같은 키를 등록했습니다 — 설정에서 바꾸세요");
        if (_mainVm is null)
        {
            var vm = _mainVm = new MainViewModel(s, _layout!, _hotKeys);
            _tick!.Tick += (_, _) => vm.Tick(DateTime.Now);
        }
        if (_main is null) { _main = new MainWindow(_mainVm, _layout!); _main.Show(); }
        else _main.ShowAfterLogin();
        _tick!.Start();
        if (!s.HasDesk) s.Notify.Info("관제 데스크 미배정 — 일반 소프트폰 모드", "콘솔 관리 › 관제 그룹에서 배정하면 그룹원 띠·대기열·청취가 켜집니다");
    }

    public void ApplyTheme(string theme)
    {
        var dict = Resources.MergedDictionaries;
        bool dark = theme == "dark";
        var uri = new Uri(dark ? "Themes/Dark.xaml" : "Themes/Light.xaml", UriKind.Relative);
        var current = dict.FirstOrDefault(d => d.Source is not null && (d.Source.OriginalString.EndsWith("Themes/Light.xaml", StringComparison.OrdinalIgnoreCase) || d.Source.OriginalString.EndsWith("Themes/Dark.xaml", StringComparison.OrdinalIgnoreCase)));
        if (current is not null && current.Source!.OriginalString.EndsWith(uri.OriginalString, StringComparison.OrdinalIgnoreCase)) return;
        if (current is not null) dict.Remove(current);
        dict.Insert(0, new ResourceDictionary { Source = uri });
    }

    /// <summary>자격 만료로 끝난 세션의 사유 — 다음 로그인 창에 한 번 띄운다.</summary>
    private string _pendingLoginError = "";

    /// <summary>로그아웃 — 등록 해제·토큰 폐기 후 로그인 창으로.</summary>
    public void Logout()
    {
        _tick?.Stop();
        _main?.HideForLogout();
        _session!.Logout();
        _ = RunLoginAsync();
    }

    public void ExitApp()
    {
        IsExiting = true;
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
