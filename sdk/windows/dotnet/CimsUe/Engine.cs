// CimsUe — Engine 파사드 (cimsue/engine.h · listener.h 1:1, ue_sdk.md §6.4)
//
// 프로세스당 1개. 명령은 어느 스레드에서 불러도 되고(코어가 ue-ctl 로 직렬화) 즉시 Result 를 돌려준다. 프로토콜 진행은
// 이벤트로 온다 — 코어 이벤트 스레드의 콜백을 여기서 관리 객체로 복사한 뒤 SynchronizationContext.Post 로 앱 스레드에
// 넘긴다(생성 시점의 SynchronizationContext.Current 또는 생성자 인자; 없으면 이벤트 스레드에서 그대로 호출). WPF Dispatcher 는
// 앱만 안다 — 이 어셈블리는 UI 프레임워크를 참조하지 않는다.
// 계정·호는 코어와 같은 정수 id 이며 Account/Call 래퍼가 id 를 감싼다(같은 id 는 같은 래퍼 — 호는 DISCONNECTED 에서 해제).
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using CimsUe.Native;
using static CimsUe.Native.NativeMethods;

namespace CimsUe;

public sealed unsafe class Engine : IDisposable
{
    private IntPtr _h;
    private GCHandle _self;
    private readonly SynchronizationContext? _sync;
    private readonly ConcurrentDictionary<int, Account> _accounts = new();
    private readonly ConcurrentDictionary<int, Call> _calls = new();

    /// <param name="eventContext">이벤트를 전달할 스레드 컨텍스트. null 이면 생성 스레드의 SynchronizationContext.Current(UI 스레드에서 만들면 UI 스레드).
    /// 그것도 없으면(콘솔·시험) 코어 이벤트 스레드에서 직접 호출한다.</param>
    public Engine(SynchronizationContext? eventContext = null)
    {
        _h = cimsue_engine_create();
        if (_h == IntPtr.Zero) throw new CimsUeException("cimsue_engine_create 실패");
        _self = GCHandle.Alloc(this);
        _sync = eventContext ?? SynchronizationContext.Current;
    }

    internal IntPtr Handle => _h != IntPtr.Zero ? _h : throw new ObjectDisposedException(nameof(Engine));

    /// <summary>라이브러리·엔진 버전.</summary>
    public static string Version => Utf8.Str(cimsue_version());
    public bool IsRunning => _h != IntPtr.Zero && cimsue_engine_running(_h) != 0;
    /// <summary>이벤트가 전달되는 컨텍스트(null = 이벤트 스레드 직접).</summary>
    public SynchronizationContext? EventContext => _sync;

    // ── 이벤트 (Listener 1:1) ──
    public event EventHandler<LogLine>? Log;
    public event EventHandler<RegInfo>? RegistrationChanged;
    /// <summary>착신 — 180 은 코어가 이미 보냈다. MCPTT 착신은 AutoAnswerMcptt 면 코어가 200 까지 보낸다.</summary>
    public event EventHandler<CallInfo>? IncomingCall;
    public event EventHandler<CallInfo>? CallStateChanged;
    /// <summary>미디어 활성/보류/소스(SSRC 라벨) 변화.</summary>
    public event EventHandler<CallInfo>? CallMediaChanged;
    /// <summary>floor participant 상태 전이 (TS 24.380 §6.2.4). 마이크 게이트는 코어가 이미 처리했다.</summary>
    public event EventHandler<FloorEvent>? FloorChanged;
    /// <summary>MCPTT 세션 조건 변화(긴급·임박, TS 24.379 §10.1.1.2.1.3~6) — Info.Condition 이 새 값, Cause 가 계기. 호 상태 이벤트와 다른 흐름이다.</summary>
    public event EventHandler<McpttConditionChange>? McpttConditionChanged;
    /// <summary>긴급 경보·취소·긴급 통지 수신(TS 24.379 §12.1.1.3). 200 OK 는 코어가 이미 보냈다.</summary>
    public event EventHandler<EmergencyAlert>? EmergencyAlertReceived;
    /// <summary>개시 호의 미응답 멤버 알림(TS 24.379 §6.3.3.3 — INFO g.3gpp.mcptt-info) — Info.NonAcknowledgedUsers. 200 OK 는 코어가 이미 보냈다.</summary>
    public event EventHandler<CallInfo>? NonAcknowledgedUsersReceived;
    /// <summary>MCVideo 송출 제어(TS 24.581 §6.2.4) — 허가·거절·회수·대기·종료.</summary>
    public event EventHandler<TransmissionEvent>? TransmissionChanged;
    /// <summary>MCVideo 수신 제어(§6.2.5) — 새 송출 알림(manual 이면 Call.AcceptReception)·수신 허가·종료.</summary>
    public event EventHandler<ReceptionEvent>? ReceptionChanged;
    /// <summary>영상 프레임(창 없는 프레임 렌더 — Windows 엔진, ue_sdk.md §4.5). **다른 이벤트와 달리 EventContext 로 넘기지 않고 영상 스레드에서
    /// 곧바로** 부른다(프레임마다 초당 15~30회) — 화소를 복사하고 곧 돌아간다. 핸들러 안에서 엔진 명령을 부르지 않는다(엔진이 영상 포트를 멈추며
    /// 이 스레드를 기다리는 중이면 교착한다). 수신 영상 = 그 호의 CallId, 셀프뷰 = -1(<see cref="SetVideoPreview"/>).</summary>
    public event VideoFrameHandler? VideoFrameReceived;
    /// <summary>통화 중 영상 전환(1:1 호, RFC 3264 §8.1) — 상대의 요청(Received → <see cref="AnswerVideoRequest"/>)·내 요청의 결과.</summary>
    public event EventHandler<VideoRequestEvent>? VideoRequestChanged;
    /// <summary>그룹 로스터(RFC 4575) — 구독 NOTIFY 또는 in-dialog NOTIFY.</summary>
    public event EventHandler<RosterUpdate>? RosterChanged;
    /// <summary>감시 대상 dialog 상태(RFC 4235 NOTIFY) — dialog 하나당 1회.</summary>
    public event EventHandler<DialogInfo>? DialogInfoReceived;
    public event EventHandler<SdsMessage>? SdsReceived;
    /// <summary>임의 요청(PUBLISH/MESSAGE/SUBSCRIBE)의 최종 응답 — token 상관.</summary>
    public event EventHandler<RequestResult>? RequestCompleted;
    /// <summary>MCData 가 아닌 MESSAGE/NOTIFY 본문(text/plain 문자·xcap-diff) — 앱이 해석.</summary>
    public event EventHandler<SipMessage>? MessageReceived;
    public event EventHandler? Stopped;
    /// <summary>이벤트 핸들러가 던진 예외(이벤트 스레드 직접 호출일 때 — 네이티브 경계 밖으로 새지 않게 잡는다).</summary>
    public event EventHandler<Exception>? HandlerFailed;

