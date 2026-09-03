// 메시지 패널 공통 — 스레드 칩 → 말풍선 → 입력. MCData SDS(①)와 SMS·LMS(③)가 상속한다 (§4.1·§4.3, mcdata_messaging.md §5).
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
    [ObservableProperty] private MessageThread? _selected;
    [ObservableProperty] private string _input = "";
    [ObservableProperty] private bool _followChannel;

    /// <summary>패널 머리 배지 — 미읽음 합계.</summary>
    public int UnreadTotal => Threads.Sum(t => t.Unread);
    public bool HasSelection => Selected is not null;
    public bool CanSend => Selected is not null && Input.Trim().Length > 0 && SendAllowed(Selected);

    protected MessagesViewModelBase(DispatchSession s, MessageKind kind)
    {
        S = s; Kind = kind;
        _followChannel = s.Settings.Current.FollowChannelThread;
        foreach (var m in s.Messages.LoadAll().Where(m => m.Kind == kind)) Put(m, persist: false);
        s.RequestCompleted += (_, r) => OnRequestCompleted(r);
    }

    protected abstract bool SendAllowed(MessageThread t);
    protected abstract void OnRequestCompleted(CimsUe.RequestResult r);

    partial void OnSelectedChanged(MessageThread? value)
    {
        if (value is not null && value.Unread > 0) { value.MarkRead(); S.Messages.MarkRead(value.Key, Kind); OnPropertyChanged(nameof(UnreadTotal)); }
        OnPropertyChanged(nameof(HasSelection)); OnPropertyChanged(nameof(CanSend));
    }
    partial void OnInputChanged(string value) => OnPropertyChanged(nameof(CanSend));

    protected MessageThread Thread(string key, string title, bool isGroup, bool isExternal = false)
    {
        if (ThreadMap.TryGetValue(key, out var t)) { if (title.Length > 0 && t.Title != title) t.Title = title; return t; }
        t = new MessageThread(key, Kind, title.Length > 0 ? title : key) { IsGroup = isGroup, IsExternal = isExternal };
        t.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(MessageThread.Unread)) OnPropertyChanged(nameof(UnreadTotal)); };
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
        OnPropertyChanged(nameof(UnreadTotal));
    }

    private void Sort()
    {
        var ordered = Threads.OrderByDescending(t => t.LastTime).ToList();
        for (int i = 0; i < ordered.Count; ++i)
            if (Threads.IndexOf(ordered[i]) != i) Threads.Move(Threads.IndexOf(ordered[i]), i);
    }

    public void SelectKey(string key, string title, bool isGroup) => Selected = Thread(key, title, isGroup);

    [RelayCommand] private void SelectThread(MessageThread t) => Selected = t;
    [RelayCommand] private void Send() => SendCore();
    [RelayCommand] private void Resend(Message m) => ResendCore(m);
    protected abstract void SendCore();
    protected abstract void ResendCore(Message m);
}
