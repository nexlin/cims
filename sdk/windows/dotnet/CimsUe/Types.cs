// CimsUe — 공개 타입 (cimsue/types.h 1:1, ue_sdk.md §4.2·§6.4)
//
// 열거형 값은 C++/C API 와 같은 정수다(바인딩이 정수를 그대로 넘긴다). 설정(EngineConfig·AccountConfig·옵션)은 입력이라
// 변경 가능한 클래스, 상태·이벤트(RegInfo·CallInfo·FloorEvent …)는 코어 스냅샷의 복사본이라 불변 record 다.
// 설정의 문자열 null 은 "코어 기본값 유지", 빈 문자열은 "지움" — C API default() 규약과 같다.
namespace CimsUe;

public enum Transport { Udp = 0, Tcp = 1, Tls = 2 }
public enum AuthScheme { Digest = 0, Aka = 1 }
/// <summary>미디어 SRTP(SDES) 정책 — 접속서비스 media_srtp 와 같은 값.</summary>
public enum MediaSecurity { Off = 0, Optional = 1, Required = 2 }
public enum RegState { Unregistered = 0, Registering = 1, Registered = 2, Failed = 3 }
public enum CallState { Null = 0, Outgoing = 1, Incoming = 2, Active = 3, Held = 4, Disconnected = 5 }
public enum CallDir { Outgoing = 0, Incoming = 1 }
/// <summary>floor participant 상태 (TS 24.380).</summary>
public enum FloorState { Idle = 0, Requesting = 1, Speaking = 2, Listening = 3, Queued = 4 }
public enum FloorEventKind
{
    Granted = 0, Denied = 1, Idle = 2, Taken = 3, TalkerLeft = 4, Revoked = 5, QueuePosition = 6, QueueCancelled = 7,
    /// <summary>요청 후 응답 없음(코어 타이머) → Idle 복귀.</summary>
    RequestTimeout = 8,
    /// <summary>Granted Duration 마감 임박/도달 — 코어가 스스로 Release.</summary>
    TalkLimit = 9,
    Other = 10,
}

/// <summary><see cref="Engine.McpttConditionChanged"/> 의 계기(types.h ConditionCause).</summary>
public enum ConditionCause
{
    /// <summary>SetCondition — 보내면서 곧바로 반영(응답 전).</summary>
    Local = 0,
    /// <summary>그 re-INVITE 의 2xx.</summary>
    Confirmed = 1,
    /// <summary>그 re-INVITE 의 4xx~6xx — 이전 값으로 되돌렸다(미인가 상향 = 403, TS 24.379 §6.3.3.1.14).</summary>
    Denied = 2,
    /// <summary>서버 재광고(수신 re-INVITE·합류 200 OK 의 mcptt-info, TS 24.379 §6.3.3.1.6·§6.3.3.1.10·§6.3.3.1.15).</summary>
    Advertised = 3,
}

/// <summary>오디오 라우트(types.h AudioRoute) — 모바일 라우트. 데스크톱 장치는 무시할 수 있다.</summary>
public enum AudioRoute { Default = 0, Earpiece = 1, Loudspeaker = 2 }

/// <summary>MC 서비스 — 호·affiliation 은 서비스마다 따로다(TS 23.280 §5.2.5). MCVideo 그룹 호는 MCPTT 호와 독립 다이얼로그.</summary>
public enum McService { Mcptt = 0, McVideo = 1 }
/// <summary>MCVideo 내 송출 상태(TS 24.581 §6.2.4 'U: …').</summary>
public enum TransmissionState { NoPermission = 0, PendingRequest = 1, Permitted = 2, PendingEnd = 3, Queued = 4 }
/// <summary>한 송출의 내 수신 상태(§6.2.5).</summary>
public enum ReceptionState { Notified = 0, PendingRequest = 1, Receiving = 2, PendingRelease = 3, Ended = 4 }
public enum TransmissionEventKind
{
    Granted = 0, Rejected = 1, Revoked = 2, QueuePosition = 3, EndRequested = 4, Ended = 5, ReceiverJoined = 6, Idle = 7,
    QueueCancelled = 8, RequestTimeout = 9, Other = 10
}
public enum ReceptionEventKind { Notified = 0, Granted = 1, Rejected = 2, Ended = 3, Released = 4, EndRequested = 5, RequestTimeout = 6, Other = 7 }

