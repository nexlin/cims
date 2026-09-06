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

    /// <summary>번호는 망 주소(E.164)라 로컬 표기로, 내선 라벨은 이름에 병기돼 온다("관제2석 1002").</summary>
    public string Label => IsMe ? $"{_s.Directory.DisplayNumber(Extension)} 나" : Name.Length > 0 ? $"{_s.Directory.DisplayNumber(Extension)} {Name}" : _s.Directory.DisplayNumber(Extension);
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

/// <summary>대기열 항목 — 대표번호에 걸려온 호 하나(발신자 기준). 서버가 포크 leg 마다 dialog 를 내므로(서버 요청서 §6-7) 같은 발신자의
/// leg 들을 한 항목으로 병합하고, Dialog 는 그중 대표 leg(confirmed 우선)다.</summary>
public sealed partial class QueueItem : ObservableObject
{
    private readonly DispatchSession _s;
    /// <summary>병합 키 — 발신자 번호(user part).</summary>
    public string CallerNumber { get; }
    [ObservableProperty] private DialogRow _dialog;
    /// <summary>confirmed 뒤 3초 제거가 예약됐는가(재수신 NOTIFY 로 되살아나지 않게).</summary>
    public bool DismissScheduled { get; set; }
    public QueueItem(DispatchSession s, DialogRow d) { _s = s; _dialog = d; CallerNumber = d.RemoteNumber; }
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
    /// <summary>이 호의 내 착신 leg 만 받는다 — 직접 착신과 동시에 울릴 때 다른 호를 받지 않도록.</summary>
    [RelayCommand] private void Answer()
    {
        var s = _s.Sessions.FirstOrDefault(x => x.IsIncoming && UserPartConverter.UserPart(x.Info.RemoteUri) == CallerNumber);
        if (s is not null) _s.Answer(s);
    }
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
    /// <summary>응답 뒤 대기열에서 내린 호의 발신자 — 그 호의 leg 가 전부 끝날 때까지 되살리지 않는다.</summary>
    private readonly HashSet<string> _dismissed = new(StringComparer.Ordinal);
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
        // 로그아웃은 Sessions/Dialogs 를 Clear 한다(개별 Ended 이벤트 없음) — VM 이 앱 수명 동안 살아 있으니 투영도 함께 비운다
        s.Dialogs.CollectionChanged += (_, e) =>
        {
            if (e.Action != System.Collections.Specialized.NotifyCollectionChangedAction.Reset) return;
            Queue.Clear(); _dismissed.Clear();
            foreach (var m in Members) m.Dialog = null;
            Refresh();
        };
        s.Sessions.CollectionChanged += (_, e) =>
        {
            if (e.Action != System.Collections.Specialized.NotifyCollectionChangedAction.Reset) return;
            Calls.Clear(); Refresh();
        };
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
            // 발신자 기준 병합: 살아 있는 leg 가 하나라도 있으면 항목 유지, 대표 leg 는 confirmed 우선. 종료된 leg 는 세션이 Dialogs 에서 이미 뺐다.
            string caller = d.RemoteNumber;
            var q = Queue.FirstOrDefault(x => x.CallerNumber == caller);
            var legs = _s.Dialogs.Where(x => _s.IsPilot(x.Watched) && x.RemoteNumber == caller && !x.IsTerminated).ToList();
            if (legs.Count == 0)
            {
                if (q is not null) Queue.Remove(q);
                _dismissed.Remove(caller);
            }
            else
            {
                var best = legs.OrderByDescending(x => x.IsConfirmed).ThenBy(x => x.FirstSeen).First();
                if (q is null)
                {
                    if (best.IsConfirmed && _dismissed.Contains(caller)) { Refresh(); return; }   // 응답 뒤 3초 표시가 끝난 호 — NOTIFY 재수신으로 되살리지 않는다
                    q = new QueueItem(_s, best); Queue.Add(q);
                }
                else if (q.Dialog != best) q.Dialog = best;
                if (best.IsConfirmed && !q.DismissScheduled)
                {
                    q.DismissScheduled = true;
                    var item = q;
                    _ = Task.Delay(3000).ContinueWith(_ => { if (Queue.Remove(item)) _dismissed.Add(item.CallerNumber); }, TaskScheduler.FromCurrentSynchronizationContext());
                }
            }
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
