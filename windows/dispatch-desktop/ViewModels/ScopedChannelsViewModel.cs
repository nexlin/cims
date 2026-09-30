// [무전] 타 채널(§4.2) — 내 멤버 그룹이 아닌 채널 행: 청취 범위 그룹(프로비저닝 pttTargets → conference 구독) · 타인 간 개별·애드혹 통화(서버 계약은
// dispatch_center.md §5.6a, 앱 구현은 §13 — 빈 목록). 행 = 점 · 이름 · 한 줄 상태 · [청취]/[청취 중] 하나, 행을 누르면 오른쪽 채널 상세.
// 필터 [전체|활성|긴급|청취 중] + 검색(🔍)은 이 칸에만. 관리만 되는 그룹(세션 상태 없음)은 여기 두지 않는다 — [더보기 › PTT 그룹] 이 맡는다.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public enum ScopedSection { Listen, Others }

public sealed partial class ScopedCard : ObservableObject
{
    private readonly DispatchSession _s;
    public ScopedSection Section { get; }
    /// <summary>청취 범위 그룹(세션 구독 대상).</summary>
    public GroupInfo? Group { get; }
    /// <summary>관리 범위 목록의 같은 그룹(소유·관리 가능 여부) — 채널 상세 [편집]·[삭제].</summary>
    public ManagedGroup? Managed { get; }
    /// <summary>고른 행 — 오른쪽 채널 상세가 이 행을 보고 있다.</summary>
    [ObservableProperty] private bool _isSelected;

    public ScopedCard(DispatchSession s, ScopedSection section, GroupInfo? g, ManagedGroup? m) { _s = s; Section = section; Group = g; Managed = m; }

    public string Id => Group?.Id ?? Managed?.Id ?? "";
    public string Title => Group?.Name ?? Managed?.Name ?? Id;
    public bool IsListenScope => Group is not null;
    public bool IsOwner => Group?.IsOwner == true || Managed?.IsOwner == true;
    public SessionItem? Listen => Group is null ? null : _s.ListenOfGroup(Group.Id);
    public bool IsListening => Listen is not null;
    public bool HasSession => Group?.HasSession == true || IsListening;
    public bool IsEmergency => Listen?.IsEmergency == true;
    public bool IsImminentPeril => Listen?.IsImminentPeril == true;
    /// <summary>청취 중인 세션이 일제 통화(floor B-bit — TS 24.380 §8.2.3.15).</summary>
    public bool IsBroadcast => Listen?.IsBroadcast == true;
    public int Participants => Group?.ConnectedCount ?? 0;
    public int MemberCount => Group?.MemberCount ?? Managed?.MemberCount ?? 0;
    public string Speaker => Listen?.Speaker ?? "";
    public bool HasSpeaker => Speaker.Length > 0;
    public TimeSpan SpeakerElapsed => Listen?.SpeakerElapsed ?? TimeSpan.Zero;
    /// <summary>참여/청취하지 않는 그룹의 경과는 로스터로 세션을 관측한 시점부터.</summary>
    public TimeSpan? Elapsed => Listen?.Elapsed ?? (Group?.SessionSince is DateTime t ? DateTime.Now - t : null);
    public string ElapsedText => Elapsed is TimeSpan e ? DispatchSession.Fmt(e) : "대기";
    public bool CanListen => IsListenScope && HasSession && !IsListening && _s.CanListenPtt;
    public string ListenTip => !IsListenScope ? "청취 범위 밖" : !HasSession ? "진행 중인 그룹 통화가 없습니다" : IsListening ? "청취 중 — 누르면 청취 종료" : "청취 전용 합류(recvonly)";
    public bool CanEdit => Managed is { CanManage: true } || IsOwner;
    public bool RouteIsSpeaker => Listen?.RouteIsSpeaker == true;
    public string ListenText => IsListening ? "청취 중" : "청취";
    /// <summary>행 한 줄 — 발언 <이름> / 세션 진행 중 · 참가 n / 마지막 세션 hh:mm / 대기.</summary>
    public string SubLine => HasSession
        ? (HasSpeaker ? $"발언 {Speaker}" + (IsEmergency && Listen is not null ? $" · 긴급 {Listen.StartedAt:HH:mm}" : "") : $"세션 진행 중 · 참가 {Participants}")
        : LastSessionEnd is DateTime le ? $"마지막 세션 {le:HH:mm} · {DispatchSession.Fmt(LastSessionLength)}" : "대기";
    [ObservableProperty] private DateTime? _lastSessionEnd;
    [ObservableProperty] private TimeSpan _lastSessionLength;

