// 오른쪽 패널 440(§3.6) — 한 대상을 자세히 보거나 목록에서 사람을 찾는 자리 하나. 왼쪽 칸 1040 은 움직이지 않고 오른쪽 칸만 민다.
// 내용 = 채널 상세(내 채널·타 채널) · 사용자(무전) · 새 PTT 그룹 · 이벤트 상세 · 주소록(통화). 규칙: 같은 대상 = 닫기 · 다른 대상 = 교체 ·
// 고정(핀)하면 모드·레일을 옮겨도 남는다 · 패널 안에서 들어간 것(사용자 › 새 그룹)만 ← 로 돌아간다.
using System.Collections.ObjectModel;
using CimsUe;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public enum PanelView { None, Channel, Users, Group, Event, Directory }

/// <summary>채널 상세의 사람 줄 — 접속 로스터 또는 편성 멤버(GMS 그룹 문서).</summary>
public sealed record ChannelPersonRow(string Name, string Uri, string Meta, bool IsMe, bool IsSpeaking, bool IsAbsent, bool IsChair, bool IsListener)
{
    public string Initial => Name.Trim().Length > 0 ? Name.Trim()[..1] : "?";
    public string Number => UserPartConverter.UserPart(Uri);
    public bool CanAct => !IsMe;
    public string NameText => IsMe ? $"{Name} (나)" : Name;
}

