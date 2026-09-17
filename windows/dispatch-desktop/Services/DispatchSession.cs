// 관제 세션 — 코어(Engine·CscClient) 위의 앱 상태 투영과 관제 동작 진입점(dispatch_desktop_ui.md §6·§11).
//
// 원칙: UI 는 코어 상태의 투영 — Sessions/Groups/Dialogs 는 CallInfo·FloorInfo·DialogInfo 스냅샷과 구독 이벤트에서 파생하고
// 앱은 별도 상태 기계를 갖지 않는다. 내역(ActivityLog)만 앱이 축적한다. 파사드 이벤트는 SynchronizationContext(UI 스레드)로 들어온다.
// 프로토콜 코드를 해석하는 곳은 ResponseText 뿐이다.
using System.Collections.ObjectModel;
using CimsUe;
using CimsUe.Platform;
using CommunityToolkit.Mvvm.ComponentModel;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;

using System.IO;

namespace DispatchDesktop.Services;

public sealed partial class DispatchSession : ObservableObject, IDisposable
{
    public Engine Engine { get; }
    public SettingsStore Settings { get; }
    public DirectoryService Directory { get; }
    public ActivityLog Activity { get; } = new();
    public MessageStore Messages { get; }
    public AudioPolicy Audio { get; } = new();
    public Notifications Notify { get; } = new();
    public CredentialStore Credentials { get; }
    public AudioEndpoints Endpoints { get; }
    public AppLog Log { get; }

    private CscClient? _csc;
    private TokenSet? _tokens;
    private string _loginPw = "";
    private readonly Dictionary<int, AccountKind> _accountKinds = new();
    private readonly Dictionary<int, Operation> _pendingOps = new();
    private readonly Dictionary<int, SessionItem> _pendingConsult = new();
    private readonly Dictionary<int, DateTime> _regRetryAt = new();
    private readonly Dictionary<int, int> _regBackoff = new();
    private readonly HashSet<string> _watched = new(StringComparer.OrdinalIgnoreCase);

    [ObservableProperty] private Profile? _profile;
    [ObservableProperty] private RegInfo _volteReg = RegInfo.Empty;
    [ObservableProperty] private RegInfo _pttReg = RegInfo.Empty;
    [ObservableProperty] private Account? _volte;
    [ObservableProperty] private Account? _ptt;
    [ObservableProperty] private bool _isReady;
    [ObservableProperty] private string _headsetName = "";
    [ObservableProperty] private string _speakerName = "";
    [ObservableProperty] private string _captureName = "";

    public ObservableCollection<SessionItem> Sessions { get; } = new();
    public ObservableCollection<GroupInfo> Groups { get; } = new();
    public ObservableCollection<DialogRow> Dialogs { get; } = new();

    public event EventHandler<SessionItem>? SessionAdded;
    public event EventHandler<SessionItem>? SessionChanged;
    public event EventHandler<SessionItem>? SessionEnded;
    public event EventHandler<SessionItem>? IncomingCall;
    public event EventHandler<(SessionItem Session, FloorEvent Event)>? Floor;
    public event EventHandler<GroupInfo>? RosterChanged;
    public event EventHandler<DialogRow>? DialogChanged;
    public event EventHandler<DialogRow>? DialogEnded;
    public event EventHandler<SdsMessage>? SdsReceived;
    public event EventHandler<SipMessage>? SipMessageReceived;
    public event EventHandler<RequestResult>? RequestCompleted;
    public event EventHandler? ProfileApplied;
    public event EventHandler? AudioChanged;

    public DispatchSession(SettingsStore settings, DirectoryService directory, AppLog log)
    {
        Settings = settings;
        Directory = directory;
        Log = log;
        Engine = new Engine(SynchronizationContext.Current);
        Credentials = new CredentialStore(AppPaths.AppName);
        Endpoints = new AudioEndpoints(SynchronizationContext.Current);
        Messages = new MessageStore(AppPaths.MessagesDb);
        Messages.FailPending();
        Messages.Prune(settings.Current.MessageRetentionDays);

        Engine.Log += (_, l) => Log.Core(l.Level, l.Message);
        Engine.RegistrationChanged += (_, r) => OnRegistration(r);
        Engine.IncomingCall += (_, c) => OnIncoming(c);
        Engine.CallStateChanged += (_, c) => OnCallState(c);
        Engine.CallMediaChanged += (_, c) => OnCallMedia(c);
        Engine.FloorChanged += (_, f) => OnFloor(f);
        Engine.RosterChanged += (_, r) => OnRoster(r);
        Engine.DialogInfoReceived += (_, d) => OnDialog(d);
        Engine.SdsReceived += (_, m) => SdsReceived?.Invoke(this, m);
        Engine.MessageReceived += (_, m) => { SipMessageReceived?.Invoke(this, m); OnSipMessage(m); };
        Engine.RequestCompleted += (_, r) => RequestCompleted?.Invoke(this, r);
        Engine.HandlerFailed += (_, ex) => Log.Error("이벤트 핸들러 예외", ex);
        Engine.Stopped += (_, _) => Log.Info("engine stopped");
        Endpoints.Changed += (_, _) => OnEndpointsChanged();
    }

    // ── 서버 인증서 만료 경고 (sip_tls_signaling.md §8.6.2 — 관제사는 매일 앉아 있는 사람이라 폐쇄망에서 가장 확실한 채널) ──
    //   표면 둘: 배너 레이어(어느 화면에서나 — 관제 캔버스 포함, 닫기 없음) + 관제 요약 띠 배지(관제 밖 화면·별창). 둘 다 ServerCertExpiry 의 투영.
    /// <summary>경고 임계(일) — 서버 A-PRC-009 경고 임계와 같은 30. 자동 갱신 대상 인증서가 여기 닿았다 = 자동 갱신 실패.</summary>
    public const int ServerCertWarnDays = 30;
    /// <summary>위험 임계(일) — 서버 A-PRC-009 critical 과 같은 7. 배너가 진한 빨강으로 바뀐다.</summary>
    public const int ServerCertCriticalDays = 7;
    /// <summary>배너 재평가 주기(초) — 관측은 핸드셰이크 때 갱신되고 잔여 일수는 하루에 한 번 바뀌므로 자주 볼 이유가 없다. 등록 성공·로그인 직후엔 즉시.</summary>
    private const int ServerCertCheckSec = 60;
    private DateTime _nextServerCertCheck = DateTime.MinValue;
    /// <summary>SIP TLS(Engine)·HTTPS(CSC) 서버 인증서 중 잔여가 짧은 것. 관측 전(평문·미접속)엔 null.</summary>
    public TlsPeerExpiry? ServerCertExpiry
    {
        get
        {
            TlsPeerExpiry? best = null;
            foreach (var e in new[] { Engine.TlsPeerExpiry, _csc?.TlsPeerExpiry ?? TlsPeerExpiry.None })
                if (e.Valid && (best is null || e.DaysLeft < best.DaysLeft)) best = e;
            return best;
        }
    }
    public bool ServerCertWarning => ServerCertExpiry is { } e && e.DaysLeft <= ServerCertWarnDays;
    public bool ServerCertCritical => ServerCertExpiry is { } e && e.DaysLeft <= ServerCertCriticalDays;
    public string ServerCertWarningText => ServerCertExpiry is { } e && e.DaysLeft <= ServerCertWarnDays ? ServerCertTitle(e) + " — 운영자에게 알리세요" : "";
    public static string ServerCertTitle(TlsPeerExpiry e) => e.DaysLeft < 0 ? "서버 인증서 만료됨" : e.DaysLeft == 0 ? "서버 인증서 오늘 만료" : $"서버 인증서 {e.DaysLeft}일 후 만료";
    /// <summary>배너 본문 — 운영자에게 그대로 전달할 수 있는 사실(어느 서버·어느 인증서·만료일)과 뜻(자동 갱신 실패 신호·확인할 알람).</summary>
    public static string ServerCertSubtitle(TlsPeerExpiry e)
        => $"{e.Remote} · {e.Subject} · 만료 {e.NotAfter.ToLocalTime():yyyy-MM-dd} · 자동 갱신 실패 신호 — 운영자에게 알리세요 (콘솔 알람 A-PRC-009 cert/…/renew)";

    /// <summary>관측값을 배너에 반영 — 경고 구간이면 배너 하나를 유지(제목·본문·단계가 바뀌면 교체), 벗어나면(서버가 갱신되면) 내린다. 닫기 버튼은 없다.</summary>
    private void UpdateServerCertBanner()
    {
        var e = ServerCertExpiry;
        var cur = Notify.BannerOfKind(BannerKind.ServerCert);
        if (e is null || e.DaysLeft > ServerCertWarnDays)
        {
            if (cur is not null) { Notify.RemoveBanner(cur); Log.Info("server cert banner cleared" + (e is null ? "" : $" — {e.Remote} days_left={e.DaysLeft}")); }
            return;
        }
        ShowServerCertBanner(e);
    }

    /// <summary>경고 배너 표시(교체). --ui-preview-canvas 표본도 이 경로로 그린다.</summary>
    public void ShowServerCertBanner(TlsPeerExpiry e)
    {
        string title = ServerCertTitle(e), sub = ServerCertSubtitle(e); bool crit = e.DaysLeft <= ServerCertCriticalDays;
        var cur = Notify.BannerOfKind(BannerKind.ServerCert);
        if (cur is not null && cur.Title == title && cur.Subtitle == sub && cur.Critical == crit) return;
        if (cur is not null) Notify.RemoveBanner(cur);
        Notify.ShowBanner(new Banner { Kind = BannerKind.ServerCert, Title = title, Subtitle = sub, Critical = crit });
        Log.Warn($"server cert {(crit ? "CRITICAL" : "warning")}: {e.Remote} subject=\"{e.Subject}\" not_after={e.NotAfter:yyyy-MM-dd} days_left={e.DaysLeft}");
    }