/// <summary>Floor Indicator 비트(TS 24.380 §8.2.3.15) — <see cref="FloorEvent.Indicator"/>·<see cref="FloorInfo.Indicator"/> 해석용.
/// 정본 = docs/design/features/mcptt_floor_defs.yaml `indicator` — scripts/gen_floor_defs.py --check 가 이 값을 대조한다.</summary>
public static class FloorIndicator
{
    public const int Normal = 0x8000;
    /// <summary>B-bit — 일제 통화(TS 24.379 §4.12). 서버가 이 세션의 floor 메시지 전부에 싣는다 — 늦게 합류한 leg 도 이것으로 안다.</summary>
    public const int BroadcastGroup = 0x4000;
    public const int System = 0x2000;
    public const int Emergency = 0x1000;
    public const int ImminentPeril = 0x0800;
    public const int Queueing = 0x0400;
    public const int DualFloor = 0x0200;
    public const int TemporaryGroup = 0x0100;
    public const int MultiTalker = 0x0080;
}

/// <summary>명령의 즉시 결과(C++ Result). 0 = 성공, 음수 = 코어 오류, 양수 = pjsua/HTTP 상태. 프로토콜 결과는 이벤트로 온다.</summary>
public readonly record struct Result(int Code, string Reason)
{
    public bool Ok => Code == 0;
    public static Result Success { get; } = new(0, "");
    public static Result Fail(int code, string reason) => new(code == 0 ? -1 : code, reason);
    public override string ToString() => Ok ? "ok" : $"{Code} {Reason}";
}

/// <summary>값을 함께 돌려주는 명령의 즉시 결과 — 실패면 Value 는 기본값.</summary>
public readonly record struct Result<T>(int Code, string Reason, T Value)
{
    public bool Ok => Code == 0;
    public static Result<T> Success(T value) => new(0, "", value);
    public static Result<T> Fail(int code, string reason) => new(code == 0 ? -1 : code, reason, default!);
    public Result WithoutValue() => new(Code, Reason);
    public override string ToString() => Ok ? $"ok {Value}" : $"{Code} {Reason}";
}

/// <summary>엔진(프로세스당 1개) 설정.</summary>
public sealed class EngineConfig
{
    public string? UserAgent { get; set; }
    /// <summary>pjsip 로그 레벨 0~6 → <see cref="Engine.Log"/>.</summary>
    public int LogLevel { get; set; } = 4;
    /// <summary>SIP TLS·HTTPS 공용 신뢰 앵커(PEM). null = 시스템 기본.</summary>
    public string? TlsCaPem { get; set; }
    public bool TlsVerifyServer { get; set; } = true;
    /// <summary>오디오 장치 없이 동작(헤드리스 — 단위시험·CI).</summary>
    public bool NullAudioDevice { get; set; }
    /// <summary>VAD(무음 억제) 비활성 — 침묵 중에도 RTP 연속 송신.</summary>
    public bool NoVad { get; set; } = true;
    public int UdpPort { get; set; }
    public int TcpPort { get; set; }
    public int TlsPort { get; set; }
    /// <summary>미디어 클럭 — pjsua 기본 16kHz(AMR-WB 정합).</summary>
    public uint ClockRate { get; set; } = 16000;
    /// <summary>RFC 3261 §18.1.1 UDP→TCP 자동 승격 비활성 — 통제된 망 전용 사이트 옵션(프로파일 <see cref="ServiceProfile.UdpNoTcpSwitch"/>). 프로세스 전역.</summary>
    public bool UdpNoTcpSwitch { get; set; }
    /// <summary>Floor Granted 뒤 마이크를 여는 지연(ms, 0 = 즉시) — 앱이 승인 톤을 재생하는 길이. 그 사이 발언을 잃으면 열지 않는다.</summary>
    public int GrantMicDelayMs { get; set; }
    /// <summary>마이크 AGC 기본 목표 — ITU-T P.56 활성 레벨 -26 dBov(types.h kMicAgcTargetDbov).</summary>
    public const double MicAgcTargetDbov = -26.0;
}