/// <summary>채널 상세 — 내 채널 카드(ChannelCard) 또는 타 채널 행(ScopedCard)의 투영. 조작은 카드의 명령을 그대로 부른다(같은 판정).</summary>
public sealed partial class ChannelDetailViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    public ChannelCard? Card { get; }
    public ScopedCard? Other { get; }
    /// <summary>connected | all — [접속 n | 편성 m] 세그먼트.</summary>
    [ObservableProperty] private string _segment = "connected";
    /// <summary>편성 멤버(GMS 그룹 문서) — [편성] 을 처음 누를 때 받는다. null = 아직 안 받음.</summary>
    [ObservableProperty] private IReadOnlyList<GroupMember>? _members;
    [ObservableProperty] private string _membersNote = "";
    public ObservableCollection<ChannelPersonRow> Rows { get; } = new();
    /// <summary>청취 중인 타 채널의 청취 leg — 음량·출력(감청 창과 같은 VM).</summary>
    [ObservableProperty] private MonitorWindowViewModel? _listen;

    public event EventHandler<GroupInfo>? MessageRequested;
    public event EventHandler<GroupInfo>? EditRequested;
    public event EventHandler<GroupInfo>? DeleteRequested;
    public event EventHandler<string>? PersonMenuRequested;
    public event EventHandler<string>? PrivateCallRequested;
    public event EventHandler<string>? SdsRequested;

    public ChannelDetailViewModel(DispatchSession s, ChannelCard card) { _s = s; Card = card; Refresh(); }
    public ChannelDetailViewModel(DispatchSession s, ScopedCard other) { _s = s; Other = other; Refresh(); }

    public GroupInfo? Group => Card?.Group ?? Other?.Group;
    public string Id => Card?.Id ?? Other?.Id ?? "";
    public string Title => Card?.Title ?? Other?.Title ?? "";
    public bool IsMine => Card is not null;
    public bool IsOther => Other is not null;
    public bool IsEmergency => Card?.IsEmergency ?? Other?.IsEmergency ?? false;
    public bool IsImminentPeril => Card?.IsImminentPeril ?? Other?.IsImminentPeril ?? false;
    public bool IsBroadcast => Card?.IsBroadcast ?? Other?.IsBroadcast ?? false;
    public bool HasLabels => IsEmergency || IsImminentPeril || IsBroadcast;
    public int ConnectedCount => Group?.ConnectedCount ?? (Card?.IsPrivate == true ? 2 : Card?.Session?.AdhocMembers.Count + 1 ?? 0);
    public int MemberCount => Members?.Count ?? Group?.MemberCount ?? 0;

    /// <summary>머리 아래 한 줄 — "멤버 그룹 · 참가 7 · 02:03 · 편성 12 · 발언 김관제".</summary>
    public string Summary
    {
        get
        {
            var parts = new List<string>();
            if (Card is { } c)
            {
                parts.Add(c.IsMember ? "멤버 그룹" : c.IsPrivate ? $"개별 {(c.IsFullDuplex ? "전이중" : "반이중")}" : "애드혹");
                if (c.IsJoined) { parts.Add($"참가 {ConnectedCount}"); parts.Add(DispatchSession.Fmt(c.Elapsed)); }
                else parts.Add(c.Group?.HasSession == true ? $"참가 {c.Group.ConnectedCount} · 진행(미참여)" : "대기");
                if (c.IsMember) parts.Add($"편성 {MemberCount}");
                if (c.HasSpeaker) parts.Add($"발언 {c.Speaker}");
            }
            else if (Other is { } o)
            {
                parts.Add("청취 범위");
                parts.Add(o.HasSession ? $"참가 {o.Participants}" : "대기");
                if (o.Elapsed is TimeSpan e) parts.Add(DispatchSession.Fmt(e));
                parts.Add($"편성 {MemberCount}");
                if (o.HasSpeaker) parts.Add($"발언 {o.Speaker}");
            }
            return string.Join(" · ", parts);
        }
    }

    // ── 조작(내 채널) — 카드의 판정 그대로 ──
    public bool ShowJoin => Card?.ShowJoin == true;
    public bool ShowLeave => Card?.IsJoined == true;
    public string LeaveText => Card?.IsMember == true ? "나가기" : "종료";
    public bool ShowEmergency => Card is { IsMember: true } c && !c.IsEmergency;
    public bool CanEmergency => Card?.CanEmergency == true;
    public string EmergencyText => Card?.IsJoined == true ? "긴급" : "긴급 참여";
    public string EmergencyTip => Card?.EmergencyTip ?? "";
    public bool ShowCancelEmergency => Card?.CanCancelEmergency == true;
    public bool ShowTarget => Card?.ShowTarget == true;
    public bool CanTarget => Card?.CanCheck == true;
    public bool IsTarget => Card?.IsChecked == true;
    public string TargetText => IsTarget ? "✓ 발언 대상" : "발언 대상";
    public bool ShowBroadcast => Card is { IsMember: true, IsJoined: false };
    public bool CanPressBroadcast => Card?.CanPressBroadcast == true;
    public string BroadcastText => Card?.BroadcastText ?? "";
    public string BroadcastTip => Card?.BroadcastTip ?? "";
    public bool IsBroadcastHeld => Card?.IsBroadcastHeld == true;
    public bool ShowMute => Card?.ShowMute == true;
    public bool IsMuted => Card?.IsMuted == true;
    /// <summary>[메시지 ›] — 멤버 그룹만(청취 범위 그룹에 그룹 SDS 를 보내면 비멤버라 서버가 403).</summary>
    public bool ShowMessage => Card?.Group is not null;
    public string MessageText => Card?.Unread > 0 ? $"메시지 {Card.Unread} ›" : "메시지 ›";
    public bool ShowRoute => Card?.IsJoined == true || Other?.IsListening == true;
    public bool CanRoute => _s.Audio.HasSpeaker;
    public bool RouteIsSpeaker => Card?.RouteIsSpeaker ?? Other?.RouteIsSpeaker ?? false;
    public string RouteText => RouteIsSpeaker ? "스피커" : "헤드셋";
    public bool ShowMore => (Card?.CanEdit ?? Other?.CanEdit ?? false) && Group is not null;
    // ── 조작(타 채널) ──
    public bool ShowListen => Other is { IsListening: false };
    public bool CanListen => Other?.CanListen == true;
    public string ListenTip => Other?.ListenTip ?? "";
    public bool ShowStopListen => Other?.IsListening == true;
    public bool HasSegment => Group is not null;
    public string SegConnected => $"접속 {ConnectedCount}";
    public string SegAll => $"편성 {MemberCount}";
    public string RosterHead => Group is null ? $"참가 {ConnectedCount}" : "";

    partial void OnSegmentChanged(string value) { if (value == "all" && Members is null) _ = LoadMembersAsync(); RebuildRows(); }
    partial void OnMembersChanged(IReadOnlyList<GroupMember>? value) { OnPropertyChanged(nameof(MemberCount)); OnPropertyChanged(nameof(SegAll)); OnPropertyChanged(nameof(Summary)); RebuildRows(); }
    [RelayCommand] private void SetSegment(string seg) => Segment = seg;

    private async Task LoadMembersAsync()
    {
        if (Group is not { } g) return;
        MembersNote = "편성 멤버를 받는 중…";
        var r = await _s.GetGroupAsync(g);
        if (!r.Ok) { MembersNote = "편성 멤버를 받지 못했습니다 — " + ResponseText.Describe(ResponseText.Area.Group, r.Code, r.Reason); return; }
        MembersNote = "";
        Members = r.Value.Members;
    }

    /// <summary>"PTT 1001 · 순찰대" — 전화번호부의 조직 이름.</summary>
    private string MetaOf(string uri, string suffix = "")
    {
        string num = UserPartConverter.UserPart(uri);
        var c = _s.Directory.PttUsers.FirstOrDefault(x => DirectoryService.Normalize(x.Number) == DirectoryService.Normalize(num));
        string org = c is { OrgCode.Length: > 0 } ? _s.Directory.OrgName(c.OrgCode) : "";
        return string.Join(" · ", new[] { "PTT " + _s.Directory.DisplayNumber(num), org, suffix }.Where(x => x.Length > 0));
    }

    private void RebuildRows()
    {
        Rows.Clear();
        string speaker = Card?.Speaker ?? Other?.Speaker ?? "";
        if (Group is { } g)
        {
            var roster = g.Roster.Where(r => r.Status != "listener" || !_s.ListenHidden).ToList();
            if (Segment == "all" && Members is { } members)
            {
                foreach (var m in members.OrderByDescending(m => m.Role == "chair").ThenBy(m => m.Name, StringComparer.CurrentCulture))
                {
                    var e = roster.FirstOrDefault(r => DirectoryService.Normalize(UserPartConverter.UserPart(r.Uri)) == DirectoryService.Normalize(UserPartConverter.UserPart(m.Uri)));
                    string name = m.Name.Length > 0 ? m.Name : _s.NameOfPtt(m.Uri);
                    bool me = _s.IsMe(m.Uri);
                    Rows.Add(new ChannelPersonRow(me ? _s.DisplayName : name, m.Uri, MetaOf(m.Uri, e is null ? "미참가" : ""), me, speaker.Length > 0 && (name == speaker || (me && speaker == "나")),
                                                  e is null, m.Role == "chair", e?.Status == "listener"));
                }
            }
            else
            {
                foreach (var r in roster.OrderByDescending(r => _s.IsMe(r.Uri)).ThenBy(r => _s.NameOfPtt(r.Uri), StringComparer.CurrentCulture))
                {
                    bool me = _s.IsMe(r.Uri);
                    string name = me ? _s.DisplayName : _s.NameOfPtt(r.Uri);
                    string st = r.Status switch { "listener" => "청취", "on-hold" => "보류", _ => "" };
                    Rows.Add(new ChannelPersonRow(name, r.Uri, MetaOf(r.Uri, st), me, speaker.Length > 0 && (name == speaker || (me && speaker == "나")), false, false, r.Status == "listener"));
                }
            }
        }
        else if (Card?.Session is { } sess)
        {
            // 개별·애드혹 — 로스터 구독이 없어 나 + 상대(애드혹 = 초대한 사람)
            Rows.Add(new ChannelPersonRow(_s.DisplayName, _s.ToTelUri(_s.MyPttNumber), MetaOf(_s.MyPttNumber), true, sess.IsSpeaking, false, false, false));
            var peers = Card.IsPrivate ? new[] { sess.Info.RemoteUri } : sess.AdhocMembers;
            foreach (var p in peers)
            {
                string name = _s.NameOfPtt(p);
                Rows.Add(new ChannelPersonRow(name, p, MetaOf(p), false, speaker.Length > 0 && name == speaker, false, false, false));
            }
        }
    }

    /// <summary>1초 틱 — 문구만(줄은 로스터·floor 이벤트 때 다시 짓는다: 매초 다시 지으면 줄 버튼의 호버·누름이 끊긴다).</summary>
    public void Tick() => Refresh(rows: false);

    public void Refresh(bool rows = true)
    {
        var l = Other?.Listen;
        if (l is null) Listen = null;
        else if (Listen?.Session != l) Listen = new MonitorWindowViewModel(_s, l);
        else Listen.Refresh();
        foreach (var p in new[] { nameof(Title), nameof(IsEmergency), nameof(IsImminentPeril), nameof(IsBroadcast), nameof(HasLabels), nameof(ConnectedCount), nameof(MemberCount), nameof(Summary),
                                  nameof(ShowJoin), nameof(ShowLeave), nameof(LeaveText), nameof(ShowEmergency), nameof(CanEmergency), nameof(EmergencyText), nameof(EmergencyTip),
                                  nameof(ShowCancelEmergency), nameof(ShowTarget), nameof(CanTarget), nameof(IsTarget), nameof(TargetText), nameof(ShowBroadcast), nameof(CanPressBroadcast),
                                  nameof(BroadcastText), nameof(BroadcastTip), nameof(IsBroadcastHeld), nameof(ShowMute), nameof(IsMuted), nameof(ShowMessage), nameof(MessageText),
                                  nameof(ShowRoute), nameof(CanRoute), nameof(RouteIsSpeaker), nameof(RouteText), nameof(ShowMore), nameof(ShowListen), nameof(CanListen), nameof(ListenTip),
                                  nameof(ShowStopListen), nameof(HasSegment), nameof(SegConnected), nameof(SegAll), nameof(RosterHead) })
            OnPropertyChanged(p);
        if (rows) RebuildRows();
    }

    [RelayCommand] private void Join() => Card?.JoinCommand.Execute(null);
    [RelayCommand] private void Leave() => Card?.LeaveCommand.Execute(null);
    [RelayCommand] private void Emergency() => Card?.EmergencyCommand.Execute(null);
    [RelayCommand] private void CancelEmergency() => Card?.CancelEmergencyCommand.Execute(null);
    [RelayCommand] private void ToggleMute() => Card?.ToggleMuteCommand.Execute(null);
    [RelayCommand] private void ToggleRoute() { if (Card is not null) Card.ToggleRouteCommand.Execute(null); else Other?.ToggleRouteCommand.Execute(null); }
    [RelayCommand] private void ToggleListen() => Other?.ToggleListenCommand.Execute(null);
    [RelayCommand] private void ShowWindow() => Other?.ShowWindowCommand.Execute(null);
    [RelayCommand] private void OpenMessages() { if (Group is { } g) MessageRequested?.Invoke(this, g); }
    [RelayCommand] private void Edit() { if (Group is { } g) EditRequested?.Invoke(this, g); }
    [RelayCommand] private void Delete() { if (Group is { } g) DeleteRequested?.Invoke(this, g); }
    [RelayCommand] private void PersonMenu(ChannelPersonRow r) => PersonMenuRequested?.Invoke(this, r.Uri);
    [RelayCommand] private void PrivateCall(ChannelPersonRow r) => PrivateCallRequested?.Invoke(this, r.Number);
    [RelayCommand] private void Sds(ChannelPersonRow r) => SdsRequested?.Invoke(this, r.Number);
}

