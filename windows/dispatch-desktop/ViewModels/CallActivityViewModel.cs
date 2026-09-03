// ④ 일반통화 내역 — 진행 중 세션 행(dialog 쌍 결합 §4.4) + 최근 기록. 정렬: 링잉 → 진행 시작 역순 → 최근 시각 역순.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class CallSessionRow : ObservableObject
{
    private readonly DispatchSession _s;
    public DialogRow Primary { get; }
    public DialogRow? Pair { get; set; }
    public CallSessionRow(DispatchSession s, DialogRow d) { _s = s; Primary = d; }

    public string A => _s.Directory.Label(Primary.IsIncomingLeg ? Primary.Info.RemoteIdentity : Primary.Watched);
    public string B => _s.Directory.Label(Primary.IsIncomingLeg ? Primary.Watched : Primary.Info.RemoteIdentity);
    public string StateText => Primary.IsConfirmed ? "통화" : Primary.IsEarly ? "링잉" : Primary.State;
    public bool IsRinging => Primary.IsEarly;
    public bool IsTalking => Primary.IsConfirmed;
    public TimeSpan Elapsed => Primary.Elapsed;
    public bool IsMine => Primary.WatchedNumber == _s.MyExtension || (Pair?.WatchedNumber == _s.MyExtension);
    public bool IsPilotPath => _s.Dialogs.Any(d => _s.IsPilot(d.Watched) && UserPartConverter.UserPart(d.Info.RemoteIdentity) == UserPartConverter.UserPart(Primary.Info.RemoteIdentity));
    public string PathBadge => IsPilotPath ? "대표 " + UserPartConverter.UserPart(_s.PilotId) : "";
    public string VisibilityBadge => _s.ListenHidden ? "은닉" : "투명";
    public bool CanPickup => IsRinging && Primary.IsIncomingLeg && !IsMine;
    public bool IsMonitoring => IsTalking && (_s.MonitorOfDialog(Primary.Info.CallId) is not null || (Pair is not null && _s.MonitorOfDialog(Pair.Info.CallId) is not null));
    public bool CanMonitor => IsTalking && !IsMine && _s.CanMonitorCalls && !IsMonitoring;
    public string MonitorTip => IsMine ? "자기 통화" : !_s.CanMonitorCalls ? "청취 범위 밖" : IsRinging ? "연결 전" : "청취";
    public void Refresh() { foreach (var p in new[] { nameof(A), nameof(B), nameof(StateText), nameof(IsRinging), nameof(IsTalking), nameof(Elapsed), nameof(IsMine), nameof(PathBadge), nameof(CanPickup), nameof(IsMonitoring), nameof(CanMonitor), nameof(MonitorTip) }) OnPropertyChanged(p); }

    [RelayCommand] private void Pickup() => _s.Pickup(Primary.WatchedNumber);
    [RelayCommand] private void Monitor() => _s.JoinMonitor(Primary);
    [RelayCommand] private void ShowWindow() { var m = _s.MonitorOfDialog(Primary.Info.CallId) ?? (Pair is null ? null : _s.MonitorOfDialog(Pair.Info.CallId)); if (m is not null) WindowRequested?.Invoke(this, m); }
    public event EventHandler<SessionItem>? WindowRequested;
}

public sealed partial class CallActivityViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    public ObservableCollection<CallSessionRow> Ongoing { get; } = new();
    public ObservableCollection<ActivityRow> Recent { get; } = new();
    /// <summary>all | pilot | missed</summary>
    [ObservableProperty] private string _filter = "all";
    [ObservableProperty] private string _search = "";

    public event EventHandler<SessionItem>? WindowRequested;
    public event EventHandler<string>? SmsRequested;

    public CallActivityViewModel(DispatchSession s)
    {
        _s = s;
        s.Activity.Call.CollectionChanged += (_, _) => Refilter();
        s.DialogChanged += (_, _) => Rebuild();
        s.DialogEnded += (_, _) => Rebuild();
        s.SessionAdded += (_, _) => Tick();
        s.SessionEnded += (_, _) => Tick();
        Refilter();
    }

    partial void OnFilterChanged(string v) => Refilter();
    partial void OnSearchChanged(string v) => Refilter();
    [RelayCommand] private void SetFilter(string f) => Filter = f;

    /// <summary>결합 규칙: 감시 대상 두 내선의 leg 가 서로를 가리키고 전이 시각이 근접하면 한 행(dispatch_center.md §5.3).</summary>
    public void Rebuild()
    {
        Ongoing.Clear();
        var used = new HashSet<DialogRow>();
        var rows = _s.Dialogs.Where(d => !_s.IsPilot(d.Watched) && !d.IsTerminated).ToList();
        foreach (var d in rows)
        {
            if (used.Contains(d)) continue;
            used.Add(d);
            var pair = rows.FirstOrDefault(o => !used.Contains(o) && o.Info.State == d.Info.State
                                                && UserPartConverter.UserPart(o.Info.RemoteIdentity) == d.WatchedNumber
                                                && UserPartConverter.UserPart(d.Info.RemoteIdentity) == o.WatchedNumber
                                                && Math.Abs((o.StateSince - d.StateSince).TotalSeconds) < 5);
            if (pair is not null) used.Add(pair);
            var row = new CallSessionRow(_s, d.IsIncomingLeg || pair is null ? d : pair) { Pair = pair is null ? null : (d.IsIncomingLeg ? pair : d) };
            row.WindowRequested += (_, m) => WindowRequested?.Invoke(this, m);
            Ongoing.Add(row);
        }
        var ordered = Ongoing.OrderByDescending(r => r.IsRinging).ThenByDescending(r => r.Primary.StateSince).ToList();
        Ongoing.Clear();
        foreach (var r in ordered) Ongoing.Add(r);
    }

    public void Tick() { foreach (var r in Ongoing) r.Refresh(); }

    private void Refilter()
    {
        Recent.Clear();
        string q = Search.Trim();
        foreach (var r in _s.Activity.Call)
        {
            if (Filter == "pilot" && !r.IsPilot) continue;
            if (Filter == "missed" && !r.IsMissed) continue;
            if (q.Length > 0 && !r.Title.Contains(q, StringComparison.OrdinalIgnoreCase) && !r.Detail.Contains(q, StringComparison.OrdinalIgnoreCase)) continue;
            Recent.Add(r);
        }
    }

    [RelayCommand] private void Redial(ActivityRow r) { if (r.Number.Length > 0) _s.Dial(r.Number); }
    [RelayCommand] private void Sms(ActivityRow r) { if (r.Number.Length > 0 && !_s.Directory.IsExternal(r.Number)) SmsRequested?.Invoke(this, r.Number); }
    [RelayCommand]
    private void Export()
    {
        var dlg = new Microsoft.Win32.SaveFileDialog { FileName = $"call-activity-{DateTime.Now:yyyyMMdd-HHmm}.csv", Filter = "CSV|*.csv" };
        if (dlg.ShowDialog() == true) _s.Activity.ExportCsv(ActivityPanel.Call, dlg.FileName);
    }
}