    // ── 신원 (§3.2 상단 바) ──
    public DispatchProfile Dispatch => Profile?.Dispatch ?? DispatchProfile.None;
    public bool HasDesk => Dispatch.Present;
    public string DisplayName => Profile?.DisplayName ?? "";
    public string LoginId => Profile?.LoginId ?? "";
    /// <summary>전화 회선 — 유선 voip 우선, 없으면 이동 volte(SDK Profile.PhoneService). 이름은 전화 계열 계정(AccountKind.Volte)을 따른다.</summary>
    public ServiceProfile? VolteService => Profile?.PhoneService;
    public ServiceProfile? PttService => Profile?.Service("ptt");
    public string MyExtension => VolteService?.Msisdn ?? "";
    /// <summary>tel:+82… (MCPTT ID). 비면 tel:+msisdn.</summary>
    public string MyPttId => PttService is { } p ? (p.McpttId.Length > 0 ? p.McpttId : "tel:" + p.Msisdn) : "";
    public string MyPttNumber => UserPartConverter.UserPart(MyPttId);
    public string PilotId => Dispatch.PilotId;
    public string GroupName => Dispatch.GroupName.Length > 0 ? Dispatch.GroupName : Dispatch.GroupId;
    public string VolteDomain => VolteService?.Domain ?? "";
    public bool CanMonitorCalls => HasDesk && Dispatch.MonitorScope != "none";
    public bool CanListenPtt => HasDesk && Dispatch.PttListen != "none";
    public bool ListenHidden => Dispatch.ListenVisibility != "visible";
    public bool CanSms => Volte is not null;      // 외부망 게이트웨이 능력 키는 §13 — 지금은 등록 가입자 간만
    /// <summary>GMS 그룹 생성 자격(`ptt.allowCreateGroup`) — [새 그룹] 노출. 편집·삭제는 그룹별 IsOwner. 관리 범위(CanManageDirectory)가 있으면 그것으로도 생성.</summary>
    public bool CanCreateGroups => (Profile?.AllowGroupCreation == true || CanManageDirectory) && Ptt is not null;
    /// <summary>관리 범위(`dispatch.directoryAdmin` own|all) — 관리 창(§4.5)의 조직/구성원/번호·PTT 그룹 탭 활성.</summary>
    public bool CanManageDirectory => Profile?.Dispatch.CanAdminDirectory == true;
    private ManagementClient? _management;
    /// <summary>관리 평면 클라이언트(조직/구성원/번호·그룹 목록·이력 창 조회·녹취) — 로그인 전 null.</summary>
    public ManagementClient? Management => _csc is null || _tokens is null ? null : (_management ??= new ManagementClient(_csc, AccessTokenAsync, RenewAccessTokenAsync, Log));
    public string PttDomain => PttService?.Domain ?? "";

    partial void OnProfileChanged(Profile? value)
    {
        OnPropertyChanged(nameof(Dispatch)); OnPropertyChanged(nameof(HasDesk)); OnPropertyChanged(nameof(DisplayName));
        OnPropertyChanged(nameof(MyExtension)); OnPropertyChanged(nameof(MyPttId)); OnPropertyChanged(nameof(MyPttNumber));
        OnPropertyChanged(nameof(PilotId)); OnPropertyChanged(nameof(GroupName)); OnPropertyChanged(nameof(CanMonitorCalls));
        OnPropertyChanged(nameof(CanListenPtt)); OnPropertyChanged(nameof(ListenHidden)); OnPropertyChanged(nameof(CanCreateGroups));
        OnPropertyChanged(nameof(CanManageDirectory));
        OnPropertyChanged(nameof(PttDomain));
    }
    partial void OnPttChanged(Account? value) => OnPropertyChanged(nameof(CanCreateGroups));

    // ── 로그인·프로비저닝 (§6) ──
    private const string RefreshTokenKey = "csc.refresh";

    public bool HasSavedLogin => Settings.Current.AutoLogin && Credentials.Load(RefreshTokenKey) is { Length: > 0 };

    private CscClient MakeCsc(string host, int port)
    {
        _history?.Dispose(); _history = null;                    // 이전 CSC 클라이언트 위에서 돌던 폴링 정지
        _csc?.Dispose();
        var s = Settings.Current;
        _csc = new CscClient(new CscEndpoint
        {
            Host = host, Port = port, VerifyServer = s.CscVerifyServer,
            CaPem = ReadPem(s.TlsCaPemPath),
        });
        return _csc;
    }

    private static string? ReadPem(string path)
    {
        try { return path.Length > 0 && File.Exists(path) ? File.ReadAllText(path) : null; }
        catch (Exception) { return null; }
    }

    /// <summary>아이디·비밀번호 로그인(PKCE) → 프로파일. 성공 시 자동 로그인이면 refresh token 만 DPAPI 저장.</summary>
    public async Task<Result> LoginAsync(string host, int port, string loginId, string password, CancellationToken ct = default)
    {
        var csc = MakeCsc(host, port);
        var tok = await csc.LoginAsync(loginId, password, ct);
        if (!tok.Ok) return tok.WithoutValue();
        NoteTokens(tok.Value);
        _loginPw = password;
        Settings.Update(s => { s.CscHost = host; s.CscPort = port; s.LoginId = loginId; });
        if (Settings.Current.AutoLogin && tok.Value.RefreshToken.Length > 0) Credentials.Save(RefreshTokenKey, tok.Value.RefreshToken);
        else Credentials.Delete(RefreshTokenKey);
        return await FetchProfileAsync(ct);
    }

    /// <summary>저장 refresh token 으로 재로그인.</summary>
    public async Task<Result> ResumeAsync(CancellationToken ct = default)
    {
        string? rt = Credentials.Load(RefreshTokenKey);
        if (string.IsNullOrEmpty(rt)) return Result.Fail(-1, "저장된 로그인 없음");
        var s = Settings.Current;
        var csc = MakeCsc(s.CscHost, s.CscPort);
        var tok = await csc.RefreshAsync(rt, ct);
        if (!tok.Ok) { Credentials.Delete(RefreshTokenKey); return tok.WithoutValue(); }
        NoteTokens(tok.Value);
        return await FetchProfileAsync(ct);
    }

    // ── OAuth 토큰 수명 (§6 «세션 수명 — 자격은 둘이지만 로그인은 하나다») ──
    //   SIP 자격(H(A1))은 재등록으로 사실상 안 죽지만 MC 서비스 인가(OAuth2)는 1시간에 만료된다. 관제사에게
    //   «통화는 되는데 조회만 안 되는» 반쯤 로그인된 상태를 보이지 않으려면 앱이 조용히 갱신해야 한다.
    //   토큰을 쓰는 곳은 전부 AccessTokenAsync 하나를 지난다 — 직접 _tokens.AccessToken 을 읽지 않는다.

    /// <summary>만료 몇 초 전부터 선제 갱신하나 — 느린 회선에서도 요청이 옛 토큰으로 나가지 않을 만큼.</summary>
    private const int TokenRenewMarginSec = 60;
    private DateTime _tokenExpiresAtUtc = DateTime.MinValue;
    private readonly SemaphoreSlim _tokenLock = new(1, 1);
    /// <summary>틱이 토큰을 들여다보는 주기(초) — 만료는 1시간이라 자주 볼 이유가 없다. 실제 갱신 판정은 AccessTokenAsync 가 한다.</summary>
    private const int TokenCheckSec = 30;
    private DateTime _nextTokenCheck = DateTime.MinValue;
    /// <summary>생성 시점(UI 스레드)의 컨텍스트 — 갱신은 이력 폴링 스레드에서도 일어나므로 배너·로그아웃은 여기로 올린다.</summary>
    private readonly SynchronizationContext? _ui = SynchronizationContext.Current;
    private void OnUi(Action a) { if (_ui is null || _ui == SynchronizationContext.Current) a(); else _ui.Post(_ => a(), null); }

    /// <summary>갱신이 실패하는 중(네트워크·5xx) — 옛 토큰으로 버티는 동안 미리 알린다. 조회를 누른 그 순간에 튕기지 않게.</summary>
    [ObservableProperty] private bool _credentialWarning;

    private void NoteTokens(TokenSet t)
    {
        _tokens = t;
        _endingSession = false;                          // Logout() 에서 풀면 EndSession 핸들러 안에서 바로 풀려 가드가 무력해진다
        _tokenExpiresAtUtc = t.ExpiresInSec > 0 ? DateTime.UtcNow.AddSeconds(t.ExpiresInSec) : DateTime.MinValue;
        if (Settings.Current.AutoLogin && t.RefreshToken.Length > 0) Credentials.Save(RefreshTokenKey, t.RefreshToken);
        SetCredentialWarning(false);
    }

    /// <summary>유효한 access token. 만료가 가까우면 먼저 갱신한다. 로그인 전이면 null.</summary>
    public async Task<string?> AccessTokenAsync(CancellationToken ct = default)
    {
        var cur = _tokens;
        if (_csc is null || cur is null) return null;
        if (_tokenExpiresAtUtc == DateTime.MinValue || DateTime.UtcNow < _tokenExpiresAtUtc.AddSeconds(-TokenRenewMarginSec))
            return cur.AccessToken;
        return await RenewLockedAsync(null, ct);
    }

    /// <summary>401 을 받은 뒤의 강제 갱신 — <paramref name="stale"/> 은 그 401 을 받은 토큰.
    /// 잠금을 기다리는 사이 다른 호출이 이미 갱신했으면 갱신을 또 하지 않고 새 토큰을 준다.</summary>
    public Task<string?> RenewAccessTokenAsync(string? stale, CancellationToken ct = default)
        => _csc is null || _tokens is null ? Task.FromResult<string?>(null) : RenewLockedAsync(stale ?? "", ct);

    private async Task<string?> RenewLockedAsync(string? stale, CancellationToken ct)
    {
        await _tokenLock.WaitAsync(ct);
        try
        {
            var csc = _csc; var cur = _tokens;
            if (csc is null || cur is null) return null;
            if (stale is null)
            {
                if (DateTime.UtcNow < _tokenExpiresAtUtc.AddSeconds(-TokenRenewMarginSec)) return cur.AccessToken;
            }
            else if (!string.Equals(cur.AccessToken, stale, StringComparison.Ordinal))
            {
                return cur.AccessToken;                       // 남이 이미 갱신했다
            }

            var r = await csc.RefreshAsync(cur.RefreshToken, ct);
            if (r.Ok) { NoteTokens(r.Value); Log.Info("csc token refreshed"); return r.Value.AccessToken; }

            if (IsSessionEnded(r.Code, r.Reason))
            {
                Log.Warn($"csc session ended: {r.Code} {r.Reason}");
                OnUi(() => EndSession("로그인이 만료되었습니다 — 다시 로그인하세요"));
                return null;
            }
            // 일시 실패 — 옛 토큰을 유지한 채 경고만 띄운다(다음 요청·다음 틱이 다시 시도한다).
            Log.Warn($"csc token refresh failed (일시): {r.Code} {r.Reason}");
            SetCredentialWarning(true);
            return cur.AccessToken;
        }
        finally { _tokenLock.Release(); }
    }

