// ① 오른쪽 위 — PTT 발신 [사설콜|애드혹] + PTT 주소록 [사용자|그룹] (§4.1).
// 사용자 목록 = 서버 회사 전화번호부(service=ptt, 조직 범위·섹션 — Android PTT 연락처 탭과 같은 동선) + CSV ptt 항목.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class PttUserRow : ObservableObject
{
    public Contact Contact { get; }
    [ObservableProperty] private bool _checked;
    [ObservableProperty] private string _status = "";
    public PttUserRow(Contact c, string displayNumber, string orgPath) { Contact = c; DisplayNumber = displayNumber; OrgPath = orgPath; }
    public string Name => Contact.Name.Length > 0 ? Contact.Name : DisplayNumber;
    public string Number => Contact.Number;
    public string DisplayNumber { get; }
    public string OrgPath { get; }
    public string Initial => Name.Length > 0 ? Name[..1] : "?";
}

public sealed partial class PttOriginateViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly List<PttUserRow> _allUsers = new();

    /// <summary>private | adhoc</summary>
    [ObservableProperty] private string _mode = "private";
    /// <summary>users | groups</summary>
    [ObservableProperty] private string _book = "users";
    [ObservableProperty] private string _target = "";
    [ObservableProperty] private bool _fullDuplex;
    [ObservableProperty] private bool _emergency;
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private OrgChoice? _orgScope;
    public ObservableCollection<OrgChoice> OrgChoices { get; } = new();
    public ObservableCollection<PttUserRow> Users { get; } = new();
    public ObservableCollection<PttUserRow> AdhocSelection { get; } = new();
    public ObservableCollection<GroupInfo> Groups { get; } = new();
    /// <summary>사설콜 대상 입력과 일치하는 PTT 사용자(이름·번호 부분 일치, 최대 8).</summary>
    public ObservableCollection<PttUserRow> Suggestions { get; } = new();
    public bool HasSuggestions => Suggestions.Count > 0;

    public event EventHandler<GroupInfo>? MessageGroupRequested;
    public event EventHandler<string>? MessageUserRequested;
    public event EventHandler<GroupInfo>? AddChannelRequested;
    /// <summary>그룹 생성/편집/삭제 — 창·확인은 MainWindow 몫(GMS XCAP, 본인 소유만).</summary>
    public event EventHandler? NewGroupRequested;
    public event EventHandler<GroupInfo>? EditGroupRequested;
    public event EventHandler<GroupInfo>? DeleteGroupRequested;

    public PttOriginateViewModel(DispatchSession s)
    {
        _s = s;
        s.Directory.Changed += (_, _) => Reload();
        s.Groups.CollectionChanged += (_, _) => ReloadGroups();
        s.RosterChanged += (_, _) => RefreshStatus();
        s.SessionAdded += (_, _) => RefreshStatus();
        s.SessionEnded += (_, _) => RefreshStatus();
        s.ProfileApplied += (_, _) => OnPropertyChanged(nameof(CanCreateGroups));
        s.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(DispatchSession.CanCreateGroups)) OnPropertyChanged(nameof(CanCreateGroups)); };
        Reload();
    }

    /// <summary>[새 그룹] 노출 — 프로비저닝 `ptt.allowCreateGroup`.</summary>
    public bool CanCreateGroups => _s.CanCreateGroups;
    [ObservableProperty] private bool _refreshingGroups;

    public bool IsPrivate => Mode == "private";
    public bool IsAdhoc => Mode == "adhoc";
    public bool IsUsers => Book == "users";
    public bool IsGroups => Book == "groups";
    public bool IsPadBook => Book == "pad";
    public string StartText => IsPrivate ? "사설콜 발신" : $"애드혹 발신 ({AdhocSelection.Count}명)";
    public bool CanStart => IsPrivate ? Target.Trim().Length > 0 : AdhocSelection.Count > 0;
    public string UserCount => $"{Users.Count}명";

    partial void OnModeChanged(string value) { OnPropertyChanged(nameof(IsPrivate)); OnPropertyChanged(nameof(IsAdhoc)); OnPropertyChanged(nameof(StartText)); OnPropertyChanged(nameof(CanStart)); }
    partial void OnBookChanged(string value) { OnPropertyChanged(nameof(IsUsers)); OnPropertyChanged(nameof(IsGroups)); OnPropertyChanged(nameof(IsPadBook)); }
    partial void OnTargetChanged(string value) { OnPropertyChanged(nameof(CanStart)); UpdateSuggestions(); }

    private void UpdateSuggestions()
    {
        Suggestions.Clear();
        string q = Target.Trim();
        if (q.Length > 0 && IsPrivate)
        {
            string qn = DirectoryService.Normalize(q);
            foreach (var u in _allUsers.Where(u => u.Name.Contains(q, StringComparison.OrdinalIgnoreCase)
                                               || (qn.Length > 0 && (DirectoryService.Normalize(u.Number).Contains(qn) || DirectoryService.Normalize(u.DisplayNumber).Contains(qn))))
                                     .Take(8))
                Suggestions.Add(u);
        }
        OnPropertyChanged(nameof(HasSuggestions));
    }
    partial void OnSearchChanged(string value) => Filter();
    partial void OnOrgScopeChanged(OrgChoice? value) => Filter();

    private void Reload()
    {
        var d = _s.Directory;
        _allUsers.Clear();
        foreach (var c in d.PttUsers.Where(c => DirectoryService.Normalize(c.Number) != DirectoryService.Normalize(_s.MyPttNumber)))
            _allUsers.Add(new PttUserRow(c, d.DisplayNumber(c.Number), d.OrgPath(c.OrgCode)));
        string? keep = OrgScope?.Code;
        OrgChoices.Clear();
        OrgChoices.Add(new OrgChoice("", "전체 조직", _allUsers.Count));
        foreach (var (code, label, count) in d.OrgTree(ContactKind.PttUser)) OrgChoices.Add(new OrgChoice(code, label, count));
        OrgScope = OrgChoices.FirstOrDefault(o => o.Code == keep) ?? OrgChoices[0];
        Filter();
        ReloadGroups();
        RefreshStatus();
    }

    private void ReloadGroups()
    {
        Groups.Clear();
        foreach (var g in _s.Groups.Where(g => g.IsMember)) Groups.Add(g);
    }

    private void Filter()
    {
        string q = Search.Trim();
        var scope = _s.Directory.OrgScope(OrgScope?.Code ?? "");
        IEnumerable<PttUserRow> rows = _allUsers;
        if (q.Length > 0) rows = rows.Where(u => u.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || u.Number.Contains(q, StringComparison.OrdinalIgnoreCase) || u.DisplayNumber.Contains(q, StringComparison.OrdinalIgnoreCase));
        else if (scope is not null) rows = rows.Where(u => scope.Contains(u.Contact.OrgCode));
        var order = _s.Directory.OrgTree(ContactKind.PttUser).Select((o, i) => (o.Code, i)).ToDictionary(x => x.Code, x => x.i);
        Users.Clear();
        foreach (var u in rows.OrderBy(u => order.TryGetValue(u.Contact.OrgCode, out int i) ? i : int.MaxValue).ThenBy(u => u.Name, StringComparer.CurrentCulture)) Users.Add(u);
        OnPropertyChanged(nameof(UserCount));
    }

    /// <summary>사용자 현재 상태 — 어느 채널에 참여/발언 중인지(로스터·세션에서 파생).</summary>
    private void RefreshStatus()
    {
        foreach (var u in _allUsers)
        {
            string st = "";
            foreach (var g in _s.Groups)
            {
                var e = g.Roster.FirstOrDefault(r => DirectoryService.Normalize(Converters.UserPartConverter.UserPart(r.Uri)) == DirectoryService.Normalize(u.Number));
                if (e is null) continue;
                var sess = _s.SessionOfGroup(g.Id);
                st = sess is not null && sess.Speaker == u.Name ? $"{g.Name} 발언" : $"{g.Name} 참여";
                break;
            }
            u.Status = st;
        }
    }

    [RelayCommand] private void SetMode(string mode) => Mode = mode;
    [RelayCommand] private void SetBook(string book) => Book = book;

    [RelayCommand]
    private void Start()
    {
        if (IsPrivate)
        {
            string t = Target.Trim();
            var row = _allUsers.FirstOrDefault(u => u.Name.Equals(t, StringComparison.OrdinalIgnoreCase))
                      ?? _allUsers.FirstOrDefault(u => DirectoryService.Normalize(u.DisplayNumber) == DirectoryService.Normalize(t));
            var r = _s.StartPrivateCall(row?.Number ?? t, FullDuplex, Emergency);
            if (r.Ok) { Target = ""; Emergency = false; }
        }
        else
        {
            var r = _s.StartAdhoc(AdhocSelection.Select(u => u.Number).ToList(), Emergency);
            if (r.Ok) { foreach (var u in AdhocSelection.ToList()) u.Checked = false; AdhocSelection.Clear(); Emergency = false; OnPropertyChanged(nameof(StartText)); OnPropertyChanged(nameof(CanStart)); }
        }
    }

    [RelayCommand] private void Pad(string key) { Mode = "private"; Target += key; }
    [RelayCommand] private void Backspace() { if (Target.Length > 0) Target = Target[..^1]; }
    [RelayCommand] private void Clear() => Target = "";
    [RelayCommand] private void Pick(PttUserRow u) { Target = u.DisplayNumber; Suggestions.Clear(); OnPropertyChanged(nameof(HasSuggestions)); }
    [RelayCommand] private void PrivateTo(PttUserRow u) { Mode = "private"; Target = u.Name; Start(); }
    [RelayCommand] private void PrivateFullTo(PttUserRow u) { Mode = "private"; FullDuplex = true; Target = u.Name; Start(); }
    [RelayCommand]
    private void ToggleAdhoc(PttUserRow u)
    {
        Mode = "adhoc";
        if (AdhocSelection.Contains(u)) { AdhocSelection.Remove(u); u.Checked = false; }
        else { AdhocSelection.Add(u); u.Checked = true; }
        OnPropertyChanged(nameof(StartText)); OnPropertyChanged(nameof(CanStart));
    }
    [RelayCommand] private void RemoveAdhoc(PttUserRow u) { if (u.Checked) ToggleAdhoc(u); }
    [RelayCommand] private void MessageUser(PttUserRow u) => MessageUserRequested?.Invoke(this, u.Number);
    [RelayCommand] private void MessageGroup(GroupInfo g) => MessageGroupRequested?.Invoke(this, g);
    [RelayCommand] private void AddChannel(GroupInfo g) => AddChannelRequested?.Invoke(this, g);

    // 그룹 관리(§4.1 [그룹] 탭) — 창·삭제 확인은 MainWindow
    [RelayCommand] private void NewGroup() => NewGroupRequested?.Invoke(this, EventArgs.Empty);
    [RelayCommand] private void EditGroup(GroupInfo g) => EditGroupRequested?.Invoke(this, g);
    [RelayCommand] private void DeleteGroup(GroupInfo g) => DeleteGroupRequested?.Invoke(this, g);
    [RelayCommand]
    private async Task RefreshGroups()
    {
        if (RefreshingGroups) return;
        RefreshingGroups = true;
        try { await _s.RefreshGroupsAsync(); } finally { RefreshingGroups = false; }
    }
}
