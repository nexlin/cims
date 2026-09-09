// ⑤ PTT 이벤트(§4.4) — PTT 세계에서 방금 일어난 것의 흐름(발언·긴급·세션·멤버·SDS·사설콜/애드혹·청취). 진행 중 상태는 ①② 카드가 맡고 여기는 이벤트만.
// 머리 [● <포커스 채널> 따라가기] 토글(기본 켬 — 포커스 카드의 채널만) · [전체|발언|긴급] · 검색 · [이력에서 보기]. 활성 긴급 행은 목록 위에 고정.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

/// <summary>목록 위 고정 행 — 진행 중인 긴급/임박 세션.</summary>
public sealed partial class PinnedEmergency : ObservableObject
{
    public SessionItem Session { get; }
    public PinnedEmergency(SessionItem s) { Session = s; }
    public string Title => Session.Title;
    public bool IsPeril => Session.IsImminentPeril && !Session.IsEmergency;
    public string KindText => IsPeril ? "임박" : "긴급";
    public TimeSpan Elapsed => Session.Elapsed;
    public string GroupId => Session.Info.GroupId;
    public void Refresh() { OnPropertyChanged(nameof(Elapsed)); OnPropertyChanged(nameof(Title)); }
}

public sealed partial class PttActivityViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    public ObservableCollection<PinnedEmergency> Pinned { get; } = new();
    public ObservableCollection<ActivityRow> Recent { get; } = new();
    /// <summary>all | talk | emergency</summary>
    [ObservableProperty] private string _filter = "all";
    [ObservableProperty] private string _search = "";
    /// <summary>포커스 채널 따라가기 — 켜면 포커스 카드의 채널 행만, 끄면 범위 전체.</summary>
    [ObservableProperty] private bool _followFocus;
    /// <summary>포커스 채널 이름(①·② 포커스 카드) — 없으면 빈 문자열(따라가기가 걸러 낼 것 없음).</summary>
    [ObservableProperty] private string _focusTitle = "";

    public event EventHandler<string>? ChannelRequested;
    /// <summary>머리 [이력에서 보기] — 끝난 세션의 날짜 창 조회는 [이력] 화면(§4.6).</summary>
    public event EventHandler? HistoryRequested;
    /// <summary>SDS 행 [답장] → ④ 스레드.</summary>
    public event EventHandler<string>? ReplyRequested;
    [RelayCommand] private void OpenHistory() => HistoryRequested?.Invoke(this, EventArgs.Empty);

    public PttActivityViewModel(DispatchSession s)
    {
        _s = s;
        _followFocus = s.Settings.Current.FollowChannelEvents;
        s.Activity.Ptt.CollectionChanged += (_, _) => Refilter();
        s.SessionAdded += (_, _) => RebuildPinned();
        s.SessionEnded += (_, _) => RebuildPinned();
        s.SessionChanged += (_, _) => RebuildPinned();
        Refilter();
    }

    public bool HasFocus => FocusTitle.Length > 0;
    public string FollowText => HasFocus ? $"{FocusTitle} 따라가기" : "따라가기";
    public bool HasPinned => Pinned.Count > 0;

    partial void OnFilterChanged(string value) => Refilter();
    partial void OnSearchChanged(string value) => Refilter();
    partial void OnFocusTitleChanged(string value) { OnPropertyChanged(nameof(HasFocus)); OnPropertyChanged(nameof(FollowText)); Refilter(); }
    partial void OnFollowFocusChanged(bool value) { _s.Settings.Update(x => x.FollowChannelEvents = value); Refilter(); }
    [RelayCommand] private void SetFilter(string f) => Filter = f;
    [RelayCommand] private void ToggleFollow() => FollowFocus = !FollowFocus;
    [RelayCommand] private void Channel(ActivityRow r) { var g = GroupOf(r); if (g is not null) ChannelRequested?.Invoke(this, g.Id); }
    [RelayCommand] private void ChannelPinned(PinnedEmergency p) => ChannelRequested?.Invoke(this, p.GroupId);
    [RelayCommand] private void Reply(ActivityRow r) { var g = GroupOf(r); if (g is not null) ReplyRequested?.Invoke(this, g.Id); }

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

    private void Refilter()
    {
        Recent.Clear();
        string q = Search.Trim();
        foreach (var r in _s.Activity.Ptt)
        {
            if (Filter == "emergency" && !r.IsEmergency) continue;
            if (Filter == "talk" && r.Kind != ActivityKind.Talk) continue;
            if (FollowFocus && HasFocus && !r.Title.StartsWith(FocusTitle, StringComparison.Ordinal)) continue;
            if (q.Length > 0 && !r.Title.Contains(q, StringComparison.OrdinalIgnoreCase) && !r.Detail.Contains(q, StringComparison.OrdinalIgnoreCase)) continue;
            Recent.Add(r);
        }
    }

    [RelayCommand]
    private void Export()
    {
        var dlg = new Microsoft.Win32.SaveFileDialog { FileName = $"ptt-events-{DateTime.Now:yyyyMMdd-HHmm}.csv", Filter = "CSV|*.csv" };
        if (dlg.ShowDialog() == true) _s.Activity.ExportCsv(ActivityPanel.Ptt, dlg.FileName);
    }
}
