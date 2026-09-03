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
}

public sealed record RegInfo(int AccountId, RegState State, int Code, string Reason, int ExpiresSec)
{
    public static RegInfo Empty { get; } = new(-1, RegState.Unregistered, 0, "", 0);
}

/// <summary>착신 INVITE 의 mcptt-info(TS 24.379 §F.1) 요약.</summary>
public sealed record McpttInfo(bool Present, string SessionType, string RequestUri, string CallingUserId, string CallingGroupId,
                               bool Emergency, bool ImminentPeril, bool PrivateCall, bool NoFloorCtrl)
{
    public static McpttInfo None { get; } = new(false, "", "", "", "", false, false, false, false);
}

/// <summary>한 호 안의 RTP 소스(SSRC) — U10 디먹스 산출. 감청 leg 는 RFC 5576 label(caller/callee)로 화자 귀속.</summary>
public sealed record MediaSource(uint Ssrc, string Label, bool Active, float Level);

/// <summary>호 스냅샷. CalledParty = 착신 INVITE 의 P-Called-Party-ID(RFC 3455, 대표번호 착신 식별). PlaybackRoute = 0 기본 재생 장치,
/// 그 외 <see cref="Engine.AddPlaybackRoute"/> 가 준 id. JoinedDialog = INVITE-Join 으로 합류한 대상 dialog 의 Call-ID.</summary>
public sealed record CallInfo(
    int CallId, int AccountId, CallDir Dir, CallState State, string RemoteUri, string CalledParty,
    bool Video, bool MediaActive, bool Muted, bool Listen, int PlaybackRoute,
    int LastCode, string LastReason, IReadOnlyList<MediaSource> Sources,
    bool IsMcptt, string GroupId, McpttInfo Mcptt, bool HalfDuplex, bool ListenOnly, string JoinedDialog)
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

/// <summary>임의 SIP 요청(PUBLISH/MESSAGE 등)의 최종 응답 — token 으로 상관.</summary>
public sealed record RequestResult(int AccountId, long Token, string Method, int Code, string Reason, string ETag);

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

/// <summary>MCData SDS (TS 24.282) — 수신 메시지·disposition 통지·FD. DispositionReq: 0 없음/1 delivery/2 read/3 both.
/// NotifType: 1 undelivered/2 delivered/3 read/4 delivered+read.</summary>
public sealed record SdsMessage(int AccountId, string FromUri, string GroupUri, string ConvId, string MsgId, long TimeSec,
                                int DispositionReq, string Text, bool Notification, int NotifType,
                                bool Fd, string FileUrl, string FileName, string FileType, long FileSize);

public sealed record StreamStats(uint RxPackets, uint RxBytes, uint RxLoss, uint RxDiscard, uint TxPackets, uint TxBytes, bool Valid);

public sealed record AudioDeviceInfo(int Id, string Name, string Driver, uint InputCount, uint OutputCount);

/// <summary>MCData 가 아닌 MESSAGE/NOTIFY 본문(text/plain 문자, xcap-diff 등) — 앱이 해석.</summary>
public sealed record SipMessage(int AccountId, string FromUri, string ContentType, string Body);

/// <summary>pjsip/코어 로그 한 줄. Level 은 pjsip 레벨(1=error … 6=trace).</summary>
public sealed record LogLine(int Level, string Message);

/// <summary>파사드가 코어 스냅샷을 잡지 못한 경우(핸들 소멸 뒤 호출 등).</summary>
public sealed class CimsUeException : Exception
{
    public CimsUeException(string message) : base(message) { }
}
