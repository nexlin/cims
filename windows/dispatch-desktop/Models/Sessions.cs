// 세션 모델 — 코어 CallInfo/FloorInfo 스냅샷의 투영(§11 Models). 앱은 별도 상태 기계를 갖지 않는다.
using CimsUe;
using CommunityToolkit.Mvvm.ComponentModel;
using DispatchDesktop.Converters;

namespace DispatchDesktop.Models;

public enum AccountKind { Volte, Ptt }

/// <summary>화면 배치 기준의 세션 종류 — isMcptt/listenOnly/privateCall/adhoc-/joinedDialog 로 판정(§11).</summary>
public enum SessionKind
{
    /// <summary>[통화] «내 통화» — VoLTE 1:1(외부망 포함).</summary>
    VolteCall,
    /// <summary>VoLTE 감청 — INVITE-Join 청취 leg(«진행 중» 행 확장, [창으로]).</summary>
    VolteMonitor,
    /// <summary>[무전] 내 채널 카드 — 멤버 그룹콜.</summary>
    PttChannel,
    /// <summary>내 채널 개별 통화 카드(TS 24.379 private call).</summary>
    PttPrivate,
    /// <summary>내 채널 애드혹 그룹 통화 카드(ad hoc group call).</summary>
    PttAdhoc,
    /// <summary>타 채널 청취 — 그룹콜 recvonly(행 [청취 중], [창으로]).</summary>
    PttListen,
    /// <summary>MCVideo 그룹 영상 호(TS 24.281) — 카드가 아니라 그 그룹 카드의 «영상» 절(§10). MCPTT 호와 독립된 다이얼로그(isMcptt=false).</summary>
    McVideo,
}

/// <summary>세션을 만든 관제 동작 — 종료 코드의 문구 사전(§9) 선택에 쓴다.</summary>
public enum Operation { Incoming, Dial, Pickup, Join, Transfer, PttJoin, PttListen, PttPrivate, PttAdhoc, Emergency, Broadcast, VideoJoin }

public static class SessionKinds
{
    public const string AdhocPrefix = "adhoc-";

    public static SessionKind Of(CallInfo c)
    {
        if (c.Service == McService.McVideo) return SessionKind.McVideo;      // isMcptt 는 false — 먼저 갈라야 VoLTE 로 읽지 않는다
        if (c.IsMcptt && c.ListenOnly) return SessionKind.PttListen;
        if (c.IsMcptt && c.Mcptt.PrivateCall) return SessionKind.PttPrivate;
        if (c.IsMcptt && c.GroupId.StartsWith(AdhocPrefix, StringComparison.Ordinal)) return SessionKind.PttAdhoc;
        if (c.IsMcptt) return SessionKind.PttChannel;
        if (c.ListenOnly && c.JoinedDialog.Length > 0) return SessionKind.VolteMonitor;
        return SessionKind.VolteCall;
    }

    /// <summary>듣기만 하는 leg — VoLTE 감청·PTT 청취. 기본 표면은 인라인(«진행 중» 행 확장·타 채널 행)이고 감청 창은 [창으로] 를 눌렀을 때만(§5).</summary>
    public static bool IsListenLeg(SessionKind k) => k is SessionKind.VolteMonitor or SessionKind.PttListen;
    public static bool IsPttCard(SessionKind k) => k is SessionKind.PttChannel or SessionKind.PttPrivate or SessionKind.PttAdhoc;
}

/// <summary>살아 있는 호 하나. Info/Floor 는 코어 스냅샷 복사본, Elapsed 는 1초 타이머가 갱신한다.</summary>
public sealed partial class SessionItem : ObservableObject
{
    public int CallId { get; }
    public AccountKind Account { get; }
    public Operation Operation { get; }
    public DateTime StartedAt { get; } = DateTime.Now;

