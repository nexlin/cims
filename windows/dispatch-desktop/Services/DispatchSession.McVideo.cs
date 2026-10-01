// MCVideo 그룹 영상(dispatch_desktop_ui.md §10 — 규격 모델·결정 정본 mcvideo.md §7 D8·D10~D12). 자격·영상 채널 합류·송출/수신 이벤트·무전 음량(D6).
//   MCVideo 는 MCPTT 와 나란한 서비스다(TS 23.280 §3) — 영상 호는 카드가 아니라 그 그룹의 «영상» 절에 붙고, 음성(MCPTT 호)과 따로 든다
//   (TS 24.281 §7.1 — 한 등록을 공유하는 독립 다이얼로그).
//   D10 «영상 참여» 단계가 없다 — 내 채널의 MCVideo 그룹은 앱이 영상 호에도 함께 합류한다(chat = 합류, prearranged = MCVideo affiliation +
//   멤버 초대 자동 수락, TS 22.280 R-8.4.2-002). «채널» = 관제사가 내 채널에 둔 의도라 무전 세션의 T4·TNG3 해제(TS 24.379 §6.3.8.1)와 무관하게 이어지고,
//   영상 호가 끝나면 다시 합류한다. 끝내는 것은 그룹이 내 채널에서 빠질 때·로그아웃뿐.
//   D8 수신 manual — 송출을 골라 [보기](한 번에 하나 — 엔진 렌더 창이 전역 하나), [바꿔 보기] = 보던 것을 그만 보고 새것을 본다.
using CimsUe;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;

namespace DispatchDesktop.Services;

public sealed partial class DispatchSession
{
    /// <summary>MCVideo user profile(TS 24.484 §9.3) — 받으면 이용 자격이 있다(404 = 없음). 그룹 목록 &lt;MCVideoGroupInfo&gt; 가 영상 채널.</summary>
    private McVideoUserProfileDoc? _mcvideoProfile;
    /// <summary>이 로그인의 PTT 계정에 MCVideo 를 실었다 — 사이트(ue-init-config MCVideo PSI) ∧ 자격(user profile). 계정 태그는 로그인 때 정한다(§10.2).</summary>
    public bool McVideoEnabled { get; private set; }
    /// <summary>영상을 받는 동안 수신 음량을 줄여 둔 무전(MCPTT 호) — callId(§10.5).</summary>
    private readonly HashSet<int> _ducked = new();
    /// <summary>무전 음량 배율 — 같은 그룹 영상을 받는 동안(D6 «영상 호 음성 우선», 단말 VIDEO_DUCK 과 같은 값).</summary>
    private const float VideoDuckLevel = 0.3f;
    /// <summary>합류 INVITE 를 보냈고 아직 세션이 서지 않은 그룹 — 같은 그룹에 두 번 보내지 않는다.</summary>
    private readonly HashSet<string> _videoJoining = new(StringComparer.OrdinalIgnoreCase);
    /// <summary>다음 합류 시도 시각(그룹 id) — 실패·예상 밖 종료 뒤 물러난다(단말 PttVideo 와 같은 규칙: 실패 10 s 배수 → 최대 2 분, 정상 종료 뒤 3 s).</summary>
    private readonly Dictionary<string, DateTime> _videoRetryAt = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, int> _videoFailures = new(StringComparer.OrdinalIgnoreCase);
    /// <summary>관제사 쪽에서 끝낸 영상 호(그룹이 빠짐·로그아웃) — 다시 합류하지 않는다.</summary>
    private readonly HashSet<int> _videoLeaving = new();
    private const int VideoBackoffSec = 10, VideoBackoffMaxSec = 120, VideoRejoinSec = 3;
    private readonly HashSet<string> _mcvideoAttrsLoading = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>영상 절이 바뀌었다(연결·송출·수신) — 카드·채널 상세가 다시 그린다.</summary>
    public event EventHandler<GroupInfo>? VideoChanged;

    private string McVideoPsi => _ueInit?.McvideoServerUri ?? "";
    /// <summary>동시 MCVideo 호 상한 N6(user profile &lt;MaxSimultaneousCallsN6&gt;) — 넘으면 서버가 486 103. 모르면 상한 없음으로 본다.</summary>
    private int McVideoN6 => _mcvideoProfile is { MaxSimultaneousCallsN6: > 0 } p ? p.MaxSimultaneousCallsN6 : int.MaxValue;

