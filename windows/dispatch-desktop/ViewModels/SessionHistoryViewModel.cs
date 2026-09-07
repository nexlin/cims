// 관리 창 › 이력·녹취 탭(§4.5.3) — 서버 통합 이력을 **창 조회**(`/provisioning/history?since=&until=`, 하루 단위)로 받아 VoLTE 통화·PTT 세션을
// 표로 보이고, 녹취가 있는 행(`hasRecording`)은 세그먼트 목록 → [재생] 으로 MP4(AAC) 를 받아(`/provisioning/recordings/{id}/segments/{seq}/audio`)
// 창의 MediaElement 로 튼다. 범위·감사는 서버(관제 그룹 monitor_scope/ptt_listen, E-AUD-016 tap_mode=history|recording). ② ④ 실시간 내역과 달리
// 이 탭은 지난 기록 열람 전용이다(폴링 없음).
using System.Collections.ObjectModel;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed class HistoryRow
{
    public HistoryEntry E { get; }
    public string TimeText => E.Time.ToString("HH:mm:ss");
    public string KindText { get; }
    public string Parties { get; }
    public string DurationText => E.DurationSec > 0 ? TimeSpan.FromSeconds(E.DurationSec).ToString(@"mm\:ss") : "";
    public string EventText { get; }
    public bool HasRecording => E.HasRecording;
    public bool IsEmergency => E.Emergency;
    public HistoryRow(HistoryEntry e, DispatchSession s)
    {
        E = e;
        KindText = e.Kind == HistoryKind.Ptt ? "PTT" : "통화";
        string Label(string u) => s.Directory.NameOf(UserPartConverter.UserPart(u)) is { Length: > 0 } n
            ? $"{n} {s.Directory.DisplayNumber(UserPartConverter.UserPart(u))}" : s.Directory.DisplayNumber(UserPartConverter.UserPart(u));
        if (e.Kind == HistoryKind.Ptt)
        {
            string g = s.Groups.FirstOrDefault(x => string.Equals(x.Uri, e.Group, StringComparison.OrdinalIgnoreCase))?.Name ?? UserPartConverter.UserPart(e.Group);
            Parties = e.From.Length > 0 ? $"{g} · 개시 {Label(e.From)}" : g;
        }
        else Parties = $"{Label(e.From)} → {Label(e.To)}";
        EventText = e.Event switch
        {
            "call.answered" => "응답", "call.missed" => "부재", "ptt.session.start" => "진행 중", "ptt.session.end" => "종료",
            _ => e.Event,
        };
    }
}

