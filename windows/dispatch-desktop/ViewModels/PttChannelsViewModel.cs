// ① 내 채널 — 멤버 그룹 전부 + 내가 건 개별 통화·애드혹 그룹 통화의 채널 카드(§4.1). 카드 = 코어 세션·그룹 로스터의 투영.
// 포커스(보는 채널, 카드 하나·테두리 Primary) ≠ 발언 대상(말하는 채널, 카드 왼쪽 체크 집합 — 발언 바 TalkBarViewModel 이 투영).
// 정렬은 핀 순서 고정(Ctrl+n 근육 기억) — 진행 중이라고 위로 올리지 않는다. 필터·검색 없음(항상 전부).
using System.Collections.ObjectModel;
using CimsUe;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public enum CardKind { Member, Private, Adhoc }

public sealed partial class ChannelCard : ObservableObject
{
    private readonly DispatchSession _s;
    public CardKind Kind { get; }
    public GroupInfo? Group { get; }
    [ObservableProperty] private SessionItem? _session;
    /// <summary>포커스 카드(3줄) — 하나만.</summary>
    [ObservableProperty] private bool _isSelected;
    /// <summary>발언 대상(체크) — 참여 중인 반이중 세션만 체크 가능.</summary>
    [ObservableProperty] private bool _isChecked;
    [ObservableProperty] private bool _deniedFlash;
    /// <summary>핀 번호(Ctrl+n) — 카드가 빠지면 다시 매겨진다.</summary>
    [ObservableProperty] private int _index;
    /// <summary>미읽음 SDS(④ 스레드) — 1줄 배지 ✉ n.</summary>
    [ObservableProperty] private int _unread;
    // 2줄 보조 — 마지막 발언(진행 중)·마지막 세션(대기)
    [ObservableProperty] private string _lastSpeaker = "";
    [ObservableProperty] private DateTime? _lastSpeakerAt;
    [ObservableProperty] private DateTime? _lastSessionEnd;
    [ObservableProperty] private TimeSpan _lastSessionLength;
    [ObservableProperty] private int _lastParticipants;

    // 그룹·세션은 카드보다 오래 산다 — 카드를 버릴 때 Detach 로 구독을 풀지 않으면 Rebuild 마다 옛 카드가 남아 계속 갱신된다
    private readonly System.ComponentModel.PropertyChangedEventHandler _onSource;

    public ChannelCard(DispatchSession s, GroupInfo group)
    {
        _s = s; Kind = CardKind.Member; Group = group;
        _onSource = (_, _) => Refresh();
        group.PropertyChanged += _onSource;
    }
    public ChannelCard(DispatchSession s, SessionItem session)
    {
        _s = s; Kind = session.Kind == SessionKind.PttPrivate ? CardKind.Private : CardKind.Adhoc; _session = session;
        _onSource = (_, _) => Refresh();
        session.PropertyChanged += _onSource;
    }

    /// <summary>카드 폐기 — 그룹·세션 PropertyChanged 구독 해제.</summary>
    public void Detach()
    {
        if (Group is not null) Group.PropertyChanged -= _onSource;
        if (Session is not null) Session.PropertyChanged -= _onSource;
    }

