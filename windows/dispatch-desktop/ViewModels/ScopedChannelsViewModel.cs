// ② 범위 채널(§4.2) — 내 멤버 그룹이 아닌 채널 카드: 청취 범위 그룹(프로비저닝 pttTargets → conference 구독) · 타인 간 사설콜·애드혹(서버 과제, §13 — 빈 섹션) ·
// 관리 범위·내 소유 그룹(멤버·청취 범위가 아니면 세션 상태 없음, 기본 접힘). 필터·검색은 이 패널에만. 청취는 카드 안 토글([🔊 청취 중]) + [창으로].
// [편집]·[+ 새 채널] 은 채널 편집 드로어(GroupEditViewModel — [PTT 그룹] 화면과 같은 VM).
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public enum ScopedSection { Listen, Others, Manage }

public sealed partial class ScopedCard : ObservableObject
{
    private readonly DispatchSession _s;
    public ScopedSection Section { get; }
    /// <summary>청취 범위 그룹(세션 구독 대상) — 관리 전용 카드는 null.</summary>
    public GroupInfo? Group { get; }
    /// <summary>관리 범위 목록의 항목(소유·관리 가능 여부) — 청취 범위에도 있으면 같은 카드에 겹친다.</summary>
    public ManagedGroup? Managed { get; }
    [ObservableProperty] private bool _isSelected;

    public ScopedCard(DispatchSession s, ScopedSection section, GroupInfo? g, ManagedGroup? m) { _s = s; Section = section; Group = g; Managed = m; }

    public string Id => Group?.Id ?? Managed?.Id ?? "";
    public string Title => Group?.Name ?? Managed?.Name ?? Id;
    public bool IsListenScope => Group is not null;
    public bool IsManageScope => Managed is { CanManage: true };
    public bool IsOwner => Group?.IsOwner == true || Managed?.IsOwner == true;
    public bool IsManageOnly => Section == ScopedSection.Manage;
    public SessionItem? Listen => Group is null ? null : _s.ListenOfGroup(Group.Id);
    public bool IsListening => Listen is not null;
    public bool HasSession => Group?.HasSession == true || IsListening;
    public bool IsEmergency => Listen?.IsEmergency == true;
    public bool IsImminentPeril => Listen?.IsImminentPeril == true;
    public int Participants => Group?.ConnectedCount ?? 0;
    public string Speaker => Listen?.Speaker ?? "";
    public bool HasSpeaker => Speaker.Length > 0;
    public TimeSpan SpeakerElapsed => Listen?.SpeakerElapsed ?? TimeSpan.Zero;
    /// <summary>참여/청취하지 않는 그룹의 경과는 로스터로 세션을 관측한 시점부터.</summary>
    public TimeSpan? Elapsed => Listen?.Elapsed ?? (Group?.SessionSince is DateTime t ? DateTime.Now - t : null);
    public string ElapsedText => Elapsed is TimeSpan e ? DispatchSession.Fmt(e) : IsManageOnly ? "—" : "대기";
    public bool CanListen => IsListenScope && HasSession && !IsListening && _s.CanListenPtt;
    public string ListenTip => !IsListenScope ? "청취 범위 밖" : !HasSession ? "진행 중인 그룹 통화가 없습니다" : IsListening ? "청취 중 — 클릭하면 청취 종료" : "청취 전용 합류(recvonly)";
    public bool CanEdit => IsManageScope || IsOwner;
    public bool RouteIsSpeaker => Listen?.RouteIsSpeaker == true;
    public string OwnerText => Managed?.IsOwner == true ? "내 소유" : "";
    /// <summary>2줄 — 진행: 발언자 ▮ mm:ss · 긴급 개시자·시각 / 세션 진행 중 · 발언 없음 · 참가 n. 대기: 마지막 세션. 상태 없음(관리 범위만).</summary>
    public string Line2 => IsManageOnly ? $"상태 없음 · 구독 범위 밖{(OwnerText.Length > 0 ? " · " + OwnerText : "")}"
                          : HasSession ? (HasSpeaker ? (IsEmergency && Listen is not null ? $"긴급 개시 {Listen.StartedAt:HH:mm}" : $"참가 {Participants}") : $"세션 진행 중 · 발언 없음 · 참가 {Participants}")
                          : LastSessionEnd is DateTime le ? $"마지막 세션 {le:HH:mm} · {DispatchSession.Fmt(LastSessionLength)}" : "세션 없음";
    [ObservableProperty] private DateTime? _lastSessionEnd;
    [ObservableProperty] private TimeSpan _lastSessionLength;