public sealed partial class SessionHistoryViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private CancellationTokenSource? _playCts;

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

    private IReadOnlyList<HistoryEntry> _all = Array.Empty<HistoryEntry>();
    public ObservableCollection<HistoryRow> Rows { get; } = new();
    public ObservableCollection<RecordingSegment> Segments { get; } = new();
    public bool HasError => Error.Length > 0;
    public bool HasRecording => Recording is not null && Segments.Count > 0;
    public bool CanPlay => SelectedSegment is not null && !LoadingAudio;
    public bool IsPlaying => MediaSource.Length > 0;

    /// <summary>재생 시작(파일 경로)/정지 — MediaElement 는 창 코드비하인드가 든다.</summary>
    public event EventHandler<string>? PlayRequested;
    public event EventHandler? StopRequested;

    partial void OnErrorChanged(string value) => OnPropertyChanged(nameof(HasError));
    partial void OnSearchChanged(string value) => Filter();
    partial void OnKindIndexChanged(int value) => _ = QueryAsync();
    partial void OnDateChanged(DateTime value) => _ = QueryAsync();
    partial void OnSelectedChanged(HistoryRow? value) => _ = LoadRecordingAsync(value);
    partial void OnSelectedSegmentChanged(RecordingSegment? value) => OnPropertyChanged(nameof(CanPlay));
    partial void OnLoadingAudioChanged(bool value) => OnPropertyChanged(nameof(CanPlay));
    partial void OnMediaSourceChanged(string value) => OnPropertyChanged(nameof(IsPlaying));
    partial void OnRecordingChanged(RecordingInfo? value) => OnPropertyChanged(nameof(HasRecording));

    private HistoryKind Kind => KindIndex == 1 ? HistoryKind.Ptt : HistoryKind.Call;

    [RelayCommand] public async Task QueryAsync()
    {
        var m = _s.Management; if (m is null) { Error = "로그인 전"; return; }
        Busy = true; Error = "";
        var from = Date.Date; var to = Date.Date.AddDays(1).AddSeconds(-1);
        var r = await m.QueryHistoryAsync(Kind, from, to);
        Busy = false;
        if (!r.Ok) { Error = ResponseText.Describe(ResponseText.Area.Management, r.Code, r.Reason); _all = Array.Empty<HistoryEntry>(); Filter(); return; }
        _all = r.Value.OrderByDescending(e => e.Time).ToList();      // 표시는 최근이 위
        Filter();
    }

    private void Filter()
    {
        Rows.Clear();
        string q = Search.Trim();
        string qn = DirectoryService.Normalize(q);
        foreach (var e in _all)
        {
            var row = new HistoryRow(e, _s);
            if (q.Length > 0 && !row.Parties.Contains(q, StringComparison.OrdinalIgnoreCase)
                && !(qn.Length > 0 && (DirectoryService.Normalize(e.From).Contains(qn) || DirectoryService.Normalize(e.To).Contains(qn)))) continue;
            Rows.Add(row);
        }
        Summary = $"{Date:yyyy-MM-dd} · {Rows.Count}건{(Rows.Count(x => x.HasRecording) is > 0 and var n ? $" · 녹취 {n}건" : "")}";
    }

    private async Task LoadRecordingAsync(HistoryRow? row)
    {
        Stop();
        Recording = null; Segments.Clear(); RecordingStatus = "";
        OnPropertyChanged(nameof(HasRecording));
        if (row is null || row.E.RecordingId.Length == 0) return;
        if (!row.HasRecording) { RecordingStatus = "녹취 없음"; return; }
        var m = _s.Management; if (m is null) return;
        RecordingStatus = "녹취 정보 조회 중…";
        var r = await m.GetRecordingAsync(row.E.RecordingId);
        if (Selected != row) return;                                   // 그 사이 다른 행 선택
        if (!r.Ok) { RecordingStatus = ResponseText.Describe(ResponseText.Area.Recording, r.Code, r.Reason); return; }
        Recording = r.Value;
        foreach (var seg in r.Value.Segments) Segments.Add(seg);
        OnPropertyChanged(nameof(HasRecording));
        RecordingStatus = Segments.Count > 0 ? $"세그먼트 {Segments.Count}개 · {r.Value.Status}" : "세그먼트 없음(녹음 진행 중이거나 미디어 없음)";
        SelectedSegment = Segments.FirstOrDefault();
    }

    [RelayCommand] private Task Play() => PlayAsync(retry: false);
    [RelayCommand] private Task RetryTranscode() => PlayAsync(retry: true);

    private async Task PlayAsync(bool retry)
    {
        var m = _s.Management; var rec = Recording; var seg = SelectedSegment;
        if (m is null || rec is null || seg is null) return;
        Stop();
        _playCts = new CancellationTokenSource();
        var ct = _playCts.Token;
        LoadingAudio = true; RecordingStatus = "오디오 받는 중…";
        var r = await m.FetchSegmentAudioAsync(rec.Id, seg.Seq, null, retry, st => RecordingStatus = st, ct);
        LoadingAudio = false;
        if (ct.IsCancellationRequested) return;
        if (!r.Ok) { RecordingStatus = ResponseText.Describe(ResponseText.Area.Recording, r.Code, r.Reason); return; }
        MediaSource = r.Value;
        PlayingLabel = $"{Parties()} · {seg.Label} ({seg.DurationText})";
        RecordingStatus = "재생 중";
        PlayRequested?.Invoke(this, r.Value);
        _s.Activity.Add(ActivityPanel.Call, ActivityKind.Note, $"녹취 재생 {Parties()} #{seg.Seq}");
    }

    private string Parties() => Selected?.Parties ?? "";

    [RelayCommand] public void Stop()
    {
        _playCts?.Cancel(); _playCts = null;
        if (MediaSource.Length > 0) { StopRequested?.Invoke(this, EventArgs.Empty); MediaSource = ""; PlayingLabel = ""; if (Recording is not null) RecordingStatus = "정지"; }
        LoadingAudio = false;
    }

    /// <summary>재생 끝(MediaEnded) — 창이 알린다.</summary>
    public void OnMediaEnded() { MediaSource = ""; PlayingLabel = ""; RecordingStatus = "재생 끝"; }
    public void OnMediaFailed(string reason) { MediaSource = ""; PlayingLabel = ""; RecordingStatus = "재생 실패 — " + reason; }

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
}