    public string Id => Group?.Id ?? Session?.Info.GroupId ?? Session?.CallId.ToString() ?? "";
    public string Title => Kind == CardKind.Member ? Group!.Name : Kind == CardKind.Adhoc ? "애드혹 · " + string.Join(", ", AdhocChips.Take(3)) + (AdhocChips.Count > 3 ? $" +{AdhocChips.Count - 3}" : "") : Session?.Title ?? "";
    public string Badge => Kind switch { CardKind.Member => "멤버", CardKind.Private => "개별", _ => "애드혹" };
    public string Duplex => Kind == CardKind.Private ? (Session?.IsFullDuplex == true ? "전이중" : "반이중") : "";
    public bool IsMember => Kind == CardKind.Member;
    public bool IsPrivate => Kind == CardKind.Private;
    public bool IsAdhoc => Kind == CardKind.Adhoc;
    public bool IsFullDuplex => Session?.IsFullDuplex == true;
    /// <summary>발언 대상이 될 수 있는가 — 참여 중 + 반이중(전이중은 항상 열린 마이크 → [음소거]).</summary>
    /// <summary>발언 대상 체크 가능 — 참여 중인 반이중 세션이고 발언 요청이 가능할 때(남이 연 일제 통화의 수신 멤버는 아니다 — Permission 0).</summary>
    public bool CanCheck => Session is not null && Session.IsLive && !IsFullDuplex && Session.CanRequestFloor;
    public string CheckTip => Session is { IsBroadcast: true } && !Session.CanRequestFloor ? "일제 통화 — 개시자만 발언합니다(수신 전용)" : "발언 대상(PTT 를 누르면 여기로 요청)";
    /// <summary>일제 통화(TS 24.379 §4.12) — 서버가 알린 호 속성(SessionItem.IsBroadcast).</summary>
    public bool IsBroadcast => Session?.IsBroadcast == true;
    public bool IsBroadcastInitiator => Session?.IsBroadcastInitiator == true;
    /// <summary>3줄 [일제 통화] — 멤버 편성 그룹에 진행 중 세션이 없을 때만. 일제 통화는 새 호를 여는 INVITE 로만 정해지고 진행 중 호는 일제로
    /// 바꿀 수 없다(TS 24.379 §10.1.1.3.1.1 15) — 진행 중 호의 INVITE 는 합류, mcptt_broadcast_group_call.md R13·§3.2). chat 그룹은 서버가 broadcast-ind 를 무시.</summary>
    public bool CanBroadcast => IsMember && !IsJoined && Group?.HasSession != true && Group?.IsChat != true;
    /// <summary>[일제 통화] 를 누르고 있다(잠금 발언이면 켜 두었다) — 개시되면 CanBroadcast 가 거짓이 되므로 버튼은 이것으로 활성을 유지한다.</summary>
    [ObservableProperty] private bool _isBroadcastHeld;
    public bool CanPressBroadcast => CanBroadcast || IsBroadcastHeld;
    public string BroadcastText => IsBroadcastHeld ? "일제 통화 중" : "일제 통화";
    partial void OnIsBroadcastHeldChanged(bool value) { OnPropertyChanged(nameof(CanPressBroadcast)); OnPropertyChanged(nameof(BroadcastText)); OnPropertyChanged(nameof(BroadcastTip)); }
    public string BroadcastTip => Group?.IsChat == true ? "채팅 그룹 — 일제 통화는 편성 그룹만"
                                : IsBroadcastHeld ? (_s.Settings.Current.LockTalk ? "일제 통화 중 — 다시 누르면 끝납니다" : "일제 통화 중 — 놓으면 끝납니다")
                                : IsJoined || Group?.HasSession == true ? "진행 중인 그룹 통화가 있습니다 — 끝난 뒤 개시(진행 중인 통화는 일제 통화로 바꿀 수 없습니다)"
                                : _s.Settings.Current.LockTalk ? "일제 통화 — 누르면 개시하고 바로 말합니다(개시자만 발언). 다시 누르면 끝납니다"
                                : "일제 통화 — 누르고 있는 동안 개시하고 말합니다(개시자만 발언). 놓으면 끝납니다";
    public string MemberText => Kind == CardKind.Member ? $"멤버 {Group!.MemberCount}" : "";
    public bool IsJoined => Session is not null && Session.IsLive;
    public bool IsActive => Session?.IsActive == true;
    public bool HasSession => IsJoined || (Group?.HasSession ?? false);
    /// <summary>1줄 오른쪽 끝 — 진행 중이면 경과, 아니면 "대기".</summary>
    public string ElapsedText => IsJoined ? DispatchSession.Fmt(Elapsed) : Group?.HasSession == true ? "진행(미참여)" : "대기";
    public int Participants => Group?.ConnectedCount ?? Session?.AdhocMembers.Count ?? 0;
    public string Speaker => Session?.Speaker ?? "";
    public bool HasSpeaker => Speaker.Length > 0;
    public TimeSpan SpeakerElapsed => Session?.SpeakerElapsed ?? TimeSpan.Zero;
    public TimeSpan Elapsed => Session?.Elapsed ?? TimeSpan.Zero;
    public bool IsEmergency => Session?.IsEmergency == true;
    public bool IsImminentPeril => Session?.IsImminentPeril == true;
    /// <summary>[긴급 호출] — 멤버 그룹, 긴급 아님, 자격(user profile allow-emergency-group-call — 못 받았으면 허용). 참여 중이면 진행 중 호의 조건 상향.</summary>
    public bool CanEmergency => IsMember && !IsEmergency && _s.Capabilities.EmergencyGroupCall;
    public string EmergencyTip => !_s.Capabilities.EmergencyGroupCall ? "긴급 그룹콜 자격이 없습니다(user profile)"
                                : IsJoined ? "진행 중인 이 그룹 통화를 긴급으로 올린다(조건 상향 — 그룹 능력이 꺼져 있으면 서버가 거절)"
                                : "이 그룹에 긴급 그룹콜 개시";
    /// <summary>[긴급 해제] — 내가 올린 조건만(DispatchSession.CanCancelCondition — 서버 과제 E1 뒤 넓힌다).</summary>
    public bool CanCancelEmergency => Session is not null && _s.CanCancelCondition(Session);
    public bool IsSpeaking => Session?.IsSpeaking == true;
    public bool IsRequesting => Session?.IsRequesting == true;
    public bool IsQueued => Session?.IsQueued == true;
    public double TalkGauge => Session?.TalkGauge ?? 0;
    public bool TalkLimitNear => Session?.TalkLimitNear == true;
    public string FloorNote => Session?.FloorNote ?? "";
    public bool RouteIsSpeaker => Session?.RouteIsSpeaker == true;
    public bool CanToggleRoute => Session is not null && _s.Audio.HasSpeaker;
    public bool IsMuted => Session?.Info.Muted == true;
    public bool HasUnread => Unread > 0;
    /// <summary>2줄 — 진행 중: [발언 없음 ·] 보조(① 마지막 발언·시각 / 애드혹 응답 n/m / 개별 통화 라우트·번호·발신 시각). 대기: 마지막 세션.</summary>
    public string Line2 => IsJoined ? string.Join(" · ", new[] { HasSpeaker ? "" : "발언 없음", Aux() }.Where(x => x.Length > 0)) : Group is null ? "" : LastSessionText;
    private string Aux() => Kind switch
    {
        CardKind.Adhoc => (IsBroadcast ? (IsBroadcastInitiator ? "일제 통화 · 발언을 놓으면 종료 · " : "일제 통화 · 수신 전용 · ") : "") + $"응답 {AdhocAnswered}/{Session!.AdhocMembers.Count}",
        CardKind.Private => $"{(RouteIsSpeaker ? "스피커" : "헤드셋")} · PTT {ShortNumber(Session!.PeerNumber)} · {(Session.Info.Dir == CallDir.Incoming ? "착신" : "발신")} {Session.StartedAt:HH:mm}",
        _ when IsBroadcast => IsBroadcastInitiator ? "일제 통화 · 발언을 놓으면 종료" : "일제 통화 · 수신 전용",
        _ => LastSpeaker.Length > 0 && LastSpeakerAt is DateTime t ? $"마지막 발언 {LastSpeaker} {t:HH:mm}" : $"참가 {Participants}",
    };
    private static string ShortNumber(string n) => n.Length > 4 ? "…" + n[^4..] : n;
    /// <summary>애드혹 응답 수 — 로스터가 없어 세션 상대(connected) 대신 그룹 로스터를 못 쓴다; 참여자 수는 코어 CallInfo 가 주지 않아 멤버 수로 상한.</summary>
    private int AdhocAnswered => Session is null ? 0 : Math.Min(Session.AdhocMembers.Count, _s.Groups.FirstOrDefault(g => g.Id == Session.Info.GroupId)?.ConnectedCount ?? 0);
    public string LastSessionText => LastSessionEnd is DateTime e ? $"마지막 세션 {e:HH:mm} · {DispatchSession.Fmt(LastSessionLength)} · 참가 {LastParticipants}"
                                     : Group?.HasSession == true ? $"세션 진행 중 · 참가 {Participants} · 미참여" : "세션 없음";
    /// <summary>3줄 로스터 칩 — 발언 중 녹색, 나 점선, 청취 멤버는 listenVisibility=visible 일 때만.</summary>
    public IReadOnlyList<RosterRow> Roster => (Group?.Roster ?? Array.Empty<RosterEntry>())
        .Where(r => r.Status != "listener" || !_s.ListenHidden)
        .Select(r => new RosterRow(_s.NameOfPtt(r.Uri), r.Uri, r.Status, _s.IsMe(r.Uri), Speaker.Length > 0 && _s.NameOfPtt(r.Uri) == Speaker)).ToList();
    /// <summary>3줄 칩은 앞 10명 + "+n" — 큰 그룹이 카드를 먹지 않게(전체는 [로스터 전체]).</summary>
    public IReadOnlyList<RosterRow> RosterPreview => Roster.OrderByDescending(r => r.IsSpeaking).ThenByDescending(r => r.IsMe).Take(10).ToList();
    public int RosterMore => Math.Max(0, Roster.Count - 10);
    public bool HasRosterMore => RosterMore > 0;
    public bool CanEdit => Group?.IsOwner == true;
    public IReadOnlyList<string> AdhocChips => Session?.AdhocMembers.Select(_s.NameOfPtt).ToList() ?? new List<string>();

