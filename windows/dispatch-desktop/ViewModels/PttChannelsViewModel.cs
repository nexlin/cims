// ① PTT 채널 — 왼쪽 채널 카드(멤버 그룹·사설콜·애드혹, §4.1). 카드 = 코어 세션·그룹 로스터의 투영. 선택 채널이 전역 PTT 핫키의 대상.
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
    [ObservableProperty] private bool _isSelected;
    [ObservableProperty] private bool _showRoster;
    [ObservableProperty] private bool _deniedFlash;
    public int Index { get; set; }

    public ChannelCard(DispatchSession s, GroupInfo group) { _s = s; Kind = CardKind.Member; Group = group; group.PropertyChanged += (_, _) => Refresh(); }
    public ChannelCard(DispatchSession s, SessionItem session)
    {
        _s = s; Kind = session.Kind == SessionKind.PttPrivate ? CardKind.Private : CardKind.Adhoc; _session = session;
        session.PropertyChanged += (_, _) => Refresh();
    }

    public string Id => Group?.Id ?? Session?.Info.GroupId ?? Session?.CallId.ToString() ?? "";
    public string Title => Kind == CardKind.Member ? Group!.Name : Kind == CardKind.Adhoc ? $"애드혹 · {Session?.AdhocMembers.Count}명" : Session?.Title ?? "";
    public string Badge => Kind switch { CardKind.Member => "멤버", CardKind.Private => "사설콜", _ => "임시" };
    public string Duplex => Kind == CardKind.Private ? (Session?.IsFullDuplex == true ? "전이중" : "반이중") : "";
    public bool IsMember => Kind == CardKind.Member;
    public bool IsPrivate => Kind == CardKind.Private;
    public bool IsAdhoc => Kind == CardKind.Adhoc;
    public bool IsFullDuplex => Session?.IsFullDuplex == true;
    public bool ShowPtt => Session is not null && Session.IsLive && !IsFullDuplex;
    public string MemberText => Kind == CardKind.Member ? $"멤버 {Group!.MemberCount}" : "";
    public string AffiliationText => Kind == CardKind.Member ? (Group!.Affiliated ? "affiliated" : "") : "";
    public bool IsJoined => Session is not null && Session.IsLive;
    public bool IsActive => Session?.IsActive == true;
    public bool HasSession => IsJoined || (Group?.HasSession ?? false);
    public string SessionText => IsJoined ? (Session!.IsActive ? "진행" : Session.StateText) : (Group?.HasSession == true ? "진행(미참여)" : "대기");
    public int Participants => Group?.ConnectedCount ?? Session?.AdhocMembers.Count ?? 0;
    public string Speaker => Session?.Speaker ?? "";
    public bool HasSpeaker => Speaker.Length > 0;
    public TimeSpan SpeakerElapsed => Session?.SpeakerElapsed ?? TimeSpan.Zero;
    public TimeSpan Elapsed => Session?.Elapsed ?? TimeSpan.Zero;
    public bool IsEmergency => Session?.IsEmergency == true;
    public bool IsImminentPeril => Session?.IsImminentPeril == true;
    public bool IsSpeaking => Session?.IsSpeaking == true;
    public bool IsRequesting => Session?.IsRequesting == true;
    public bool IsQueued => Session?.IsQueued == true;
    public double TalkGauge => Session?.TalkGauge ?? 0;
    public bool TalkLimitNear => Session?.TalkLimitNear == true;
    public string FloorNote => Session?.FloorNote ?? "";
    public bool RouteIsSpeaker => Session?.RouteIsSpeaker == true;
    public bool CanToggleRoute => Session is not null && _s.Audio.HasSpeaker;
    public bool IsMuted => Session?.Info.Muted == true;
    public string PttText => IsSpeaking ? $"발언 중 {DispatchSession.Fmt(SpeakerElapsed)}" : IsRequesting ? "요청 중" : IsQueued ? FloorNote : DeniedFlash ? "거부" : "PTT";
    public IReadOnlyList<RosterRow> Roster => (Group?.Roster ?? Array.Empty<RosterEntry>())
        .Select(r => new RosterRow(_s.NameOfPtt(r.Uri), r.Uri, r.Status)).ToList();
    public IReadOnlyList<string> AdhocChips => Session?.AdhocMembers.Select(_s.NameOfPtt).ToList() ?? new List<string>();

    public void Refresh()
    {
        foreach (var p in new[] { nameof(Title), nameof(ShowPtt), nameof(IsJoined), nameof(IsActive), nameof(HasSession), nameof(SessionText), nameof(Participants),
                                  nameof(Speaker), nameof(HasSpeaker), nameof(SpeakerElapsed), nameof(Elapsed), nameof(IsEmergency), nameof(IsImminentPeril), nameof(IsSpeaking),
                                  nameof(IsRequesting), nameof(IsQueued), nameof(TalkGauge), nameof(TalkLimitNear), nameof(FloorNote), nameof(RouteIsSpeaker),
                                  nameof(CanToggleRoute), nameof(IsMuted), nameof(PttText), nameof(Roster), nameof(AdhocChips), nameof(MemberText), nameof(AffiliationText),
                                  nameof(IsFullDuplex), nameof(Duplex) })
            OnPropertyChanged(p);
    }

    partial void OnSessionChanged(SessionItem? value) { if (value is not null) value.PropertyChanged += (_, _) => Refresh(); Refresh(); }
    partial void OnDeniedFlashChanged(bool value) => OnPropertyChanged(nameof(PttText));

    [RelayCommand] private void Join() { if (Group is not null) _s.JoinChannel(Group); }
    [RelayCommand] private void Leave() { if (Session is not null) { if (Kind == CardKind.Member) _s.LeaveChannel(Session); else _s.Hangup(Session); } }
    [RelayCommand] private void ToggleRoute() { if (Session is not null) _s.ToggleRoute(Session); }
    [RelayCommand] private void ToggleMute() { if (Session is not null) _s.ToggleMute(Session); }
    [RelayCommand] private void ToggleRosterView() => ShowRoster = !ShowRoster;
    [RelayCommand] private void Emergency() { if (Group is not null) _s.EmergencyCall(Group); }
    [RelayCommand] private void CancelQueue() { if (Session is not null) _s.FloorQueueCancel(Session); }

    public void PttDown() { if (Session is not null && ShowPtt) _s.FloorRequest(Session); }
    public void PttUp() { if (Session is not null && ShowPtt && (IsSpeaking || IsRequesting)) _s.FloorRelease(Session); }
}

