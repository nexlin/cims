// 관제 요약 띠(§3.5) — 관제 밖 화면([이력]·[PTT 그룹]·[관리]) 상단에 상시. 선택 채널 + 발언 상태 + PTT 버튼, 대표번호 대기열,
// 내 통화, 감청 창 수, [관제로]. 관제 밖에서 눌린 PTT 핫키의 결과(발언권)가 여기 보인다. 값은 전부 다른 VM 의 투영이라 상태를 갖지 않는다.
using CommunityToolkit.Mvvm.ComponentModel;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class DispatchSummaryViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly PttChannelsViewModel _ptt;
    private readonly CallDeskViewModel _desk;
    private readonly DeskViewModel _top;

    public DispatchSummaryViewModel(DispatchSession s, PttChannelsViewModel ptt, CallDeskViewModel desk, DeskViewModel top)
    {
        _s = s; _ptt = ptt; _desk = desk; _top = top;
        ptt.SelectionChanged += (_, _) => Refresh();
        desk.Queue.CollectionChanged += (_, _) => Refresh();
        top.Monitors.CollectionChanged += (_, _) => Refresh();
    }

    private ChannelCard? Card => _ptt.Selected;

    public bool HasChannel => Card is not null;
    public string ChannelName => Card?.Title ?? "채널 없음";
    public string ChannelHotKey => Card is { Index: > 0 } c ? $"Ctrl+{c.Index}" : "";
    public bool IsEmergency => Card?.IsEmergency == true;
    public bool IsSpeaking => Card?.IsSpeaking == true;
    public bool IsRequesting => Card?.IsRequesting == true || Card?.IsQueued == true;
    public bool HasSpeaker => !IsSpeaking && Card?.HasSpeaker == true;
    public string Speaker => Card?.Speaker ?? "";
    public double TalkGauge => Card?.TalkGauge ?? 0;
    public TimeSpan SpeakerElapsed => Card?.SpeakerElapsed ?? TimeSpan.Zero;
    /// <summary>발언 상태 문구 — 내 발언 중 / 요청 중·대기열 / 발언자 / 대기.</summary>
    public string FloorText => IsSpeaking ? "내 발언 중" : IsRequesting ? (Card!.FloorNote.Length > 0 ? Card.FloorNote : "요청 중") : HasSpeaker ? "발언" : Card is null ? "" : Card.SessionText;
    public string PttText => Card?.PttText ?? "PTT";
    public bool CanPtt => Card is not null && (Card.ShowPtt || Card.Session is null && Card.Group is not null);

    public int QueueCount => _desk.Queue.Count;
    public bool QueueBusy => QueueCount > 0;
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

    public void PttDown() => _ptt.PttDown();
    public void PttUp() => _ptt.PttUp();

    /// <summary>1초 틱·선택 변경 시 — 전부 파생값이라 통째로 알린다.</summary>
    public void Refresh()
    {
        foreach (var p in new[] { nameof(HasChannel), nameof(ChannelName), nameof(ChannelHotKey), nameof(IsEmergency), nameof(IsSpeaking), nameof(IsRequesting), nameof(HasSpeaker),
                                  nameof(Speaker), nameof(TalkGauge), nameof(SpeakerElapsed), nameof(FloorText), nameof(PttText), nameof(CanPtt),
                                  nameof(QueueCount), nameof(QueueBusy), nameof(CallsText), nameof(MonitorCount), nameof(HasMonitors) })
            OnPropertyChanged(p);
    }
}
