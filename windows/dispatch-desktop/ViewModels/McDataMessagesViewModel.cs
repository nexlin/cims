// [무전] 메시지 — MCData SDS·FD. 대화 목록(300) : 대화(머리 = 그룹 전원 라벨·접속 수·[채널 정보 ›], 빠른 답, 📎·끌어 놓기). 그룹 = groupUri 스레드, 1:1 = 상대 번호 스레드(양방향). disposition 요청은 delivered 자동 회신, 통지 수신 → ✓✓.
// 파일(FD) = 📎·끌어 놓기 → CSC 콘텐츠 서버 업로드 → FD 알림 MESSAGE, 받은 파일은 [받기]로 다운로드\CIMS 에 저장(mcdata_messaging.md §4.5).
// 발신 결과 상관: SendGroupSds 가 (msgId, token) 을 주고 최종 응답은 RequestCompleted(MESSAGE, token) 으로 오므로 token 으로 짝을 맞춘다
// (SMS 와 같은 규칙 — disposition 통지 발신의 완료 이벤트는 어느 메시지에도 맞지 않아 무시된다).
using CimsUe;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class McDataMessagesViewModel : MessagesViewModelBase
{
    public McDataMessagesViewModel(DispatchSession s) : base(s, MessageKind.McData)
    {
        s.SdsReceived += (_, m) => OnSds(m);
        s.Groups.CollectionChanged += (_, _) => { foreach (var g in s.Groups) if (ThreadMap.TryGetValue(g.Uri, out var t)) t.Title = g.Name; };
    }

    /// <summary>그룹 스레드 = 그룹 SDS·FD, 1:1 스레드(키 = 상대 번호) = one-to-one SDS·FD.</summary>
    protected override bool SendAllowed(MessageThread t) => true;
    public override bool SupportsAttachments => true;
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
        // 1:1 스레드 키 = 상대 번호(OpenUser 와 같은 형) — 발신 uri 형이 tel:/sip: 로 달라도 스레드가 갈라지지 않게
        string key = group ? m.GroupUri : UserPartConverter.UserPart(m.FromUri);
        var msgIn = new Message
        {
            Kind = MessageKind.McData, ThreadKey = key, Direction = MessageDirection.In, Peer = m.FromUri, PeerName = S.NameOfPtt(m.FromUri),
            GroupUri = m.GroupUri, ConvId = m.ConvId, MsgId = m.MsgId, Text = m.Text,
            Time = m.TimeSec > 0 ? DateTimeOffset.FromUnixTimeSeconds(m.TimeSec).LocalDateTime : DateTime.Now,
            FileName = m.FileName, FileUrl = m.FileUrl, FileSize = m.FileSize, FileType = m.FileType,
        };
        Put(msgIn, persist: true);
        string gname = S.Groups.FirstOrDefault(g => g.Uri == m.GroupUri || g.Id == UserPartConverter.UserPart(m.GroupUri))?.Name ?? UserPartConverter.UserPart(m.GroupUri);
        S.Activity.Add(ActivityPanel.Ptt, ActivityKind.Sds, $"{(group ? gname : "1:1")} SDS {S.NameOfPtt(m.FromUri)}", Trim(m.Text.Length > 0 ? m.Text : m.FileName));
        if (m.DispositionReq is 1 or 3) S.SendSdsNotification(m.FromUri, m.ConvId, m.MsgId, 2, m.GroupUri);   // 그룹 SDS 면 mcdata-calling-group-id
    }

    private static string Trim(string t) => t.Length > 40 ? "\"" + t[..39] + "…\"" : "\"" + t + "\"";

    protected override void SendCore()
    {
        if (!CanSend || Selected is null) return;
        string text = Input.Trim();
        var t = Selected;
        var r = t.IsGroup ? S.SendGroupSds(UserPartConverter.UserPart(t.Key), text) : S.SendSds(t.Key, text);
        var msg = NewOut(t, text: text);
        msg.MsgId = r.Ok ? r.Value.MsgId : ""; msg.Token = r.Ok ? r.Value.Token : 0; msg.State = r.Ok ? SendState.Pending : SendState.Failed;
        Put(msg, persist: true);
        Input = "";
    }

    /// <summary>스레드의 발신 메시지 — 그룹이면 GroupUri, 1:1 이면 Peer(상대 번호).</summary>
    private static Message NewOut(MessageThread t, string text = "", string fileName = "", long fileSize = 0, string fileType = "", string localPath = "") => new()
    {
        Kind = MessageKind.McData, ThreadKey = t.Key, Direction = MessageDirection.Out,
        Peer = t.IsGroup ? "" : t.Key, GroupUri = t.IsGroup ? t.Key : "", Read = true,
        Text = text, FileName = fileName, FileSize = fileSize, FileType = fileType, LocalPath = localPath,
    };

    protected override void ResendCore(Message m)
    {
        if (m.State != SendState.Failed || !m.IsOut) return;
        if (m.IsAttachment) { _ = SendFileCore(m); return; }
        // 처음의 message ID 로 다시 보낸다(TS 24.282 SDS SIGNALLING PAYLOAD 의 Message ID) — 앞 발신이 일부에게 닿았어도 받는 쪽이 같은 메시지로 대조하고
        //   ✓✓ disposition 도 같은 ID 로 맞물린다. 앞 발신이 즉시 실패해 ID 가 없으면 새로.
        string? again = m.MsgId.Length > 0 ? m.MsgId : null;
        var r = m.GroupUri.Length > 0 ? S.SendGroupSds(UserPartConverter.UserPart(m.GroupUri), m.Text, again) : S.SendSds(m.ThreadKey, m.Text, again);
        if (r.Ok) { m.MsgId = r.Value.MsgId; m.Token = r.Value.Token; }
        m.State = r.Ok ? SendState.Pending : SendState.Failed;
        S.Messages.UpdateResend(m.Id, m.MsgId, m.Token, m.State);
    }

    // ── 파일(MCData FD, mcdata_messaging.md §4.5) — 업로드 → FD 알림. 그룹은 서버가 allow_fd·멤버십으로 게이트 ──
    protected override async Task AttachCore()
    {
        if (!CanAttach || Selected is null) return;
        var dlg = new Microsoft.Win32.OpenFileDialog { Title = $"{Selected.Title} — 보낼 파일", Filter = "모든 파일|*.*", Multiselect = true };
        if (dlg.ShowDialog() != true) return;
        var t = Selected;
        foreach (var path in dlg.FileNames) await SendFileAsync(t, path);
    }

    /// <summary>파일 하나를 스레드로 보낸다(📎·끌어 놓기 공용). 말풍선을 먼저 세우고(올리는 중…) 업로드가 끝나면 FD 알림을 보낸다.</summary>
    public async Task SendFileAsync(MessageThread t, string path)
    {
        System.IO.FileInfo fi;
        try { fi = new System.IO.FileInfo(path); } catch (Exception ex) { S.Notify.Error("파일을 읽을 수 없습니다", ex.Message); return; }
        if (!fi.Exists) return;
        if (fi.Length == 0) { S.Notify.Info("빈 파일은 보낼 수 없습니다", fi.Name); return; }
        if (fi.Length > DispatchSession.MaxFileBytes)
        { S.Notify.Error($"파일이 너무 큽니다 — 최대 {DispatchSession.MaxFileBytes / (1024 * 1024)} MB", $"{fi.Name} · {fi.Length / (1024.0 * 1024):0.#} MB"); return; }
        var msg = NewOut(t, fileName: fi.Name, fileSize: fi.Length, fileType: AppPaths.MimeOf(path), localPath: fi.FullName);
        msg.State = SendState.Pending;
        Put(msg, persist: true);
        await SendFileCore(msg);
    }

    /// <summary>업로드(FILEURL 이 아직 없으면) → FD 알림 발신. 재전송도 여기 — 이미 올린 파일은 알림만 다시 보낸다.</summary>
    private async Task SendFileCore(Message m)
    {
        bool group = m.GroupUri.Length > 0;
        string target = group ? UserPartConverter.UserPart(m.GroupUri) : m.ThreadKey;
        m.State = SendState.Pending;
        if (m.FileUrl.Length == 0)
        {
            if (!m.HasLocalFile) { Fail(m, "원본 파일이 없어 다시 보낼 수 없습니다", m.LocalPath); return; }
            m.TransferNote = "올리는 중…";
            byte[] data;
            try { data = await System.IO.File.ReadAllBytesAsync(m.LocalPath); }
            catch (Exception ex) when (ex is System.IO.IOException or UnauthorizedAccessException) { m.TransferNote = ""; Fail(m, "파일을 읽을 수 없습니다", ex.Message); return; }
            var up = await S.UploadFileAsync(data, m.FileName, m.FileType, group ? target : null);
            m.TransferNote = "";
            if (!up.Ok) { Fail(m, null, null); return; }
            m.FileUrl = up.Value.Url;
        }
        var r = group ? S.SendGroupFd(target, new FdFile(m.FileUrl, m.FileName, m.FileType, m.FileSize))
                      : S.SendFd(target, new FdFile(m.FileUrl, m.FileName, m.FileType, m.FileSize));
        if (r.Ok) { m.MsgId = r.Value.MsgId; m.Token = r.Value.Token; }
        m.State = r.Ok ? SendState.Pending : SendState.Failed;
        S.Messages.UpdateFile(m.Id, m.FileUrl, m.MsgId, m.Token, m.State);
    }

    private void Fail(Message m, string? title, string? detail)
    {
        m.State = SendState.Failed;
        S.Messages.UpdateState(m.Id, SendState.Failed);
        if (title is not null) S.Notify.Error(title, detail ?? "");
    }

    protected override void OnRequestCompleted(RequestResult r)
    {
        // 큰 그룹 SDS 는 코어가 media plane(MSRP, TS 24.282 §9.2.3)으로 보내 최종 결과가 method "MSRP" 로 온다 — token 상관은 같다
        if (r.Method is not ("MESSAGE" or "MSRP") || S.Ptt is null || r.AccountId != S.Ptt.Id) return;
        var m = ThreadMap.Values.SelectMany(t => t.Messages).FirstOrDefault(x => x.IsOut && x.Token == r.Token && x.State == SendState.Pending);
        if (m is null) return;                                                  // 통지 발신 등 내 메시지가 아닌 MESSAGE 완료
        bool ok = r.Code is >= 200 and < 300;
        m.State = ok ? SendState.Sent : SendState.Failed;
        S.Messages.UpdateState(m.Id, m.State);
        if (!ok) S.Notify.Error(ResponseText.Describe(ResponseText.Area.Sds, r.Code, r.Reason), $"{r.Code} {r.Reason}");
    }

    /// <summary>채널 카드를 눌렀다 → 그 그룹 대화로(설정 FollowChannelThread — 머리 [따라가기]).</summary>
    public void FollowGroup(GroupInfo g) { if (FollowChannel) SelectKey(g.Uri, g.Name, true); }
    public void OpenGroup(GroupInfo g) => SelectKey(g.Uri, g.Name, true);

    // ── 대화 머리(§4.4) — 그룹 = «그룹 전원 · 편성 n» 라벨 + «접속 n» + [채널 정보 ›], 1:1 = «1:1» ──
    public GroupInfo? SelectedGroup => Selected is { IsGroup: true } t
        ? S.Groups.FirstOrDefault(g => string.Equals(g.Uri, t.Key, StringComparison.OrdinalIgnoreCase) || g.Id == UserPartConverter.UserPart(t.Key)) : null;
    public bool IsGroupConv => Selected?.IsGroup == true;
    public string ConvLabel => IsGroupConv ? $"그룹 전원 · 편성 {SelectedGroup?.MemberCount ?? 0}" : "1:1";
    public string ConvSub => IsGroupConv ? (SelectedGroup is { } g ? $"접속 {g.ConnectedCount}" : "") : Selected is null ? "" : "PTT " + S.Directory.DisplayNumber(Selected.Key);
    public string InputHint => Selected is null ? "대화를 고르세요" : IsGroupConv ? $"그룹 전원에게 ({Selected.Title} · {SelectedGroup?.MemberCount ?? 0}명)" : $"이 사람에게 ({Selected.Title})";
    /// <summary>[채널 정보 ›] → 오른쪽 채널 상세.</summary>
    public event EventHandler<GroupInfo>? ChannelInfoRequested;
    /// <summary>[＋ 새 대화] → 오른쪽 [사용자] 목록(사람 메뉴 [무전 메시지]).</summary>
    public event EventHandler? NewConversationRequested;
    [CommunityToolkit.Mvvm.Input.RelayCommand] private void OpenChannelInfo() { if (SelectedGroup is { } g) ChannelInfoRequested?.Invoke(this, g); }
    [CommunityToolkit.Mvvm.Input.RelayCommand] private void NewConversation() => NewConversationRequested?.Invoke(this, EventArgs.Empty);
    protected override void OnSelectionChanged() => RefreshHeader();
    public void RefreshHeader() { foreach (var p in new[] { nameof(SelectedGroup), nameof(IsGroupConv), nameof(ConvLabel), nameof(ConvSub), nameof(InputHint) }) OnPropertyChanged(p); }

    /// <summary>--ui-preview-canvas 표본 — 저장하지 않는 말풍선(글 · 받은 파일 · 올리는 중인 파일).</summary>
    public void SeedPreview(GroupInfo g)
    {
        var now = DateTime.Now;
        Put(new Message { Kind = MessageKind.McData, ThreadKey = g.Uri, Direction = MessageDirection.In, Peer = "tel:1004", PeerName = "박경장", GroupUri = g.Uri,
                          Text = "교대 인원 2명 추가 배치 바랍니다", Time = now.AddMinutes(-3), Read = true }, persist: false);
        Put(new Message { Kind = MessageKind.McData, ThreadKey = g.Uri, Direction = MessageDirection.In, Peer = "tel:1003", PeerName = "이순경", GroupUri = g.Uri,
                          FileName = "현장사진_01.jpg", FileUrl = "https://csc/mcdata/fd/0", FileSize = 1258291, FileType = "image/jpeg", Time = now.AddMinutes(-2), Read = true }, persist: false);
        // 긴 글 — 말풍선 줄바꿈·최대 폭 점검용
        Put(new Message { Kind = MessageKind.McData, ThreadKey = g.Uri, Direction = MessageDirection.In, Peer = "tel:1008", PeerName = "윤순경", GroupUri = g.Uri,
                          Text = "3번 게이트 앞 차량 정체가 심합니다. 우회로(북문 → 순환도로)로 진입하도록 안내 중이며, 10분 뒤 다시 상황 보고하겠습니다.",
                          Time = now.AddMinutes(-2), Read = true }, persist: false);
        Put(new Message { Kind = MessageKind.McData, ThreadKey = g.Uri, Direction = MessageDirection.Out, GroupUri = g.Uri, FileName = "순찰 구역 변경.pdf", FileSize = 348160,
                          FileType = "application/pdf", Time = now.AddMinutes(-1), State = SendState.Pending, TransferNote = "올리는 중…", Read = true }, persist: false);
        SelectKey(g.Uri, g.Name, true);
    }
    public void OpenUser(string number) => SelectKey(UserPartConverter.UserPart(number), S.NameOfPtt(number), false);
}