    // ── 수명 ──

    /// <summary>기동 — transport 생성·코덱 정합·장치 준비. 이미 기동 중이면 실패.</summary>
    public Result Start(EngineConfig cfg)
    {
        ArgumentNullException.ThrowIfNull(cfg);
        using var s = new NativeStrings();
        cimsue_engine_config_t n;
        cimsue_engine_config_default(&n);
        n.user_agent = s.Add(cfg.UserAgent);
        n.log_level = cfg.LogLevel;
        n.tls_ca_pem = s.Add(cfg.TlsCaPem);
        n.tls_verify_server = B(cfg.TlsVerifyServer);
        n.null_audio_device = B(cfg.NullAudioDevice);
        n.no_vad = B(cfg.NoVad);
        n.udp_port = cfg.UdpPort; n.tcp_port = cfg.TcpPort; n.tls_port = cfg.TlsPort;
        n.clock_rate = cfg.ClockRate;
        n.udp_no_tcp_switch = B(cfg.UdpNoTcpSwitch);
        n.grant_mic_delay_ms = cfg.GrantMicDelayMs;
        cimsue_listener_t l = MakeListener();
        return Status(cimsue_engine_start(Handle, &n, &l));
    }

    /// <summary>모든 호·계정 정리 후 종료. 이후 Start 로 재기동 가능.</summary>
    public void Stop()
    {
        if (_h != IntPtr.Zero) cimsue_engine_stop(_h);
    }

    public void Dispose()
    {
        IntPtr h = Interlocked.Exchange(ref _h, IntPtr.Zero);
        if (h == IntPtr.Zero) return;
        cimsue_engine_destroy(h);                 // 기동 중이면 stop(콜백 완료) 후 파괴
        if (_self.IsAllocated) _self.Free();
        _accounts.Clear();
        _calls.Clear();
    }

    // ── 계정 ──

    /// <summary>계정 추가(등록은 하지 않음).</summary>
    public Result<Account> AddAccount(AccountConfig cfg)
    {
        ArgumentNullException.ThrowIfNull(cfg);
        using var s = new NativeStrings();
        cimsue_account_config_t n = ToNative(cfg, s);
        int id = cimsue_engine_add_account(Handle, &n);
        if (id < 0) return Result<Account>.Fail(-1, LastError());
        return Result<Account>.Success(GetAccount(id));
    }

    /// <summary>id 의 래퍼(같은 id 는 같은 객체).</summary>
    public Account GetAccount(int accountId) => _accounts.GetOrAdd(accountId, id => new Account(this, id));

    /// <summary>코어의 현재 계정 목록(스냅샷).</summary>
    public IReadOnlyList<Account> Accounts
    {
        get
        {
            int* ids;
            int n = cimsue_engine_accounts(Handle, &ids);
            var list = new Account[n];
            for (int i = 0; i < n; ++i) list[i] = GetAccount(ids[i]);
            return list;
        }
    }

    internal RegInfo RegInfoOf(int accountId)
    {
        cimsue_reg_info_t r;
        cimsue_engine_reg_info(Handle, accountId, &r);
        return ToManaged(r);
    }

    // ── 호 ──

    /// <summary>id 의 래퍼(같은 id 는 같은 객체 — Disconnected 이벤트 뒤 해제).</summary>
    public Call GetCall(int callId) => _calls.GetOrAdd(callId, id => new Call(this, id));

    /// <summary>코어의 현재 호 목록(스냅샷).</summary>
    public IReadOnlyList<Call> Calls
    {
        get
        {
            int* ids;
            int n = cimsue_engine_calls(Handle, &ids);
            var list = new Call[n];
            for (int i = 0; i < n; ++i) list[i] = GetCall(ids[i]);
            return list;
        }
    }

    internal CallInfo CallInfoOf(int callId)
    {
        cimsue_call_info_t c;
        cimsue_engine_call_info(Handle, callId, &c);
        return ToManaged(&c);
    }

    internal FloorInfo FloorInfoOf(int callId)
    {
        cimsue_floor_info_t f;
        cimsue_engine_floor_info(Handle, callId, &f);
        return ToManaged(&f);
    }

