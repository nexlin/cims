// ③ 일반통화 운영(왼쪽) — 관제 그룹원 상태 띠(BLF)·대표번호 대기열·내 통화 카드 (§4.3). 전부 DialogInfo·CallInfo 의 투영.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

/// <summary>그룹원 칩 — 내선 하나의 dialog 상태 요약.</summary>
public sealed partial class MemberChip : ObservableObject
{
    private readonly DispatchSession _s;
    public string Extension { get; }
    public string Name { get; }
    public bool IsMe { get; }
    [ObservableProperty] private DialogRow? _dialog;
    public MemberChip(DispatchSession s, string ext, string name, bool me) { _s = s; Extension = ext; Name = name; IsMe = me; }

    public string Label => IsMe ? $"{Extension} 나" : Name.Length > 0 ? $"{Extension} {Name}" : Extension;
    public string State => Dialog is null ? "idle" : Dialog.IsEarly ? "ringing" : Dialog.IsConfirmed ? "talking" : "idle";
    public string StateText => Dialog is null ? "대기" : Dialog.IsEarly ? "링잉" : Dialog.IsConfirmed ? "통화" : "대기";
    public string Peer => Dialog is null ? "" : _s.Directory.Label(Dialog.Info.RemoteIdentity);
    public TimeSpan Elapsed => Dialog?.Elapsed ?? TimeSpan.Zero;
    public bool IsRinging => Dialog?.IsEarly == true && Dialog.IsIncomingLeg;
    public bool IsTalking => Dialog?.IsConfirmed == true;
    public bool IsIdle => Dialog is null;
    public bool CanPickup => IsRinging && !IsMe;
    public bool CanMonitor => IsTalking && !IsMe && _s.CanMonitorCalls && _s.MonitorOfDialog(Dialog!.Info.CallId) is null;
    public bool IsMonitoring => IsTalking && _s.MonitorOfDialog(Dialog!.Info.CallId) is not null;

    public void Refresh()
    {
        foreach (var p in new[] { nameof(State), nameof(StateText), nameof(Peer), nameof(Elapsed), nameof(IsRinging), nameof(IsTalking), nameof(IsIdle), nameof(CanPickup), nameof(CanMonitor), nameof(IsMonitoring) })
            OnPropertyChanged(p);
    }
    partial void OnDialogChanged(DialogRow? value) => Refresh();

    [RelayCommand] private void Pickup() => _s.Pickup(Extension);
    [RelayCommand] private void Monitor() { if (Dialog is not null) _s.JoinMonitor(Dialog); }
    [RelayCommand] private void Fill() => FillRequested?.Invoke(this, Extension);
    public event EventHandler<string>? FillRequested;
}

/// <summary>대기열 항목 — 대표번호 AoR 의 dialog 하나(포크 집합).</summary>
public sealed partial class QueueItem : ObservableObject
{
    private readonly DispatchSession _s;
    public DialogRow Dialog { get; }
    public QueueItem(DispatchSession s, DialogRow d) { _s = s; Dialog = d; }
    public string Caller => _s.Directory.Label(Dialog.Info.RemoteIdentity);
    public string Pilot => UserPartConverter.UserPart(Dialog.Watched);
    public TimeSpan Elapsed => Dialog.Elapsed;
    public bool IsRinging => Dialog.IsEarly;
    public bool IsAnswered => Dialog.IsConfirmed;
    /// <summary>울리는 그룹원(각 내선 dialog 의 early 로 추정, RLS 전).</summary>
    public string Ringing => string.Join(", ", _s.Dialogs.Where(d => d != Dialog && d.IsEarly && d.IsIncomingLeg && !_s.IsPilot(d.Watched)
                                                             && UserPartConverter.UserPart(d.Info.RemoteIdentity) == UserPartConverter.UserPart(Dialog.Info.RemoteIdentity))
                                                   .Select(d => d.WatchedNumber));
    public bool RingsMe => _s.Sessions.Any(x => x.IsIncoming && UserPartConverter.UserPart(x.Info.RemoteUri) == UserPartConverter.UserPart(Dialog.Info.RemoteIdentity));
    public string AnsweredBy => IsAnswered ? "응답: " + (_s.Dialogs.FirstOrDefault(d => d != Dialog && d.IsConfirmed && UserPartConverter.UserPart(d.Info.RemoteIdentity) == UserPartConverter.UserPart(Dialog.Info.RemoteIdentity)) is { } m ? _s.Directory.Label(m.WatchedNumber) : "") : "";
    public void Refresh() { foreach (var p in new[] { nameof(Elapsed), nameof(IsRinging), nameof(IsAnswered), nameof(Ringing), nameof(RingsMe), nameof(AnsweredBy), nameof(Caller) }) OnPropertyChanged(p); }
    [RelayCommand] private void Pickup() => _s.Pickup(Pilot);
    [RelayCommand] private void Answer() { var s = _s.Sessions.FirstOrDefault(x => x.IsIncoming); if (s is not null) _s.Answer(s); }
}