    /// <summary>되살릴 수 없는 실패인가 — refresh 폐기·회전 실패·만료(RFC 6749 §5.2 invalid_grant)와 401.
    /// 네트워크·5xx 는 여기 들지 않는다(일시 실패로 다뤄 옛 토큰을 유지한다).</summary>
    internal static bool IsSessionEnded(int code, string? reason)
    {
        if (code == 401) return true;
        string r = reason ?? "";
        return code == 400 && (r.Contains("invalid_grant", StringComparison.OrdinalIgnoreCase)
                            || r.Contains("invalid_token", StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>자격이 끝났다 — 실제 정리(등록 해제·창 전환)는 App 이 한다. 여기서는 사유와 함께 알리기만 한다:
    /// Logout() 만 불러서는 틱 정지·창 숨김·로그인 창 복귀가 빠져 반쯤 로그아웃된 상태가 된다.</summary>
    public void EndSession(string why)
    {
        if (_endingSession) return;                      // 여러 요청이 동시에 401 을 받아도 한 번만
        _endingSession = true;
        CredentialsEnded?.Invoke(this, why);
    }
    private bool _endingSession;

    /// <summary>자격 만료로 세션이 끝났다 — 셸이 로그인 창을 띄운다.</summary>
    public event EventHandler<string>? CredentialsEnded;

    private void SetCredentialWarning(bool on) => OnUi(() => ApplyCredentialWarning(on));

    private void ApplyCredentialWarning(bool on)
    {
        if (CredentialWarning == on) return;
        CredentialWarning = on;
        var cur = Notify.BannerOfKind(BannerKind.Credential);
        if (!on) { if (cur is not null) Notify.RemoveBanner(cur); return; }
        if (cur is not null) return;
        Notify.ShowBanner(new Banner
        {
            Kind = BannerKind.Credential,
            Title = "서버 자격 갱신 실패",
            Subtitle = "이력·관리·PTT 그룹 조회가 곧 막힐 수 있습니다 — 통화는 계속됩니다. 서버 연결을 확인하세요",
        });
    }

    private async Task<Result> FetchProfileAsync(CancellationToken ct)
    {
        if (_csc is null || _tokens is null) return Result.Fail(-1, "로그인 전");
        if (await AccessTokenAsync(ct) is not { } tk) return Result.Fail(-1, "로그인 전");
        var p = await _csc.FetchProfileAsync(tk, ct);
        if (!p.Ok) return p.WithoutValue();
        Profile = p.Value;
        Directory.CountryCode = p.Value.CountryCode;
        Directory.SetMembers(p.Value.Dispatch.Members, p.Value.Dispatch.GroupId);   // 서버 감시 대상·그룹원 목록(없으면 CSV member 폴백)
        Log.Info($"profile {p.Value.LoginId} services={string.Join(",", p.Value.Services.Select(s => s.Kind))} desk={p.Value.Dispatch.Present} " +
                 $"group={p.Value.Dispatch.GroupId} pilot={p.Value.Dispatch.PilotId} monitor={p.Value.Dispatch.MonitorScope} pttListen={p.Value.Dispatch.PttListen} " +
                 $"members={p.Value.Dispatch.Members.Count} pttTargets={p.Value.Dispatch.PttTargets.Count} groupCreate={p.Value.AllowGroupCreation} cc={p.Value.CountryCode}");
        return Result.Success;
    }

    /// <summary>회사 전화번호부 동기화 — `/provisioning/directory?service=volte|ptt`, ETag 로 304 면 다운로드 생략(android_ue_provisioning.md §3-1).
    /// `service=volte` 는 **전화 가족 축**(이동 volte + 유선 voip 를 서버가 합산) — 앱은 회선 종류를 나눠 요청하지 않는다(그룹원 `volteAor` 와 같은 어휘).</summary>
    public async Task SyncDirectoryAsync(CancellationToken ct = default)
    {
        if (_csc is null || _tokens is null) return;
        var csc = _csc; if (await AccessTokenAsync() is not { } token) return;
        foreach (string service in new[] { "volte", "ptt" })
        {
            if (service == "ptt" && Ptt is null && PttService is null) continue;
            string? etag = Directory.Etag(service);
            var r = await Task.Run(() => csc.XcapGet(token, $"/provisioning/directory?service={service}", "application/json", etag), ct);
            if (!r.Ok) { Log.Warn($"directory {service}: {r}"); if (Directory.Etag(service) is null) Notify.Warn($"전화번호부({service}) 동기화 실패", r.ToString()); continue; }
            if (r.Value.NotModified) { Directory.TouchServer(); Log.Info($"directory {service}: not modified"); continue; }
            if (Directory.ApplyServer(service, r.Value.Body, r.Value.ETag)) Log.Info($"directory {service}: {Directory.Contacts.Count(c => c.IsServer)} entries etag={r.Value.ETag}");
            else Log.Warn($"directory {service}: bad json");
        }
    }

    /// <summary>엔진 기동 → 계정 추가·등록 → 관제 범위 적용 → 그룹 목록·affiliation·로스터 구독.</summary>
    public async Task<Result> StartAsync()
    {
        if (Profile is null) return Result.Fail(-1, "프로파일 없음");
        var s = Settings.Current;
        if (!Engine.IsRunning)
        {
            var r = Engine.Start(new EngineConfig
            {
                UserAgent = "CIMS-Dispatch/0.1", LogLevel = s.LogLevel, TlsCaPem = ReadPem(s.TlsCaPemPath), TlsVerifyServer = s.CscVerifyServer,
            });
            if (!r.Ok) return r;
        }
        ApplyAudioSettings();

        // 계정으로 올리는 서비스 = PTT 전부 + 전화 계열은 SDK 가 고른 하나(Profile.PhoneService — 유선 voip 우선, 없으면 이동 volte).
        //   관제사가 volte·voip 를 둘 다 가졌을 때 둘 다 등록하면 이동 번호까지 관제석에 바인딩되어 착신이 이 앱으로 포크되고,
        //   전화 계정 참조(Volte)·등록 상태(VolteReg)를 마지막 계정이 덮어쓴다 — 전화 계열은 한 계정만 올린다.
        var toRegister = Profile.Services.Where(s => s.Kind == "ptt").ToList();
        if (Profile.PhoneService is { } phone) toRegister.Insert(0, phone);
        foreach (var sp in toRegister)
        {
            var cfg = sp.ToAccountConfig(_loginPw.Length > 0 ? _loginPw : null);
            cfg.DisplayName = Profile.DisplayName;
            cfg.AutoAnswerMcptt = sp.Kind == "ptt";           // 그룹콜 자동 수락(사설콜 분리는 §13 코어 과제)
            var a = Engine.AddAccount(cfg);
            if (!a.Ok) { Log.Warn($"addAccount {sp.Kind}: {a}"); Notify.Error($"{sp.Kind.ToUpperInvariant()} 계정 추가 실패", a.ToString()); continue; }
            var kind = sp.Kind == "ptt" ? AccountKind.Ptt : AccountKind.Volte;
            _accountKinds[a.Value.Id] = kind;
            if (kind == AccountKind.Ptt) Ptt = a.Value; else Volte = a.Value;
            var reg = a.Value.Register();
            if (!reg.Ok) Notify.Error($"{sp.Kind.ToUpperInvariant()} 등록 요청 실패", reg.ToString());
        }
        OnPropertyChanged(nameof(CanSms));
        await SyncDirectoryAsync();

        // 관제 범위: 대표번호·감시 대상 전원 dialog 구독 (§4.3) — 대상 = 프로비저닝 members[](서버가 monitorScope 해석, 정본) 또는 CSV member 폴백
        if (HasDesk && Volte is not null) WatchAll();
        _nextDispatchPoll = DateTime.Now.AddSeconds(DispatchPollSec);
        // 멤버 그룹(GMS 목록 → affiliation + conference 구독)·청취 범위 그룹(pttTargets → conference 구독) (§4.1·§4.2)
        if (Ptt is not null)
        {
            await RefreshGroupsAsync();
            // 서버발 그룹 변경(GROUP_CHANGED → xcap-diff NOTIFY, RFC 5875) 을 받아 목록을 자동 재조회
            if (PttDomain.Length > 0)
            {
                var x = Ptt.SubscribeXcapDiff($"sip:gms_psi@{PttDomain}", true);
                if (!x.Ok) Log.Warn($"xcap-diff subscribe: {x}");
            }
        }
        IsReady = true;
        _nextServerCertCheck = DateTime.Now.AddSeconds(ServerCertCheckSec);
        UpdateServerCertBanner();                                // 로그인(HTTPS)·등록(TLS) 핸드셰이크 직후 — 배너는 관제 캔버스에서도 보인다
        ProfileApplied?.Invoke(this, EventArgs.Empty);
        _ = StartHistoryAsync();                                 // 서버 통합 이력(P3b) — 없으면 탐침에서 조용히 꺼진다
        return Result.Success;
    }

    // ── 서버 통합 이력 폴링 (P3b — `/provisioning/history`, dispatch_desktop_ui.md §13) ──
    private HistoryClient? _history;

    private async Task StartHistoryAsync()
    {
        if (_csc is null || _tokens is null || !HasDesk) return;
        _history?.Dispose();
        _history = new HistoryClient(_csc, AccessTokenAsync, RenewAccessTokenAsync, Log);
        _history.Received += (_, e) => OnHistory(e);
        if (await _history.ProbeAsync() == true)
            _history.Start(new[] { HistoryKind.Call, HistoryKind.Ptt, HistoryKind.Message });
    }

    /// <summary>이력 항목 → ②④ 내역 행. 내가 당사자인 항목은 이미 로컬 행이 있으니 건너뛴다 — 서버 이력의 몫은 관제 범위 안 타인의 통화·세션·메시지.
    /// 대표번호 호(부재·동료 응답·내 응답)도 로컬(대표번호 dialog, RecordPilotOutcome)이 즉시 기록하고 이력엔 응답자 필드가 없어 구분이 안 되므로 건너뛴다.</summary>
    private void OnHistory(HistoryEntry e)
    {
        if (IsMe(e.From) || IsMe(e.To)) return;
        bool pilot = (e.To.Length > 0 && IsPilotUri(e.To)) || (e.From.Length > 0 && IsPilotUri(e.From));
        if (pilot && e.Kind == HistoryKind.Call) return;                       // 대표번호 호 결과는 dialog 가 정본(RecordPilotOutcome)
        string from = Label(e.From);
        string to = pilot && IsPilotUri(e.To) ? $"대표 {Directory.DisplayNumber(UserPartConverter.UserPart(e.To))}" : Label(e.To);
        string dur = e.DurationSec > 0 ? $"{e.DurationSec / 60}:{e.DurationSec % 60:00}" : "";
        switch (e.Kind)
        {
            case HistoryKind.Call:
            {
                var kind = e.Event switch
                {
                    "call.answered" or "call.ended" => ActivityKind.Incoming, "call.missed" or "call.noanswer" => ActivityKind.Missed,
                    "call.transferred" => ActivityKind.Transfer, "call.pickup" => ActivityKind.Pickup, _ => ActivityKind.Note,
                };
                Activity.Add(new ActivityRow(e.Time, ActivityPanel.Call, kind, $"{from} → {to}", dur.Length > 0 ? dur : HistoryEventText(e.Event),
                                             e.Emergency, kind == ActivityKind.Missed, UserPartConverter.UserPart(e.From), IsPilot: pilot, IsOthers: true));
                break;
            }
            case HistoryKind.Ptt:
            {
                string group = Groups.FirstOrDefault(g => string.Equals(g.Uri, e.Group, StringComparison.OrdinalIgnoreCase))?.Name ?? UserPartConverter.UserPart(e.Group);
                var kind = e.Event switch
                {
                    "ptt.talk" => ActivityKind.Talk, "ptt.session.start" => ActivityKind.SessionStart, "ptt.session.end" => ActivityKind.SessionEnd,
                    "ptt.emergency" => ActivityKind.Emergency, "ptt.private" => ActivityKind.Private, "ptt.adhoc" => ActivityKind.Adhoc, _ => ActivityKind.Note,
                };
                Activity.Add(new ActivityRow(e.Time, ActivityPanel.Ptt, kind, $"{group} · {from}", dur.Length > 0 ? dur : HistoryEventText(e.Event), e.Emergency, false, UserPartConverter.UserPart(e.From), IsOthers: true));
                break;
            }
            case HistoryKind.Message:
            {
                bool sms = e.Event.StartsWith("message.sms", StringComparison.Ordinal);
                string target = e.Group.Length > 0
                    ? Groups.FirstOrDefault(g => string.Equals(g.Uri, e.Group, StringComparison.OrdinalIgnoreCase))?.Name ?? UserPartConverter.UserPart(e.Group) : to;
                string text = e.Text.Length > 60 ? e.Text[..60] + "…" : e.Text;
                Activity.Add(new ActivityRow(e.Time, sms ? ActivityPanel.Call : ActivityPanel.Ptt, sms ? ActivityKind.Sms : ActivityKind.Sds,
                                             $"{from} → {target}", text, e.Emergency, false, UserPartConverter.UserPart(e.From), IsOthers: true));
                break;
            }
        }
    }

    private string Label(string uri) => Directory.NameOf(UserPartConverter.UserPart(uri)) is { Length: > 0 } n ? n : Directory.DisplayNumber(UserPartConverter.UserPart(uri));

    /// <summary>이력 event 이름표(android_ue_provisioning.md §3-2) → 상세 문구. 모르는 이름은 그대로.</summary>
    private static string HistoryEventText(string ev) => ev switch
    {
        "call.answered" => "응답", "call.ended" => "종료", "call.missed" => "부재", "call.noanswer" => "무응답", "call.transferred" => "전달", "call.pickup" => "당겨받기",
        "ptt.talk" => "발언", "ptt.session.start" => "세션 시작", "ptt.session.end" => "세션 종료", "ptt.emergency" => "긴급", "ptt.private" => "사설콜", "ptt.adhoc" => "애드혹",
        _ => ev,
    };

    private bool IsPilotUri(string uri) => PilotId.Length > 0 && string.Equals(UserPartConverter.UserPart(uri), UserPartConverter.UserPart(PilotId), StringComparison.Ordinal);

    // ── PTT 그룹 목록·관리 (GMS, TS 24.481 — 서버 요청서 §1) ──
    private int _groupRefreshSeq;

    /// <summary>GMS 목록 재조회 → Groups 를 차분 갱신: 새 멤버 그룹은 affiliation+conference 구독, 사라진 그룹은 해제.
    /// 청취 범위 그룹(pttTargets, 비멤버)은 conference 구독만. Groups 의 CollectionChanged 로 주소록·채널 카드·설정이 따라온다.</summary>
    public async Task<Result> RefreshGroupsAsync(CancellationToken ct = default)
    {
        if (Ptt is null || _csc is null || _tokens is null) return Result.Fail(-1, "PTT 계정 없음");
        var csc = _csc; var ptt = Ptt;
        if (await AccessTokenAsync() is not { } token) return Result.Fail(-1, "로그인 전");
        var groups = await csc.ListGroupsAsync(token, MyPttId, ct);
        if (!groups.Ok) { Notify.Warn("그룹 목록을 받지 못했습니다", groups.ToString()); return groups.WithoutValue(); }
        if (Ptt != ptt) return Result.Fail(-1, "세션 종료");                 // 조회 중 로그아웃
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var g in groups.Value)
        {
            string id = UserPartConverter.UserPart(g.Uri);
            if (id.Length == 0 || !seen.Add(id)) continue;
            string name = g.DisplayName.Length > 0 ? g.DisplayName : id;
            var gi = Groups.FirstOrDefault(x => x.Id == id);
            if (gi is not null && !gi.IsMember) { Groups.Remove(gi); gi = null; }   // 청취 범위 → 멤버 승격
            if (gi is null)
            {
                gi = new GroupInfo(id, g.Uri, name, g.MemberCount) { IsOwner = g.IsOwner, Etag = g.ETag };
                Groups.Add(gi);
                gi.Affiliated = ptt.Affiliate(id, true).Ok;
                var sc = ptt.SubscribeConference(id, true);
                if (!sc.Ok) Log.Warn($"conference subscribe {id}: {sc}");
            }
            else { gi.Name = name; gi.MemberCount = g.MemberCount; gi.IsOwner = g.IsOwner; gi.Etag = g.ETag; }
        }
        var goneIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var gone in Groups.Where(x => x.IsMember && !seen.Contains(x.Id)).ToList())
        {
            ptt.Affiliate(gone.Id, false);
            ptt.SubscribeConference(gone.Id, false);
            Groups.Remove(gone);
            goneIds.Add(gone.Id);
            Log.Info($"group gone {gone.Id}");
            // 서버 pttTargets(ptt_listen=all 은 전 그룹)가 아직 이 그룹을 들고 있을 수 있다 — 다음 틱에 /provisioning/me 재조회를 당겨
            // 삭제된 그룹을 청취 범위로 다시 구독하는 창을 없앤다(정상 주기는 60초).
            if (HasDesk) _nextDispatchPoll = DateTime.Now;
        }
        // 청취 범위 그룹 — 프로비저닝 pttTargets(서버가 ptt_listen 범위를 해석한 목록). 로스터 NOTIFY 로 진행/참가자 수를 안다.
        // 범위 밖·자격 없음은 서버가 403 + Warning 138(브로드캐스트 480 + 105)로 거절한다 — 구독은 여기서 1회, 재시도 루프 없음.
        var listenIds = CanListenPtt ? Dispatch.PttTargets.Select(t => t.Id).Where(id => id.Length > 0).ToHashSet(StringComparer.OrdinalIgnoreCase)
                                     : new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var t in Dispatch.PttTargets)
        {
            if (!listenIds.Contains(t.Id) || seen.Contains(t.Id) || goneIds.Contains(t.Id) || Groups.Any(x => x.Id == t.Id)) continue;
            string uri = t.Uri.Length > 0 ? t.Uri : $"tel:{t.Id}";
            Groups.Add(new GroupInfo(t.Id, uri, t.Name.Length > 0 ? t.Name : t.Id, 0) { IsMember = false });
            var sc = ptt.SubscribeConference(t.Id, true);
            if (!sc.Ok) Log.Warn($"conference subscribe(listen scope) {t.Id}: {sc}");
        }
        foreach (var gone in Groups.Where(x => !x.IsMember && !listenIds.Contains(x.Id)).ToList())   // 범위 재조회로 빠진 청취 그룹
        {
            ptt.SubscribeConference(gone.Id, false);
            Groups.Remove(gone);
            Log.Info($"listen-scope group gone {gone.Id}");
        }
        Directory.SetGroups(Groups.Where(x => x.IsMember));
        Log.Info($"groups {Groups.Count(x => x.IsMember)} member ({Groups.Count(x => x.IsOwner)} owned), {Groups.Count(x => !x.IsMember)} listen-scope");
        return Result.Success;
    }

