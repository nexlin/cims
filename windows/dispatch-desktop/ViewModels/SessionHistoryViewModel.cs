// [이력] 화면(§4.6) — 서버 통합 이력을 **창 조회**(`/provisioning/history?since=&until=`, 하루 단위)로 받아 콘솔 `/service/history/volte`·
// `/service/history/ptt` 와 같은 구성으로 보인다: 도구줄 아래 **시간대 밴드**(그날의 분포이자 필터) · 통화는 콘솔과 같은 열의 표(유형·발신→착신·
// 상태·시작/응답/종료·통화시간·종료사유·녹취) · PTT 는 좌 세션 요약 카드(종류·상태·대상·시각·턴/화자/발화/동시·개시) + 우 선택 세션 패널
// (지표 → 참여자 → 발언 타임라인(화자 레인, 막대 클릭 = 그 턴 재생) → 이벤트 타임라인(floor 중재 + 입퇴장) → 녹취 재생).
// 발언 지표·참여자·floor 는 서버가 콘솔 PTT 이력의 읽기 모델(OAM 세션 인덱스·세션 상세)을 범위 게이트 뒤에서 프록시한 값이고,
// 발언 턴은 녹취 세그먼트의 슬롯 트랙에서 만든다(콘솔 segTurns 와 같은 해석). 범위·감사는 서버(monitor_scope/ptt_listen, E-AUD-016).
// 녹취 재생은 MP4(AAC) 를 받아(`/provisioning/recordings/{id}/segments/{seq}/audio?slot=`) 창의 MediaElement 로 튼다. ② ④ 실시간 내역과 달리
// 이 화면은 지난 기록 열람 전용이다(폴링 없음).
using System.Collections.ObjectModel;
using System.IO;
using System.Windows;
using CimsUe;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

/// <summary>시간대 밴드 한 칸 — 건수·상대 농도(0 = 없음, 0.18~1 = 많을수록 진하게)·선택.</summary>
public sealed partial class HourCell : ObservableObject
{
    public string Hour { get; }
    public int Count { get; }
    public double Ratio { get; }
    public string CountText => Count > 0 ? Count.ToString() : "";
    public string Tip => $"{Hour}시 · {Count}건";
    [ObservableProperty] private bool _isSelected;
    public HourCell(string hour, int count, int max) { Hour = hour; Count = count; Ratio = count > 0 && max > 0 ? 0.18 + 0.82 * count / max : 0; }
}

/// <summary>목록 한 행 — 통화(표 열)·PTT(요약 카드) 두 종류의 표시값을 서버 항목에서 미리 만든다. 이름은 주소록, 그룹은 GMS 목록 이름.</summary>
public sealed class HistoryRow
{
    public HistoryEntry E { get; }
    public string TimeText => E.Time.ToString("HH:mm:ss");
    public string KindText { get; }
    public string Parties { get; }
    public string DurationText { get; }
    public string EventText { get; }
    public bool HasRecording => E.HasRecording;
    public bool IsEmergency => E.Emergency;
    public bool IsLive { get; }
    public string StateText { get; }

    // 통화 표 열 (콘솔 VoLTE 이력과 같은 열)
    public bool IsVideo => E.CallType == "volte_video";
    public string TypeText => IsVideo ? "영상" : "음성";
    public string CallerLabel { get; }
    public string CalleeLabel { get; }
    public string InviteText => Clock(E.InviteTime ?? (E.Kind == HistoryKind.Call ? E.Time : null));
    public string AnswerText => Clock(E.AnswerTime);
    public string EndText => Clock(E.EndTime);
    public string EndReasonText { get; }

    // PTT 요약 카드 (콘솔 SessionCard 와 같은 항목)
    public string SessionKindText { get; }
    public bool IsDuplex => E.FloorControl == "off";
    public string TargetText { get; }
    public string RangeText { get; }
    public string GroupIdText { get; }
    /// <summary>카드 둘째 줄 — 시각 범위(·길이)(· 그룹 id).</summary>
    public string SubText => GroupIdText.Length > 0 ? $"{RangeText} · {GroupIdText}" : RangeText;
    public int TurnCount => E.TurnCount;
    public int SpeakerCount => E.SpeakerCount > 0 ? E.SpeakerCount : E.People.Count;
    public string SpeechText => SessionHistoryViewModel.FmtSpeech(E.TotalSpeechMs);
    public int MaxConcurrent => E.MaxConcurrent;
    public bool ShowConcurrent => E.MaxConcurrent > 1;
    public string InitiatorLabel { get; }
    public bool HasInitiator => E.From.Length > 0;
    public string FloorPolicyText { get; }
    public bool HasFloorPolicy => E.FloorControl == "on" && E.FloorPolicy.Length > 0;

    private static string Clock(DateTime? t) => t is { } d ? d.ToString("HH:mm:ss") : "—";

    public HistoryRow(HistoryEntry e, DispatchSession s)
    {
        E = e;
        KindText = e.Kind == HistoryKind.Ptt ? "PTT" : "통화";
        string who(string u) => SessionHistoryViewModel.Who(s, u);
        IsLive = e.State is "active" or "ringing" || e.Event == "ptt.session.start";
        if (e.Kind == HistoryKind.Ptt)
        {
            string g = s.Groups.FirstOrDefault(x => string.Equals(x.Uri, e.Group, StringComparison.OrdinalIgnoreCase))?.Name
                       ?? (e.GroupName.Length > 0 ? e.GroupName : UserPartConverter.UserPart(e.Group));
            var peers = e.People.Where(p => !SessionHistoryViewModel.SameUser(p, e.From)).ToList();
            SessionKindText = e.SessionKind switch { "private" => "1:1", "adhoc" => "임시", "" or "group" => "그룹", _ => "미상" };
            TargetText = e.SessionKind switch
            {
                "private" => peers.Count > 0 ? $"{who(e.From)} ↔ {string.Join(", ", peers.Select(who))}" : who(e.From),
                "adhoc" => $"{who(e.From)} 외 {peers.Count}명",
                _ => g,
            };
            Parties = e.From.Length > 0 ? $"{TargetText} · 개시 {who(e.From)}" : TargetText;
            var st = e.StartTime ?? e.Time;
            int dur = e.EndTime is { } et && e.StartTime is { } s0 ? Math.Max(0, (int)(et - s0).TotalSeconds) : e.DurationSec;
            RangeText = $"{st:HH:mm:ss} ~ {(IsLive ? "진행중" : Clock(e.EndTime))}" + (dur > 0 ? $" · {SessionHistoryViewModel.FmtDur(dur)}" : "");
            DurationText = dur > 0 ? SessionHistoryViewModel.FmtDur(dur) : "";
            GroupIdText = e.SessionKind is "" or "group" ? UserPartConverter.UserPart(e.Group) : "";
            InitiatorLabel = who(e.From);
            StateText = IsLive ? "진행중" : "종료";
            FloorPolicyText = e.FloorPolicy switch { "multi" => $"multi · 최대 {(e.MaxTalkers > 0 ? e.MaxTalkers.ToString() : "?")}명", "dual" => "dual · 2명", _ => "single" };
            CallerLabel = CalleeLabel = EndReasonText = "";
        }
        else
        {
            CallerLabel = who(e.From); CalleeLabel = e.To.Length > 0 ? who(e.To) : "—";
            Parties = $"{CallerLabel} → {CalleeLabel}";
            DurationText = e.DurationSec > 0 ? SessionHistoryViewModel.FmtDur(e.DurationSec) : "—";
            StateText = e.State switch { "active" => "통화중", "ringing" => "호출중", "ended" or "" => "종료", var x => x };
            EndReasonText = SessionHistoryViewModel.EndReasonText(e.EndReason);
            SessionKindText = TargetText = RangeText = GroupIdText = InitiatorLabel = FloorPolicyText = "";
        }
        EventText = e.Event switch
        {
            "call.answered" => "응답", "call.missed" => "부재", "ptt.session.start" => "진행 중", "ptt.session.end" => "종료",
            _ => e.Event,
        };
    }
}

