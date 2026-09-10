// 관제 요약 띠(§3.5) — 관제 밖 화면([이력]·[PTT 그룹]·[관리]) 상단에 상시. 발언 대상(① 체크 집합) + 발언 상태 + PTT 버튼, 대표번호 대기열·문자 미읽음,
// 내 통화, 감청 창 수, [관제로]. 관제 밖에서 눌린 PTT 핫키의 결과(발언권)가 여기 보인다. 값은 전부 다른 VM 의 투영이라 상태를 갖지 않는다.
using CommunityToolkit.Mvvm.ComponentModel;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class DispatchSummaryViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly PttChannelsViewModel _ptt;
    private readonly TalkBarViewModel _talk;
    private readonly CallDeskViewModel _desk;
    private readonly DeskViewModel _top;
    private readonly SmsMessagesViewModel _sms;

    public DispatchSummaryViewModel(DispatchSession s, PttChannelsViewModel ptt, TalkBarViewModel talk, CallDeskViewModel desk, DeskViewModel top, SmsMessagesViewModel sms)
    {
        _s = s; _ptt = ptt; _talk = talk; _desk = desk; _top = top; _sms = sms;
        ptt.SelectionChanged += (_, _) => Refresh();
        ptt.TargetsChanged += (_, _) => Refresh();
        desk.Queue.CollectionChanged += (_, _) => Refresh();
        top.Monitors.CollectionChanged += (_, _) => Refresh();
        sms.UnreadChanged += (_, _) => Refresh();
    }

    /// <summary>포커스 카드 — 발언자 표시용(발언 대상이 없을 때).</summary>
    private ChannelCard? Focus => _ptt.Selected;

    public bool HasTargets => _talk.HasTargets;
    public string TargetNames => _talk.TargetNames;
    public int TargetCount => _talk.TargetCount;
    public bool IsEmergency => _talk.IsEmergency || Focus?.IsEmergency == true;
    public bool IsSpeaking => _talk.IsSpeaking;
    public bool IsRequesting => _talk.IsRequesting;
    /// <summary>발언 중이 아닐 때 포커스 채널의 현재 발언자.</summary>
    public bool HasSpeaker => !IsSpeaking && Focus?.HasSpeaker == true;
    public string Speaker => Focus?.Speaker ?? "";
    public double TalkGauge => IsSpeaking ? _talk.MinGauge : Focus?.TalkGauge ?? 0;
    public TimeSpan SpeakerElapsed => IsSpeaking ? _talk.SpeakerElapsed : Focus?.SpeakerElapsed ?? TimeSpan.Zero;
    /// <summary>발언 상태 문구 — 내 발언 중 n/m / 요청 중·대기열 / 발언 <이름> / 대기.</summary>
    public string FloorText => IsSpeaking ? (_talk.IsPartial ? $"내 발언 중 {_talk.GrantedCount}/{_talk.TargetCount}" : "내 발언 중")
                             : IsRequesting ? (_talk.Targets.FirstOrDefault(t => t.IsQueued)?.StateText ?? "요청 중")
                             : HasSpeaker ? "발언" : "대기";
    public string PttText => _talk.PttText;
    public bool CanPtt => _talk.CanPtt;

    public int QueueCount => _desk.Queue.Count;
    public bool QueueBusy => QueueCount > 0;
    public int SmsUnread => _sms.UnreadTotal;
    public bool HasSmsUnread => SmsUnread > 0;
    public string CallsText
    {
        get
        {
            int active = _s.VolteCalls.Count(c => c.IsActive || c.IsOutgoing), held = _s.VolteCalls.Count(c => c.IsHeld), inc = _s.VolteCalls.Count(c => c.IsIncoming);
            var parts = new List<string>();
            if (inc > 0) parts.Add($"착신 {inc}");
            if (active > 0) parts.Add($"통화 {active}");
            if (held > 0) parts.Add($"보류 {held}");
            return parts.Count == 0 ? "없음" : string.Join(" · ", parts);
        }
    }
    public int MonitorCount => _top.MonitorCount;
    public bool HasMonitors => _top.HasMonitors;

    public void PttDown() => _talk.PttDown();
    public void PttUp() => _talk.PttUp();

    /// <summary>1초 틱·선택 변경 시 — 전부 파생값이라 통째로 알린다.</summary>
    public void Refresh()
    {
        foreach (var p in new[] { nameof(HasTargets), nameof(TargetNames), nameof(TargetCount), nameof(IsEmergency), nameof(IsSpeaking), nameof(IsRequesting), nameof(HasSpeaker),
                                  nameof(Speaker), nameof(TalkGauge), nameof(SpeakerElapsed), nameof(FloorText), nameof(PttText), nameof(CanPtt),
                                  nameof(QueueCount), nameof(QueueBusy), nameof(SmsUnread), nameof(HasSmsUnread), nameof(CallsText), nameof(MonitorCount), nameof(HasMonitors) })
            OnPropertyChanged(p);
    }
}