    [ObservableProperty] private CallInfo _info;
    [ObservableProperty] private FloorInfo _floor = FloorInfo.Empty;
    [ObservableProperty] private FloorEvent? _lastFloor;
    [ObservableProperty] private TimeSpan _elapsed;
    [ObservableProperty] private DateTime? _connectedAt;
    /// <summary>현재 발언자(로스터·주소록으로 이름 해석된 표시명).</summary>
    [ObservableProperty] private string _speaker = "";
    [ObservableProperty] private DateTime? _speakerSince;
    [ObservableProperty] private TimeSpan _speakerElapsed;
    /// <summary>발언 시간 게이지 0~1 (Granted Duration 기준, 남은 비율).</summary>
    [ObservableProperty] private double _talkGauge;
    [ObservableProperty] private bool _talkLimitNear;
    /// <summary>Denied/Revoked 사유 한 줄(카드 하단, 잠시 표시).</summary>
    [ObservableProperty] private string _floorNote = "";
    /// <summary>표시 이름(그룹명·상대 이름) — 주소록으로 해석.</summary>
    [ObservableProperty] private string _title = "";
    [ObservableProperty] private bool _isSelected;
    /// <summary>상담 전달 중인 원 통화(이 세션이 상담 호일 때).</summary>
    [ObservableProperty] private SessionItem? _consultFor;
    /// <summary>전달 진행 표시("전달 중 → 1003").</summary>
    [ObservableProperty] private string _transferNote = "";
    /// <summary>P-Answer-State Unconfirmed 를 «이벤트» 에 적었다(한 번만).</summary>
    public bool AnswerStateNoted { get; set; }
    /// <summary>애드혹 멤버 칩(응답 상태는 로스터).</summary>
    public IReadOnlyList<string> AdhocMembers { get; set; } = Array.Empty<string>();
    /// <summary>MCVideo 호의 전송 제어 현재값(코어 transmissionInfo — 송출 목록 = 알려진 송출, 내 것 제외). 송출·수신 이벤트마다 다시 읽는다.</summary>
    [ObservableProperty] private TransmissionInfo _transmission = TransmissionInfo.Empty;
    /// <summary>«새 영상» 으로 이미 알린 송출(송출자 MCVideo ID) — 거절·그만 보기 뒤 Notified 로 돌아온 것을 새 송출로 다시 알리지 않는다. 송출이 끝나면 뺀다.</summary>
    public HashSet<string> TransmitterAnnounced { get; } = new(StringComparer.OrdinalIgnoreCase);
    /// <summary>송출을 처음 안 시각(송출자 MCVideo ID → 시각) — 송출 목록의 경과. 코어 값에는 시각이 없다.</summary>
    public Dictionary<string, DateTime> TransmitterSince { get; } = new(StringComparer.OrdinalIgnoreCase);
    public bool IsMcVideo => Kind == SessionKind.McVideo;
    /// <summary>지금 받고 있는 송출(1차는 하나) — 수신 상태 Receiving·PendingRelease.</summary>
    public VideoTransmitter? Receiving => Transmission.Transmitters.FirstOrDefault(t => t.State is ReceptionState.Receiving or ReceptionState.PendingRelease);
    /// <summary>알림 받은(아직 끝나지 않은) 송출 — 목록에 보이는 것.</summary>
    public IReadOnlyList<VideoTransmitter> Transmitters => Transmission.Transmitters.Where(t => t.State != ReceptionState.Ended).ToList();
    partial void OnTransmissionChanged(TransmissionInfo value)
    {
        foreach (var t in value.Transmitters.Where(t => t.State != ReceptionState.Ended)) TransmitterSince.TryAdd(t.UserId, DateTime.Now);
        foreach (var gone in TransmitterSince.Keys.Where(k => !value.Transmitters.Any(t => t.State != ReceptionState.Ended && string.Equals(t.UserId, k, StringComparison.OrdinalIgnoreCase))).ToList())
            TransmitterSince.Remove(gone);
        OnPropertyChanged(nameof(Receiving)); OnPropertyChanged(nameof(Transmitters));
    }

    public SessionItem(CallInfo info, AccountKind account, Operation op)
    {
        CallId = info.CallId;
        Account = account;
        Operation = op;
        _info = info;
    }