/// <summary>계정(접속서비스 kind 당 1개) 설정 — 프로비저닝 프로파일(<see cref="ServiceProfile.ToAccountConfig"/>)에서 채운다.</summary>
public sealed class AccountConfig
{
    public string? ServerHost { get; set; }
    public int ServerPort { get; set; } = 5060;
    public Transport Transport { get; set; } = Transport.Udp;
    public string? Domain { get; set; }
    public string? Msisdn { get; set; }
    public string? Imsi { get; set; }
    /// <summary>전체 IMPI 직접 지정. null 이면 imsi@domain 합성.</summary>
    public string? AuthId { get; set; }
    public string? DisplayName { get; set; }
    /// <summary>MD5(IMPI:realm:pw) hex32 — 평문보다 우선.</summary>
    public string? Ha1 { get; set; }
    public string? Password { get; set; }
    public AuthScheme AuthScheme { get; set; } = AuthScheme.Digest;
    public string? AkaK { get; set; }
    public string? AkaOpc { get; set; }
    /// <summary>null = 코어 기본 "8000".</summary>
    public string? AkaAmf { get; set; }
    /// <summary>서버 제시 채널 보호 목록(RFC 3329). null = 없음.</summary>
    public IReadOnlyList<string>? SecMechanisms { get; set; }
    public MediaSecurity MediaSecurity { get; set; } = MediaSecurity.Off;
    public int ExpiresSec { get; set; } = 3600;
    /// <summary>REGISTER Contact 부가 파라미터(feature tag 등).</summary>
    public string? ContactParams { get; set; }
    public bool VideoAutoTransmit { get; set; }
    /// <summary>MCPTT ID (TS 24.379). null 이면 "tel:"+msisdn.</summary>
    public string? McpttId { get; set; }
    /// <summary>MCPTT 착신 INVITE 자동 수락 — PTT 단말 기본 동작.</summary>
    public bool AutoAnswerMcptt { get; set; } = true;
    /// <summary>REGISTER Contact +sip.instance(TS 24.229 §5.1.1.2) — 꺾쇠 없는 URN. IMEI 를 못 얻는 데스크톱은 설치별 고유
    /// "urn:uuid:…"(RFC 4122)를 저장해 두고 쓴다. null 이면 pjsip 기본값(호스트명 해시 — 기기마다 같을 수 있다).</summary>
    public string? InstanceId { get; set; }
    /// <summary>MCPTT client ID(TS 24.379 §4.10) — 긴급 경보 &lt;mcptt-client-id&gt;. null 이면 InstanceId 가 urn:uuid: 일 때 그것.</summary>
    public string? McpttClientId { get; set; }
    /// <summary>Resource-Priority r-value(긴급·임박·일반 그룹콜, TS 24.379 §6.2.8.1) — null = 코어 기본(mcpttp.15/8/0). 정본은 service-config.</summary>
    public string? RpEmergency { get; set; }
    public string? RpImminentPeril { get; set; }
    public string? RpNormal { get; set; }
    /// <summary>그룹 SDS 시그널링 평면 상한(octet) — 넘으면 media plane(MSRP, TS 24.282 §9.2.3)으로 가고 최종 결과는 RequestCompleted method "MSRP".
    /// 0 = 제한 없음. 프로파일(<see cref="ServiceProfile.MaxPayloadSdsCplaneBytes"/>)이 채운다.</summary>
    public int MaxSdsCplaneBytes { get; set; }
    /// <summary>서버발 MSRP 배포 수신 — REGISTER Contact 에 ICSI mcdata.sds. false 면 서버가 큰 그룹 SDS 를 FILEURL(FD)로 폴백한다.</summary>
    public bool McdataMsrp { get; set; }
    /// <summary>참여 MCPTT 기능 PSI — 긴급 경보 Request-URI(TS 24.379 §12.1.1.1 8)). null 이면 그룹 URI(옛 서버 전환기).
    /// 정본 = ue-init-config <see cref="UeInitConfigDoc.McpttServerUri"/>(TS 24.484 §7.2).</summary>
    public string? McpttServerUri { get; set; }
    /// <summary>참여 MCData 기능 PSI — SDS disposition 통지 Request-URI(TS 24.282 §12.2.1.1). null 이면 원 발신자 AoR 직행(CSP 0.2.180 전
    /// 서버 전환기). 정본 = ue-init-config <see cref="UeInitConfigDoc.McdataServerUri"/>.</summary>
    public string? McdataServerUri { get; set; }
    /// <summary>MCVideo 서비스 사용 — REGISTER Contact 에 MCVideo 태그(TS 24.281 §7.2.1AA). 빼고 다시 등록하면 MCVideo 로그오프.</summary>
    public bool McvideoEnabled { get; set; }
    /// <summary>참여 MCVideo 기능 PSI — MCVideo 그룹 호·affiliation Request-URI. 정본 = ue-init-config <see cref="UeInitConfigDoc.McvideoServerUri"/>.</summary>
    public string? McvideoServerUri { get; set; }
    /// <summary>MCVideo 멤버 초대(prearranged) 자동 수락(§6.2.3.1.2). 수락은 세션 합류일 뿐 — 영상 보기는 수신 제어(AcceptReception).</summary>
    public bool AutoAnswerMcvideo { get; set; } = true;

