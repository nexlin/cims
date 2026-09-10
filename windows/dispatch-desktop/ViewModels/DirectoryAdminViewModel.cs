// 관리 창 › 조직/구성원 탭(§4.5.1) — 서버가 관리 범위(dispatch.directoryAdmin)로 걸러 준 조직 트리·구성원·VoLTE/VoIP/PTT 번호를 편집한다.
// 왼쪽 조직 트리(선택 = 하위 포함 필터) · 가운데 구성원 목록 · 오른쪽 편집 폼(구성원 속성 + 회선 셋 + PTT 자격). 쓰기는 전부 서버가
// 판정(범위 밖 403·번호 충돌 409·H(A1) 재결박 400)하고 앱은 사전(ResponseText.Area.Management)으로 문구만 낸다. 저장 뒤 한 벌을 다시 받는다.
using System.Collections.ObjectModel;
using System.ComponentModel;
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
        VoipText = info.Voip is { } w ? d.DisplayNumber(w.Msisdn) : "";
        PttText = info.Ptt is { } p ? d.DisplayNumber(p.Msisdn) : "";
    }
    public long UserId => Info.UserId;
    public string Name => Info.Name.Length > 0 ? Info.Name : $"#{Info.UserId}";
    public string Title => Info.Title;
    public string VolteText { get; }
    public string VoipText { get; }
    public string PttText { get; }
    public string Flags => Info.Ptt?.Profile is { } pf
        ? string.Join(" ", new[] { pf.GetValueOrDefault("allowCreateGroup") ? "그룹생성" : "", pf.GetValueOrDefault("allowAmbientListening") ? "청취" : "" }.Where(x => x.Length > 0))
        : "";
    public bool CanCreateGroup => Info.Ptt?.Profile?.GetValueOrDefault("allowCreateGroup") == true;
    public bool CanAmbientListen => Info.Ptt?.Profile?.GetValueOrDefault("allowAmbientListening") == true;
    public string VolteCell => VolteText.Length > 0 ? VolteText : "–";
    public string VoipCell => VoipText.Length > 0 ? VoipText : "–";
    public string PttCell => PttText.Length > 0 ? PttText : "–";
}

