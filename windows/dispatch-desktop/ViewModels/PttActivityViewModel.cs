// ② PTT 내역 — 진행 중 행(멤버 그룹 로스터·참여 세션·청취 범위 그룹) + 최근 이벤트(링 버퍼) (§4.2).
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class PttOngoingRow : ObservableObject
{
    private readonly DispatchSession _s;
    public GroupInfo? Group { get; }
    [ObservableProperty] private SessionItem? _session;
    public PttOngoingRow(DispatchSession s, GroupInfo? g, SessionItem? session) { _s = s; Group = g; _session = session; }

    public string Key => Group?.Id ?? Session?.Info.GroupId ?? "";
    public string Title => Group?.Name ?? Session?.Title ?? "";
    public bool IsMember => Group?.IsMember == true;
    public bool IsAdhoc => Session?.Kind == SessionKind.PttAdhoc;
    public bool IsScope => Group is not null && !Group.IsMember;
    public string Badge => IsAdhoc ? "임시" : IsMember ? "멤버" : "청취 범위";
    public int Participants => Group?.ConnectedCount ?? Session?.AdhocMembers.Count ?? 0;
    public string ParticipantsText => Participants > 0 ? $"{Participants}명" : "미상";
    public string Speaker => Session?.Speaker ?? "";
    public TimeSpan Elapsed => Session?.Elapsed ?? (Group?.RosterAt is DateTime t ? DateTime.Now - t : TimeSpan.Zero);
    public bool IsEmergency => Session?.IsEmergency == true;
    public bool HasSession => Session is not null || Group?.HasSession == true;
    public bool IsJoined => Session is not null && !Session.Info.ListenOnly;
    public bool IsListening => Session is not null && Session.Info.ListenOnly;
    public bool CanListen => IsScope && HasSession && !IsListening && _s.CanListenPtt;
    public string StateText => IsListening ? "청취 중" : IsJoined ? "참여" : HasSession ? "진행" : "대기";

    public void Refresh()
    {
        foreach (var p in new[] { nameof(Participants), nameof(ParticipantsText), nameof(Speaker), nameof(Elapsed), nameof(IsEmergency), nameof(HasSession),
                                  nameof(IsJoined), nameof(IsListening), nameof(CanListen), nameof(StateText), nameof(Title) })
            OnPropertyChanged(p);
    }

    [RelayCommand] private void Channel() => ChannelRequested?.Invoke(this, Key);
    [RelayCommand] private void Listen() { if (Group is not null) _s.ListenGroup(Group); }
    [RelayCommand] private void ShowWindow() { if (Session is not null) WindowRequested?.Invoke(this, Session); }
    public event EventHandler<string>? ChannelRequested;
    public event EventHandler<SessionItem>? WindowRequested;
}

public sealed partial class PttActivityViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    public ObservableCollection<PttOngoingRow> Ongoing { get; } = new();
    public ObservableCollection<ActivityRow> Recent { get; } = new();
    /// <summary>all | mine | emergency</summary>
    [ObservableProperty] private string _filter = "all";
    [ObservableProperty] private string _search = "";

    public event EventHandler<string>? ChannelRequested;
    public event EventHandler<SessionItem>? WindowRequested;

    public PttActivityViewModel(DispatchSession s)
    {
        _s = s;
        s.Activity.Ptt.CollectionChanged += (_, _) => Refilter();
        s.Groups.CollectionChanged += (_, _) => Rebuild();
        s.RosterChanged += (_, _) => Rebuild();
        s.SessionAdded += (_, _) => Rebuild();
        s.SessionEnded += (_, _) => Rebuild();
        s.SessionChanged += (_, _) => Tick();
        s.ProfileApplied += (_, _) => Rebuild();
        Refilter();
    }

    partial void OnFilterChanged(string value) => Refilter();
    partial void OnSearchChanged(string value) => Refilter();
    [RelayCommand] private void SetFilter(string f) => Filter = f;

    public void Rebuild()
    {
        Ongoing.Clear();
        foreach (var g in _s.Groups)
        {
            var sess = _s.SessionOfGroup(g.Id) ?? _s.ListenOfGroup(g.Id);
            if (sess is null && !g.HasSession && g.IsMember) continue;         // 멤버 그룹은 세션 있을 때만
            if (sess is null && !g.HasSession && !_s.CanListenPtt) continue;
            var row = new PttOngoingRow(_s, g, sess);
            Wire(row);
            Ongoing.Add(row);
        }
        foreach (var a in _s.Sessions.Where(x => x.Kind == SessionKind.PttAdhoc))
        {
            var row = new PttOngoingRow(_s, null, a);
            Wire(row);
            Ongoing.Add(row);
        }
    }

    private void Wire(PttOngoingRow row)
    {
        row.ChannelRequested += (_, k) => ChannelRequested?.Invoke(this, k);
        row.WindowRequested += (_, s) => WindowRequested?.Invoke(this, s);
    }

    public void Tick() { foreach (var r in Ongoing) r.Refresh(); }

    private void Refilter()
    {
        Recent.Clear();
        string q = Search.Trim();
        var mine = _s.Groups.Where(g => g.IsMember).Select(g => g.Name).ToHashSet();
        foreach (var r in _s.Activity.Ptt)
        {
            if (Filter == "emergency" && !r.IsEmergency) continue;
            if (Filter == "mine" && !mine.Any(n => r.Title.StartsWith(n, StringComparison.Ordinal))) continue;
            if (q.Length > 0 && !r.Title.Contains(q, StringComparison.OrdinalIgnoreCase) && !r.Detail.Contains(q, StringComparison.OrdinalIgnoreCase)) continue;
            Recent.Add(r);
        }
    }

    [RelayCommand]
    private void Export()
    {
        var dlg = new Microsoft.Win32.SaveFileDialog { FileName = $"ptt-activity-{DateTime.Now:yyyyMMdd-HHmm}.csv", Filter = "CSV|*.csv" };
        if (dlg.ShowDialog() == true) _s.Activity.ExportCsv(ActivityPanel.Ptt, dlg.FileName);
    }
}