    public void Refresh()
    {
        foreach (var p in new[] { nameof(Title), nameof(IsListening), nameof(HasSession), nameof(IsEmergency), nameof(IsImminentPeril), nameof(Participants), nameof(Speaker), nameof(HasSpeaker),
                                  nameof(SpeakerElapsed), nameof(Elapsed), nameof(ElapsedText), nameof(CanListen), nameof(ListenTip), nameof(CanEdit), nameof(RouteIsSpeaker), nameof(Line2) })
            OnPropertyChanged(p);
    }

    [RelayCommand] private void ToggleListen() { if (Group is null) return; if (Listen is { } l) _s.Hangup(l); else if (CanListen) _s.ListenGroup(Group); }
    [RelayCommand] private void ToggleRoute() { if (Listen is { } l) _s.ToggleRoute(l); }
    [RelayCommand] private void ShowWindow() { if (Listen is { } l) WindowRequested?.Invoke(this, l); }
    [RelayCommand] private void Edit() => EditRequested?.Invoke(this, ToGroupInfo());
    [RelayCommand] private void Delete() => DeleteRequested?.Invoke(this, ToGroupInfo());
    public GroupInfo ToGroupInfo() => Group ?? new GroupInfo(Managed!.Id, Managed.Uri, Managed.Name, Managed.MemberCount) { IsOwner = Managed.IsOwner, Etag = Managed.ETag };
    public event EventHandler<SessionItem>? WindowRequested;
    public event EventHandler<GroupInfo>? EditRequested;
    public event EventHandler<GroupInfo>? DeleteRequested;
}

