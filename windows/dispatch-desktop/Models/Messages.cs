// 메시지 모델 — MCData SDS(①)·SMS/LMS(③) 공용. 스레드 키 규칙: 그룹 = groupUri, 1:1 = 상대(mcdata_messaging.md §5 threadKeyOf).
using CommunityToolkit.Mvvm.ComponentModel;

namespace DispatchDesktop.Models;

public enum MessageKind { McData, Sms }
public enum MessageDirection { In, Out }
/// <summary>발신 상태 말풍선: 🕓 Pending → ✓ Sent → ✓✓ Delivered / ⚠ Failed(재전송).</summary>
public enum SendState { None, Pending, Sent, Delivered, Failed }

public sealed partial class Message : ObservableObject
{
    public long Id { get; set; }
    public MessageKind Kind { get; init; }
    public string ThreadKey { get; init; } = "";
    public MessageDirection Direction { get; init; }
    /// <summary>상대(1:1) 또는 그룹 수신 시 발신자.</summary>
    public string Peer { get; init; } = "";
    public string PeerName { get; set; } = "";
    public string GroupUri { get; init; } = "";
    public string ConvId { get; init; } = "";
    public string MsgId { get; init; } = "";
    /// <summary>sendRequest token(SMS) — 최종 응답 상관.</summary>
    public long Token { get; set; }
    public string Text { get; init; } = "";
    public DateTime Time { get; init; } = DateTime.Now;
    [ObservableProperty] private SendState _state;
    [ObservableProperty] private bool _read;
    public string FileName { get; init; } = "";
    public string FileUrl { get; init; } = "";
    public long FileSize { get; init; }
    public bool IsOut => Direction == MessageDirection.Out;
    public bool IsAttachment => FileName.Length > 0 || FileUrl.Length > 0;
    public string StateMark => State switch
    {
        SendState.Pending => "🕓", SendState.Sent => "✓", SendState.Delivered => "✓✓", SendState.Failed => "⚠ 재전송", _ => "",
    };
    partial void OnStateChanged(SendState value) => OnPropertyChanged(nameof(StateMark));
}

public sealed partial class MessageThread : ObservableObject
{
    public string Key { get; }
    public MessageKind Kind { get; }
    [ObservableProperty] private string _title;
    /// <summary>그룹 스레드(MCData)인가.</summary>
    public bool IsGroup { get; init; }
    /// <summary>외부망 번호(SMS 게이트웨이 없음 → 전송 비활성).</summary>
    public bool IsExternal { get; init; }
    [ObservableProperty] private int _unread;
    [ObservableProperty] private DateTime _lastTime;
    public System.Collections.ObjectModel.ObservableCollection<Message> Messages { get; } = new();

    public MessageThread(string key, MessageKind kind, string title) { Key = key; Kind = kind; _title = title; }

    public void Add(Message m)
    {
        int i = Messages.Count;
        while (i > 0 && Messages[i - 1].Time > m.Time) --i;
        Messages.Insert(i, m);
        if (m.Time > LastTime) LastTime = m.Time;
        if (!m.Read && m.Direction == MessageDirection.In) Unread++;
    }

    public void MarkRead()
    {
        foreach (var m in Messages) m.Read = true;
        Unread = 0;
    }
}