    internal TransmissionInfo TransmissionInfoOf(int callId)
    {
        cimsue_transmission_info_t t;
        cimsue_engine_transmission_info(Handle, callId, &t);
        return ToManaged(&t);
    }

    internal StreamStats StreamStatsOf(int callId)
    {
        cimsue_stream_stats_t s;
        cimsue_engine_stream_stats(Handle, callId, &s);
        return new StreamStats(s.rx_packets, s.rx_bytes, s.rx_loss, s.rx_discard, s.tx_packets, s.tx_bytes, s.valid != 0);
    }

    internal CallQuality CallQualityOf(int callId)
    {
        cimsue_call_quality_t q;
        cimsue_engine_call_quality(Handle, callId, &q);
        static QualityDirection Dir(in cimsue_quality_direction_t d) =>
            new(d.valid != 0, d.packets, d.lost, d.discarded, d.loss_pct, d.discard_pct, d.jitter_ms, d.jitter_max_ms,
                d.burst_density_pct, d.gap_density_pct, d.burst_ms, d.gap_ms, d.signal_dbm, d.noise_dbm);
        return new CallQuality(q.valid != 0, Utf8.Str(q.codec), q.clock_rate, q.wideband != 0, Dir(q.rx), Dir(q.remote),
                               q.rtd_ms, q.esd_ms, q.one_way_ms, q.r_lq, q.r_cq, q.mos_lq, q.mos_cq, q.start_epoch_ms, q.duration_ms);
    }

    /// <summary>SIP TLS 서버 인증서 만료 관측 — 마지막 성공 핸드셰이크의 peer 인증서(관제 요약 띠 경고의 입력, §8.6.2). 관측 전엔 Valid=false.</summary>
    public TlsPeerExpiry TlsPeerExpiry
    {
        get
        {
            cimsue_tls_peer_expiry_t t;
            cimsue_engine_tls_peer_expiry(Handle, &t);
            return ToManaged(&t);
        }
    }

    internal static TlsPeerExpiry ToManaged(cimsue_tls_peer_expiry_t* t) => t->valid == 0 ? TlsPeerExpiry.None
        : new TlsPeerExpiry(true, DateTimeOffset.FromUnixTimeSeconds(t->not_after_epoch), DateTimeOffset.FromUnixTimeSeconds(t->observed_epoch),
                            t->days_left, Utf8.Str(t->subject), Utf8.Str(t->remote));

    // ── 장치 ──

    public IReadOnlyList<AudioDeviceInfo> AudioDevices
    {
        get
        {
            cimsue_audio_device_info_t* p;
            int n = cimsue_engine_audio_devices(Handle, &p);
            var list = new AudioDeviceInfo[n];
            for (int i = 0; i < n; ++i)
                list[i] = new AudioDeviceInfo(p[i].id, Utf8.Str(p[i].name), Utf8.Str(p[i].driver), p[i].input_count, p[i].output_count);
            return list;
        }
    }

    /// <summary>장치 목록 재열거(핫플러그 뒤 — Platform.AudioEndpoints 통지에서 부른다).</summary>
    public Result RefreshAudioDevices() => Status(cimsue_engine_refresh_audio_devices(Handle));
    /// <summary>망 변경(기본 망 전환·복귀) — 코어가 TCP/TLS 연결을 닫고 등록을 켠 계정마다 재등록(Engine::handleNetworkChange). 앞 등록이 걸려 있으면 끝난 뒤 한 번 더.</summary>
    public Result HandleNetworkChange() => Status(cimsue_engine_handle_network_change(Handle));
    /// <summary>캡처/재생 장치 선택(pjmedia 장치 id). -1=기본 캡처, -2=기본 재생.</summary>
    public Result SetAudioDevices(int captureDev, int playbackDev) => Status(cimsue_engine_set_audio_devices(Handle, captureDev, playbackDev));
    /// <summary>추가 재생 라우트 — 두 번째 재생 장치를 재생 전용으로 연다(관제석 헤드셋+스피커). 반환 routeId ≥ 1. 기본 재생 장치 = 라우트 0.</summary>
    public Result<int> AddPlaybackRoute(int playbackDev)
    {
        int id = cimsue_engine_add_playback_route(Handle, playbackDev);
        return id < 0 ? Result<int>.Fail(-1, LastError()) : Result<int>.Success(id);
    }
    /// <summary>라우트 닫기. 이 라우트에 붙은 호는 라우트 0 으로 되돌아간다.</summary>
    public Result RemovePlaybackRoute(int routeId) => Status(cimsue_engine_remove_playback_route(Handle, routeId));
    /// <summary>캡처 게이트 — false = 캡처 스트림을 열지 않는다(재생만). 호 음소거·floor 게이트와 별개인 장치 단위 스위치이고 장치 선택을 넘어 유지된다.</summary>
    public Result SetCaptureEnabled(bool on) => Status(cimsue_engine_set_capture_enabled(Handle, B(on)));
    public bool CaptureEnabled => cimsue_engine_capture_enabled(Handle) != 0;
    /// <summary>장치 단 음량(ue_audio_level.md §2·§6) — speaker = 스피커 배율(1 = 원음), micTargetDbov = 마이크 AGC 목표(-40..-10, 기본
    /// <see cref="EngineConfig.MicAgcTargetDbov"/>). 코어가 기억해 게이트 전환·장치 재오픈·호 결선 뒤 다시 건다.</summary>
    public Result SetDeviceAudioLevels(float speaker, double micTargetDbov = EngineConfig.MicAgcTargetDbov) =>
        Status(cimsue_engine_set_device_audio_levels(Handle, speaker, micTargetDbov));
    /// <summary>오디오 라우트(모바일 OUTPUT/INPUT_ROUTE) — 라우트가 없는 데스크톱 장치는 무시될 수 있다.</summary>
    public Result SetAudioRoute(AudioRoute output, AudioRoute input) => Status(cimsue_engine_set_audio_route(Handle, (int)output, (int)input));
    /// <summary>사운드 장치 재오픈 — 열려 있으면 닫고 곧바로 다시 연다(재생·캡처 트랙 재생성, 브리지 결선·게이트·라우트·음량 유지). 닫혀 있으면 아무것도 하지 않는다.</summary>
    public Result ReopenAudioDevice() => Status(cimsue_engine_reopen_audio_device(Handle));