    public SessionKind Kind => SessionKinds.Of(Info);
    public bool IsListenLeg => SessionKinds.IsListenLeg(Kind);
    public bool IsPttCard => SessionKinds.IsPttCard(Kind);
    public bool IsVolteCall => Kind == SessionKind.VolteCall;
    public string PeerNumber => UserPartConverter.UserPart(Info.RemoteUri);
    public string CalledParty => UserPartConverter.UserPart(Info.CalledParty);
    public bool IsIncoming => Info.State == CallState.Incoming;
    public bool IsActive => Info.State == CallState.Active;
    public bool IsHeld => Info.State == CallState.Held;
    public bool IsOutgoing => Info.State == CallState.Outgoing;
    public bool IsLive => Info.IsLive;
    /// <summary>긴급·임박 = 코어 세션 조건의 **현재값**(CallInfo.Condition — 개시 mcptt-info 로 시작해 상향·하향 re-INVITE·서버 재광고로 바뀐다,
    /// TS 24.379 §10.1.1.2.1.3~6). CallInfo.Mcptt 는 호를 세운 INVITE 의 값으로 불변이라 판정에 쓰지 않는다.</summary>
    public bool IsEmergency => Info.Condition.Emergency;
    public bool IsImminentPeril => Info.Condition.ImminentPeril && !Info.Condition.Emergency;
    /// <summary>이 단말이 올린 조건(개시 옵션·상향) — 내가 풀 수 있다.</summary>
    public bool IsConditionMine => Info.Condition.Mine;
    /// <summary>조건 개시자 — 호를 세운 INVITE 가 그 조건을 실었을 때만(mcptt-calling-user-id). 진행 중에 걸린 조건은 재광고가 개시자를 싣지만
    /// 코어 조건에 그 값이 없어 비운다(호 발신자를 개시자로 적지 않는다 — ue_sdk.md §11).</summary>
    public string ConditionInitiator => (IsEmergency && Info.Mcptt.Emergency) || (IsImminentPeril && Info.Mcptt.ImminentPeril) ? Info.Mcptt.CallingUserId : "";
    /// <summary>일제 통화 개시를 요청한 내 발신 호(mcptt-info broadcast-ind, TS 24.379 §6.2.8.2) — 서버가 받아들였는지는 <see cref="IsBroadcast"/>.</summary>
    public bool IsBroadcastRequest => Info.Dir == CallDir.Outgoing && Info.Mcptt.Broadcast;
    /// <summary>서버가 일제 통화(TS 24.379 §4.12, 호 속성)로 알린 호 — fan-out 착신 INVITE 의 broadcast-ind 또는 floor 메시지의 B-bit
    /// (TS 24.380 §8.2.3.15). 개시자와 진행 중 일제 통화에 늦게 합류한 leg 은 B-bit 로만 안다.</summary>
    public bool IsBroadcast => (Info.Dir == CallDir.Incoming && Info.Mcptt.Broadcast) || (Floor.Indicator & FloorIndicator.BroadcastGroup) != 0;
    /// <summary>내가 연 일제 통화 — 발언을 놓으면 코어가 호를 해제한다(TS 24.380 §6.2.4.6.4). 남이 연 일제 통화에 합류한 leg 은 Permission 0 이라 아니다.</summary>
    public bool IsBroadcastInitiator => IsBroadcastRequest && IsBroadcast && Floor.CanRequest;
    public bool IsFullDuplex => Info.Mcptt.NoFloorCtrl || (Info.IsMcptt && !Info.HalfDuplex);
    public bool IsSpeaking => Floor.State == FloorState.Speaking;
    public bool IsRequesting => Floor.State == FloorState.Requesting;
    public bool IsQueued => Floor.State == FloorState.Queued;
    /// <summary>발언 요청 가능 — 서버 Permission(Taken·Idle 의 0 = 불가) · 청취 전용 leg 아님 · 남이 연 일제 통화가 아님(개시자만 발언 — R5).</summary>
    public bool CanRequestFloor => Floor.CanRequest && !Info.ListenOnly && !(IsBroadcast && !IsBroadcastRequest);
    public int Route => Info.PlaybackRoute;
    public bool RouteIsSpeaker => Info.PlaybackRoute != 0;
    public string StateText => Info.State switch
    {
        CallState.Outgoing => "발신 중",
        CallState.Incoming => "착신",
        CallState.Active => Kind == SessionKind.VolteMonitor ? "감청" : Kind == SessionKind.PttListen ? "청취" : "통화",
        CallState.Held => "보류",
        CallState.Disconnected => "종료",
        _ => "",
    };

    partial void OnInfoChanged(CallInfo value)
    {
        OnPropertyChanged(nameof(Kind)); OnPropertyChanged(nameof(IsListenLeg)); OnPropertyChanged(nameof(IsPttCard));
        OnPropertyChanged(nameof(IsVolteCall)); OnPropertyChanged(nameof(PeerNumber)); OnPropertyChanged(nameof(CalledParty));
        OnPropertyChanged(nameof(IsIncoming)); OnPropertyChanged(nameof(IsActive)); OnPropertyChanged(nameof(IsHeld));
        OnPropertyChanged(nameof(IsOutgoing)); OnPropertyChanged(nameof(IsLive)); OnPropertyChanged(nameof(IsEmergency));
        OnPropertyChanged(nameof(IsImminentPeril)); OnPropertyChanged(nameof(IsConditionMine)); OnPropertyChanged(nameof(ConditionInitiator));
        OnPropertyChanged(nameof(IsFullDuplex)); OnPropertyChanged(nameof(Route));
        OnPropertyChanged(nameof(IsBroadcastRequest)); OnPropertyChanged(nameof(IsBroadcast)); OnPropertyChanged(nameof(IsBroadcastInitiator));
        OnPropertyChanged(nameof(RouteIsSpeaker)); OnPropertyChanged(nameof(StateText)); OnPropertyChanged(nameof(CanRequestFloor)); OnPropertyChanged(nameof(IsMcVideo));
        if (value.State == CallState.Active && ConnectedAt is null) ConnectedAt = DateTime.Now;
    }