    /// <summary>"sip:msisdn@domain".</summary>
    public string Aor() => Engine.AccountConfigString(this, Engine.AccountStringKind.Aor);
    /// <summary>비면 "tel:"+msisdn.</summary>
    public string EffectiveMcpttId() => Engine.AccountConfigString(this, Engine.AccountStringKind.McpttId);
    /// <summary>Digest username = 전체 IMPI. msisdn 폴백 없음(서버는 불일치 시 즉시 403).</summary>
    public string DigestUsername() => Engine.AccountConfigString(this, Engine.AccountStringKind.DigestUsername);
    /// <summary>등록에 필요한 필드(접속점·도메인·번호·IMPI·자격)가 다 있는가 — 코어 규칙.</summary>
    public bool IsComplete() => Engine.AccountConfigIsComplete(this);
}

public sealed class CallOptions
{
    public bool Video { get; set; }
    public bool Emergency { get; set; }
}

/// <summary>그룹콜/사설콜(MCPTT) 개시 옵션 (TS 24.379).</summary>
public sealed class GroupCallOptions
{
    public bool Emergency { get; set; }
    public bool ImminentPeril { get; set; }
    /// <summary>청취 전용 합류(a=recvonly) — 관제 PTT 청취. floor 요청 불가.</summary>
    public bool ListenOnly { get; set; }
    /// <summary>전이중 1:1(mc_no_floor_ctrl). StartPrivateCall 전용.</summary>
    public bool FullDuplex { get; set; }
    /// <summary>애드혹 임시 그룹 멤버(tel: URI). JoinGroupCall 전용.</summary>
    public IReadOnlyList<string>? Members { get; set; }
    /// <summary>일제 통화 개시(mcptt-info broadcast-ind, TS 24.379 §4.12). 개시자만 발언하고, 발언을 놓은 뒤 서버의
    /// Floor Idle(B-bit)을 받으면 코어가 호를 해제한다. JoinGroupCall 전용.</summary>
    public bool Broadcast { get; set; }
    /// <summary>암묵적 발언 요청(TS 24.380 §14.2.5 mc_implicit_request + §14.2.4 mc_granted) — 개시 INVITE 가 발언 요청을 싣는다.
    /// floor 는 호 성립 전부터 Requesting, 200 OK 의 mc_granted 나 Floor Granted 로 Speaking. 승인·성립 전에 FloorRelease 하면
    /// 발언권을 돌려준다. 누르는 동안 개시하고 말하는 한 버튼 발신(일제 통화)용.</summary>
    public bool ImplicitFloorRequest { get; set; }
}