    // ── 영상 (§4.5 — 코어는 창을 열지 않는다. Windows 엔진은 디코드 프레임을 VideoFrameReceived 로 넘긴다) ──
    /// <summary>수신 영상 렌더 대상 — 플랫폼 창 핸들. IntPtr.Zero = 해제. 영상 없는 빌드면 실패. Windows 엔진(프레임 렌더)도 실패한다 —
    /// 활성 영상 호마다 프레임이 <see cref="VideoFrameReceived"/> 로 온다.</summary>
    public Result SetVideoWindow(IntPtr nativeWindow) => Status(cimsue_engine_set_video_window(Handle, nativeWindow));
    /// <summary>셀프뷰 프레임 — on 이면 내 영상을 보내는 동안 카메라 프레임을 <see cref="VideoFrameReceived"/>(CallId -1)로도 넘긴다.
    /// 카메라는 송출이 연다(셀프뷰만으로 열지 않는다). 프레임 렌더가 없는 빌드면 실패.</summary>
    public Result SetVideoPreview(bool on) => Status(cimsue_engine_set_video_preview(Handle, B(on)));
    /// <summary>캡처 카메라 선택 — <see cref="VideoDevices"/> 의 캡처 장치 Id, -1 = 기본(이름에 front, 없으면 첫 카메라). 다음 송출부터 쓰고 지금 송출 중인 호는 곧바로 바꾼다.</summary>
    public Result SetVideoCaptureDevice(int deviceId) => Status(cimsue_engine_set_video_capture_device(Handle, deviceId));
    /// <summary>캡처 카메라 전환 — 활성 영상 호의 송신 장치를 다음 카메라로.</summary>
    public Result SwitchCamera(int callId) => Status(cimsue_engine_switch_camera(Handle, callId));
    /// <summary>통화 중 영상 전환(1:1 호, re-INVITE) — on = 추가 요청(결과는 <see cref="VideoRequestChanged"/>), false = 제거(묻지 않는다).
    /// 성립 전·보류 중·진행 중인 요청이 있으면 실패. 영상 없는 빌드면 실패.</summary>
    public Result SetCallVideo(int callId, bool on) => Status(cimsue_engine_set_call_video(Handle, callId, on ? 1 : 0));
    /// <summary>상대의 영상 추가 요청에 답한다 — accept = 영상을 받는 200 OK, false = m=video port 0(음성은 그대로, RFC 3264 §6).</summary>
    public Result AnswerVideoRequest(int callId, bool accept) => Status(cimsue_engine_answer_video_request(Handle, callId, accept ? 1 : 0));
    public IReadOnlyList<VideoDeviceInfo> VideoDevices
    {
        get
        {
            cimsue_video_device_info_t* p;
            int n = cimsue_engine_video_devices(Handle, &p);
            var list = new VideoDeviceInfo[n];
            for (int i = 0; i < n; ++i)
                list[i] = new VideoDeviceInfo(p[i].id, Utf8.Str(p[i].name), Utf8.Str(p[i].driver), p[i].capture != 0, p[i].render != 0);
            return list;
        }
    }

    // ── 정적 헬퍼 (types.h 인라인 멤버 1:1 — 규칙은 코어에 하나만 둔다) ──

    internal enum AccountStringKind { Aor, McpttId, DigestUsername }

    internal static string AccountConfigString(AccountConfig cfg, AccountStringKind kind)
    {
        using var s = new NativeStrings();
        cimsue_account_config_t n = ToNative(cfg, s);
        int need = AccountString(kind, &n, null, 0);          // 길이만
        if (need <= 0) return "";
        byte[] buf = new byte[need + 1];
        fixed (byte* p = buf)
        {
            int got = AccountString(kind, &n, p, buf.Length);
            return System.Text.Encoding.UTF8.GetString(buf, 0, Math.Min(got, need));
        }
    }

    private static int AccountString(AccountStringKind kind, cimsue_account_config_t* n, byte* buf, int cap) => kind switch
    {
        AccountStringKind.Aor => cimsue_account_config_aor(n, buf, cap),
        AccountStringKind.McpttId => cimsue_account_config_mcptt_id(n, buf, cap),
        _ => cimsue_account_config_digest_username(n, buf, cap),
    };

    internal static bool AccountConfigIsComplete(AccountConfig cfg)
    {
        using var s = new NativeStrings();
        cimsue_account_config_t n = ToNative(cfg, s);
        return cimsue_account_config_is_complete(&n) != 0;
    }