    partial void OnFloorChanged(FloorInfo value)
    {
        OnPropertyChanged(nameof(IsSpeaking)); OnPropertyChanged(nameof(IsRequesting)); OnPropertyChanged(nameof(IsQueued));
        OnPropertyChanged(nameof(CanRequestFloor)); OnPropertyChanged(nameof(IsBroadcast)); OnPropertyChanged(nameof(IsBroadcastInitiator));
    }

    public void Tick(DateTime now)
    {
        Elapsed = now - (ConnectedAt ?? StartedAt);
        if (SpeakerSince is DateTime s) SpeakerElapsed = now - s;
        if (LastFloor is { Kind: FloorEventKind.Granted, DurationSec: > 0 } g && SpeakerSince is DateTime since)
        {
            double remain = 1 - (now - since).TotalSeconds / g.DurationSec;
            TalkGauge = Math.Clamp(remain, 0, 1);
            TalkLimitNear = remain < 0.15;
        }
    }
}

/// <summary>GMS 그룹(멤버 그룹) — 카드의 채널 소스(§4.1).</summary>
public sealed partial class GroupInfo : ObservableObject
{
    public string Id { get; }
    public string Uri { get; }
    [ObservableProperty] private string _name;
    [ObservableProperty] private int _memberCount;
    [ObservableProperty] private bool _affiliated;
    /// <summary>멤버 그룹(true) / 청취 범위 그룹(false — pttListen 대상).</summary>
    public bool IsMember { get; init; } = true;
    /// <summary>내가 소유(authorized user)한 그룹 — [편집]·[삭제] 가능(GMS 목록 is_owner).</summary>
    [ObservableProperty] private bool _isOwner;
    /// <summary>GMS 목록의 문서 ETag — 편집 PUT 의 If-Match.</summary>
    [ObservableProperty] private string _etag = "";
    /// <summary>그룹 종류 prearranged | chat(TS 24.481 on-network-invite-members) — GMS 목록엔 없어 관리 목록에서 옮겨 온다. 빈 값 = 모름.</summary>
    [ObservableProperty] private string _sessionType = "";
    [ObservableProperty] private IReadOnlyList<RosterEntry> _roster = Array.Empty<RosterEntry>();
    [ObservableProperty] private DateTime? _rosterAt;
    /// <summary>로스터에 접속 참가자가 생긴 시각(세션 관측 시작) — 없으면 null. 타 채널 행의 경과.</summary>
    [ObservableProperty] private DateTime? _sessionSince;
    /// <summary>MCVideo 그룹 — MCVideo user profile 의 &lt;MCVideoGroupInfo&gt;(TS 24.484 §9.3)에 있는 멤버 그룹. 이것만 채널 상세 «영상» 절을 갖는다(§10.2).</summary>
    [ObservableProperty] private bool _mcVideo;
    /// <summary>MCVideo 호 방식(TS 24.481 mcvideo-on-network-invite-members) — prearranged | chat, 빈 값 = 아직 문서를 안 받음(합류 때 받는다).</summary>
    [ObservableProperty] private string _mcVideoType = "";
    /// <summary>MCVideo 동시 송출 상한(그룹 문서) — 0 = 모름.</summary>
    [ObservableProperty] private int _mcVideoMaxTransmitters;
    /// <summary>MCVideo 로 affiliate 했다(TS 24.281 §8.2.1.2 — prearranged 영상 호 초대 대상).</summary>
    [ObservableProperty] private bool _mcVideoAffiliated;
    /// <summary>이 그룹의 MCVideo 그룹 영상 호(참가 중일 때) — 카드 «영상 n» 태그·채널 상세 «영상» 절의 원천.</summary>
    [ObservableProperty] private SessionItem? _videoSession;
    /// <summary>영상 채널 연결 상태 한 줄(영상 호가 없을 때 — «영상 연결 중…» · 편성 그룹 초대 대기 · 한도·실패·재시도). 빈 값 = 알릴 것 없음.</summary>
    [ObservableProperty] private string _videoNote = "";

    public GroupInfo(string id, string uri, string name, int memberCount)
    {
        Id = id; Uri = uri; _name = name; _memberCount = memberCount;
    }

    public bool IsChat => SessionType == "chat";
    partial void OnSessionTypeChanged(string value) => OnPropertyChanged(nameof(IsChat));

    public int ConnectedCount => Roster.Count(r => r.Status == "connected");
    /// <summary>진행 중 세션이 있는가(로스터에 접속 참가자) — 타 채널 행·카드 «세션 진행 중».</summary>
    public bool HasSession => ConnectedCount > 0;

    partial void OnRosterChanged(IReadOnlyList<RosterEntry> value)
    {
        OnPropertyChanged(nameof(ConnectedCount));
        OnPropertyChanged(nameof(HasSession));
        if (HasSession) SessionSince ??= DateTime.Now;
        else SessionSince = null;
    }
}
