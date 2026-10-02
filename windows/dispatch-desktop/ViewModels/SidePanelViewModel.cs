// 오른쪽 패널 440(§3.6) — 한 대상을 자세히 보거나 목록에서 사람을 찾는 자리 하나. 왼쪽 칸 1040 은 움직이지 않고 오른쪽 칸만 민다.
// 내용 = 채널 상세(내 채널·타 채널) · 사용자(무전) · 새 PTT 그룹 · 이벤트 상세 · 주소록(통화). 규칙: 같은 대상 = 닫기 · 다른 대상 = 교체 ·
// 고정(핀)하면 모드·레일을 옮겨도 남는다 · 패널 안에서 들어간 것(사용자 › 새 그룹)만 ← 로 돌아간다.
using System.Collections.ObjectModel;
using CimsUe;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public enum PanelView { None, Channel, Users, Group, Event, Directory }

/// <summary>채널 상세의 사람 줄 — 접속 로스터 또는 편성 멤버(GMS 그룹 문서).</summary>
public sealed record ChannelPersonRow(string Name, string Uri, string Meta, bool IsMe, bool IsSpeaking, bool IsAbsent, bool IsChair, bool IsListener)
{
    /// <summary>아바타 머리글자 — 번호뿐인 사람은 빈 값(화면이 전화 아이콘을 그린다).</summary>
    public string Initial => Name.Trim() is { Length: > 0 } n && !(char.IsDigit(n[0]) || n[0] == '+') ? n[..1] : "";
    public string Number => UserPartConverter.UserPart(Uri);
    public bool CanAct => !IsMe;
    public string NameText => IsMe ? $"{Name} (나)" : Name;
}

/// <summary>채널 상세 «영상» 절의 송출 한 줄(§10.3) — 이름 · 기능 별칭 · 경과 + [보기]/[바꿔 보기] · «보는 중». 1초 틱은 경과만 바꾼다(버튼 호버가 끊기지 않게).</summary>
public sealed partial class VideoTxRow : ObservableObject
{
    public string UserId { get; }
    public string Name { get; }
    public string Alias { get; }
    [ObservableProperty] private ReceptionState _state;
    /// <summary>다른 송출을 보고 있다 — 이 줄의 버튼이 [바꿔 보기](보던 것을 그만 보고 이것을 본다 — 1차 한 번에 하나).</summary>
    [ObservableProperty] private bool _otherReceiving;
    [ObservableProperty] private string _elapsedText = "";
    public VideoTxRow(string userId, string name, string alias) { UserId = userId; Name = name; Alias = alias; }
    /// <summary>"현장지휘 · 0:34" — 별칭이 없으면 경과만.</summary>
    public string Meta => string.Join(" · ", new[] { Alias, ElapsedText }.Where(x => x.Length > 0));
    public bool IsReceiving => State == ReceptionState.Receiving;
    public bool CanAccept => State == ReceptionState.Notified;
    public string ActionText => State switch
    {
        ReceptionState.PendingRequest => "요청 중…", ReceptionState.PendingRelease => "끝내는 중…",
        _ => OtherReceiving ? "바꿔 보기" : "보기",
    };
    partial void OnStateChanged(ReceptionState value) { OnPropertyChanged(nameof(IsReceiving)); OnPropertyChanged(nameof(CanAccept)); OnPropertyChanged(nameof(ActionText)); }
    partial void OnOtherReceivingChanged(bool value) => OnPropertyChanged(nameof(ActionText));
    partial void OnElapsedTextChanged(string value) => OnPropertyChanged(nameof(Meta));
}