    public void Refresh()
    {
        foreach (var p in new[] { nameof(Title), nameof(CanCheck), nameof(IsJoined), nameof(IsActive), nameof(HasSession), nameof(ElapsedText), nameof(Participants),
                                  nameof(Speaker), nameof(HasSpeaker), nameof(SpeakerElapsed), nameof(Elapsed), nameof(IsEmergency), nameof(IsImminentPeril), nameof(IsSpeaking),
                                  nameof(IsRequesting), nameof(IsQueued), nameof(TalkGauge), nameof(TalkLimitNear), nameof(FloorNote), nameof(RouteIsSpeaker),
                                  nameof(CanToggleRoute), nameof(IsMuted), nameof(Roster), nameof(AdhocChips), nameof(MemberText), nameof(IsFullDuplex), nameof(Duplex),
                                  nameof(Line2), nameof(LastSessionText), nameof(RosterPreview), nameof(RosterMore), nameof(HasRosterMore), nameof(CanEdit),
                                  nameof(CheckTip), nameof(IsBroadcast), nameof(IsBroadcastInitiator), nameof(CanBroadcast), nameof(BroadcastTip),
                                  nameof(CanPressBroadcast), nameof(CanEmergency), nameof(EmergencyTip), nameof(CanCancelEmergency) })
            OnPropertyChanged(p);
        if (HasSpeaker && Speaker != LastSpeaker) { LastSpeaker = Speaker; LastSpeakerAt = DateTime.Now; }
        else if (HasSpeaker) LastSpeakerAt ??= DateTime.Now;
    }

