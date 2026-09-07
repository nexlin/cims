// 관리 창(§4.5) — 조직/구성원·번호 · PTT 그룹 · 이력·녹취 세 탭의 조립. 앞 두 탭은 관리 범위(dispatch.directoryAdmin)가 있을 때 활성,
// 이력·녹취는 관제 그룹 소속이면 된다(monitor_scope/ptt_listen 범위는 서버가 건다).
using CommunityToolkit.Mvvm.ComponentModel;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class ManagementViewModel : ObservableObject
{
    public DispatchSession Session { get; }
    public DirectoryAdminViewModel Directory { get; }
    public GroupAdminViewModel Groups { get; }
    public SessionHistoryViewModel History { get; }

    [ObservableProperty] private int _tab;

    public ManagementViewModel(DispatchSession session)
    {
        Session = session;
        Directory = new DirectoryAdminViewModel(session);
        Groups = new GroupAdminViewModel(session);
        History = new SessionHistoryViewModel(session);
        if (!CanManage) Tab = 2;
    }

    public bool CanManage => Session.CanManageDirectory;
    public bool HasDesk => Session.HasDesk;
    public string Title => $"관리 — {Session.DisplayName} · {Session.GroupName}";
    public string ManageHint => CanManage ? "" : "조직/구성원·PTT 그룹 관리는 관제 그룹의 관리 범위(콘솔 구성 > 관제 그룹 > 관리 범위)가 있어야 합니다.";

    public async Task LoadAsync()
    {
        var tasks = new List<Task> { History.QueryAsync() };
        if (CanManage) tasks.Add(Directory.LoadAsync());
        tasks.Add(Groups.LoadAsync());
        await Task.WhenAll(tasks);
    }
}
