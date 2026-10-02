// 메인 — «모드마다 한 화면»(§3): VM 조립 · [무전|통화] 모드 · 오른쪽 패널(채널 상세·사용자·새 그룹·이벤트 상세·주소록) 규칙 · 칸 사이 연동
// (카드 → 메시지 따라가기, 사람 메뉴 → 기록·개별 통화·무전 메시지) · 핫키(§8) · 감청 창(§5, [창으로] 로만) · 레일 화면 전환(§3.4: 관제·이력·PTT 그룹·관리 —
// 화면 VM 은 앱 수명 동안 하나, 전환은 가시성만) · 자동 복귀(세션을 만든 조작은 그 호의 모드로 한 번).
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class MainViewModel : ObservableObject
{
    public DispatchSession Session { get; }
    public Notifications Notify => Session.Notify;
    public LayoutStore Layout { get; }
    public DeskViewModel Desk { get; }
    // [무전]
    public PttChannelsViewModel PttChannels { get; }
    public ScopedChannelsViewModel Scoped { get; }
    public McDataMessagesViewModel McData { get; }
    public PttActivityViewModel PttActivity { get; }
    public PttUsersViewModel Users { get; }
    public TalkBarViewModel TalkBar { get; }
    // [통화]
    public CallDeskViewModel CallDesk { get; }
    public CallOriginateViewModel CallOriginate { get; }
    public CallActivityViewModel CallActivity { get; }
    public SmsMessagesViewModel Sms { get; }
    public CallRecordsViewModel Records { get; }
    // 공통
    public SidePanelViewModel Panel { get; } = new();
    public PersonActionsViewModel People { get; }
    public HotKeyMap HotKeys { get; }

    /// <summary>ptt | call — 탭 줄 [무전|통화] 세그먼트(§3.1). 모드를 바꾸면 고정하지 않은 패널은 닫힌다.</summary>
    [ObservableProperty] private string _mode = "ptt";
    /// <summary>[통화] «통화» 머리 [키패드] 팝오버 — 누를 때만 뜬다.</summary>
    [ObservableProperty] private bool _keypadOpen;

    // ── 레일 화면(§3.4) — 관제 외 셋은 로그인당 1회 적재, 폼 상태는 전환해도 유지 ──
    public SessionHistoryViewModel HistoryScreen { get; }
    public GroupAdminViewModel GroupsScreen { get; }
    public DirectoryAdminViewModel AdminScreen { get; }
    /// <summary>관제 요약 띠(§3.5) — 화면 별창(ScreenWindow)에만. 주 창은 발언 바가 모든 화면에 있어 띠를 두지 않는다.</summary>
    public DispatchSummaryViewModel Summary { get; }
    [ObservableProperty] private AppScreen _screen = AppScreen.Dispatch;
    /// <summary>별창으로 떼어낸 화면 — 주 창 쪽은 자리표시자만 보인다(§3.4).</summary>
    public System.Collections.ObjectModel.ObservableCollection<AppScreen> PoppedOut { get; } = new();
    public event EventHandler<AppScreen>? ScreenPopOutRequested;
    /// <summary>[관제로] — 별창에서 눌렀으면 주 창을 앞으로 가져와야 관제가 보인다. 창 활성화는 MainWindow.</summary>
    public event EventHandler? DispatchActivateRequested;
    private bool _screensLoaded;

    /// <summary>감청 창 열기/활성화·닫기 요청 — 창 관리는 MainWindow. 감청 창은 [창으로] 를 눌렀을 때만 뜬다(기본 표면은 인라인, §5).</summary>
    public event EventHandler<SessionItem>? MonitorWindowActivateRequested;
    public event EventHandler<SessionItem>? MonitorWindowCloseRequested;
    /// <summary>PTT 그룹 삭제 확인 요청 — 대화상자는 MainWindow.</summary>
    public event EventHandler<GroupInfo>? GroupDeleteRequested;
    /// <summary>새 그룹 폼을 [PTT 그룹] 화면으로 넘긴다([고급 설정]) — 패널을 닫아도 폼을 취소하지 않는다.</summary>
    private bool _groupToScreen;
    /// <summary>세션을 만든 조작의 자동 복귀를 한 번만(호마다) — 통화 중에 모드를 바꿔도 다음 상태 변화가 되돌리지 않게.</summary>
    private readonly HashSet<int> _autoShown = new();

    public MainViewModel(DispatchSession session, LayoutStore layout, HotKeyMap hotKeys)
    {
        Session = session;
        Layout = layout;
        HotKeys = hotKeys;
        Desk = new DeskViewModel(session);
        PttChannels = new PttChannelsViewModel(session);
        TalkBar = new TalkBarViewModel(session, PttChannels);
        Users = new PttUsersViewModel(session);
        McData = new McDataMessagesViewModel(session);
        PttActivity = new PttActivityViewModel(session);
        CallDesk = new CallDeskViewModel(session);
        CallOriginate = new CallOriginateViewModel(session);
        Sms = new SmsMessagesViewModel(session);
        Records = new CallRecordsViewModel(session, Sms, CallDesk);
        CallActivity = new CallActivityViewModel(session);
        HistoryScreen = new SessionHistoryViewModel(session);
        GroupsScreen = new GroupAdminViewModel(session);
        AdminScreen = new DirectoryAdminViewModel(session);
        Scoped = new ScopedChannelsViewModel(session, GroupsScreen);
        People = new PersonActionsViewModel(session);
        Summary = new DispatchSummaryViewModel(session, PttChannels, TalkBar, CallDesk, Desk, Sms);

        // 그룹 종류(prearranged|chat)는 GMS 목록에 없다 — 관리 목록이 적재될 때 멤버 그룹에 옮긴다([일제 통화]는 편성 그룹만)
        //   관리 범위로 고칠 수 있는 그룹도 같이 옮긴다 — 내 소유가 아닌 멤버 그룹의 채널 카드에도 ⋮[편집]·[삭제] 가 선다(타 채널 행과 같은 판정)
        GroupsScreen.Loaded += (_, _) =>
        {
            session.NoteGroupTypes(GroupsScreen.All.Select(m => (m.Id, m.SessionType)));
            session.NoteManagedGroups(GroupsScreen.All.Where(m => m.CanManage).Select(m => m.Id));
            foreach (var c in PttChannels.Cards) c.Refresh();
            Panel.Channel?.Refresh(rows: false);
        };
        GroupsScreen.ChannelRequested += (_, id) => FocusChannel(id);   // [채널로] — 채널 상세로(합류하지 않는다)
        GroupsScreen.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(GroupAdminViewModel.IsEditing) && !GroupsScreen.IsEditing) OnGroupFormClosed(); };
        AdminScreen.PropertyChanged += (_, e) => { if (e.PropertyName is nameof(DirectoryAdminViewModel.IsDirty)) OnPropertyChanged(nameof(AdminEditing)); };
        PttActivity.HistoryRequested += (_, _) => ShowHistory("ptt");
        Records.HistoryRequested += (_, _) => ShowHistory("call");
        session.ProfileApplied += (_, _) => { _screensLoaded = false; OnPropertyChanged(nameof(CanManage)); OnPropertyChanged(nameof(ManageHint)); OnPropertyChanged(nameof(ManageTip)); _ = LoadGroupsForScopedAsync(); };

        // ── [무전] 칸 사이 ──
        PttChannels.SelectionChanged += (_, c) => { if (c?.Group is not null) McData.FollowGroup(c.Group); };
        PttChannels.IndexPicked += (_, c) => { if (c.Group is not null) McData.FollowGroup(c.Group); };
        PttChannels.PersonMenuRequested += (_, uri) => People.OpenMenu(uri);
        PttChannels.Cards.CollectionChanged += (_, _) => OnCardsChanged();
        TalkBar.FocusRequested += (_, c) => { ShowDispatch("ptt"); OpenChannel(c, toggle: false); };
        McData.UnreadChanged += (_, _) => { PttChannels.SetUnread(g => McData.UnreadOf(g.Uri)); RaiseBadges(); };
        McData.ChannelInfoRequested += (_, g) => FocusChannel(g.Id);
        McData.NewConversationRequested += (_, _) => OpenUsers();
        PttActivity.ChannelRequested += (_, id) => FocusChannel(id);
        PttActivity.RowRequested += (_, r) => OpenEvent(r);
        Scoped.WindowRequested += (_, s) => MonitorWindowActivateRequested?.Invoke(this, s);
        Users.MenuRequested += (_, n) => People.OpenMenu(n);
        Users.SaveAsGroupRequested += (_, rows) => OpenGroupForm(rows);
        Users.Started += (_, _) => { if (!Panel.Pinned && Panel.IsUsers) Panel.View = PanelView.None; };

        // ── [통화] 칸 사이 ──
        CallDesk.FillRequested += (_, n) => CallOriginate.Fill(n);
        CallDesk.MenuRequested += (_, n) => People.OpenMenu(n);
        CallDesk.Queue.CollectionChanged += (_, _) => RaiseBadges();
        CallOriginate.SmsRequested += (_, n) => OpenRecord(n);
        CallActivity.WindowRequested += (_, s) => MonitorWindowActivateRequested?.Invoke(this, s);
        Records.MenuRequested += (_, n) => People.OpenMenu(n);
        Sms.UnreadChanged += (_, _) => RaiseBadges();
        Desk.MonitorActivateRequested += (_, s) => MonitorWindowActivateRequested?.Invoke(this, s);

        // ── 오른쪽 패널 ──
        Panel.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(SidePanelViewModel.View)) OnPanelViewChanged(); };
        Panel.BackRequested += (_, _) => { if (Panel.IsGroup) Panel.Group?.CancelCommand.Execute(null); };

        // ── 사람 메뉴 · Ctrl+K 행동 ──
        People.PrivateCallRequested += (_, n) => { Users.PrivateCallTo(n); };
        People.AdhocAddRequested += (_, n) => { Users.AddAdhoc(n); ShowDispatch("ptt"); OpenUsers(toggle: false); };
        People.SdsRequested += (_, n) => { ShowDispatch("ptt"); McData.OpenUser(n); };
        People.CallRequested += (_, n) => Session.Dial(n);
        People.SmsRequested += (_, n) => OpenRecord(n);
        People.RecordRequested += (_, n) => OpenRecord(n);
        People.ChannelRequested += (_, id) => FocusChannel(id);
        People.AddMemberRequested += (_, g) => EditGroup(g);

        session.SessionAdded += (_, s) => { Desk.SyncMonitors(session.Sessions); CallOriginate.RefreshPad(); AutoShow(s); RaiseBadges(); };
        session.SessionEnded += (_, s) =>
        {
            if (s.IsListenLeg) MonitorWindowCloseRequested?.Invoke(this, s);
            _autoShown.Remove(s.CallId);
            Desk.SyncMonitors(session.Sessions); CallOriginate.RefreshPad(); RaiseBadges();
            if (Panel.Channel?.Card?.Session == s && Panel.Channel.Card.Group is null && !Panel.Pinned) Panel.View = PanelView.None;   // 끝난 개별·애드혹 카드는 사라진다
        };
        session.SessionChanged += (_, s) => { CallOriginate.RefreshPad(); AutoShow(s); };
        session.RosterChanged += (_, _) => Panel.Channel?.Refresh();
        session.VideoChanged += (_, _) => Panel.Channel?.Refresh(rows: false);        // «영상» 절(§10.3) — 로스터 줄은 그대로
        session.Floor += (_, _) => Panel.Channel?.Refresh();

        // 전역 핫키
        hotKeys.Pressed += (_, e) => OnHotKey(e.Name, down: true);
        hotKeys.Released += (_, e) => OnHotKey(e.Name, down: false);
    }

    /// <summary>재기동·재접속 후 화면 재구성 = 스냅샷 재조회(§11). 감청·청취는 인라인으로 보인다(창은 다시 띄우지 않는다). 화면은 관제 [무전].</summary>
    public void RestoreFromSnapshot()
    {
        PttChannels.Rebuild();
        Scoped.Rebuild();
        PttActivity.Rebuild();
        CallActivity.Rebuild();
        CallDesk.Rebuild();
        Records.Rebuild();
        Desk.SyncMonitors(Session.Sessions);
        Panel.Pinned = false; Panel.View = PanelView.None;
        Screen = AppScreen.Dispatch;
        Mode = "ptt";
        OnPropertyChanged(nameof(CanManage)); OnPropertyChanged(nameof(ManageHint)); OnPropertyChanged(nameof(ManageTip));
        Summary.Refresh();
        RaiseBadges();
    }

    /// <summary>타 채널의 편집 판정([편집]·[삭제])은 [PTT 그룹] 화면과 같은 관리 목록을 쓴다 — 로그인 직후 한 번 적재(화면을 열지 않아도).</summary>
    private async Task LoadGroupsForScopedAsync()
    {
        if (Session.Management is null) return;
        try { await GroupsScreen.LoadAsync(); } catch (Exception ex) { Session.Log.Warn("scoped groups load: " + ex.Message); }
    }

    // ── 모드 · 탭 줄(§3.1) ──
    public bool IsPtt => Mode == "ptt";
    public bool IsCall => Mode == "call";
    public string ModeHint => IsPtt ? "내 채널 · 타 채널 · 메시지 · 이벤트" : "통화 · 기록(통화 + 문자)";
    /// <summary>세그먼트 수 — 무전 = 안 읽은 무전 메시지, 통화 = 대표번호 대기열 + 안 읽은 문자. 0 이면 숨김.</summary>
    public int PttBadge => McData.UnreadTotal;
    public int CallBadge => CallDesk.QueueCount + Sms.UnreadTotal;
    /// <summary>레일 [관제] 수 — 관제 밖 화면에서 둘을 합쳐 보인다.</summary>
    public int DispatchBadge => PttBadge + CallBadge;
    public string ListText => IsPtt ? "사용자" : "주소록";
    /// <summary>탭 줄 목록 버튼 켜짐 — 무전 = 사용자(·새 그룹), 통화 = 주소록.</summary>
    public bool ListOpen => IsPtt ? Panel.IsUsers || Panel.IsGroup : Panel.IsDirectory;
    private void RaiseBadges() { OnPropertyChanged(nameof(PttBadge)); OnPropertyChanged(nameof(CallBadge)); OnPropertyChanged(nameof(DispatchBadge)); }

    partial void OnModeChanged(string value)
    {
        foreach (var p in new[] { nameof(IsPtt), nameof(IsCall), nameof(ModeHint), nameof(ListText), nameof(ListOpen) }) OnPropertyChanged(p);
        if (!Panel.Pinned) Panel.View = PanelView.None;
        KeypadOpen = false;
    }
    [RelayCommand] private void SetMode(string m) { if (m is "ptt" or "call") { if (Screen != AppScreen.Dispatch) Screen = AppScreen.Dispatch; Mode = m; } }

    /// <summary>관제 화면의 그 모드로(레일·모드 둘 다) — 세션을 만든 조작·[채널로]·배너 [응답] 이 쓴다.</summary>
    private void ShowDispatch(string mode) { if (Screen != AppScreen.Dispatch) Screen = AppScreen.Dispatch; if (Mode != mode) Mode = mode; }

    [RelayCommand] private void ToggleList() { if (IsPtt) OpenUsers(); else OpenDirectory(); }
    [RelayCommand] private void ToggleKeypad() => KeypadOpen = !KeypadOpen;

    // ── 오른쪽 패널(§3.6) ──
    /// <summary>[사용자] — 같은 목록이면 닫는다(toggle). 새 그룹 폼에서 누르면 폼을 버리고 닫는다.</summary>
    public void OpenUsers(bool toggle = true)
    {
        if (toggle && (Panel.IsUsers || Panel.IsGroup)) { Panel.View = PanelView.None; return; }
        Panel.UsersCount = Users.TotalCount;
        Panel.Show(PanelView.Users);
    }
    private void OpenDirectory()
    {
        if (Panel.IsDirectory) { Panel.View = PanelView.None; return; }
        Panel.DirectoryCount = CallOriginate.Book.Count;
        Panel.Show(PanelView.Directory);
    }
    [RelayCommand] private void OpenUsersTile() => OpenUsers(toggle: false);

    /// <summary>내 채널 카드 → 채널 상세 + «메시지» 가 그 채널 대화로(따라가기 켬). 같은 카드면 닫는다.</summary>
    public void OpenChannel(ChannelCard c, bool toggle = true)
    {
        if (toggle && Panel.IsChannel && Panel.Channel?.Card == c) { Panel.View = PanelView.None; return; }
        var d = new ChannelDetailViewModel(Session, c);
        Wire(d);
        Panel.Channel = d;
        Panel.Show(PanelView.Channel);
        Scoped.Mark(null);
        PttChannels.Select(c, collapseSame: false);
    }
    /// <summary>타 채널 행 → 채널 상세. 같은 행이면 닫는다.</summary>
    public void OpenOther(ScopedCard o, bool toggle = true)
    {
        if (toggle && Panel.IsChannel && Panel.Channel?.Other?.Id == o.Id) { Panel.View = PanelView.None; return; }
        var d = new ChannelDetailViewModel(Session, o);
        Wire(d);
        Panel.Channel = d;
        Panel.Show(PanelView.Channel);
        PttChannels.Select(null, collapseSame: false);
        Scoped.Mark(o.Id);
    }
    [RelayCommand] private void OpenCard(ChannelCard c) => OpenChannel(c);
    [RelayCommand] private void OpenScoped(ScopedCard o) => OpenOther(o);

    private void Wire(ChannelDetailViewModel d)
    {
        d.MessageRequested += (_, g) => McData.OpenGroup(g);
        d.EditRequested += (_, g) => EditGroup(g);
        d.DeleteRequested += (_, g) => GroupDeleteRequested?.Invoke(this, g);
        d.PersonMenuRequested += (_, uri) => People.OpenMenu(uri);
        d.PrivateCallRequested += (_, n) => Users.PrivateCallTo(n);
        d.SdsRequested += (_, n) => McData.OpenUser(n);
    }

    /// <summary>이벤트 행 → 이벤트 상세. 같은 행이면 닫는다.</summary>
    private void OpenEvent(EventRow r)
    {
        if (Panel.IsEvent && Panel.Event is { } cur && ReferenceEquals(cur.Row.Row, r.Row)) { Panel.View = PanelView.None; return; }
        Panel.Event = new EventDetail(r, PttActivity.Around(r));
        Panel.Show(PanelView.Event);
        PttActivity.Mark(r);
    }
    [RelayCommand] private void EventReply() { if (Panel.Event?.Row.Group is { } g) McData.OpenGroup(g); }
    [RelayCommand] private void EventChannel() { if (Panel.Event?.Row.Group is { } g) FocusChannel(g.Id); }
    [RelayCommand] private void EventHistory() => ShowHistory("ptt");

    /// <summary>[그룹으로 저장 ›] — 고른 사람을 멤버로 한 새 그룹 폼을 패널에(← = 사용자 목록).</summary>
    private void OpenGroupForm(IReadOnlyList<PttUserRow> rows)
    {
        if (GroupsScreen.IsEditing) { Notify.Warn("편집 중인 그룹 폼이 있습니다", "[PTT 그룹] 화면에서 저장하거나 취소한 뒤 다시 시도하세요"); return; }
        var vm = GroupsScreen.NewExternal(rows.Select(r => (r.Number, r.Name)));
        if (vm is null) { Notify.Warn("그룹을 만들 수 없습니다", "PTT 그룹 생성 자격(ptt.allowCreateGroup)이 없습니다"); return; }
        vm.Saved += (_, _) => { Users.ClearPickedCommand.Execute(null); Notify.Info($"PTT 그룹 «{vm.Name.Trim()}» 을 만들었습니다"); };
        Panel.Group = vm;
        Panel.Show(PanelView.Group, fromUsers: true);
    }
    /// <summary>새 그룹 폼 [▸ 고급 설정] — 같은 폼을 [PTT 그룹] 화면에서 이어 쓴다(능력·우선순위·확인 통화·멤버 역할).</summary>
    [RelayCommand] private void GroupAdvanced() { _groupToScreen = true; Panel.View = PanelView.None; ShowScreen(AppScreen.PttGroups); }
    /// <summary>폼이 닫혔다(저장·취소) — 패널에서 열었던 폼이면 사용자 목록으로 돌아간다.</summary>
    private void OnGroupFormClosed()
    {
        _groupToScreen = false;
        if (Panel.IsGroup) { Panel.Group = null; Panel.Show(PanelView.Users); }
    }
    /// <summary>채널 상세 ⋮ [편집]·Ctrl+K [멤버 추가] — 편집 폼은 [PTT 그룹] 화면(§4.7).</summary>
    private void EditGroup(GroupInfo g)
    {
        if (GroupsScreen.IsEditing) { Notify.Warn("편집 중인 그룹 폼이 있습니다", "저장하거나 취소한 뒤 다시 시도하세요"); return; }
        GroupsScreen.EditExternal(g);
        ShowScreen(AppScreen.PttGroups);
    }

    private void OnPanelViewChanged()
    {
        if (!Panel.IsChannel) { Panel.Channel = null; PttChannels.Select(null, collapseSame: false); Scoped.Mark(null); }
        if (!Panel.IsEvent) { Panel.Event = null; PttActivity.Mark(null); }
        if (!Panel.IsGroup && Panel.Group is { } g) { Panel.Group = null; if (!_groupToScreen && GroupsScreen.Editor == g) g.CancelCommand.Execute(null); }
        OnPropertyChanged(nameof(ListOpen));
    }

    /// <summary>내 채널 카드가 바뀌었다(개별·애드혹 종료) — 보던 카드가 사라졌으면 채널 상세를 닫는다.</summary>
    private void OnCardsChanged()
    {
        if (Panel.Channel?.Card is { } c && !PttChannels.Cards.Contains(c))
        {
            var again = c.Group is null ? null : PttChannels.Cards.FirstOrDefault(x => x.Id == c.Id);
            if (again is not null) OpenChannel(again, toggle: false);          // Rebuild — 같은 그룹의 새 카드로
            else if (!Panel.Pinned) Panel.View = PanelView.None;
        }
    }

    /// <summary>[통화] «기록» 에서 그 상대의 통화·문자 한 줄기(사람 메뉴 [문자]·[기록 보기]).</summary>
    public void OpenRecord(string number) { ShowDispatch("call"); Records.Open(number); }

    // ── 레일(§3.4) ──
    public bool CanManage => Session.CanManageDirectory;
    public string ManageHint => CanManage ? "" : "조직/구성원·번호 관리는 관제 역할의 관리 범위(콘솔 관리 > 역할)가 있어야 합니다. PTT 그룹은 내 소유 그룹만 편집합니다.";
    /// <summary>레일 [관리] 툴팁 — 누를 수 없으면 그 이유.</summary>
    public string ManageTip => CanManage ? "관리 F4 — 조직 · 구성원 · 번호" : ManageHint;
    /// <summary>레일 [관리] 점 배지 — 저장하지 않은 관리 폼이 있다(전환을 막지 않는다).</summary>
    public bool AdminEditing => AdminScreen.IsDirty;
    public bool IsDispatch => Screen == AppScreen.Dispatch;
    public bool IsHistory => Screen == AppScreen.History;
    public bool IsPttGroups => Screen == AppScreen.PttGroups;
    public bool IsAdmin => Screen == AppScreen.Admin;
    public string ScreenTitle => AppScreens.Title(Screen);

    partial void OnScreenChanged(AppScreen value)
    {
        foreach (var p in new[] { nameof(IsDispatch), nameof(IsHistory), nameof(IsPttGroups), nameof(IsAdmin), nameof(ScreenTitle) }) OnPropertyChanged(p);
        if (!Panel.Pinned && !(value == AppScreen.PttGroups && _groupToScreen)) Panel.View = PanelView.None;
        if (value != AppScreen.Dispatch) { KeypadOpen = false; _ = LoadScreensAsync(); }
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

    /// <summary>«이벤트»·«기록» 의 [이력에서 보기] — 종류를 맞춰 [이력] 로.</summary>
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

    /// <summary>자동 복귀(§3.4): 세션을 만드는 조작(발신·응답·당겨받기·개별·애드혹 그룹 통화)은 그 호의 모드로 **호마다 한 번** 돌아온다 — 보류·전달·종료 버튼이
    /// 거기 있다. 착신(링잉)·멤버 채널 합류·감청/청취는 배너·행만 바꾸고 화면을 옮기지 않는다.</summary>
    private void AutoShow(SessionItem s)
    {
        if (s.IsListenLeg || s.Kind is SessionKind.PttChannel or SessionKind.McVideo || _autoShown.Contains(s.CallId)) return;   // 영상 호 = 그룹 «영상» 절(자동 합류 포함)
        bool placed = s.Info.Dir == CimsUe.CallDir.Outgoing && (s.IsOutgoing || s.IsActive);   // 발신·당겨받기·개별·애드혹
        bool answered = s.Info.Dir == CimsUe.CallDir.Incoming && s.IsActive;                  // 이 자리에서 받은 착신
        if (!placed && !answered) return;
        _autoShown.Add(s.CallId);
        ShowDispatch(s.IsVolteCall ? "call" : "ptt");
    }

    public void OnHotKey(string name, bool down)
    {
        switch (name)
        {
            case "ptt": if (down) TalkBar.PttDown(); else TalkBar.PttUp(); break;
            case "answer": if (down && Notify.TopIncoming?.Session is { } inc) { Session.Answer(inc); ShowDispatch(inc.IsVolteCall ? "call" : "ptt"); } break;
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

    /// <summary>Ctrl+n — 발언 대상을 카드 n 하나로 + «메시지» 따라가기(채널 상세는 열지 않는다).</summary>
    public void SelectChannel(int n) => PttChannels.SelectIndex(n);
    /// <summary>Ctrl+Shift+n — 카드 n 발언 대상 토글.</summary>
    public void ToggleChannel(int n) => PttChannels.ToggleIndex(n);

    [RelayCommand] private void AnswerBanner(Banner b) { if (b.Session is not null) { Session.Answer(b.Session); ShowDispatch(b.Session.IsVolteCall ? "call" : "ptt"); } }
    [RelayCommand] private void RejectBanner(Banner b) { if (b.Session is not null) Session.Reject(b.Session); }
    [RelayCommand] private void GoToChannel(Banner b) { if (b.GroupId.Length > 0) FocusChannel(b.GroupId); else ShowDispatch("ptt"); }
    /// <summary>배너 [긴급 해제]/[경보 해제] — 세션 조건 하향 또는 경보 취소(§3.2).</summary>
    [RelayCommand] private void CancelBanner(Banner b) => Session.CancelBanner(b);
    /// <summary>경보 배너 [닫기] — 로컬 표시만(취소 신호 유실 대비).</summary>
    [RelayCommand] private void DismissBanner(Banner b) { if (b.IsVideo) Session.Notify.RemoveBanner(b); else Session.DismissAlert(b); }
    /// <summary>«새 영상» 배너 [보기](§10.3) — 그 채널 상세를 열고 그 송출을 본다(보던 것이 있으면 바꿔 본다).</summary>
    [RelayCommand] private void AcceptVideoBanner(Banner b) { string g = b.GroupId; Session.AcceptVideoBanner(b); if (g.Length > 0) FocusChannel(g); }

    /// <summary>[채널로] 공통 — 관제 [무전] 으로 와서 그 채널의 채널 상세를 연다(내 채널 카드, 없으면 타 채널 행). 어느 쪽도 합류시키지 않는다 — 청취 범위 그룹에
    /// sendrecv 로 합류하면 비멤버라 서버가 403 으로 거절한다(TS 24.379 §10.1.1, dispatch_center.md §5.6). 청취는 타 채널의 [청취] 다.</summary>
    public void FocusChannel(string groupId)
    {
        ShowDispatch("ptt");
        if (PttChannels.Cards.FirstOrDefault(c => c.Id == groupId) is { } card) { OpenChannel(card, toggle: false); return; }
        if (Scoped.Mark(groupId) is { } other) { OpenOther(other, toggle: false); return; }
        Notify.Info("채널이 없습니다", $"{groupId} — 멤버 그룹도 청취 범위 그룹도 아닙니다");
    }

    [RelayCommand] private void DismissToast(Toast t) => Notify.Dismiss(t);
    [RelayCommand] private void ToggleToastDetail(Toast t) => t.ShowDetail = !t.ShowDetail;
    [RelayCommand] private void ToggleSearch() => People.SearchOpen = !People.SearchOpen;

    public void Tick(DateTime now)
    {
        Session.Tick(now);
        PttChannels.Tick();
        TalkBar.Refresh();
        Scoped.Tick();
        if (Screen == AppScreen.PttGroups || PoppedOut.Contains(AppScreen.PttGroups)) GroupsScreen.Tick();
        PttActivity.Tick();
        CallActivity.Tick();
        CallDesk.Refresh();
        Records.Tick();
        McData.RefreshHeader();
        Panel.Channel?.Tick();
        if (PoppedOut.Count > 0) Summary.Refresh();
    }
}