    // ── 발견 목록 주기 재조회 (android_ue_provisioning.md §3 — `/provisioning/me` If-None-Match → 304 면 무변경) ──
    public const int DispatchPollSec = 60;
    private string _profileEtag = "";
    private DateTime _nextDispatchPoll = DateTime.MaxValue;

    /// <summary>관제 편성 변경(그룹원·청취 대상) 추적 — 304 면 끝, 200 이면 두 목록을 비교해 바뀐 것만 dialog watch·conference 구독에 재적용.</summary>
    private async Task RefreshDispatchAsync()
    {
        if (_csc is null || _tokens is null || Profile is null) return;
        var csc = _csc; if (await AccessTokenAsync() is not { } token) return;
        var r = await Task.Run(() => csc.XcapGet(token, "/provisioning/me", "application/json", _profileEtag.Length > 0 ? _profileEtag : null));
        if (!r.Ok) { Log.Warn($"provisioning/me poll: {r}"); return; }
        if (r.Value.NotModified || Profile is null) return;
        _profileEtag = r.Value.ETag;
        var p = CscClient.ParseProfile(r.Value.Body);
        if (!p.Ok) { Log.Warn($"provisioning/me poll: bad profile — {p.Reason}"); return; }
        var oldD = Profile.Dispatch; var newD = p.Value.Dispatch;
        bool sameMembers = oldD.Members.Select(m => $"{m.VolteAor}|{m.GroupId}|{m.Name}").ToHashSet().SetEquals(newD.Members.Select(m => $"{m.VolteAor}|{m.GroupId}|{m.Name}"));
        bool sameTargets = oldD.PttTargets.Select(t => t.Id).ToHashSet().SetEquals(newD.PttTargets.Select(t => t.Id));
        bool sameScope = oldD.MonitorScope == newD.MonitorScope && oldD.PttListen == newD.PttListen && oldD.GroupId == newD.GroupId && oldD.PilotId == newD.PilotId;
        if (sameMembers && sameTargets && sameScope) return;
        Log.Info($"dispatch discovery changed: members {oldD.Members.Count}→{newD.Members.Count} pttTargets {oldD.PttTargets.Count}→{newD.PttTargets.Count} scope {newD.MonitorScope}/{newD.PttListen}");
        Profile = p.Value;
        Directory.SetMembers(newD.Members, newD.GroupId);
        if (Volte is not null && HasDesk)
        {
            var want = Directory.WatchTargets.Select(m => ToSipUri(m.Number)).ToHashSet(StringComparer.OrdinalIgnoreCase);
            if (PilotId.Length > 0) want.Add(ToSipUri(PilotId));
            foreach (var aor in _watched.Where(a => !want.Contains(a)).ToList())
            {
                Volte.DialogWatch(aor, false); _watched.Remove(aor);
                foreach (var row in Dialogs.Where(d => string.Equals(d.Watched, aor, StringComparison.OrdinalIgnoreCase)).ToList()) { Dialogs.Remove(row); DialogEnded?.Invoke(this, row); }
            }
            WatchAll();
        }
        if (Ptt is not null) await RefreshGroupsAsync();
        Notify.Info("관제 편성이 바뀌었습니다", $"그룹원 {Directory.Members.Count} · 감시 {Directory.WatchTargets.Count} · 청취 그룹 {Groups.Count(g => !g.IsMember)}");
    }

    /// <summary>xcap-diff NOTIFY(gms 축) → 그룹 목록 재조회(연속 통지는 0.5초 합침).</summary>
    private void OnSipMessage(SipMessage m)
    {
        if (!m.ContentType.Contains("xcap-diff", StringComparison.OrdinalIgnoreCase)) return;
        if (!m.Body.Contains("org.openmobilealliance.groups", StringComparison.Ordinal)) return;
        int seq = ++_groupRefreshSeq;
        Log.Info("xcap-diff: group document changed — refreshing");
        _ = Task.Delay(500).ContinueWith(_ => { if (seq == _groupRefreshSeq && Ptt is not null) _ = RefreshGroupsAsync(); },
                                         TaskScheduler.FromCurrentSynchronizationContext());
    }

