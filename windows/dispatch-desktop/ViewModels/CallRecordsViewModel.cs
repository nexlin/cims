// [통화] 기록(§4.4) — 일반통화 내역과 문자(SMS·LMS)를 **상대별**로 한 줄기에 묶는다. 목록(300) = 상대마다 한 줄(마지막 사건 라벨·요약·시각·미읽음),
// 오른쪽 = 그 상대의 통화 알약·문자 말풍선을 시간순으로 + 문자 입력(외부망은 게이트웨이가 없으면 막고 이유를 보인다).
// 원천 = 링 버퍼(ActivityLog.Call — 이 데스크의 착신·발신·부재·전달·픽업·감청) + SMS 스레드(SmsMessagesViewModel) + 진행 중 VoLTE 호.
// 시간순 표(응답·종료·울림 열)는 두지 않는다 — 끝난 통화의 날짜 조회는 [이력] F2, 내보내기는 [CSV].
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

/// <summary>기록의 사건 종류 — 목록 라벨(색)과 거르기의 기준.</summary>
public enum RecordKind { Live, Missed, Incoming, Outgoing, Transfer, Pickup, Monitor, Sms }

/// <summary>상대 한 줄.</summary>
public sealed partial class RecordRow : ObservableObject
{
    public string Key { get; }
    [ObservableProperty] private string _name = "";
    [ObservableProperty] private string _sub = "";
    [ObservableProperty] private RecordKind _kind;
    [ObservableProperty] private string _last = "";
    [ObservableProperty] private DateTime _time;
    [ObservableProperty] private int _unread;
    [ObservableProperty] private bool _isSelected;
    public bool IsExternal { get; set; }
    public bool HasCall { get; set; }
    public bool HasSms { get; set; }
    public bool HasMissed { get; set; }
    public bool HasPilot { get; set; }
    public RecordRow(string key) { Key = key; }
    /// <summary>아바타 머리글자 — 번호뿐인 상대(이름 없음)는 빈 값(화면이 전화 아이콘을 그린다).</summary>
    public string Initial => Name.Trim() is { Length: > 0 } n && !(char.IsDigit(n[0]) || n[0] == '+') ? n[..1] : "";
    public string KindText => RecordsText.Of(Kind);
    public string TimeText => Kind == RecordKind.Live ? "지금" : Time == default ? "" : Time.Date == DateTime.Today ? Time.ToString("HH:mm") : Time.ToString("M/d");
    public bool HasUnread => Unread > 0;
    partial void OnNameChanged(string value) => OnPropertyChanged(nameof(Initial));
    partial void OnKindChanged(RecordKind value) { OnPropertyChanged(nameof(KindText)); OnPropertyChanged(nameof(TimeText)); }
    partial void OnTimeChanged(DateTime value) => OnPropertyChanged(nameof(TimeText));
    partial void OnUnreadChanged(int value) => OnPropertyChanged(nameof(HasUnread));
}

/// <summary>한 줄기의 사건 — 통화(가운데 알약) 또는 문자(말풍선, Message).</summary>
public sealed class RecordItem
{
    public DateTime Time { get; init; }
    public RecordKind Kind { get; init; }
    public string Text { get; init; } = "";
    public Message? Message { get; init; }
    public bool IsCall => Message is null;
    public bool IsIn => Message is { IsOut: false };
    public bool IsOut => Message is { IsOut: true };
    public string KindText => RecordsText.Of(Kind);
    public string TimeText => Time.ToString("HH:mm");
}

public static class RecordsText
{
    public static string Of(RecordKind k) => k switch
    {
        RecordKind.Live => "통화 중", RecordKind.Missed => "부재", RecordKind.Incoming => "착신", RecordKind.Outgoing => "발신", RecordKind.Transfer => "전달",
        RecordKind.Pickup => "픽업", RecordKind.Monitor => "감청", _ => "문자",
    };
}

