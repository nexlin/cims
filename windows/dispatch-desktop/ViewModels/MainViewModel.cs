// 메인 — 패널 ViewModel 조립(3×2: ① 내 채널·② 범위 채널·③ 일반통화 / ④ PTT 메시지·⑤ PTT 이벤트·⑥ 일반통화 내역)·패널 간 연동(발신 필드 채움·스레드/이벤트 따라가기·
// [채널로] 포커스·팝오버 닫힘)·핫키(§8)·감청 창 관리(§5)·채널 편집 드로어(§4.2)·사람 메뉴/Ctrl+K(§4.1)·
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
    // ① 내 채널
    public PttChannelsViewModel PttChannels { get; }
    public TalkBarViewModel TalkBar { get; }
    public PttOriginateViewModel PttOriginate { get; }
    // ② 범위 채널
    public ScopedChannelsViewModel Scoped { get; }
    // ③ 일반통화
    public CallDeskViewModel CallDesk { get; }
    public CallOriginateViewModel CallOriginate { get; }
    public SmsMessagesViewModel Sms { get; }
    // ④⑤⑥
    public McDataMessagesViewModel McData { get; }
    public PttActivityViewModel PttActivity { get; }
    public CallActivityViewModel CallActivity { get; }
    // 사람 메뉴 · Ctrl+K
    public PersonActionsViewModel People { get; }
    public HotKeyMap HotKeys { get; }

    // ── 팝오버 상태(§4.4 공통 규칙: 비모달, Esc·바깥 클릭 닫힘, 세션이 성립하면 자동 닫힘) ──
    /// <summary>① [사설콜 ▾]/[애드혹 ▾] 팝오버(PttOriginateView).</summary>
    [ObservableProperty] private bool _pttOriginateOpen;
    /// <summary>③ [▦ ▾] 팝오버(CallOriginateView — 다이얼패드|주소록|최근).</summary>
    [ObservableProperty] private bool _callOriginateOpen;
    /// <summary>③ [문자 n] 팝오버(SMS·LMS).</summary>
    [ObservableProperty] private bool _smsOpen;
    /// <summary>채널 편집 드로어(§4.2) — GroupsScreen.Editor 가 있을 때 관제 캔버스 오른쪽에서 밀려 나온다.</summary>
    public bool DrawerOpen => GroupsScreen.IsEditing && _drawerRequested;
    private bool _drawerRequested;

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
    /// <summary>[관제로 F1] — 별창에서 눌렀으면 주 창을 앞으로 가져와야 관제 캔버스가 보인다. 창 활성화는 MainWindow.</summary>
    public event EventHandler? DispatchActivateRequested;
    private bool _screensLoaded;

    /// <summary>감청 창 열기/활성화 요청 — 창 관리는 MainWindow.</summary>
    public event EventHandler<SessionItem>? MonitorWindowRequested;
    public event EventHandler<SessionItem>? MonitorWindowActivateRequested;
    public event EventHandler<SessionItem>? MonitorWindowCloseRequested;
    /// <summary>PTT 그룹 삭제 확인 요청 — 대화상자는 MainWindow.</summary>
    public event EventHandler<GroupInfo>? GroupDeleteRequested;

    public MainViewModel(DispatchSession session, LayoutStore layout, HotKeyMap hotKeys)
    {
        Session = session;
        HotKeys = hotKeys;
        Desk = new DeskViewModel(session, layout);
        PttChannels = new PttChannelsViewModel(session);
        TalkBar = new TalkBarViewModel(session, PttChannels);
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
        Scoped = new ScopedChannelsViewModel(session, GroupsScreen);
        People = new PersonActionsViewModel(session);
        Summary = new DispatchSummaryViewModel(session, PttChannels, TalkBar, CallDesk, Desk, Sms);
        AdminScreen.PropertyChanged += (_, e) => { if (e.PropertyName is nameof(DirectoryAdminViewModel.IsDirty)) OnPropertyChanged(nameof(AdminEditing)); };
        GroupsScreen.ChannelRequested += (_, id) => { Screen = AppScreen.Dispatch; PttChannels.FocusGroup(id); };   // [채널로] — 채널 카드로(없으면 합류)
        GroupsScreen.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(GroupAdminViewModel.IsEditing)) { if (!GroupsScreen.IsEditing) _drawerRequested = false; OnPropertyChanged(nameof(DrawerOpen)); } };
        PttActivity.HistoryRequested += (_, _) => ShowHistory("ptt");
        CallActivity.HistoryRequested += (_, _) => ShowHistory("call");
        session.ProfileApplied += (_, _) => { _screensLoaded = false; OnPropertyChanged(nameof(CanManage)); OnPropertyChanged(nameof(ManageHint)); _ = LoadGroupsForScopedAsync(); };

        // ── ① 내 채널 ↔ ④⑤ 따라가기 · 발언 바 · 발신 팝오버 ──
        PttChannels.SelectionChanged += (_, c) =>
        {
            if (c?.Group is not null) McData.FollowGroup(c.Group); else McData.ClearFocus();
            PttActivity.FocusTitle = c?.Group?.Name ?? c?.Title ?? "";
        };
        PttChannels.ThreadRequested += (_, g) => McData.OpenGroup(g);
        PttChannels.PersonMenuRequested += (_, uri) => People.OpenMenu(uri);
        PttChannels.EditRequested += (_, g) => OpenDrawerEdit(g);
        PttOriginate.CloseRequested += (_, _) => PttOriginateOpen = false;
        TalkBar.FocusRequested += (_, c) => PttChannels.Select(c, collapseSame: false);
        McData.UnreadChanged += (_, _) => PttChannels.SetUnread(g => McData.UnreadOf(g.Uri));
        PttOriginate.MessageGroupRequested += (_, g) => McData.OpenGroup(g);
        PttOriginate.MessageUserRequested += (_, n) => McData.OpenUser(n);
        PttOriginate.ChannelRequested += (_, g) => { PttChannels.FocusGroup(g.Id); PttOriginateOpen = false; };
        // 주소록 [그룹] 탭의 [새 그룹]/[편집] — 채널 편집 드로어(§4.2, [PTT 그룹] 화면과 같은 VM)
        PttOriginate.NewGroupRequested += (_, _) => OpenDrawerNew();
        PttOriginate.EditGroupRequested += (_, g) => OpenDrawerEdit(g);
        PttOriginate.DeleteGroupRequested += (_, g) => GroupDeleteRequested?.Invoke(this, g);
        PttActivity.ChannelRequested += (_, id) => PttChannels.FocusGroup(id);
        PttActivity.ReplyRequested += (_, id) => { if (Session.Groups.FirstOrDefault(g => g.Id == id) is { } g) McData.OpenGroup(g); };

        // ── ② 범위 채널 ──
        Scoped.WindowRequested += (_, s) => MonitorWindowActivateRequested?.Invoke(this, s);
        Scoped.EditRequested += (_, g) => OpenDrawerEdit(g);
        Scoped.DeleteRequested += (_, g) => GroupDeleteRequested?.Invoke(this, g);
        Scoped.NewChannelRequested += (_, _) => OpenDrawerNew();

        // ── ③ 일반통화 ──
        CallDesk.FillRequested += (_, n) => { CallOriginate.Fill(n); };
        CallDesk.MenuRequested += (_, n) => People.OpenMenu(n);
        CallDesk.DeskFilterRequested += (_, f) => CallActivity.Filter = f;
        CallOriginate.SmsRequested += (_, n) => OpenSms(n);
        CallActivity.SmsRequested += (_, n) => OpenSms(n);
        CallActivity.MenuRequested += (_, n) => People.OpenMenu(n);
        CallActivity.WindowRequested += (_, s) => MonitorWindowActivateRequested?.Invoke(this, s);
        Desk.MonitorActivateRequested += (_, s) => MonitorWindowActivateRequested?.Invoke(this, s);

        // ── 사람 메뉴 · Ctrl+K 행동 ──
        People.PrivateCallRequested += (_, n) => { PttOriginate.PrivateCallTo(n); ReturnToDispatchSilently(); };
        People.AdhocAddRequested += (_, n) => { PttOriginate.AddAdhoc(n); PttOriginateOpen = true; ReturnToDispatchSilently(); };
        People.SdsRequested += (_, n) => McData.OpenUser(n);
        People.CallRequested += (_, n) => { Session.Dial(n); };
        People.SmsRequested += (_, n) => OpenSms(n);
        People.ChannelRequested += (_, id) => { ReturnToDispatchSilently(); PttChannels.FocusGroup(id); };
        People.AddMemberRequested += (_, g) => OpenDrawerEdit(g);

        session.SessionAdded += (_, s) => { if (s.IsWindow) MonitorWindowRequested?.Invoke(this, s); Desk.SyncMonitors(session.Sessions); CallOriginate.RefreshPad(); ReturnIfSessionStarted(s); ClosePopoversFor(s); };
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
        Scoped.Rebuild();
        PttActivity.Rebuild();
        CallActivity.Rebuild();
        CallDesk.Rebuild();
        foreach (var s in Session.Sessions.Where(x => x.IsWindow)) MonitorWindowRequested?.Invoke(this, s);
        Desk.SyncMonitors(Session.Sessions);
        Screen = AppScreen.Dispatch;
        OnPropertyChanged(nameof(CanManage)); OnPropertyChanged(nameof(ManageHint));
        Summary.Refresh();
    }

    /// <summary>② 관리 범위 섹션은 [PTT 그룹] 화면과 같은 목록을 쓴다 — 로그인 직후 한 번 적재(화면을 열지 않아도).</summary>
    private async Task LoadGroupsForScopedAsync()
    {
        if (Session.Management is null) return;
        try { await GroupsScreen.LoadAsync(); } catch (Exception ex) { Session.Log.Warn("scoped groups load: " + ex.Message); }
    }

    // ── 최상위 메뉴(§3.4) ──
    public bool CanManage => Session.CanManageDirectory;
    public string ManageHint => CanManage ? "" : "조직/구성원·번호 관리는 관제 그룹의 관리 범위(콘솔 구성 > 관제 그룹 > 관리 범위)가 있어야 합니다. PTT 그룹은 내 소유 그룹만 편집합니다.";
    /// <summary>[관리] 메뉴 점 배지 — 저장하지 않은 변경이 있다(전환을 막지 않는다).</summary>
    public bool AdminEditing => AdminScreen.IsDirty;
    public bool IsDispatch => Screen == AppScreen.Dispatch;
    public string ScreenTitle => AppScreens.Title(Screen);

    partial void OnScreenChanged(AppScreen value)
    {
        OnPropertyChanged(nameof(IsDispatch)); OnPropertyChanged(nameof(ScreenTitle));
        if (value != AppScreen.Dispatch) { Summary.Refresh(); _ = LoadScreensAsync(); PttOriginateOpen = CallOriginateOpen = SmsOpen = false; }
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
    [RelayCommand] private void ReturnToDispatch() { Screen = AppScreen.Dispatch; DispatchActivateRequested?.Invoke(this, EventArgs.Empty); }
    private void ReturnToDispatchSilently() { if (Screen != AppScreen.Dispatch) Screen = AppScreen.Dispatch; }

    /// <summary>⑤⑥ 머리의 [이력에서 보기] — 종류를 맞춰 [이력] 로.</summary>
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

    /// <summary>팝오버 공통 규칙 — 세션이 성립하면 자동 닫힘(발신·사설콜·애드혹).</summary>
    private void ClosePopoversFor(SessionItem s)
    {
        if (s.Info.Dir != CimsUe.CallDir.Outgoing) return;
        if (s.Kind is SessionKind.PttPrivate or SessionKind.PttAdhoc) PttOriginateOpen = false;
        if (s.Kind == SessionKind.VolteCall) CallOriginateOpen = false;
    }

    // ── 팝오버·드로어 ──
    /// <summary>① [사설콜 ▾]/[애드혹 ▾] — 같은 모드로 다시 누르면 닫힌다, 다른 모드면 바꿔 연다.</summary>
    [RelayCommand] private void OpenPttOriginate(string mode)
    {
        if (PttOriginateOpen && PttOriginate.Mode == mode) { PttOriginateOpen = false; return; }
        PttOriginate.Mode = mode; PttOriginateOpen = true;
    }
    [RelayCommand] private void ToggleCallOriginate() => CallOriginateOpen = !CallOriginateOpen;
    [RelayCommand] private void ToggleSms() => SmsOpen = !SmsOpen;
    /// <summary>[문자] 팝오버를 받는 사람으로 연다(사람 메뉴·그룹원 칩·주소록·⑥ 행).</summary>
    public void OpenSms(string number) { Sms.OpenNumber(number); SmsOpen = true; ReturnToDispatchSilently(); }
    [RelayCommand] private void ToggleSearch() => People.SearchOpen = !People.SearchOpen;

    private void OpenDrawerNew() { if (!GroupsScreen.IsEditing) { GroupsScreen.NewExternal(); _drawerRequested = GroupsScreen.IsEditing; OnPropertyChanged(nameof(DrawerOpen)); ReturnToDispatchSilently(); } else Notify.Warn("편집 중인 폼이 있습니다", "저장하거나 취소한 뒤 다시 시도하세요"); }
    private void OpenDrawerEdit(GroupInfo g) { if (!GroupsScreen.IsEditing) { GroupsScreen.EditExternal(g); _drawerRequested = GroupsScreen.IsEditing; OnPropertyChanged(nameof(DrawerOpen)); ReturnToDispatchSilently(); } else Notify.Warn("편집 중인 폼이 있습니다", "저장하거나 취소한 뒤 다시 시도하세요"); }

    public void OnHotKey(string name, bool down)
    {
        switch (name)
        {
            case "ptt": if (down) TalkBar.PttDown(); else TalkBar.PttUp(); break;
            case "answer": if (down && Notify.TopIncoming?.Session is { } inc) { Session.Answer(inc); Screen = AppScreen.Dispatch; } break;
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

    /// <summary>Ctrl+n — 카드 n 포커스 + 발언 대상을 그 채널 하나로.</summary>
    public void SelectChannel(int n) => PttChannels.SelectIndex(n);
    /// <summary>Ctrl+Shift+n — 카드 n 발언 대상 토글.</summary>
    public void ToggleChannel(int n) => PttChannels.ToggleIndex(n);

    [RelayCommand] private void AnswerBanner(Banner b) { if (b.Session is not null) { Session.Answer(b.Session); Screen = AppScreen.Dispatch; } }
    [RelayCommand] private void RejectBanner(Banner b) { if (b.Session is not null) Session.Reject(b.Session); }
    [RelayCommand] private void GoToChannel(Banner b) { Screen = AppScreen.Dispatch; if (b.GroupId.Length > 0) PttChannels.FocusGroup(b.GroupId); }
    [RelayCommand] private void DismissToast(Toast t) => Notify.Dismiss(t);
    [RelayCommand] private void ToggleToastDetail(Toast t) => t.ShowDetail = !t.ShowDetail;

    /// <summary>--ui-preview-canvas: 서버 없이 관제 캔버스에 표본(멤버 그룹 4·청취 범위 2·진행 중 그룹콜·사설콜·애드혹·VoLTE 통화)을 심어 카드 2/3줄·발언 바·② 섹션을 그려 본다.
    /// 코어 세션이 아니라 스냅샷 모델만 채우므로 조작 버튼은 동작하지 않는다.</summary>
    public void SeedCanvasPreview()
    {
        var s = Session;
        CimsUe.CallInfo Ci(int id, CimsUe.CallState st, string remote, bool mcptt, string group, bool priv = false, bool half = true, bool emg = false, bool listen = false, CimsUe.CallDir dir = CimsUe.CallDir.Outgoing) =>
            new(id, 1, dir, st, remote, "", false, true, false, true, 0, 0, "", Array.Empty<CimsUe.MediaSource>(), mcptt, group,
                new CimsUe.McpttInfo(mcptt, priv ? "private" : "prearranged", "", "tel:1001", group, emg, false, priv, !half), half, listen, "");
        GroupInfo G(string id, string name, int members, bool member, params (string, string)[] roster)
        {
            var g = new GroupInfo(id, "tel:" + id, name, members) { IsMember = member, IsOwner = id == "g-ops" };
            g.Roster = roster.Select(r => new CimsUe.RosterEntry(r.Item1, r.Item2)).ToList();
            return g;
        }
        s.Groups.Add(G("g-patrol1", "순찰1", 12, true, ("tel:1001", "connected"), ("tel:1002", "connected"), ("tel:1003", "connected"), ("tel:1004", "connected"), ("tel:1005", "connected"), ("tel:1006", "connected")));
        s.Groups.Add(G("g-ops", "상황실", 8, true, ("tel:1001", "connected"), ("tel:1007", "connected")));
        s.Groups.Add(G("g-traffic", "교통1", 6, true));
        s.Groups.Add(G("g-patrol2", "순찰2", 9, true));
        s.Groups.Add(G("g-night", "야간", 15, false, ("tel:1010", "connected"), ("tel:1011", "connected"), ("tel:1012", "connected")));
        s.Groups.Add(G("g-support", "지원", 7, false, ("tel:1013", "connected"), ("tel:1014", "connected")));
        s.Groups.Add(G("g-guard", "경비", 5, false));
        var now = DateTime.Now;
        var patrol = new SessionItem(Ci(11, CimsUe.CallState.Active, "sip:g-patrol1@ptt", true, "g-patrol1"), AccountKind.Ptt, Operation.PttJoin) { Title = "순찰1", Speaker = "김순경", SpeakerSince = now.AddSeconds(-8), ConnectedAt = now.AddMinutes(-2) };
        var ops = new SessionItem(Ci(12, CimsUe.CallState.Active, "sip:g-ops@ptt", true, "g-ops"), AccountKind.Ptt, Operation.PttJoin) { Title = "상황실", ConnectedAt = now.AddMinutes(-14) };
        var adhoc = new SessionItem(Ci(13, CimsUe.CallState.Active, "sip:adhoc-1002-1@ptt", true, "adhoc-1002-1"), AccountKind.Ptt, Operation.PttAdhoc) { Title = "애드혹", AdhocMembers = new[] { "tel:1003", "tel:1008", "tel:1009" }, Speaker = "최순경", SpeakerSince = now.AddSeconds(-3), ConnectedAt = now.AddSeconds(-25) };
        var priv = new SessionItem(Ci(14, CimsUe.CallState.Active, "tel:1008", true, "", priv: true, half: false), AccountKind.Ptt, Operation.PttPrivate) { Title = "윤순경", ConnectedAt = now.AddMinutes(-1) };
        var night = new SessionItem(Ci(15, CimsUe.CallState.Active, "sip:g-night@ptt", true, "g-night", emg: true, listen: true), AccountKind.Ptt, Operation.PttListen) { Title = "야간", Speaker = "박경장", SpeakerSince = now.AddSeconds(-14), ConnectedAt = now.AddMinutes(-18) };
        var volte = new SessionItem(Ci(16, CimsUe.CallState.Active, "tel:+82233334444", false, "", dir: CimsUe.CallDir.Incoming), AccountKind.Volte, Operation.Incoming) { Title = "02-333-4444", ConnectedAt = now.AddMinutes(-2) };
        var held = new SessionItem(Ci(17, CimsUe.CallState.Held, "tel:1006", false, "", dir: CimsUe.CallDir.Incoming), AccountKind.Volte, Operation.Incoming) { Title = "1006 박경장", ConnectedAt = now.AddMinutes(-5) };
        foreach (var x in new[] { patrol, ops, adhoc, priv, night, volte, held }) { x.Tick(now); s.Sessions.Add(x); }
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Talk, "순찰1 김순경 발언 12초");
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Sds, "순찰1 SDS 박경장", "\"교대 인원 2명…\"");
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Member, "순찰1 정경장 합류", "12명");
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Emergency, "야간 긴급 개시", "박경장", emergency: true);
        s.Activity.Add(ActivityPanel.Call, ActivityKind.Incoming, "착신 7000 ← 010-2222-3333", "응답 1004 · 03:12", number: "+821022223333", pilot: true);
        s.Activity.Add(ActivityPanel.Call, ActivityKind.Missed, "부재 7000 ← 010-7777-8888", "→ 넘김 7100", missed: true, number: "+821077778888", pilot: true);
        RestoreFromSnapshot();
        PttChannels.Select(PttChannels.Cards.FirstOrDefault(), collapseSame: false);
        if (PttChannels.Cards.FirstOrDefault() is { } first) PttChannels.SetSingleTarget(first);
        patrol.Floor = new CimsUe.FloorInfo(CimsUe.FloorState.Speaking, Array.Empty<CimsUe.Talker>(), true, 0, -1, 0, "", 0, 1, 0, 0);
        patrol.Speaker = "나"; patrol.TalkGauge = 0.6;
        PttChannels.Tick(); TalkBar.Refresh(); Scoped.Rebuild();
    }

    public void Tick(DateTime now)
    {
        Session.Tick(now);
        Desk.Tick(now);
        PttChannels.Tick();
        TalkBar.Refresh();
        Scoped.Tick();
        if (Screen == AppScreen.PttGroups || PoppedOut.Contains(AppScreen.PttGroups)) GroupsScreen.Tick();
        PttActivity.Tick();
        CallActivity.Tick();
        CallDesk.Refresh();
        if (Screen != AppScreen.Dispatch || PoppedOut.Count > 0) Summary.Refresh();
    }
}