/// <summary>이벤트 상세 — 시각·채널·종류·내용 + 같은 채널의 앞뒤 이벤트 + [답장][채널 열기][이력에서 세션 보기].</summary>
public sealed record EventDetail(EventRow Row, IReadOnlyList<EventRow> Around)
{
    public string TimeText => Row.Time.ToString("yyyy-MM-dd HH:mm:ss");
    public string Channel => Row.Channel.Length > 0 ? Row.Channel : "—";
    public string KindText => Row.KindText;
    public string Content => Row.Content;
    public bool HasChannel => Row.Group is not null;
    public bool CanReply => Row.CanReply;
    public bool HasAround => Around.Count > 1;
}

public sealed partial class SidePanelViewModel : ObservableObject
{
    [ObservableProperty] private PanelView _view;
    /// <summary>고정(핀) — 모드·레일을 옮겨도 남는다(채널 상세·사용자·주소록만).</summary>
    [ObservableProperty] private bool _pinned;
    [ObservableProperty] private ChannelDetailViewModel? _channel;
    [ObservableProperty] private EventDetail? _event;
    /// <summary>새 PTT 그룹 폼(GroupAdminViewModel.Editor) — 패널이 호스팅하는 동안.</summary>
    [ObservableProperty] private GroupEditViewModel? _group;