public sealed record RosterRow(string Name, string Uri, string Status)
{
    public string StatusText => Status switch { "connected" => "참여", "listener" => "청취", "on-hold" => "보류", _ => Status };
    public bool IsListener => Status == "listener";
}

public sealed partial class PttChannelsViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private ChannelCard? _previousSelection;

    public ObservableCollection<ChannelCard> Cards { get; } = new();
    [ObservableProperty] private ChannelCard? _selected;
    [ObservableProperty] private string _viewMode = "card";
    public bool IsTile => ViewMode == "tile";
    public int JoinedCount => Cards.Count(c => c.IsJoined);

    public event EventHandler<ChannelCard?>? SelectionChanged;

    public PttChannelsViewModel(DispatchSession s)
    {
        _s = s;
        _viewMode = s.Settings.Current.ChannelViewMode;
        s.ProfileApplied += (_, _) => Rebuild();
        s.Groups.CollectionChanged += (_, _) => Rebuild();
        s.SessionAdded += (_, item) => OnSession(item, added: true);
        s.SessionEnded += (_, item) => OnSession(item, added: false);
        s.SessionChanged += (_, _) => OnPropertyChanged(nameof(JoinedCount));
        s.Floor += (_, e) => OnFloor(e.Session, e.Event);
        s.Settings.Changed += (_, _) => { ViewMode = s.Settings.Current.ChannelViewMode; Rebuild(); };
    }

    partial void OnViewModeChanged(string value) => OnPropertyChanged(nameof(IsTile));

    public void Rebuild()
    {
        string? selId = Selected?.Id;
        Cards.Clear();
        var chosen = _s.Settings.Current.SelectedChannels;
        foreach (var g in _s.Groups.Where(g => g.IsMember && (chosen.Count == 0 || chosen.Contains(g.Id))))
            Cards.Add(new ChannelCard(_s, g) { Session = _s.SessionOfGroup(g.Id) });
        foreach (var sess in _s.Sessions.Where(x => x.Kind is SessionKind.PttPrivate or SessionKind.PttAdhoc))
            Cards.Add(new ChannelCard(_s, sess));
        Renumber();
        Select(Cards.FirstOrDefault(c => c.Id == selId) ?? Cards.FirstOrDefault());
    }

    private void Renumber() { int i = 1; foreach (var c in Cards) c.Index = i++; OnPropertyChanged(nameof(JoinedCount)); }

    private void OnSession(SessionItem item, bool added)
    {
        switch (item.Kind)
        {
            case SessionKind.PttChannel:
                var card = Cards.FirstOrDefault(c => c.IsMember && c.Group!.Id == item.Info.GroupId);
                if (card is not null) card.Session = added ? item : null;
                break;
            case SessionKind.PttPrivate:
            case SessionKind.PttAdhoc:
                if (added)
                {
                    var c = new ChannelCard(_s, item);
                    Cards.Add(c);
                    Renumber();
                    if (c.IsAdhoc || !c.IsFullDuplex) { _previousSelection = Selected; Select(c); }      // "애드혹 우선" 규칙
                }
                else
                {
                    var c = Cards.FirstOrDefault(x => x.Session == item);
                    if (c is not null)
                    {
                        Cards.Remove(c);
                        Renumber();
                        if (Selected == c) Select(_previousSelection is not null && Cards.Contains(_previousSelection) ? _previousSelection : Cards.FirstOrDefault());
                    }
                }
                break;
        }
        OnPropertyChanged(nameof(JoinedCount));
    }

    private void OnFloor(SessionItem s, FloorEvent ev)
    {
        var c = Cards.FirstOrDefault(x => x.Session == s);
        if (c is null) return;
        c.Refresh();
        if (ev.Kind == FloorEventKind.Denied) { c.DeniedFlash = true; _ = Task.Delay(1000).ContinueWith(_ => c.DeniedFlash = false, TaskScheduler.FromCurrentSynchronizationContext()); }
    }

    public void Select(ChannelCard? c)
    {
        foreach (var x in Cards) x.IsSelected = x == c;
        Selected = c;
        SelectionChanged?.Invoke(this, c);
    }

    [RelayCommand] private void SelectCard(ChannelCard c) => Select(c);
    public void SelectIndex(int n) { var c = Cards.FirstOrDefault(x => x.Index == n); if (c is not null) Select(c); }
    [RelayCommand] private void SetViewMode(string mode) { ViewMode = mode; _s.Settings.Update(x => x.ChannelViewMode = mode); }

    /// <summary>전역 PTT 키 — 선택 채널. 미참여 멤버 채널이면 먼저 참여만.</summary>
    public void PttDown()
    {
        if (Selected is null) return;
        if (Selected.Session is null && Selected.Group is not null) { _s.JoinChannel(Selected.Group); return; }
        Selected.PttDown();
    }
    public void PttUp() => Selected?.PttUp();

    public void Tick() { foreach (var c in Cards) if (c.Session is not null) c.Refresh(); }

    public void FocusGroup(string groupId)
    {
        var c = Cards.FirstOrDefault(x => x.Id == groupId);
        if (c is not null) Select(c);
        else if (_s.Groups.FirstOrDefault(g => g.Id == groupId) is { } g) _s.JoinChannel(g);
    }
}
