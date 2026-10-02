// 감청 창 하나(§5) — VoLTE 감청(join 호) 또는 PTT 청취(listenOnly 그룹콜). MediaSource 미터·발언자·라우트·음량·종료.
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class MonitorWindowViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    public SessionItem Session { get; }
    [ObservableProperty] private double _volume = 1.0;
    [ObservableProperty] private string _closingNote = "";
    private readonly string _callerName = "", _calleeName = "";
    public MonitorWindowViewModel(DispatchSession s, SessionItem item)
    {
        _s = s; Session = item;
        _volume = item.Info.RxLevel;                                   // 코어가 호에 기억하는 수신 음량 — 인라인 행과 감청 창이 같은 값에서 시작한다
        item.PropertyChanged += (_, _) => Refresh();
        // 소스 귀속(caller/callee)의 이름 — 합류한 통화의 dialog 행에서 든다: 감시 회선과 그 상대, 방향(RFC 4235 direction)으로 누가 걸었는지 가른다.
        //   행은 통화가 끝나면 사라지므로 만들 때 한 번 적어 둔다. 못 찾으면(이미 끝남) 감청 leg 의 상대(대상 내선)만 안다.
        if (item.Kind == SessionKind.VolteMonitor && s.Dialogs.FirstOrDefault(d => d.Info.CallId == item.Info.JoinedDialog) is { } row)
        {
            string watched = s.Directory.Label(row.WatchedNumber), remote = s.Directory.Label(row.Info.RemoteIdentity);
            (_callerName, _calleeName) = row.IsIncomingLeg ? (remote, watched) : (watched, remote);
        }
    }

    public bool IsVolte => Session.Kind == SessionKind.VolteMonitor;
    public bool IsPtt => Session.Kind == SessionKind.PttListen;
    public string Title => IsVolte ? $"감청 — {Session.Title}" : $"청취 — {Session.Title}";
    public string Badge => IsVolte ? (_s.ListenHidden ? "은닉" : "투명") : "청취 전용";
    public TimeSpan Elapsed => Session.Elapsed;
    public string StateText => Session.StateText;
    public bool IsLive => Session.IsLive;
    public bool RouteIsSpeaker => Session.RouteIsSpeaker;
    public bool CanToggleRoute => _s.Audio.HasSpeaker;
    public string RouteLabel => AudioPolicy.RouteLabel(Session.Route);

    // VoLTE: caller / callee (RFC 5576 label)
    public CimsUe.MediaSource? Caller => Session.Info.Sources.FirstOrDefault(x => x.Label.Equals("caller", StringComparison.OrdinalIgnoreCase)) ?? Session.Info.Sources.ElementAtOrDefault(0);
    public CimsUe.MediaSource? Callee => Session.Info.Sources.FirstOrDefault(x => x.Label.Equals("callee", StringComparison.OrdinalIgnoreCase)) ?? Session.Info.Sources.ElementAtOrDefault(1);
    public string CallerName => _callerName.Length > 0 ? _callerName : Parts(0);
    public string CalleeName => _calleeName.Length > 0 ? _calleeName : Parts(1);
    public float CallerLevel => Caller?.Level ?? 0;
    public float CalleeLevel => Callee?.Level ?? 0;
    public bool CallerActive => Caller?.Active ?? false;
    public bool CalleeActive => Callee?.Active ?? false;
    private string Parts(int i)
    {
        // 제목 "A ↔ B" 는 dialog 행에서 만든 것이 아니라 세션 Title(상대 표시) — 감청 leg 의 RemoteUri 는 대상 내선
        var parts = Session.Title.Split('↔', StringSplitOptions.TrimEntries);
        return parts.Length > i ? parts[i] : i == 0 ? Session.Title : "";
    }

    // PTT
    public string Speaker => Session.Speaker;
    public bool HasSpeaker => Session.Speaker.Length > 0;
    public TimeSpan SpeakerElapsed => Session.SpeakerElapsed;
    public int Participants => _s.Groups.FirstOrDefault(g => g.Id == Session.Info.GroupId)?.ConnectedCount ?? 0;
    public bool IsEmergency => Session.IsEmergency;
    public string VideoNote => "영상 없음(음성 감청)";

    public void Refresh()
    {
        foreach (var p in new[] { nameof(Elapsed), nameof(StateText), nameof(IsLive), nameof(RouteIsSpeaker), nameof(RouteLabel), nameof(Caller), nameof(Callee), nameof(CallerLevel),
                                  nameof(CalleeLevel), nameof(CallerActive), nameof(CalleeActive), nameof(Speaker), nameof(HasSpeaker), nameof(SpeakerElapsed), nameof(Participants), nameof(IsEmergency) })
            OnPropertyChanged(p);
    }

    partial void OnVolumeChanged(double value) => _s.Engine.GetCall(Session.CallId).SetRxLevel((float)value);

    [RelayCommand] private void ToggleRoute() => _s.ToggleRoute(Session);
    [RelayCommand] private void Stop() { if (IsPtt) _s.LeaveChannel(Session); else _s.Hangup(Session); }
}
