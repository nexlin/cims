// [통화] 발신(§4.3) — «통화» 머리의 번호칸(입력 제안)·[발신]·[키패드](누를 때만 뜨는 팝오버 — 통화 중·번호칸이 비면 DTMF)·[픽업] +
// 오른쪽 [주소록] 패널(서버 회사 전화번호부 — 조직 칩 거르기 + 검색, 줄 = 사람 메뉴 · [바로 걸기]) + CSV 외부망 번호.
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
    private readonly bool _smsGateway;
    public BookRow(Contact c, string displayNumber, string orgPath, bool smsGateway = false) { Contact = c; DisplayNumber = displayNumber; OrgPath = orgPath; _smsGateway = smsGateway; }
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
    /// <summary>[문자] 가능 — 가입자 번호, 또는 외부망 번호인데 전화 회선에 SMS 게이트웨이가 있을 때(capabilities.smsGateway).</summary>
    public bool CanSms => !IsExternal || _smsGateway;
    public string SmsTip => CanSms ? "문자" : "문자 게이트웨이 미구성";
}

public sealed record OrgChoice(string Code, string Label, int Count)
{
    public string Text => Code.Length == 0 ? Label : $"{Label} ({Count})";
    /// <summary>칩 글자 — 들여쓰기 없는 이름("전체 조직" 은 "전체").</summary>
    public string ChipText => Code.Length == 0 ? "전체" : Label.Trim();
}

public sealed partial class CallOriginateViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly List<BookRow> _all = new();

    [ObservableProperty] private string _number = "";
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private OrgChoice? _orgScope;
    public ObservableCollection<OrgChoice> OrgChoices { get; } = new();
    /// <summary>[주소록] 조직 칩 — 전체 + 위 두 단(깊은 조직은 검색으로).</summary>
    public ObservableCollection<OrgChoice> OrgChips { get; } = new();
    public ObservableCollection<BookRow> Book { get; } = new();
    /// <summary>번호 필드 입력과 일치하는 주소록 항목(이름·번호·로컬 표기 부분 일치, 최대 8).</summary>
    public ObservableCollection<BookRow> Suggestions { get; } = new();
    public bool HasSuggestions => Suggestions.Count > 0;

    public event EventHandler<string>? SmsRequested;

    public CallOriginateViewModel(DispatchSession s)
    {
        _s = s;
        s.Directory.Changed += (_, _) => Reload();
        s.DialogChanged += (_, _) => RefreshStatus();
        s.DialogEnded += (_, _) => RefreshStatus();
        Reload();
    }

    public string PickupCode => _s.Settings.Current.PickupFeatureCode;
    /// <summary>활성 통화가 있고 필드가 비어 있으면 패드는 DTMF.</summary>
    public bool PadIsDtmf => Number.Length == 0 && _s.ActiveVolteCall is not null;
    public string PadHint => PadIsDtmf ? "통화 중이면 DTMF 로 보낸다 · Esc 로 닫기" : "통화 중이면 DTMF 로 보낸다 · Esc 로 닫기";
    /// <summary>키패드 팝오버 화면 — 번호칸이 비고 통화 중이면 보낸 DTMF, 아니면 번호.</summary>
    public string PadDisplay => PadIsDtmf ? _dtmfSent : Number;
    private string _dtmfSent = "";
    public string BookCount => $"{Book.Count}명";
    public string SyncText => _s.Directory.ServerSyncedAt is DateTime t ? $"동기화 {t:HH:mm}" : "서버 전화번호부 미동기화";

    partial void OnNumberChanged(string value) { OnPropertyChanged(nameof(PadIsDtmf)); OnPropertyChanged(nameof(PadHint)); OnPropertyChanged(nameof(PadDisplay)); UpdateSuggestions(); }

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
    partial void OnSearchChanged(string value) => Filter();
    partial void OnOrgScopeChanged(OrgChoice? value) => Filter();

    public void RefreshPad() { if (_s.ActiveVolteCall is null) _dtmfSent = ""; OnPropertyChanged(nameof(PadIsDtmf)); OnPropertyChanged(nameof(PadHint)); OnPropertyChanged(nameof(PadDisplay)); }

    private void Reload()
    {
        var d = _s.Directory;
        _all.Clear();
        foreach (var c in d.CallBook.Where(c => DirectoryService.Normalize(c.Number) != DirectoryService.Normalize(_s.MyExtension)))
            _all.Add(new BookRow(c, d.DisplayNumber(c.Number), c.IsExternal ? "외부" : d.OrgPath(c.OrgCode), _s.SmsGateway));
        string? keep = OrgScope?.Code;
        OrgChoices.Clear();
        OrgChoices.Add(new OrgChoice("", "전체 조직", _all.Count));
        foreach (var (code, label, count) in d.OrgTree(ContactKind.Extension)) OrgChoices.Add(new OrgChoice(code, label, count));
        OrgScope = OrgChoices.FirstOrDefault(o => o.Code == keep) ?? OrgChoices[0];
        OrgChips.Clear();
        OrgChips.Add(OrgChoices[0]);
        foreach (var o in OrgChoices.Skip(1)) if (o.Count > 0 && d.OrgDepth(o.Code) <= 1) OrgChips.Add(o);
        Filter();
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

    private void RefreshStatus()
    {
        foreach (var r in _all)
        {
            var d = _s.Dialogs.Where(x => x.WatchedNumber == r.Number).OrderByDescending(x => x.IsConfirmed).FirstOrDefault();
            r.Status = d is null ? "" : d.IsConfirmed ? "통화중" : d.IsEarly ? "링잉" : "";
        }
    }

    /// <summary>[주소록] 조직 칩 — 그 조직과 하위만(전체 = 빈 코드).</summary>
    [RelayCommand] private void SetOrg(OrgChoice o) => OrgScope = o;
    [RelayCommand] private void Dial() { string n = Number.Trim(); if (n.Length == 0) return; if (Resolve(n) is { } t && _s.Dial(t).Ok) Number = ""; }
    [RelayCommand] private void Pickup() => _s.Pickup();
    [RelayCommand]
    private void Pad(string key)
    {
        if (PadIsDtmf)
        {
            var c = _s.ActiveVolteCall;
            if (c is not null && _s.Dtmf(c, key).Ok) { _dtmfSent = (_dtmfSent + key).Length > 16 ? key : _dtmfSent + key; OnPropertyChanged(nameof(PadDisplay)); }
            return;
        }
        Number += key;
    }
    [RelayCommand] private void Backspace() { if (Number.Length > 0) Number = Number[..^1]; }
    public void ClearSuggestions() { Suggestions.Clear(); OnPropertyChanged(nameof(HasSuggestions)); }
    [RelayCommand] private void Clear() => Number = "";
    [RelayCommand] private void Call(BookRow r) => _s.Dial(r.Number);
    /// <summary>제안 행 클릭 — 필드에 채움(발신은 Resolve 가 원본 번호로).</summary>
    [RelayCommand] private void Pick(BookRow r) { Number = r.DisplayNumber; Suggestions.Clear(); OnPropertyChanged(nameof(HasSuggestions)); }
    [RelayCommand] private void CallSuggestion(BookRow r) { if (_s.Dial(r.Number).Ok) Number = ""; }
    [RelayCommand] private void Sms(BookRow r) { if (r.CanSms) SmsRequested?.Invoke(this, r.Number); }
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