/// <summary>선택 세션의 참여자 한 줄 — 입퇴장 기록 ∪ 화자(녹취 턴). 발언 통계는 턴에서 센다(참가만 한 사람은 0).</summary>
public sealed record ParticipantRow(string Id, string Label, bool IsInitiator, string RangeText, int Turns, string SpeechText, string Color, bool HasSpoken);

/// <summary>발언 턴 막대 — 한 화자가 한 슬롯을 점유한 구간. Multi = 세그먼트에 턴이 여럿(동시 발언·슬롯 재사용) → 단독 트랙(slot) 재생.</summary>
public sealed record TurnBar(int Seq, int Slot, bool Multi, string Speaker, string Color, double LeftRatio, double WidthRatio, string Tooltip, bool Playable);

public sealed record SpeakerLane(string Speaker, string Label, string Color, IReadOnlyList<TurnBar> Bars);

/// <summary>이벤트 타임라인 한 줄 — floor 중재(TS 24.380 op) 또는 입퇴장(events.jsonl). Detail 은 op 별 부가 정보(사유·대기 순번·회수 유예).</summary>
public sealed record TimelineItem(DateTime Ts, bool IsFloor, string Color, string Who, string Text, string Detail)
{
    public string TimeText => Ts.ToString("HH:mm:ss");
    public bool HasWho => Who.Length > 0;
    public bool HasDetail => Detail.Length > 0;
}

public sealed record MetricItem(string Key, string Value, string Suffix, string Hint);

/// <summary>발언 타임라인 눈금 — 트랙 폭 대비 위치(0~1)와 시각 표기.</summary>
public sealed record AxisTick(double Ratio, string Label);