/// <summary>내 통화 카드.</summary>
public sealed partial class CallCard : ObservableObject
{
    private readonly DispatchSession _s;
    public SessionItem Session { get; }
    [ObservableProperty] private string _transferTarget = "";
    [ObservableProperty] private bool _transferOpen;
    public CallCard(DispatchSession s, SessionItem item) { _s = s; Session = item; item.PropertyChanged += (_, _) => Refresh(); }

    public string Title => Session.Title;
    public bool IsPilotIncoming => Session.Info.Dir == CimsUe.CallDir.Incoming && _s.IsPilot(Session.Info.CalledParty);
    public string PathBadge => IsPilotIncoming ? $"대표 {Session.CalledParty} 착신" : Session.ConsultFor is not null ? "상담" : "";
    public bool IsConsult => Session.ConsultFor is not null;
    public bool HasTransferNote => Session.TransferNote.Length > 0;
    public bool CanToggleRoute => _s.Audio.HasSpeaker;
    public bool CanAnswer => Session.IsIncoming;
    public bool CanHold => Session.IsActive;
    public bool CanResume => Session.IsHeld;
    public bool CanTransfer => (Session.IsActive || Session.IsHeld) && !IsConsult;
    public bool CanComplete => IsConsult && Session.IsActive;
    public void Refresh() { foreach (var p in new[] { nameof(Title), nameof(PathBadge), nameof(IsConsult), nameof(HasTransferNote), nameof(CanAnswer), nameof(CanHold), nameof(CanResume), nameof(CanTransfer), nameof(CanComplete) }) OnPropertyChanged(p); }

    [RelayCommand] private void Answer() => _s.Answer(Session);
    [RelayCommand] private void Reject() => _s.Reject(Session);
    [RelayCommand] private void Hangup() => _s.Hangup(Session);
    [RelayCommand] private void Hold() => _s.Hold(Session);
    [RelayCommand] private void Resume() => _s.Resume(Session);
    [RelayCommand] private void Mute() => _s.ToggleMute(Session);
    [RelayCommand] private void Route() => _s.ToggleRoute(Session);
    [RelayCommand] private void Dtmf() => DtmfRequested?.Invoke(this, Session);
    [RelayCommand] private void OpenTransfer() => TransferOpen = !TransferOpen;
    [RelayCommand] private void TransferBlind() { if (TransferTarget.Trim().Length > 0 && _s.TransferBlind(Session, TransferTarget.Trim()).Ok) { TransferOpen = false; TransferTarget = ""; } }
    [RelayCommand] private void Consult() { if (TransferTarget.Trim().Length > 0 && _s.StartConsult(Session, TransferTarget.Trim()).Ok) { TransferOpen = false; TransferTarget = ""; } }
    [RelayCommand] private void Complete() => _s.CompleteConsult(Session);
    [RelayCommand] private void CancelConsult() => _s.CancelConsult(Session);
    [RelayCommand] private void PickMember(string ext) => TransferTarget = ext;
    public event EventHandler<SessionItem>? DtmfRequested;
}