/// <summary>MCVideo 그룹 호 개시·합류 옵션(TS 24.281 §9.2.1 prearranged · §9.2.2 chat, 제어 채널 fmtp TS 24.581 §14.2).</summary>
public sealed class VideoGroupCallOptions
{
    /// <summary>session-type prearranged(false = chat). 그룹 문서 mcvideo-on-network-invite-members 와 맞아야 한다(어긋나면 404 Warning 117·118).</summary>
    public bool Prearranged { get; set; }
    /// <summary>송출 요청 대기열 지원(mc_queueing).</summary>
    public bool Queueing { get; set; }
    /// <summary>요청할 최대 송출 우선순위 1~255(mc_priority), &lt;0 = 미기재.</summary>
    public int MaxPriority { get; set; } = -1;
    /// <summary>요청할 최대 수신 우선순위 1~255(mc_reception_priority), &lt;0 = 미기재.</summary>
    public int MaxReceptionPriority { get; set; } = -1;
    /// <summary>호 성립과 함께 송출 요청(mc_implicit_request + mc_granted). 서버가 받지 않으면 코어가 명시 요청으로 잇는다.</summary>
    public bool ImplicitTransmissionRequest { get; set; }
    /// <summary>진행 중 세션 재합류(§9.2.1.2.4) — 앞 호의 <see cref="CallInfo.SessionUri"/>. null = 새 합류.</summary>
    public string? SessionUri { get; set; }
}

public sealed record RegInfo(int AccountId, RegState State, int Code, string Reason, int ExpiresSec)
{
    public static RegInfo Empty { get; } = new(-1, RegState.Unregistered, 0, "", 0);
}

/// <summary>착신 INVITE 의 mcptt-info(TS 24.379 §F.1) 요약. Broadcast = broadcast-ind(일제 통화 — 그룹 종류가 아니라 호 속성).</summary>
public sealed record McpttInfo(bool Present, string SessionType, string RequestUri, string CallingUserId, string CallingGroupId,
                               bool Emergency, bool ImminentPeril, bool PrivateCall, bool NoFloorCtrl, bool Broadcast = false)
{
    public static McpttInfo None { get; } = new(false, "", "", "", "", false, false, false, false);
}

/// <summary>MCPTT 세션 조건(types.h McpttCondition) — 그룹의 긴급·임박 상태를 이 호에서 본 **현재값**. 개시 mcptt-info 로 시작해 SetCondition(상향·하향)과
/// 서버 재광고로 바뀐다(TS 24.379 §10.1.1.2.1.3~6). 긴급이 임박을 대체한다. Mine = 이 단말이 올린 조건, Pending = 응답 대기.</summary>
public readonly record struct McpttCondition(bool Emergency, bool ImminentPeril, bool Mine, bool Pending, int LastCode);

/// <summary>긴급 경보·긴급 통지 수신(TS 24.379 §12.1.1.3). 지시자 1 = true, -1 = false, 0 = 요소 없음. GroupId·UserId·OriginatedBy 는 bare.
/// AlertInd 1 = 경보, -1 = 경보 취소(OriginatedBy 가 있으면 제3자 취소 — 그 사용자의 경보), 0 = 그룹 상태 통지. Self = 내 에코.</summary>
/// <summary><see cref="Engine.McpttConditionChanged"/> 인자 — Info.Condition 이 새 값.</summary>
public sealed record McpttConditionChange(CallInfo Info, ConditionCause Cause);

public sealed record EmergencyAlert(int AccountId, string GroupId, string UserId, string OriginatedBy, string McOrg,
                                    int AlertInd, int EmergencyInd, int ImminentPerilInd, bool Self);

/// <summary>한 호 안의 RTP 소스(SSRC) — U10 디먹스 산출. 감청 leg 는 RFC 5576 label(caller/callee)로 화자 귀속.</summary>
public sealed record MediaSource(uint Ssrc, string Label, bool Active, float Level);