public sealed partial class SessionHistoryViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private CancellationTokenSource? _playCts;
    private int _detailSeq;

    public SessionHistoryViewModel(DispatchSession s) { _s = s; }

    public IReadOnlyList<string> Kinds { get; } = new[] { "통화(VoLTE)", "PTT 세션" };

    [ObservableProperty] private int _kindIndex;
    [ObservableProperty] private DateTime _date = DateTime.Today;
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private bool _busy;
    [ObservableProperty] private string _error = "";
    [ObservableProperty] private string _summary = "";
    [ObservableProperty] private HistoryRow? _selected;
    [ObservableProperty] private RecordingInfo? _recording;
    [ObservableProperty] private RecordingSegment? _selectedSegment;
    [ObservableProperty] private string _recordingStatus = "";
    [ObservableProperty] private string _mediaSource = "";
    [ObservableProperty] private string _playingLabel = "";
    [ObservableProperty] private bool _loadingAudio;
    /// <summary>시간대 필터("" = 전체). 밴드 칸 클릭으로 토글, 날짜·종류가 바뀌면 해제.</summary>
    [ObservableProperty] private string _selectedHour = "";
    [ObservableProperty] private bool _detailLoading;
    [ObservableProperty] private string _detailStatus = "";
    [ObservableProperty] private bool _showFloorLayer = true;
    [ObservableProperty] private bool _showMemberLayer = true;
    [ObservableProperty] private string _axisStartText = "";
    [ObservableProperty] private string _axisEndText = "";
    /// <summary>발언 타임라인 확대 배율(1 = 세션 전체가 트랙 폭). 콘솔 LaneTimebar 와 같은 ×1.25 단계, 상한 64.</summary>
    [ObservableProperty] private double _talkZoom = 1.0;
    private DateTime _axisT0;
    private double _axisSpanMs = 1000;

    private IReadOnlyList<HistoryEntry> _all = Array.Empty<HistoryEntry>();
    private bool _suppressQuery;                                          // 표본 심기 중 종류 전환이 서버 조회를 부르지 않게
    private IReadOnlyDictionary<string, int> _hours = new Dictionary<string, int>();
    private readonly List<TimelineItem> _timelineAll = new();
    private readonly Queue<RecordingSegment> _playQueue = new();

    public ObservableCollection<HistoryRow> Rows { get; } = new();
    public ObservableCollection<HourCell> Hours { get; } = new();
    public ObservableCollection<RecordingSegment> Segments { get; } = new();
    public ObservableCollection<ParticipantRow> Participants { get; } = new();
    public ObservableCollection<SpeakerLane> Lanes { get; } = new();
    public ObservableCollection<TimelineItem> Timeline { get; } = new();
    public ObservableCollection<MetricItem> Metrics { get; } = new();
    public ObservableCollection<AxisTick> AxisTicks { get; } = new();
    public string TalkZoomText => TalkZoom > 1.001 ? $"×{TalkZoom:0.#}" : "";
    public bool IsTalkZoomed => TalkZoom > 1.001;
    public const double TalkZoomMax = 64;

    public bool HasError => Error.Length > 0;
    public bool HasRecording => Recording is not null && Segments.Count > 0;
    public bool CanPlay => SelectedSegment is not null && !LoadingAudio;
    public bool IsPlaying => MediaSource.Length > 0;
    public bool IsPtt => KindIndex == 1;
    public bool IsCall => KindIndex != 1;
    public bool HasSelection => Selected is not null;
    public bool HasLanes => Lanes.Count > 0;
    public bool HasTimeline => Timeline.Count > 0;
    public bool HasParticipants => Participants.Count > 0;
    public int FloorCount { get; private set; }
    public int MemberEventCount { get; private set; }
    public bool HasHourFilter => SelectedHour.Length > 0;
    /// <summary>좌 목록 : 우 패널 폭 — 통화는 한 줄이 곧 상세라 표가 전체 폭(우 패널 0), PTT 는 세션 패널이 주역이라 콘솔처럼 1 : 3
    /// (카드 목록은 요약이면 충분하다). 화면은 이 값을 기본 폭으로 놓고 사용자가 경계를 끌어 바꿀 수 있다(더블클릭 = 기본 폭).</summary>
    public GridLength ListWidth => new GridLength(1, GridUnitType.Star);
    public GridLength PaneWidth => IsPtt ? new GridLength(3, GridUnitType.Star) : new GridLength(0);
    /// <summary>고른 행에 녹취가 있는가 — 통화 표 아래 녹취 띠의 표시 조건(세그먼트 로딩 중에도 띠는 보인다).</summary>
    public bool SelectedHasRecording => Selected?.HasRecording == true;

    /// <summary>재생 시작(파일 경로)/정지 — MediaElement 는 창 코드비하인드가 든다.</summary>
    public event EventHandler<string>? PlayRequested;
    public event EventHandler? StopRequested;

    partial void OnErrorChanged(string value) => OnPropertyChanged(nameof(HasError));
    partial void OnSearchChanged(string value) => Filter();
    partial void OnKindIndexChanged(int value)
    {
        OnPropertyChanged(nameof(IsPtt)); OnPropertyChanged(nameof(IsCall)); OnPropertyChanged(nameof(PaneWidth));
        SelectedHour = ""; if (!_suppressQuery) _ = QueryAsync();
    }
    partial void OnDateChanged(DateTime value) { SelectedHour = ""; _ = QueryAsync(); }
    partial void OnSelectedChanged(HistoryRow? value) { OnPropertyChanged(nameof(HasSelection)); OnPropertyChanged(nameof(SelectedHasRecording)); _ = LoadSelectionAsync(value); }
    partial void OnSelectedSegmentChanged(RecordingSegment? value) => OnPropertyChanged(nameof(CanPlay));
    partial void OnLoadingAudioChanged(bool value) => OnPropertyChanged(nameof(CanPlay));
    partial void OnMediaSourceChanged(string value) => OnPropertyChanged(nameof(IsPlaying));
    partial void OnRecordingChanged(RecordingInfo? value) => OnPropertyChanged(nameof(HasRecording));
    partial void OnSelectedHourChanged(string value) { OnPropertyChanged(nameof(HasHourFilter)); foreach (var c in Hours) c.IsSelected = c.Hour == value; Filter(); }
    partial void OnTalkZoomChanged(double value) { OnPropertyChanged(nameof(TalkZoomText)); OnPropertyChanged(nameof(IsTalkZoomed)); RebuildAxisTicks(); }
    partial void OnShowFloorLayerChanged(bool value) => ApplyTimelineLayers();
    partial void OnShowMemberLayerChanged(bool value) => ApplyTimelineLayers();

    private HistoryKind Kind => KindIndex == 1 ? HistoryKind.Ptt : HistoryKind.Call;

    // ── 조회 ──

    /// <summary>날짜 창 이동 — -1 전날 · +1 다음 날 · 0 오늘(§4.6). 값이 바뀌면 OnDateChanged 가 조회한다.</summary>
    [RelayCommand] private void ShiftDate(string delta)
    {
        int d = int.TryParse(delta, out var n) ? n : 0;
        var next = d == 0 ? DateTime.Today : Date.Date.AddDays(d);
        if (next > DateTime.Today) next = DateTime.Today;
        if (next != Date.Date) Date = next;
    }

    [RelayCommand] public async Task QueryAsync()
    {
        var m = _s.Management; if (m is null) { Error = "로그인 전"; return; }
        Busy = true; Error = "";
        var from = Date.Date; var to = Date.Date.AddDays(1).AddSeconds(-1);
        var r = await m.QueryHistoryAsync(Kind, from, to);
        Busy = false;
        if (!r.Ok)
        {
            Error = ResponseText.Describe(ResponseText.Area.Management, r.Code, r.Reason);
            _all = Array.Empty<HistoryEntry>(); _hours = new Dictionary<string, int>(); RebuildHours(); Filter(); return;
        }
        _all = r.Value.Items.OrderByDescending(e => e.Time).ToList();      // 표시는 최근이 위
        _hours = r.Value.Hours.Count > 0 ? r.Value.Hours : CountHours(_all);
        RebuildHours();
        Filter();
    }

    private static Dictionary<string, int> CountHours(IEnumerable<HistoryEntry> items)
    {
        var d = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var e in items) { string h = e.AxisTime.ToString("HH"); d[h] = d.TryGetValue(h, out int n) ? n + 1 : 1; }
        return d;
    }

    private void RebuildHours()
    {
        Hours.Clear();
        int max = _hours.Count > 0 ? _hours.Values.Max() : 0;
        for (int h = 0; h < 24; h++)
        {
            string k = h.ToString("00");
            Hours.Add(new HourCell(k, _hours.TryGetValue(k, out int n) ? n : 0, max) { IsSelected = k == SelectedHour });
        }
    }

    /// <summary>밴드 칸 클릭 — 그 시간대만(다시 누르면 해제). 건수 0 인 칸은 무시.</summary>
    [RelayCommand] private void SelectHour(string hour)
    {
        if (!_hours.TryGetValue(hour, out int n) || n == 0) { if (SelectedHour == hour) SelectedHour = ""; return; }
        SelectedHour = SelectedHour == hour ? "" : hour;
    }

    private void Filter()
    {
        Rows.Clear();
        string q = Search.Trim();
        string qn = DirectoryService.Normalize(q);
        foreach (var e in _all)
        {
            if (SelectedHour.Length > 0 && e.AxisTime.ToString("HH") != SelectedHour) continue;
            var row = new HistoryRow(e, _s);
            if (q.Length > 0 && !row.Parties.Contains(q, StringComparison.OrdinalIgnoreCase)
                && !(qn.Length > 0 && (DirectoryService.Normalize(e.From).Contains(qn) || DirectoryService.Normalize(e.To).Contains(qn)
                                       || e.People.Any(p => DirectoryService.Normalize(p).Contains(qn))))) continue;
            Rows.Add(row);
        }
        int live = Rows.Count(x => x.IsLive);
        Summary = $"{Date:yyyy-MM-dd}{(SelectedHour.Length > 0 ? $" {SelectedHour}시" : "")} · {Rows.Count}건"
                  + (live > 0 ? $" · 진행중 {live}" : "")
                  + (Rows.Count(x => x.HasRecording) is > 0 and var n ? $" · 녹취 {n}건" : "")
                  + (IsPtt && Rows.Sum(x => x.E.TotalSpeechMs) is > 0 and var sp ? $" · 발화 합 {FmtSpeech(sp)}" : "");
        if (Selected is not null && !Rows.Contains(Selected)) Selected = null;
    }

    // ── 선택 세션 ──

    private async Task LoadSelectionAsync(HistoryRow? row)
    {
        int seq = ++_detailSeq;
        Stop(); _playQueue.Clear();
        Recording = null; Segments.Clear(); RecordingStatus = "";
        Participants.Clear(); Lanes.Clear(); Timeline.Clear(); Metrics.Clear(); _timelineAll.Clear();
        FloorCount = MemberEventCount = 0; AxisStartText = AxisEndText = ""; DetailStatus = "";
        RaiseDetailChanged();
        if (row is null) return;
        if (_previewSegments is not null)                                 // --ui-preview 표본: 서버 없이 패널만 그린다
        {
            if (row.HasRecording)
            {
                foreach (var s in _previewSegments) Segments.Add(s);
                Recording = new RecordingInfo(row.E.RecordingId, row.E.Kind == HistoryKind.Ptt ? "ptt" : "volte", row.E.From, row.E.To, row.E.Group, row.E.StartTime, row.E.EndTime, row.E.DurationSec, "ready", _previewSegments);
                RecordingStatus = $"세그먼트 {Segments.Count}개 · ready (표본)"; SelectedSegment = Segments.FirstOrDefault();
            }
            else RecordingStatus = row.IsLive ? "진행 중 — 끝나면 녹취가 잡힙니다" : "녹취 없음";
            if (row.E.Kind == HistoryKind.Ptt) BuildPttPane(row, _previewDetail is { } pd && pd.RecordingId == row.E.RecordingId ? pd : null);
            return;
        }
        var m = _s.Management; if (m is null) return;

        Task<Result<PttSessionDetail>>? detailTask = null;
        Task<Result<RecordingInfo>>? recTask = null;
        if (row.E.Kind == HistoryKind.Ptt && row.E.RecordingId.Length > 0)
        {
            DetailLoading = true; DetailStatus = "세션 상세 조회 중…";
            detailTask = m.GetPttSessionDetailAsync(row.E.RecordingId);
        }
        if (row.E.RecordingId.Length > 0 && row.HasRecording) { RecordingStatus = "녹취 정보 조회 중…"; recTask = m.GetRecordingAsync(row.E.RecordingId); }
        else RecordingStatus = row.IsLive ? "진행 중 — 끝나면 녹취가 잡힙니다" : "녹취 없음";

        PttSessionDetail? detail = null;
        if (detailTask is not null)
        {
            var d = await detailTask;
            if (seq != _detailSeq) return;
            if (d.Ok) { detail = d.Value; DetailStatus = ""; }
            else DetailStatus = ResponseText.Describe(ResponseText.Area.Management, d.Code, d.Reason);
        }
        if (recTask is not null)
        {
            var r = await recTask;
            if (seq != _detailSeq) return;
            if (!r.Ok) RecordingStatus = ResponseText.Describe(ResponseText.Area.Recording, r.Code, r.Reason);
            else
            {
                Recording = r.Value;
                foreach (var s in r.Value.Segments) Segments.Add(s);
                OnPropertyChanged(nameof(HasRecording));
                RecordingStatus = Segments.Count > 0 ? $"세그먼트 {Segments.Count}개 · {r.Value.Status}" : "세그먼트 없음(녹음 진행 중이거나 미디어 없음)";
                SelectedSegment = Segments.FirstOrDefault();
            }
        }
        DetailLoading = false;
        if (row.E.Kind == HistoryKind.Ptt) BuildPttPane(row, detail);
    }

    /// <summary>발언 턴(세그먼트 슬롯 트랙) · 참여자 · 이벤트 타임라인 · 지표를 만든다. 콘솔 PanelDetail 과 같은 구성.</summary>
    private void BuildPttPane(HistoryRow row, PttSessionDetail? detail)
    {
        var e = row.E;
        // 발언 턴 — 세그먼트 → 슬롯 트랙의 화자 구간(콘솔 segTurns). 트랙이 없으면 세그먼트 전체가 대표 화자의 한 턴.
        var turns = new List<(int Seq, int Slot, string Spk, DateTime Start, DateTime End, int DurMs, bool Playable, bool Multi)>();
        foreach (var seg in Segments)
        {
            if (seg.Start is not { } b) continue;
            bool playable = seg.Status != "recording";
            var audio = seg.Tracks.Where(t => t.Kind == "audio").ToList();
            var mine = new List<(int, int, string, DateTime, DateTime, int, bool, bool)>();
            if (audio.Count == 0)
                mine.Add((seg.Seq, 0, seg.SpeakerId, b, b.AddMilliseconds(seg.DurationMs), seg.DurationMs, playable, false));
            else
                foreach (var t in audio)
                {
                    var spans = t.Speakers.Count > 0 ? t.Speakers : new[] { new SpeakerSpan(seg.SpeakerId, 0, seg.DurationMs) };
                    foreach (var sp in spans)
                        mine.Add((seg.Seq, t.Slot, sp.Id.Length > 0 ? sp.Id : seg.SpeakerId, b.AddMilliseconds(sp.OffsetMs),
                                  b.AddMilliseconds(sp.OffsetMs + sp.DurMs), sp.DurMs, playable && t.Status != "recording", false));
                }
            bool multi = mine.Count > 1;
            foreach (var t in mine) turns.Add((t.Item1, t.Item2, t.Item3, t.Item4, t.Item5, t.Item6, t.Item7, multi));
        }
        turns.Sort((a, b) => a.Start != b.Start ? a.Start.CompareTo(b.Start) : a.Slot.CompareTo(b.Slot));
        var order = new List<string>();
        foreach (var t in turns) if (t.Spk.Length > 0 && !order.Contains(t.Spk)) order.Add(t.Spk);
        string colorOf(string id) { int i = order.IndexOf(id); return SpkColors[(i < 0 ? 0 : i) % SpkColors.Length]; }

        var parts = detail?.Participants ?? Array.Empty<PttParticipant>();
        var events = detail?.Events ?? Array.Empty<PttEvent>();
        var floor = detail?.Floor ?? Array.Empty<PttFloorEvent>();

        // 시간축 = 세션 시작~종료(항목) ∪ 턴·이벤트가 실제 걸친 범위
        var times = new List<DateTime>();
        if (e.StartTime is { } st) times.Add(st);
        if (e.EndTime is { } et) times.Add(et);
        times.AddRange(turns.Select(t => t.Start)); times.AddRange(turns.Select(t => t.End));
        times.AddRange(events.Where(x => x.Ts is not null).Select(x => x.Ts!.Value));
        times.AddRange(floor.Where(x => x.Ts is not null).Select(x => x.Ts!.Value));
        DateTime t0 = times.Count > 0 ? times.Min() : e.Time, t1 = times.Count > 0 ? times.Max() : e.Time;
        double span = Math.Max(1000, (t1 - t0).TotalMilliseconds);
        AxisStartText = t0.ToString("HH:mm:ss"); AxisEndText = t1.ToString("HH:mm:ss");
        _axisT0 = t0; _axisSpanMs = span;
        TalkZoom = 1.0;                                                    // 눈금은 레인을 다 만든 뒤(아래) 다시 센다

        // 화자 레인
        foreach (var spk in order)
        {
            var bars = turns.Where(t => t.Spk == spk).Select(t => new TurnBar(t.Seq, t.Slot, t.Multi, spk, colorOf(spk),
                (t.Start - t0).TotalMilliseconds / span, Math.Max(0.002, t.DurMs / span),
                $"#{t.Seq}{(t.Multi ? $" 슬롯 {t.Slot}" : "")} · {Who(_s, spk)} · {t.Start:HH:mm:ss} · {FmtSpeech(t.DurMs)}{(t.Playable ? " · 클릭 = 재생" : " · 녹음 중")}",
                t.Playable)).ToList();
            Lanes.Add(new SpeakerLane(spk, Who(_s, spk), colorOf(spk), bars));
        }

        // 참여자 = 입퇴장 기록 ∪ 화자
        var ids = parts.Select(p => p.Msisdn).ToList();
        foreach (var spk in order) if (!ids.Any(x => SameUser(x, spk))) ids.Add(spk);
        foreach (var id in ids)
        {
            var p = parts.FirstOrDefault(x => x.Msisdn == id);
            var mine = turns.Where(t => SameUser(t.Spk, id)).ToList();
            string range = p is { } && (p.Join is not null || p.Leave is not null)
                ? $"{(p.Join is { } j ? j.ToString("HH:mm:ss") : "—")} ~ {(p.Leave is { } l ? l.ToString("HH:mm:ss") : (row.IsLive ? "참여중" : "—"))}" : "";
            bool spoke = order.Any(o => SameUser(o, id));
            Participants.Add(new ParticipantRow(id, Who(_s, id), p?.Role == "initiator" || (p is null && SameUser(id, e.From)), range,
                                                mine.Count, FmtSpeech(mine.Sum(t => t.DurMs)), spoke ? colorOf(order.First(o => SameUser(o, id))) : "", spoke));
        }

        // 이벤트 타임라인 — floor 중재 + 입퇴장, 시간순
        foreach (var f in floor.Where(x => x.Ts is not null))
            _timelineAll.Add(new TimelineItem(f.Ts!.Value, true, FloorColor(f.Op), f.User.Length > 0 ? Who(_s, f.User) : "", FloorLabel(f.Op), FloorDetail(f)));
        foreach (var ev in events.Where(x => x.Ts is not null))
        {
            var (label, color) = EventDisplay(ev.Type);
            string detailTxt = (ev.Type == "member_join" && ev.Role == "initiator" ? "개시자" : "")
                               + (ev.DurationSec is { } d ? $"{(ev.Role == "initiator" && ev.Type == "member_join" ? " · " : "")}{FmtDur(d)}" : "");
            _timelineAll.Add(new TimelineItem(ev.Ts!.Value, false, color, ev.Member.Length > 0 ? Who(_s, ev.Member) : "", label, detailTxt));
        }
        _timelineAll.Sort((a, b) => a.Ts.CompareTo(b.Ts));
        FloorCount = floor.Count; MemberEventCount = events.Count;
        ApplyTimelineLayers();

        // 지표 — 왼쪽 카드와 겹치지 않게 발화 구간/누적·세그먼트를 함께 둔다
        int talkMs = e.TalkMs > 0 ? e.TalkMs : turns.Sum(t => t.DurMs);
        Metrics.Add(new MetricItem("발언 턴", (e.TurnCount > 0 ? e.TurnCount : turns.Count).ToString(), "건", "화자 구간 수 — 동시 발언 세그먼트는 턴이 여럿"));
        Metrics.Add(new MetricItem("녹취 세그먼트", Segments.Count.ToString(), "개", ""));
        if (e.MaxConcurrent > 1) Metrics.Add(new MetricItem("최대 동시 발언", e.MaxConcurrent.ToString(), "명", ""));
        Metrics.Add(new MetricItem("발화 구간", FmtSpeech(e.TotalSpeechMs), "", "겹침을 1회로 센 실제 무전 점유 시간"));
        Metrics.Add(new MetricItem("발화 누적", FmtSpeech(talkMs), "", "화자별 발언 시간의 합"));
        Metrics.Add(new MetricItem("화자", (e.SpeakerCount > 0 ? e.SpeakerCount : order.Count).ToString(), "명", ""));
        if (detail is null && DetailStatus.Length == 0 && e.RecordingId.Length == 0) DetailStatus = "세션 기록 없음";
        RebuildAxisTicks();
        RaiseDetailChanged();
    }

    // ── 발언 타임라인 확대·축소 ──
    [RelayCommand] private void TalkZoomIn() => TalkZoom = Math.Min(TalkZoomMax, TalkZoom * 1.25);
    [RelayCommand] private void TalkZoomOut() => TalkZoom = Math.Max(1.0, TalkZoom / 1.25);
    [RelayCommand] private void TalkZoomReset() => TalkZoom = 1.0;
    /// <summary>배율을 factor 배 — Ctrl+휠(코드비하인드가 커서 기준으로 스크롤 위치를 맞춘다).</summary>
    public void TalkZoomBy(double factor) => TalkZoom = Math.Clamp(TalkZoom * factor, 1.0, TalkZoomMax);

    /// <summary>눈금 — 배율에 따라 6~8 개가 보이도록 간격을 1·2·5·10·15·30초·1·2·5·10·30분 중에서 고른다.</summary>
    private void RebuildAxisTicks()
    {
        AxisTicks.Clear();
        if (Lanes.Count == 0 || _axisSpanMs <= 0) return;
        double targetMs = _axisSpanMs / (6.0 * TalkZoom);
        int[] steps = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600 };
        int stepSec = steps.FirstOrDefault(x => x * 1000.0 >= targetMs);
        if (stepSec == 0) stepSec = steps[^1];
        var first = new DateTime((_axisT0.Ticks / TimeSpan.TicksPerSecond / stepSec + 1) * stepSec * TimeSpan.TicksPerSecond, _axisT0.Kind);
        for (var t = first; (t - _axisT0).TotalMilliseconds < _axisSpanMs && AxisTicks.Count < 400; t = t.AddSeconds(stepSec))
            AxisTicks.Add(new AxisTick((t - _axisT0).TotalMilliseconds / _axisSpanMs, stepSec >= 60 ? t.ToString("HH:mm") : t.ToString("HH:mm:ss")));
    }

    private void ApplyTimelineLayers()
    {
        Timeline.Clear();
        foreach (var it in _timelineAll) if (it.IsFloor ? ShowFloorLayer : ShowMemberLayer) Timeline.Add(it);
        OnPropertyChanged(nameof(HasTimeline));
    }

    private void RaiseDetailChanged()
    {
        OnPropertyChanged(nameof(HasLanes)); OnPropertyChanged(nameof(HasTimeline)); OnPropertyChanged(nameof(HasParticipants));
        OnPropertyChanged(nameof(FloorCount)); OnPropertyChanged(nameof(MemberEventCount)); OnPropertyChanged(nameof(HasRecording));
    }

    // ── 재생 ──

    [RelayCommand] private Task Play() => PlayAsync(retry: false, slot: null);
    [RelayCommand] private Task RetryTranscode() => PlayAsync(retry: true, slot: null);

    /// <summary>발언 턴 막대 클릭 — 그 세그먼트(동시 발언이면 단독 트랙 slot)를 재생.</summary>
    [RelayCommand] private Task PlayTurn(TurnBar bar)
    {
        var seg = Segments.FirstOrDefault(x => x.Seq == bar.Seq);
        if (seg is null || !bar.Playable) return Task.CompletedTask;
        _playQueue.Clear();
        SelectedSegment = seg;
        return PlayAsync(retry: false, slot: bar.Multi ? bar.Slot : null);
    }

    /// <summary>세션 전체 — 재생 가능한 세그먼트를 순서대로(끝나면 다음).</summary>
    [RelayCommand] private Task PlayAll()
    {
        _playQueue.Clear();
        foreach (var s in Segments.Where(x => x.Status != "recording").OrderBy(x => x.Seq)) _playQueue.Enqueue(s);
        if (_playQueue.Count == 0) return Task.CompletedTask;
        SelectedSegment = _playQueue.Dequeue();
        return PlayAsync(retry: false, slot: null);
    }

    private async Task PlayAsync(bool retry, int? slot)
    {
        var m = _s.Management; var rec = Recording; var seg = SelectedSegment;
        if (m is null || rec is null || seg is null) return;
        StopMedia();
        _playCts = new CancellationTokenSource();
        var ct = _playCts.Token;
        LoadingAudio = true; RecordingStatus = "오디오 받는 중…";
        var r = await m.FetchSegmentAudioAsync(rec.Id, seg.Seq, slot, retry, st => RecordingStatus = st, ct);
        LoadingAudio = false;
        if (ct.IsCancellationRequested) return;
        if (!r.Ok) { RecordingStatus = ResponseText.Describe(ResponseText.Area.Recording, r.Code, r.Reason); _playQueue.Clear(); return; }
        MediaSource = r.Value;
        PlayingLabel = $"{Parties()} · {seg.Label}{(slot is { } sl ? $" 슬롯 {sl}" : "")} ({seg.DurationText})";
        RecordingStatus = _playQueue.Count > 0 ? $"재생 중 · 이어서 {_playQueue.Count}개" : "재생 중";
        PlayRequested?.Invoke(this, r.Value);
        _s.Activity.Add(ActivityPanel.Call, ActivityKind.Note, $"녹취 재생 {Parties()} #{seg.Seq}");
    }

    private string Parties() => Selected?.Parties ?? "";

    /// <summary>정지 — 재생 중인 것과 대기열 모두.</summary>
    [RelayCommand] public void Stop() { _playQueue.Clear(); StopMedia(); }

    private void StopMedia()
    {
        _playCts?.Cancel(); _playCts = null;
        if (MediaSource.Length > 0) { StopRequested?.Invoke(this, EventArgs.Empty); MediaSource = ""; PlayingLabel = ""; if (Recording is not null) RecordingStatus = "정지"; }
        LoadingAudio = false;
    }

    /// <summary>재생 끝(MediaEnded) — 창이 알린다. 전체 재생 대기열이 있으면 다음 세그먼트로.</summary>
    public void OnMediaEnded()
    {
        MediaSource = ""; PlayingLabel = "";
        if (_playQueue.Count > 0) { SelectedSegment = _playQueue.Dequeue(); _ = PlayAsync(retry: false, slot: null); return; }
        RecordingStatus = "재생 끝";
    }
    public void OnMediaFailed(string reason) { _playQueue.Clear(); MediaSource = ""; PlayingLabel = ""; RecordingStatus = "재생 실패 — " + reason; }

    // ── --ui-preview 표본 (서버 없이 화면 배치·바인딩 점검 — App.xaml.cs 개발 스위치 전용) ──
    private PttSessionDetail? _previewDetail;
    private IReadOnlyList<RecordingSegment>? _previewSegments;

    /// <summary>표본 이력 한 날(통화 또는 PTT)을 심고 첫 행을 고른다. 서버 조회 없이 §4.6 화면을 그려 보는 개발 스위치용.</summary>
    public void SeedPreview(HistoryKind kind)
    {
        var day = Date.Date;
        DateTime at(int h, int m, int s = 0) => day.AddHours(h).AddMinutes(m).AddSeconds(s);
        var list = new List<HistoryEntry>();
        if (kind == HistoryKind.Ptt)
        {
            _suppressQuery = true; KindIndex = 1; _suppressQuery = false;
            list.Add(new HistoryEntry("ses-1", at(9, 12, 40), HistoryKind.Ptt, "ptt.session.end", "tel:+821310002001", "", "tel:g002", 160, false, "", "ptt/1/2026/09/08/09/S20260908091000000000_1", true)
            { State = "ended", SessionKind = "group", StartTime = at(9, 10), EndTime = at(9, 12, 40), GroupName = "1팀 무전", MemberCount = 6, TurnCount = 7, SpeakerCount = 3,
              TotalSpeechMs = 61_000, TalkMs = 66_000, MaxConcurrent = 2, FloorControl = "on", FloorPolicy = "dual", MaxTalkers = 2,
              People = new[] { "+821310002001", "+821310002002", "+821310002003", "+821310002004" } });
            list.Add(new HistoryEntry("ses-2", at(11, 3, 5), HistoryKind.Ptt, "ptt.session.end", "tel:+821310002002", "", "tel:g002", 25, true, "", "ptt/1/2026/09/08/11/S20260908110240000000_1", true)
            { State = "ended", SessionKind = "group", StartTime = at(11, 2, 40), EndTime = at(11, 3, 5), GroupName = "1팀 무전", TurnCount = 2, SpeakerCount = 1, TotalSpeechMs = 9_000, FloorControl = "on", FloorPolicy = "single" });
            list.Add(new HistoryEntry("ses-3", at(11, 40), HistoryKind.Ptt, "ptt.session.start", "tel:+821310002003", "", "tel:g003", 0, false, "", "", false)
            { State = "active", SessionKind = "group", StartTime = at(11, 40), GroupName = "야간 순찰", TurnCount = 1, SpeakerCount = 1, TotalSpeechMs = 3_000, FloorControl = "on" });
            list.Add(new HistoryEntry("ses-4", at(14, 20, 30), HistoryKind.Ptt, "ptt.session.end", "tel:+821310002001", "", "tel:priv-1", 30, false, "", "", false)
            { State = "ended", SessionKind = "private", StartTime = at(14, 20), EndTime = at(14, 20, 30), FloorControl = "off", People = new[] { "+821310002001", "+821310002004" } });
            var s0 = at(9, 10);
            _previewSegments = new[]
            {
                new RecordingSegment(1, "ptt", "+821310002001", s0.AddSeconds(2), s0.AddSeconds(14), 12_000, false, "ready", new[] { "+821310002001" }, 1),
                new RecordingSegment(2, "ptt", "+821310002002", s0.AddSeconds(20), s0.AddSeconds(45), 25_000, false, "ready", new[] { "+821310002002", "+821310002003" }, 2)
                { Tracks = new[] { new SegmentTrack(0, "audio", new[] { new SpeakerSpan("+821310002002", 0, 25_000) }, false, "ready"),
                                   new SegmentTrack(1, "audio", new[] { new SpeakerSpan("+821310002003", 8_000, 10_000) }, false, "ready") } },
                new RecordingSegment(3, "ptt", "+821310002001", s0.AddSeconds(60), s0.AddSeconds(84), 24_000, false, "ready", new[] { "+821310002001" }, 1),
                new RecordingSegment(4, "ptt", "+821310002003", s0.AddSeconds(100), s0.AddSeconds(150), 50_000, false, "transcoding", new[] { "+821310002003" }, 1),
            };
            _previewDetail = new PttSessionDetail(list[0].RecordingId,
                new[] { new PttParticipant("+821310002001", "initiator", s0, s0.AddSeconds(160)), new PttParticipant("+821310002002", "member", s0.AddSeconds(1), s0.AddSeconds(160)),
                        new PttParticipant("+821310002003", "member", s0.AddSeconds(1), null), new PttParticipant("+821310002004", "member", s0.AddSeconds(30), s0.AddSeconds(90)) },
                new[] { new PttEvent(s0, "session_start", "", "", null), new PttEvent(s0, "member_join", "+821310002001", "initiator", null),
                        new PttEvent(s0.AddSeconds(1), "member_join", "+821310002002", "member", null), new PttEvent(s0.AddSeconds(1), "member_join", "+821310002003", "member", null),
                        new PttEvent(s0.AddSeconds(30), "member_join", "+821310002004", "member", null), new PttEvent(s0.AddSeconds(90), "member_leave", "+821310002004", "member", null),
                        new PttEvent(s0.AddSeconds(160), "session_end", "", "", 160) },
                new[] { new PttFloorEvent(s0.AddSeconds(2), "GRANT", "+821310002001", 0, 0, 1, "dual", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(14), "RELEASE", "+821310002001", 0, null, null, "", false, "", "", null, "", null, null, "", null, null, 300, ""),
                        new PttFloorEvent(s0.AddSeconds(20), "GRANT", "+821310002002", 0, 0, 1, "dual", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(28), "GRANT", "+821310002003", 1, 0, 2, "dual", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(31), "DENY", "+821310002004", null, null, null, "", false, "", "recv_only", 3, "+821310002002", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(38), "RELEASE", "+821310002003", 1, null, null, "", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(45), "RELEASE", "+821310002002", 0, null, null, "", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(60), "GRANT", "+821310002001", 0, 0, 1, "dual", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(70), "QUEUE", "+821310002003", null, null, null, "", false, "", "", null, "", 1, 1, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(84), "RELEASE", "+821310002001", 0, null, null, "", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(100), "GRANT", "+821310002003", 0, 0, 1, "dual", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(150), "REVOKE", "+821310002003", 0, null, null, "", false, "", "", null, "", null, null, "", null, 3, null, "") },
                true);
        }
        else
        {
            list.Add(new HistoryEntry("c1", at(9, 5, 12), HistoryKind.Call, "call.answered", "tel:+821310002001", "tel:+821310009999", "", 42, false, "", "volte/2026/09/08/09/010/01000000001/c1.d", true)
            { State = "ended", CallType = "volte", InviteTime = at(9, 4, 25), AnswerTime = at(9, 4, 30), EndTime = at(9, 5, 12), EndReason = "normal", SipStatus = 200 });
            list.Add(new HistoryEntry("c2", at(9, 31), HistoryKind.Call, "call.missed", "tel:+821310002002", "tel:+821310002001", "", 0, false, "", "volte/2026/09/08/09/010/01000000002/c2.d", false)
            { State = "ended", CallType = "volte", InviteTime = at(9, 30, 40), EndTime = at(9, 31), EndReason = "no_answer", SipStatus = 480 });
            list.Add(new HistoryEntry("c3", at(13, 12, 3), HistoryKind.Call, "call.answered", "tel:+821310002003", "tel:0212345678", "", 305, true, "", "volte/2026/09/08/13/010/01000000003/c3.d", true)
            { State = "ended", CallType = "volte_video", InviteTime = at(13, 6, 50), AnswerTime = at(13, 6, 58), EndTime = at(13, 12, 3), EndReason = "normal", SipStatus = 200 });
            list.Add(new HistoryEntry("c4", at(13, 40), HistoryKind.Call, "call.answered", "tel:+821310002001", "tel:+821310002002", "", 0, false, "", "", false)
            { State = "active", CallType = "volte", InviteTime = at(13, 40), AnswerTime = at(13, 40, 4) });
            list.Add(new HistoryEntry("c5", at(16, 2), HistoryKind.Call, "call.missed", "tel:+821310009999", "tel:+821310002002", "", 0, false, "", "", false)
            { State = "ended", CallType = "volte", InviteTime = at(16, 1, 30), EndTime = at(16, 2), EndReason = "busy", SipStatus = 486 });
            _previewSegments = new[] { new RecordingSegment(1, "volte", "+821310002001", at(9, 4, 30), at(9, 5, 12), 42_000, false, "ready", Array.Empty<string>(), 2) };
        }
        _all = list.OrderByDescending(e => e.Time).ToList();
        _hours = CountHours(_all);
        RebuildHours(); Filter();
        Selected = Rows.FirstOrDefault(r => _previewDetail is { } pd && r.E.RecordingId == pd.RecordingId) ?? Rows.FirstOrDefault(r => r.HasRecording) ?? Rows.FirstOrDefault();
    }

    /// <summary>임시 오디오 파일 정리(창 닫을 때).</summary>
    public static void CleanupTemp()
    {
        try
        {
            string dir = Path.Combine(Path.GetTempPath(), "CIMS", AppPaths.AppName, "rec");
            if (!Directory.Exists(dir)) return;
            foreach (var f in Directory.EnumerateFiles(dir, "*.mp4"))
                try { if (File.GetLastWriteTimeUtc(f) < DateTime.UtcNow.AddHours(-6)) File.Delete(f); } catch (IOException) { }
        }
        catch (Exception) { }
    }

    // ── 표시 사전 (콘솔 pttSession.tsx 의 EVENT_ICONS·FLOOR_OPS·DENY_REASON, VoLTE 이력의 종료사유와 같은 문구) ──

    /// <summary>화자 레인 색 — 등장 순서로 배정(콘솔 SPK_COLORS 와 같은 팔레트).</summary>
    internal static readonly string[] SpkColors = { "#2563EB", "#16A34A", "#D97706", "#9333EA", "#DC2626", "#0891B2", "#CA8A04", "#DB2777", "#4F46E5", "#059669", "#E11D48", "#0D9488" };

    internal static string EndReasonText(string reason) => reason switch
    {
        "normal" => "정상종료", "no_answer" => "무응답", "busy" => "통화중", "rejected" => "거절", "error" => "오류",
        "timeout" => "시간초과", "incomplete" => "비정상 종료(기록 없음)", "" => "—", var x => x,
    };

    internal static string FloorLabel(string op) => op switch
    {
        "GRANT" => "발언권 부여", "RELEASE" => "발언 종료", "IDLE" => "유휴", "REVOKE" => "회수 통지", "REVOKE_END" => "회수 확정",
        "QUEUE" => "대기열 등록", "QUEUE_CANCEL" => "대기 취소", "DENY" => "거절", var x => x,
    };

    internal static string FloorColor(string op) => op switch
    {
        "GRANT" => "#16A34A", "REVOKE" => "#D97706", "REVOKE_END" or "DENY" => "#DC2626", "QUEUE" => "#0891B2", _ => "#9CA3AF",
    };

    /// <summary>op 별 부가 정보 — 규격 용어(TS 24.380)로 사유·대기 순번·회수 유예·선점을 펼친다.</summary>
    internal static string FloorDetail(PttFloorEvent f)
    {
        var p = new List<string>();
        switch (f.Op)
        {
            case "GRANT":
                if (f.Preempt) p.Add(f.PreemptedFrom.Length > 0 ? $"선점 — {UserPartConverter.UserPart(f.PreemptedFrom)} 회수" : "선점");
                if (f.Talkers is { } tk && tk > 1) p.Add($"동시 {tk}명");
                if (f.Slot is { } sl && f.Talkers is { } t2 && t2 > 1) p.Add($"슬롯 {sl}");
                if (f.Prio is { } pr && pr > 0) p.Add($"우선순위 {pr}");
                break;
            case "DENY":
                p.Add(f.Reason switch { "recv_only" => "수신전용(ambient)", "only_one" => "참가자 1인", "broadcast" => "broadcast 비개시자", "" => "", var x => x });
                if (f.Owner.Length > 0) p.Add($"발언 중 {UserPartConverter.UserPart(f.Owner)}");
                if (f.Cause is { } c) p.Add($"cause {c}");
                break;
            case "QUEUE":
                if (f.Pos is { } pos) p.Add(f.QSize is { } qs ? $"대기 {pos}/{qs}" : $"대기 {pos}번");
                break;
            case "QUEUE_CANCEL":
                if (f.Revoked.Length > 0) p.Add($"{UserPartConverter.UserPart(f.Revoked)} 대기 해제");
                if (f.Removed is { } rm && rm > 0) p.Add($"{rm}건 제거");
                break;
            case "REVOKE":
                if (f.GraceSec is { } g) p.Add($"{g}초 후 회수");
                if (f.PreemptedBy.Length > 0) p.Add($"선점자 {UserPartConverter.UserPart(f.PreemptedBy)}");
                break;
            case "REVOKE_END":
                if (f.PreemptedBy.Length > 0) p.Add($"선점자 {UserPartConverter.UserPart(f.PreemptedBy)}");
                break;
            case "RELEASE":
            case "IDLE":
                if (f.IdleMs is { } idle && idle > 0) p.Add($"무음 {FmtSpeech(idle)}");
                break;
        }
        return string.Join(" · ", p.Where(x => x.Length > 0));
    }

    internal static (string Label, string Color) EventDisplay(string type) => type switch
    {
        "session_start" => ("세션 시작", "#16A34A"), "session_end" => ("세션 종료", "#DC2626"),
        "member_join" => ("입장", "#2563EB"), "member_leave" => ("퇴장", "#F97316"),
        "floor-grant" => ("발언 시작", "#16A34A"), "floor-release" => ("발언 종료", "#9CA3AF"),
        "config_change" => ("설정 변경", "#9333EA"), "member_invite" => ("초대", "#0891B2"),
        var x => (x, "#9CA3AF"),
    };

    internal static string FmtDur(int sec) { if (sec <= 0) return "—"; int m = sec / 60; return m > 0 ? $"{m}분 {sec % 60}초" : $"{sec}초"; }
    /// <summary>발화 시간(ms) — 1분 이상은 "m분 s초", 미만은 "s.d초".</summary>
    internal static string FmtSpeech(int ms)
    {
        if (ms <= 0) return "0초";
        if (ms >= 60_000) { int s = ms / 1000; return $"{s / 60}분 {s % 60}초"; }
        return ms >= 10_000 ? $"{ms / 1000}초" : $"{ms / 1000.0:0.#}초";
    }

    /// <summary>이름이 있으면 이름, 없으면 표시 번호(홈 국가 로컬 표기).</summary>
    internal static string Who(DispatchSession s, string u)
    {
        string up = UserPartConverter.UserPart(u);
        return s.Directory.NameOf(up) is { Length: > 0 } n ? n : s.Directory.DisplayNumber(up);
    }
    internal static bool SameUser(string a, string b) =>
        a.Length > 0 && b.Length > 0 && DirectoryService.Normalize(UserPartConverter.UserPart(a)) == DirectoryService.Normalize(UserPartConverter.UserPart(b));
}
