// [무전] 사용자 패널(§4.5) — 탭 줄 [사용자]·내 채널 [+ 개별 · 애드혹 열기] 로 여는 오른쪽 패널. 사람을 골라(☐) 애드혹 그룹 통화를 한 번 열거나
// [그룹으로 저장 ›](새 PTT 그룹 — GMS XCAP), 고른 사람들에게 애드혹 일제 통화(한 버튼). 줄 ⋮ = 사람 메뉴(개별 통화·무전 메시지·통화·문자).
// 사용자 목록 = 서버 회사 전화번호부(service=ptt, 조직 범위) + CSV ptt 항목. 줄 상태 = 어느 채널에 참여/발언 중인지(로스터 파생).
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
    public PttUserRow(Contact c, string displayNumber, string orgPath, string orgName) { Contact = c; DisplayNumber = displayNumber; OrgPath = orgPath; OrgName = orgName; }
    public string Name => Contact.Name.Length > 0 ? Contact.Name : DisplayNumber;
    public string Number => Contact.Number;
    public string DisplayNumber { get; }
    public string OrgPath { get; }
    public string OrgName { get; }
    public string Initial => Name.Length > 0 ? Name[..1] : "?";
    /// <summary>줄 둘째 줄 — "PTT 1001 · 순찰대".</summary>
    public string Meta => $"PTT {DisplayNumber}" + (OrgName.Length > 0 ? " · " + OrgName : "");
    public bool IsBusy => Status.Length > 0;
    partial void OnStatusChanged(string value) => OnPropertyChanged(nameof(IsBusy));
}

public sealed record OrgChip(string Code, string Label)
{
    public bool IsAll => Code.Length == 0;
}