    /// <summary>새 그룹 uri — XCAP 은 클라이언트가 문서를 명명한다. 정규형 `tel:g-&lt;소문자 hex 8&gt;`(mcptt_api.md §2; `adhoc-`/`priv-` 예약).</summary>
    public string NewGroupUri() => "tel:g-" + Guid.NewGuid().ToString("N")[..8];

    public async Task<Result<GroupDoc>> GetGroupAsync(GroupInfo g, CancellationToken ct = default)
    {
        if (_csc is null || _tokens is null) return Result<GroupDoc>.Fail(-1, "로그인 전");
        if (await AccessTokenAsync(ct) is not { } tk) return Result<GroupDoc>.Fail(-1, "로그인 전");
        return await _csc.GetGroupAsync(tk, MyPttId, g.Uri, ct);
    }

    /// <summary>그룹 생성/수정(PUT). ifMatch = 편집 시작 시 ETag(충돌 412). 신규 id 충돌(409 `uri_taken`)은 id 를 다시 만들어 한 번만
    /// 조용히 재시도한다 — 성공 문서의 uri 가 정본이니 호출자는 r.Value.Uri 를 쓴다. 성공 시 목록 재조회.</summary>
    public async Task<Result<GroupDoc>> SaveGroupAsync(GroupDoc doc, string? ifMatch, CancellationToken ct = default)
    {
        if (_csc is null || _tokens is null) return Result<GroupDoc>.Fail(-1, "로그인 전");
        if (await AccessTokenAsync(ct) is not { } tk) return Result<GroupDoc>.Fail(-1, "로그인 전");
        bool isNew = ifMatch is null || ifMatch.Length == 0;
        var r = await _csc.PutGroupAsync(tk, MyPttId, doc, ifMatch, ct);
        if (!r.Ok && isNew && r.Code == 409 && ResponseText.GroupError(r.Reason).Error == "uri_taken")
        {
            // 클라이언트 명명 id 가 타인 소유와 충돌(mcptt_api.md §2) — 첫 409 는 사용자에게 보이지 않는다
            Log.Info($"putGroup {doc.Uri}: uri_taken — id 재생성 후 재시도");
            doc.Uri = NewGroupUri();
            r = await _csc.PutGroupAsync(tk, MyPttId, doc, null, ct);
        }
        if (!r.Ok) { Notify.Error(ResponseText.Describe(ResponseText.Area.Group, r.Code, r.Reason), r.ToString()); return r; }
        Activity.Add(ActivityPanel.Ptt, ActivityKind.Note, $"그룹 {(isNew ? "생성" : "편집")} {r.Value.DisplayName}", $"멤버 {r.Value.Members.Count}");
        Notify.Info($"그룹 {(isNew ? "생성" : "편집")} 완료", r.Value.DisplayName);
        await RefreshGroupsAsync(ct);
        return r;
    }

    public async Task<Result> DeleteGroupAsync(GroupInfo g, CancellationToken ct = default)
    {
        if (_csc is null || _tokens is null) return Result.Fail(-1, "로그인 전");
        if (await AccessTokenAsync(ct) is not { } tk) return Result.Fail(-1, "로그인 전");
        var r = await _csc.DeleteGroupAsync(tk, MyPttId, g.Uri, ct);
        if (!r.Ok)
        {
            Notify.Error(ResponseText.Describe(ResponseText.Area.Group, r.Code, r.Reason), r.ToString());
            if (r.Code == 404) await RefreshGroupsAsync(ct);                    // 이미 없어진 그룹 — 목록만 맞춘다
            return r;
        }
        Activity.Add(ActivityPanel.Ptt, ActivityKind.Note, $"그룹 삭제 {g.Name}");
        Notify.Info("그룹 삭제 완료", g.Name);
        await RefreshGroupsAsync(ct);
        return r;
    }

    /// <summary>대표번호를 먼저(대기열은 이 구독 하나에 달렸다), 그다음 감시 대상 전원. 구독 슬롯이 모자라면 앞쪽이 산다 —
    /// 실패는 줄마다 로그하되 관제사에게는 한 번만 알린다(감시 누락은 조용히 넘기면 안 되는 상태).</summary>
    private void WatchAll()
    {
        if (Volte is null) return;
        int failed = 0; string first = "";
        if (PilotId.Length > 0 && !Watch(PilotId)) { failed++; first = PilotId; }
        foreach (var m in Directory.WatchTargets)
            if (!Watch(m.Number)) { if (failed++ == 0) first = m.Number; }
        if (failed > 0) Notify.Warn($"회선 감시 구독 실패 {failed}건", $"{first} 등 — 그룹원 상태·대기열이 빠질 수 있습니다 (로그 dialogWatch 참조)");
    }

    /// <summary>이미 구독 중이면 true(중복 요청 없음), 새 구독은 결과대로.</summary>
    private bool Watch(string numberOrAor)
    {
        if (Volte is null) return false;
        string aor = ToSipUri(numberOrAor);
        if (!_watched.Add(aor)) return true;
        var r = Volte.DialogWatch(aor, true);
        if (!r.Ok) { Log.Warn($"dialogWatch {aor}: {r}"); _watched.Remove(aor); return false; }
        return true;
    }

    public string ToSipUri(string numberOrUri)
    {
        if (numberOrUri.StartsWith("sip:", StringComparison.OrdinalIgnoreCase) || numberOrUri.StartsWith("tel:", StringComparison.OrdinalIgnoreCase)) return numberOrUri;
        string n = new string(numberOrUri.Where(c => char.IsLetterOrDigit(c) || c is '+' or '*' or '#').ToArray());
        return VolteDomain.Length > 0 ? $"sip:{n}@{VolteDomain}" : n;
    }

    public string ToTelUri(string number)
    {
        if (number.StartsWith("tel:", StringComparison.OrdinalIgnoreCase)) return number;
        return "tel:" + UserPartConverter.UserPart(number);
    }

    /// <summary>등록 해제 → 토큰 폐기 → 초기 상태(로그인 창으로).</summary>
    public void Logout()
    {
        foreach (var s in Sessions.ToList()) Engine.GetCall(s.CallId).Hangup();
        _history?.Dispose(); _history = null;
        Engine.Stop();
        Credentials.Delete(RefreshTokenKey);
        _tokens = null; _tokenExpiresAtUtc = DateTime.MinValue; _nextTokenCheck = DateTime.MinValue; _loginPw = ""; _management = null;
        if (Notify.BannerOfKind(BannerKind.Credential) is { } crb) Notify.RemoveBanner(crb);
        CredentialWarning = false;
        Volte = null; Ptt = null; Profile = null;
        _accountKinds.Clear(); _watched.Clear(); _pendingOps.Clear(); _pendingConsult.Clear(); _adhocMembers.Clear(); _regRetryAt.Clear(); _regBackoff.Clear();
        Sessions.Clear(); Groups.Clear(); Dialogs.Clear();
        VolteReg = RegInfo.Empty; PttReg = RegInfo.Empty;
        _nextDispatchPoll = DateTime.MaxValue; _profileEtag = "";
        if (Notify.BannerOfKind(BannerKind.ServerCert) is { } cb) Notify.RemoveBanner(cb);   // 다음 로그인의 핸드셰이크가 다시 판정한다
        _nextServerCertCheck = DateTime.MinValue;
        IsReady = false;
    }

    // ── 오디오 (§7) ──
    public void ApplyAudioSettings()
    {
        if (!Engine.IsRunning) return;
        var s = Settings.Current;
        Engine.RefreshAudioDevices();
        var devs = Engine.AudioDevices;
        var cap = AudioEndpoints.MatchEngineDevice(devs, s.CaptureDevice, AudioFlow.Capture);
        var head = AudioEndpoints.MatchEngineDevice(devs, s.HeadsetDevice, AudioFlow.Render);
        var r = Engine.SetAudioDevices(cap?.Id ?? -1, head?.Id ?? -1);
        if (!r.Ok) Notify.Warn("오디오 장치 설정 실패", r.ToString());
        CaptureName = cap?.Name ?? DefaultEndpointName(AudioFlow.Capture);
        HeadsetName = head?.Name ?? DefaultEndpointName(AudioFlow.Render);

        if (Audio.SpeakerRoute > 0) { Engine.RemovePlaybackRoute(Audio.SpeakerRoute); Audio.SpeakerRoute = 0; }
        SpeakerName = "";
        if (s.SpeakerRouteEnabled && s.SpeakerDevice.Length > 0)
        {
            var spk = AudioEndpoints.MatchEngineDevice(devs, s.SpeakerDevice, AudioFlow.Render);
            if (spk is not null)
            {
                var ar = Engine.AddPlaybackRoute(spk.Id);
                if (ar.Ok) { Audio.SpeakerRoute = ar.Value; SpeakerName = spk.Name; }
                else Notify.Warn("데스크 스피커 라우트 실패", ar.ToString());
            }
            else Notify.Warn("데스크 스피커를 찾지 못했습니다 — 헤드셋으로 출력", s.SpeakerDevice);
        }
        AudioChanged?.Invoke(this, EventArgs.Empty);
    }

    private string DefaultEndpointName(AudioFlow flow)
    {
        try { return Endpoints.Default(flow)?.Name ?? "기본 장치"; } catch (Exception) { return "기본 장치"; }
    }

    private void OnEndpointsChanged()
    {
        if (!Engine.IsRunning) return;
        Log.Info("audio endpoints changed");
        ApplyAudioSettings();                   // 선택 장치가 사라졌으면 기본 장치 폴백, 다시 붙으면 복귀(설정 이름 기준)
        Notify.Info("오디오 장치 변경 감지 — 장치를 다시 적용했습니다");
    }

    // ── 등록 ──
    private void OnRegistration(RegInfo r)
    {
        // 로그아웃 뒤 늦게 오는 un-REGISTER 실패 등 — 계정 표가 비어 있으면 이 세션의 것이 아니다(로그인 창에 토스트가 뜨지 않게)
        if (!_accountKinds.ContainsKey(r.AccountId)) { Log.Info($"reg late acc={r.AccountId} {r.State} {r.Code} — ignored"); return; }
        var kind = KindOf(r.AccountId);
        if (kind == AccountKind.Ptt) PttReg = r; else VolteReg = r;
        string name = kind == AccountKind.Ptt ? "PTT" : "VoLTE";
        Log.Info($"reg {name} {r.State} {r.Code} {r.Reason}");
        if (r.State == RegState.Registered)
        {
            _regRetryAt.Remove(r.AccountId); _regBackoff.Remove(r.AccountId);
            if (IsReady) UpdateServerCertBanner();                 // TLS 등록 = 새 핸드셰이크 = 관측 갱신 시점
            return;
        }
        if (r.State == RegState.Failed)
        {
            string msg = ResponseText.Describe(ResponseText.Area.Register, r.Code, r.Reason);
            Notify.Error($"{name} 등록 실패 — {msg}", $"{r.Code} {r.Reason}");
            int back = _regBackoff.TryGetValue(r.AccountId, out int b) ? Math.Min(60, b * 2) : 5;
            _regBackoff[r.AccountId] = back;
            _regRetryAt[r.AccountId] = DateTime.Now.AddSeconds(back);
        }
    }

