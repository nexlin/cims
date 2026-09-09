// 사람 메뉴 + 통합 검색 Ctrl+K (§4.1) — 서버 전화번호부(VoLTE·PTT 번호 동시)를 사람 단위로 묶어, 회선별 행동(사설콜·애드혹에 추가·SDS / 통화·문자)을 한 곳에서.
// 사람 행 = 이름 · 소속 · PTT 상태(로스터 파생) · 내선 상태(dialog) + 행동 버튼. 그룹 행 = [채널로][멤버 추가]. 행동은 이벤트로 MainViewModel 이 잇는다.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class PersonEntry : ObservableObject
{
    public string Name { get; }
    public string PttNumber { get; }
    public string Extension { get; }
    public string OrgPath { get; }
    public string DisplayExtension { get; }
    public string DisplayPtt { get; }
    [ObservableProperty] private string _pttStatus = "";
    [ObservableProperty] private string _lineStatus = "";
    public PersonEntry(string name, string ptt, string ext, string orgPath, string dispExt, string dispPtt) { Name = name; PttNumber = ptt; Extension = ext; OrgPath = orgPath; DisplayExtension = dispExt; DisplayPtt = dispPtt; }
    public bool HasPtt => PttNumber.Length > 0;
    public bool HasLine => Extension.Length > 0;
    public string Initial => Name.Length > 0 ? Name[..1] : "?";
    /// <summary>메뉴 머리 — 이름 · PTT 번호 · 내선.</summary>
    public string Head => string.Join(" · ", new[] { Name, HasPtt ? "PTT " + DisplayPtt : "", HasLine ? "내선 " + DisplayExtension : "" }.Where(x => x.Length > 0));
    public string Sub => string.Join(" · ", new[] { OrgPath, PttStatus, LineStatus }.Where(x => x.Length > 0));
    public void Refresh() => OnPropertyChanged(nameof(Sub));
}

public sealed record GroupEntry(GroupInfo Group)
{
    public string Name => Group.Name;
    public string Meta => $"멤버 {Group.MemberCount}" + (Group.IsMember ? " · 멤버" : " · 청취 범위") + (Group.HasSession ? $" · 진행 중 {Group.ConnectedCount}" : "");
}