/// <summary>채널 상세 — 내 채널 카드(ChannelCard) 또는 타 채널 행(ScopedCard)의 투영. 조작은 카드의 명령을 그대로 부른다(같은 판정).</summary>
public sealed partial class ChannelDetailViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    public ChannelCard? Card { get; }
    public ScopedCard? Other { get; }
    /// <summary>connected | all — [접속 n | 편성 m] 세그먼트.</summary>
    [ObservableProperty] private string _segment = "connected";
    /// <summary>편성 멤버(GMS 그룹 문서) — [편성] 을 처음 누를 때 받는다. null = 아직 안 받음.</summary>
    [ObservableProperty] private IReadOnlyList<GroupMember>? _members;
    [ObservableProperty] private string _membersNote = "";
    public ObservableCollection<ChannelPersonRow> Rows { get; } = new();
    /// <summary>청취 중인 타 채널의 청취 leg — 음량·출력(감청 창과 같은 VM).</summary>
    [ObservableProperty] private MonitorWindowViewModel? _listen;

    public event EventHandler<GroupInfo>? MessageRequested;
    public event EventHandler<GroupInfo>? EditRequested;
    public event EventHandler<GroupInfo>? DeleteRequested;
    public event EventHandler<string>? PersonMenuRequested;
    public event EventHandler<string>? PrivateCallRequested;
    public event EventHandler<string>? SdsRequested;

    public ChannelDetailViewModel(DispatchSession s, ChannelCard card) { _s = s; Card = card; Refresh(); }
    public ChannelDetailViewModel(DispatchSession s, ScopedCard other) { _s = s; Other = other; Refresh(); }

    public GroupInfo? Group => Card?.Group ?? Other?.Group;
    public string Id => Card?.Id ?? Other?.Id ?? "";
    public string Title => Card?.Title ?? Other?.Title ?? "";
    public bool IsMine => Card is not null;
    public bool IsOther => Other is not null;
    public bool IsEmergency => Card?.IsEmergency ?? Other?.IsEmergency ?? false;
    public bool IsImminentPeril => Card?.IsImminentPeril ?? Other?.IsImminentPeril ?? false;
    public bool IsBroadcast => Card?.IsBroadcast ?? Other?.IsBroadcast ?? false;
    public bool HasLabels => IsEmergency || IsImminentPeril || IsBroadcast;
    public int ConnectedCount => Group?.ConnectedCount ?? (Card?.IsPrivate == true ? 2 : Card?.Session?.AdhocMembers.Count + 1 ?? 0);
    public int MemberCount => Members?.Count ?? Group?.MemberCount ?? 0;

    /// <summary>머리 아래 한 줄 — "멤버 그룹 · 참가 7 · 02:03 · 편성 12 · 발언 김관제".</summary>
    public string Summary
    {
        get
        {
            var parts = new List<string>();
            if (Card is { } c)
            {
                parts.Add(c.IsMember ? "멤버 그룹" : c.IsPrivate ? $"개별 {(c.IsFullDuplex ? "전이중" : "반이중")}" : "애드혹");
                if (c.IsJoined) { parts.Add($"참가 {ConnectedCount}"); parts.Add(DispatchSession.Fmt(c.Elapsed)); }
                else parts.Add(c.Group?.HasSession == true ? $"참가 {c.Group.ConnectedCount} · 진행(미참여)" : "대기");
                if (c.IsMember) parts.Add($"편성 {MemberCount}");
                if (c.HasSpeaker) parts.Add($"발언 {c.Speaker}");
            }
            else if (Other is { } o)
            {
                parts.Add("청취 범위");
                parts.Add(o.HasSession ? $"참가 {o.Participants}" : "대기");
                if (o.Elapsed is TimeSpan e) parts.Add(DispatchSession.Fmt(e));
                parts.Add($"편성 {MemberCount}");
                if (o.HasSpeaker) parts.Add($"발언 {o.Speaker}");
            }
            return string.Join(" · ", parts);
        }
    }

    // ── 조작(내 채널) — 카드의 판정 그대로 ──
    public bool ShowJoin => Card?.ShowJoin == true;
    public bool ShowLeave => Card?.IsJoined == true;
    public string LeaveText => Card?.IsMember == true ? "나가기" : "종료";
    public bool ShowEmergency => Card is { IsMember: true } c && !c.IsEmergency;
    public bool CanEmergency => Card?.CanEmergency == true;
    public string EmergencyText => Card?.IsJoined == true ? "긴급" : "긴급 참여";
    public string EmergencyTip => Card?.EmergencyTip ?? "";
    public bool ShowCancelEmergency => Card?.CanCancelEmergency == true;
    public bool ShowTarget => Card?.ShowTarget == true;
    public bool CanTarget => Card?.CanCheck == true;
    public bool IsTarget => Card?.IsChecked == true;
    public string TargetText => IsTarget ? "✓ 발언 대상" : "발언 대상";
    /// <summary>[일제 통화] — 미참여 멤버 그룹에. **누르고 있는 동안은 남긴다** — 개시되면 참여 중이 되는데, 그때 버튼을 숨기면 마우스 캡처가 풀려
    /// (HoldButton.OnLostMouseCapture) 놓은 것으로 읽히고 연 일제 통화가 곧바로 끝난다.</summary>
    public bool ShowBroadcast => Card is { IsMember: true } c && (!c.IsJoined || c.IsBroadcastHeld);
    public bool CanPressBroadcast => Card?.CanPressBroadcast == true;
    public string BroadcastText => Card?.BroadcastText ?? "";
    public string BroadcastTip => Card?.BroadcastTip ?? "";
    public bool IsBroadcastHeld => Card?.IsBroadcastHeld == true;
    public bool ShowMute => Card?.ShowMute == true;
    public bool IsMuted => Card?.IsMuted == true;
    /// <summary>[메시지 ›] — 멤버 그룹만(청취 범위 그룹에 그룹 SDS 를 보내면 비멤버라 서버가 403).</summary>
    public bool ShowMessage => Card?.Group is not null;
    public string MessageText => Card?.Unread > 0 ? $"메시지 {Card.Unread} ›" : "메시지 ›";
    public bool ShowRoute => Card?.IsJoined == true || Other?.IsListening == true;
    public bool CanRoute => _s.Audio.HasSpeaker;
    public bool RouteIsSpeaker => Card?.RouteIsSpeaker ?? Other?.RouteIsSpeaker ?? false;
    public string RouteText => RouteIsSpeaker ? "스피커" : "헤드셋";
    public bool ShowMore => (Card?.CanEdit ?? Other?.CanEdit ?? false) && Group is not null;
    // ── 조작(타 채널) ──
    public bool ShowListen => Other is { IsListening: false };
    public bool CanListen => Other?.CanListen == true;
    public string ListenTip => Other?.ListenTip ?? "";
    public bool ShowStopListen => Other?.IsListening == true;
    public bool HasSegment => Group is not null;
    public string SegConnected => $"접속 {ConnectedCount}";
    public string SegAll => $"편성 {MemberCount}";
    public string RosterHead => Group is null ? $"참가 {ConnectedCount}" : "";

    // ── 영상 채널(MCVideo, §10.3) — 멤버 영상 채널만. «영상 참여» 없음(D10 — 앱이 영상 호에 함께 합류), 수신 manual(D8 — 골라 [보기]) ──
    public bool ShowVideo => Card?.IsVideoGroup == true;
    public SessionItem? Video => Card?.Video;
    /// <summary>영상 호가 **성립했다**(200 OK 뒤) — 합류 INVITE 가 나가 있는 동안은 아니다(«연결됨»·[영상 보내기] 를 성립 전에 세우지 않는다).</summary>
    public bool VideoConnected => Video is { IsActive: true };
    /// <summary>머리 옆 작은 글 — 연결됨이면 «채널 참여와 함께 연결됨», 아니면 연결 상태(연결 중·편성 초대 대기·한도·실패·재시도).</summary>
    /// <summary>내가 연 편성 영상 호가 성립 전 — 제어 기능이 멤버를 초대하는 중(TS 24.281 §9.2.1.4.2).</summary>
    public bool VideoOpening => _s.IsVideoOpening(Video);
    /// <summary>편성 영상 채널에 영상 호가 없다 — [영상 보내기] 가 영상 호를 연다.</summary>
    public bool CanOpenVideo => Group is { } g && _s.CanOpenVideo(g);
    public bool IsPrearrangedVideo => Group is { McVideo: true, McVideoType: "prearranged" };
    /// <summary>[영상 보내기] 를 보일 것인가 — 영상 호가 있거나, 편성 채널(호가 없으면 이 버튼이 연다).</summary>
    public bool ShowVideoSend => VideoConnected || IsPrearrangedVideo;
    public string VideoSub => VideoOpening ? "영상 호를 여는 중 — 멤버가 받기를 기다립니다"
        : VideoConnected ? (IsPrearrangedVideo ? "편성 영상 호 연결됨" : "채널 참여와 함께 연결됨")
        : Group?.VideoNote is { Length: > 0 } n ? n : "영상 연결 중…";
    public int VideoSenderCount => VideoConnected ? Video!.Transmitters.Count : 0;
    /// <summary>보내는 사람 없음 = 머리 한 줄 + [영상 보내기]만(영상 칸·목록 없음).</summary>
    public bool ShowNoSender => VideoConnected && VideoSenderCount == 0;
    public bool HasVideoRows => VideoRows.Count > 0;
    /// <summary>"보내는 중 2 · 보는 중 1 (한 번에 1개)".</summary>
    public string VideoCountText => $"보내는 중 {VideoSenderCount}" + (IsVideoReceiving ? " · 보는 중 1" : "") + " (한 번에 1개)";
    public bool IsVideoReceiving => Video?.Receiving is not null;
    /// <summary>영상 칸 캡션 "김현장 · 현장지휘 · 0:34".</summary>
    public string ReceivingCaption => Video?.Receiving is { } r
        ? string.Join(" · ", new[] { _s.NameOfPtt(r.UserId), r.FunctionalAlias, ElapsedOf(Video, r.UserId) }.Where(x => x.Length > 0)) : "";
    /// <summary>영상 미디어가 열렸다(호가 m=video 를 협상) — 아니면(서버 answer port 0) 그림이 오지 않는다(영상 호 소리만, §10.3).</summary>
    public bool VideoCanRender => Video?.Info.Video == true;
    /// <summary>칸의 자리 표시 — 첫 장이 그려지면 거둔다(VideoView.HasPicture). [보기] 직후 첫 키프레임까지 잠깐 보인다.</summary>
    public string VideoSurfaceText => VideoCanRender ? "영상 기다리는 중…" : "영상 미디어가 열리지 않았습니다 — 영상 호 소리만 들립니다";
    /// <summary>보는 송출의 그림 우편함(그 영상 호) — 영상 칸의 VideoView 가 그린다(엔진 = 창 없는 프레임 렌더, ue_sdk.md §4.5).</summary>
    public VideoFeed? ReceivingFeed => Video is { IsLive: true } v ? _s.VideoFrames.Feed(v.CallId) : null;

    // ── 내 송출(D11 — [영상 보내기] 는 음성 무전과 따로, TS 24.581 §6.2.4) ──
    public TransmissionState TxState => VideoConnected ? Video!.Transmission.State : TransmissionState.NoPermission;
    public bool IsVideoSending => TxState == TransmissionState.Permitted;
    /// <summary>여는 중·요청·대기·송출 중 — [영상 보내기] 가 끄는 버튼이 된다.</summary>
    public bool IsVideoTxActive => VideoOpening || TxState != TransmissionState.NoPermission;
    public bool CanVideoSend => VideoOpening || (VideoConnected ? TxState != TransmissionState.PendingEnd && (TxState != TransmissionState.NoPermission || _s.HasCamera) : CanOpenVideo);
    public string VideoSendText => VideoOpening ? "여는 중… · 취소" : TxState switch
    {
        TransmissionState.PendingRequest => "요청 중… · 취소",
        TransmissionState.Queued => Video!.Transmission.QueuePosition is > 0 and < 254 and var q ? $"대기 {q}번째 · 대기 취소" : "대기 중 · 대기 취소",
        TransmissionState.Permitted => "보내기 끝",
        TransmissionState.PendingEnd => "끝내는 중…",
        _ => _s.HasCamera ? "영상 보내기" : "영상 보내기 — 카메라 없음",
    };
    public string VideoSendTip => VideoOpening ? "영상 호 열기를 거둔다(멤버 초대 취소)" : TxState switch
    {
        TransmissionState.NoPermission when !_s.HasCamera => "이 PC 에서 카메라를 찾지 못했습니다 — 설정 › 영상",
        TransmissionState.NoPermission when !VideoConnected && Group is { McVideoAffiliated: false } => "이 채널의 영상(MCVideo) 제휴가 아직 서지 않았습니다 — 잠시 뒤 다시",
        TransmissionState.NoPermission when !VideoConnected => $"편성 영상 그룹 — 영상 호를 열고 내 카메라 영상을 보낸다({_s.CameraName}). 영상 채널에 있는 멤버를 초대한다(TS 24.281 §9.2.1.2.1.1)",
        TransmissionState.NoPermission => $"이 채널에 내 카메라 영상을 보낸다({_s.CameraName}) — 말하기는 발언 바 PTT(음성 무전, D11)",
        TransmissionState.Permitted => "영상 보내기를 끝낸다(카메라를 닫는다 — TS 24.581 §6.2.4.5)",
        _ => "영상 보내기 요청을 거둔다",
    };
    /// <summary>"내 영상 보내는 중 · 0:12 · 보는 사람 2" — 보는 사람은 서버가 알릴 때만(Media Reception Notification).</summary>
    public string SendingCaption => Video is { TxSince: { } at } v
        ? "내 영상 보내는 중 · " + DispatchSession.Fmt(DateTime.Now - at) + (v.TxReceivers.Count > 0 ? $" · 보는 사람 {v.TxReceivers.Count}" : "")
        : "내 영상 보내는 중";
    /// <summary>셀프뷰 우편함(내 카메라 — 코어가 송출 중일 때만 넘긴다).</summary>
    public VideoFeed SelfFeed => _s.VideoFrames.Feed(VideoFrames.SelfView);
    /// <summary>D12 음성 우선 — 무전을 말하는 동안 영상 호 소리를 멈췄다.</summary>
    public bool ShowVideoMicNote => IsVideoSending && _s.VideoMicYielded;
    [RelayCommand]
    private void ToggleVideoSend()
    {
        if (VideoOpening) { _s.CancelVideoOpen(Video!); Refresh(rows: false); return; }                 // 여는 중에 다시 누름 = 개시를 거둔다
        if (Video is not { IsLive: true } v)
        {
            if (Group is { } g && _s.CanOpenVideo(g)) { _s.OpenVideoTx(g); Refresh(rows: false); }      // 편성 채널 — 영상 호를 연다
            return;
        }
        if (!v.IsActive) return;                                                                         // 합류 중 — 성립한 뒤에 누른다
        if (TxState == TransmissionState.NoPermission) _s.RequestVideoTx(v);
        else if (TxState != TransmissionState.PendingEnd) _s.ReleaseVideoTx(v);
        Refresh(rows: false);
    }
    /// <summary>영상 회전(°, 시계 방향) — 보고 있는 송출의 것(보내는 사람마다 기억, DispatchSession.RotateVideo).</summary>
    public int VideoRotation => Video?.Receiving is { } r ? _s.VideoRotationOf(r.UserId) : 0;
    /// <summary>칸 모양 — 기본 세로 480×640(3:4, 0°·180°), 90°·270° 면 가로 640×480(4:3). 오른쪽 패널 폭에 맞춰 0.5625 배(270×360 / 360×270).</summary>
    public bool VideoLandscape => VideoRotation % 180 == 90;
    public double VideoFrameWidth => VideoLandscape ? 360 : 270;
    public double VideoFrameHeight => VideoLandscape ? 270 : 360;
    public string VideoRotationTip => $"영상 회전 — 지금 {VideoRotation}° (보내는 쪽 카메라 방향이 다를 때 90° 씩)";
    public ObservableCollection<VideoTxRow> VideoRows { get; } = new();
    /// <summary>[영상 소리](TS 22.280 R-8.3-002 — 동시 오디오 원천의 상대 음량) 열림.</summary>
    [ObservableProperty] private bool _videoVolumeOpen;
    /// <summary>영상 호 수신 음량 0~2 — 코어가 호에 기억하고(CallInfo.RxLevel) 화면은 끈 값을 든다(스냅샷이 늦게 와도 막대가 되돌아가지 않게). 호가 바뀌면 코어 값에서 다시.</summary>
    public double VideoVolume
    {
        get => _volumeCall == Video?.CallId && _volume is double v ? v : Video?.Info.RxLevel ?? 1f;
        set { if (Video is { } call) { _volumeCall = call.CallId; _volume = value; _s.SetVideoVolume(call, (float)value); OnPropertyChanged(); } }
    }
    private double? _volume;
    private int _volumeCall = -1;

    private static string ElapsedOf(SessionItem v, string userId) => v.TransmitterSince.TryGetValue(userId, out var at) ? DispatchSession.Fmt(DateTime.Now - at) : "";

    /// <summary>송출 줄 동기화 — 같은 송출자는 줄을 유지하고 상태·경과만 바꾼다(버튼 호버·누름이 끊기지 않게).</summary>
    private void SyncVideoRows()
    {
        var list = Video is { IsLive: true } v ? v.Transmitters : Array.Empty<VideoTransmitter>();
        bool receiving = Video?.Receiving is not null;
        foreach (var gone in VideoRows.Where(r => !list.Any(t => string.Equals(t.UserId, r.UserId, StringComparison.OrdinalIgnoreCase))).ToList()) VideoRows.Remove(gone);
        foreach (var t in list)
        {
            var row = VideoRows.FirstOrDefault(r => string.Equals(r.UserId, t.UserId, StringComparison.OrdinalIgnoreCase));
            if (row is null) { row = new VideoTxRow(t.UserId, _s.NameOfPtt(t.UserId), t.FunctionalAlias); VideoRows.Add(row); }
            row.State = t.State;
            row.OtherReceiving = receiving && t.State != ReceptionState.Receiving;
            row.ElapsedText = ElapsedOf(Video!, t.UserId);
        }
        OnPropertyChanged(nameof(HasVideoRows));
    }

    [RelayCommand] private void AcceptVideo(VideoTxRow r) { if (Video is { } v && r.CanAccept) _s.AcceptVideo(v, r.UserId); }
    [RelayCommand] private void EndVideo() { if (Video is { Receiving: { } t } v) _s.EndVideo(v, t.UserId); }
    [RelayCommand] private void ToggleVideoVolume() => VideoVolumeOpen = !VideoVolumeOpen;
    /// <summary>[↻] cw = 시계 방향 90° · [↺] ccw = 반시계 방향 90°.</summary>
    [RelayCommand]
    private void RotateVideo(string dir)
    {
        if (Video?.Receiving is not { } r) return;
        _s.RotateVideo(r.UserId, dir == "ccw" ? -90 : 90);
        foreach (var p in new[] { nameof(VideoRotation), nameof(VideoLandscape), nameof(VideoFrameWidth), nameof(VideoFrameHeight), nameof(VideoRotationTip) }) OnPropertyChanged(p);
    }

    partial void OnSegmentChanged(string value) { if (value == "all" && Members is null) _ = LoadMembersAsync(); RebuildRows(); }
    partial void OnMembersChanged(IReadOnlyList<GroupMember>? value) { OnPropertyChanged(nameof(MemberCount)); OnPropertyChanged(nameof(SegAll)); OnPropertyChanged(nameof(Summary)); RebuildRows(); }
    [RelayCommand] private void SetSegment(string seg) => Segment = seg;

    private async Task LoadMembersAsync()
    {
        if (Group is not { } g) return;
        MembersNote = "편성 멤버를 받는 중…";
        var r = await _s.GetGroupAsync(g);
        if (!r.Ok) { MembersNote = "편성 멤버를 받지 못했습니다 — " + ResponseText.Describe(ResponseText.Area.Group, r.Code, r.Reason); return; }
        MembersNote = "";
        Members = r.Value.Members;
    }

    /// <summary>"PTT 1001 · 순찰대" — 전화번호부의 조직 이름.</summary>
    private string MetaOf(string uri, string suffix = "")
    {
        string num = UserPartConverter.UserPart(uri);
        var c = _s.Directory.PttUsers.FirstOrDefault(x => DirectoryService.Normalize(x.Number) == DirectoryService.Normalize(num));
        string org = c is { OrgCode.Length: > 0 } ? _s.Directory.OrgName(c.OrgCode) : "";
        return string.Join(" · ", new[] { "PTT " + _s.Directory.DisplayNumber(num), org, suffix }.Where(x => x.Length > 0));
    }

    private void RebuildRows()
    {
        Rows.Clear();
        string speaker = Card?.Speaker ?? Other?.Speaker ?? "";
        if (Group is { } g)
        {
            var roster = g.Roster.Where(r => r.Status != "listener" || !_s.ListenHidden).ToList();
            if (Segment == "all" && Members is { } members)
            {
                foreach (var m in members.OrderByDescending(m => m.Role == "chair").ThenBy(m => m.Name, StringComparer.CurrentCulture))
                {
                    var e = roster.FirstOrDefault(r => DirectoryService.Normalize(UserPartConverter.UserPart(r.Uri)) == DirectoryService.Normalize(UserPartConverter.UserPart(m.Uri)));
                    string name = m.Name.Length > 0 ? m.Name : _s.NameOfPtt(m.Uri);
                    bool me = _s.IsMe(m.Uri);
                    Rows.Add(new ChannelPersonRow(me ? _s.DisplayName : name, m.Uri, MetaOf(m.Uri, e is null ? "미참가" : ""), me, speaker.Length > 0 && (name == speaker || (me && speaker == "나")),
                                                  e is null, m.Role == "chair", e?.Status == "listener"));
                }
            }
            else
            {
                foreach (var r in roster.OrderByDescending(r => _s.IsMe(r.Uri)).ThenBy(r => _s.NameOfPtt(r.Uri), StringComparer.CurrentCulture))
                {
                    bool me = _s.IsMe(r.Uri);
                    string name = me ? _s.DisplayName : _s.NameOfPtt(r.Uri);
                    string st = r.Status switch { "listener" => "청취", "on-hold" => "보류", _ => "" };
                    Rows.Add(new ChannelPersonRow(name, r.Uri, MetaOf(r.Uri, st), me, speaker.Length > 0 && (name == speaker || (me && speaker == "나")), false, false, r.Status == "listener"));
                }
            }
        }
        else if (Card?.Session is { } sess)
        {
            // 개별·애드혹 — 로스터 구독이 없어 나 + 상대(애드혹 = 초대한 사람)
            Rows.Add(new ChannelPersonRow(_s.DisplayName, _s.ToTelUri(_s.MyPttNumber), MetaOf(_s.MyPttNumber), true, sess.IsSpeaking, false, false, false));
            var peers = Card.IsPrivate ? new[] { sess.Info.RemoteUri } : sess.AdhocMembers;
            foreach (var p in peers)
            {
                string name = _s.NameOfPtt(p);
                Rows.Add(new ChannelPersonRow(name, p, MetaOf(p), false, speaker.Length > 0 && name == speaker, false, false, false));
            }
        }
    }

    /// <summary>1초 틱 — 문구만(줄은 로스터·floor 이벤트 때 다시 짓는다: 매초 다시 지으면 줄 버튼의 호버·누름이 끊긴다).</summary>
    public void Tick() => Refresh(rows: false);

    public void Refresh(bool rows = true)
    {
        var l = Other?.Listen;
        if (l is null) Listen = null;
        else if (Listen?.Session != l) Listen = new MonitorWindowViewModel(_s, l);
        else Listen.Refresh();
        foreach (var p in new[] { nameof(Title), nameof(IsEmergency), nameof(IsImminentPeril), nameof(IsBroadcast), nameof(HasLabels), nameof(ConnectedCount), nameof(MemberCount), nameof(Summary),
                                  nameof(ShowJoin), nameof(ShowLeave), nameof(LeaveText), nameof(ShowEmergency), nameof(CanEmergency), nameof(EmergencyText), nameof(EmergencyTip),
                                  nameof(ShowCancelEmergency), nameof(ShowTarget), nameof(CanTarget), nameof(IsTarget), nameof(TargetText), nameof(ShowBroadcast), nameof(CanPressBroadcast),
                                  nameof(BroadcastText), nameof(BroadcastTip), nameof(IsBroadcastHeld), nameof(ShowMute), nameof(IsMuted), nameof(ShowMessage), nameof(MessageText),
                                  nameof(ShowRoute), nameof(CanRoute), nameof(RouteIsSpeaker), nameof(RouteText), nameof(ShowMore), nameof(ShowListen), nameof(CanListen), nameof(ListenTip),
                                  nameof(ShowStopListen), nameof(HasSegment), nameof(SegConnected), nameof(SegAll), nameof(RosterHead),
                                  nameof(ShowVideo), nameof(Video), nameof(VideoConnected), nameof(VideoSub), nameof(VideoSenderCount), nameof(ShowNoSender), nameof(VideoCountText),
                                  nameof(IsVideoReceiving), nameof(ReceivingCaption), nameof(VideoCanRender), nameof(VideoSurfaceText), nameof(VideoVolume),
                                  nameof(VideoRotation), nameof(VideoLandscape), nameof(VideoFrameWidth), nameof(VideoFrameHeight), nameof(VideoRotationTip),
                                  nameof(ReceivingFeed), nameof(TxState), nameof(IsVideoSending), nameof(IsVideoTxActive), nameof(CanVideoSend), nameof(VideoSendText),
                                  nameof(VideoSendTip), nameof(SendingCaption), nameof(ShowVideoMicNote), nameof(VideoOpening), nameof(CanOpenVideo), nameof(IsPrearrangedVideo),
                                  nameof(ShowVideoSend) })
            OnPropertyChanged(p);
        SyncVideoRows();
        if (rows) RebuildRows();
    }

    [RelayCommand] private void Join() => Card?.JoinCommand.Execute(null);
    [RelayCommand] private void Leave() => Card?.LeaveCommand.Execute(null);
    [RelayCommand] private void Emergency() => Card?.EmergencyCommand.Execute(null);
    [RelayCommand] private void CancelEmergency() => Card?.CancelEmergencyCommand.Execute(null);
    [RelayCommand] private void ToggleMute() => Card?.ToggleMuteCommand.Execute(null);
    [RelayCommand] private void ToggleRoute() { if (Card is not null) Card.ToggleRouteCommand.Execute(null); else Other?.ToggleRouteCommand.Execute(null); }
    [RelayCommand] private void ToggleListen() => Other?.ToggleListenCommand.Execute(null);
    [RelayCommand] private void ShowWindow() => Other?.ShowWindowCommand.Execute(null);
    [RelayCommand] private void OpenMessages() { if (Group is { } g) MessageRequested?.Invoke(this, g); }
    [RelayCommand] private void Edit() { if (Group is { } g) EditRequested?.Invoke(this, g); }
    [RelayCommand] private void Delete() { if (Group is { } g) DeleteRequested?.Invoke(this, g); }
    [RelayCommand] private void PersonMenu(ChannelPersonRow r) => PersonMenuRequested?.Invoke(this, r.Uri);
    [RelayCommand] private void PrivateCall(ChannelPersonRow r) => PrivateCallRequested?.Invoke(this, r.Number);
    [RelayCommand] private void Sds(ChannelPersonRow r) => SdsRequested?.Invoke(this, r.Number);
}