    /// <summary>네트워크 복귀 — 즉시 재등록(§6).</summary>
    public void RefreshRegistrations()
    {
        foreach (var a in new[] { Volte, Ptt }) a?.RefreshRegistration();
    }

    private AccountKind KindOf(int accountId) => _accountKinds.TryGetValue(accountId, out var k) ? k : AccountKind.Volte;

    // ── 세션 투영 ──
    public SessionItem? Find(int callId) => Sessions.FirstOrDefault(s => s.CallId == callId);
    public IEnumerable<SessionItem> VolteCalls => Sessions.Where(s => s.IsVolteCall);
    public SessionItem? ActiveVolteCall => Sessions.FirstOrDefault(s => s.IsVolteCall && s.IsActive);
    public SessionItem? SessionOfGroup(string groupId) => Sessions.FirstOrDefault(s => s.Info.IsMcptt && s.Info.GroupId == groupId && !s.Info.ListenOnly);
    public SessionItem? ListenOfGroup(string groupId) => Sessions.FirstOrDefault(s => s.Info.IsMcptt && s.Info.GroupId == groupId && s.Info.ListenOnly);
    public SessionItem? MonitorOfDialog(string callId) => Sessions.FirstOrDefault(s => s.Kind == SessionKind.VolteMonitor && s.Info.JoinedDialog == callId);

    private void OnIncoming(CallInfo ci)
    {
        var s = Find(ci.CallId) ?? Create(ci, Operation.Incoming);
        s.Info = ci;
        if (!ci.IsMcptt || ci.Mcptt.PrivateCall)
        {
            var kind = ci.IsMcptt ? BannerKind.PttPrivateIncoming : IsPilot(ci.CalledParty) ? BannerKind.PilotIncoming : BannerKind.DirectIncoming;
            string who = Directory.Label(ci.RemoteUri);
            string title = kind switch
            {
                BannerKind.PilotIncoming => $"대표번호 {UserPartConverter.UserPart(ci.CalledParty)} 착신",
                BannerKind.PttPrivateIncoming => "PTT 사설콜 착신",
                _ => "착신",
            };
            Notify.ShowBanner(new Banner { Kind = kind, Title = title, Subtitle = who, Session = s });
        }
        IncomingCall?.Invoke(this, s);
    }

    public bool IsPilot(string? calledParty)
    {
        if (string.IsNullOrEmpty(calledParty) || PilotId.Length == 0) return false;
        return string.Equals(UserPartConverter.UserPart(calledParty), UserPartConverter.UserPart(PilotId), StringComparison.Ordinal);
    }

    private void OnCallState(CallInfo ci)
    {
        var s = Find(ci.CallId);
        if (ci.State == CallState.Disconnected)
        {
            if (s is null) return;
            s.Info = ci;
            End(s);
            return;
        }
        if (s is null)
        {
            Operation op = _pendingOps.Remove(ci.CallId, out var o) ? o : ci.Dir == CallDir.Incoming ? Operation.Incoming : Operation.Dial;
            s = Create(ci, op);
        }
        else s.Info = ci;
        if (ci.State != CallState.Incoming && Notify.BannerOf(s) is { } b) Notify.RemoveBanner(b);
        if (ci.State == CallState.Active && s.Kind == SessionKind.VolteCall && Settings.Current.AutoHoldOnAnswer)
            foreach (var other in VolteCalls.Where(o => o != s && o.IsActive).ToList()) Engine.GetCall(other.CallId).Hold();
        UpdateEmergencyBanner(s);
        SessionChanged?.Invoke(this, s);
    }

    private void OnCallMedia(CallInfo ci)
    {
        var s = Find(ci.CallId);
        if (s is null) return;
        s.Info = ci;
        SessionChanged?.Invoke(this, s);
    }

    private SessionItem Create(CallInfo ci, Operation op)
    {
        var s = new SessionItem(ci, KindOf(ci.AccountId), op);
        if (_pendingConsult.Remove(ci.CallId, out var orig)) s.ConsultFor = orig;
        s.Title = TitleOf(ci);
        if (s.Kind == SessionKind.PttAdhoc) s.AdhocMembers = AdhocMembersOf(ci.CallId);
        int route = Audio.DefaultRouteFor(s.Kind);
        if (route != 0) Engine.GetCall(ci.CallId).SetRoute(route);
        Sessions.Add(s);
        Log.Info($"session + #{ci.CallId} {s.Kind} {op} {ci.RemoteUri} group={ci.GroupId}");
        switch (s.Kind)
        {
            case SessionKind.PttPrivate: Activity.Add(ActivityPanel.Ptt, ActivityKind.Private, $"사설콜 {s.Title}", ci.Dir == CallDir.Incoming ? "착신" : "발신"); break;
            case SessionKind.PttAdhoc: Activity.Add(ActivityPanel.Ptt, ActivityKind.Adhoc, $"애드혹 {s.Title}", $"{s.AdhocMembers.Count}명"); break;
            case SessionKind.PttListen: Activity.Add(ActivityPanel.Ptt, ActivityKind.ListenStart, $"청취 시작 {s.Title}"); break;
            case SessionKind.VolteMonitor: Activity.Add(ActivityPanel.Call, ActivityKind.ListenStart, $"청취 시작 {s.Title}"); break;
        }
        SessionAdded?.Invoke(this, s);
        return s;
    }

    private string TitleOf(CallInfo ci)
    {
        if (ci.IsMcptt && !ci.Mcptt.PrivateCall)
        {
            if (AdhocIdFactory.IsAdhoc(ci.GroupId)) return "애드혹";
            return Groups.FirstOrDefault(g => g.Id == ci.GroupId)?.Name ?? ci.GroupId;
        }
        string label = Directory.Label(ci.RemoteUri);
        return label.Length > 0 ? label : UserPartConverter.UserPart(ci.RemoteUri);
    }

    private void End(SessionItem s)
    {
        Sessions.Remove(s);
        if (Notify.BannerOf(s) is { } b) Notify.RemoveBanner(b);
        var ci = s.Info;
        string dur = s.ConnectedAt is null ? "" : $" · {Fmt(DateTime.Now - s.ConnectedAt.Value)}";
        Log.Info($"session - #{s.CallId} {s.Kind} code={ci.LastCode} {ci.LastReason}");
        switch (s.Kind)
        {
            case SessionKind.VolteCall when s.Operation == Operation.Transfer && s.ConsultFor is not null:
                Activity.Add(ActivityPanel.Call, ActivityKind.Transfer, $"전달 {MyExtension} → {s.Title} attended", dur.Trim(' ', '·'), number: s.PeerNumber);
                break;
            case SessionKind.VolteCall when ci.Dir == CallDir.Incoming && s.ConnectedAt is null && IsPilot(ci.CalledParty):
                // 대표번호 포크 leg 가 응답 없이 끝남 — 동료가 받아 CANCEL 된 것일 수 있어 여기서는 부재로 세지 않는다.
                // 전원 무응답 부재는 대표번호 dialog 가 confirmed 없이 terminated 될 때(OnDialog) 1건만 기록한다.
                break;
            case SessionKind.VolteCall when ci.Dir == CallDir.Incoming && s.ConnectedAt is null:
                Activity.Add(ActivityPanel.Call, ActivityKind.Missed, $"부재 {MyExtension} ← {s.Title}", "", missed: true, number: s.PeerNumber);
                break;
            case SessionKind.VolteCall when ci.Dir == CallDir.Incoming:
                Activity.Add(ActivityPanel.Call, ActivityKind.Incoming, $"착신 {(IsPilot(ci.CalledParty) ? UserPartConverter.UserPart(ci.CalledParty) : MyExtension)} ← {s.Title}",
                             $"응답 {MyExtension}{dur}", number: s.PeerNumber, pilot: IsPilot(ci.CalledParty));
                break;
            case SessionKind.VolteCall when s.Operation == Operation.Pickup:
                Activity.Add(ActivityPanel.Call, ActivityKind.Pickup, $"픽업 {s.Title}", dur.Trim(' ', '·'), number: s.PeerNumber);
                break;
            case SessionKind.VolteCall:
                Activity.Add(ActivityPanel.Call, ActivityKind.Outgoing, $"발신 {MyExtension} → {s.Title}", s.ConnectedAt is null ? Fail(s) : dur.Trim(' ', '·'), number: s.PeerNumber);
                break;
            case SessionKind.VolteMonitor:
                Activity.Add(ActivityPanel.Call, ActivityKind.ListenEnd, $"청취 종료 {s.Title}", dur.Trim(' ', '·'));
                break;
            case SessionKind.PttListen:
                Activity.Add(ActivityPanel.Ptt, ActivityKind.ListenEnd, $"청취 종료 {s.Title}", dur.Trim(' ', '·'));
                break;
            case SessionKind.PttPrivate:
                Activity.Add(ActivityPanel.Ptt, ActivityKind.Private, $"사설콜 종료 {s.Title}", s.ConnectedAt is null ? Fail(s) : dur.Trim(' ', '·'));
                break;
            case SessionKind.PttAdhoc:
                Activity.Add(ActivityPanel.Ptt, ActivityKind.SessionEnd, $"애드혹 종료 {s.Title}", $"{dur.Trim(' ', '·')} · 참가 {s.AdhocMembers.Count}");
                break;
            case SessionKind.PttChannel:
                Activity.Add(ActivityPanel.Ptt, ActivityKind.SessionEnd, $"{s.Title} 세션 종료", dur.Trim(' ', '·'));
                break;
        }
        // 실패한 발신 동작의 사유(§9 사전) — 착신·정상 종료(BYE)는 제외
        if (s.ConnectedAt is null && ci.Dir == CallDir.Outgoing && ci.LastCode >= 300)
        {
            var area = ResponseText.AreaOf(s.Operation);
            if (s.Operation == Operation.Dial && ci.IsMcptt) area = s.Kind == SessionKind.PttPrivate ? ResponseText.Area.PttPrivate : ResponseText.Area.PttJoin;
            Notify.Error(ResponseText.Describe(area, ci.LastCode, ci.LastReason), $"{ci.LastCode} {ci.LastReason}");
        }
        if (Notify.BannerOfGroup(ci.GroupId) is { } eb && SessionOfGroup(ci.GroupId) is null) Notify.RemoveBanner(eb);
        SessionEnded?.Invoke(this, s);
    }

