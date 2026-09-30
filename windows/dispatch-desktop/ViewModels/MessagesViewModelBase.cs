// 메시지 공통 — 대화 목록(거르기 전체/그룹/1:1/안 읽음) : 대화(말풍선 → 입력). MCData SDS([무전] «메시지»)와 SMS·LMS([통화] «기록»)가 상속한다
// (§4.4, mcdata_messaging.md §5).
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public abstract partial class MessagesViewModelBase : ObservableObject
{
    protected readonly DispatchSession S;
    protected readonly Dictionary<string, MessageThread> ThreadMap = new(StringComparer.OrdinalIgnoreCase);

    public MessageKind Kind { get; }
    public ObservableCollection<MessageThread> Threads { get; } = new();
    /// <summary>대화 목록 — Threads 를 거르기(ThreadFilter)로 본 것. 정렬은 Threads 순(최근 위).</summary>
    public System.ComponentModel.ICollectionView ThreadsView { get; }
    /// <summary>all | group | one | unread</summary>
    [ObservableProperty] private string _threadFilter = "all";
    [ObservableProperty] private MessageThread? _selected;
    [ObservableProperty] private string _input = "";
    [ObservableProperty] private bool _followChannel;

    /// <summary>미읽음 합계 — 레일·세그먼트·[안 읽음 n] 칩.</summary>
    public int UnreadTotal => Threads.Sum(t => t.Unread);
    public string UnreadChipText => UnreadTotal > 0 ? $"안 읽음 {UnreadTotal}" : "안 읽음";

    private bool PassFilter(MessageThread t) => ThreadFilter switch
    {
        "group" => t.IsGroup, "one" => !t.IsGroup, "unread" => t.Unread > 0 || t == Selected, _ => true,
    };
    partial void OnThreadFilterChanged(string value) => ThreadsView.Refresh();
    [RelayCommand] private void SetThreadFilter(string f) => ThreadFilter = f;
    public bool HasSelection => Selected is not null;
    public bool CanSend => Selected is not null && Input.Trim().Length > 0 && SendAllowed(Selected);
    /// <summary>첨부(📎)를 쓰는 패널인가 — MCData FD 만(SMS·LMS 는 첨부 없음).</summary>
    public virtual bool SupportsAttachments => false;
    public bool CanAttach => SupportsAttachments && Selected is not null && SendAllowed(Selected);

    protected MessagesViewModelBase(DispatchSession s, MessageKind kind)
    {
        S = s; Kind = kind;
        _followChannel = s.Settings.Current.FollowChannelThread;
        ThreadsView = new System.Windows.Data.ListCollectionView(Threads) { Filter = o => o is MessageThread t && PassFilter(t) };
        foreach (var m in s.Messages.LoadAll().Where(m => m.Kind == kind)) Put(m, persist: false);
        s.RequestCompleted += (_, r) => OnRequestCompleted(r);
        s.ProfileApplied += (_, _) => Reload();              // 재로그인 — 보관 주인(로그인 ID)이 바뀌었을 수 있다
    }

    /// <summary>보관에서 다시 읽는다 — 지금 주인(로그인 ID)의 스레드만(MessageStore.Owner).</summary>
    private void Reload()
    {
        Selected = null;
        ThreadMap.Clear(); Threads.Clear();
        foreach (var m in S.Messages.LoadAll().Where(m => m.Kind == Kind)) Put(m, persist: false);
        OnPropertyChanged(nameof(UnreadTotal)); OnPropertyChanged(nameof(UnreadChipText));
        RaiseUnread();
    }

    protected abstract bool SendAllowed(MessageThread t);
    /// <summary>선택 대화가 바뀌었다 — 대화 머리(라벨·부제)를 다시 그린다.</summary>
    protected virtual void OnSelectionChanged() { }
    protected abstract void OnRequestCompleted(CimsUe.RequestResult r);

    partial void OnSelectedChanged(MessageThread? value)
    {
        if (value is not null && value.Unread > 0) { value.MarkRead(); S.Messages.MarkRead(value.Key, Kind); OnPropertyChanged(nameof(UnreadTotal)); OnPropertyChanged(nameof(UnreadChipText)); }
        OnPropertyChanged(nameof(HasSelection)); OnPropertyChanged(nameof(CanSend)); OnPropertyChanged(nameof(CanAttach));
        OnSelectionChanged();
    }
    partial void OnInputChanged(string value) => OnPropertyChanged(nameof(CanSend));

    protected MessageThread Thread(string key, string title, bool isGroup, bool isExternal = false)
    {
        if (ThreadMap.TryGetValue(key, out var t)) { if (title.Length > 0 && t.Title != title) t.Title = title; return t; }
        t = new MessageThread(key, Kind, title.Length > 0 ? title : key) { IsGroup = isGroup, IsExternal = isExternal };
        t.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(MessageThread.Unread)) { OnPropertyChanged(nameof(UnreadTotal)); OnPropertyChanged(nameof(UnreadChipText)); RaiseUnread(); if (ThreadFilter == "unread") ThreadsView.Refresh(); } };
        ThreadMap[key] = t;
        Threads.Add(t);
        return t;
    }

    protected virtual string TitleOfKey(string key) => S.Directory.Label(key);

    protected void Put(Message m, bool persist)
    {
        var t = Thread(m.ThreadKey, m.GroupUri.Length > 0 ? S.Groups.FirstOrDefault(g => g.Id == Converters.UserPartConverter.UserPart(m.GroupUri))?.Name ?? "" : TitleOfKey(m.ThreadKey),
                       m.GroupUri.Length > 0, Kind == MessageKind.Sms && S.Directory.IsExternal(m.ThreadKey));
        if (persist) S.Messages.Insert(m);
        if (m == null) return;
        if (Selected == t && m.Direction == MessageDirection.In) { m.Read = true; S.Messages.MarkRead(t.Key, Kind); }
        t.Add(m);
        Sort();
        OnPropertyChanged(nameof(UnreadTotal)); OnPropertyChanged(nameof(UnreadChipText));
    }

    private void Sort()
    {
        var ordered = Threads.OrderByDescending(t => t.LastTime).ToList();
        for (int i = 0; i < ordered.Count; ++i)
            if (Threads.IndexOf(ordered[i]) != i) Threads.Move(Threads.IndexOf(ordered[i]), i);
    }

    public void SelectKey(string key, string title, bool isGroup) => Selected = Thread(key, title, isGroup);

    [RelayCommand] private void SelectThread(MessageThread t) => Selected = t;
    /// <summary>머리 [따라가기] 토글(채널 카드를 누르면 그 채널 대화로) — 설정에 저장.</summary>
    [RelayCommand] private void ToggleFollow() { FollowChannel = !FollowChannel; S.Settings.Update(x => x.FollowChannelThread = FollowChannel); }
    /// <summary>스레드 키의 미읽음(채널 카드 수 배지).</summary>
    public int UnreadOf(string key) => ThreadMap.TryGetValue(key, out var t) ? t.Unread : 0;
    public event EventHandler? UnreadChanged;
    protected void RaiseUnread() => UnreadChanged?.Invoke(this, EventArgs.Empty);
    [RelayCommand] private void Send() => SendCore();
    /// <summary>빠른 답 — 한 번 눌러 곧바로 보낸다(관제사가 가장 자주 치는 말, §4.4).</summary>
    public IReadOnlyList<string> QuickReplies { get; } = new[] { "확인했습니다", "이동 중", "도착했습니다", "대기 바랍니다" };
    [RelayCommand] private void QuickReply(string text) { if (Selected is null || !SendAllowed(Selected)) return; Input = text; SendCore(); }
    [RelayCommand] private void Resend(Message m) => ResendCore(m);
    [RelayCommand] private Task Attach() => AttachCore();
    protected abstract void SendCore();
    protected abstract void ResendCore(Message m);
    protected virtual Task AttachCore() => Task.CompletedTask;

    // ── 첨부 말풍선(받기·열기·폴더) ──
    /// <summary>받은 파일 받기 — 받은 파일 폴더(다운로드\CIMS)에 저장하고 경로를 기록한다. 끝나면 연다.</summary>
    [RelayCommand]
    private async Task DownloadFile(Message m)
    {
        if (!m.CanDownload) return;
        m.TransferNote = "받는 중…";
        var r = await S.DownloadFileAsync(m.FileUrl, m.FileName);
        m.TransferNote = "";
        if (!r.Ok) return;
        m.LocalPath = r.Value;
        S.Messages.UpdateLocalPath(m.Id, r.Value);
        OpenFile(m);
    }
    /// <summary>열기 — 연결된 프로그램(셸). 받지 않은 수신 파일이면 받기부터.</summary>
    [RelayCommand]
    private void OpenFile(Message m)
    {
        if (!m.HasLocalFile) { if (m.CanDownload) _ = DownloadFile(m); return; }
        try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(m.LocalPath) { UseShellExecute = true }); }
        catch (Exception ex) { S.Notify.Error("파일을 열 수 없습니다", ex.Message); }
    }
    /// <summary>탐색기에서 파일 위치 보기.</summary>
    [RelayCommand]
    private void ShowFile(Message m)
    {
        if (!m.HasLocalFile) return;
        try { System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{m.LocalPath}\""); }
        catch (Exception ex) { S.Notify.Error("폴더를 열 수 없습니다", ex.Message); }
    }
}