    /// <summary>세션(참여 또는 로스터 관측)이 끝났다 — 2줄 "마지막 세션" 갱신.</summary>
    public void RecordSessionEnd(TimeSpan length, int participants) { LastSessionEnd = DateTime.Now; LastSessionLength = length; LastParticipants = participants; OnPropertyChanged(nameof(Line2)); OnPropertyChanged(nameof(LastSessionText)); }

    partial void OnSessionChanging(SessionItem? value) { if (Session is not null && Session != value) Session.PropertyChanged -= _onSource; }
    partial void OnSessionChanged(SessionItem? value) { if (value is not null) value.PropertyChanged += _onSource; Refresh(); }
    partial void OnUnreadChanged(int value) => OnPropertyChanged(nameof(HasUnread));

    [RelayCommand] private void Join() { if (Group is not null) _s.JoinChannel(Group); }
    [RelayCommand] private void Leave() { if (Session is not null) { if (Kind == CardKind.Member) _s.LeaveChannel(Session); else _s.Hangup(Session); } }
    [RelayCommand] private void ToggleRoute() { if (Session is not null) _s.ToggleRoute(Session); }
    [RelayCommand] private void ToggleMute() { if (Session is not null) _s.ToggleMute(Session); }
    [RelayCommand] private void Emergency() { if (Group is not null) _s.EmergencyCall(Group); }
    [RelayCommand] private void CancelEmergency() { if (Session is not null) _s.CancelEmergency(Session); }
    [RelayCommand] private void CancelQueue() { if (Session is not null) _s.FloorQueueCancel(Session); }
}