public sealed partial class DirectoryAdminViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private AdminView? _view;
    private string _etag = "";

    public DirectoryAdminViewModel(DispatchSession s) { _s = s; }

    /// <summary>SIP transport 콤보 — 콘솔 라벨과 같은 넷. ANY = 가입자 override 없음(서버 NULL)을 양방향 명시값으로 주고받는다(없으면 NULL 로 되돌릴 길이 없다).</summary>
    public IReadOnlyList<string> Transports { get; } = SipTransports.All;

    [ObservableProperty] private bool _loaded;
    [ObservableProperty] private bool _busy;
    [ObservableProperty] private string _error = "";
    [ObservableProperty] private string _scopeText = "";
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private OrgRow? _selectedOrg;
    [ObservableProperty] private MemberRow? _selectedMember;

    public ObservableCollection<OrgRow> Orgs { get; } = new();
    public ObservableCollection<MemberRow> Members { get; } = new();
    // 접속서비스 후보 — 회선 종류별 버킷(콘솔 매트릭스와 같다: VoLTE 카드는 volte 후보만, VoIP 카드는 voip 후보만)
    public ObservableCollection<ServiceRef> VolteServices { get; } = new();
    public ObservableCollection<ServiceRef> VoipServices { get; } = new();
    public ObservableCollection<ServiceRef> PttServices { get; } = new();
    private ObservableCollection<ServiceRef> ServicesOf(string kind) => kind switch { LineKind.Voip => VoipServices, LineKind.Ptt => PttServices, _ => VolteServices };

    // ── 조직 폼 ──
    [ObservableProperty] private bool _orgEditing;
    [ObservableProperty] private bool _orgIsNew;
    [ObservableProperty] private string _orgCode = "";
    [ObservableProperty] private string _orgName = "";
    [ObservableProperty] private OrgRow? _orgParent;
    [ObservableProperty] private int _orgSort;
    public string OrgFormTitle => OrgIsNew ? "새 조직" : $"조직 편집 — {OrgName}";

    // ── 구성원 폼 ──
    [ObservableProperty] private bool _memberEditing;
    [ObservableProperty] private bool _memberIsNew;
    [ObservableProperty] private long _editUserId;
    [ObservableProperty] private string _editName = "";
    [ObservableProperty] private OrgRow? _editOrg;
    [ObservableProperty] private string _editTitle = "";
    [ObservableProperty] private string _editLoginId = "";
    [ObservableProperty] private string _editPassword = "";
    // 회선 카드 셋 — VoLTE(이동) · VoIP(유선) · PTT. 서버가 종류별 첫 회선을 갈라 내리므로 이동·유선을 둘 다 가진 관제사도 둘 다 관리된다.
    [ObservableProperty] private string _volteNumber = "";
    [ObservableProperty] private ServiceRef? _volteService;
    [ObservableProperty] private string _volteTransport = "TLS";
    [ObservableProperty] private string _voltePassword = "";
    [ObservableProperty] private string _voipNumber = "";
    [ObservableProperty] private ServiceRef? _voipService;
    [ObservableProperty] private string _voipTransport = "TLS";
    [ObservableProperty] private string _voipPassword = "";
    [ObservableProperty] private string _voipExtension = "";          // 읽기전용 표시 — 내선 라벨(서버 파생)
    [ObservableProperty] private string _voipPickupGroup = "";        // 읽기전용 표시 — 픽업 그룹(전화 그룹 id 파생, 콘솔 전화 그룹에서 편성)
    [ObservableProperty] private string _pttNumber = "";
    [ObservableProperty] private ServiceRef? _pttService;
    [ObservableProperty] private string _pttTransport = "TLS";
    [ObservableProperty] private string _pttPassword = "";
    [ObservableProperty] private bool _allowCreateGroup;
    [ObservableProperty] private bool _allowAmbientListening;
    private string _origVolte = "", _origVoip = "", _origPtt = "";
    private string _origVolteImsi = "", _origVoipImsi = "", _origPttImsi = "";   // 저장된 IMSI — 같은 번호로 PUT 할 때 그대로 실어 서버의 H(A1) 재결박 오판을 막는다
    private bool _origCreate;                                          // allowAmbientListening 은 편집 불가(역할 배정의 결과) — 원본 추적 없음
    /// <summary>서버가 접속서비스 후보를 하나도 내려주지 않았다 — 회선 개설이 400 으로 실패하므로 폼에 경고한다(서버 csc.json Provisioning.Services / access_services 미러).</summary>
    public bool NoVolteServices => VolteServices.Count == 0;
    public bool NoVoipServices => VoipServices.Count == 0;
    public bool NoPttServices => PttServices.Count == 0;
    public string MemberFormTitle => MemberIsNew ? "새 구성원" : $"편집 — {EditName}";
    public bool HasVolte => _origVolte.Length > 0;
    public bool HasVoip => _origVoip.Length > 0;
    public bool HasPtt => _origPtt.Length > 0;
    /// <summary>유선 회선의 읽기전용 줄(내선 라벨 · 픽업 그룹)에 보일 것이 있다.</summary>
    public bool HasVoipDerived => VoipExtension.Length > 0 || VoipPickupGroup.Length > 0;
    public bool HasError => Error.Length > 0;
    /// <summary>편집 폼이 열려 있다. 화면을 오가도 폼은 유지된다.</summary>
    public bool IsEditing => OrgEditing || MemberEditing;
    /// <summary>저장하지 않은 변경이 있다 — [관리] 메뉴 점 배지·화면 머리 "저장하지 않은 변경"(§3.4). 행 클릭만으로 폼이 열리므로 열림≠변경.</summary>
    public bool IsDirty => IsEditing && Fingerprint() != _formBase;
    private string _formBase = "";
    private string Fingerprint() => string.Join("\u001f", EditName, EditOrg?.Code, EditTitle, EditLoginId, EditPassword, VolteNumber, VolteService?.Name, VolteTransport, VoltePassword,
                                                VoipNumber, VoipService?.Name, VoipTransport, VoipPassword,
                                                PttNumber, PttService?.Name, PttTransport, PttPassword, AllowCreateGroup, OrgCode, OrgName, OrgParent?.Code, OrgSort);
    private void MarkClean() { _formBase = Fingerprint(); OnPropertyChanged(nameof(IsDirty)); }
    protected override void OnPropertyChanged(PropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        if (e.PropertyName is not (nameof(IsDirty) or nameof(HasError) or nameof(Error) or nameof(Busy) or nameof(Loaded) or nameof(ScopeText) or nameof(MemberHeader) or nameof(Search)))
            base.OnPropertyChanged(new PropertyChangedEventArgs(nameof(IsDirty)));
    }

    /// <summary>확인 대화상자 — 창이 붙인다(제목, 본문) → 예/아니오.</summary>
    public Func<string, string, bool>? Confirm { get; set; }

    partial void OnErrorChanged(string value) => OnPropertyChanged(nameof(HasError));
    partial void OnVoipExtensionChanged(string value) => OnPropertyChanged(nameof(HasVoipDerived));
    partial void OnVoipPickupGroupChanged(string value) => OnPropertyChanged(nameof(HasVoipDerived));
    partial void OnOrgIsNewChanged(bool value) => OnPropertyChanged(nameof(OrgFormTitle));
    partial void OnOrgEditingChanged(bool value) => OnPropertyChanged(nameof(IsEditing));
    partial void OnMemberEditingChanged(bool value) => OnPropertyChanged(nameof(IsEditing));
    partial void OnOrgCodeChanged(string value) => OnPropertyChanged(nameof(OrgFormTitle));
    partial void OnOrgNameChanged(string value) => OnPropertyChanged(nameof(OrgFormTitle));
    partial void OnMemberIsNewChanged(bool value) => OnPropertyChanged(nameof(MemberFormTitle));
    partial void OnEditNameChanged(string value) => OnPropertyChanged(nameof(MemberFormTitle));
    partial void OnSearchChanged(string value) => Filter();
    partial void OnSelectedOrgChanged(OrgRow? value) { Filter(); OnPropertyChanged(nameof(MemberHeader)); }
    /// <summary>행 한 번 클릭 = 오른쪽 폼(§4.5 시안). 같은 구성원을 편집 중이면(재필터로 다시 선택될 때) 입력을 지우지 않는다.</summary>
    private MemberRow? _prevMember;
    partial void OnSelectedMemberChanged(MemberRow? value)
    {
        if (value is null) { _prevMember = null; return; }
        if (MemberEditing && !MemberIsNew && EditUserId == value.UserId) { _prevMember = value; return; }
        if (IsDirty && Confirm?.Invoke("변경 버림", "저장하지 않은 변경이 있습니다. 버리고 다른 항목으로 넘어갈까요?") == false)
        {
            var back = _prevMember; SelectedMember = back;             // 되돌림 — 같은 구성원이라 위 가드에서 멈춘다
            return;
        }
        _prevMember = value;
        EditMember();
    }
    /// <summary>구성원 머리 — "N명 · {선택 조직} 하위 포함".</summary>
    public string MemberHeader => SelectedOrg is null ? $"{Members.Count}명 · 범위 전체" : $"{Members.Count}명 · {SelectedOrg.Name} 하위 포함";

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
        VolteServices.Clear(); VoipServices.Clear(); PttServices.Clear();
        foreach (var s in v.Services) ServicesOf(s.Kind).Add(s);
        OnPropertyChanged(nameof(NoVolteServices)); OnPropertyChanged(nameof(NoVoipServices)); OnPropertyChanged(nameof(NoPttServices));
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
                && !(qn.Length > 0 && LineKind.All.Any(k => DirectoryService.Normalize(m.Line(k)?.Msisdn ?? "").Contains(qn)))) continue;
            Members.Add(new MemberRow(m, OrgPathOf(_view.Orgs, m.Org), _s.Directory));
        }
        SelectedMember = Members.FirstOrDefault(x => x.UserId == keep);
        OnPropertyChanged(nameof(MemberHeader));
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
        MarkClean();
    }
    [RelayCommand] private void BeginEditOrg()
    {
        if (SelectedOrg is null) return;
        OrgIsNew = false; OrgCode = SelectedOrg.Code; OrgName = SelectedOrg.Name; OrgParent = Orgs.FirstOrDefault(o => o.Code == SelectedOrg.Parent);
        OrgSort = _view?.Orgs.FirstOrDefault(o => o.Code == SelectedOrg.Code)?.Sort ?? 0; OrgEditing = true; MemberEditing = false;
        MarkClean();
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
        // 카드마다 자기 버킷의 첫 후보 — 버킷이 갈렸으므로 종류를 건너뛰는 폴백은 없다. 기본 transport 는 셋 다 TLS(관제 소프트폰 규약).
        VolteNumber = ""; VolteService = VolteServices.FirstOrDefault(); VolteTransport = "TLS"; VoltePassword = "";
        VoipNumber = ""; VoipService = VoipServices.FirstOrDefault(); VoipTransport = "TLS"; VoipPassword = ""; VoipExtension = ""; VoipPickupGroup = "";
        PttNumber = ""; PttService = PttServices.FirstOrDefault(); PttTransport = "TLS"; PttPassword = "";
        AllowCreateGroup = false; AllowAmbientListening = false; _origVolte = _origVoip = _origPtt = _origVolteImsi = _origVoipImsi = _origPttImsi = ""; _origCreate = false;
        OnPropertyChanged(nameof(HasVolte)); OnPropertyChanged(nameof(HasVoip)); OnPropertyChanged(nameof(HasPtt));
        MemberEditing = true; OrgEditing = false;
        MarkClean();
    }

    /// <summary>회선의 접속서비스 선택값. 새 회선 = 첫 후보. 기존 회선 = 저장된 이름 그대로 — 목록에 없으면 그 이름을 후보에 넣어 보이고(선택 = 현재값 = 변경 없음),
    /// 저장값이 비었으면 비워 둔다(서버가 현재값 유지). 저장값 대신 첫 후보로 바꿔 놓으면 저장할 때마다 "서비스 변경 → H(A1) 재결박 비밀번호 필요(400)" 가 난다.</summary>
    private static ServiceRef? PickService(ObservableCollection<ServiceRef> list, string kind, NumberInfo? line)
    {
        if (line is null) return list.FirstOrDefault();
        if (line.ServiceRef.Length == 0) return null;
        var hit = list.FirstOrDefault(s => s.Name == line.ServiceRef);
        if (hit is null) { hit = new ServiceRef(kind, line.ServiceRef, ""); list.Add(hit); }
        return hit;
    }
    [RelayCommand] private void EditMember()
    {
        if (SelectedMember is null) return;
        var i = SelectedMember.Info;
        MemberIsNew = false; EditUserId = i.UserId; EditName = i.Name; EditOrg = Orgs.FirstOrDefault(o => o.Code == i.Org); EditTitle = i.Title; EditLoginId = i.LoginId; EditPassword = "";
        // 기존 회선의 transport 는 서버 값 그대로(ANY 포함). 새로 개설할 카드(회선 없음)만 TLS 기본.
        VolteNumber = i.Volte?.Msisdn ?? ""; VolteService = PickService(VolteServices, LineKind.Volte, i.Volte);
        VolteTransport = i.Volte is null ? "TLS" : SipTransports.Normalize(i.Volte.SipTransport); VoltePassword = "";
        VoipNumber = i.Voip?.Msisdn ?? ""; VoipService = PickService(VoipServices, LineKind.Voip, i.Voip);
        VoipTransport = i.Voip is null ? "TLS" : SipTransports.Normalize(i.Voip.SipTransport); VoipPassword = "";
        VoipExtension = i.Voip?.Extension ?? ""; VoipPickupGroup = i.Voip?.PickupGroup ?? "";
        PttNumber = i.Ptt?.Msisdn ?? ""; PttService = PickService(PttServices, LineKind.Ptt, i.Ptt);
        PttTransport = i.Ptt is null ? "TLS" : SipTransports.Normalize(i.Ptt.SipTransport); PttPassword = "";
        AllowCreateGroup = i.Ptt?.Profile?.GetValueOrDefault("allowCreateGroup") == true;
        AllowAmbientListening = i.Ptt?.Profile?.GetValueOrDefault("allowAmbientListening") == true;
        _origVolte = VolteNumber; _origVoip = VoipNumber; _origPtt = PttNumber; _origCreate = AllowCreateGroup;
        _origVolteImsi = i.Volte?.Imsi ?? ""; _origVoipImsi = i.Voip?.Imsi ?? ""; _origPttImsi = i.Ptt?.Imsi ?? "";
        OnPropertyChanged(nameof(HasVolte)); OnPropertyChanged(nameof(HasVoip)); OnPropertyChanged(nameof(HasPtt));
        MemberEditing = true; OrgEditing = false;
        MarkClean();
    }
    [RelayCommand] private void CancelMember() => MemberEditing = false;

    private NumberInput? NumberOf(string number, ServiceRef? svc, string transport, string password, string imsi = "") =>
        number.Trim().Length == 0 ? null : new NumberInput { Msisdn = number.Trim(), Imsi = imsi, ServiceRef = svc?.Name ?? "", SipTransport = transport, Password = password };

    [RelayCommand] private async Task SaveMember()
    {
        var m = _s.Management; if (m is null) return;
        if (EditName.Trim().Length == 0 || EditOrg is null) { Error = "이름과 소속 조직은 필수입니다"; return; }
        // 회선 카드 셋 — (kind, 입력 번호, 저장 번호, 저장 IMSI, 접속서비스, transport, 비밀번호)
        var lines = new[]
        {
            (kind: LineKind.Volte, number: VolteNumber.Trim(), orig: _origVolte, origImsi: _origVolteImsi, svc: VolteService, tr: VolteTransport, pw: VoltePassword),
            (kind: LineKind.Voip,  number: VoipNumber.Trim(),  orig: _origVoip,  origImsi: _origVoipImsi,  svc: VoipService,  tr: VoipTransport,  pw: VoipPassword),
            (kind: LineKind.Ptt,   number: PttNumber.Trim(),   orig: _origPtt,   origImsi: _origPttImsi,   svc: PttService,   tr: PttTransport,   pw: PttPassword),
        };
        string ptt = PttNumber.Trim();
        if (MemberIsNew)
        {
            if (lines.Any(l => l.number.Length > 0 && l.pw.Length == 0)) { Error = "새 회선에는 SIP 비밀번호가 필요합니다(H(A1) 결박)"; return; }
            if (lines.FirstOrDefault(l => l.number.Length > 0 && l.svc is null) is { kind: { Length: > 0 } } missing)
            { Error = $"{LineKind.Label(missing.kind)} 회선을 개설하려면 접속서비스가 필요합니다 — 목록이 비어 있으면 서버 설정(운영자) 문제입니다"; return; }
            var input = new MemberInput { Name = EditName.Trim(), Org = EditOrg.Code, Title = EditTitle.Trim(), LoginId = EditLoginId.Trim(), Password = EditPassword,
                                          Volte = NumberOf(lines[0].number, VolteService, VolteTransport, VoltePassword),
                                          Voip = NumberOf(lines[1].number, VoipService, VoipTransport, VoipPassword),
                                          Ptt = NumberOf(lines[2].number, PttService, PttTransport, PttPassword) };
            Busy = true; Error = "";
            var r = await m.CreateMemberAsync(input);
            Busy = false;
            if (!r.Ok) { Error = "구성원 생성 실패 — " + ResponseText.Describe(ResponseText.Area.Management, r.Code, r.Reason); return; }
            // 원격 청취 자격(allowAmbientListening)은 관제 앱이 바꾸지 않는다 — 콘솔에서 역할(청취 범위)로 부여(서버 400 not_editable).
            if (ptt.Length > 0 && AllowCreateGroup)
                await m.PutPttProfileAsync(r.Value, new Dictionary<string, bool> { ["allowCreateGroup"] = AllowCreateGroup });
            _s.Notify.Info("구성원 생성 완료");
            await LoadAsync(force: true); _ = _s.SyncDirectoryAsync();
            MemberEditing = false;
            SelectedMember = Members.FirstOrDefault(x => x.UserId == r.Value);
            return;
        }
        long uid = EditUserId;
        var upd = new MemberInput { Name = EditName.Trim(), Org = EditOrg.Code, Title = EditTitle.Trim(), LoginId = EditLoginId.Trim(), Password = EditPassword };
        if (!await RunAsync(m.UpdateMemberAsync(uid, upd), "구성원 저장")) return;
        // 회선 — 번호·접속서비스가 바뀌었거나 비밀번호를 넣었으면 PUT(개설/변경/재결박), 비웠으면 DELETE. 바뀐 것이 없으면 보내지 않는다.
        //   접속서비스는 골랐을 때만 변경으로 본다(비어 있으면 서버가 현재값 유지). 같은 번호면 저장된 IMSI 를 그대로 실어 서버가 "IMSI 변경" 으로 오판하지 않게 한다
        //   (서버는 IMSI 가 없으면 번호 숫자로 채우므로, 실제 IMSI 가 다른 회선은 비밀번호 없는 PUT 이 400 이 된다). 번호가 바뀌면 새 회선이라 IMSI 를 비운다.
        //   transport 만 바뀐 회선도 PUT 이다(ANY 포함 명시값 — 비밀번호 불필요, H(A1) 과 무관).
        foreach (var (kind, number, orig, origImsi, svc, tr, pw) in lines)
        {
            string what = LineKind.Label(kind);
            bool serviceChanged = svc is not null && svc.Name != OrigService(kind);
            bool transportChanged = orig.Length > 0 && tr != SipTransports.Normalize(OrigLine(kind)?.SipTransport ?? "");
            if (number.Length == 0 && orig.Length > 0)
            {
                if (Confirm?.Invoke("회선 삭제", $"{what} 번호 {orig} 를 삭제할까요? 단말 등록이 끊깁니다.") != true) continue;
                if (!await RunAsync(m.DeleteNumberAsync(uid, kind), $"{what} 회선 삭제")) return;
            }
            else if (number.Length > 0 && (number != orig || pw.Length > 0 || serviceChanged || transportChanged))
            {
                if (orig.Length == 0 && svc is null) { Error = $"{what} 회선을 개설하려면 접속서비스가 필요합니다 — 목록이 비어 있으면 서버 설정(운영자) 문제입니다"; return; }
                if ((number != orig || (orig.Length > 0 && serviceChanged)) && pw.Length == 0)
                { Error = $"{what} {(number != orig ? "번호" : "접속서비스")}를 바꾸려면 SIP 비밀번호가 필요합니다(H(A1) 재결박)"; return; }
                var input = NumberOf(number, svc, tr, pw, number == orig ? origImsi : "")!;
                if (!await RunAsync(m.PutNumberAsync(uid, kind, input), $"{what} 회선 {(orig.Length == 0 ? "개설" : "변경")}")) return;
            }
        }
        if (ptt.Length > 0 && AllowCreateGroup != _origCreate)
            if (!await RunAsync(m.PutPttProfileAsync(uid, new Dictionary<string, bool> { ["allowCreateGroup"] = AllowCreateGroup }), "PTT 자격 저장")) return;
        MemberEditing = false;
    }

    private NumberInfo? OrigLine(string kind) => _view?.Members.FirstOrDefault(x => x.UserId == EditUserId)?.Line(kind);
    private string OrigService(string kind) => OrigLine(kind)?.ServiceRef ?? "";

    [RelayCommand] private async Task DeleteMember()
    {
        var m = _s.Management; if (m is null || SelectedMember is null) return;
        var row = SelectedMember;
        if (Confirm?.Invoke("구성원 삭제", $"'{row.Name}' 을 삭제할까요?\nVoLTE/VoIP/PTT 회선도 함께 삭제되고 단말 등록이 끊깁니다.") != true) return;
        if (await RunAsync(m.DeleteMemberAsync(row.UserId), "구성원 삭제")) { MemberEditing = false; SelectedMember = null; }
    }

    [RelayCommand] private Task Refresh() => LoadAsync(force: true);
}