public sealed partial class CallDeskViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    public ObservableCollection<MemberChip> Members { get; } = new();
    public ObservableCollection<QueueItem> Queue { get; } = new();
    public ObservableCollection<CallCard> Calls { get; } = new();
    public IReadOnlyList<string> MemberExtensions => Members.Where(m => !m.IsMe).Select(m => m.Extension).ToList();

    public event EventHandler<string>? FillRequested;
    public event EventHandler<SessionItem>? DtmfRequested;

    public CallDeskViewModel(DispatchSession s)
    {
        _s = s;
        s.ProfileApplied += (_, _) => RebuildMembers();
        s.Directory.Changed += (_, _) => RebuildMembers();
        s.DialogChanged += (_, d) => OnDialog(d);
        s.DialogEnded += (_, d) => OnDialog(d);
        s.SessionAdded += (_, item) => { if (item.IsVolteCall) { var c = new CallCard(s, item); c.DtmfRequested += (_, x) => DtmfRequested?.Invoke(this, x); Calls.Add(c); } Refresh(); };
        s.SessionEnded += (_, item) => { var c = Calls.FirstOrDefault(x => x.Session == item); if (c is not null) Calls.Remove(c); Refresh(); };
        s.SessionChanged += (_, _) => Refresh();
    }

    public bool HasDesk => _s.HasDesk;
    public int TodayAnswered => _s.Activity.Call.Count(r => r.Kind == ActivityKind.Incoming && r.Time.Date == DateTime.Today);
    public int TodayMissed => _s.Activity.Call.Count(r => r.IsMissed && r.Time.Date == DateTime.Today);
    public string EmptyQueueText => $"대기 호 없음 · 오늘 응대 {TodayAnswered} · 부재 {TodayMissed}";
    public bool QueueEmpty => Queue.Count == 0;

    private void RebuildMembers()
    {
        Members.Clear();
        var me = _s.MyExtension;
        var list = _s.Directory.Members.ToList();
        if (me.Length > 0 && !list.Any(c => c.Number == me)) list.Insert(0, new Contact(ContactKind.Extension, me, _s.DisplayName, new[] { "member" }));
        foreach (var c in list)
        {
            var chip = new MemberChip(_s, c.Number, c.Name, c.Number == me);
            chip.FillRequested += (_, e) => FillRequested?.Invoke(this, e);
            chip.Dialog = _s.Dialogs.FirstOrDefault(d => d.WatchedNumber == c.Number);
            Members.Add(chip);
        }
        OnPropertyChanged(nameof(HasDesk)); OnPropertyChanged(nameof(MemberExtensions));
    }

    private void OnDialog(DialogRow d)
    {
        if (_s.IsPilot(d.Watched))
        {
            var q = Queue.FirstOrDefault(x => x.Dialog == d);
            if (d.IsTerminated) { if (q is not null) Queue.Remove(q); }
            else if (q is null) Queue.Add(new QueueItem(_s, d));
            else if (d.IsConfirmed) _ = Task.Delay(3000).ContinueWith(_ => { if (Queue.Contains(q)) Queue.Remove(q); }, TaskScheduler.FromCurrentSynchronizationContext());
        }
        else
        {
            var chip = Members.FirstOrDefault(m => m.Extension == d.WatchedNumber);
            if (chip is not null)
            {
                // 내선 하나에 dialog 여러 개면 confirmed 우선, 없으면 early
                chip.Dialog = _s.Dialogs.Where(x => x.WatchedNumber == d.WatchedNumber).OrderByDescending(x => x.IsConfirmed).ThenByDescending(x => x.IsEarly).FirstOrDefault();
            }
        }
        Refresh();
    }

    public void Refresh()
    {
        foreach (var m in Members) m.Refresh();
        foreach (var q in Queue) q.Refresh();
        foreach (var c in Calls) c.Refresh();
        OnPropertyChanged(nameof(EmptyQueueText)); OnPropertyChanged(nameof(QueueEmpty));
    }
}
