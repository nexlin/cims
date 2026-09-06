// 메인 — 패널 ViewModel 조립·패널 간 연동(발신 필드 채움·스레드 따라가기·[채널] 포커스)·핫키(§8)·감청 창 관리(§5).
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

        session.SessionAdded += (_, s) => { if (s.IsWindow) MonitorWindowRequested?.Invoke(this, s); Desk.SyncMonitors(session.Sessions); CallOriginate.RefreshPad(); };
        session.SessionEnded += (_, s) => { if (s.IsWindow) MonitorWindowCloseRequested?.Invoke(this, s); Desk.SyncMonitors(session.Sessions); CallOriginate.RefreshPad(); };
        session.SessionChanged += (_, _) => CallOriginate.RefreshPad();

        // 전역 핫키
        hotKeys.Pressed += (_, e) => OnHotKey(e.Name, down: true);
        hotKeys.Released += (_, e) => OnHotKey(e.Name, down: false);
    }

    /// <summary>재기동·재접속 후 화면 재구성 = 스냅샷 재조회(§11). 열린 감청 창도 listenOnly 호에서 복원.</summary>
    public void RestoreFromSnapshot()
    {
        PttChannels.Rebuild();
        PttActivity.Rebuild();
        CallActivity.Rebuild();
        CallDesk.Refresh();
        foreach (var s in Session.Sessions.Where(x => x.IsWindow)) MonitorWindowRequested?.Invoke(this, s);
        Desk.SyncMonitors(Session.Sessions);
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

    [RelayCommand] private void AnswerBanner(Banner b) { if (b.Session is not null) Session.Answer(b.Session); }
    [RelayCommand] private void RejectBanner(Banner b) { if (b.Session is not null) Session.Reject(b.Session); }
    [RelayCommand] private void GoToChannel(Banner b) { if (b.GroupId.Length > 0) PttChannels.FocusGroup(b.GroupId); }
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
    }
}
