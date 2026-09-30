// [통화] «진행 중 · 관제 그룹»(§4.3) — 감시 대상 dialog 를 세션 행으로 결합(dialog 쌍 §4.3 결합 규칙). 행 = A ↔ B · 상태·경과 · 대표 라벨 · 조작 하나
// ([청취] / [지정 픽업] / 청취 중이면 [청취 종료]). 감청은 축이 아니라 상태다 — 청취 중인 행이 그 자리에서 펼쳐져 소스 귀속 두 줄(caller/callee 레벨)·
// 은닉/투명·출력을 보이고, [창으로] 는 선택(두 번째 모니터). 끝난 통화는 «기록» 이 맡는다.
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
    public string Title => $"{A} ↔ {B}";
    public string StateText => Primary.IsConfirmed ? "통화" : Primary.IsEarly ? "링잉" : Primary.State;
    public string StateLine => $"{StateText} {DispatchSession.Fmt(Elapsed)}";
    public bool IsRinging => Primary.IsEarly;
    public bool IsTalking => Primary.IsConfirmed;
    public TimeSpan Elapsed => Primary.Elapsed;
    public bool IsMine => Primary.WatchedNumber == _s.MyExtension || (Pair?.WatchedNumber == _s.MyExtension);
    public bool IsPilotPath => _s.Dialogs.Any(d => _s.IsPilot(d.Watched) && UserPartConverter.UserPart(d.Info.RemoteIdentity) == UserPartConverter.UserPart(Primary.Info.RemoteIdentity));
    public bool VisibilityHidden => _s.ListenHidden;
    public string VisibilityBadge => _s.ListenHidden ? "은닉" : "투명";
    public bool CanPickup => IsRinging && Primary.IsIncomingLeg && !IsMine;
    /// <summary>이 행을 청취 중인 감청 leg(INVITE-Join) — 행 확장의 원천.</summary>
    public SessionItem? MonitorSession => IsTalking ? _s.MonitorOfDialog(Primary.Info.CallId) ?? (Pair is null ? null : _s.MonitorOfDialog(Pair.Info.CallId)) : null;
    public bool IsMonitoring => MonitorSession is not null;
    public bool CanMonitor => IsTalking && !IsMine && _s.CanMonitorCalls && !IsMonitoring;
    public bool ShowMonitor => !IsMonitoring && !CanPickup && !IsMine;
    public string MonitorTip => IsMine ? "자기 통화" : !_s.CanMonitorCalls ? "청취 범위 밖" : IsRinging ? "연결 전" : "청취 — 이 행이 펼쳐져 두 소스를 따로 보인다";
    /// <summary>행 확장 — 감청 창과 같은 VM(caller/callee 레벨·출력·음량)을 행 안에서 쓴다.</summary>
    [ObservableProperty] private MonitorWindowViewModel? _monitor;

    public void Refresh()
    {
        var m = MonitorSession;
        if (m is null) Monitor = null;
        else if (Monitor?.Session != m) Monitor = new MonitorWindowViewModel(_s, m);
        else Monitor.Refresh();
        foreach (var p in new[] { nameof(A), nameof(B), nameof(Title), nameof(StateText), nameof(StateLine), nameof(IsRinging), nameof(IsTalking), nameof(Elapsed), nameof(IsMine),
                                  nameof(IsPilotPath), nameof(VisibilityHidden), nameof(VisibilityBadge), nameof(CanPickup), nameof(MonitorSession), nameof(IsMonitoring),
                                  nameof(CanMonitor), nameof(ShowMonitor), nameof(MonitorTip) })
            OnPropertyChanged(p);
    }

    [RelayCommand] private void Pickup() => _s.Pickup(Primary.WatchedNumber);
    [RelayCommand] private void StartMonitor() => _s.JoinMonitor(Primary);
    [RelayCommand] private void StopMonitor() { if (MonitorSession is { } m) _s.Hangup(m); }
    [RelayCommand] private void ShowWindow() { if (MonitorSession is { } m) WindowRequested?.Invoke(this, m); }
    public event EventHandler<SessionItem>? WindowRequested;
}

public sealed partial class CallActivityViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    public ObservableCollection<CallSessionRow> Ongoing { get; } = new();

    public event EventHandler<SessionItem>? WindowRequested;

    public CallActivityViewModel(DispatchSession s)
    {
        _s = s;
        s.DialogChanged += (_, _) => Rebuild();
        s.DialogEnded += (_, _) => Rebuild();
        s.SessionAdded += (_, _) => Tick();
        s.SessionEnded += (_, _) => Tick();
        s.Dialogs.CollectionChanged += (_, e) => { if (e.Action == System.Collections.Specialized.NotifyCollectionChangedAction.Reset) Rebuild(); };
    }

    public int OngoingCount => Ongoing.Count;

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
            row.Refresh();
            Ongoing.Add(row);
        }
        var ordered = Ongoing.OrderByDescending(r => r.IsMonitoring).ThenByDescending(r => r.IsRinging).ThenByDescending(r => r.Primary.StateSince).ToList();
        Ongoing.Clear();
        foreach (var r in ordered) Ongoing.Add(r);
        OnPropertyChanged(nameof(OngoingCount));
    }

    public void Tick() { foreach (var r in Ongoing) r.Refresh(); }
}