/// <summary>이벤트 상세 — 시각·채널·종류·내용 + 같은 채널의 앞뒤 이벤트 + [답장][채널 열기][이력에서 세션 보기].</summary>
public sealed record EventDetail(EventRow Row, IReadOnlyList<EventRow> Around)
{
    public string TimeText => Row.Time.ToString("yyyy-MM-dd HH:mm:ss");
    public string Channel => Row.Channel.Length > 0 ? Row.Channel : "—";
    public string KindText => Row.KindText;
    public string Content => Row.Content;
    public bool HasChannel => Row.Group is not null;
    public bool CanReply => Row.CanReply;
    public bool HasAround => Around.Count > 1;
}

public sealed partial class SidePanelViewModel : ObservableObject
{
    [ObservableProperty] private PanelView _view;
    /// <summary>고정(핀) — 모드·레일을 옮겨도 남는다(채널 상세·사용자·주소록만).</summary>
    [ObservableProperty] private bool _pinned;
    [ObservableProperty] private ChannelDetailViewModel? _channel;
    [ObservableProperty] private EventDetail? _event;
    /// <summary>새 PTT 그룹 폼(GroupAdminViewModel.Editor) — 패널이 호스팅하는 동안.</summary>
    [ObservableProperty] private GroupEditViewModel? _group;

