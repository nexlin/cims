// 관리 창 › PTT 그룹 탭(§4.5.2) — 관리 범위 안 PTT 그룹 전부(`/provisioning/directory/groups`, 멤버가 아니어도)를 열거하고
// 생성·편집·삭제는 종전 GMS XCAP 경로(GroupEditWindow → CscClient.PutGroup/DeleteGroup)를 그대로 쓴다 — 서버가 관리 범위로
// 소유자가 아닌 그룹의 PUT/DELETE 도 허용(dispatch_center.md §3.4). 관리 범위가 없으면 GMS 목록의 내 소유 그룹만 보인다.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class GroupAdminRow : ObservableObject
{
    public ManagedGroup G { get; }
    public string OrgPath { get; }
    public GroupAdminRow(ManagedGroup g, string orgPath, string ownerText) { G = g; OrgPath = orgPath; _ownerText = ownerText; }
    public string Name => G.Name;
    public string Id => G.Id;
    public string Meta => $"{G.MemberCount}명 · {SessionTypeText}{(G.IsOwner ? " · 내 그룹" : "")}{(G.IsMember ? " · 멤버" : "")}{(!G.CanManage && G.InListenScope ? " · 청취 범위" : "")}";
    /// <summary>편집·삭제 버튼 — 관리 범위 밖(청취·멤버로만 보이는) 행은 숨긴다(서버 GMS 게이트가 어차피 403).</summary>
    public bool CanManage => G.CanManage;
    public int MemberCount => G.MemberCount;
    /// <summary>관계 열(§4.7) — 멤버 › 청취 범위 › 소유 › 범위(관리만).</summary>
    public string Relation => G.IsMember ? "멤버" : G.InListenScope ? "청취 범위" : G.IsOwner ? "소유" : "범위";
    public bool IsMemberRelation => G.IsMember;
    /// <summary>소유자 열 — 목록엔 소유 여부만 있어 내 것은 "이름(나)", 나머지는 상세(문서 GET)가 채운다.</summary>
    [ObservableProperty] private string _ownerText = "";
    public string SessionTypeText => G.SessionType switch { "chat" => "채팅", "broadcast" => "방송", _ => "사전편성" };
    /// <summary>GroupEditViewModel 이 받는 항목 — 목록 ETag 는 편집 창이 문서 GET 으로 다시 받는다.</summary>
    public GroupInfo ToGroupInfo() => new(G.Id, G.Uri, G.Name, G.MemberCount) { IsOwner = G.IsOwner, Etag = G.ETag };
}

/// <summary>상세 패널의 멤버 한 줄 — 문서 멤버 + 로스터 상태(참여/발언 중/미참가) + 나.</summary>
public sealed record GroupDetailMember(string Name, string Number, string Status, bool IsMe, bool IsChair)
{
    public bool IsSpeaking => Status == "발언 중";
    public bool IsAbsent => Status == "미참가";
    public string Label => Name.Length > 0 ? Name : Number;
    public string ChairText => IsChair ? " 의장" : "";
}

