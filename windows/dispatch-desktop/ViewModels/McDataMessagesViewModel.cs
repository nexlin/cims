// ① 오른쪽 아래 — MCData 메시지(SDS). 그룹 = groupUri 스레드, 1:1 = 발신자 스레드. disposition 요청은 delivered 자동 회신, 통지 수신 → ✓✓.
// 발신 결과 상관: SendGroupSds 는 msgId 를 주고 최종 응답은 RequestCompleted(MESSAGE, token) 로 오므로 발신 순서 큐로 짝을 맞춘다.
using CimsUe;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed class McDataMessagesViewModel : MessagesViewModelBase
{
    private readonly Queue<Message> _pendingSends = new();

    public McDataMessagesViewModel(DispatchSession s) : base(s, MessageKind.McData)
    {
        s.SdsReceived += (_, m) => OnSds(m);
        s.Groups.CollectionChanged += (_, _) => { foreach (var g in s.Groups) if (ThreadMap.TryGetValue(g.Uri, out var t)) t.Title = g.Name; };
    }

    protected override bool SendAllowed(MessageThread t) => t.IsGroup;     // 1:1 SDS 발신 API 는 코어 후속(§13) — 수신·표시만
    protected override string TitleOfKey(string key) => S.NameOfPtt(key);

    private void OnSds(SdsMessage m)
    {
        if (m.Notification)
        {
            var msg = ThreadMap.Values.SelectMany(t => t.Messages).FirstOrDefault(x => x.MsgId == m.MsgId && x.IsOut);
            if (msg is not null && m.NotifType is 2 or 3 or 4) { msg.State = SendState.Delivered; S.Messages.UpdateState(msg.Id, SendState.Delivered); }
            return;
        }
        bool group = m.GroupUri.Length > 0;
        string key = group ? m.GroupUri : m.FromUri;
        var msgIn = new Message
        {
            Kind = MessageKind.McData, ThreadKey = key, Direction = MessageDirection.In, Peer = m.FromUri, PeerName = S.NameOfPtt(m.FromUri),
            GroupUri = m.GroupUri, ConvId = m.ConvId, MsgId = m.MsgId, Text = m.Text,
            Time = m.TimeSec > 0 ? DateTimeOffset.FromUnixTimeSeconds(m.TimeSec).LocalDateTime : DateTime.Now,
            FileName = m.FileName, FileUrl = m.FileUrl, FileSize = m.FileSize,
        };
        Put(msgIn, persist: true);
        string gname = S.Groups.FirstOrDefault(g => g.Uri == m.GroupUri || g.Id == UserPartConverter.UserPart(m.GroupUri))?.Name ?? UserPartConverter.UserPart(m.GroupUri);
        S.Activity.Add(ActivityPanel.Ptt, ActivityKind.Sds, $"{(group ? gname : "1:1")} SDS {S.NameOfPtt(m.FromUri)}", Trim(m.Text.Length > 0 ? m.Text : m.FileName));
        if (m.DispositionReq is 1 or 3) S.SendSdsNotification(m.FromUri, m.ConvId, m.MsgId, 2);
    }

    private static string Trim(string t) => t.Length > 40 ? "\"" + t[..39] + "…\"" : "\"" + t + "\"";

    protected override void SendCore()
    {
        if (!CanSend || Selected is null) return;
        string text = Input.Trim();
        string groupId = UserPartConverter.UserPart(Selected.Key);
        var r = S.SendGroupSds(groupId, text);
        var msg = new Message
        {
            Kind = MessageKind.McData, ThreadKey = Selected.Key, Direction = MessageDirection.Out, Peer = "", GroupUri = Selected.Key,
            MsgId = r.Ok ? r.Value : "", Text = text, State = r.Ok ? SendState.Pending : SendState.Failed, Read = true,
        };
        Put(msg, persist: true);
        if (r.Ok) _pendingSends.Enqueue(msg);
        Input = "";
    }

    protected override void ResendCore(Message m)
    {
        if (m.State != SendState.Failed || !m.IsOut) return;
        var r = S.SendGroupSds(UserPartConverter.UserPart(m.GroupUri), m.Text);
        m.State = r.Ok ? SendState.Pending : SendState.Failed;
        S.Messages.UpdateState(m.Id, m.State);
        if (r.Ok) _pendingSends.Enqueue(m);
    }

    protected override void OnRequestCompleted(RequestResult r)
    {
        if (r.Method != "MESSAGE" || S.Ptt is null || r.AccountId != S.Ptt.Id) return;
        if (_pendingSends.Count == 0) return;
        var m = _pendingSends.Dequeue();
        bool ok = r.Code is >= 200 and < 300;
        m.State = ok ? SendState.Sent : SendState.Failed;
        S.Messages.UpdateState(m.Id, m.State);
        if (!ok) S.Notify.Error(ResponseText.Describe(ResponseText.Area.Sds, r.Code, r.Reason), $"{r.Code} {r.Reason}");
    }

    /// <summary>채널 카드 선택 → 그 그룹 스레드(설정 FollowChannelThread).</summary>
    public void FollowGroup(GroupInfo g) { if (FollowChannel) SelectKey(g.Uri, g.Name, true); }
    public void OpenGroup(GroupInfo g) => SelectKey(g.Uri, g.Name, true);
    public void OpenUser(string number) => SelectKey(number, S.NameOfPtt(number), false);
}
