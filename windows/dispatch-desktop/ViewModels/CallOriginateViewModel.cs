// ③ 오른쪽 위 — 일반통화 발신: 번호 필드·[발신]·[픽업 **] + 세그먼트 [다이얼패드|주소록|최근] 중 하나 (§4.3). 다이얼패드는 통화 중 DTMF 겸용.
// 주소록 = 서버 회사 전화번호부(조직 범위 선택 + 조직별 섹션, Android 연락처 탭과 같은 동선) + CSV 외부망 번호.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class BookRow : ObservableObject
{
    public Contact Contact { get; }
    [ObservableProperty] private string _status = "";
    public BookRow(Contact c, string displayNumber, string orgPath) { Contact = c; DisplayNumber = displayNumber; OrgPath = orgPath; }
    public string Name => Contact.Name.Length > 0 ? Contact.Name : DisplayNumber;
    public string Number => Contact.Number;
    /// <summary>로컬 표기(홈 국가 축약). 원본은 툴팁.</summary>
    public string DisplayNumber { get; }
    /// <summary>섹션 헤더(그룹핑 키) — "CIMS › 제1본부 › 팀01". 외부망은 "외부".</summary>
    public string OrgPath { get; }
    public string KindText => Contact.KindText;
    public bool IsExternal => Contact.IsExternal;
    public bool IsMember => Contact.IsMember;
    public string Initial => Name.Length > 0 ? Name[..1] : "?";
    public string SmsTip => IsExternal ? "문자 게이트웨이 미구성" : "문자";
}

public sealed record OrgChoice(string Code, string Label, int Count)
{
    public string Text => Code.Length == 0 ? Label : $"{Label} ({Count})";
}