public sealed partial class GroupAdminViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private IReadOnlyList<ManagedGroup> _all = Array.Empty<ManagedGroup>();

    public GroupAdminViewModel(DispatchSession s)
    {
        _s = s;
        s.Groups.CollectionChanged += (_, _) => { if (!_s.CanManageDirectory) FromSession(); };
    }

    [ObservableProperty] private bool _busy;
    [ObservableProperty] private string _error = "";
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private GroupAdminRow? _selected;
    [ObservableProperty] private string _hint = "";
    /// <summary>목록 필터 칩(§4.7) — all | member | mine.</summary>
    [ObservableProperty] private string _listFilter = "all";
    public ObservableCollection<GroupAdminRow> Groups { get; } = new();
    public int TotalCount => _all.Count;

    // ── 상세 패널(선택 그룹) — 문서 GET(소유자·우선순위·긴급·멤버) + 세션 로스터(참여·발언) ──
    [ObservableProperty] private bool _detailLoading;
    [ObservableProperty] private string _detailError = "";
    [ObservableProperty] private CimsUe.GroupDoc? _detailDoc;
    public ObservableCollection<GroupDetailMember> DetailMembers { get; } = new();
    public bool HasDetail => Selected is not null;
    public string DetailOwner => DetailDoc is null ? "" : OwnerLabel(DetailDoc.AuthorizedUser);
    public string DetailOrg => Selected is null ? "" : (Selected.OrgPath.Length > 0 ? Selected.OrgPath : "—");
    public string DetailPolicy => DetailDoc is null ? "" : $"우선순위 {DetailDoc.Priority} · 긴급 {(DetailDoc.EmergencyCall ? "허용" : "불가")} · {(DetailDoc.SessionType switch { "chat" => "채팅", "broadcast" => "방송", _ => "사전편성" })}";
    public string DetailCapability => DetailDoc is null ? "" : string.Join(" · ", new[] { DetailDoc.AllowSds ? "SDS" : "", DetailDoc.AllowFd ? "FD" : "", DetailDoc.VideoEnabled ? "영상" : "", DetailDoc.Encryption ? "암호화" : "", DetailDoc.RequireAffiliation ? "affiliation 필요" : "" }.Where(x => x.Length > 0));
    public string DetailListenVisibility => _s.ListenHidden ? "은닉" : "투명";
    public bool DetailHasSession => Selected is not null && _s.Groups.FirstOrDefault(g => g.Id == Selected.Id)?.HasSession == true;
    public int DetailAffiliated => DetailMembers.Count(m => !m.IsAbsent);
    public string DetailMemberCount => $"멤버 {DetailMembers.Count}";
    public string DetailAffiliationText => $"affiliation {DetailAffiliated}";
    /// <summary>[채널로] — 관제 캔버스의 채널 카드로(없으면 합류). 화면 전환은 MainViewModel.</summary>
    public event EventHandler<string>? ChannelRequested;
    public bool HasError => Error.Length > 0;
    public bool CanCreate => _s.CanCreateGroups;

    /// <summary>편집 창(생성 = 새 VM)·삭제 확인 — 창이 붙인다.</summary>
    public event EventHandler<GroupEditViewModel>? EditRequested;
    public Func<string, string, bool>? Confirm { get; set; }

    partial void OnErrorChanged(string value) => OnPropertyChanged(nameof(HasError));
    partial void OnSearchChanged(string value) => Filter();
    partial void OnListFilterChanged(string value) => Filter();
    partial void OnSelectedChanged(GroupAdminRow? value) { OnPropertyChanged(nameof(HasDetail)); OnPropertyChanged(nameof(DetailOrg)); _ = LoadDetailAsync(value); }
    partial void OnDetailDocChanged(CimsUe.GroupDoc? value)
    {
        foreach (var p in new[] { nameof(DetailOwner), nameof(DetailPolicy), nameof(DetailCapability), nameof(DetailListenVisibility), nameof(DetailHasSession) }) OnPropertyChanged(p);
    }
    [RelayCommand] private void SetFilter(string f) => ListFilter = f;
    [RelayCommand] private void GoToChannel() { if (Selected is not null) ChannelRequested?.Invoke(this, Selected.Id); }

    private string OwnerLabel(string owner)
    {
        if (owner.Length == 0) return "—";
        string n = _s.Directory.NameOf(owner);
        return _s.IsMe(owner) ? $"{(n.Length > 0 ? n : _s.DisplayName)}(나)" : n.Length > 0 ? n : owner;
    }

    private int _detailSeq;
    private async Task LoadDetailAsync(GroupAdminRow? row)
    {
        int seq = ++_detailSeq;
        DetailDoc = null; DetailMembers.Clear(); DetailError = "";
        RefreshDetailCounts();
        if (row is null) return;
        DetailLoading = true;
        var r = await _s.GetGroupAsync(row.ToGroupInfo());
        if (seq != _detailSeq) return;                                  // 그 사이 다른 행 선택
        DetailLoading = false;
        if (!r.Ok) { DetailError = ResponseText.Describe(ResponseText.Area.Group, r.Code, r.Reason); return; }
        DetailDoc = r.Value;
        row.OwnerText = OwnerLabel(r.Value.AuthorizedUser);
        RefreshDetailMembers();
    }

    /// <summary>로스터·발언 변화를 상세에 반영 — 1초 틱(MainViewModel.Tick)에서 호출.</summary>
    public void Tick() { if (Selected is not null && DetailDoc is not null) RefreshDetailMembers(); }

    private void RefreshDetailMembers()
    {
        if (Selected is null || DetailDoc is null) return;
        var live = _s.Groups.FirstOrDefault(g => g.Id == Selected.Id);
        var session = _s.SessionOfGroup(Selected.Id);
        string speaker = session?.Speaker ?? "";
        var rows = new List<GroupDetailMember>();
        foreach (var m in DetailDoc.Members)
        {
            string number = Converters.UserPartConverter.UserPart(m.Uri);
            string name = m.Name.Length > 0 ? m.Name : _s.Directory.NameOf(number);
            string norm = DirectoryService.Normalize(number);
            bool connected = live?.Roster.Any(e => DirectoryService.Normalize(Converters.UserPartConverter.UserPart(e.Uri)) == norm && e.Status is "connected" or "listener") == true;
            bool speaking = speaker.Length > 0 && (speaker == name || DirectoryService.Normalize(speaker) == norm);
            string status = speaking ? "발언 중" : connected ? "참여" : "미참가";
            rows.Add(new GroupDetailMember(name, _s.Directory.DisplayNumber(number), status, _s.IsMe(m.Uri), m.Role == "chair"));
        }
        rows.Sort((a, b) => (a.IsAbsent ? 1 : 0).CompareTo(b.IsAbsent ? 1 : 0));
        if (rows.Count == DetailMembers.Count && rows.Zip(DetailMembers).All(x => x.First == x.Second)) { OnPropertyChanged(nameof(DetailHasSession)); return; }
        DetailMembers.Clear();
        foreach (var x in rows) DetailMembers.Add(x);
        RefreshDetailCounts();
    }

    private void RefreshDetailCounts()
    {
        foreach (var p in new[] { nameof(DetailAffiliated), nameof(DetailMemberCount), nameof(DetailAffiliationText), nameof(DetailHasSession) }) OnPropertyChanged(p);
    }

    public async Task LoadAsync()
    {
        OnPropertyChanged(nameof(CanCreate));
        if (!_s.CanManageDirectory) { FromSession(); Hint = "관리 범위가 없어 GMS 목록의 내 소유 그룹만 보입니다"; return; }
        var m = _s.Management; if (m is null) return;
        Busy = true; Error = "";
        var r = await m.ListGroupsAsync();
        Busy = false;
        if (!r.Ok) { Error = ResponseText.Describe(ResponseText.Area.Management, r.Code, r.Reason); return; }
        _all = r.Value;
        int manageable = _all.Count(g => g.CanManage);
        Hint = manageable == _all.Count ? "관리 범위 안 그룹 전부" : $"관리 가능 {manageable}개 · 나머지는 청취 범위·멤버 그룹(보기만)";
        Filter();
    }

    private void FromSession()
    {
        _all = _s.Groups.Where(g => g.IsMember).Select(g => new ManagedGroup(g.Id, g.Uri, g.Name, g.MemberCount, g.IsOwner, "", "prearranged", g.Etag, CanManage: g.IsOwner, IsMember: true)).ToList();
        Filter();
    }

    private void Filter()
    {
        string keep = Selected?.Id ?? "";
        Groups.Clear();
        string q = Search.Trim();
        var owners = Groups.ToDictionary(x => x.Id, x => x.OwnerText);      // 상세가 채운 소유자 표시는 재필터 뒤에도 유지
        foreach (var g in _all)
        {
            if (ListFilter == "member" && !g.IsMember) continue;
            if (ListFilter == "mine" && !g.IsOwner) continue;
            if (q.Length > 0 && !g.Name.Contains(q, StringComparison.OrdinalIgnoreCase) && !g.Id.Contains(q, StringComparison.OrdinalIgnoreCase)) continue;
            Groups.Add(new GroupAdminRow(g, g.OrgCode.Length > 0 ? _s.Directory.OrgPath(g.OrgCode) : "", g.IsOwner ? $"{_s.DisplayName}(나)" : owners.GetValueOrDefault(g.Id, "")));
        }
        OnPropertyChanged(nameof(TotalCount));
        Selected = Groups.FirstOrDefault(x => x.Id == keep) ?? Groups.FirstOrDefault();
    }

    [RelayCommand] private Task Refresh() => LoadAsync();

    [RelayCommand] private void NewGroup()
    {
        var vm = new GroupEditViewModel(_s, null);
        vm.Saved += async (_, _) => await LoadAsync();
        EditRequested?.Invoke(this, vm);
    }

    [RelayCommand] private void EditGroup(GroupAdminRow? row)
    {
        row ??= Selected;
        if (row is null || !row.CanManage) return;                    // 더블클릭 경로 — 보기 전용 행은 편집 창을 열지 않는다
        var vm = new GroupEditViewModel(_s, row.ToGroupInfo());
        vm.Saved += async (_, _) => await LoadAsync();
        EditRequested?.Invoke(this, vm);
    }

    [RelayCommand] private async Task DeleteGroup(GroupAdminRow? row)
    {
        row ??= Selected;
        if (row is null) return;
        var live = _s.SessionOfGroup(row.Id) ?? _s.ListenOfGroup(row.Id);
        string extra = live is not null ? "\n진행 중인 세션이 있습니다 — 삭제하면 서버가 세션을 정리합니다." : "";
        if (Confirm?.Invoke("그룹 삭제", $"그룹 '{row.Name}' ({row.Id}) 을 삭제할까요?\n멤버 {row.G.MemberCount}명의 단말에서도 사라집니다.{extra}") != true) return;
        Busy = true; Error = "";
        var r = await _s.DeleteGroupAsync(row.ToGroupInfo());
        Busy = false;
        if (!r.Ok) Error = ResponseText.Describe(ResponseText.Area.Group, r.Code, r.Reason);
        await LoadAsync();
    }
}