public sealed record RosterRow(string Name, string Uri, string Status, bool IsMe, bool IsSpeaking)
{
    public string StatusText => Status switch { "connected" => "참여", "listener" => "청취", "on-hold" => "보류", _ => Status };
    public bool IsListener => Status == "listener";
    public string Number => Converters.UserPartConverter.UserPart(Uri);
}

public sealed partial class PttChannelsViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private ChannelCard? _previousSelection;

    public ObservableCollection<ChannelCard> Cards { get; } = new();
    /// <summary>포커스 카드(보는 채널) — ④ 스레드·⑤ 필터가 따라온다.</summary>
    [ObservableProperty] private ChannelCard? _selected;
    public int JoinedCount => Cards.Count(c => c.IsJoined);
    public int TargetCount => Cards.Count(c => c.IsChecked);
    public IEnumerable<ChannelCard> Targets => Cards.Where(c => c.IsChecked);

    /// <summary>floor 를 요청해 둔 세션 — 발언 대상(체크)과 따로 든다. 해제를 «현재 대상» 으로 하면 누른 채 대상이 바뀌었을 때
    /// (Ctrl+n·내가 건 호 우선·칩 ✕·세션 종료) 옛 채널에 floorRelease 가 가지 않아 그 채널로 마이크가 계속 나간다(마이크 차단은 코어
    /// floor participant 의 release 가 한다). 불변: 대상에서 빠진 세션은 반드시 floorRelease 를 받고 여기서 빠진다(OnTargetsChanged).</summary>
    private readonly HashSet<SessionItem> _talking = new();
    /// <summary>요청해 둔 세션 중 아직 발언·요청·대기 중인 것이 있는가 — 잠금 발언 해제 판정.</summary>
    public bool IsTalking => _talking.Any(x => x.IsSpeaking || x.IsRequesting || x.IsQueued);

    public event EventHandler<ChannelCard?>? SelectionChanged;
    /// <summary>발언 대상 집합이 바뀌었다(체크·해제·세션 종료).</summary>
    public event EventHandler? TargetsChanged;
    /// <summary>로스터 칩·카드 클릭 → 사람 메뉴(§4.1). 메뉴는 MainViewModel.People.</summary>
    public event EventHandler<string>? PersonMenuRequested;
    /// <summary>✉ n 배지 클릭 → ④ 스레드.</summary>
    public event EventHandler<GroupInfo>? ThreadRequested;
    /// <summary>3줄 [편집](내 소유 그룹) → 채널 편집 드로어.</summary>
    public event EventHandler<GroupInfo>? EditRequested;

    public PttChannelsViewModel(DispatchSession s)
    {
        _s = s;
        s.ProfileApplied += (_, _) => Rebuild();
        s.Groups.CollectionChanged += (_, _) => Rebuild();
        s.SessionAdded += (_, item) => OnSession(item, added: true);
        s.SessionEnded += (_, item) => OnSession(item, added: false);
        s.SessionChanged += (_, item) => { OnPropertyChanged(nameof(JoinedCount)); if (Cards.FirstOrDefault(c => c.Session == item) is { } c && c.IsChecked && !c.CanCheck) SetChecked(c, false); };
        s.RosterChanged += (_, g) => OnRoster(g);
        s.Floor += (_, e) => OnFloor(e.Session, e.Event);
        s.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(DispatchSession.Capabilities)) foreach (var c in Cards) c.Refresh(); };   // 긴급 호출 자격
    }

    public void Rebuild()
    {
        string? selId = Selected?.Id;
        var checkedIds = Cards.Where(c => c.IsChecked).Select(c => c.Id).ToHashSet(StringComparer.Ordinal);
        var memo = Cards.ToDictionary(c => c.Id, c => (c.LastSessionEnd, c.LastSessionLength, c.LastParticipants, c.LastSpeaker, c.LastSpeakerAt, c.Unread));
        foreach (var c in Cards) c.Detach();
        Cards.Clear();
        foreach (var g in _s.Groups.Where(g => g.IsMember))
            Cards.Add(new ChannelCard(_s, g) { Session = _s.SessionOfGroup(g.Id) });
        foreach (var sess in _s.Sessions.Where(x => x.Kind is SessionKind.PttPrivate or SessionKind.PttAdhoc))
            Cards.Add(new ChannelCard(_s, sess));
        foreach (var c in Cards)
        {
            if (memo.TryGetValue(c.Id, out var m)) { c.LastSessionEnd = m.LastSessionEnd; c.LastSessionLength = m.LastSessionLength; c.LastParticipants = m.LastParticipants; c.LastSpeaker = m.LastSpeaker; c.LastSpeakerAt = m.LastSpeakerAt; c.Unread = m.Unread; }
            if (checkedIds.Contains(c.Id) && c.CanCheck) c.IsChecked = true;
        }
        Renumber();
        Select(Cards.FirstOrDefault(c => c.Id == selId) ?? Cards.FirstOrDefault(), collapseSame: false);
        OnTargetsChanged();
    }

    private void Renumber() { int i = 1; foreach (var c in Cards) c.Index = i++; OnPropertyChanged(nameof(JoinedCount)); OnPropertyChanged(nameof(TargetCount)); }

    private void OnSession(SessionItem item, bool added)
    {
        switch (item.Kind)
        {
            case SessionKind.PttChannel:
                var card = Cards.FirstOrDefault(c => c.IsMember && c.Group!.Id == item.Info.GroupId);
                if (card is not null)
                {
                    if (!added) { card.RecordSessionEnd(item.Elapsed, Math.Max(card.Participants, card.LastParticipants)); if (card.IsChecked) SetChecked(card, false); }
                    if (!added && _bcHold is { } h && h.Card == card && h.CallId == item.CallId) ClearHold(h);    // 서버·코어가 먼저 끝냈다
                    card.Session = added ? item : null;
                    // 일제 통화 개시 — 곧 말하려는 채널: 포커스 + 단일 발언 대상("내가 건 호 우선"과 같은 규칙)
                    if (added && item.Operation == Operation.Broadcast) { Select(card, collapseSame: false); SetSingleTarget(card); }
                }
                break;
            case SessionKind.PttPrivate:
            case SessionKind.PttAdhoc:
                if (added)
                {
                    var c = new ChannelCard(_s, item);
                    Cards.Add(c);
                    Renumber();
                    // "내가 건 호 우선" 규칙 — 내가 건 애드혹 그룹 통화·반이중 개별 통화는 자동 포커스 + 단일 발언 대상(발신자가 곧 말하려는 채널)
                    if (c.IsAdhoc || !c.IsFullDuplex)
                    {
                        _previousSelection = Selected; Select(c, collapseSame: false);
                        if (item.Info.Dir == CallDir.Outgoing) SetSingleTarget(c);
                    }
                }
                else
                {
                    var c = Cards.FirstOrDefault(x => x.Session == item);
                    if (c is not null)
                    {
                        c.Detach();
                        bool wasTarget = c.IsChecked;
                        Cards.Remove(c);
                        Renumber();
                        if (Selected == c) Select(_previousSelection is not null && Cards.Contains(_previousSelection) ? _previousSelection : Cards.FirstOrDefault(), collapseSame: false);
                        if (wasTarget) OnTargetsChanged();
                    }
                }
                break;
        }
        OnPropertyChanged(nameof(JoinedCount));
    }

    /// <summary>미참여 멤버 그룹의 세션 관측(로스터)이 끝나면 2줄 "마지막 세션".</summary>
    private void OnRoster(GroupInfo g)
    {
        var c = Cards.FirstOrDefault(x => x.Group == g);
        if (c is null) return;
        if (!g.HasSession && c.Session is null && _rosterStart.Remove(g.Id, out var started)) c.RecordSessionEnd(DateTime.Now - started.At, started.Max);
        else if (g.HasSession && c.Session is null)
        {
            if (!_rosterStart.TryGetValue(g.Id, out var st)) _rosterStart[g.Id] = (DateTime.Now, g.ConnectedCount);
            else if (g.ConnectedCount > st.Max) _rosterStart[g.Id] = (st.At, g.ConnectedCount);
        }
        c.Refresh();
    }
    private readonly Dictionary<string, (DateTime At, int Max)> _rosterStart = new(StringComparer.Ordinal);

    private void OnFloor(SessionItem s, FloorEvent ev)
    {
        var c = Cards.FirstOrDefault(x => x.Session == s);
        if (c is null) return;
        c.Refresh();
        if (ev.Kind == FloorEventKind.Denied) { c.DeniedFlash = true; _ = Task.Delay(1000).ContinueWith(_ => c.DeniedFlash = false, TaskScheduler.FromCurrentSynchronizationContext()); }
        if (c.IsChecked && !c.CanCheck) SetChecked(c, false);        // Permission 0(일제 통화 수신)이 오면 발언 대상에서 뺀다
        TargetsChanged?.Invoke(this, EventArgs.Empty);
    }

    // ── 포커스 ──
    /// <summary>카드 클릭 — 같은 카드를 다시 클릭하면 접힌다(포커스 없음). Ctrl+n·자동 포커스는 접지 않는다.</summary>
    public void Select(ChannelCard? c, bool collapseSame = true)
    {
        if (collapseSame && c is not null && Selected == c) c = null;
        foreach (var x in Cards) x.IsSelected = x == c;
        Selected = c;
        SelectionChanged?.Invoke(this, c);
    }

    [RelayCommand] private void SelectCard(ChannelCard c) => Select(c);
    /// <summary>Ctrl+n — 카드 n 포커스 + 발언 대상을 그 채널 하나로(단일 모드 복귀).</summary>
    public void SelectIndex(int n)
    {
        var c = Cards.FirstOrDefault(x => x.Index == n);
        if (c is null) return;
        Select(c, collapseSame: false);
        if (c.CanCheck) SetSingleTarget(c);
    }
    /// <summary>Ctrl+Shift+n — 카드 n 체크 토글(다중 추가).</summary>
    public void ToggleIndex(int n) { var c = Cards.FirstOrDefault(x => x.Index == n); if (c is not null) ToggleTarget(c); }

    // ── 발언 대상 ──
    // 동시 발언 = 단말 팬아웃(§4.1) — 3GPP 에 UE 의 다중 그룹 동시 발언 절차가 없어 대상 세션마다 floor 를 따로 요청하고,
    // 코어가 승인된 세션마다 같은 마이크를 결선한다(세션별 floor participant·micOpen). 상한 없음, 서버 변경 없음.
    [RelayCommand]
    private void ToggleTarget(ChannelCard c)
    {
        if (!c.CanCheck) { if (c.Session is null && c.Group is not null) _s.Notify.Info($"{c.Title} — 먼저 [참여]하세요"); return; }
        SetChecked(c, !c.IsChecked);
    }

    public void SetSingleTarget(ChannelCard c)
    {
        foreach (var x in Cards) x.IsChecked = x == c && c.CanCheck;
        OnTargetsChanged();
    }

    private void SetChecked(ChannelCard c, bool on)
    {
        if (c.IsChecked == on) return;
        c.IsChecked = on;
        OnTargetsChanged();
    }

    [RelayCommand] private void ClearTargets() { foreach (var x in Cards) x.IsChecked = false; OnTargetsChanged(); }

    /// <summary>대상 변경은 전부 여기를 지난다 — 대상에서 빠진 세션의 floor 를 놓고(_talking 불변) 알린다.</summary>
    private void OnTargetsChanged()
    {
        foreach (var x in _talking.Where(x => !Cards.Any(c => c.IsChecked && c.Session == x)).ToList()) Release(x);
        OnPropertyChanged(nameof(TargetCount));
        TargetsChanged?.Invoke(this, EventArgs.Empty);
    }

    // ── 발언(floor) ──
    /// <summary>PTT 누름 — 대상 전부에 floorRequest. 이전 요청이 남아 있으면(대상 밖) 먼저 푼다.</summary>
    public void PttDown()
    {
        var sessions = Cards.Where(c => c.IsChecked && c.CanCheck).Select(c => c.Session!).ToList();
        foreach (var x in _talking.Except(sessions).ToList()) Release(x);
        foreach (var x in sessions) { _talking.Add(x); _s.FloorRequest(x); }
    }
    /// <summary>PTT 뗌 — 요청해 둔 세션 전부 해제. 상태를 보지 않는다(요청 직후 Granted 전에 떼도 반드시 놓는다 — 코어 release 는 유휴에서 no-op).</summary>
    public void PttRelease() { foreach (var x in _talking.ToList()) Release(x); }
    private void Release(SessionItem x) { _talking.Remove(x); if (x.IsLive) _s.FloorRelease(x); }

    // ── 일제 통화 한 버튼(§4.1) — 누르는 동안 개시하고 말하며 놓으면 끝(잠금 발언이면 누를 때마다 켜고 끈다). 개시 INVITE 가 암묵적 발언 요청이라
    //   (TS 24.380 §14.2.5) PTT 를 따로 누르지 않는다. 한 번에 하나. 개시 전(그룹 종류 조회 중)에 놓으면 개시 직후 끝낸다.
    private sealed class BroadcastHold { public required ChannelCard Card; public int CallId = -1; public bool Released; }
    private BroadcastHold? _bcHold;

    public async void BroadcastDown(ChannelCard c)
    {
        if (_bcHold is not null) { if (_s.Settings.Current.LockTalk && _bcHold.Card == c) BroadcastEnd(); return; }
        if (c.Group is null || !c.CanBroadcast) return;
        var h = _bcHold = new BroadcastHold { Card = c };
        c.IsBroadcastHeld = true;
        int id;
        try { id = await _s.BroadcastCallAsync(c.Group); }
        catch (Exception ex) { _s.Log.Error("broadcast start", ex); ClearHold(h); return; }     // async void — 예외를 흘리면 앱이 죽는다
        if (_bcHold != h) return;
        if (id < 0) { ClearHold(h); return; }
        h.CallId = id;
        if (h.Released) FinishHold(h);
    }
    public void BroadcastUp() { if (!_s.Settings.Current.LockTalk) BroadcastEnd(); }
    private void BroadcastEnd()
    {
        var h = _bcHold;
        if (h is null) return;
        h.Released = true;
        if (h.CallId >= 0) FinishHold(h);
    }
    private void FinishHold(BroadcastHold h)
    {
        ClearHold(h);
        if (_talking.FirstOrDefault(x => x.CallId == h.CallId) is { } t) _talking.Remove(t);
        _s.ReleaseBroadcast(h.CallId);
    }
    private void ClearHold(BroadcastHold h) { if (_bcHold == h) _bcHold = null; h.Card.IsBroadcastHeld = false; }

    [RelayCommand] private void PersonMenu(RosterRow r) => PersonMenuRequested?.Invoke(this, r.Uri);
    [RelayCommand] private void OpenThread(ChannelCard c) { if (c.Group is not null) ThreadRequested?.Invoke(this, c.Group); }
    [RelayCommand] private void EditGroup(ChannelCard c) { if (c.Group is not null) EditRequested?.Invoke(this, c.Group); }

    public void Tick() { foreach (var c in Cards) if (c.Session is not null) c.Refresh(); }

    /// <summary>④ 미읽음 → 카드 배지.</summary>
    public void SetUnread(Func<GroupInfo, int> unreadOf) { foreach (var c in Cards) if (c.Group is not null) c.Unread = unreadOf(c.Group); }

    /// <summary>[채널로] — ① 카드(멤버 그룹·내 개별/애드혹)로 포커스만 옮긴다. 합류하지 않는다 — 참여는 카드의 [참여]다.
    /// ① 에 카드가 없으면(청취 범위 그룹 등) false — 호출자가 ② 카드로 옮긴다(비멤버 sendrecv 합류는 서버가 403, TS 24.379 §10.1.1).</summary>
    public bool FocusGroup(string groupId)
    {
        var c = Cards.FirstOrDefault(x => x.Id == groupId);
        if (c is null) return false;
        Select(c, collapseSame: false);
        return true;
    }
}