    internal static string DialogJoinHeader(DialogInfo d)
    {
        using var s = new NativeStrings();
        cimsue_dialog_info_t n = ToNative(d, s);
        int need = cimsue_dialog_info_join_header(&n, null, 0);
        if (need <= 0) return "";
        byte[] buf = new byte[need + 1];
        fixed (byte* p = buf)
        {
            int got = cimsue_dialog_info_join_header(&n, p, buf.Length);
            return System.Text.Encoding.UTF8.GetString(buf, 0, Math.Min(got, need));
        }
    }

    public static string ToText(RegState s) => Utf8.Str(cimsue_reg_state_str((int)s));
    public static string ToText(CallState s) => Utf8.Str(cimsue_call_state_str((int)s));
    public static string ToText(Transport t) => Utf8.Str(cimsue_transport_str((int)t));
    public static string ToText(FloorState s) => Utf8.Str(cimsue_floor_state_str((int)s));
    public static string ToText(FloorEventKind k) => Utf8.Str(cimsue_floor_kind_str((int)k));
    public static string ToText(ConditionCause c) => Utf8.Str(cimsue_condition_cause_str((int)c));

    /// <summary>REGISTER User-Agent 규약 `&lt;제품&gt;/&lt;앱 버전&gt; (&lt;OS&gt;; &lt;모델&gt;)`(mcptt_management_views.md §4.1) — 코어 userAgentOf
    /// (괄호·역슬래시·제어 문자 정리 포함). Windows 기기 값을 채운 결과는 <see cref="Platform.DeviceIdentity.UserAgent"/>.</summary>
    public static string UserAgentOf(string product, string version, string os, string model) =>
        CopyOutString((p, cap) => cimsue_user_agent_of(product, version, os, model, p, cap));
    /// <summary>IMEI → RFC 7254 instance URN(`urn:gsma:imei:TTTTTTTT-SSSSSS-0` — 셋째 칸은 spare 0) — 자릿수·Luhn 검사 숫자가 틀리면 빈 문자열.</summary>
    public static string ImeiUrn(string imei) => CopyOutString((p, cap) => cimsue_imei_urn(imei, p, cap));

    private delegate int CopyOutFn(byte* buf, int cap);
    /// <summary>문자열 산출 헬퍼 규약(길이만 → 버퍼) 공통.</summary>
    private static string CopyOutString(CopyOutFn fn)
    {
        int need = fn(null, 0);
        if (need <= 0) return "";
        byte[] buf = new byte[need + 1];
        fixed (byte* p = buf)
        {
            int got = fn(p, buf.Length);
            return System.Text.Encoding.UTF8.GetString(buf, 0, Math.Min(got, need));
        }
    }

    // ── 내부 공통 ──

    internal static int B(bool b) => b ? 1 : 0;
    internal static string LastError() => Utf8.Str(cimsue_last_error());
    internal static Result Status(int st) => st == 0 ? Result.Success : new Result(st, LastError());
    internal Result<Call> CallResult(int id) => id < 0 ? Result<Call>.Fail(-1, LastError()) : Result<Call>.Success(GetCall(id));

    internal static cimsue_account_config_t ToNative(AccountConfig a, NativeStrings s)
    {
        cimsue_account_config_t n;
        cimsue_account_config_default(&n);
        n.server_host = s.Add(a.ServerHost);
        n.server_port = a.ServerPort;
        n.transport = (int)a.Transport;
        n.domain = s.Add(a.Domain);
        n.msisdn = s.Add(a.Msisdn);
        n.imsi = s.Add(a.Imsi);
        n.auth_id = s.Add(a.AuthId);
        n.display_name = s.Add(a.DisplayName);
        n.ha1 = s.Add(a.Ha1);
        n.password = s.Add(a.Password);
        n.auth_scheme = (int)a.AuthScheme;
        n.aka_k = s.Add(a.AkaK);
        n.aka_opc = s.Add(a.AkaOpc);
        n.aka_amf = s.Add(a.AkaAmf);
        n.sec_mechanisms = s.AddArray(a.SecMechanisms, out n.sec_mechanism_count);
        n.media_security = (int)a.MediaSecurity;
        n.expires_sec = a.ExpiresSec;
        n.contact_params = s.Add(a.ContactParams);
        n.video_auto_transmit = B(a.VideoAutoTransmit);
        n.mcptt_id = s.Add(a.McpttId);
        n.auto_answer_mcptt = B(a.AutoAnswerMcptt);
        n.instance_id = s.Add(a.InstanceId);
        n.mcptt_client_id = s.Add(a.McpttClientId);
        n.rp_emergency = s.Add(a.RpEmergency);
        n.rp_imminent_peril = s.Add(a.RpImminentPeril);
        n.rp_normal = s.Add(a.RpNormal);
        n.max_sds_cplane_bytes = a.MaxSdsCplaneBytes;
        n.mcdata_msrp = B(a.McdataMsrp);
        n.mcptt_server_uri = s.Add(a.McpttServerUri);
        n.mcdata_server_uri = s.Add(a.McdataServerUri);
        n.mcvideo_enabled = B(a.McvideoEnabled);
        n.mcvideo_server_uri = s.Add(a.McvideoServerUri);
        n.auto_answer_mcvideo = B(a.AutoAnswerMcvideo);
        return n;
    }