    public bool IsOpen => View != PanelView.None;
    public bool IsChannel => View == PanelView.Channel;
    public bool IsUsers => View == PanelView.Users;
    public bool IsGroup => View == PanelView.Group;
    public bool IsEvent => View == PanelView.Event;
    public bool IsDirectory => View == PanelView.Directory;
    /// <summary>← — 패널 안에서 들어간 것(사용자 › 새 그룹)만.</summary>
    public bool HasBack => View == PanelView.Group && _backToUsers;
    public bool HasPin => View is PanelView.Channel or PanelView.Users or PanelView.Directory;
    public bool HasTag => View != PanelView.Group;
    public string Tag => View switch
    {
        PanelView.Channel => "채널 상세", PanelView.Users => "사용자", PanelView.Event => "이벤트 상세", PanelView.Directory => "주소록", _ => "",
    };
    public string Title => View switch
    {
        PanelView.Channel => Channel?.Title ?? "",
        PanelView.Users => $"사용자 {UsersCount}",
        PanelView.Group => "새 PTT 그룹",
        PanelView.Event => Event is { } e ? $"{e.KindText} · {e.Channel}" : "",
        PanelView.Directory => $"주소록 {DirectoryCount}",
        _ => "",
    };
    private bool _backToUsers;
    public int UsersCount { get; set; }
    public int DirectoryCount { get; set; }

    public event EventHandler? BackRequested;

    partial void OnViewChanged(PanelView value)
    {
        if (value is not (PanelView.Channel or PanelView.Users or PanelView.Directory)) Pinned = false;
        foreach (var p in new[] { nameof(IsOpen), nameof(IsChannel), nameof(IsUsers), nameof(IsGroup), nameof(IsEvent), nameof(IsDirectory), nameof(HasBack), nameof(HasPin), nameof(HasTag), nameof(Tag), nameof(Title) })
            OnPropertyChanged(p);
    }
    partial void OnChannelChanged(ChannelDetailViewModel? value) => OnPropertyChanged(nameof(Title));
    partial void OnEventChanged(EventDetail? value) => OnPropertyChanged(nameof(Title));

    public void Show(PanelView v, bool fromUsers = false) { _backToUsers = fromUsers; if (View == v) OnViewChanged(v); else View = v; }
    public void RefreshTitle() => OnPropertyChanged(nameof(Title));
    [RelayCommand] private void Close() => View = PanelView.None;
    [RelayCommand] private void TogglePin() => Pinned = !Pinned;
    [RelayCommand] private void Back() => BackRequested?.Invoke(this, EventArgs.Empty);
}