public sealed partial class PersonActionsViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly List<PersonEntry> _people = new();

    // ── 통합 검색(Ctrl+K) ──
    [ObservableProperty] private bool _searchOpen;
    [ObservableProperty] private string _query = "";
    [ObservableProperty] private int _selectedIndex;
    public ObservableCollection<object> Results { get; } = new();
    public bool HasResults => Results.Count > 0;

    // ── 사람 메뉴(로스터 칩·⑤ 행·③ 그룹원 칩·주소록 행) ──
    [ObservableProperty] private bool _menuOpen;
    [ObservableProperty] private PersonEntry? _menuEntry;

    public event EventHandler<string>? PrivateCallRequested;
    public event EventHandler<string>? AdhocAddRequested;
    public event EventHandler<string>? SdsRequested;
    public event EventHandler<string>? CallRequested;
    public event EventHandler<string>? SmsRequested;
    public event EventHandler<string>? ChannelRequested;
    public event EventHandler<GroupInfo>? AddMemberRequested;

    public PersonActionsViewModel(DispatchSession s)
    {
        _s = s;
        s.Directory.Changed += (_, _) => Reload();
        s.RosterChanged += (_, _) => RefreshStatus();
        s.DialogChanged += (_, _) => RefreshStatus();
        s.DialogEnded += (_, _) => RefreshStatus();
        s.SessionAdded += (_, _) => RefreshStatus();
        s.SessionEnded += (_, _) => RefreshStatus();
        Reload();
    }

    /// <summary>같은 사람의 PTT 번호·내선을 한 항목으로 — 키 = 이름+조직(서버 전화번호부는 가입자 하나에 회선 종류별 번호를 준다).</summary>
    private void Reload()
    {
        _people.Clear();
        var d = _s.Directory;
        var byKey = new Dictionary<string, PersonEntry>(StringComparer.OrdinalIgnoreCase);
        foreach (var c in d.Contacts.Where(c => c.Kind is ContactKind.Extension or ContactKind.PttUser))
        {
            if (DirectoryService.Normalize(c.Number) == DirectoryService.Normalize(_s.MyExtension) || DirectoryService.Normalize(c.Number) == DirectoryService.Normalize(_s.MyPttNumber)) continue;
            string key = c.Name.Length > 0 ? c.Name + "|" + c.OrgCode : c.Number;
            string ptt = c.Kind == ContactKind.PttUser ? c.Number : "", ext = c.Kind == ContactKind.Extension ? c.Number : "";
            if (byKey.TryGetValue(key, out var e))
            {
                if (e.HasPtt && e.HasLine) { key = c.Number; }         // 동명이인·회선 셋 이상은 따로
                else { var merged = new PersonEntry(e.Name, e.PttNumber.Length > 0 ? e.PttNumber : ptt, e.Extension.Length > 0 ? e.Extension : ext, e.OrgPath, e.DisplayExtension.Length > 0 ? e.DisplayExtension : d.DisplayNumber(ext), e.DisplayPtt.Length > 0 ? e.DisplayPtt : d.DisplayNumber(ptt)); byKey[key] = merged; continue; }
            }
            byKey[key] = new PersonEntry(c.Name.Length > 0 ? c.Name : d.DisplayNumber(c.Number), ptt, ext, d.OrgPath(c.OrgCode), d.DisplayNumber(ext), d.DisplayNumber(ptt));
        }
        _people.AddRange(byKey.Values.OrderBy(p => p.Name, StringComparer.CurrentCulture));
        RefreshStatus();
        Filter();
    }

    private void RefreshStatus()
    {
        foreach (var p in _people)
        {
            string ptt = "";
            if (p.HasPtt)
            {
                string norm = DirectoryService.Normalize(p.PttNumber);
                foreach (var g in _s.Groups)
                {
                    if (!g.Roster.Any(r => DirectoryService.Normalize(UserPartConverter.UserPart(r.Uri)) == norm)) continue;
                    var sess = _s.SessionOfGroup(g.Id) ?? _s.ListenOfGroup(g.Id);
                    ptt = sess is not null && sess.Speaker == p.Name ? $"{g.Name} 발언 중" : $"{g.Name} 참여";
                    break;
                }
            }
            string line = "";
            if (p.HasLine)
            {
                var dlg = _s.Dialogs.Where(x => x.WatchedNumber == p.Extension && !x.IsTerminated).OrderByDescending(x => x.IsConfirmed).FirstOrDefault();
                line = dlg is null ? "" : dlg.IsConfirmed ? "통화 중" : dlg.IsEarly ? "링잉" : "";
            }
            p.PttStatus = ptt; p.LineStatus = line; p.Refresh();
        }
    }

    partial void OnQueryChanged(string value) => Filter();
    partial void OnSearchOpenChanged(bool value) { if (value) { Query = ""; Filter(); } }

    private void Filter()
    {
        Results.Clear();
        string q = Query.Trim();
        string qn = DirectoryService.Normalize(q);
        IEnumerable<PersonEntry> people = _people;
        if (q.Length > 0)
            people = people.Where(p => p.Name.Contains(q, StringComparison.OrdinalIgnoreCase)
                                    || (qn.Length > 0 && (DirectoryService.Normalize(p.Extension).Contains(qn) || DirectoryService.Normalize(p.PttNumber).Contains(qn) || DirectoryService.Normalize(p.DisplayExtension).Contains(qn))));
        foreach (var p in people.Take(12)) Results.Add(p);
        foreach (var g in _s.Groups.Where(g => q.Length == 0 || g.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || g.Id.Contains(q, StringComparison.OrdinalIgnoreCase)).Take(6)) Results.Add(new GroupEntry(g));
        SelectedIndex = Results.Count > 0 ? 0 : -1;
        OnPropertyChanged(nameof(HasResults));
    }

    public PersonEntry? Resolve(string numberOrUri)
    {
        string n = DirectoryService.Normalize(UserPartConverter.UserPart(numberOrUri));
        if (n.Length == 0) return null;
        return _people.FirstOrDefault(p => DirectoryService.Normalize(p.PttNumber) == n || DirectoryService.Normalize(p.Extension) == n)
               ?? new PersonEntry(_s.Directory.Label(numberOrUri), numberOrUri.Contains("tel:") || !_s.Directory.CallBook.Any(c => DirectoryService.Normalize(c.Number) == n) ? UserPartConverter.UserPart(numberOrUri) : "",
                                  _s.Directory.CallBook.Any(c => DirectoryService.Normalize(c.Number) == n) ? UserPartConverter.UserPart(numberOrUri) : "", "", _s.Directory.DisplayNumber(UserPartConverter.UserPart(numberOrUri)), _s.Directory.DisplayNumber(UserPartConverter.UserPart(numberOrUri)));
    }

    /// <summary>사람 메뉴 열기 — 번호/URI 하나로(PTT 번호든 내선이든 같은 사람을 찾는다).</summary>
    public void OpenMenu(string numberOrUri) { MenuEntry = Resolve(numberOrUri); MenuOpen = MenuEntry is not null; }
    [RelayCommand] private void OpenMenuFor(string numberOrUri) => OpenMenu(numberOrUri);
    [RelayCommand] private void CloseMenu() => MenuOpen = false;

    // ── 행동 ──
    [RelayCommand] private void PrivateCall(PersonEntry p) { if (p.HasPtt) PrivateCallRequested?.Invoke(this, p.PttNumber); Close(); }
    [RelayCommand] private void AddToAdhoc(PersonEntry p) { if (p.HasPtt) AdhocAddRequested?.Invoke(this, p.PttNumber); Close(); }
    [RelayCommand] private void Sds(PersonEntry p) { if (p.HasPtt) SdsRequested?.Invoke(this, p.PttNumber); Close(); }
    [RelayCommand] private void Call(PersonEntry p) { if (p.HasLine) CallRequested?.Invoke(this, p.Extension); Close(); }
    [RelayCommand] private void Sms(PersonEntry p) { if (p.HasLine) SmsRequested?.Invoke(this, p.Extension); Close(); }
    [RelayCommand] private void Channel(GroupEntry g) { ChannelRequested?.Invoke(this, g.Group.Id); Close(); }
    [RelayCommand] private void AddMember(GroupEntry g) { AddMemberRequested?.Invoke(this, g.Group); Close(); }
    private void Close() { MenuOpen = false; SearchOpen = false; }

    /// <summary>Ctrl+K 목록 — ↑↓ 이동, Enter = 첫 행동(사람: 사설콜 있으면 사설콜, 없으면 통화 / 그룹: 채널로).</summary>
    public void Move(int delta) { if (Results.Count == 0) return; SelectedIndex = Math.Clamp(SelectedIndex + delta, 0, Results.Count - 1); }
    public void Enter()
    {
        if (SelectedIndex < 0 || SelectedIndex >= Results.Count) return;
        switch (Results[SelectedIndex])
        {
            case PersonEntry p when p.HasPtt: PrivateCall(p); break;
            case PersonEntry p when p.HasLine: Call(p); break;
            case GroupEntry g: Channel(g); break;
        }
    }
}