    private static string Fail(SessionItem s) => s.Info.LastCode >= 300 ? $"실패 {s.Info.LastCode}" : "";
    public static string Fmt(TimeSpan t) => t.TotalHours >= 1 ? t.ToString(@"h\:mm\:ss") : t.ToString(@"mm\:ss");

    private void UpdateEmergencyBanner(SessionItem s)
    {
        if (!s.Info.IsMcptt || s.Kind == SessionKind.PttPrivate) return;
        var existing = Notify.BannerOfGroup(s.Info.GroupId);
        bool emg = s.IsEmergency, peril = s.IsImminentPeril;
        if (!emg && !peril) { if (existing is not null) { Notify.RemoveBanner(existing); Activity.Add(ActivityPanel.Ptt, ActivityKind.Emergency, $"{s.Title} 긴급 해제", emergency: true); } return; }
        if (existing is not null && existing.IsEmg == emg) return;
        if (existing is not null) Notify.RemoveBanner(existing);
        Notify.ShowBanner(new Banner
        {
            Kind = emg ? BannerKind.Emergency : BannerKind.ImminentPeril, GroupId = s.Info.GroupId, Session = s,
            Title = emg ? $"긴급 — {s.Title}" : $"임박 위험 — {s.Title}",
            Subtitle = s.Info.Mcptt.CallingUserId.Length > 0 ? Directory.Label(s.Info.Mcptt.CallingUserId) : "",
        });
        Activity.Add(ActivityPanel.Ptt, ActivityKind.Emergency, $"{s.Title} {(emg ? "긴급" : "임박")} 개시", Directory.Label(s.Info.Mcptt.CallingUserId), emergency: true);
    }

    // ── floor ──
    private void OnFloor(FloorEvent ev)
    {
        var s = Find(ev.CallId);
        if (s is null) return;
        s.Floor = Engine.GetCall(ev.CallId).FloorInfo;
        s.LastFloor = ev;
        var now = DateTime.Now;
        switch (ev.Kind)
        {
            case FloorEventKind.Granted:
                s.Speaker = "나"; s.SpeakerSince = now; s.FloorNote = ""; s.TalkLimitNear = false; s.TalkGauge = 1;
                break;
            case FloorEventKind.Taken:
                var t = ev.Talkers.FirstOrDefault(x => !x.Self) ?? ev.Talkers.FirstOrDefault();
                string who = t is null ? "" : t.Self ? "나" : NameOfPtt(t.Id);
                if (s.Speaker != who) { CloseTalk(s, now); s.Speaker = who; s.SpeakerSince = now; }
                s.TalkGauge = 0;
                break;
            case FloorEventKind.Idle:
            case FloorEventKind.TalkerLeft:
                CloseTalk(s, now);
                s.TalkGauge = 0; s.TalkLimitNear = false;
                break;
            case FloorEventKind.Denied:
                s.FloorNote = ev.CauseText.Length > 0 ? ev.CauseText : "요청 거부"; CloseTalk(s, now); break;
            case FloorEventKind.Revoked:
                s.FloorNote = ev.CauseText.Length > 0 ? ev.CauseText : "발언권 회수"; CloseTalk(s, now); break;
            case FloorEventKind.QueuePosition:
                s.FloorNote = ev.QueuePosition >= 0 ? $"대기 {ev.QueuePosition + 1}번째" : "대기열"; break;
            case FloorEventKind.QueueCancelled:
                s.FloorNote = ""; break;
            case FloorEventKind.RequestTimeout:
                s.FloorNote = "요청 시간 초과"; break;
            case FloorEventKind.TalkLimit:
                s.TalkLimitNear = true; break;
        }
        Floor?.Invoke(this, (s, ev));
    }

    private void CloseTalk(SessionItem s, DateTime now)
    {
        if (s.Speaker.Length > 0 && s.SpeakerSince is DateTime since)
        {
            int sec = (int)Math.Round((now - since).TotalSeconds);
            Activity.Add(ActivityPanel.Ptt, ActivityKind.Talk, $"{s.Title} {s.Speaker} 발언 {sec}초");
        }
        s.Speaker = ""; s.SpeakerSince = null; s.SpeakerElapsed = TimeSpan.Zero;
    }

    public string NameOfPtt(string idOrUri)
    {
        string n = Directory.NameOf(idOrUri);
        if (n.Length > 0) return n;
        string u = UserPartConverter.UserPart(idOrUri);
        return u.Length > 4 ? "…" + u[^4..] : u;
    }

    // ── 로스터 ──
    private void OnRoster(RosterUpdate r)
    {
        Log.Info($"roster {r.GroupId} full={r.Full} " + string.Join(",", r.Users.Select(u => u.Uri + ":" + u.Status)));
        var g = Groups.FirstOrDefault(x => x.Id == r.GroupId);
        if (g is null) { g = new GroupInfo(r.GroupId, r.GroupId, r.GroupId, 0) { IsMember = false }; Groups.Add(g); }
        var before = g.Roster.Where(e => e.Status == "connected").Select(e => e.Uri).ToHashSet(StringComparer.OrdinalIgnoreCase);
        List<RosterEntry> next;
        if (r.Full) next = r.Users.ToList();
        else
        {
            next = g.Roster.ToList();
            foreach (var u in r.Users)
            {
                next.RemoveAll(e => string.Equals(e.Uri, u.Uri, StringComparison.OrdinalIgnoreCase));
                if (u.Status != "disconnected") next.Add(u);
            }
        }
        bool firstSnapshot = g.RosterAt is null;                 // 구독 직후 full 스냅샷 — 이미 있던 참가자를 "합류"로 적지 않는다
        g.Roster = next;
        g.RosterAt = DateTime.Now;
        var after = next.Where(e => e.Status == "connected").Select(e => e.Uri).ToHashSet(StringComparer.OrdinalIgnoreCase);
        if (g.IsMember && !firstSnapshot && before.Count + after.Count > 0)
        {
            foreach (var u in after.Except(before)) if (!IsMe(u)) Activity.Add(ActivityPanel.Ptt, ActivityKind.Member, $"{g.Name} {NameOfPtt(u)} 합류");
            foreach (var u in before.Except(after)) if (!IsMe(u)) Activity.Add(ActivityPanel.Ptt, ActivityKind.Member, $"{g.Name} {NameOfPtt(u)} 이탈");
        }
        RosterChanged?.Invoke(this, g);
    }

    public bool IsMe(string uri) => string.Equals(UserPartConverter.UserPart(uri), MyPttNumber, StringComparison.Ordinal)
                                   || string.Equals(UserPartConverter.UserPart(uri), MyExtension, StringComparison.Ordinal);

    // ── dialog (BLF·대기열·④) ──
    private void OnDialog(DialogInfo d)
    {
        Log.Info($"dialog watched={d.Watched} id={d.Id} state={d.State} dir={d.Direction} remote={d.RemoteIdentity} callid={d.CallId} full={d.Full}");
        // 초기 full 스냅샷에 dialog 가 없으면 코어가 id·state 빈 자리표시자를 올린다(구독 성립 신호) — 행으로 만들지 않는다
        if (d.Id.Length == 0 || d.State.Length == 0) return;
        string key = d.Watched + "|" + d.Id;
        var row = Dialogs.FirstOrDefault(x => x.Key == key);
        if (d.State == "terminated")
        {
            // dialog id(Call-ID+태그)는 양 당사자에게 같은 하나의 dialog 다 — 종료는 entity 가 무엇이든 그 id 의 행 전부에 적용한다.
            // (CSP 가 대표번호 포크 호 종료 NOTIFY 의 entity/direction 을 다른 회선으로 붙여 보내는 경우가 있어 — 서버 결함 보고 —
            //  entity|id 키만 보면 ③ 띠·④ 진행 중에 "통화 중" 행이 남는다.)
            var ended = Dialogs.Where(x => x.Id == d.Id).ToList();
            if (ended.Count == 0) return;
            foreach (var r in ended)
            {
                var since = r.StateSince; var last = r.Info;                    // confirmed 시각(통화 길이)·상대 — 종료 본문이 덮기 전에
                if (r == row) r.Apply(d);
                else r.Apply(r.Info with { State = "terminated" });
                Dialogs.Remove(r);
                if (IsPilot(r.Watched)) RecordPilotOutcome(r, last, since);
                DialogEnded?.Invoke(this, r);
            }
            return;
        }
        if (row is null) { row = new DialogRow(d); Dialogs.Add(row); }
        else row.Apply(d);
        if (row.IsConfirmed)
        {
            row.WasConfirmed = true;
            // 그룹원 회선이 같은 발신자와 confirmed 되면 그 회선이 대표번호 호의 응답자(포크 승자) — 대표번호 행에 기록(§4.4).
            // 대표번호·회선 confirmed NOTIFY 순서는 서버가 정하지 않으므로 어느 쪽이 먼저 와도 맞물리게 양방향으로 본다.
            if (!IsPilot(row.Watched))
                foreach (var pr in Dialogs.Where(x => IsPilot(x.Watched) && x.AnsweredBy.Length == 0 && x.RemoteNumber == row.RemoteNumber)) pr.AnsweredBy = row.WatchedNumber;
            else if (row.AnsweredBy.Length == 0)
                row.AnsweredBy = Dialogs.FirstOrDefault(x => !IsPilot(x.Watched) && x.IsConfirmed && x.RemoteNumber == row.RemoteNumber)?.WatchedNumber ?? "";
        }
        DialogChanged?.Invoke(this, row);
    }

    /// <summary>대표번호 호의 결과 행 — 대표번호 dialog 가 정본(§4.4·§13): 전원 무응답 = 부재, 동료 응답 = 착신(응답자 표기).
    /// 내가 받은 호는 내 세션 종료(OnSessionEnded)가 이미 "착신 … 응답 나" 를 남기므로 여기서는 만들지 않는다.
    /// 서버 이력(call.*)의 대표번호 항목은 응답자 필드가 없어 이 행과 겹치므로 OnHistory 가 건너뛴다.</summary>
    private void RecordPilotOutcome(DialogRow r, DialogInfo last, DateTime confirmedAt)
    {
        string pilot = UserPartConverter.UserPart(PilotId);
        string caller = UserPartConverter.UserPart(last.RemoteIdentity);
        if (!r.WasConfirmed)
        {
            Activity.Add(ActivityPanel.Call, ActivityKind.Missed, $"부재 {pilot} ← {Directory.Label(caller)}", "전원 무응답", missed: true, number: caller, pilot: true);
            return;
        }
        if (r.AnsweredBy == MyExtension) return;
        string who = r.AnsweredBy.Length > 0 ? Directory.Label(r.AnsweredBy) : "그룹원(미상)";
        Activity.Add(ActivityPanel.Call, ActivityKind.Incoming, $"착신 {pilot} ← {Directory.Label(caller)}", $"응답 {who} · {Fmt(DateTime.Now - confirmedAt)}",
                     number: caller, pilot: true);
    }

