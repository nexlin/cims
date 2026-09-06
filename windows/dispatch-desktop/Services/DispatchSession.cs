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

    // ── 신원 (§3.2 상단 바) ──
    public DispatchProfile Dispatch => Profile?.Dispatch ?? DispatchProfile.None;
    public bool HasDesk => Dispatch.Present;
    public string DisplayName => Profile?.DisplayName ?? "";
    public string LoginId => Profile?.LoginId ?? "";
    public ServiceProfile? VolteService => Profile?.Service("volte");
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
    /// <summary>GMS 그룹 생성 자격(`ptt.allowGroupCreation`) — [새 그룹] 노출. 편집·삭제는 그룹별 IsOwner.</summary>
    public bool CanCreateGroups => Profile?.AllowGroupCreation == true && Ptt is not null;
    public string PttDomain => PttService?.Domain ?? "";

    partial void OnProfileChanged(Profile? value)
    {
        OnPropertyChanged(nameof(Dispatch)); OnPropertyChanged(nameof(HasDesk)); OnPropertyChanged(nameof(DisplayName));
        OnPropertyChanged(nameof(MyExtension)); OnPropertyChanged(nameof(MyPttId)); OnPropertyChanged(nameof(MyPttNumber));
        OnPropertyChanged(nameof(PilotId)); OnPropertyChanged(nameof(GroupName)); OnPropertyChanged(nameof(CanMonitorCalls));
        OnPropertyChanged(nameof(CanListenPtt)); OnPropertyChanged(nameof(ListenHidden)); OnPropertyChanged(nameof(CanCreateGroups));
        OnPropertyChanged(nameof(PttDomain));
    }
    partial void OnPttChanged(Account? value) => OnPropertyChanged(nameof(CanCreateGroups));

    // ── 로그인·프로비저닝 (§6) ──
    private const string RefreshTokenKey = "csc.refresh";

    public bool HasSavedLogin => Settings.Current.AutoLogin && Credentials.Load(RefreshTokenKey) is { Length: > 0 };

    private CscClient MakeCsc(string host, int port)
    {
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
        _tokens = tok.Value;
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
        _tokens = tok.Value;
        if (tok.Value.RefreshToken.Length > 0) Credentials.Save(RefreshTokenKey, tok.Value.RefreshToken);
        return await FetchProfileAsync(ct);
    }

    private async Task<Result> FetchProfileAsync(CancellationToken ct)
    {
        if (_csc is null || _tokens is null) return Result.Fail(-1, "로그인 전");
        var p = await _csc.FetchProfileAsync(_tokens.AccessToken, ct);
        if (!p.Ok) return p.WithoutValue();
        Profile = p.Value;
        Directory.CountryCode = p.Value.CountryCode;
        Directory.SetMembers(p.Value.Dispatch.Members);              // 서버 그룹원 목록(없으면 CSV member 폴백)
        Log.Info($"profile {p.Value.LoginId} services={string.Join(",", p.Value.Services.Select(s => s.Kind))} desk={p.Value.Dispatch.Present} " +
                 $"group={p.Value.Dispatch.GroupId} pilot={p.Value.Dispatch.PilotId} monitor={p.Value.Dispatch.MonitorScope} pttListen={p.Value.Dispatch.PttListen} " +
                 $"members={p.Value.Dispatch.Members.Count} pttTargets={p.Value.Dispatch.PttTargets.Count} groupCreate={p.Value.AllowGroupCreation} cc={p.Value.CountryCode}");
        return Result.Success;
    }

    /// <summary>회사 전화번호부 동기화 — `/provisioning/directory?service=volte|ptt`, ETag 로 304 면 다운로드 생략(android_ue_provisioning.md §3-1).</summary>
    public async Task SyncDirectoryAsync(CancellationToken ct = default)
    {
        if (_csc is null || _tokens is null) return;
        var csc = _csc; string token = _tokens.AccessToken;
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

        foreach (var sp in Profile.Services)
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

        // 관제 범위: 그룹원·대표번호 dialog 구독 (§4.3) — 그룹원 = 프로비저닝 members[](정본) 또는 CSV member 폴백
        if (HasDesk && Volte is not null)
        {
            foreach (var m in Directory.Members) Watch(m.Number);
            if (PilotId.Length > 0) Watch(PilotId);
        }
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
        ProfileApplied?.Invoke(this, EventArgs.Empty);
        return Result.Success;
    }

    // ── PTT 그룹 목록·관리 (GMS, TS 24.481 — 서버 요청서 §1) ──
    private int _groupRefreshSeq;

    /// <summary>GMS 목록 재조회 → Groups 를 차분 갱신: 새 멤버 그룹은 affiliation+conference 구독, 사라진 그룹은 해제.
    /// 청취 범위 그룹(pttTargets, 비멤버)은 conference 구독만. Groups 의 CollectionChanged 로 주소록·채널 카드·설정이 따라온다.</summary>
    public async Task<Result> RefreshGroupsAsync(CancellationToken ct = default)
    {
        if (Ptt is null || _csc is null || _tokens is null) return Result.Fail(-1, "PTT 계정 없음");
        var csc = _csc; string token = _tokens.AccessToken; var ptt = Ptt;
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
        foreach (var gone in Groups.Where(x => x.IsMember && !seen.Contains(x.Id)).ToList())
        {
            ptt.Affiliate(gone.Id, false);
            ptt.SubscribeConference(gone.Id, false);
            Groups.Remove(gone);
            Log.Info($"group gone {gone.Id}");
        }
        // 청취 범위 그룹 — 프로비저닝 pttTargets(서버가 ptt_listen 범위를 해석한 목록). 로스터 NOTIFY 로 진행/참가자 수를 안다.
        if (CanListenPtt)
            foreach (var t in Dispatch.PttTargets)
            {
                if (t.Id.Length == 0 || seen.Contains(t.Id) || Groups.Any(x => x.Id == t.Id)) continue;
                string uri = t.Uri.Length > 0 ? t.Uri : $"sip:{t.Id}@{PttDomain}";
                Groups.Add(new GroupInfo(t.Id, uri, t.Name.Length > 0 ? t.Name : t.Id, 0) { IsMember = false });
                var sc = ptt.SubscribeConference(t.Id, true);
                if (!sc.Ok) Log.Warn($"conference subscribe(listen scope) {t.Id}: {sc}");
            }
        Directory.SetGroups(Groups.Where(x => x.IsMember));
        Log.Info($"groups {Groups.Count(x => x.IsMember)} member ({Groups.Count(x => x.IsOwner)} owned), {Groups.Count(x => !x.IsMember)} listen-scope");
        return Result.Success;
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

    /// <summary>새 그룹 uri — XCAP 은 클라이언트가 문서를 명명한다(sip:g-&lt;8hex&gt;@ptt 도메인).</summary>
    public string NewGroupUri() => $"sip:g-{Guid.NewGuid():N}"[..14] + "@" + PttDomain;

    public Task<Result<GroupDoc>> GetGroupAsync(GroupInfo g, CancellationToken ct = default)
    {
        if (_csc is null || _tokens is null) return Task.FromResult(Result<GroupDoc>.Fail(-1, "로그인 전"));
        return _csc.GetGroupAsync(_tokens.AccessToken, MyPttId, g.Uri, ct);
    }

    /// <summary>그룹 생성/수정(PUT). ifMatch = 편집 시작 시 ETag(충돌 412). 성공 시 목록 재조회.</summary>
    public async Task<Result<GroupDoc>> SaveGroupAsync(GroupDoc doc, string? ifMatch, CancellationToken ct = default)
    {
        if (_csc is null || _tokens is null) return Result<GroupDoc>.Fail(-1, "로그인 전");
        var r = await _csc.PutGroupAsync(_tokens.AccessToken, MyPttId, doc, ifMatch, ct);
        if (!r.Ok) { Notify.Error(ResponseText.Describe(ResponseText.Area.Group, r.Code, r.Reason), r.ToString()); return r; }
        bool isNew = ifMatch is null || ifMatch.Length == 0;
        Activity.Add(ActivityPanel.Ptt, ActivityKind.Note, $"그룹 {(isNew ? "생성" : "편집")} {r.Value.DisplayName}", $"멤버 {r.Value.Members.Count}");
        Notify.Info($"그룹 {(isNew ? "생성" : "편집")} 완료", r.Value.DisplayName);
        await RefreshGroupsAsync(ct);
        return r;
    }

    public async Task<Result> DeleteGroupAsync(GroupInfo g, CancellationToken ct = default)
    {
        if (_csc is null || _tokens is null) return Result.Fail(-1, "로그인 전");
        var r = await _csc.DeleteGroupAsync(_tokens.AccessToken, MyPttId, g.Uri, ct);
        if (!r.Ok) { Notify.Error(ResponseText.Describe(ResponseText.Area.Group, r.Code, r.Reason), r.ToString()); return r; }
        Activity.Add(ActivityPanel.Ptt, ActivityKind.Note, $"그룹 삭제 {g.Name}");
        Notify.Info("그룹 삭제 완료", g.Name);
        await RefreshGroupsAsync(ct);
        return r;
    }

    private void Watch(string numberOrAor)
    {
        if (Volte is null) return;
        string aor = ToSipUri(numberOrAor);
        if (!_watched.Add(aor)) return;
        var r = Volte.DialogWatch(aor, true);
        if (!r.Ok) Log.Warn($"dialogWatch {aor}: {r}");
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
        Engine.Stop();
        Credentials.Delete(RefreshTokenKey);
        _tokens = null; _loginPw = "";
        Volte = null; Ptt = null; Profile = null;
        _accountKinds.Clear(); _watched.Clear(); _pendingOps.Clear(); _regRetryAt.Clear(); _regBackoff.Clear();
        Sessions.Clear(); Groups.Clear(); Dialogs.Clear();
        VolteReg = RegInfo.Empty; PttReg = RegInfo.Empty;
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
        var kind = KindOf(r.AccountId);
        if (kind == AccountKind.Ptt) PttReg = r; else VolteReg = r;
        string name = kind == AccountKind.Ptt ? "PTT" : "VoLTE";
        Log.Info($"reg {name} {r.State} {r.Code} {r.Reason}");
        if (r.State == RegState.Registered) { _regRetryAt.Remove(r.AccountId); _regBackoff.Remove(r.AccountId); return; }
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
            case SessionKind.VolteCall when ci.Dir == CallDir.Incoming && s.ConnectedAt is null:
                Activity.Add(ActivityPanel.Call, ActivityKind.Missed, $"부재 {(IsPilot(ci.CalledParty) ? UserPartConverter.UserPart(ci.CalledParty) : MyExtension)} ← {s.Title}",
                             "", missed: true, number: s.PeerNumber, pilot: IsPilot(ci.CalledParty));
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
        g.Roster = next;
        g.RosterAt = DateTime.Now;
        var after = next.Where(e => e.Status == "connected").Select(e => e.Uri).ToHashSet(StringComparer.OrdinalIgnoreCase);
        if (g.IsMember && g.RosterAt is not null && before.Count + after.Count > 0)
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
        string key = d.Watched + "|" + d.Id;
        var row = Dialogs.FirstOrDefault(x => x.Key == key);
        if (d.State == "terminated")
        {
            if (row is null) return;
            row.Apply(d);
            Dialogs.Remove(row);
            if (IsPilot(d.Watched) && !row.WasConfirmed)
                Activity.Add(ActivityPanel.Call, ActivityKind.Missed, $"부재 {UserPartConverter.UserPart(PilotId)} ← {Directory.Label(d.RemoteIdentity)}", "전원 무응답",
                             missed: true, number: row.RemoteNumber, pilot: true);
            DialogEnded?.Invoke(this, row);
            return;
        }
        if (row is null) { row = new DialogRow(d); Dialogs.Add(row); }
        else row.Apply(d);
        if (row.IsConfirmed) row.WasConfirmed = true;
        DialogChanged?.Invoke(this, row);
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
    public Result<string> SendGroupSds(string groupId, string text)
    {
        if (Ptt is null) return Result<string>.Fail(-1, "PTT 계정 없음");
        var r = Ptt.SendGroupSds(groupId, text, requestDelivery: true);
        if (!r.Ok) Notify.Error(ResponseText.Describe(ResponseText.Area.Sds, r.Code, r.Reason), r.ToString());
        return r;
    }

    public Result SendSdsNotification(string peer, string convId, string msgId, int notifType) =>
        Ptt?.SendSdsNotification(UserPartConverter.UserPart(peer), convId, msgId, notifType) ?? Fail("PTT 계정 없음");

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
        if (now.Second == 0 && now.Minute == 0) Activity.Prune(now);
    }

    public void Dispose()
    {
        Endpoints.Dispose();
        Engine.Dispose();
        _csc?.Dispose();
        Messages.Dispose();
    }
}
