// 메시지 모델 — MCData SDS([무전] «메시지»)·SMS/LMS([통화] «기록») 공용. 스레드 키 규칙: 그룹 = groupUri, 1:1 = 상대(mcdata_messaging.md §5 threadKeyOf).
using CommunityToolkit.Mvvm.ComponentModel;

namespace DispatchDesktop.Models;

public enum MessageKind { McData, Sms }
public enum MessageDirection { In, Out }
/// <summary>발신 상태 말풍선: 보내는 중(Pending) → ✓ Sent → ✓✓ Delivered / 실패 Failed([재전송]).</summary>
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
    /// <summary>MCData 메시지 id — 재전송하면 새 id 를 받으므로 갱신된다(disposition 통지 상관).</summary>
    public string MsgId { get; set; } = "";
    /// <summary>sendRequest token(SMS·SDS 공통) — 최종 응답 RequestCompleted 상관.</summary>
    public long Token { get; set; }
    public string Text { get; init; } = "";
    public DateTime Time { get; init; } = DateTime.Now;
    [ObservableProperty] private SendState _state;
    [ObservableProperty] private bool _read;
    // ── MCData FD 첨부(mcdata_messaging.md §4.5·§5) ──
    public string FileName { get; init; } = "";
    /// <summary>FILEURL — 수신은 FD 알림 값, 발신은 업로드가 끝난 뒤 채워진다(빈 값 = 아직 안 올라감 → 재전송이 업로드부터).</summary>
    public string FileUrl { get; set; } = "";
    public long FileSize { get; init; }
    public string FileType { get; init; } = "";
    /// <summary>이 PC 의 파일 — 발신 원본 또는 받은 파일. 비면 아직 안 받음(수신).</summary>
    [ObservableProperty] private string _localPath = "";
    /// <summary>진행 문구("올리는 중…"·"받는 중…") — 비면 진행 없음.</summary>
    [ObservableProperty] private string _transferNote = "";
    public bool IsOut => Direction == MessageDirection.Out;
    public bool IsAttachment => FileName.Length > 0 || FileUrl.Length > 0;
    public bool HasLocalFile => LocalPath.Length > 0 && System.IO.File.Exists(LocalPath);
    public bool IsTransferring => TransferNote.Length > 0;
    public bool CanDownload => IsAttachment && !IsOut && !HasLocalFile && !IsTransferring && FileUrl.Length > 0;
    public string FileSizeText => FileSize <= 0 ? "" : FileSize < 1024 ? $"{FileSize} B" : FileSize < 1024 * 1024 ? $"{FileSize / 1024.0:0.#} KB" : $"{FileSize / (1024.0 * 1024):0.#} MB";
    /// <summary>보낸 말풍선 아래 상태 — 보내는 중 · ✓(서버 수락) · ✓✓(전달 확인) · 실패([재전송]).</summary>
    public string StateMark => State switch
    {
        SendState.Pending => "· 보내는 중", SendState.Sent => "✓", SendState.Delivered => "✓✓", SendState.Failed => "· 실패", _ => "",
    };
    partial void OnStateChanged(SendState value) => OnPropertyChanged(nameof(StateMark));
    partial void OnLocalPathChanged(string value) { OnPropertyChanged(nameof(HasLocalFile)); OnPropertyChanged(nameof(CanDownload)); }
    partial void OnTransferNoteChanged(string value) { OnPropertyChanged(nameof(IsTransferring)); OnPropertyChanged(nameof(CanDownload)); }
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

    /// <summary>대화 목록 줄(§4.4) — 아바타 첫 글자 · 종류 라벨 · 마지막 말 미리 보기(«나: …» / «박경장: …» / 파일 이름) · 시각.</summary>
    public string Initial => Title.Trim().Length > 0 ? Title.Trim()[..1] : "?";
    public string KindText => IsGroup ? "그룹" : "1:1";
    public string LastPreview
    {
        get
        {
            var m = Messages.LastOrDefault();
            if (m is null) return "";
            string body = m.Text.Length > 0 ? m.Text : m.IsAttachment ? "파일 " + m.FileName : "";
            string who = m.IsOut ? "나" : IsGroup ? m.PeerName : "";
            return who.Length > 0 ? $"{who}: {body}" : body;
        }
    }
    public string LastTimeText => LastTime == default ? "" : LastTime.Date == DateTime.Today ? LastTime.ToString("HH:mm") : LastTime.ToString("M/d");
    public bool HasUnread => Unread > 0;
    partial void OnTitleChanged(string value) => OnPropertyChanged(nameof(Initial));
    partial void OnUnreadChanged(int value) => OnPropertyChanged(nameof(HasUnread));
    partial void OnLastTimeChanged(DateTime value) => OnPropertyChanged(nameof(LastTimeText));

    public void Add(Message m)
    {
        int i = Messages.Count;
        while (i > 0 && Messages[i - 1].Time > m.Time) --i;
        Messages.Insert(i, m);
        if (m.Time > LastTime) LastTime = m.Time;
        if (!m.Read && m.Direction == MessageDirection.In) Unread++;
        OnPropertyChanged(nameof(LastPreview));
    }

    public void MarkRead()
    {
        foreach (var m in Messages) m.Read = true;
        Unread = 0;
    }
}
