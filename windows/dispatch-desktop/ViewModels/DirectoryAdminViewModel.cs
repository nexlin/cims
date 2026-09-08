// 관리 창 › 조직/구성원 탭(§4.5.1) — 서버가 관리 범위(dispatch.directoryAdmin)로 걸러 준 조직 트리·구성원·VoLTE/PTT 번호를 편집한다.
// 왼쪽 조직 트리(선택 = 하위 포함 필터) · 가운데 구성원 목록 · 오른쪽 편집 폼(구성원 속성 + 회선 둘 + PTT 자격). 쓰기는 전부 서버가
// 판정(범위 밖 403·번호 충돌 409·H(A1) 재결박 400)하고 앱은 사전(ResponseText.Area.Management)으로 문구만 낸다. 저장 뒤 한 벌을 다시 받는다.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed class OrgRow
{
    public string Code { get; }
    public string Name { get; }
    public string Parent { get; }
    public int Depth { get; }
    public int Count { get; set; }
    public OrgRow(string code, string name, string parent, int depth) { Code = code; Name = name; Parent = parent; Depth = depth; }
    public string Label => new string(' ', Depth * 3) + Name;
    public string CountText => Count > 0 ? $"{Count}명" : "";
    public override string ToString() => Name;
}

public sealed class MemberRow
{
    public MemberInfo Info { get; }
    public string OrgPath { get; }
    public MemberRow(MemberInfo info, string orgPath, DirectoryService d)
    {
        Info = info; OrgPath = orgPath;
        VolteText = info.Volte is { } v ? d.DisplayNumber(v.Msisdn) : "";
        PttText = info.Ptt is { } p ? d.DisplayNumber(p.Msisdn) : "";
    }
    public long UserId => Info.UserId;
    public string Name => Info.Name.Length > 0 ? Info.Name : $"#{Info.UserId}";
    public string Title => Info.Title;
    public string VolteText { get; }
    public string PttText { get; }
    public string Flags => Info.Ptt?.Profile is { } pf
        ? string.Join(" ", new[] { pf.GetValueOrDefault("allowCreateGroup") ? "그룹생성" : "", pf.GetValueOrDefault("allowAmbientListening") ? "청취" : "" }.Where(x => x.Length > 0))
        : "";
}

