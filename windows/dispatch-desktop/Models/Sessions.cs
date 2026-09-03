// 세션 모델 — 코어 CallInfo/FloorInfo 스냅샷의 투영(§11 Models). 앱은 별도 상태 기계를 갖지 않는다.
using CimsUe;
using CommunityToolkit.Mvvm.ComponentModel;
using DispatchDesktop.Converters;

namespace DispatchDesktop.Models;

public enum AccountKind { Volte, Ptt }

/// <summary>화면 배치 기준의 세션 종류 — isMcptt/listenOnly/privateCall/adhoc-/joinedDialog 로 판정(§11).</summary>
public enum SessionKind
{
    /// <summary>③ 내 통화 — VoLTE 1:1(외부망 포함).</summary>
    VolteCall,
    /// <summary>감청 창 — INVITE-Join 청취 leg.</summary>
    VolteMonitor,
    /// <summary>① 멤버 채널 카드 — 그룹콜.</summary>
    PttChannel,
    /// <summary>① 사설콜 카드.</summary>
    PttPrivate,
    /// <summary>① 애드혹 카드.</summary>
    PttAdhoc,
    /// <summary>청취 창 — 그룹콜 recvonly.</summary>
    PttListen,
}

/// <summary>세션을 만든 관제 동작 — 종료 코드의 문구 사전(§9) 선택에 쓴다.</summary>
public enum Operation { Incoming, Dial, Pickup, Join, Transfer, PttJoin, PttListen, PttPrivate, PttAdhoc, Emergency }

public static class SessionKinds
{
    public const string AdhocPrefix = "adhoc-";

    public static SessionKind Of(CallInfo c)
    {
        if (c.IsMcptt && c.ListenOnly) return SessionKind.PttListen;
        if (c.IsMcptt && c.Mcptt.PrivateCall) return SessionKind.PttPrivate;
        if (c.IsMcptt && c.GroupId.StartsWith(AdhocPrefix, StringComparison.Ordinal)) return SessionKind.PttAdhoc;
        if (c.IsMcptt) return SessionKind.PttChannel;
        if (c.ListenOnly && c.JoinedDialog.Length > 0) return SessionKind.VolteMonitor;
        return SessionKind.VolteCall;
    }

    public static bool IsWindow(SessionKind k) => k is SessionKind.VolteMonitor or SessionKind.PttListen;
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
    /// <summary>애드혹 멤버 칩(응답 상태는 로스터).</summary>
    public IReadOnlyList<string> AdhocMembers { get; set; } = Array.Empty<string>();

    public SessionItem(CallInfo info, AccountKind account, Operation op)
    {
        CallId = info.CallId;
        Account = account;
        Operation = op;
        _info = info;
    }

    public SessionKind Kind => SessionKinds.Of(Info);
    public bool IsWindow => SessionKinds.IsWindow(Kind);
    public bool IsPttCard => SessionKinds.IsPttCard(Kind);
    public bool IsVolteCall => Kind == SessionKind.VolteCall;
    public string PeerNumber => UserPartConverter.UserPart(Info.RemoteUri);
    public string CalledParty => UserPartConverter.UserPart(Info.CalledParty);
    public bool IsIncoming => Info.State == CallState.Incoming;
    public bool IsActive => Info.State == CallState.Active;
    public bool IsHeld => Info.State == CallState.Held;
    public bool IsOutgoing => Info.State == CallState.Outgoing;
    public bool IsLive => Info.IsLive;
    public bool IsEmergency => Info.Mcptt.Emergency;
    public bool IsImminentPeril => Info.Mcptt.ImminentPeril;
    public bool IsFullDuplex => Info.Mcptt.NoFloorCtrl || (Info.IsMcptt && !Info.HalfDuplex);
    public bool IsSpeaking => Floor.State == FloorState.Speaking;
    public bool IsRequesting => Floor.State == FloorState.Requesting;
    public bool IsQueued => Floor.State == FloorState.Queued;
    public bool CanRequestFloor => Floor.CanRequest && !Info.ListenOnly;
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
        OnPropertyChanged(nameof(Kind)); OnPropertyChanged(nameof(IsWindow)); OnPropertyChanged(nameof(IsPttCard));
        OnPropertyChanged(nameof(IsVolteCall)); OnPropertyChanged(nameof(PeerNumber)); OnPropertyChanged(nameof(CalledParty));
        OnPropertyChanged(nameof(IsIncoming)); OnPropertyChanged(nameof(IsActive)); OnPropertyChanged(nameof(IsHeld));
        OnPropertyChanged(nameof(IsOutgoing)); OnPropertyChanged(nameof(IsLive)); OnPropertyChanged(nameof(IsEmergency));
        OnPropertyChanged(nameof(IsImminentPeril)); OnPropertyChanged(nameof(IsFullDuplex)); OnPropertyChanged(nameof(Route));
        OnPropertyChanged(nameof(RouteIsSpeaker)); OnPropertyChanged(nameof(StateText)); OnPropertyChanged(nameof(CanRequestFloor));
        if (value.State == CallState.Active && ConnectedAt is null) ConnectedAt = DateTime.Now;
    }

    partial void OnFloorChanged(FloorInfo value)
    {
        OnPropertyChanged(nameof(IsSpeaking)); OnPropertyChanged(nameof(IsRequesting)); OnPropertyChanged(nameof(IsQueued));
        OnPropertyChanged(nameof(CanRequestFloor));
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
    [ObservableProperty] private IReadOnlyList<RosterEntry> _roster = Array.Empty<RosterEntry>();
    [ObservableProperty] private DateTime? _rosterAt;

    public GroupInfo(string id, string uri, string name, int memberCount)
    {
        Id = id; Uri = uri; _name = name; _memberCount = memberCount;
    }

    public int ConnectedCount => Roster.Count(r => r.Status == "connected");
    /// <summary>진행 중 세션이 있는가(로스터에 접속 참가자) — ② 진행 중 행.</summary>
    public bool HasSession => ConnectedCount > 0;

    partial void OnRosterChanged(IReadOnlyList<RosterEntry> value)
    {
        OnPropertyChanged(nameof(ConnectedCount));
        OnPropertyChanged(nameof(HasSession));
    }
}
