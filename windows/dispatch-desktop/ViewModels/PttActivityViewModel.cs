// [무전] 이벤트(§4.4) — PTT 세계에서 방금 일어난 것의 흐름(발언·입퇴장·긴급·SDS·오류). 진행 중 상태는 채널 카드가 맡고 여기는 이벤트만.
// 머리 [따라가기](새 줄이 오면 맨 위로) · [이력에서 보기] · [CSV], 종류 칩(켜고 끄기, 수 병기) + [채널 · 전체 ▾], 진행 중 긴급·임박은 목록 위 고정 줄.
// 표 = 시각 | 채널 | 종류 | 내용(오른쪽 패널이 열려 칸이 좁으면 채널 열을 접는다). 행을 누르면 오른쪽 이벤트 상세.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

/// <summary>목록 위 고정 줄 — 진행 중인 긴급/임박 세션.</summary>
public sealed partial class PinnedEmergency : ObservableObject
{
    public SessionItem Session { get; }
    public PinnedEmergency(SessionItem s) { Session = s; }
    public string Title => Session.Title;
    public bool IsPeril => Session.IsImminentPeril && !Session.IsEmergency;
    public string KindText => IsPeril ? "임박" : "긴급";
    public TimeSpan Elapsed => Session.Elapsed;
    public string GroupId => Session.Info.GroupId;
    public void Refresh() { OnPropertyChanged(nameof(Elapsed)); OnPropertyChanged(nameof(Title)); OnPropertyChanged(nameof(IsPeril)); OnPropertyChanged(nameof(KindText)); }
}

/// <summary>이벤트 종류 칩 — 발언 · 입퇴장 · 긴급 · SDS · 오류.</summary>
public enum EventClass { Talk, Presence, Emergency, Sds, Error, Other }

/// <summary>표 한 줄 — 링 버퍼 행(ActivityRow)의 투영. 채널 = 행 제목 앞의 그룹명(DispatchSession 이 "<그룹명> …" 으로 쓴다).</summary>
public sealed partial class EventRow : ObservableObject
{
    public ActivityRow Row { get; }
    public GroupInfo? Group { get; }
    public EventClass Class { get; }
    [ObservableProperty] private bool _isSelected;
    public EventRow(ActivityRow row, GroupInfo? group)
    {
        Row = row; Group = group;
        Class = row.Kind switch
        {
            ActivityKind.Talk => EventClass.Talk,
            ActivityKind.Member or ActivityKind.SessionStart or ActivityKind.SessionEnd or ActivityKind.Private or ActivityKind.Adhoc
                or ActivityKind.ListenStart or ActivityKind.ListenEnd => EventClass.Presence,
            ActivityKind.Emergency => EventClass.Emergency,
            ActivityKind.Sds => EventClass.Sds,
            ActivityKind.Error => EventClass.Error,
            _ => row.IsEmergency ? EventClass.Emergency : EventClass.Other,
        };
    }
    public DateTime Time => Row.Time;
    public string Channel => Group?.Name ?? "";
    public string GroupId => Group?.Id ?? "";
    public string KindText => Class switch
    {
        EventClass.Talk => "발언", EventClass.Presence => "입퇴장", EventClass.Emergency => "긴급", EventClass.Sds => "SDS", EventClass.Error => "오류", _ => Row.KindText.Length > 0 ? Row.KindText : "기타",
    };
    /// <summary>내용 — 제목에서 채널명을 뗀 나머지 + 상세("박현장 발언 14초 · …").</summary>
    public string Content
    {
        get
        {
            string t = Group is not null && Row.Title.StartsWith(Group.Name, StringComparison.Ordinal) ? Row.Title[Group.Name.Length..].TrimStart() : Row.Title;
            return Row.Detail.Length > 0 ? $"{t} · {Row.Detail}" : t;
        }
    }
    public bool IsEmergency => Class == EventClass.Emergency;
    public bool IsError => Class == EventClass.Error;
    public bool CanReply => Class == EventClass.Sds && Group is not null;
}

public sealed partial class EventKindChip : ObservableObject
{
    public EventClass Class { get; }
    public string Name { get; }
    [ObservableProperty] private bool _on = true;
    [ObservableProperty] private int _count;
    public EventKindChip(EventClass c, string name) { Class = c; Name = name; }
}