    public void Refresh()
    {
        foreach (var p in new[] { nameof(Title), nameof(IsListening), nameof(HasSession), nameof(IsEmergency), nameof(IsImminentPeril), nameof(IsBroadcast), nameof(Participants),
                                  nameof(MemberCount), nameof(Speaker), nameof(HasSpeaker), nameof(SpeakerElapsed), nameof(Elapsed), nameof(ElapsedText), nameof(CanListen),
                                  nameof(ListenTip), nameof(CanEdit), nameof(RouteIsSpeaker), nameof(ListenText), nameof(SubLine) })
            OnPropertyChanged(p);
    }

    [RelayCommand] private void ToggleListen() { if (Group is null) return; if (Listen is { } l) _s.Hangup(l); else if (CanListen) _s.ListenGroup(Group); }
    [RelayCommand] private void ToggleRoute() { if (Listen is { } l) _s.ToggleRoute(l); }
    [RelayCommand] private void ShowWindow() { if (Listen is { } l) WindowRequested?.Invoke(this, l); }
    public GroupInfo ToGroupInfo() => Group ?? new GroupInfo(Managed!.Id, Managed.Uri, Managed.Name, Managed.MemberCount) { IsOwner = Managed.IsOwner, Etag = Managed.ETag };
    public event EventHandler<SessionItem>? WindowRequested;
}