    public bool IsOpen => View != PanelView.None;
    public bool IsChannel => View == PanelView.Channel;
    public bool IsUsers => View == PanelView.Users;
    public bool IsGroup => View == PanelView.Group;
    public bool IsEvent => View == PanelView.Event;
    public bool IsDirectory => View == PanelView.Directory;
    /// <summary>← — 패널 안에서 들어간 것(사용자 › 새 그룹)만.</summary>
    public bool HasBack => View == PanelView.Group && _backToUsers;
    public bool HasPin => View is PanelView.Channel or PanelView.Users or PanelView.Directory;
    public bool HasTag => View != PanelView.Group;
    public string Tag => View switch
    {
        PanelView.Channel => "채널 상세", PanelView.Users => "사용자", PanelView.Event => "이벤트 상세", PanelView.Directory => "주소록", _ => "",
    };
    public string Title => View switch
    {
        PanelView.Channel => Channel?.Title ?? "",
        PanelView.Users => $"사용자 {UsersCount}",
        PanelView.Group => "새 PTT 그룹",
        PanelView.Event => Event is { } e ? $"{e.KindText} · {e.Channel}" : "",
        PanelView.Directory => $"주소록 {DirectoryCount}",
        _ => "",
    };
    private bool _backToUsers;
    public int UsersCount { get; set; }
    public int DirectoryCount { get; set; }

    public event EventHandler? BackRequested;

    partial void OnViewChanged(PanelView value)
    {
        if (value is not (PanelView.Channel or PanelView.Users or PanelView.Directory)) Pinned = false;
        foreach (var p in new[] { nameof(IsOpen), nameof(IsChannel), nameof(IsUsers), nameof(IsGroup), nameof(IsEvent), nameof(IsDirectory), nameof(HasBack), nameof(HasPin), nameof(HasTag), nameof(Tag), nameof(Title) })
            OnPropertyChanged(p);
    }
    partial void OnChannelChanged(ChannelDetailViewModel? value) => OnPropertyChanged(nameof(Title));
    partial void OnEventChanged(EventDetail? value) => OnPropertyChanged(nameof(Title));

    public void Show(PanelView v, bool fromUsers = false) { _backToUsers = fromUsers; if (View == v) OnViewChanged(v); else View = v; }
    public void RefreshTitle() => OnPropertyChanged(nameof(Title));
    [RelayCommand] private void Close() => View = PanelView.None;
    [RelayCommand] private void TogglePin() => Pinned = !Pinned;
    [RelayCommand] private void Back() => BackRequested?.Invoke(this, EventArgs.Empty);
}