/// <summary>호 스냅샷. CalledParty = 착신 INVITE 의 P-Called-Party-ID(RFC 3455, 대표번호 착신 식별). PlaybackRoute = 0 기본 재생 장치,
/// 그 외 <see cref="Engine.AddPlaybackRoute"/> 가 준 id. JoinedDialog = INVITE-Join 으로 합류한 대상 dialog 의 Call-ID.
/// Mcptt = 개시·착신 INVITE 의 mcptt-info(호 종류 — 이후 불변), Condition = 긴급·임박의 현재값(판정은 이것으로). RxLevel = 이 호에서 듣는 크기.
/// AnswerState = 개시 200 OK 의 P-Answer-State(RFC 4964 — "Unconfirmed" = 서버가 멤버 확인 전에 받았다, TS 24.379 §10.1.1.2.1.1 2A)),
/// NonAcknowledgedUsers = 서버가 알린 미응답 멤버 MCPTT ID(bare, §6.3.3.3 — 알릴 때 <see cref="Engine.NonAcknowledgedUsersReceived"/>).</summary>
public sealed record CallInfo(
    int CallId, int AccountId, CallDir Dir, CallState State, string RemoteUri, string CalledParty,
    bool Video, bool MediaActive, bool Muted, bool Listen, int PlaybackRoute,
    int LastCode, string LastReason, IReadOnlyList<MediaSource> Sources,
    bool IsMcptt, string GroupId, McpttInfo Mcptt, bool HalfDuplex, bool ListenOnly, string JoinedDialog,
    float RxLevel = 1f, McpttCondition Condition = default, string AnswerState = "", IReadOnlyList<string>? NonAcknowledgedUsers = null,
    McService Service = McService.Mcptt, string SessionUri = "", bool VideoSend = true)
{
    public static CallInfo Empty { get; } = new(-1, -1, CallDir.Outgoing, CallState.Null, "", "", false, false, false, true, 0, 0, "",
                                                Array.Empty<MediaSource>(), false, "", McpttInfo.None, false, false, "");
    public bool IsLive => State is CallState.Outgoing or CallState.Incoming or CallState.Active or CallState.Held;
}

public sealed record Talker(string Id, uint Ssrc, bool Self);

/// <summary>floor participant 이벤트 — 상태 전이와 함께 온다. Permission = Taken 의 Permission to Request the Floor(0=요청 불가).</summary>
public sealed record FloorEvent(FloorEventKind Kind, int CallId, FloorState State, int DurationSec, int Cause, string CauseText,
                                int Indicator, int Permission, int QueuePosition, bool MeSpeaking, IReadOnlyList<Talker> Talkers, int RawType);

public sealed record FloorInfo(FloorState State, IReadOnlyList<Talker> Talkers, bool CanRequest, int Indicator, int QueuePosition,
                               int LocalPort, string RemoteIp, int RemotePort, uint GrantedCount, uint TakenCount, uint DenyCount)
{
    public static FloorInfo Empty { get; } = new(FloorState.Idle, Array.Empty<Talker>(), true, 0, -1, 0, "", 0, 0, 0, 0);
}

/// <summary>MCVideo 한 송출 — 송출자 한 명의 audio·video 흐름 쌍(Media Transmission Notification §9.2.13). UserId 가 AcceptReception·EndReception 인자.</summary>
public sealed record VideoTransmitter(string UserId, uint AudioSsrc, uint VideoSsrc, string FunctionalAlias, bool Automatic, ReceptionState State);

/// <summary>MCVideo 송출 제어 이벤트(TS 24.581 §6.2.4) — 송출(마이크·카메라) 게이트는 코어가 이미 처리했다.</summary>
public sealed record TransmissionEvent(TransmissionEventKind Kind, int CallId, TransmissionState State, int Cause, string CauseText,
                                       int DurationSec, int Priority, int QueuePosition, int Indicator, uint AudioSsrc, uint VideoSsrc,
                                       string ReceiverId, int RawType);

