// dialog 이벤트(RFC 4235) 행 — 그룹원 띠(BLF)·대기열·④ 진행 중 행의 소스(§4.3·§4.4).
using CimsUe;
using CommunityToolkit.Mvvm.ComponentModel;
using DispatchDesktop.Converters;

namespace DispatchDesktop.Models;

public sealed partial class DialogRow : ObservableObject
{
    /// <summary>감시 대상 AoR(entity) — 내선 또는 대표번호.</summary>
    public string Watched { get; }
    public string WatchedNumber => UserPartConverter.UserPart(Watched);
    public string Id { get; }
    public string Key => Watched + "|" + Id;

    [ObservableProperty] private DialogInfo _info;
    [ObservableProperty] private DateTime _firstSeen = DateTime.Now;
    [ObservableProperty] private DateTime _stateSince = DateTime.Now;
    [ObservableProperty] private TimeSpan _elapsed;
    /// <summary>한 번이라도 confirmed 였는가 — 대표번호 부재(전원 무응답) 판정.</summary>
    public bool WasConfirmed { get; set; }

    public DialogRow(DialogInfo info)
    {
        Watched = info.Watched; Id = info.Id; _info = info;
    }

    public string State => Info.State;
    public bool IsEarly => Info.State is "early" or "proceeding" or "trying";
    public bool IsConfirmed => Info.State == "confirmed";
    public bool IsTerminated => Info.State == "terminated";
    public bool IsIncomingLeg => Info.Direction == "recipient";
    public string RemoteNumber => UserPartConverter.UserPart(Info.RemoteIdentity);

    public void Apply(DialogInfo d)
    {
        bool changed = d.State != Info.State;
        Info = d;
        if (changed) StateSince = DateTime.Now;
        OnPropertyChanged(nameof(State)); OnPropertyChanged(nameof(IsEarly)); OnPropertyChanged(nameof(IsConfirmed));
        OnPropertyChanged(nameof(IsTerminated)); OnPropertyChanged(nameof(IsIncomingLeg)); OnPropertyChanged(nameof(RemoteNumber));
    }

    public void Tick(DateTime now) => Elapsed = now - StateSince;
}