    // ── 관제 동작 (§2 표) ──
    private Result Track(Result<Call> r, Operation op, SessionItem? consultFor = null)
    {
        if (!r.Ok) { Notify.Error(ResponseText.Describe(ResponseText.AreaOf(op), r.Code, r.Reason), r.ToString()); return r.WithoutValue(); }
        _pendingOps[r.Value.Id] = op;
        if (consultFor is not null) _pendingConsult[r.Value.Id] = consultFor;
        // 같은 그룹 재참여처럼 이미 세션이 있으면 즉시 반영
        if (Find(r.Value.Id) is null && r.Value.Info.IsLive) OnCallState(r.Value.Info);
        return Result.Success;
    }

    private Result Show(Result r, ResponseText.Area area)
    {
        if (!r.Ok) Notify.Error(ResponseText.Describe(area, r.Code, r.Reason), r.ToString());
        return r;
    }

    public Result Dial(string target)
    {
        if (Volte is null) return Fail("VoLTE 계정 없음");
        return Track(Volte.Dial(target.Contains(':') ? target : ToSipUri(target)), Operation.Dial);
    }

    public Result Pickup(string? number = null)
    {
        if (Volte is null) return Fail("VoLTE 계정 없음");
        return Track(Volte.Pickup(Settings.Current.PickupFeatureCode, number), Operation.Pickup);
    }

    public Result JoinMonitor(DialogRow row)
    {
        if (Volte is null) return Fail("VoLTE 계정 없음");
        if (MonitorOfDialog(row.Info.CallId) is not null) return Fail("이미 청취 중");
        if (MonitorCount >= Settings.Current.MaxMonitorWindows) return Fail($"동시 청취 상한 {Settings.Current.MaxMonitorWindows}");
        return Track(Volte.Join(row.Watched, row.Info), Operation.Join);
    }

    public int MonitorCount => Sessions.Count(s => s.IsWindow);

    public Result Answer(SessionItem s) => Show(Engine.GetCall(s.CallId).Answer(), ResponseText.Area.Call);
    public Result Reject(SessionItem s) => Show(Engine.GetCall(s.CallId).Reject(486), ResponseText.Area.Call);
    public Result Hangup(SessionItem s) => Show(Engine.GetCall(s.CallId).Hangup(), ResponseText.Area.Call);
    public Result Hold(SessionItem s) => Show(Engine.GetCall(s.CallId).Hold(), ResponseText.Area.Call);
    public Result Resume(SessionItem s)
    {
        if (Settings.Current.AutoHoldOnAnswer)
            foreach (var other in VolteCalls.Where(o => o != s && o.IsActive).ToList()) Engine.GetCall(other.CallId).Hold();
        return Show(Engine.GetCall(s.CallId).Resume(), ResponseText.Area.Call);
    }
    public Result ToggleMute(SessionItem s) => Show(Engine.GetCall(s.CallId).SetMuted(!s.Info.Muted), ResponseText.Area.Call);
    public Result Dtmf(SessionItem s, string digits) => Show(Engine.GetCall(s.CallId).SendDtmf(digits), ResponseText.Area.Call);
    public Result SetRoute(SessionItem s, int route) => Show(Engine.GetCall(s.CallId).SetRoute(route), ResponseText.Area.Call);
    public Result ToggleRoute(SessionItem s) => SetRoute(s, s.Route == 0 ? Audio.SpeakerRoute : 0);

    public Result TransferBlind(SessionItem s, string target)
    {
        var r = Engine.GetCall(s.CallId).Transfer(target.Contains(':') ? target : ToSipUri(target));
        if (r.Ok) { s.TransferNote = $"전달 중 → {Directory.Label(target)}"; Activity.Add(ActivityPanel.Call, ActivityKind.Transfer, $"전달 {MyExtension} → {Directory.Label(target)} blind", s.Title, number: UserPartConverter.UserPart(target)); }
        return Show(r, ResponseText.Area.Transfer);
    }

    /// <summary>상담 전달 시작 — 원 통화 보류 + 상담 호 발신(배지 "상담").</summary>
    public Result StartConsult(SessionItem original, string target)
    {
        if (Volte is null) return Fail("VoLTE 계정 없음");
        if (original.IsActive) Engine.GetCall(original.CallId).Hold();
        return Track(Volte.Dial(target.Contains(':') ? target : ToSipUri(target)), Operation.Transfer, original);
    }

    public Result CompleteConsult(SessionItem consult)
    {
        if (consult.ConsultFor is null) return Fail("상담 호가 아님");
        var r = Engine.GetCall(consult.ConsultFor.CallId).TransferAttended(Engine.GetCall(consult.CallId));
        if (r.Ok) consult.ConsultFor.TransferNote = $"전달 중 → {consult.Title}";
        return Show(r, ResponseText.Area.Transfer);
    }

    public Result CancelConsult(SessionItem consult)
    {
        var orig = consult.ConsultFor;
        var r = Engine.GetCall(consult.CallId).Hangup();
        if (orig is not null && orig.IsHeld) Engine.GetCall(orig.CallId).Resume();
        return Show(r, ResponseText.Area.Call);
    }

    // PTT
    public Result JoinChannel(GroupInfo g)
    {
        if (Ptt is null) return Fail("PTT 계정 없음");
        return Track(Ptt.JoinGroupCall(g.Id), Operation.PttJoin);
    }

    public Result LeaveChannel(SessionItem s) => Show(Engine.GetCall(s.CallId).LeaveGroupCall(), ResponseText.Area.PttJoin);

    public Result ListenGroup(GroupInfo g)
    {
        if (Ptt is null) return Fail("PTT 계정 없음");
        if (ListenOfGroup(g.Id) is not null) return Fail("이미 청취 중");
        if (MonitorCount >= Settings.Current.MaxMonitorWindows) return Fail($"동시 청취 상한 {Settings.Current.MaxMonitorWindows}");
        return Track(Ptt.JoinGroupCall(g.Id, new GroupCallOptions { ListenOnly = true }), Operation.PttListen);
    }

    public Result EmergencyCall(GroupInfo g)
    {
        if (Ptt is null) return Fail("PTT 계정 없음");
        return Track(Ptt.JoinGroupCall(g.Id, new GroupCallOptions { Emergency = true }), Operation.Emergency);
    }

    public Result StartPrivateCall(string peer, bool fullDuplex, bool emergency)
    {
        if (Ptt is null) return Fail("PTT 계정 없음");
        return Track(Ptt.StartPrivateCall(UserPartConverter.UserPart(peer), new GroupCallOptions { FullDuplex = fullDuplex, Emergency = emergency }),
                     emergency ? Operation.Emergency : Operation.PttPrivate);
    }

    public Result StartAdhoc(IReadOnlyList<string> members, bool emergency)
    {
        if (Ptt is null) return Fail("PTT 계정 없음");
        if (members.Count == 0) return Fail("대상을 고르세요");
        string id = AdhocIdFactory.Create(MyPttNumber);
        var tels = members.Select(ToTelUri).ToList();
        var r = Ptt.JoinGroupCall(id, new GroupCallOptions { Members = tels, Emergency = emergency });
        if (r.Ok) _adhocMembers[r.Value.Id] = tels;
        return Track(r, Operation.PttAdhoc);
    }
    private readonly Dictionary<int, List<string>> _adhocMembers = new();
    public IReadOnlyList<string> AdhocMembersOf(int callId) => _adhocMembers.TryGetValue(callId, out var m) ? m : Array.Empty<string>();

    public Result FloorRequest(SessionItem s) => Show(Engine.GetCall(s.CallId).FloorRequest(), ResponseText.Area.PttJoin);
    public Result FloorRelease(SessionItem s) => Engine.GetCall(s.CallId).FloorRelease();
    public Result FloorQueueCancel(SessionItem s) => Engine.GetCall(s.CallId).FloorQueueCancel();

    // 메시지
    /// <summary>그룹 SDS 발신 — Value.MsgId(disposition 상관)·Value.Token(RequestCompleted 상관).</summary>
    public Result<SdsSend> SendGroupSds(string groupId, string text)
    {
        if (Ptt is null) return Result<SdsSend>.Fail(-1, "PTT 계정 없음");
        var r = Ptt.SendGroupSds(groupId, text, requestDelivery: true);
        if (!r.Ok) Notify.Error(ResponseText.Describe(ResponseText.Area.Sds, r.Code, r.Reason), r.ToString());
        return r;
    }

    public Result<long> SendSdsNotification(string peer, string convId, string msgId, int notifType)
    {
        if (Ptt is null) return Result<long>.Fail(-1, "PTT 계정 없음");
        var r = Ptt.SendSdsNotification(UserPartConverter.UserPart(peer), convId, msgId, notifType);
        if (!r.Ok) Log.Warn($"sds notification → {peer}: {r}");
        return r;
    }

    public Result<long> SendSms(string target, string text)
    {
        if (Volte is null) return Result<long>.Fail(-1, "VoLTE 계정 없음");
        var r = Volte.SendRequest("MESSAGE", target.Contains(':') ? target : ToSipUri(target), "text/plain", text);
        if (!r.Ok) Notify.Error(ResponseText.Describe(ResponseText.Area.Sms, r.Code, r.Reason), r.ToString());
        return r;
    }

    private Result Fail(string why) { Notify.Warn(why); return Result.Fail(-1, why); }

    // ── 1초 틱 ──
    public void Tick(DateTime now)
    {
        foreach (var s in Sessions) s.Tick(now);
        foreach (var d in Dialogs) d.Tick(now);
        Notify.Tick(now);
        foreach (var (acc, at) in _regRetryAt.ToList())
            if (now >= at) { _regRetryAt.Remove(acc); Engine.GetAccount(acc).Register(); }
        if (now.Date != _lastPrune) { _lastPrune = now.Date; Activity.Prune(now); }   // 날짜가 바뀐 첫 틱 — 정각 틱을 놓쳐도 하루 밀리지 않게
        if (IsReady && HasDesk && now >= _nextDispatchPoll) { _nextDispatchPoll = now.AddSeconds(DispatchPollSec); _ = RefreshDispatchAsync(); }
        if (IsReady && now >= _nextServerCertCheck) { _nextServerCertCheck = now.AddSeconds(ServerCertCheckSec); UpdateServerCertBanner(); }
        // 아무것도 조회하지 않는 관제석도 토큰은 살아 있어야 한다 — 만료가 가까우면 틱이 먼저 갱신한다(§6).
        //   갱신이 실패하는 중이면 다음 틱이 다시 시도하므로 경고 띠가 붙은 채 스스로 회복한다.
        if (_tokens is not null && now >= _nextTokenCheck) { _nextTokenCheck = now.AddSeconds(TokenCheckSec); _ = AccessTokenAsync(); }
    }
    private DateTime _lastPrune = DateTime.Today;

    public void Dispose()
    {
        _history?.Dispose();
        Endpoints.Dispose();
        Engine.Dispose();
        _csc?.Dispose();
        Messages.Dispose();
    }
}