/// <summary>MCVideo 수신 제어 이벤트(§6.2.5) — 새 송출 알림(manual 이면 앱이 [받기])·수신 허가·종료.</summary>
public sealed record ReceptionEvent(ReceptionEventKind Kind, int CallId, VideoTransmitter Transmitter, int Cause, string CauseText, int RawType);

/// <summary>MCVideo 호의 전송 제어 현재값. Transmitters = 알려진 송출(내 것 제외).</summary>
public sealed record TransmissionInfo(TransmissionState State, IReadOnlyList<VideoTransmitter> Transmitters, int QueuePosition,
                                      int LocalPort, string RemoteIp, int RemotePort)
{
    public static TransmissionInfo Empty { get; } = new(TransmissionState.NoPermission, Array.Empty<VideoTransmitter>(), -1, 0, "", 0);
}

/// <summary>임의 SIP 요청(PUBLISH/MESSAGE 등)의 최종 응답 — token 으로 상관.</summary>
public sealed record RequestResult(int AccountId, long Token, string Method, int Code, string Reason, string ETag);

/// <summary>그룹 SDS 발신 결과 — MsgId 는 disposition 통지 상관, Token 은 RequestCompleted 상관.</summary>
public sealed record SdsSend(string MsgId, long Token);

/// <summary>감시 대상의 dialog 상태 (RFC 4235) — 관제 BLF·INVITE-Join 대상 식별.</summary>
public sealed record DialogInfo(int AccountId, string Watched, string Id, string CallId, string LocalTag, string RemoteTag,
                                string Direction, string State, string RemoteIdentity, bool Full)
{
    /// <summary>Join 헤더 값 — &lt;call-id&gt;;to-tag=&lt;remote-tag&gt;;from-tag=&lt;local-tag&gt;.</summary>
    public string JoinHeader() => Engine.DialogJoinHeader(this);
}

/// <summary>회의 로스터 항목 (RFC 4575 conference-info).</summary>
public sealed record RosterEntry(string Uri, string Status);

/// <summary>그룹 로스터 NOTIFY 한 벌. Full = 전체 스냅샷.</summary>
public sealed record RosterUpdate(int AccountId, string GroupId, IReadOnlyList<RosterEntry> Users, bool Full);

/// <summary>MCData FD 로 알릴 파일(types.h FdFile) — Url = CscClient.UploadFd 결과, Type = MIME(빈 값 = application/octet-stream).</summary>
public sealed record FdFile(string Url, string Name, string Type, long Size);

/// <summary>MCData SDS (TS 24.282) — 수신 메시지·disposition 통지·FD. GroupUri = 그룹 SDS·FD 의 request-uri(1:1 은 빈 값 — FromUri 가 상대).
/// DispositionReq: 0 없음/1 delivery/2 read/3 both. NotifType: 1 undelivered/2 delivered/3 read/4 delivered+read. MediaPlane = media plane(MSRP) 배포로 받았다.</summary>
public sealed record SdsMessage(int AccountId, string FromUri, string GroupUri, string ConvId, string MsgId, long TimeSec,
                                int DispositionReq, string Text, bool Notification, int NotifType,
                                bool Fd, string FileUrl, string FileName, string FileType, long FileSize, bool MediaPlane = false);

public sealed record StreamStats(uint RxPackets, uint RxBytes, uint RxLoss, uint RxDiscard, uint TxPackets, uint TxBytes, bool Valid);

/// <summary>호 품질 한 방향 — Rx = 내가 받은 스트림, Remote = 상대가 받은 내 스트림(상대 RTCP RR·XR). 비율 %, 값 없음 = -1, 레벨 127 = 없음.</summary>
public sealed record QualityDirection(bool Valid, uint Packets, uint Lost, uint Discarded, double LossPct, double DiscardPct,
                                      double JitterMs, double JitterMaxMs, double BurstDensityPct, double GapDensityPct,
                                      int BurstMs, int GapMs, int SignalDbm, int NoiseDbm);