public sealed partial class ScopedChannelsViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly GroupAdminViewModel _groups;
    private readonly Dictionary<string, (DateTime? End, TimeSpan Len, DateTime? Since)> _memo = new(StringComparer.Ordinal);

    public ObservableCollection<ScopedCard> Listen { get; } = new();
    public ObservableCollection<ScopedCard> Others { get; } = new();
    public ObservableCollection<ScopedCard> Manage { get; } = new();
    /// <summary>all | active | emergency | listen | manage</summary>
    [ObservableProperty] private string _filter = "all";
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private bool _manageExpanded;
    [ObservableProperty] private ScopedCard? _selected;

    public event EventHandler<SessionItem>? WindowRequested;
    public event EventHandler<GroupInfo>? EditRequested;
    public event EventHandler<GroupInfo>? DeleteRequested;
    public event EventHandler? NewChannelRequested;

    public ScopedChannelsViewModel(DispatchSession s, GroupAdminViewModel groups)
    {
        _s = s; _groups = groups;
        _manageExpanded = s.Settings.Current.ScopedManageExpanded;
        s.Groups.CollectionChanged += (_, _) => Rebuild();
        s.RosterChanged += (_, g) => OnRoster(g);
        s.SessionAdded += (_, x) => { if (x.Kind == SessionKind.PttListen) Rebuild(); };
        s.SessionEnded += (_, x) => { if (x.Kind == SessionKind.PttListen) Rebuild(); };
        s.SessionChanged += (_, _) => Tick();
        s.ProfileApplied += (_, _) => Rebuild();
        groups.Loaded += (_, _) => Rebuild();
        Rebuild();
    }

    public int ListenCount => Listen.Count;
    public int OthersCount => Others.Count;
    public int ManageCount => Manage.Count;
    public int ListeningCount => _s.Sessions.Count(x => x.Kind == SessionKind.PttListen);
    public int ListenLimit => _s.Settings.Current.MaxMonitorWindows;
    public string HeaderCounts => $"청취 {ListenCount} · 관리 {ManageCount} · 타인 {OthersCount}";
    public string ListeningText => $"동시 청취 {ListeningCount}/{ListenLimit}";
    public bool CanCreate => _s.CanCreateGroups;
    public bool HasNoScope => !_s.CanListenPtt && ManageCount == 0;
    public string OthersHint => "타인 간 사설콜·애드혹 세션 가시성은 서버 과제(§13) — 서버가 세션 목록을 주면 여기 보입니다";

    partial void OnFilterChanged(string value) => Rebuild();
    partial void OnSearchChanged(string value) => Rebuild();
    partial void OnManageExpandedChanged(bool value) => _s.Settings.Update(x => x.ScopedManageExpanded = value);
    [RelayCommand] private void SetFilter(string f) => Filter = f;
    [RelayCommand] private void ToggleManage() => ManageExpanded = !ManageExpanded;
    [RelayCommand] private void NewChannel() => NewChannelRequested?.Invoke(this, EventArgs.Empty);
    [RelayCommand] private void SelectCard(ScopedCard c) { var next = Selected == c ? null : c; foreach (var x in All) x.IsSelected = x == next; Selected = next; }
    private IEnumerable<ScopedCard> All => Listen.Concat(Others).Concat(Manage);

    private void OnRoster(GroupInfo g)
    {
        if (g.IsMember) return;
        var m = _memo.GetValueOrDefault(g.Id);
        if (g.HasSession && m.Since is null) _memo[g.Id] = (m.End, m.Len, DateTime.Now);
        else if (!g.HasSession && m.Since is DateTime since && _s.ListenOfGroup(g.Id) is null) _memo[g.Id] = (DateTime.Now, DateTime.Now - since, null);
        var card = All.FirstOrDefault(c => c.Id == g.Id);
        if (card is null) { Rebuild(); return; }
        ApplyMemo(card); card.Refresh();
        Resort(card.Section == ScopedSection.Listen ? Listen : Manage);
    }

    private void ApplyMemo(ScopedCard c) { if (_memo.TryGetValue(c.Id, out var m)) { c.LastSessionEnd = m.End; c.LastSessionLength = m.Len; } }

    public void Rebuild()
    {
        string? sel = Selected?.Id;
        string q = Search.Trim();
        bool Match(string title, string id) => q.Length == 0 || title.Contains(q, StringComparison.OrdinalIgnoreCase) || id.Contains(q, StringComparison.OrdinalIgnoreCase);
        Listen.Clear(); Others.Clear(); Manage.Clear();
        var managed = _groups.All.ToDictionary(g => g.Id, g => g, StringComparer.Ordinal);
        // 청취 범위 — 프로비저닝 pttTargets(멤버가 아닌 그룹)
        foreach (var g in _s.Groups.Where(g => !g.IsMember))
        {
            var c = new ScopedCard(_s, ScopedSection.Listen, g, managed.GetValueOrDefault(g.Id));
            ApplyMemo(c);
            if (!Match(c.Title, c.Id) || !PassFilter(c)) continue;
            Wire(c); Listen.Add(c);
        }
        // 관리 범위·내 소유(멤버·청취 범위가 아닌 것만 — 멤버는 ①, 청취 범위는 위 섹션)
        var memberIds = _s.Groups.Where(g => g.IsMember).Select(g => g.Id).ToHashSet(StringComparer.Ordinal);
        var listenIds = _s.Groups.Where(g => !g.IsMember).Select(g => g.Id).ToHashSet(StringComparer.Ordinal);
        foreach (var m in _groups.All.Where(m => (m.CanManage || m.IsOwner) && !memberIds.Contains(m.Id) && !listenIds.Contains(m.Id)))
        {
            var c = new ScopedCard(_s, ScopedSection.Manage, null, m);
            if (!Match(c.Title, c.Id) || !PassFilter(c)) continue;
            Wire(c); Manage.Add(c);
        }
        Resort(Listen); Resort(Manage);
        Selected = All.FirstOrDefault(c => c.Id == sel);
        foreach (var x in All) x.IsSelected = x == Selected;
        RefreshCounts();
    }

    private bool PassFilter(ScopedCard c) => Filter switch
    {
        "active" => c.HasSession,
        "emergency" => c.IsEmergency || c.IsImminentPeril,
        "listen" => c.IsListenScope,
        "manage" => c.IsManageScope || c.IsOwner,
        _ => true,
    };

    /// <summary>섹션 안 정렬 — 긴급 › 진행 중 › 대기(§4 공통).</summary>
    private static void Resort(ObservableCollection<ScopedCard> list)
    {
        var ordered = list.OrderByDescending(c => c.IsEmergency || c.IsImminentPeril).ThenByDescending(c => c.HasSession).ThenBy(c => c.Title, StringComparer.CurrentCulture).ToList();
        for (int i = 0; i < ordered.Count; ++i) { int at = list.IndexOf(ordered[i]); if (at != i) list.Move(at, i); }
    }

    private void Wire(ScopedCard c)
    {
        c.WindowRequested += (_, s) => WindowRequested?.Invoke(this, s);
        c.EditRequested += (_, g) => EditRequested?.Invoke(this, g);
        c.DeleteRequested += (_, g) => DeleteRequested?.Invoke(this, g);
    }

    private void RefreshCounts()
    {
        foreach (var p in new[] { nameof(ListenCount), nameof(OthersCount), nameof(ManageCount), nameof(ListeningCount), nameof(ListenLimit), nameof(HeaderCounts), nameof(ListeningText), nameof(CanCreate), nameof(HasNoScope) })
            OnPropertyChanged(p);
    }

    public void Tick() { foreach (var c in All) c.Refresh(); OnPropertyChanged(nameof(ListeningCount)); OnPropertyChanged(nameof(ListeningText)); }
}
