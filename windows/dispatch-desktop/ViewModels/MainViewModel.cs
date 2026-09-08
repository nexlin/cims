// 메인 — 패널 ViewModel 조립·패널 간 연동(발신 필드 채움·스레드 따라가기·[채널] 포커스)·핫키(§8)·감청 창 관리(§5)·
// 최상위 메뉴 화면 전환(§3.4: 관제·이력·PTT 그룹·관리 — 화면 VM 은 앱 수명 동안 하나, 전환은 가시성만)·자동 복귀 규칙.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class MainViewModel : ObservableObject
{
    public DispatchSession Session { get; }
    public Notifications Notify => Session.Notify;
    public DeskViewModel Desk { get; }
    public PttChannelsViewModel PttChannels { get; }
    public PttOriginateViewModel PttOriginate { get; }
    public McDataMessagesViewModel McData { get; }
    public PttActivityViewModel PttActivity { get; }
    public CallDeskViewModel CallDesk { get; }
    public CallOriginateViewModel CallOriginate { get; }
    public SmsMessagesViewModel Sms { get; }
    public CallActivityViewModel CallActivity { get; }
    public HotKeyMap HotKeys { get; }

    // ── 최상위 메뉴 화면(§3.4) — 관제 외 셋은 로그인당 1회 적재, 폼 상태는 전환해도 유지 ──
    public SessionHistoryViewModel HistoryScreen { get; }
    public GroupAdminViewModel GroupsScreen { get; }
    public DirectoryAdminViewModel AdminScreen { get; }
    /// <summary>관제 요약 띠(§3.5) — 관제 밖 화면 상단.</summary>
    public DispatchSummaryViewModel Summary { get; }
    [ObservableProperty] private AppScreen _screen = AppScreen.Dispatch;
    /// <summary>별창으로 떼어낸 화면 — 주 창 쪽은 자리표시자만 보인다(§3.4).</summary>
    public ObservableCollection<AppScreen> PoppedOut { get; } = new();
    /// <summary>[별창으로] — 창 관리는 MainWindow.</summary>
    public event EventHandler<AppScreen>? ScreenPopOutRequested;
    private bool _screensLoaded;

    /// <summary>감청 창 열기/활성화 요청 — 창 관리는 MainWindow.</summary>
    public event EventHandler<SessionItem>? MonitorWindowRequested;
    public event EventHandler<SessionItem>? MonitorWindowActivateRequested;
    public event EventHandler<SessionItem>? MonitorWindowCloseRequested;
    /// <summary>PTT 그룹 편집 창(생성 = 인자 null)·삭제 확인 요청 — 창은 MainWindow.</summary>
    public event EventHandler<GroupEditViewModel>? GroupEditRequested;
    public event EventHandler<GroupInfo>? GroupDeleteRequested;

    public MainViewModel(DispatchSession session, LayoutStore layout, HotKeyMap hotKeys)
    {
        Session = session;
        HotKeys = hotKeys;
        Desk = new DeskViewModel(session, layout);
        PttChannels = new PttChannelsViewModel(session);
        PttOriginate = new PttOriginateViewModel(session);
        McData = new McDataMessagesViewModel(session);
        PttActivity = new PttActivityViewModel(session);
        CallDesk = new CallDeskViewModel(session);
        CallOriginate = new CallOriginateViewModel(session);
        Sms = new SmsMessagesViewModel(session);
        CallActivity = new CallActivityViewModel(session);
        HistoryScreen = new SessionHistoryViewModel(session);
        GroupsScreen = new GroupAdminViewModel(session);
        AdminScreen = new DirectoryAdminViewModel(session);
        Summary = new DispatchSummaryViewModel(session, PttChannels, CallDesk, Desk);
        AdminScreen.PropertyChanged += (_, e) => { if (e.PropertyName is nameof(DirectoryAdminViewModel.IsEditing)) OnPropertyChanged(nameof(AdminEditing)); };
        GroupsScreen.EditRequested += (_, g) => GroupEditRequested?.Invoke(this, g);
        PttActivity.HistoryRequested += (_, _) => ShowHistory("ptt");
        CallActivity.HistoryRequested += (_, _) => ShowHistory("call");
        session.ProfileApplied += (_, _) => { _screensLoaded = false; OnPropertyChanged(nameof(CanManage)); OnPropertyChanged(nameof(ManageHint)); };

        // 패널 간 연동
        PttChannels.SelectionChanged += (_, c) => { if (c?.Group is not null) McData.FollowGroup(c.Group); };
        PttOriginate.MessageGroupRequested += (_, g) => McData.OpenGroup(g);
        PttOriginate.MessageUserRequested += (_, n) => McData.OpenUser(n);
        PttOriginate.AddChannelRequested += (_, g) => { Session.Settings.Update(s => { if (s.SelectedChannels.Count > 0 && !s.SelectedChannels.Contains(g.Id)) s.SelectedChannels.Add(g.Id); }); PttChannels.FocusGroup(g.Id); };
        PttOriginate.NewGroupRequested += (_, _) => GroupEditRequested?.Invoke(this, new GroupEditViewModel(Session, null));
        PttOriginate.EditGroupRequested += (_, g) => GroupEditRequested?.Invoke(this, new GroupEditViewModel(Session, g));
        PttOriginate.DeleteGroupRequested += (_, g) => GroupDeleteRequested?.Invoke(this, g);
        PttActivity.ChannelRequested += (_, id) => PttChannels.FocusGroup(id);
        PttActivity.WindowRequested += (_, s) => MonitorWindowActivateRequested?.Invoke(this, s);
        CallDesk.FillRequested += (_, n) => CallOriginate.Fill(n);
        CallDesk.DtmfRequested += (_, _) => CallOriginate.Mode = "pad";
        CallOriginate.SmsRequested += (_, n) => Sms.OpenNumber(n);
        CallActivity.SmsRequested += (_, n) => Sms.OpenNumber(n);
        CallActivity.WindowRequested += (_, s) => MonitorWindowActivateRequested?.Invoke(this, s);
        Desk.MonitorActivateRequested += (_, s) => MonitorWindowActivateRequested?.Invoke(this, s);

        session.SessionAdded += (_, s) => { if (s.IsWindow) MonitorWindowRequested?.Invoke(this, s); Desk.SyncMonitors(session.Sessions); CallOriginate.RefreshPad(); ReturnIfSessionStarted(s); };
        session.SessionEnded += (_, s) => { if (s.IsWindow) MonitorWindowCloseRequested?.Invoke(this, s); Desk.SyncMonitors(session.Sessions); CallOriginate.RefreshPad(); };
        session.SessionChanged += (_, s) => { CallOriginate.RefreshPad(); ReturnIfSessionStarted(s); };

        // 전역 핫키
        hotKeys.Pressed += (_, e) => OnHotKey(e.Name, down: true);
        hotKeys.Released += (_, e) => OnHotKey(e.Name, down: false);
    }

    /// <summary>재기동·재접속 후 화면 재구성 = 스냅샷 재조회(§11). 열린 감청 창도 listenOnly 호에서 복원. 화면은 관제로.</summary>
    public void RestoreFromSnapshot()
    {
        PttChannels.Rebuild();
        PttActivity.Rebuild();
        CallActivity.Rebuild();
        CallDesk.Refresh();
        foreach (var s in Session.Sessions.Where(x => x.IsWindow)) MonitorWindowRequested?.Invoke(this, s);
        Desk.SyncMonitors(Session.Sessions);
        Screen = AppScreen.Dispatch;
        OnPropertyChanged(nameof(CanManage)); OnPropertyChanged(nameof(ManageHint));
        Summary.Refresh();
    }

    // ── 최상위 메뉴(§3.4) ──
    public bool CanManage => Session.CanManageDirectory;
    public string ManageHint => CanManage ? "" : "조직/구성원·번호 관리는 관제 그룹의 관리 범위(콘솔 구성 > 관제 그룹 > 관리 범위)가 있어야 합니다. PTT 그룹은 내 소유 그룹만 편집합니다.";
    /// <summary>[관리] 메뉴 점 배지 — 편집 폼이 열려 있다(전환을 막지 않는다).</summary>
    public bool AdminEditing => AdminScreen.IsEditing;
    public bool IsDispatch => Screen == AppScreen.Dispatch;
    public string ScreenTitle => AppScreens.Title(Screen);

    partial void OnScreenChanged(AppScreen value)
    {
        OnPropertyChanged(nameof(IsDispatch)); OnPropertyChanged(nameof(ScreenTitle));
        if (value != AppScreen.Dispatch) { Summary.Refresh(); _ = LoadScreensAsync(); }
    }

    /// <summary>관제 외 화면의 서버 자료는 로그인 뒤 처음 그 화면을 열 때 한 번 받는다(이후는 화면 안 [새로고침]·저장 후 재조회).</summary>
    public async Task LoadScreensAsync()
    {
        if (_screensLoaded || Session.Management is null) return;
        _screensLoaded = true;
        var tasks = new List<Task> { HistoryScreen.QueryAsync(), GroupsScreen.LoadAsync() };
        if (CanManage) tasks.Add(AdminScreen.LoadAsync());
        try { await Task.WhenAll(tasks); }
        catch (Exception ex) { Session.Log.Warn("screens load: " + ex.Message); }
    }

    [RelayCommand] private void ShowScreen(AppScreen s)
    {
        if (s == AppScreen.Admin && !CanManage) { Notify.Warn("관리 범위 없음", ManageHint); return; }
        Screen = s;
    }
    [RelayCommand] private void ReturnToDispatch() => Screen = AppScreen.Dispatch;

    /// <summary>②④ 머리의 [이력에서 보기] — 종류를 맞춰 [이력] 로.</summary>
    public void ShowHistory(string kind)
    {
        int idx = kind == "ptt" ? 1 : 0;
        bool changed = HistoryScreen.KindIndex != idx;
        HistoryScreen.KindIndex = idx;                       // 바뀌면 VM 이 스스로 조회한다
        Screen = AppScreen.History;
        if (!changed && _screensLoaded) _ = HistoryScreen.QueryAsync();
    }

    [RelayCommand] private void PopOutScreen(AppScreen s)
    {
        if (s == AppScreen.Dispatch || PoppedOut.Contains(s)) return;
        PoppedOut.Add(s);
        ScreenPopOutRequested?.Invoke(this, s);
        if (Screen == s) Screen = AppScreen.Dispatch;
    }
    /// <summary>별창이 닫혔다 — 주 창 화면으로 되돌아온다.</summary>
    public void OnScreenWindowClosed(AppScreen s) => PoppedOut.Remove(s);

    /// <summary>자동 복귀 규칙(§3.4): 세션을 만드는 조작(응답·당겨받기·발신·사설콜·애드혹)은 관제로 돌아온다 — 보류·전달·종료 버튼이 거기 있다.
    /// 착신(링잉)·멤버 채널 합류·감청 창은 배너/칩만 띄우고 화면을 바꾸지 않는다.</summary>
    private void ReturnIfSessionStarted(SessionItem s)
    {
        if (Screen == AppScreen.Dispatch || s.IsWindow || s.Kind == SessionKind.PttChannel) return;
        if (s.IsActive || s.IsOutgoing) Screen = AppScreen.Dispatch;
    }

    public void OnHotKey(string name, bool down)
    {
        switch (name)
        {
            case "ptt": if (down) PttChannels.PttDown(); else PttChannels.PttUp(); break;
            case "answer": if (down && Notify.TopIncoming?.Session is { } inc) Session.Answer(inc); break;
            case "hangup": if (down && Session.ActiveVolteCall is { } act) Session.Hangup(act); break;
            case "pickup": if (down) Session.Pickup(); break;
            case "hold":
                if (!down) break;
                if (Session.ActiveVolteCall is { } a) Session.Hold(a);
                else if (Session.VolteCalls.FirstOrDefault(c => c.IsHeld) is { } h) Session.Resume(h);
                break;
            case "mute":
                if (!down) break;
                var target = Session.ActiveVolteCall ?? Session.Sessions.FirstOrDefault(s => s.Kind == SessionKind.PttPrivate && s.IsFullDuplex && s.IsActive);
                if (target is not null) Session.ToggleMute(target);
                break;
        }
    }

    public void SelectChannel(int n) => PttChannels.SelectIndex(n);

    [RelayCommand] private void AnswerBanner(Banner b) { if (b.Session is not null) { Session.Answer(b.Session); Screen = AppScreen.Dispatch; } }
    [RelayCommand] private void RejectBanner(Banner b) { if (b.Session is not null) Session.Reject(b.Session); }
    [RelayCommand] private void GoToChannel(Banner b) { Screen = AppScreen.Dispatch; if (b.GroupId.Length > 0) PttChannels.FocusGroup(b.GroupId); }
    [RelayCommand] private void DismissToast(Toast t) => Notify.Dismiss(t);
    [RelayCommand] private void ToggleToastDetail(Toast t) => t.ShowDetail = !t.ShowDetail;

    public void Tick(DateTime now)
    {
        Session.Tick(now);
        Desk.Tick(now);
        PttChannels.Tick();
        PttActivity.Tick();
        CallActivity.Tick();
        CallDesk.Refresh();
        if (Screen != AppScreen.Dispatch || PoppedOut.Count > 0) Summary.Refresh();
    }
}