public sealed partial class CallRecordsViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly SmsMessagesViewModel _sms;
    private readonly CallDeskViewModel _desk;
    private readonly Dictionary<string, RecordRow> _rows = new(StringComparer.Ordinal);
    private MessageThread? _watched;

    public ObservableCollection<RecordRow> Rows { get; } = new();
    public ObservableCollection<RecordItem> Items { get; } = new();
    /// <summary>all | call | sms | missed | pilot</summary>
    [ObservableProperty] private string _filter = "all";
    [ObservableProperty] private RecordRow? _selected;

    public event EventHandler? HistoryRequested;
    public event EventHandler<string>? MenuRequested;
    /// <summary>한 줄기에 새 사건이 붙었다 — 뷰가 맨 아래로.</summary>
    public event EventHandler? ItemsGrew;

    public CallRecordsViewModel(DispatchSession s, SmsMessagesViewModel sms, CallDeskViewModel desk)
    {
        _s = s; _sms = sms; _desk = desk;
        s.Activity.Call.CollectionChanged += (_, _) => Rebuild();
        s.SessionAdded += (_, x) => { if (x.IsVolteCall) Rebuild(); };
        s.SessionEnded += (_, x) => { if (x.IsVolteCall) Rebuild(); };
        s.Directory.Changed += (_, _) => Rebuild();
        sms.Threads.CollectionChanged += (_, _) => Rebuild();
        sms.UnreadChanged += (_, _) => RefreshUnread();
        sms.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(SmsMessagesViewModel.Input) or nameof(SmsMessagesViewModel.CountText) or nameof(SmsMessagesViewModel.CanSend))
            { OnPropertyChanged(nameof(CountText)); OnPropertyChanged(nameof(CanSend)); }
        };
        s.PropertyChanged += (_, e) => { if (e.PropertyName is nameof(DispatchSession.SmsGateway) or nameof(DispatchSession.CanSms)) RefreshSelection(); };
        Rebuild();
    }

    // ── 머리 · 오늘 집계(이 데스크 것만 — 서버 이력의 타인 간 통화 제외) ──
    public int TodayAnswered => _desk.TodayAnswered;
    public int TodayMissed => _desk.TodayMissed;
    public int TodayOutgoing => _desk.TodayOutgoing;
    public int TodayTransfer => _desk.TodayTransfer;
    public int TodayMonitor => _desk.TodayMonitor;
    public int TodaySms => _sms.Threads.SelectMany(t => t.Messages).Count(m => m.Time.Date == DateTime.Today);
    public string MissedChipText => TodayMissed > 0 ? $"부재 {TodayMissed}" : "부재";
    public bool IsEmpty => Rows.Count == 0;

    // ── 선택한 상대 ──
    public bool HasSelection => Selected is not null;
    public string SelName => Selected?.Name ?? "";
    public bool SelExternal => Selected?.IsExternal == true;
    public string SelDetail => Selected is null ? "" : Detail(Selected.Key);
    /// <summary>외부망 번호인데 게이트웨이가 없다 — 문자를 보낼 수 없고 받는 것만 된다.</summary>
    public bool SmsBlocked => Selected is { IsExternal: true } && !_s.SmsGateway;
    public string BlockText => "외부망 번호입니다 — 게이트웨이가 없어 문자를 보낼 수 없습니다(받는 것은 됩니다)";
    public string Input { get => _sms.Input; set { if (_sms.Input != value) { _sms.Input = value; OnPropertyChanged(); } } }
    public string CountText => _sms.CountText;
    public bool CanSend => !SmsBlocked && _sms.CanSend;
    public bool CanDial => Selected is not null && _s.Volte is not null;

    partial void OnFilterChanged(string value) => ApplyFilter();
    partial void OnSelectedChanged(RecordRow? value)
    {
        foreach (var r in Rows) r.IsSelected = r == value;
        if (value is not null) _sms.OpenNumber(value.Key);             // 문자 입력·읽음 표시는 SMS 스레드가 맡는다
        Watch(value is null ? null : _sms.Threads.FirstOrDefault(t => Key(t.Key) == value.Key));
        RefreshSelection();
        RebuildItems();
        RefreshUnread();
    }

    [RelayCommand] private void SetFilter(string f) => Filter = f;
    [RelayCommand] private void Pick(RecordRow r) => Selected = r;
    [RelayCommand] private void Send() { if (CanSend) { _sms.SendCommand.Execute(null); OnPropertyChanged(nameof(Input)); } }
    [RelayCommand] private void Dial() { if (Selected is not null) _s.Dial(Selected.Key); }
    [RelayCommand] private void Menu() { if (Selected is not null) MenuRequested?.Invoke(this, Selected.Key); }
    [RelayCommand] private void Resend(Message m) => _sms.ResendCommand.Execute(m);
    [RelayCommand] private void OpenHistory() => HistoryRequested?.Invoke(this, EventArgs.Empty);
    [RelayCommand]
    private void Export()
    {
        var dlg = new Microsoft.Win32.SaveFileDialog { FileName = $"call-records-{DateTime.Now:yyyyMMdd-HHmm}.csv", Filter = "CSV|*.csv" };
        if (dlg.ShowDialog() == true) _s.Activity.ExportCsv(ActivityPanel.Call, dlg.FileName);
    }

    /// <summary>사람 메뉴 [기록 보기]·[통화] 모드의 [문자] — 그 상대 한 줄기를 연다(기록이 없으면 빈 줄기를 새로).</summary>
    public void Open(string number)
    {
        string key = Key(number);
        if (key.Length == 0) return;
        if (!_rows.TryGetValue(key, out var row))
        {
            row = _rows[key] = new RecordRow(key) { Kind = RecordKind.Sms, Time = DateTime.Now };
            Describe(row);
            Rows.Insert(0, row);
        }
        if (Filter != "all" && !Pass(row)) Filter = "all";
        Selected = row;
    }

    private string Key(string numberOrUri) => _s.Directory.Canonical(UserPartConverter.UserPart(numberOrUri));

    /// <summary>상대 이름·부제 — 내선 가입자면 번호가 부제, 외부 번호면 이름 없이 번호. 대표번호로 온 호는 부제 "대표 7000".</summary>
    private void Describe(RecordRow r)
    {
        string raw = r.Key;
        string name = _s.Directory.NameOf(raw);
        string disp = _s.Directory.DisplayNumber(raw);
        r.Name = name.Length > 0 ? name : disp;
        r.IsExternal = _s.Directory.IsExternal(raw);
        r.Sub = r.HasPilot && _s.PilotId.Length > 0 ? "대표 " + UserPartConverter.UserPart(_s.PilotId) : name.Length > 0 ? disp : "";
    }

    private string Detail(string key)
    {
        var parts = new List<string>();
        string disp = _s.Directory.DisplayNumber(key);
        if (_s.Directory.NameOf(key).Length > 0) parts.Add((_s.Directory.IsExternal(key) ? "" : "내선 ") + disp);
        var c = _s.Directory.Contacts.FirstOrDefault(x => _s.Directory.Canonical(x.Number) == key);
        if (c is not null && c.OrgCode.Length > 0) parts.Add(_s.Directory.OrgName(c.OrgCode));
        if (_rows.TryGetValue(key, out var r) && r.HasPilot && _s.PilotId.Length > 0) parts.Add($"대표번호 {UserPartConverter.UserPart(_s.PilotId)} 으로 온 호");
        return string.Join(" · ", parts);
    }

    private static RecordKind KindOf(ActivityRow a) => a.Kind switch
    {
        ActivityKind.Missed => RecordKind.Missed, ActivityKind.Incoming => RecordKind.Incoming, ActivityKind.Outgoing => RecordKind.Outgoing,
        ActivityKind.Transfer => RecordKind.Transfer, ActivityKind.Pickup => RecordKind.Pickup,
        ActivityKind.ListenStart or ActivityKind.ListenEnd => RecordKind.Monitor, _ => RecordKind.Sms,
    };

    /// <summary>통화 알약·목록 요약 — "03:12 · 응답 7004 한지원" / "부재 · 넘김 7100".</summary>
    private static string CallText(ActivityRow a) => a.Kind switch
    {
        ActivityKind.Missed => a.Detail.Length > 0 ? $"부재 · {a.Detail}" : "부재",
        _ => a.Detail.Length > 0 ? a.Detail : a.Title,
    };

    public void Rebuild()
    {
        foreach (var r in _rows.Values) { r.HasCall = r.HasSms = r.HasMissed = r.HasPilot = false; r.Time = default; }
        var seen = new HashSet<string>(StringComparer.Ordinal);
        void Touch(string key, DateTime at, RecordKind kind, string last)
        {
            if (!_rows.TryGetValue(key, out var r)) r = _rows[key] = new RecordRow(key);
            seen.Add(key);
            if (at >= r.Time || r.Time == default) { r.Time = at; r.Kind = kind; r.Last = last; }
        }
        foreach (var a in _s.Activity.Call.Where(a => !a.IsOthers && a.Number.Length > 0 && a.Kind != ActivityKind.Sms && a.Kind != ActivityKind.ListenEnd))
        {
            string key = Key(a.Number);
            if (key.Length == 0) continue;
            Touch(key, a.Time, KindOf(a), CallText(a));
            var r = _rows[key];
            r.HasCall = true; r.HasMissed |= a.IsMissed; r.HasPilot |= a.IsPilot;
        }
        foreach (var t in _sms.Threads)
        {
            var m = t.Messages.LastOrDefault();
            if (m is null) continue;
            string key = Key(t.Key);
            Touch(key, m.Time, RecordKind.Sms, (m.IsOut ? "나: " : "") + m.Text);
            _rows[key].HasSms = true;
        }
        foreach (var x in _s.Sessions.Where(x => x.IsVolteCall))
        {
            string key = Key(x.PeerNumber);
            if (key.Length == 0) continue;
            if (!_rows.TryGetValue(key, out var r)) r = _rows[key] = new RecordRow(key);
            seen.Add(key);
            r.Time = DateTime.Now; r.Kind = RecordKind.Live; r.HasCall = true;
            r.Last = $"{x.StateText} {DispatchSession.Fmt(x.Elapsed)}" + (x.Info.Muted ? " · 음소거" : "");
        }
        // 한 줄기를 열어 둔 상대는 기록이 없어도 남긴다(사람 메뉴 [기록 보기] 로 연 빈 줄기)
        if (Selected is not null) seen.Add(Selected.Key);
        foreach (var k in _rows.Keys.Where(k => !seen.Contains(k)).ToList()) _rows.Remove(k);
        foreach (var r in _rows.Values) Describe(r);
        ApplyFilter();
        RefreshUnread();
        foreach (var p in new[] { nameof(TodayAnswered), nameof(TodayMissed), nameof(TodayOutgoing), nameof(TodayTransfer), nameof(TodayMonitor), nameof(TodaySms), nameof(MissedChipText) })
            OnPropertyChanged(p);
        RefreshSelection();
        RebuildItems();
    }

    private bool Pass(RecordRow r) => Filter switch
    {
        "call" => r.HasCall, "sms" => r.HasSms, "missed" => r.HasMissed, "pilot" => r.HasPilot, _ => true,
    };

    private void ApplyFilter()
    {
        var ordered = _rows.Values.Where(r => Pass(r) || r == Selected).OrderByDescending(r => r.Kind == RecordKind.Live).ThenByDescending(r => r.Time).ToList();
        for (int i = 0; i < ordered.Count; ++i)
        {
            int at = Rows.IndexOf(ordered[i]);
            if (at < 0) Rows.Insert(i, ordered[i]);
            else if (at != i) Rows.Move(at, i);
        }
        while (Rows.Count > ordered.Count) Rows.RemoveAt(Rows.Count - 1);
        OnPropertyChanged(nameof(IsEmpty));
    }

    private void RefreshUnread()
    {
        foreach (var r in _rows.Values) r.Unread = _sms.Threads.FirstOrDefault(t => Key(t.Key) == r.Key)?.Unread ?? 0;
    }

    private void RefreshSelection()
    {
        foreach (var p in new[] { nameof(HasSelection), nameof(SelName), nameof(SelExternal), nameof(SelDetail), nameof(SmsBlocked), nameof(CanSend), nameof(CanDial), nameof(Input), nameof(CountText) })
            OnPropertyChanged(p);
    }

    private void Watch(MessageThread? t)
    {
        if (_watched == t) return;
        if (_watched is not null) _watched.Messages.CollectionChanged -= OnThreadMessages;
        _watched = t;
        if (_watched is not null) _watched.Messages.CollectionChanged += OnThreadMessages;
    }
    private void OnThreadMessages(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e) => Rebuild();

    /// <summary>선택한 상대의 한 줄기 — 통화(링 버퍼)와 문자(SMS 스레드)를 시간순으로.</summary>
    private void RebuildItems()
    {
        int before = Items.Count;
        Items.Clear();
        if (Selected is null) return;
        string key = Selected.Key;
        var list = new List<RecordItem>();
        foreach (var a in _s.Activity.Call.Where(a => !a.IsOthers && a.Number.Length > 0 && a.Kind != ActivityKind.Sms && a.Kind != ActivityKind.ListenEnd && Key(a.Number) == key))
            list.Add(new RecordItem { Time = a.Time, Kind = KindOf(a), Text = CallText(a) });
        if (_sms.Threads.FirstOrDefault(t => Key(t.Key) == key) is { } th)
            foreach (var m in th.Messages) list.Add(new RecordItem { Time = m.Time, Kind = RecordKind.Sms, Text = m.Text, Message = m });
        foreach (var x in _s.Sessions.Where(x => x.IsVolteCall && Key(x.PeerNumber) == key))
            list.Add(new RecordItem { Time = x.ConnectedAt ?? x.StartedAt, Kind = RecordKind.Live, Text = x.StateText });
        foreach (var it in list.OrderBy(i => i.Time)) Items.Add(it);
        if (Items.Count > before) ItemsGrew?.Invoke(this, EventArgs.Empty);
    }

    public void Tick()
    {
        if (!_s.Sessions.Any(x => x.IsVolteCall)) return;
        foreach (var x in _s.Sessions.Where(x => x.IsVolteCall))
            if (_rows.TryGetValue(Key(x.PeerNumber), out var r)) r.Last = $"{x.StateText} {DispatchSession.Fmt(x.Elapsed)}" + (x.Info.Muted ? " · 음소거" : "");
    }
}