public sealed partial class CallOriginateViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly List<BookRow> _all = new();

    [ObservableProperty] private string _number = "";
    /// <summary>pad | book | recent</summary>
    [ObservableProperty] private string _mode = "book";
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private OrgChoice? _orgScope;
    public ObservableCollection<OrgChoice> OrgChoices { get; } = new();
    public ObservableCollection<BookRow> Book { get; } = new();
    public ObservableCollection<ActivityRow> Recent { get; } = new();
    /// <summary>번호 필드 입력과 일치하는 주소록 항목(이름·번호·로컬 표기 부분 일치, 최대 8).</summary>
    public ObservableCollection<BookRow> Suggestions { get; } = new();
    public bool HasSuggestions => Suggestions.Count > 0;

    public event EventHandler<string>? SmsRequested;

    public CallOriginateViewModel(DispatchSession s)
    {
        _s = s;
        _mode = s.Settings.Current.OriginateMode;
        s.Directory.Changed += (_, _) => Reload();
        s.Activity.Call.CollectionChanged += (_, _) => ReloadRecent();
        s.DialogChanged += (_, _) => RefreshStatus();
        s.DialogEnded += (_, _) => RefreshStatus();
        Reload();
    }

    public bool IsPad => Mode == "pad";
    public bool IsBook => Mode == "book";
    public bool IsRecent => Mode == "recent";
    public string PickupCode => _s.Settings.Current.PickupFeatureCode;
    /// <summary>활성 통화가 있고 필드가 비어 있으면 패드는 DTMF.</summary>
    public bool PadIsDtmf => Number.Length == 0 && _s.ActiveVolteCall is not null;
    public string PadHint => PadIsDtmf ? "통화 중 — DTMF 로 전송" : "번호를 입력";
    public string BookCount => $"{Book.Count}명";
    public string SyncText => _s.Directory.ServerSyncedAt is DateTime t ? $"동기화 {t:HH:mm}" : "서버 전화번호부 미동기화";

    partial void OnModeChanged(string v) { OnPropertyChanged(nameof(IsPad)); OnPropertyChanged(nameof(IsBook)); OnPropertyChanged(nameof(IsRecent)); _s.Settings.Update(x => x.OriginateMode = v); }
    partial void OnNumberChanged(string v) { OnPropertyChanged(nameof(PadIsDtmf)); OnPropertyChanged(nameof(PadHint)); UpdateSuggestions(); }

    private void UpdateSuggestions()
    {
        Suggestions.Clear();
        string q = Number.Trim();
        if (q.Length > 0 && !q.Contains(':'))
        {
            string qn = DirectoryService.Normalize(q);
            foreach (var r in _all.Where(r => r.Name.Contains(q, StringComparison.OrdinalIgnoreCase)
                                           || (qn.Length > 0 && (DirectoryService.Normalize(r.Number).Contains(qn) || DirectoryService.Normalize(r.DisplayNumber).Contains(qn))))
                                 .Take(8))
                Suggestions.Add(r);
        }
        OnPropertyChanged(nameof(HasSuggestions));
    }
    partial void OnSearchChanged(string v) => Filter();
    partial void OnOrgScopeChanged(OrgChoice? v) => Filter();

    public void RefreshPad() { OnPropertyChanged(nameof(PadIsDtmf)); OnPropertyChanged(nameof(PadHint)); }

    private void Reload()
    {
        var d = _s.Directory;
        _all.Clear();
        foreach (var c in d.CallBook.Where(c => DirectoryService.Normalize(c.Number) != DirectoryService.Normalize(_s.MyExtension)))
            _all.Add(new BookRow(c, d.DisplayNumber(c.Number), c.IsExternal ? "외부" : d.OrgPath(c.OrgCode)));
        string? keep = OrgScope?.Code;
        OrgChoices.Clear();
        OrgChoices.Add(new OrgChoice("", "전체 조직", _all.Count));
        foreach (var (code, label, count) in d.OrgTree(ContactKind.Extension)) OrgChoices.Add(new OrgChoice(code, label, count));
        OrgScope = OrgChoices.FirstOrDefault(o => o.Code == keep) ?? OrgChoices[0];
        Filter();
        ReloadRecent();
        RefreshStatus();
        OnPropertyChanged(nameof(SyncText));
    }

    private void Filter()
    {
        string q = Search.Trim();
        var scope = _s.Directory.OrgScope(OrgScope?.Code ?? "");
        Book.Clear();
        IEnumerable<BookRow> rows = _all;
        if (q.Length > 0) rows = rows.Where(r => r.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || r.Number.Contains(q, StringComparison.OrdinalIgnoreCase) || r.DisplayNumber.Contains(q, StringComparison.OrdinalIgnoreCase));
        else if (scope is not null) rows = rows.Where(r => !r.IsExternal && scope.Contains(r.Contact.OrgCode));
        // 조직 정렬(트리 순) → 이름순. 외부망은 맨 뒤.
        var order = _s.Directory.OrgTree(ContactKind.Extension).Select((o, i) => (o.Code, i)).ToDictionary(x => x.Code, x => x.i);
        foreach (var r in rows.OrderBy(r => r.IsExternal ? int.MaxValue : order.TryGetValue(r.Contact.OrgCode, out int i) ? i : int.MaxValue - 1).ThenBy(r => r.Name, StringComparer.CurrentCulture))
            Book.Add(r);
        OnPropertyChanged(nameof(BookCount));
    }

    private void ReloadRecent()
    {
        Recent.Clear();
        foreach (var r in _s.Activity.Call.Where(r => r.CanRedial).Take(50)) Recent.Add(r);
    }

    private void RefreshStatus()
    {
        foreach (var r in _all)
        {
            var d = _s.Dialogs.Where(x => x.WatchedNumber == r.Number).OrderByDescending(x => x.IsConfirmed).FirstOrDefault();
            r.Status = d is null ? "" : d.IsConfirmed ? "통화중" : d.IsEarly ? "링잉" : "";
        }
    }

    [RelayCommand] private void SetMode(string m) => Mode = m;
    [RelayCommand] private void Dial() { string n = Number.Trim(); if (n.Length == 0) return; if (Resolve(n) is { } t && _s.Dial(t).Ok) Number = ""; }
    [RelayCommand] private void Pickup() => _s.Pickup();
    [RelayCommand]
    private void Pad(string key)
    {
        if (PadIsDtmf) { var c = _s.ActiveVolteCall; if (c is not null) _s.Dtmf(c, key); return; }
        Number += key;
    }
    [RelayCommand] private void Backspace() { if (Number.Length > 0) Number = Number[..^1]; }
    [RelayCommand] private void Clear() => Number = "";
    [RelayCommand] private void Call(BookRow r) => _s.Dial(r.Number);
    /// <summary>제안 행 클릭 — 필드에 채움(발신은 Resolve 가 원본 번호로).</summary>
    [RelayCommand] private void Pick(BookRow r) { Number = r.DisplayNumber; Suggestions.Clear(); OnPropertyChanged(nameof(HasSuggestions)); }
    [RelayCommand] private void CallSuggestion(BookRow r) { if (_s.Dial(r.Number).Ok) Number = ""; }
    [RelayCommand] private void Sms(BookRow r) { if (!r.IsExternal) SmsRequested?.Invoke(this, r.Number); }
    [RelayCommand] private void Redial(ActivityRow r) { if (r.Number.Length > 0) _s.Dial(r.Number); }
    [RelayCommand] private void SmsRecent(ActivityRow r) { if (r.Number.Length > 0 && !_s.Directory.IsExternal(r.Number)) SmsRequested?.Invoke(this, r.Number); }
    [RelayCommand] private async Task SyncAsync() { await _s.SyncDirectoryAsync(); OnPropertyChanged(nameof(SyncText)); }
    public void Fill(string number) => Number = number;

    /// <summary>이름 입력이면 주소록에서 번호로. 로컬 표기 번호면 원본(E.164)으로.</summary>
    private string? Resolve(string input)
    {
        var byName = _all.FirstOrDefault(r => r.Name.Equals(input, StringComparison.OrdinalIgnoreCase));
        if (byName is not null) return byName.Number;
        var byLocal = _all.FirstOrDefault(r => DirectoryService.Normalize(r.DisplayNumber) == DirectoryService.Normalize(input));
        return byLocal?.Number ?? input;
    }
}