    /// <summary>산출 AccountConfig(to_account) → 관리. 빈 문자열은 null(코어 기본값)로 — 다시 넣어도 같은 뜻이다.</summary>
    internal static AccountConfig FromNative(cimsue_account_config_t* n)
    {
        static string? Opt(byte* p) { string s = Utf8.Str(p); return s.Length == 0 ? null : s; }
        var sec = new string[Math.Max(0, n->sec_mechanism_count)];
        for (int i = 0; i < sec.Length; ++i) sec[i] = Utf8.Str(n->sec_mechanisms[i]);
        return new AccountConfig
        {
            ServerHost = Opt(n->server_host), ServerPort = n->server_port, Transport = (Transport)n->transport,
            Domain = Opt(n->domain), Msisdn = Opt(n->msisdn), Imsi = Opt(n->imsi), AuthId = Opt(n->auth_id),
            DisplayName = Opt(n->display_name), Ha1 = Opt(n->ha1), Password = Opt(n->password),
            AuthScheme = (AuthScheme)n->auth_scheme, AkaK = Opt(n->aka_k), AkaOpc = Opt(n->aka_opc), AkaAmf = Opt(n->aka_amf),
            SecMechanisms = sec.Length == 0 ? null : sec, MediaSecurity = (MediaSecurity)n->media_security,
            ExpiresSec = n->expires_sec, ContactParams = Opt(n->contact_params), VideoAutoTransmit = n->video_auto_transmit != 0,
            McpttId = Opt(n->mcptt_id), AutoAnswerMcptt = n->auto_answer_mcptt != 0, InstanceId = Opt(n->instance_id),
            McpttClientId = Opt(n->mcptt_client_id), RpEmergency = Opt(n->rp_emergency), RpImminentPeril = Opt(n->rp_imminent_peril),
            RpNormal = Opt(n->rp_normal), MaxSdsCplaneBytes = n->max_sds_cplane_bytes, McdataMsrp = n->mcdata_msrp != 0,
            McpttServerUri = Opt(n->mcptt_server_uri), McdataServerUri = Opt(n->mcdata_server_uri),
            McvideoEnabled = n->mcvideo_enabled != 0, McvideoServerUri = Opt(n->mcvideo_server_uri),
            AutoAnswerMcvideo = n->auto_answer_mcvideo != 0,
        };
    }

    internal static cimsue_dialog_info_t ToNative(DialogInfo d, NativeStrings s) => new()
    {
        account_id = d.AccountId,
        watched = s.Add(d.Watched), id = s.Add(d.Id), call_id = s.Add(d.CallId),
        local_tag = s.Add(d.LocalTag), remote_tag = s.Add(d.RemoteTag),
        direction = s.Add(d.Direction), state = s.Add(d.State), remote_identity = s.Add(d.RemoteIdentity),
        full = B(d.Full),
    };

    internal static RegInfo ToManaged(in cimsue_reg_info_t r) =>
        new(r.account_id, (RegState)r.state, r.code, Utf8.Str(r.reason), r.expires_sec);

    internal static McpttInfo ToManaged(in cimsue_mcptt_info_t m) =>
        new(m.present != 0, Utf8.Str(m.session_type), Utf8.Str(m.request_uri), Utf8.Str(m.calling_user_id),
            Utf8.Str(m.calling_group_id), m.emergency != 0, m.imminent_peril != 0, m.private_call != 0, m.no_floor_ctrl != 0,
            m.broadcast != 0);

    internal static CallInfo ToManaged(cimsue_call_info_t* c)
    {
        var src = new MediaSource[Math.Max(0, c->source_count)];
        for (int i = 0; i < src.Length; ++i)
            src[i] = new MediaSource(c->sources[i].ssrc, Utf8.Str(c->sources[i].label), c->sources[i].active != 0, c->sources[i].level);
        return new CallInfo(c->call_id, c->account_id, (CallDir)c->dir, (CallState)c->state, Utf8.Str(c->remote_uri),
                            Utf8.Str(c->called_party), c->video != 0, c->media_active != 0, c->muted != 0, c->listen != 0,
                            c->playback_route, c->last_code, Utf8.Str(c->last_reason), src, c->is_mcptt != 0, Utf8.Str(c->group_id),
                            ToManaged(c->mcptt), c->half_duplex != 0, c->listen_only != 0, Utf8.Str(c->joined_dialog),
                            c->rx_level, new McpttCondition(c->condition.emergency != 0, c->condition.imminent_peril != 0, c->condition.mine != 0,
                                                            c->condition.pending != 0, c->condition.last_code),
                            Utf8.Str(c->answer_state), NonAck(c), (McService)c->service, Utf8.Str(c->session_uri), c->video_send != 0,
                            (VideoRequestState)c->video_request);
    }

    private static string[] NonAck(cimsue_call_info_t* c)
    {
        var a = new string[Math.Max(0, c->non_ack_user_count)];
        for (int i = 0; i < a.Length; ++i) a[i] = Utf8.Str(c->non_ack_users[i]);
        return a;
    }

    internal static EmergencyAlert ToManaged(cimsue_emergency_alert_t* a) =>
        new(a->account_id, Utf8.Str(a->group_id), Utf8.Str(a->user_id), Utf8.Str(a->originated_by), Utf8.Str(a->mc_org),
            a->alert_ind, a->emergency_ind, a->imminent_peril_ind, a->self != 0);

    internal static Talker[] ToManaged(cimsue_talker_t* t, int n)
    {
        var arr = new Talker[Math.Max(0, n)];
        for (int i = 0; i < arr.Length; ++i) arr[i] = new Talker(Utf8.Str(t[i].id), t[i].ssrc, t[i].self != 0);
        return arr;
    }

    internal static FloorEvent ToManaged(cimsue_floor_event_t* f) =>
        new((FloorEventKind)f->kind, f->call_id, (FloorState)f->state, f->duration_sec, f->cause, Utf8.Str(f->cause_text),
            f->indicator, f->permission, f->queue_position, f->me_speaking != 0, ToManaged(f->talkers, f->talker_count), f->raw_type);

