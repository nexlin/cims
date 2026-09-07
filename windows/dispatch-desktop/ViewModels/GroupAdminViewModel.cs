// 관리 창 › PTT 그룹 탭(§4.5.2) — 관리 범위 안 PTT 그룹 전부(`/provisioning/directory/groups`, 멤버가 아니어도)를 열거하고
// 생성·편집·삭제는 종전 GMS XCAP 경로(GroupEditWindow → CscClient.PutGroup/DeleteGroup)를 그대로 쓴다 — 서버가 관리 범위로
// 소유자가 아닌 그룹의 PUT/DELETE 도 허용(dispatch_center.md §3.4). 관리 범위가 없으면 GMS 목록의 내 소유 그룹만 보인다.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed class GroupAdminRow
{
    public ManagedGroup G { get; }
    public string OrgPath { get; }
    public GroupAdminRow(ManagedGroup g, string orgPath) { G = g; OrgPath = orgPath; }
    public string Name => G.Name;
    public string Id => G.Id;
    public string Meta => $"{G.MemberCount}명 · {SessionTypeText}{(G.IsOwner ? " · 내 그룹" : "")}";
    public string SessionTypeText => G.SessionType switch { "chat" => "채팅", "broadcast" => "방송", _ => "사전편성" };
    /// <summary>GroupEditViewModel 이 받는 항목 — 목록 ETag 는 편집 창이 문서 GET 으로 다시 받는다.</summary>
    public GroupInfo ToGroupInfo() => new(G.Id, G.Uri, G.Name, G.MemberCount) { IsOwner = G.IsOwner, Etag = G.ETag };
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
    public ObservableCollection<GroupAdminRow> Groups { get; } = new();
    public bool HasError => Error.Length > 0;
    public bool CanCreate => _s.CanCreateGroups;

    /// <summary>편집 창(생성 = 새 VM)·삭제 확인 — 창이 붙인다.</summary>
    public event EventHandler<GroupEditViewModel>? EditRequested;
    public Func<string, string, bool>? Confirm { get; set; }

    partial void OnErrorChanged(string value) => OnPropertyChanged(nameof(HasError));
    partial void OnSearchChanged(string value) => Filter();

    public async Task LoadAsync()
    {
        OnPropertyChanged(nameof(CanCreate));
        if (!_s.CanManageDirectory) { FromSession(); Hint = "관리 범위가 없어 GMS 목록의 내 소유 그룹만 보입니다"; return; }
        var m = _s.Management; if (m is null) return;
        Busy = true; Error = "";
        var r = await m.ListGroupsAsync();
        Busy = false;
        if (!r.Ok) { Error = ResponseText.Describe(ResponseText.Area.Management, r.Code, r.Reason); return; }
        _all = r.Value; Hint = $"관리 범위 안 PTT 그룹 {_all.Count}개";
        Filter();
    }

    private void FromSession()
    {
        _all = _s.Groups.Where(g => g.IsMember).Select(g => new ManagedGroup(g.Id, g.Uri, g.Name, g.MemberCount, g.IsOwner, "", "prearranged", g.Etag)).ToList();
        Filter();
    }

    private void Filter()
    {
        string keep = Selected?.Id ?? "";
        Groups.Clear();
        string q = Search.Trim();
        foreach (var g in _all)
        {
            if (q.Length > 0 && !g.Name.Contains(q, StringComparison.OrdinalIgnoreCase) && !g.Id.Contains(q, StringComparison.OrdinalIgnoreCase)) continue;
            Groups.Add(new GroupAdminRow(g, g.OrgCode.Length > 0 ? _s.Directory.OrgPath(g.OrgCode) : ""));
        }
        Selected = Groups.FirstOrDefault(x => x.Id == keep);
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
        if (row is null) return;
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