public sealed partial class PttUsersViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly List<PttUserRow> _allUsers = new();

    [ObservableProperty] private bool _emergency;
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private OrgChip? _org;
    /// <summary>조직 칩 — 전체 + 위 두 단(깊은 조직은 검색으로).</summary>
    public ObservableCollection<OrgChip> Orgs { get; } = new();
    public ObservableCollection<PttUserRow> Users { get; } = new();
    public ObservableCollection<PttUserRow> Picked { get; } = new();

    /// <summary>[그룹으로 저장 ›] — 고른 사람으로 새 PTT 그룹 폼(패널 «새 PTT 그룹»).</summary>
    public event EventHandler<IReadOnlyList<PttUserRow>>? SaveAsGroupRequested;
    /// <summary>줄 ⋮ → 사람 메뉴.</summary>
    public event EventHandler<string>? MenuRequested;
    /// <summary>애드혹 그룹 통화·일제 통화를 열었다 — 호스트가 패널을 닫는다(고정했으면 남긴다).</summary>
    public event EventHandler? Started;

    public PttUsersViewModel(DispatchSession s)
    {
        _s = s;
        s.Directory.Changed += (_, _) => Reload();
        s.RosterChanged += (_, _) => RefreshStatus();
        s.SessionAdded += (_, _) => RefreshStatus();
        s.SessionEnded += (_, _) => RefreshStatus();
        s.ProfileApplied += (_, _) => OnPropertyChanged(nameof(CanCreateGroups));
        s.SessionEnded += (_, item) => { if (IsBroadcastHeld && item.CallId == _bcCall) { IsBroadcastHeld = false; _bcCall = -1; } };   // 서버·코어가 먼저 끝냈다
        s.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(DispatchSession.CanCreateGroups)) OnPropertyChanged(nameof(CanCreateGroups)); };
        Reload();
    }

    /// <summary>[그룹으로 저장] 노출 — 프로비저닝 `ptt.allowCreateGroup`(관리 범위 포함).</summary>
    public bool CanCreateGroups => _s.CanCreateGroups;
    public int TotalCount => _allUsers.Count;
    public string PickedText => Picked.Count > 0 ? $"{Picked.Count}명 선택" : "사람을 고르세요";
    public bool HasPicked => Picked.Count > 0;
    public bool CanStart => Picked.Count > 0 && !IsBroadcastHeld;
    public bool CanSaveGroup => Picked.Count > 0 && CanCreateGroups && !IsBroadcastHeld;

    partial void OnSearchChanged(string value) => Filter();
    partial void OnOrgChanged(OrgChip? value) => Filter();
    [RelayCommand] private void SetOrg(OrgChip o) => Org = o;

    private void Reload()
    {
        var d = _s.Directory;
        _allUsers.Clear();
        foreach (var c in d.PttUsers.Where(c => DirectoryService.Normalize(c.Number) != DirectoryService.Normalize(_s.MyPttNumber)))
            _allUsers.Add(new PttUserRow(c, d.DisplayNumber(c.Number), d.OrgPath(c.OrgCode), c.OrgCode.Length > 0 ? d.OrgName(c.OrgCode) : ""));
        string? keep = Org?.Code;
        Orgs.Clear();
        Orgs.Add(new OrgChip("", "전체"));
        foreach (var (code, label, count) in d.OrgTree(ContactKind.PttUser))
            if (count > 0 && d.OrgDepth(code) <= 1) Orgs.Add(new OrgChip(code, label.Trim()));
        Org = Orgs.FirstOrDefault(o => o.Code == keep) ?? Orgs[0];
        // 골라 둔 사람은 새 목록의 같은 번호 줄로 옮긴다
        var pickedNumbers = Picked.Select(p => DirectoryService.Normalize(p.Number)).ToHashSet();
        Picked.Clear();
        foreach (var u in _allUsers.Where(u => pickedNumbers.Contains(DirectoryService.Normalize(u.Number)))) { u.Checked = true; Picked.Add(u); }
        Filter();
        RefreshStatus();
        RaisePicked();
        OnPropertyChanged(nameof(TotalCount));
    }

    private void Filter()
    {
        string q = Search.Trim();
        string qn = DirectoryService.Normalize(q);
        var scope = _s.Directory.OrgScope(Org?.Code ?? "");
        IEnumerable<PttUserRow> rows = _allUsers;
        if (q.Length > 0) rows = rows.Where(u => u.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || u.OrgPath.Contains(q, StringComparison.OrdinalIgnoreCase)
                                             || (qn.Length > 0 && (DirectoryService.Normalize(u.Number).Contains(qn) || DirectoryService.Normalize(u.DisplayNumber).Contains(qn))));
        else if (scope is not null) rows = rows.Where(u => scope.Contains(u.Contact.OrgCode));
        var order = _s.Directory.OrgTree(ContactKind.PttUser).Select((o, i) => (o.Code, i)).ToDictionary(x => x.Code, x => x.i);
        Users.Clear();
        foreach (var u in rows.OrderBy(u => order.TryGetValue(u.Contact.OrgCode, out int i) ? i : int.MaxValue).ThenBy(u => u.Name, StringComparer.CurrentCulture)) Users.Add(u);
    }

    /// <summary>줄 상태 — 어느 채널에 참여/발언 중인지(로스터·세션에서 파생).</summary>
    private void RefreshStatus()
    {
        foreach (var u in _allUsers)
        {
            string st = "";
            foreach (var g in _s.Groups)
            {
                var e = g.Roster.FirstOrDefault(r => DirectoryService.Normalize(Converters.UserPartConverter.UserPart(r.Uri)) == DirectoryService.Normalize(u.Number));
                if (e is null) continue;
                var sess = _s.SessionOfGroup(g.Id) ?? _s.ListenOfGroup(g.Id);
                st = sess is not null && sess.Speaker == u.Name ? $"{g.Name} 발언" : $"{g.Name} 참여";
                break;
            }
            u.Status = st;
        }
    }

    private void RaisePicked()
    {
        foreach (var p in new[] { nameof(PickedText), nameof(HasPicked), nameof(CanStart), nameof(CanSaveGroup), nameof(CanPressBroadcast) }) OnPropertyChanged(p);
    }

    /// <summary>줄을 누름 — 고르기 토글(일제 통화 중엔 대상이 고정이다).</summary>
    [RelayCommand]
    private void Toggle(PttUserRow u)
    {
        if (IsBroadcastHeld) return;
        if (Picked.Contains(u)) { Picked.Remove(u); u.Checked = false; }
        else { Picked.Add(u); u.Checked = true; }
        RaisePicked();
    }
    [RelayCommand] private void ClearPicked()
    {
        if (IsBroadcastHeld) return;
        foreach (var u in Picked.ToList()) u.Checked = false;
        Picked.Clear(); RaisePicked();
    }
    [RelayCommand] private void Menu(PttUserRow u) => MenuRequested?.Invoke(this, u.Number);

    /// <summary>[애드혹 열기 · 지금 한 번] — 고른 사람으로 애드혹 그룹 통화(TS 24.379 ad hoc group call). 저장하지 않는 한 번짜리.</summary>
    [RelayCommand]
    private void StartAdhoc()
    {
        if (!CanStart) return;
        var r = _s.StartAdhoc(Picked.Select(u => u.Number).ToList(), Emergency);
        if (!r.Ok) return;
        foreach (var u in Picked.ToList()) u.Checked = false;
        Picked.Clear(); Emergency = false; RaisePicked();
        Started?.Invoke(this, EventArgs.Empty);
    }
    [RelayCommand] private void SaveAsGroup() { if (CanSaveGroup) SaveAsGroupRequested?.Invoke(this, Picked.ToList()); }

    /// <summary>개별 통화 발신 — 사람 메뉴 [개별 통화](반이중) 경로. 반이중/전이중은 누른 조작이 정한다.</summary>
    public void PrivateCallTo(string number, bool fullDuplex = false) => _s.StartPrivateCall(number, fullDuplex, emergency: false);

    /// <summary>사람 메뉴 [애드혹에 추가] — 번호로 줄을 찾아 고른다(없으면 안내).</summary>
    public void AddAdhoc(string number)
    {
        string n = DirectoryService.Normalize(number);
        var u = _allUsers.FirstOrDefault(x => DirectoryService.Normalize(x.Number) == n);
        if (u is null) { _s.Notify.Warn("PTT 주소록에 없는 번호", number); return; }
        if (!Picked.Contains(u)) { Picked.Add(u); u.Checked = true; RaisePicked(); }
    }

    // ── 애드혹 일제 통화 한 버튼(TS 24.379 §17.2.2.1.1 9)) — 그룹 카드·채널 상세 [일제 통화] 와 같은 규칙: 누르는 동안 개시+발언, 놓으면 끝
    //   (성립 전 CANCEL / 뒤 Floor Release, 잠금 발언 = 클릭 토글). 누르는 동안은 고른 사람이 잠긴다.
    [ObservableProperty] private bool _isBroadcastHeld;
    private int _bcCall = -1;
    public bool CanPressBroadcast => Picked.Count > 0 || IsBroadcastHeld;
    public string BroadcastText => IsBroadcastHeld ? "일제 통화 중" : "일제 통화";
    public string BroadcastTip => IsBroadcastHeld ? (_s.Settings.Current.LockTalk ? "일제 통화 중 — 다시 누르면 끝납니다" : "일제 통화 중 — 놓으면 끝납니다")
                                : _s.Settings.Current.LockTalk ? "고른 사람들에게 일제 통화 — 누르면 개시하고 바로 말합니다(나만 발언). 다시 누르면 끝납니다"
                                : "고른 사람들에게 일제 통화 — 누르고 있는 동안 개시하고 말합니다(나만 발언). 놓으면 끝납니다";
    partial void OnIsBroadcastHeldChanged(bool value) { OnPropertyChanged(nameof(BroadcastText)); OnPropertyChanged(nameof(BroadcastTip)); RaisePicked(); }

    [RelayCommand]
    public void BroadcastDown()
    {
        if (IsBroadcastHeld) { if (_s.Settings.Current.LockTalk) BroadcastEnd(); return; }
        if (Picked.Count == 0) return;
        int id = _s.StartAdhocBroadcast(Picked.Select(u => u.Number).ToList());
        if (id < 0) return;
        _bcCall = id;
        IsBroadcastHeld = true;
    }
    [RelayCommand]
    public void BroadcastUp() { if (!_s.Settings.Current.LockTalk) BroadcastEnd(); }
    private void BroadcastEnd()
    {
        if (!IsBroadcastHeld) return;
        int id = _bcCall;
        IsBroadcastHeld = false; _bcCall = -1;
        _s.ReleaseBroadcast(id);
        foreach (var u in Picked.ToList()) u.Checked = false;
        Picked.Clear(); Emergency = false; RaisePicked();
        Started?.Invoke(this, EventArgs.Empty);
    }
}