    internal static FloorInfo ToManaged(cimsue_floor_info_t* f) =>
        new((FloorState)f->state, ToManaged(f->talkers, f->talker_count), f->can_request != 0, f->indicator, f->queue_position,
            f->local_port, Utf8.Str(f->remote_ip), f->remote_port, f->granted_count, f->taken_count, f->deny_count);

    internal static VideoTransmitter ToManaged(in cimsue_video_transmitter_t t) =>
        new(Utf8.Str(t.user_id), t.audio_ssrc, t.video_ssrc, Utf8.Str(t.functional_alias), t.automatic != 0, (ReceptionState)t.state);

    internal static TransmissionEvent ToManaged(cimsue_transmission_event_t* e) =>
        new((TransmissionEventKind)e->kind, e->call_id, (TransmissionState)e->state, e->cause, Utf8.Str(e->cause_text), e->duration_sec,
            e->priority, e->queue_position, e->indicator, e->audio_ssrc, e->video_ssrc, Utf8.Str(e->receiver_id), e->raw_type);

    internal static ReceptionEvent ToManaged(cimsue_reception_event_t* e) =>
        new((ReceptionEventKind)e->kind, e->call_id, ToManaged(e->transmitter), e->cause, Utf8.Str(e->cause_text), e->raw_type);

    internal static VideoRequestEvent ToManaged(cimsue_video_request_event_t* e) =>
        new((VideoRequestEventKind)e->kind, e->call_id, e->code, Utf8.Str(e->reason));

    internal static TransmissionInfo ToManaged(cimsue_transmission_info_t* t)
    {
        var arr = new VideoTransmitter[Math.Max(0, t->transmitter_count)];
        for (int i = 0; i < arr.Length; ++i) arr[i] = ToManaged(t->transmitters[i]);
        return new TransmissionInfo((TransmissionState)t->state, arr, t->queue_position, t->local_port, Utf8.Str(t->remote_ip), t->remote_port);
    }

    internal static RequestResult ToManaged(cimsue_request_result_t* r) =>
        new(r->account_id, r->token, Utf8.Str(r->method), r->code, Utf8.Str(r->reason), Utf8.Str(r->etag));

    internal static DialogInfo ToManaged(cimsue_dialog_info_t* d) =>
        new(d->account_id, Utf8.Str(d->watched), Utf8.Str(d->id), Utf8.Str(d->call_id), Utf8.Str(d->local_tag),
            Utf8.Str(d->remote_tag), Utf8.Str(d->direction), Utf8.Str(d->state), Utf8.Str(d->remote_identity), d->full != 0);

    internal static SdsMessage ToManaged(cimsue_sds_message_t* m) =>
        new(m->account_id, Utf8.Str(m->from_uri), Utf8.Str(m->group_uri), Utf8.Str(m->conv_id), Utf8.Str(m->msg_id), m->time_sec,
            m->disposition_req, Utf8.Str(m->text), m->notification != 0, m->notif_type, m->fd != 0, Utf8.Str(m->file_url),
            Utf8.Str(m->file_name), Utf8.Str(m->file_type), m->file_size, m->media_plane != 0);

    // ── 콜백 → 이벤트 ──

    private cimsue_listener_t MakeListener() => new()
    {
        user = (void*)GCHandle.ToIntPtr(_self),
        on_log = &Cb.OnLog,
        on_reg_state = &Cb.OnRegState,
        on_incoming_call = &Cb.OnIncomingCall,
        on_call_state = &Cb.OnCallState,
        on_call_media = &Cb.OnCallMedia,
        on_floor = &Cb.OnFloor,
        on_roster = &Cb.OnRoster,
        on_dialog_info = &Cb.OnDialogInfo,
        on_sds = &Cb.OnSds,
        on_request_result = &Cb.OnRequestResult,
        on_message = &Cb.OnMessage,
        on_engine_stopped = &Cb.OnEngineStopped,
        on_mcptt_condition = &Cb.OnMcpttCondition,
        on_emergency_alert = &Cb.OnEmergencyAlert,
        on_non_acknowledged_users = &Cb.OnNonAcknowledgedUsers,
        on_transmission = &Cb.OnTransmission,
        on_reception = &Cb.OnReception,
        on_video_frame = &Cb.OnVideoFrame,
        on_video_request = &Cb.OnVideoRequest,
    };

    /// <summary>앱 스레드로 넘긴다. 컨텍스트가 없으면 이벤트 스레드에서 직접 — 예외는 네이티브 경계 밖으로 새지 않게 잡는다.</summary>
    private void Dispatch(Action a)
    {
        if (_sync is not null) { _sync.Post(static o => ((Action)o!)(), a); return; }
        try { a(); }
        catch (Exception ex) { try { HandlerFailed?.Invoke(this, ex); } catch { /* 마지막 방어 */ } }
    }