/// <summary>호 품질(ue_voice_quality.md §3) — 손실·폐기·지터·RTD(RTCP)·단말 지연과 ITU-T G.107/G.107.1 E-model 추정 R·MOS
/// (LQ = 지연 손상 제외, CQ = 지연 포함). 값 없음 = -1. 종료된 호는 마지막 값.</summary>
public sealed record CallQuality(bool Valid, string Codec, uint ClockRate, bool Wideband, QualityDirection Rx, QualityDirection Remote,
                                 double RtdMs, double EsdMs, double OneWayMs, double RLq, double RCq, double MosLq, double MosCq,
                                 long StartEpochMs, long DurationMs);

/// <summary>서버 인증서 만료 관측 — 마지막 성공 TLS 핸드셰이크의 peer 인증서(SIP TLS = Engine, HTTPS = CscClient).
/// 관측 전엔 Valid=false. DaysLeft 는 코어가 계산한 잔여 일수(음수 = 만료). 임계는 서버와 같다: ≤30일 경고(자동 갱신 실패 신호), ≤7일 위험
/// (sip_tls_signaling.md §8.6.2).</summary>
public sealed record TlsPeerExpiry(bool Valid, DateTimeOffset NotAfter, DateTimeOffset ObservedAt, int DaysLeft, string Subject, string Remote)
{
    public static readonly TlsPeerExpiry None = new(false, DateTimeOffset.UnixEpoch, DateTimeOffset.UnixEpoch, 0, "", "");
}

public sealed record AudioDeviceInfo(int Id, string Name, string Driver, uint InputCount, uint OutputCount);

/// <summary>영상 장치(types.h VideoDeviceInfo) — 캡처(카메라)·렌더. Windows 웹캠 driver = "dshow", 합성 색 막대 = "Colorbar"(시험용), 렌더 = "CIMS"(프레임 콜백).</summary>
public sealed record VideoDeviceInfo(int Id, string Name, string Driver, bool Capture, bool Render)
{
    /// <summary>실제 카메라 — 캡처 장치 중 합성 장치(Colorbar)·AVI 재생기를 뺀 것.</summary>
    public bool IsCamera => Capture && Driver is not ("Colorbar" or "AVI");
}

/// <summary>영상 프레임 한 장(types.h VideoFrame) — BGRA 32 bpp(WPF PixelFormats.Bgr32/Bgra32), 위 줄부터. <see cref="Pixels"/> 는 핸들러가 도는 동안만
/// 유효하다 — 복사해서 쓴다. CallId = 수신 영상의 호, -1 = 셀프뷰.</summary>
public readonly ref struct VideoFrame
{
    public VideoFrame(int callId, int width, int height, int stride, ReadOnlySpan<byte> pixels)
    {
        CallId = callId; Width = width; Height = height; Stride = stride; Pixels = pixels;
    }
    public int CallId { get; }
    public int Width { get; }
    public int Height { get; }
    /// <summary>한 줄 바이트 수.</summary>
    public int Stride { get; }
    public ReadOnlySpan<byte> Pixels { get; }
    public bool IsSelfView => CallId < 0;
}

/// <summary><see cref="Engine.VideoFrameReceived"/> 핸들러 — 영상 스레드에서 곧바로 불린다.</summary>
public delegate void VideoFrameHandler(Engine sender, in VideoFrame frame);

/// <summary>MCData 가 아닌 MESSAGE/NOTIFY 본문(text/plain 문자, xcap-diff 등) — 앱이 해석.</summary>
public sealed record SipMessage(int AccountId, string FromUri, string ContentType, string Body);

/// <summary>pjsip/코어 로그 한 줄. Level 은 pjsip 레벨(1=error … 6=trace).</summary>
public sealed record LogLine(int Level, string Message);

/// <summary>파사드가 코어 스냅샷을 잡지 못한 경우(핸들 소멸 뒤 호출 등).</summary>
public sealed class CimsUeException : Exception
{
    public CimsUeException(string message) : base(message) { }
}