public sealed partial class ScopedChannelsViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly GroupAdminViewModel _groups;
    private readonly Dictionary<string, (DateTime? End, TimeSpan Len, DateTime? Since)> _memo = new(StringComparer.Ordinal);

    public ObservableCollection<ScopedCard> Listen { get; } = new();
    public ObservableCollection<ScopedCard> Others { get; } = new();
    /// <summary>all | active | emergency | listening</summary>
    [ObservableProperty] private string _filter = "all";
    [ObservableProperty] private string _search = "";
    /// <summary>머리 🔍 — 검색 칸을 연다(닫으면 검색어를 비운다).</summary>
    [ObservableProperty] private bool _searchOpen;
    [ObservableProperty] private ScopedCard? _selected;

    public event EventHandler<SessionItem>? WindowRequested;

    public ScopedChannelsViewModel(DispatchSession s, GroupAdminViewModel groups)
    {
        _s = s; _groups = groups;
        s.Groups.CollectionChanged += (_, _) => Rebuild();
        s.RosterChanged += (_, g) => OnRoster(g);
        s.SessionAdded += (_, x) => { if (x.Kind == SessionKind.PttListen) Rebuild(); };
        s.SessionEnded += (_, x) => { if (x.Kind == SessionKind.PttListen) Rebuild(); };
        s.SessionChanged += (_, _) => Tick();
        s.ProfileApplied += (_, _) => Rebuild();
        groups.Loaded += (_, _) => Rebuild();
        Rebuild();
    }

    public int ListenCount => _s.Groups.Count(g => !g.IsMember);
    public int ListeningCount => _s.Sessions.Count(x => x.Kind == SessionKind.PttListen);
    public int ListenLimit => _s.Settings.Current.MaxMonitorWindows;
    public string ListeningText => $"동시 청취 {ListeningCount}/{ListenLimit}";
    public bool HasNoScope => !_s.CanListenPtt;
    public bool IsEmpty => Listen.Count == 0 && Others.Count == 0;
    public string EmptyText => HasNoScope ? "청취 범위가 없습니다 — 콘솔 관리 › 역할에서 PTT 청취 범위를 주면 여기 채널이 생깁니다"
                             : Filter != "all" || Search.Length > 0 ? "조건에 맞는 채널이 없습니다" : "청취 범위 그룹 없음";

    partial void OnFilterChanged(string value) => Rebuild();
    partial void OnSearchChanged(string value) => Rebuild();
    partial void OnSearchOpenChanged(bool value) { if (!value) Search = ""; }
    [RelayCommand] private void SetFilter(string f) => Filter = f;
    [RelayCommand] private void ToggleSearch() => SearchOpen = !SearchOpen;

    /// <summary>행 고르기 표시 — 채널 상세가 이 행을 볼 때(null = 없음). 필터·검색이 가리고 있으면 풀어서 찾는다(«사라졌다» 로 보이지 않게).</summary>
    public ScopedCard? Mark(string? id)
    {
        if (id is not null && All.FirstOrDefault(c => c.Id == id) is null && (Filter != "all" || Search.Length > 0)) { Filter = "all"; Search = ""; }
        var card = id is null ? null : All.FirstOrDefault(c => c.Id == id);
        foreach (var x in All) x.IsSelected = x == card;
        Selected = card;
        return card;
    }
    public IEnumerable<ScopedCard> All => Listen.Concat(Others);

    private void OnRoster(GroupInfo g)
    {
        if (g.IsMember) return;
        var m = _memo.GetValueOrDefault(g.Id);
        if (g.HasSession && m.Since is null) _memo[g.Id] = (m.End, m.Len, DateTime.Now);
        else if (!g.HasSession && m.Since is DateTime since && _s.ListenOfGroup(g.Id) is null) _memo[g.Id] = (DateTime.Now, DateTime.Now - since, null);
        var card = All.FirstOrDefault(c => c.Id == g.Id);
        if (card is null) { Rebuild(); return; }
        ApplyMemo(card); card.Refresh();
        if (!PassFilter(card)) { Rebuild(); return; }
        Resort(Listen);
    }

    private void ApplyMemo(ScopedCard c) { if (_memo.TryGetValue(c.Id, out var m)) { c.LastSessionEnd = m.End; c.LastSessionLength = m.Len; } }

    public void Rebuild()
    {
        string? sel = Selected?.Id;
        string q = Search.Trim();
        bool Match(string title, string id) => q.Length == 0 || title.Contains(q, StringComparison.OrdinalIgnoreCase) || id.Contains(q, StringComparison.OrdinalIgnoreCase);
        Listen.Clear(); Others.Clear();
        var managed = _groups.All.ToDictionary(g => g.Id, g => g, StringComparer.Ordinal);
        // 청취 범위 — 프로비저닝 pttTargets(멤버가 아닌 그룹)
        foreach (var g in _s.Groups.Where(g => !g.IsMember))
        {
            var c = new ScopedCard(_s, ScopedSection.Listen, g, managed.GetValueOrDefault(g.Id));
            ApplyMemo(c);
            if (!Match(c.Title, c.Id) || !PassFilter(c)) continue;
            c.WindowRequested += (_, x) => WindowRequested?.Invoke(this, x);
            Listen.Add(c);
        }
        Resort(Listen);
        Selected = All.FirstOrDefault(c => c.Id == sel);
        foreach (var x in All) x.IsSelected = x == Selected;
        RefreshCounts();
    }

    private bool PassFilter(ScopedCard c) => Filter switch
    {
        "active" => c.HasSession,
        "emergency" => c.IsEmergency || c.IsImminentPeril,
        "listening" => c.IsListening,
        _ => true,
    };

    /// <summary>정렬 — 긴급 › 청취 중 › 진행 중 › 대기(§4.2).</summary>
    private static void Resort(ObservableCollection<ScopedCard> list)
    {
        var ordered = list.OrderByDescending(c => c.IsEmergency || c.IsImminentPeril).ThenByDescending(c => c.IsListening).ThenByDescending(c => c.HasSession)
                          .ThenBy(c => c.Title, StringComparer.CurrentCulture).ToList();
        for (int i = 0; i < ordered.Count; ++i) { int at = list.IndexOf(ordered[i]); if (at != i) list.Move(at, i); }
    }

    private void RefreshCounts()
    {
        foreach (var p in new[] { nameof(ListenCount), nameof(ListeningCount), nameof(ListenLimit), nameof(ListeningText), nameof(HasNoScope), nameof(IsEmpty), nameof(EmptyText) })
            OnPropertyChanged(p);
    }

    public void Tick() { foreach (var c in All) c.Refresh(); OnPropertyChanged(nameof(ListeningCount)); OnPropertyChanged(nameof(ListeningText)); }
}