public sealed partial class DirectoryAdminViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private AdminView? _view;
    private string _etag = "";

    public DirectoryAdminViewModel(DispatchSession s) { _s = s; }

    public IReadOnlyList<string> Transports { get; } = new[] { "TLS", "TCP", "UDP" };

    [ObservableProperty] private bool _loaded;
    [ObservableProperty] private bool _busy;
    [ObservableProperty] private string _error = "";
    [ObservableProperty] private string _scopeText = "";
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private OrgRow? _selectedOrg;
    [ObservableProperty] private MemberRow? _selectedMember;

    public ObservableCollection<OrgRow> Orgs { get; } = new();
    public ObservableCollection<MemberRow> Members { get; } = new();
    public ObservableCollection<ServiceRef> VolteServices { get; } = new();
    public ObservableCollection<ServiceRef> PttServices { get; } = new();

    // ── 조직 폼 ──
    [ObservableProperty] private bool _orgEditing;
    [ObservableProperty] private bool _orgIsNew;
    [ObservableProperty] private string _orgCode = "";
    [ObservableProperty] private string _orgName = "";
    [ObservableProperty] private OrgRow? _orgParent;
    [ObservableProperty] private int _orgSort;
    public string OrgFormTitle => OrgIsNew ? "새 조직" : $"조직 편집 — {OrgCode}";

    // ── 구성원 폼 ──
    [ObservableProperty] private bool _memberEditing;
    [ObservableProperty] private bool _memberIsNew;
    [ObservableProperty] private long _editUserId;
    [ObservableProperty] private string _editName = "";
    [ObservableProperty] private OrgRow? _editOrg;
    [ObservableProperty] private string _editTitle = "";
    [ObservableProperty] private string _editLoginId = "";
    [ObservableProperty] private string _editPassword = "";
    [ObservableProperty] private string _volteNumber = "";
    [ObservableProperty] private ServiceRef? _volteService;
    [ObservableProperty] private string _volteTransport = "TLS";
    [ObservableProperty] private string _voltePassword = "";
    [ObservableProperty] private string _pttNumber = "";
    [ObservableProperty] private ServiceRef? _pttService;
    [ObservableProperty] private string _pttTransport = "TLS";
    [ObservableProperty] private string _pttPassword = "";
    [ObservableProperty] private bool _allowCreateGroup;
    [ObservableProperty] private bool _allowAmbientListening;
    private string _origVolte = "", _origPtt = "";
    private bool _origCreate, _origAmbient;
    public string MemberFormTitle => MemberIsNew ? "새 구성원" : $"구성원 편집 — {EditName}";
    public bool HasVolte => _origVolte.Length > 0;
    public bool HasPtt => _origPtt.Length > 0;
    public bool HasError => Error.Length > 0;
    /// <summary>편집 폼이 열려 있다 — [관리] 메뉴의 점 배지(§3.4). 화면을 오가도 폼은 유지된다.</summary>
    public bool IsEditing => OrgEditing || MemberEditing;

    /// <summary>확인 대화상자 — 창이 붙인다(제목, 본문) → 예/아니오.</summary>
    public Func<string, string, bool>? Confirm { get; set; }

    partial void OnErrorChanged(string value) => OnPropertyChanged(nameof(HasError));
    partial void OnOrgIsNewChanged(bool value) => OnPropertyChanged(nameof(OrgFormTitle));
    partial void OnOrgEditingChanged(bool value) => OnPropertyChanged(nameof(IsEditing));
    partial void OnMemberEditingChanged(bool value) => OnPropertyChanged(nameof(IsEditing));
    partial void OnOrgCodeChanged(string value) => OnPropertyChanged(nameof(OrgFormTitle));
    partial void OnMemberIsNewChanged(bool value) => OnPropertyChanged(nameof(MemberFormTitle));
    partial void OnEditNameChanged(string value) => OnPropertyChanged(nameof(MemberFormTitle));
    partial void OnSearchChanged(string value) => Filter();
    partial void OnSelectedOrgChanged(OrgRow? value) => Filter();

    public async Task LoadAsync(bool force = false)
    {
        var m = _s.Management;
        if (m is null) { Error = "로그인 전"; return; }
        Busy = true; Error = "";
        var r = await m.GetAdminViewAsync(force ? null : (_etag.Length > 0 ? _etag : null));
        Busy = false;
        if (!r.Ok) { Error = ResponseText.Describe(ResponseText.Area.Management, r.Code, r.Reason); Loaded = _view is not null; return; }
        if (r.Value is null) return;                                   // 304 — 그대로
        _view = r.Value; _etag = r.Value.ETag;
        Apply(r.Value);
        Loaded = true;
    }

    private void Apply(AdminView v)
    {
        ScopeText = v.Scope.DirectoryAdmin == "all" ? "관리 범위: 전체 조직" : $"관리 범위: {OrgPathOf(v.Orgs, v.Scope.OrgCode)} 하위";
        VolteServices.Clear(); PttServices.Clear();
        foreach (var s in v.Services) (s.Kind == "ptt" ? PttServices : VolteServices).Add(s);
        string keepOrg = SelectedOrg?.Code ?? "";
        Orgs.Clear();
        var byParent = v.Orgs.GroupBy(o => o.Parent).ToDictionary(g => g.Key, g => g.OrderBy(x => x.Sort).ThenBy(x => x.Name).ToList());
        var codes = v.Orgs.Select(o => o.Code).ToHashSet(StringComparer.Ordinal);
        void Walk(string parent, int depth)
        {
            if (!byParent.TryGetValue(parent, out var kids)) return;
            foreach (var o in kids)
            {
                var row = new OrgRow(o.Code, o.Name, o.Parent, depth) { Count = v.Members.Count(m => m.Org == o.Code) };
                Orgs.Add(row);
                Walk(o.Code, depth + 1);
            }
        }
        // 루트 = 부모가 없거나 부모가 범위 밖(own 의 루트)인 조직
        foreach (var root in v.Orgs.Where(o => o.Parent.Length == 0 || !codes.Contains(o.Parent)).OrderBy(o => o.Sort).ThenBy(o => o.Name))
        {
            Orgs.Add(new OrgRow(root.Code, root.Name, root.Parent, 0) { Count = v.Members.Count(m => m.Org == root.Code) });
            Walk(root.Code, 1);
        }
        SelectedOrg = Orgs.FirstOrDefault(o => o.Code == keepOrg);
        Filter();
    }

    private static string OrgPathOf(IReadOnlyList<OrgNode> orgs, string code)
    {
        var parts = new List<string>(); var seen = new HashSet<string>(); string cur = code;
        while (cur.Length > 0 && seen.Add(cur))
        {
            var o = orgs.FirstOrDefault(x => x.Code == cur);
            if (o is null) { parts.Add(cur); break; }
            parts.Add(o.Name); cur = o.Parent;
        }
        parts.Reverse();
        return parts.Count > 0 ? string.Join(" › ", parts) : code;
    }

    private HashSet<string>? Scope(string code)
    {
        if (code.Length == 0) return null;
        var set = new HashSet<string> { code };
        bool grew = true;
        while (grew) { grew = false; foreach (var o in Orgs) if (set.Contains(o.Parent) && set.Add(o.Code)) grew = true; }
        return set;
    }

    private void Filter()
    {
        long keep = SelectedMember?.UserId ?? 0;
        Members.Clear();
        if (_view is null) return;
        var scope = Scope(SelectedOrg?.Code ?? "");
        string q = Search.Trim();
        string qn = DirectoryService.Normalize(q);
        foreach (var m in _view.Members)
        {
            if (scope is not null && !scope.Contains(m.Org)) continue;
            if (q.Length > 0 && !m.Name.Contains(q, StringComparison.OrdinalIgnoreCase) && !m.LoginId.Contains(q, StringComparison.OrdinalIgnoreCase)
                && !(qn.Length > 0 && (DirectoryService.Normalize(m.Volte?.Msisdn ?? "").Contains(qn) || DirectoryService.Normalize(m.Ptt?.Msisdn ?? "").Contains(qn)))) continue;
            Members.Add(new MemberRow(m, OrgPathOf(_view.Orgs, m.Org), _s.Directory));
        }
        SelectedMember = Members.FirstOrDefault(x => x.UserId == keep);
    }

    private async Task<bool> RunAsync(Task<CimsUe.Result<CimsUe.HttpResponse>> op, string what)
    {
        Busy = true; Error = "";
        var r = await op;
        Busy = false;
        if (!r.Ok) { Error = $"{what} 실패 — " + ResponseText.Describe(ResponseText.Area.Management, r.Code, r.Reason); _s.Log.Warn($"mgmt {what}: {r.Code} {r.Reason}"); return false; }
        _s.Notify.Info($"{what} 완료");
        await LoadAsync(force: true);
        _ = _s.SyncDirectoryAsync();                                   // 주소록(전화번호부)도 따라오게
        return true;
    }

    // ── 조직 ──
    [RelayCommand] private void NewOrg()
    {
        OrgIsNew = true; OrgCode = ""; OrgName = ""; OrgSort = 0; OrgParent = SelectedOrg; OrgEditing = true; MemberEditing = false;
    }
    [RelayCommand] private void BeginEditOrg()
    {
        if (SelectedOrg is null) return;
        OrgIsNew = false; OrgCode = SelectedOrg.Code; OrgName = SelectedOrg.Name; OrgParent = Orgs.FirstOrDefault(o => o.Code == SelectedOrg.Parent);
        OrgSort = _view?.Orgs.FirstOrDefault(o => o.Code == SelectedOrg.Code)?.Sort ?? 0; OrgEditing = true; MemberEditing = false;
    }
    [RelayCommand] private void CancelOrg() => OrgEditing = false;
    [RelayCommand] private async Task SaveOrg()
    {
        var m = _s.Management; if (m is null) return;
        if (OrgName.Trim().Length == 0 || (OrgIsNew && OrgCode.Trim().Length == 0)) { Error = "코드와 이름은 필수입니다"; return; }
        bool ok = OrgIsNew
            ? await RunAsync(m.CreateOrgAsync(OrgCode.Trim(), OrgName.Trim(), OrgParent?.Code ?? "", OrgSort), "조직 생성")
            : await RunAsync(m.UpdateOrgAsync(OrgCode, OrgName.Trim(), OrgParent?.Code ?? "", OrgSort), "조직 저장");
        if (ok) { OrgEditing = false; SelectedOrg = Orgs.FirstOrDefault(o => o.Code == OrgCode.Trim()); }
    }
    [RelayCommand] private async Task DeleteOrg()
    {
        var m = _s.Management; if (m is null || SelectedOrg is null) return;
        if (Confirm?.Invoke("조직 삭제", $"조직 '{SelectedOrg.Name}' ({SelectedOrg.Code}) 을 삭제할까요?\n하위 조직·구성원이 남아 있으면 지울 수 없습니다.") != true) return;
        string code = SelectedOrg.Code;
        if (await RunAsync(m.DeleteOrgAsync(code), "조직 삭제")) SelectedOrg = null;
    }

    // ── 구성원 ──
    [RelayCommand] private void NewMember()
    {
        MemberIsNew = true; EditUserId = 0; EditName = ""; EditOrg = SelectedOrg ?? Orgs.FirstOrDefault(); EditTitle = ""; EditLoginId = ""; EditPassword = "";
        VolteNumber = ""; VolteService = VolteServices.FirstOrDefault(); VolteTransport = "TLS"; VoltePassword = "";
        PttNumber = ""; PttService = PttServices.FirstOrDefault(); PttTransport = "TLS"; PttPassword = "";
        AllowCreateGroup = false; AllowAmbientListening = false; _origVolte = _origPtt = ""; _origCreate = _origAmbient = false;
        OnPropertyChanged(nameof(HasVolte)); OnPropertyChanged(nameof(HasPtt));
        MemberEditing = true; OrgEditing = false;
    }
    [RelayCommand] private void EditMember()
    {
        if (SelectedMember is null) return;
        var i = SelectedMember.Info;
        MemberIsNew = false; EditUserId = i.UserId; EditName = i.Name; EditOrg = Orgs.FirstOrDefault(o => o.Code == i.Org); EditTitle = i.Title; EditLoginId = i.LoginId; EditPassword = "";
        VolteNumber = i.Volte?.Msisdn ?? ""; VolteService = VolteServices.FirstOrDefault(s => s.Name == i.Volte?.ServiceRef) ?? VolteServices.FirstOrDefault();
        VolteTransport = i.Volte?.SipTransport is { Length: > 0 } vt ? vt : "TLS"; VoltePassword = "";
        PttNumber = i.Ptt?.Msisdn ?? ""; PttService = PttServices.FirstOrDefault(s => s.Name == i.Ptt?.ServiceRef) ?? PttServices.FirstOrDefault();
        PttTransport = i.Ptt?.SipTransport is { Length: > 0 } pt ? pt : "TLS"; PttPassword = "";
        AllowCreateGroup = i.Ptt?.Profile?.GetValueOrDefault("allowCreateGroup") == true;
        AllowAmbientListening = i.Ptt?.Profile?.GetValueOrDefault("allowAmbientListening") == true;
        _origVolte = VolteNumber; _origPtt = PttNumber; _origCreate = AllowCreateGroup; _origAmbient = AllowAmbientListening;
        OnPropertyChanged(nameof(HasVolte)); OnPropertyChanged(nameof(HasPtt));
        MemberEditing = true; OrgEditing = false;
    }
    [RelayCommand] private void CancelMember() => MemberEditing = false;

    private NumberInput? NumberOf(string number, ServiceRef? svc, string transport, string password) =>
        number.Trim().Length == 0 ? null : new NumberInput { Msisdn = number.Trim(), ServiceRef = svc?.Name ?? "", SipTransport = transport, Password = password };

    [RelayCommand] private async Task SaveMember()
    {
        var m = _s.Management; if (m is null) return;
        if (EditName.Trim().Length == 0 || EditOrg is null) { Error = "이름과 소속 조직은 필수입니다"; return; }
        string volte = VolteNumber.Trim(), ptt = PttNumber.Trim();
        if (MemberIsNew)
        {
            if ((volte.Length > 0 && VoltePassword.Length == 0) || (ptt.Length > 0 && PttPassword.Length == 0)) { Error = "새 회선에는 SIP 비밀번호가 필요합니다(H(A1) 결박)"; return; }
            var input = new MemberInput { Name = EditName.Trim(), Org = EditOrg.Code, Title = EditTitle.Trim(), LoginId = EditLoginId.Trim(), Password = EditPassword,
                                          Volte = NumberOf(volte, VolteService, VolteTransport, VoltePassword), Ptt = NumberOf(ptt, PttService, PttTransport, PttPassword) };
            Busy = true; Error = "";
            var r = await m.CreateMemberAsync(input);
            Busy = false;
            if (!r.Ok) { Error = "구성원 생성 실패 — " + ResponseText.Describe(ResponseText.Area.Management, r.Code, r.Reason); return; }
            if (ptt.Length > 0 && (AllowCreateGroup || AllowAmbientListening))
                await m.PutPttProfileAsync(r.Value, new Dictionary<string, bool> { ["allowCreateGroup"] = AllowCreateGroup, ["allowAmbientListening"] = AllowAmbientListening });
            _s.Notify.Info("구성원 생성 완료");
            await LoadAsync(force: true); _ = _s.SyncDirectoryAsync();
            MemberEditing = false;
            SelectedMember = Members.FirstOrDefault(x => x.UserId == r.Value);
            return;
        }
        long uid = EditUserId;
        var upd = new MemberInput { Name = EditName.Trim(), Org = EditOrg.Code, Title = EditTitle.Trim(), LoginId = EditLoginId.Trim(), Password = EditPassword };
        if (!await RunAsync(m.UpdateMemberAsync(uid, upd), "구성원 저장")) return;
        // 회선 — 번호가 바뀌었거나 비밀번호를 넣었으면 PUT(개설/변경/재결박), 비웠으면 DELETE
        foreach (var (kind, number, orig, svc, tr, pw) in new[] { ("volte", volte, _origVolte, VolteService, VolteTransport, VoltePassword), ("ptt", ptt, _origPtt, PttService, PttTransport, PttPassword) })
        {
            if (number.Length == 0 && orig.Length > 0)
            {
                if (Confirm?.Invoke("회선 삭제", $"{(kind == "volte" ? "VoLTE" : "PTT")} 번호 {orig} 를 삭제할까요? 단말 등록이 끊깁니다.") != true) continue;
                if (!await RunAsync(m.DeleteNumberAsync(uid, kind), $"{kind} 회선 삭제")) return;
            }
            else if (number.Length > 0 && (number != orig || pw.Length > 0 || (svc?.Name ?? "") != OrigService(kind)))
            {
                if ((number != orig) && pw.Length == 0) { Error = $"{(kind == "volte" ? "VoLTE" : "PTT")} 번호를 바꾸려면 SIP 비밀번호가 필요합니다(H(A1) 재결박)"; return; }
                if (!await RunAsync(m.PutNumberAsync(uid, kind, NumberOf(number, svc, tr, pw)!), $"{kind} 회선 {(orig.Length == 0 ? "개설" : "변경")}")) return;
            }
        }
        if (ptt.Length > 0 && (AllowCreateGroup != _origCreate || AllowAmbientListening != _origAmbient))
            if (!await RunAsync(m.PutPttProfileAsync(uid, new Dictionary<string, bool> { ["allowCreateGroup"] = AllowCreateGroup, ["allowAmbientListening"] = AllowAmbientListening }), "PTT 자격 저장")) return;
        MemberEditing = false;
    }

    private string OrigService(string kind)
    {
        var i = _view?.Members.FirstOrDefault(x => x.UserId == EditUserId);
        return (kind == "volte" ? i?.Volte?.ServiceRef : i?.Ptt?.ServiceRef) ?? "";
    }

    [RelayCommand] private async Task DeleteMember()
    {
        var m = _s.Management; if (m is null || SelectedMember is null) return;
        var row = SelectedMember;
        if (Confirm?.Invoke("구성원 삭제", $"'{row.Name}' 을 삭제할까요?\nVoLTE/PTT 회선도 함께 삭제되고 단말 등록이 끊깁니다.") != true) return;
        if (await RunAsync(m.DeleteMemberAsync(row.UserId), "구성원 삭제")) { MemberEditing = false; SelectedMember = null; }
    }

    [RelayCommand] private Task Refresh() => LoadAsync(force: true);
}