    /// <summary>코어 이벤트 스레드에서 호출되는 진입점. 인자 문자열은 콜백 동안만 유효하므로 여기서 전부 복사한다.</summary>
    private static class Cb
    {
        private static Engine? Of(void* user)
        {
            try { return GCHandle.FromIntPtr((IntPtr)user).Target as Engine; }
            catch { return null; }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnLog(void* user, int level, byte* msg)
        {
            var e = Of(user); if (e is null || e.Log is null) return;
            try { var line = new LogLine(level, Utf8.Str(msg)); e.Dispatch(() => e.Log?.Invoke(e, line)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnRegState(void* user, cimsue_reg_info_t* info)
        {
            var e = Of(user); if (e is null) return;
            try { var r = ToManaged(*info); e.Dispatch(() => e.RegistrationChanged?.Invoke(e, r)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnIncomingCall(void* user, cimsue_call_info_t* info)
        {
            var e = Of(user); if (e is null) return;
            try { var c = ToManaged(info); e.GetCall(c.CallId); e.Dispatch(() => e.IncomingCall?.Invoke(e, c)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnCallState(void* user, cimsue_call_info_t* info)
        {
            var e = Of(user); if (e is null) return;
            try
            {
                var c = ToManaged(info);
                e.Dispatch(() =>
                {
                    e.CallStateChanged?.Invoke(e, c);
                    if (c.State == CallState.Disconnected) e._calls.TryRemove(c.CallId, out _);
                });
            }
            catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnCallMedia(void* user, cimsue_call_info_t* info)
        {
            var e = Of(user); if (e is null) return;
            try { var c = ToManaged(info); e.Dispatch(() => e.CallMediaChanged?.Invoke(e, c)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnFloor(void* user, cimsue_floor_event_t* ev)
        {
            var e = Of(user); if (e is null) return;
            try { var f = ToManaged(ev); e.Dispatch(() => e.FloorChanged?.Invoke(e, f)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnRoster(void* user, int accountId, byte* groupId, cimsue_roster_entry_t* users, int count, int full)
        {
            var e = Of(user); if (e is null) return;
            try
            {
                var list = new RosterEntry[Math.Max(0, count)];
                for (int i = 0; i < list.Length; ++i) list[i] = new RosterEntry(Utf8.Str(users[i].uri), Utf8.Str(users[i].status));
                var r = new RosterUpdate(accountId, Utf8.Str(groupId), list, full != 0);
                e.Dispatch(() => e.RosterChanged?.Invoke(e, r));
            }
            catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnDialogInfo(void* user, cimsue_dialog_info_t* d)
        {
            var e = Of(user); if (e is null) return;
            try { var m = ToManaged(d); e.Dispatch(() => e.DialogInfoReceived?.Invoke(e, m)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnSds(void* user, cimsue_sds_message_t* msg)
        {
            var e = Of(user); if (e is null) return;
            try { var m = ToManaged(msg); e.Dispatch(() => e.SdsReceived?.Invoke(e, m)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnRequestResult(void* user, cimsue_request_result_t* r)
        {
            var e = Of(user); if (e is null) return;
            try { var m = ToManaged(r); e.Dispatch(() => e.RequestCompleted?.Invoke(e, m)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnMessage(void* user, int accountId, byte* from, byte* contentType, byte* body)
        {
            var e = Of(user); if (e is null) return;
            try
            {
                var m = new SipMessage(accountId, Utf8.Str(from), Utf8.Str(contentType), Utf8.Str(body));
                e.Dispatch(() => e.MessageReceived?.Invoke(e, m));
            }
            catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnMcpttCondition(void* user, cimsue_call_info_t* info, int cause)
        {
            var e = Of(user); if (e is null) return;
            try { var m = new McpttConditionChange(ToManaged(info), (ConditionCause)cause); e.Dispatch(() => e.McpttConditionChanged?.Invoke(e, m)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnEmergencyAlert(void* user, cimsue_emergency_alert_t* alert)
        {
            var e = Of(user); if (e is null) return;
            try { var a = ToManaged(alert); e.Dispatch(() => e.EmergencyAlertReceived?.Invoke(e, a)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnNonAcknowledgedUsers(void* user, cimsue_call_info_t* info)
        {
            var e = Of(user); if (e is null) return;
            try { var c = ToManaged(info); e.Dispatch(() => e.NonAcknowledgedUsersReceived?.Invoke(e, c)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnTransmission(void* user, cimsue_transmission_event_t* ev)
        {
            var e = Of(user); if (e is null) return;
            try { var t = ToManaged(ev); e.Dispatch(() => e.TransmissionChanged?.Invoke(e, t)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnReception(void* user, cimsue_reception_event_t* ev)
        {
            var e = Of(user); if (e is null) return;
            try { var r = ToManaged(ev); e.Dispatch(() => e.ReceptionChanged?.Invoke(e, r)); } catch { }
        }

        /// <summary>영상 스레드 — 복사·마샬링 없이 핸들러에 넘긴다(VideoFrame.Pixels 는 이 호출 동안만 유효).</summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnVideoFrame(void* user, cimsue_video_frame_t* f)
        {
            var e = Of(user); var h = e?.VideoFrameReceived; if (e is null || h is null || f is null || f->data is null) return;
            try
            {
                var frame = new VideoFrame(f->call_id, f->width, f->height, f->stride, new ReadOnlySpan<byte>(f->data, (int)f->size));
                h(e, in frame);
            }
            catch (Exception ex) { try { e.HandlerFailed?.Invoke(e, ex); } catch { /* 마지막 방어 */ } }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnVideoRequest(void* user, cimsue_video_request_event_t* ev)
        {
            var e = Of(user); if (e is null) return;
            try { var r = ToManaged(ev); e.Dispatch(() => e.VideoRequestChanged?.Invoke(e, r)); } catch { }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void OnEngineStopped(void* user)
        {
            var e = Of(user); if (e is null) return;
            try { e.Dispatch(() => e.Stopped?.Invoke(e, EventArgs.Empty)); } catch { }
        }
    }
}