    /// <summary>MCVideo user profile 재조회(ETag). 404 = 자격 없음(사본을 버린다), 그 밖의 실패(망·5xx)는 가진 사본을 둔다.</summary>
    private async Task RefreshMcVideoProfileAsync()
    {
        if (_csc is null || MyPttId.Length == 0 || McVideoPsi.Length == 0) return;
        var csc = _csc; if (await AccessTokenAsync() is not { } token) return;
        var r = await csc.FetchMcVideoUserProfileAsync(token, MyPttId, _mcvideoProfile?.ETag);
        if (r.Ok) { if (!r.Value.NotModified) _mcvideoProfile = r.Value; }
        else if (r.Code == 404) _mcvideoProfile = null;
        else { Log.Warn($"mcvideo user-profile: {r}"); return; }
        Log.Info($"mcvideo user-profile {(_mcvideoProfile is { } p ? $"groups={p.Groups.Count} streams={p.MaxSimultaneousVideoStreams} n6={p.MaxSimultaneousCallsN6}" : "none(자격 없음)")}");
        if (Ptt is not null) ApplyMcVideoGroups();
    }

    /// <summary>멤버 그룹의 영상 채널 표시·MCVideo affiliation 을 user profile 그룹 목록에 맞추고 영상 호 합류를 맞춘다(§10.2). affiliation 은 관심 그룹 전부를
    /// 한 PUBLISH 로 — 코어가 집합을 들고 호출마다 다시 싣는다(TS 24.281 §8.2.1.2). 자격이 없어졌으면(다음 로그인까지 태그는 그대로) 표시를 내리고 영상 호를 끝낸다.</summary>
    private void ApplyMcVideoGroups()
    {
        var ptt = Ptt;
        var ids = (_mcvideoProfile?.Groups ?? Array.Empty<string>()).Select(UserPartConverter.UserPart).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var g in Groups.Where(x => x.IsMember))
        {
            bool on = McVideoEnabled && ids.Contains(g.Id);
            if (g.McVideo && !on) LeaveVideoChannel(g);
            g.McVideo = on;
            if (ptt is null || on == g.McVideoAffiliated) continue;
            var r = ptt.Affiliate(g.Id, on, McService.McVideo);
            if (!r.Ok) { Log.Warn($"mcvideo affiliate {g.Id} {on}: {r}"); continue; }
            g.McVideoAffiliated = on;
        }
        EnsureVideoChannels();
    }

    /// <summary>그룹 문서의 MCVideo 몫(호 방식·동시 송출 상한)을 한 번 받는다 — 합류 INVITE 의 session-type 이 그룹 종류와 어긋나면 서버가 404 117/118(TS 24.281 §6.3.5.2).
    /// 이미 알면 아무것도 하지 않는다. 알고 나면 영상 호 합류를 다시 맞춘다.</summary>
    public async Task EnsureMcVideoAttrsAsync(GroupInfo g)
    {
        if (!g.McVideo || g.McVideoType.Length > 0 || !_mcvideoAttrsLoading.Add(g.Id)) return;
        try { await LoadMcVideoAttrsAsync(g); }
        finally { _mcvideoAttrsLoading.Remove(g.Id); }
        VideoChanged?.Invoke(this, g);
        if (g.McVideoType.Length > 0) EnsureVideoChannels();
    }

    private async Task LoadMcVideoAttrsAsync(GroupInfo g)
    {
        var r = await GetGroupAsync(g);
        if (!r.Ok)
        {
            Log.Warn($"mcvideo attrs {g.Id}: {r}");
            g.VideoNote = "영상 그룹 문서를 받지 못했습니다 — 30초 뒤 다시";
            _videoRetryAt[g.Id] = DateTime.Now.AddSeconds(30);
            return;
        }
        if (r.Value.Mcvideo is { } a)
        {
            g.McVideoType = a.InviteMembers ? "prearranged" : "chat";
            g.McVideoMaxTransmitters = a.MaxTransmitters ?? 0;
        }
        else g.VideoNote = "그룹 문서에 영상(MCVideo) 몫이 없습니다";
    }

    // ── 영상 채널 합류(D10) ──

    /// <summary>내 채널의 영상 채널마다 영상 호를 맞춘다 — chat 은 합류(INVITE, 합류가 곧 affiliation — TS 24.281 §9.2.2), prearranged 는 초대를 기다린다
    /// (멤버 초대 자동 수락 — §9.2.1.3). 동시 MCVideo 호 상한 N6 안에서 카드 순서대로. 부르는 곳 = 그룹 목록 갱신·PTT 등록 성공·그룹 문서 도착·영상 호 종료·1초 틱(재시도 시각).</summary>
    private void EnsureVideoChannels()
    {
        if (!McVideoEnabled || Ptt is not { } ptt || PttReg.State != RegState.Registered) return;
        var now = DateTime.Now;
        int live = Sessions.Count(s => s.IsMcVideo && s.IsLive) + _videoJoining.Count;
        foreach (var g in Groups.Where(x => x.IsMember && x.McVideo))
        {
            if (g.VideoSession is not null || _videoJoining.Contains(g.Id)) continue;
            if (_videoRetryAt.TryGetValue(g.Id, out var at) && now < at) continue;
            if (g.McVideoType.Length == 0) { _ = EnsureMcVideoAttrsAsync(g); continue; }        // 호 방식을 안 뒤 다시
            if (g.McVideoType == "prearranged") { g.VideoNote = "편성 영상 그룹 — 멤버가 영상 호를 열면 함께 합류합니다"; continue; }
            if (live >= McVideoN6) { g.VideoNote = $"동시 영상 호 한도(N6 = {McVideoN6})가 찼습니다 — 이 채널 영상은 연결하지 않았습니다(운영자에게 한도 상향 요청)"; continue; }
            var r = ptt.JoinVideoGroupCall(g.Id, new VideoGroupCallOptions { Prearranged = false, Queueing = true });
            if (!r.Ok) { Log.Warn($"mcvideo join {g.Id}: {r}"); VideoBackoff(g, failed: true); continue; }
            _pendingOps[r.Value.Id] = Operation.VideoJoin;
            _videoJoining.Add(g.Id);
            g.VideoNote = "영상 연결 중…";
            live++;
            if (Find(r.Value.Id) is null && r.Value.Info.IsLive) OnCallState(r.Value.Info);   // 같은 그룹 재합류처럼 이미 세션이 있으면
        }
    }

    /// <summary>합류 실패·예상 밖 종료 — 다음 시도까지 물러난다. 정상 종료(예: TNG3 해제) 뒤 재합류는 3 초.</summary>
    private void VideoBackoff(GroupInfo g, bool failed, int code = 0)
    {
        int wait;
        if (failed)
        {
            int n = _videoFailures[g.Id] = _videoFailures.GetValueOrDefault(g.Id) + 1;
            wait = Math.Min(VideoBackoffMaxSec, VideoBackoffSec << Math.Min(n - 1, 4));
            g.VideoNote = $"영상 연결 안 됨 — {(code >= 300 ? ResponseText.Describe(ResponseText.Area.Video, code, "") : "호를 열지 못했습니다")} · {wait}초 뒤 다시";
        }
        else { _videoFailures.Remove(g.Id); wait = VideoRejoinSec; }
        _videoRetryAt[g.Id] = DateTime.Now.AddSeconds(wait);
    }

    /// <summary>영상 채널에서 나간다(그룹이 내 채널에서 빠짐·자격 없어짐) — 영상 호 BYE(MCPTT 호는 그대로 — TS 24.281 §6.2.4.1). 다시 합류하지 않는다.</summary>
    private void LeaveVideoChannel(GroupInfo g)
    {
        if (VideoOfGroup(g.Id) is { } v) { _videoLeaving.Add(v.CallId); Engine.GetCall(v.CallId).Hangup(); }
        _videoJoining.Remove(g.Id); _videoRetryAt.Remove(g.Id); _videoFailures.Remove(g.Id);
    }

    /// <summary>1초 틱 — 재합류 시각이 된 채널.</summary>
    private void TickMcVideo(DateTime now)
    {
        if (!McVideoEnabled) return;
        var due = _videoRetryAt.Where(kv => now >= kv.Value).Select(kv => kv.Key).ToList();
        if (due.Count == 0) return;
        foreach (var id in due) _videoRetryAt.Remove(id);
        EnsureVideoChannels();
    }

    /// <summary>로그아웃 — 영상 상태를 비운다(호는 Logout 이 끊는다, 재합류 없음).</summary>
    private void ResetMcVideo()
    {
        McVideoEnabled = false; _mcvideoProfile = null;               // 재합류 판정이 먼저 막힌다 — 호 번호는 다음 엔진에서 다시 쓰이므로 나가기 표시는 남기지 않는다
        _ducked.Clear(); _videoJoining.Clear(); _videoRetryAt.Clear(); _videoFailures.Clear(); _mcvideoAttrsLoading.Clear(); _videoLeaving.Clear(); _watchedLast.Clear();
        foreach (var b in Notify.Banners.Where(b => b.IsVideo).ToList()) Notify.RemoveBanner(b);
    }

    // ── 관제 동작 ──

    /// <summary>[보기]·[바꿔 보기](§10.4) — Receive Media Request(TS 24.581 §6.2.5.3.3). 한 번에 하나 — 보던 송출이 있으면 먼저 그만 본다(엔진 렌더 창 하나).</summary>
    public Result AcceptVideo(SessionItem s, string transmitterId)
    {
        var call = Engine.GetCall(s.CallId);
        if (s.Receiving is { } cur)
        {
            if (string.Equals(cur.UserId, transmitterId, StringComparison.OrdinalIgnoreCase)) return Result.Success;
            var e = call.EndReception(cur.UserId);
            if (!e.Ok) Log.Warn($"mcvideo end reception {cur.UserId}: {e}");
        }
        foreach (var other in Sessions.Where(x => x.IsMcVideo && x != s && x.Receiving is not null).ToList())   // 다른 채널에서 보던 것도(창 하나)
            Engine.GetCall(other.CallId).EndReception(other.Receiving!.UserId);
        var r = call.AcceptReception(transmitterId);
        RefreshTransmission(s);
        return Show(r, ResponseText.Area.Video);
    }

    /// <summary>[그만 보기] — Media Reception End Request(§6.2.5.5). 영상 호는 남는다 — 다른 송출을 다시 고를 수 있다.</summary>
    public Result EndVideo(SessionItem s, string transmitterId)
    {
        var r = Engine.GetCall(s.CallId).EndReception(transmitterId);
        RefreshTransmission(s);
        return Show(r, ResponseText.Area.Video);
    }

    /// <summary>보내는 사람마다 기억하는 영상 회전(°, 시계 방향 0·90·180·270 — 송출자 번호 키). 영상 칸은 세로 480×640(3:4)이 기본이고, 보내는 쪽 카메라 방향이
    /// 다르면 관제사가 90° 씩 돌려 맞춘다(§10.3). 앱이 켜져 있는 동안 기억한다.</summary>
    private readonly Dictionary<string, int> _videoRotation = new(StringComparer.OrdinalIgnoreCase);
    public int VideoRotationOf(string transmitterId) => _videoRotation.GetValueOrDefault(UserPartConverter.UserPart(transmitterId));
    /// <summary>[↻]/[↺] — 90° 씩(시계 방향 +90, 반시계 −90). 돌린 값을 돌려준다.</summary>
    public int RotateVideo(string transmitterId, int delta)
    {
        string key = UserPartConverter.UserPart(transmitterId);
        int v = ((_videoRotation.GetValueOrDefault(key) + delta) % 360 + 360) % 360;
        if (v == 0) _videoRotation.Remove(key); else _videoRotation[key] = v;
        return v;
    }

    /// <summary>[영상 소리](§10.5 — TS 22.280 R-8.3-002 동시 오디오 원천의 상대 음량) — 영상 호 수신 음량 0~2.</summary>
    public void SetVideoVolume(SessionItem s, float level) => Engine.GetCall(s.CallId).SetRxLevel(Math.Clamp(level, 0f, 2f));

    /// <summary>«새 영상» 배너 [보기] — 그 송출을 본다(채널 상세는 MainViewModel 이 연다).</summary>
    public void AcceptVideoBanner(Banner b)
    {
        if (b.Session is { } s && b.Transmitter.Length > 0) AcceptVideo(s, b.Transmitter);
        Notify.RemoveBanner(b);
    }

    public SessionItem? VideoOfGroup(string groupId) => Sessions.FirstOrDefault(s => s.IsMcVideo && s.Info.GroupId == groupId);
    private GroupInfo? GroupOf(SessionItem s) => Groups.FirstOrDefault(g => g.IsMember && g.Id == s.Info.GroupId);
    /// <summary>송출자 표시 — 기능 별칭이 있으면 «이름(별칭)».</summary>
    public string VideoSenderName(VideoTransmitter t) => t.FunctionalAlias.Length > 0 ? $"{NameOfPtt(t.UserId)}({t.FunctionalAlias})" : NameOfPtt(t.UserId);

    // ── 세션 투영 ──

    /// <summary>영상 호가 생겼다 — 그 그룹 카드의 «영상» 절에 붙인다(카드를 새로 만들지 않는다).</summary>
    private void OnVideoSessionAdded(SessionItem s)
    {
        _videoJoining.Remove(s.Info.GroupId);
        if (GroupOf(s) is { } g)
        {
            g.VideoSession = s; g.VideoNote = "";
            VideoChanged?.Invoke(this, g);
        }
        RefreshTransmission(s);
        Log.Info($"mcvideo session + #{s.CallId} {s.Info.GroupId} {(s.Info.Dir == CallDir.Incoming ? "invited" : "joined")}");
    }

    private void OnVideoSessionEnded(SessionItem s, string dur)
    {
        var ci = s.Info;
        bool mine = _videoLeaving.Remove(s.CallId);
        _videoJoining.Remove(ci.GroupId);
        if (Notify.VideoBannerOf(s) is { } b) Notify.RemoveBanner(b);
        Unduck(ci.GroupId);
        Log.Info($"mcvideo session - #{s.CallId} {ci.GroupId} code={ci.LastCode} {ci.LastReason} mine={mine} connected={s.ConnectedAt is not null}");
        if (GroupOf(s) is not { } g) return;
        if (g.VideoSession == s) g.VideoSession = null;
        // 채널은 남아 있다(D10) — 관제사가 끝낸 게 아니면 다시 합류한다(chat). 서지 못한 합류(4xx~6xx)는 물러나고, 섰다가 끝난 호(TNG3·서버 해제)는 곧바로
        if (!mine && McVideoEnabled && g.McVideo && g.McVideoType == "chat")
            VideoBackoff(g, failed: s.ConnectedAt is null || ci.LastCode >= 300, ci.LastCode);
        else if (g.McVideoType == "prearranged") g.VideoNote = "편성 영상 그룹 — 멤버가 영상 호를 열면 함께 합류합니다";
        VideoChanged?.Invoke(this, g);
    }

    /// <summary>코어 전송 제어 현재값을 다시 읽는다 — 송출 목록·보는 중·무전 음량·배너가 이것의 투영이다.</summary>
    private void RefreshTransmission(SessionItem s)
    {
        s.Transmission = Engine.GetCall(s.CallId).TransmissionInfo;
        UpdateDuck(s);
        if (Notify.VideoBannerOf(s) is { } b && !s.Transmitters.Any(t => t.State == ReceptionState.Notified && string.Equals(t.UserId, b.Transmitter, StringComparison.OrdinalIgnoreCase)))
            Notify.RemoveBanner(b);                                   // 그 송출을 보기 시작했거나 송출이 끝났다
        if (GroupOf(s) is { } g) VideoChanged?.Invoke(this, g);
    }

    /// <summary>내 송출(§6.2.4) — 1차 관제 앱은 [영상 보내기]를 비활성(카메라 없음, D11)으로 두므로 서버발 거절·회수만 알린다.</summary>
    private void OnTransmission(TransmissionEvent e)
    {
        if (Find(e.CallId) is not { IsMcVideo: true } s) return;
        Log.Info($"mcvideo tx #{e.CallId} {e.Kind} state={e.State} cause={e.Cause} {e.CauseText}");
        if (e.Kind is TransmissionEventKind.Rejected or TransmissionEventKind.Revoked)
            Notify.Warn($"{s.Title} — {ResponseText.VideoTransmissionText(e.Kind, e.Cause, GroupOf(s)?.McVideoMaxTransmitters ?? 0)}", $"#{e.Cause} {e.CauseText}");
        RefreshTransmission(s);
    }

    /// <summary>수신 제어(§6.2.5) — 새 송출 알림·보기·끝. 새 송출은 «새 영상» 배너(채널마다 하나 — 가장 최근 송출)로 알린다: 송출자는 아무도 보지 않으면
    /// 서버가 송출을 끝내는 시한(T11, 기본 10 s — TS 24.581 §6.3.4.4.13 #8)을 기다리고 있다.</summary>
    private void OnReception(ReceptionEvent e)
    {
        if (Find(e.CallId) is not { IsMcVideo: true } s) return;
        var t = e.Transmitter;
        string who = VideoSenderName(t);
        Log.Info($"mcvideo rx #{e.CallId} {e.Kind} from={t.UserId} state={t.State} auto={t.Automatic} cause={e.Cause} {e.CauseText}");
        RefreshTransmission(s);
        string num = UserPartConverter.UserPart(t.UserId);
        switch (e.Kind)
        {
            case ReceptionEventKind.Notified when t.State == ReceptionState.Notified && !t.Automatic && s.Transmitters.Any(x => x.UserId == t.UserId):
                if (!s.TransmitterAnnounced.Add(t.UserId)) break;          // 거절·그만 보기 뒤의 Notified 복귀는 새 송출이 아니다
                Activity.Add(ActivityPanel.Ptt, ActivityKind.Video, $"{s.Title} {who} 영상 보내기 시작", "", number: num);
                if (Notify.VideoBannerOf(s) is { } old) Notify.RemoveBanner(old);
                Notify.ShowBanner(new Banner { Kind = BannerKind.Video, Title = $"새 영상 · {s.Title}", Subtitle = $"{ResponseText.WithIGa(who)} 영상을 보냅니다", Session = s, GroupId = s.Info.GroupId, Transmitter = t.UserId });
                break;
            case ReceptionEventKind.Granted:
                Activity.Add(ActivityPanel.Ptt, ActivityKind.Video, $"{s.Title} {who} 영상 보기", "", number: num);
                break;
            case ReceptionEventKind.Rejected:
            case ReceptionEventKind.RequestTimeout:
                Notify.Error($"{s.Title} — {ResponseText.VideoReceptionText(e.Kind, e.Cause, who)}", $"#{e.Cause} {e.CauseText}");
                Activity.Add(ActivityPanel.Ptt, ActivityKind.Error, $"{s.Title} {who} 영상 보기 실패", e.CauseText, number: num);
                break;
            case ReceptionEventKind.Ended:
                s.TransmitterAnnounced.Remove(t.UserId);
                Activity.Add(ActivityPanel.Ptt, ActivityKind.Video, $"{s.Title} {who} 영상 보내기 끝", "", number: num);
                if (e.Transmitter.State == ReceptionState.Ended && _watchedLast.Remove((s.CallId, t.UserId))) Notify.Info($"{s.Title} — {ResponseText.WithIGa(who)} 영상 보내기를 멈췄습니다");
                break;
            case ReceptionEventKind.Released:
                Activity.Add(ActivityPanel.Ptt, ActivityKind.Video, $"{s.Title} {who} 영상 그만 보기", "", number: num);
                break;
            case ReceptionEventKind.EndRequested:
                Notify.Info($"{s.Title} — {who} 영상 보기가 끝났습니다", $"#{e.Cause} {e.CauseText}");
                Activity.Add(ActivityPanel.Ptt, ActivityKind.Video, $"{s.Title} {who} 영상 보기 끝(서버)", e.CauseText, number: num);
                break;
        }
        if (e.Kind == ReceptionEventKind.Granted) _watchedLast.Add((s.CallId, t.UserId));
        else if (e.Kind is ReceptionEventKind.Released or ReceptionEventKind.EndRequested) _watchedLast.Remove((s.CallId, t.UserId));
    }
    /// <summary>보고 있던 송출(callId, 송출자) — 송출자가 멈추면 «멈췄습니다» 를 알린다(보지 않던 송출의 끝은 «이벤트» 줄만).</summary>
    private readonly HashSet<(int, string)> _watchedLast = new();

    // ── 소리(D6 — 영상 호 송출 음성 우선) ──

    /// <summary>같은 그룹의 영상을 보는 동안 그 그룹 무전의 수신 음량을 줄이고, 보기가 끝나면 되돌린다. 다른 그룹 무전은 그대로(§10.5).</summary>
    private void UpdateDuck(SessionItem video)
    {
        if (video.Receiving is null) { Unduck(video.Info.GroupId); return; }
        if (SessionOfGroup(video.Info.GroupId) is not { } voice || !_ducked.Add(voice.CallId)) return;
        var r = Engine.GetCall(voice.CallId).SetRxLevel(VideoDuckLevel);
        if (!r.Ok) { _ducked.Remove(voice.CallId); Log.Warn($"duck #{voice.CallId}: {r}"); }
    }

    private void Unduck(string groupId)
    {
        if (SessionOfGroup(groupId) is not { } voice || !_ducked.Remove(voice.CallId)) return;
        Engine.GetCall(voice.CallId).SetRxLevel(1f);
    }

    /// <summary>무전 호가 새로 생기면(영상을 보는 중에 무전 합류) 줄임을 다시 판정한다.</summary>
    private void ReapplyDuck(SessionItem voice)
    {
        if (voice.Kind == SessionKind.PttChannel && VideoOfGroup(voice.Info.GroupId) is { } v) UpdateDuck(v);
    }
}