public sealed partial class PttActivityViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    public ObservableCollection<PinnedEmergency> Pinned { get; } = new();
    public ObservableCollection<EventRow> Rows { get; } = new();
    public IReadOnlyList<EventKindChip> Kinds { get; } = new[]
    {
        new EventKindChip(EventClass.Talk, "발언"), new EventKindChip(EventClass.Presence, "입퇴장"), new EventKindChip(EventClass.Emergency, "긴급"),
        new EventKindChip(EventClass.Sds, "SDS"), new EventKindChip(EventClass.Error, "오류"),
    };
    /// <summary>[채널 · <이름> ▾] — 빈 값 = 전체.</summary>
    [ObservableProperty] private string _channel = "";
    [ObservableProperty] private bool _channelMenuOpen;
    /// <summary>[따라가기] — 새 줄이 오면 맨 위로 스크롤(뷰가 본다). 기본 켬.</summary>
    [ObservableProperty] private bool _follow;
    [ObservableProperty] private EventRow? _selected;
    public ObservableCollection<string> Channels { get; } = new();

    public event EventHandler<string>? ChannelRequested;
    /// <summary>머리 [이력에서 보기] — 끝난 세션의 날짜 창 조회는 [이력] 화면(§4.6).</summary>
    public event EventHandler? HistoryRequested;
    /// <summary>행을 누름 → 오른쪽 이벤트 상세(같은 행이면 닫기).</summary>
    public event EventHandler<EventRow>? RowRequested;
    /// <summary>새 줄이 맨 위에 들어왔다(따라가기 켬이면 뷰가 맨 위로).</summary>
    public event EventHandler? RowInserted;
    [RelayCommand] private void OpenHistory() => HistoryRequested?.Invoke(this, EventArgs.Empty);

    public PttActivityViewModel(DispatchSession s)
    {
        _s = s;
        _follow = s.Settings.Current.FollowEvents;
        foreach (var k in Kinds) k.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(EventKindChip.On)) Refilter(); };
        s.Activity.Ptt.CollectionChanged += (_, e) =>
        {
            bool inserted = e.Action == System.Collections.Specialized.NotifyCollectionChangedAction.Add && e.NewStartingIndex == 0;
            Refilter();
            if (inserted && Follow) RowInserted?.Invoke(this, EventArgs.Empty);
        };
        s.SessionAdded += (_, _) => RebuildPinned();
        s.SessionEnded += (_, _) => RebuildPinned();
        s.SessionChanged += (_, _) => RebuildPinned();
        Refilter();
    }

    public bool HasPinned => Pinned.Count > 0;
    public int TotalCount => _s.Activity.Ptt.Count;
    public string ChannelText => Channel.Length > 0 ? $"채널 · {Channel}" : "채널 · 전체";

    partial void OnChannelChanged(string value) { OnPropertyChanged(nameof(ChannelText)); Refilter(); }
    partial void OnFollowChanged(bool value) => _s.Settings.Update(x => x.FollowEvents = value);
    [RelayCommand] private void ToggleFollow() => Follow = !Follow;
    [RelayCommand] private void ToggleKind(EventKindChip k) => k.On = !k.On;
    [RelayCommand] private void PickChannel(string name) { Channel = name; ChannelMenuOpen = false; }
    [RelayCommand] private void OpenRow(EventRow r) => RowRequested?.Invoke(this, r);
    [RelayCommand] private void GoToChannel(EventRow r) { if (r.Group is not null) ChannelRequested?.Invoke(this, r.Group.Id); }
    [RelayCommand] private void ChannelPinned(PinnedEmergency p) => ChannelRequested?.Invoke(this, p.GroupId);

    /// <summary>행 제목은 "<그룹명> …" 으로 시작한다(DispatchSession 이 그렇게 쓴다) — 가장 긴 이름 일치.</summary>
    private GroupInfo? GroupOf(ActivityRow r) => _s.Groups.Where(g => r.Title.StartsWith(g.Name, StringComparison.Ordinal)).OrderByDescending(g => g.Name.Length).FirstOrDefault();

    public void Rebuild() { RebuildPinned(); Refilter(); }

    private void RebuildPinned()
    {
        var live = _s.Sessions.Where(x => x.Info.IsMcptt && (x.IsEmergency || x.IsImminentPeril) && x.Kind != SessionKind.PttPrivate).ToList();
        if (live.Count == Pinned.Count && live.All(x => Pinned.Any(p => p.Session == x))) { foreach (var p in Pinned) p.Refresh(); return; }
        Pinned.Clear();
        foreach (var x in live) Pinned.Add(new PinnedEmergency(x));
        OnPropertyChanged(nameof(HasPinned));
    }

    public void Tick() { foreach (var p in Pinned) p.Refresh(); }

    /// <summary>선택 표시 — 이벤트 상세가 보는 행(null = 없음).</summary>
    public void Mark(EventRow? r) { foreach (var x in Rows) x.IsSelected = x == r; Selected = r; }

    private void Refilter()
    {
        var sel = Selected?.Row;
        var all = _s.Activity.Ptt.Select(r => new EventRow(r, GroupOf(r))).ToList();
        foreach (var k in Kinds) k.Count = all.Count(r => r.Class == k.Class);
        var names = all.Select(r => r.Channel).Where(n => n.Length > 0).Distinct(StringComparer.Ordinal).OrderBy(n => n, StringComparer.CurrentCulture).ToList();
        if (!names.SequenceEqual(Channels)) { Channels.Clear(); foreach (var n in names) Channels.Add(n); }
        var hidden = Kinds.Where(k => !k.On).Select(k => k.Class).ToHashSet();
        Rows.Clear();
        foreach (var r in all)
        {
            if (hidden.Contains(r.Class)) continue;
            if (Channel.Length > 0 && r.Channel != Channel) continue;
            if (sel is not null && ReferenceEquals(r.Row, sel)) { r.IsSelected = true; Selected = r; }
            Rows.Add(r);
        }
        OnPropertyChanged(nameof(TotalCount));
    }

    /// <summary>이벤트 상세 «앞뒤 이벤트» — 같은 채널의 앞 둘·뒤 둘(시간순).</summary>
    public IReadOnlyList<EventRow> Around(EventRow r)
    {
        var same = _s.Activity.Ptt.Select(x => new EventRow(x, GroupOf(x))).Where(x => x.GroupId == r.GroupId).Reverse().ToList();
        int i = same.FindIndex(x => ReferenceEquals(x.Row, r.Row));
        if (i < 0) return Array.Empty<EventRow>();
        int from = Math.Max(0, i - 2);
        return same.Skip(from).Take(i + 3 - from).ToList();
    }

    [RelayCommand]
    private void Export()
    {
        var dlg = new Microsoft.Win32.SaveFileDialog { FileName = $"ptt-events-{DateTime.Now:yyyyMMdd-HHmm}.csv", Filter = "CSV|*.csv" };
        if (dlg.ShowDialog() == true) _s.Activity.ExportCsv(ActivityPanel.Ptt, dlg.FileName);
    }
}
