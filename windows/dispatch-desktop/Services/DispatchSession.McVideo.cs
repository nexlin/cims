// MCVideo 그룹 영상(dispatch_desktop_ui.md §10 — 규격 모델·결정 정본 mcvideo.md §7 D8·D10~D12). 자격·영상 채널 합류·송출/수신 이벤트·무전 음량(D6).
//   MCVideo 는 MCPTT 와 나란한 서비스다(TS 23.280 §3) — 영상 호는 카드가 아니라 그 그룹의 «영상» 절에 붙고, 음성(MCPTT 호)과 따로 든다
//   (TS 24.281 §7.1 — 한 등록을 공유하는 독립 다이얼로그).
//   D10 «영상 참여» 단계가 없다 — 내 채널의 MCVideo 그룹은 앱이 영상 호에도 함께 합류한다(chat = 합류, prearranged = MCVideo affiliation +
//   멤버 초대 자동 수락, TS 22.280 R-8.4.2-002). «채널» = 관제사가 내 채널에 둔 의도라 무전 세션의 T4·TNG3 해제(TS 24.379 §6.3.8.1)와 무관하게 이어지고,
//   영상 호가 끝나면 다시 합류한다. 끝내는 것은 그룹이 내 채널에서 빠질 때·로그아웃뿐.
//   D8 수신 manual — 송출을 골라 [보기](한 번에 하나 — 1차 수신 상한 1), [바꿔 보기] = 보던 것을 그만 보고 새것을 본다.
//   그림 = 엔진 프레임(창 없는 프레임 렌더 — ue_sdk.md §4.5)을 VideoFrames 우편함이 보는 칸에만 넘긴다. 내 송출(D11)은 [영상 보내기] — 허가에서만 코어가
//   카메라·영상 호 마이크를 연다. D12 마이크 경합 = 음성 우선(무전 발언 동안 영상 호 음성만 멈춤) / 영상 우선(영상 송출 동안 무전 발언 막음, 긴급·임박 예외).
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
    /// <summary>내가 연 편성(prearranged) 영상 호(callId) — 성립 전에는 [영상 보내기] 가 «여는 중…» 이고 다시 누르면 개시를 거둔다(CANCEL), 성립 전에 끝나면
    /// 다시 열지 않고 사유만 알린다(TS 24.281 §9.2.1.2.1.1 · §9.2.1.4.2). chat 합류의 자동 재시도(물러남)와 섞지 않는다.</summary>
    private readonly HashSet<int> _videoOpening = new();
    /// <summary>내가 거둔 개시(CANCEL) — 실패 토스트(487)를 내지 않는다.</summary>
    private readonly HashSet<int> _quietVideoEnd = new();
    /// <summary>마지막으로 맞춘 영상 채널 그룹 문서의 지문(ETag) — 문서가 바뀌었을 때만 물러남을 풀고 affiliation 을 다시 싣는다(같은 문서 재조회로는 풀지 않는다).</summary>
    private readonly Dictionary<string, string> _mcvideoDocPrint = new(StringComparer.OrdinalIgnoreCase);
    /// <summary>동시 제휴 그룹 한도(N2)를 넘어 MCVideo 로 affiliate 하지 않은 영상 채널 — chat 합류도 하지 않는다(서버가 486 Warning 102).</summary>
    private readonly HashSet<string> _videoOverN2 = new(StringComparer.OrdinalIgnoreCase);
    private const string PrearrangedNote = "편성 영상 그룹 — [영상 보내기] 로 영상 호를 엽니다";
    private const string OpeningNote = "영상 호를 여는 중 — 멤버가 받기를 기다립니다";

    /// <summary>영상 절이 바뀌었다(연결·송출·수신) — 카드·채널 상세가 다시 그린다.</summary>
    public event EventHandler<GroupInfo>? VideoChanged;
    /// <summary>영상 그림 우편함(§10.3) — 엔진 프레임(영상 스레드)을 보는 칸(호별·셀프뷰)에만 넘긴다.</summary>
    public VideoFrames VideoFrames { get; }
    /// <summary>[영상 보내기] 카메라(엔진 영상 장치 이름) — 비면 이 PC 에 카메라가 없다(송출 버튼 비활성).</summary>
    public string CameraName { get; private set; } = "";
    public bool HasCamera => CameraName.Length > 0;
    /// <summary>D12 — 내가 무전 발언(요청·대기·발언) 중이라 영상 송출 호의 음성을 멈춰 둔 동안 true(채널 상세 «무전 중 — 영상은 계속» 안내).</summary>
    public bool VideoMicYielded { get; private set; }
    /// <summary>D12 음성 우선으로 음성을 멈춘 영상 호(callId).</summary>
    private readonly HashSet<int> _videoMicMuted = new();
    private DateTime _videoBlockNotedAt;

    private string McVideoPsi => _ueInit?.McvideoServerUri ?? "";
    /// <summary>동시 MCVideo 호 상한 N6(user profile &lt;MaxSimultaneousCallsN6&gt;) — 넘으면 서버가 486 103. 모르면 상한 없음으로 본다.</summary>
    private int McVideoN6 => _mcvideoProfile is { MaxSimultaneousCallsN6: > 0 } p ? p.MaxSimultaneousCallsN6 : int.MaxValue;
    /// <summary>동시 MCVideo 제휴 그룹 한도 N2(user profile &lt;MaxAffiliationsN2&gt; — 회선 «동시 제휴 그룹», 기본 4). 넘는 그룹은 서버가 제휴 PUBLISH 에서 줄인다. 모르면 상한 없음.</summary>
    private int McVideoN2 => _mcvideoProfile is { MaxAffiliationsN2: > 0 } p ? p.MaxAffiliationsN2 : int.MaxValue;

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
        var members = Groups.Where(x => x.IsMember).ToList();
        // 켜는 그룹을 먼저, 끄는 그룹을 뒤에 — 코어 PUBLISH 는 관심 그룹 전부를 싣는데, 먼저 끄면 빈 집합(Expires 0 = 그 사용자 MCVideo 제휴 전부 해제)을 한 번 거친다.
        // 영상 채널은 카드 순서로 N2 개까지만 affiliate 한다 — 넘는 그룹은 서버가 PUBLISH 에서 줄이고(제휴 NOTIFY 에 없다) chat 합류는 486 Warning 102 다.
        int n = 0;
        foreach (var g in members.Where(x => McVideoEnabled && ids.Contains(x.Id)))
        {
            g.McVideo = true;
            bool over = n >= McVideoN2;
            if (over)
            {
                if (_videoOverN2.Add(g.Id)) Log.Warn($"mcvideo channel {g.Id}: not affiliated — N2 {McVideoN2} reached");
                g.VideoNote = $"동시 제휴 그룹 한도(N2 = {McVideoN2})가 찼습니다 — 이 채널 영상은 연결하지 않았습니다(운영자에게 한도 상향 요청)";
                continue;
            }
            n++;
            if (_videoOverN2.Remove(g.Id)) g.VideoNote = "";
            if (ptt is null || g.McVideoAffiliated) continue;
            var r = ptt.Affiliate(g.Id, true, McService.McVideo);
            if (!r.Ok) { Log.Warn($"mcvideo affiliate {g.Id} True: {r}"); continue; }
            g.McVideoAffiliated = true;
        }
        foreach (var g in members.Where(x => !(McVideoEnabled && ids.Contains(x.Id))))
        {
            if (g.McVideo || g.McVideoAffiliated) LeaveVideoChannel(g);   // 영상 호를 끊고 MCVideo 제휴도 푼다
            g.McVideo = false;
        }
        foreach (var g in members.Where(x => x.McVideoAffiliated && _videoOverN2.Contains(x.Id))) LeaveVideoChannel(g);   // 한도가 줄어 밀려난 채널
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
        ApplyMcVideoDoc(g, r.Value);
    }

    /// <summary>그룹 문서의 MCVideo 몫을 그 채널에 맞춘다 — 호 방식·동시 송출 상한이 바뀌었으면 갱신하고, **문서가 바뀌었으면**(지문 = 문서 ETag — 처음 받은 것과
    /// 같은 문서 재조회는 변경이 아니다) 그 그룹의 합류 물러남을 지우고 MCVideo affiliation 을 다시 싣는다. 갓 만든 그룹은 서버가 아직 MCVideo 그룹으로 모를 때
    /// 합류가 거절되거나(404 Warning 113) affiliation 이 기록되지 않을 수 있는데(PUBLISH 는 200 — 결과는 NOTIFY 로만 온다, TS 24.281 §8.2.2.2.3) 서버는 그룹을
    /// 다시 적재한 뒤 문서 변경을 통지한다 — 그때 다시 맞춘다. affiliation 재호출 = 코어가 관심 그룹 집합을 다시 PUBLISH(§8.2.1.2).</summary>
    private void ApplyMcVideoDoc(GroupInfo g, GroupDoc doc)
    {
        string type = doc.Mcvideo is { } a ? (a.InviteMembers ? "prearranged" : "chat") : "";
        int max = doc.Mcvideo?.MaxTransmitters ?? 0;
        string print = doc.ETag.Length > 0 ? doc.ETag : $"{type}|{max}|{string.Join(',', doc.Members.Select(m => m.Uri))}";
        bool changed = _mcvideoDocPrint.TryGetValue(g.Id, out var old) && old != print;
        _mcvideoDocPrint[g.Id] = print;
        if (type != g.McVideoType || max != g.McVideoMaxTransmitters || changed)
            // 영상 채널이 왜 연결되거나 안 되는지 로그에서 가른다 — chat = 앱이 합류, prearranged = [영상 보내기] 가 열거나 멤버의 초대를 받는다
            Log.Info($"mcvideo channel {g.Id}: {(type.Length > 0 ? type : "(no MCVideo part)")} max-tx={max}{(changed ? " — document changed" : "")}");
        g.McVideoType = type; g.McVideoMaxTransmitters = max;
        if (type.Length == 0) g.VideoNote = "그룹 문서에 영상(MCVideo) 몫이 없습니다";
        else if (g.VideoSession is null && !_videoJoining.Contains(g.Id) && !_videoOverN2.Contains(g.Id)) g.VideoNote = type == "prearranged" ? PrearrangedNote : "";
        if (!changed) return;
        _videoRetryAt.Remove(g.Id); _videoFailures.Remove(g.Id);
        if (Ptt is { } ptt && g.McVideo && !_videoOverN2.Contains(g.Id))
        {
            var r = ptt.Affiliate(g.Id, true, McService.McVideo);
            if (r.Ok) g.McVideoAffiliated = true; else Log.Warn($"mcvideo re-affiliate {g.Id}: {r}");
        }
    }

    /// <summary>그룹 문서가 바뀌었을 수 있다(GMS xcap-diff) — 영상 채널의 그룹 문서를 다시 받아 맞춘다: 콘솔에서 호 방식(편성 ↔ chat)·동시 송출 상한을 바꾸면
    /// 로그인 중에도 따라가고, 문서가 바뀐 채널은 물러남·affiliation 을 새로 한다(<see cref="ApplyMcVideoDoc"/>).</summary>
    private async Task RefreshMcVideoDocsAsync()
    {
        if (!McVideoEnabled) return;
        foreach (var g in Groups.Where(x => x.IsMember && x.McVideo).ToList())
        {
            if (!_mcvideoAttrsLoading.Add(g.Id)) continue;
            try
            {
                var r = await GetGroupAsync(g);
                if (r.Ok) ApplyMcVideoDoc(g, r.Value); else Log.Warn($"mcvideo attrs {g.Id}: {r}");
            }
            finally { _mcvideoAttrsLoading.Remove(g.Id); }
            VideoChanged?.Invoke(this, g);
        }
        EnsureVideoChannels();
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
            if (g.VideoSession is not null || _videoJoining.Contains(g.Id) || _videoOverN2.Contains(g.Id)) continue;
            if (_videoRetryAt.TryGetValue(g.Id, out var at) && now < at) continue;
            if (g.McVideoType.Length == 0) { _ = EnsureMcVideoAttrsAsync(g); continue; }        // 호 방식을 안 뒤 다시
            // 편성 = 앱이 호를 열어 두지 않는다(전원 초대라 보낼 사람이 연다 — [영상 보내기], OpenVideoTx). 멤버가 열면 초대를 자동 수락한다
            if (g.McVideoType == "prearranged") { g.VideoNote = PrearrangedNote; continue; }
            if (live >= McVideoN6)
            {
                string note = $"동시 영상 호 한도(N6 = {McVideoN6})가 찼습니다 — 이 채널 영상은 연결하지 않았습니다(운영자에게 한도 상향 요청)";
                if (g.VideoNote != note) Log.Warn($"mcvideo channel {g.Id}: not joined — N6 {McVideoN6} reached ({live} live)");
                g.VideoNote = note; continue;
            }
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
        if (VideoOfGroup(g.Id) is { } v) { _videoLeaving.Add(v.CallId); _quietVideoEnd.Add(v.CallId); Engine.GetCall(v.CallId).Hangup(); }
        _videoJoining.Remove(g.Id); _videoRetryAt.Remove(g.Id); _videoFailures.Remove(g.Id); _mcvideoDocPrint.Remove(g.Id);
        // 떠나면 MCVideo 제휴도 푼다(영상 호 BYE 다음에) — chat 합류의 암묵적 제휴는 나갈 때 풀어 주는 절차가 규격에 없어(해제는 클라이언트 몫 — TS 24.281 §8.2.1.2)
        //   그대로 두면 옮겨 다닌 채널이 쌓여 N2 를 넘고 새 채널 합류가 486 Warning 102 로 막힌다
        if (g.McVideoAffiliated && Ptt is { } ptt)
        {
            var r = ptt.Affiliate(g.Id, false, McService.McVideo);
            if (!r.Ok) Log.Warn($"mcvideo affiliate {g.Id} False: {r}");
        }
        g.McVideoAffiliated = false;
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
        _videoOpening.Clear(); _quietVideoEnd.Clear(); _mcvideoDocPrint.Clear(); _videoOverN2.Clear();
        _videoMicMuted.Clear(); VideoMicYielded = false;
        VideoFrames.Remove(VideoFrames.SelfView);
        foreach (var b in Notify.Banners.Where(b => b.IsVideo).ToList()) Notify.RemoveBanner(b);
    }

    // ── 영상 장치(§10.3 — 엔진 = DirectShow 캡처 + 창 없는 프레임 렌더) ──

    /// <summary>카메라·셀프뷰를 엔진에 건다 — 기동 뒤·설정 저장 뒤. 카메라는 설정 이름으로 찾고(id 는 재부팅마다 바뀔 수 있다) 없으면 첫 카메라.
    /// 셀프뷰는 늘 켜 둔다 — 코어가 송출 중일 때만 카메라 프레임을 넘긴다(셀프뷰만으로 카메라를 열지 않는다).</summary>
    public void ApplyVideoSettings()
    {
        if (!Engine.IsRunning) return;
        var cams = Cameras();
        string want = Settings.Current.VideoCaptureDevice;
        var cam = cams.FirstOrDefault(d => d.Name == want) ?? cams.FirstOrDefault();
        CameraName = cam?.Name ?? "";
        if (cam is not null && Engine.SetVideoCaptureDevice(cam.Id) is { Ok: false } r) Log.Warn($"video capture device {cam.Name}: {r}");
        if (Engine.SetVideoPreview(true) is { Ok: false } p) Log.Warn($"video self view: {p}");
        Log.Info($"video camera={(cam is null ? "(없음)" : $"{cam.Name} [{cam.Driver}] #{cam.Id}")} cameras={cams.Count}" +
                 (want.Length > 0 && cam?.Name != want ? $" — 설정 카메라 '{want}' 없음" : ""));
        foreach (var g in Groups.Where(x => x.McVideo)) VideoChanged?.Invoke(this, g);
    }

    /// <summary>화면 점검(--ui-preview-videotx) — 엔진 없이 카메라가 있는 것처럼.</summary>
    internal void SeedPreviewCamera(string name) => CameraName = name;

    /// <summary>엔진이 아는 카메라(합성 색 막대·AVI 재생기 제외). 엔진이 서기 전엔 빈 목록 — 장치는 엔진 기동 때 한 번 열거한다.</summary>
    public IReadOnlyList<VideoDeviceInfo> Cameras() =>
        Engine.IsRunning ? Engine.VideoDevices.Where(d => d.IsCamera).ToList() : Array.Empty<VideoDeviceInfo>();

    // ── 내 송출(§10.3 — D11 음성과 영상의 송출은 따로, TS 24.581 §6.2.4) ──

    /// <summary>[영상 보내기] — Transmission Request(TS 24.581 §6.2.4.3.2). 허가가 오면 코어가 카메라·영상 호 마이크를 연다(허가에서만 — §6.2.4.4.6).</summary>
    public Result RequestVideoTx(SessionItem s)
    {
        if (!HasCamera) return Show(Result.Fail(-1, "카메라 없음"), ResponseText.Area.Video);
        var r = Engine.GetCall(s.CallId).RequestTransmission();
        RefreshTransmission(s);
        return Show(r, ResponseText.Area.Video);
    }

    /// <summary>편성(prearranged) 영상 채널에 영상 호가 없다 — [영상 보내기] 가 영상 호를 연다(초대 대상이 되려면 MCVideo 제휴가 서 있어야 한다).</summary>
    public bool CanOpenVideo(GroupInfo g) =>
        McVideoEnabled && g.IsMember && g.McVideo && g.McVideoType == "prearranged" && g.McVideoAffiliated && g.VideoSession is null && !_videoJoining.Contains(g.Id)
        && HasCamera && Ptt is not null && PttReg.State == RegState.Registered;

    /// <summary>내가 연 편성 영상 호가 아직 성립 전인가 — [영상 보내기] 는 «여는 중…», 다시 누르면 개시를 거둔다.</summary>
    public bool IsVideoOpening(SessionItem? s) => s is { IsLive: true, IsActive: false } && _videoOpening.Contains(s.CallId);

    /// <summary>[영상 보내기](편성 영상 채널, 영상 호 없음) — prearranged MCVideo 그룹 호를 연다(TS 24.281 §9.2.1.2.1.1). 송출 요청은 개시 INVITE 에 싣는다
    /// (16) · §6.4 — TS 24.581 §14.2.4 `mc_implicit_request`). 제어 기능이 MCVideo 로 affiliate 한 멤버를 초대하고 첫 멤버가 붙으면 200 OK, 10 s 안에 아무도 붙지 않으면
    /// 480(§9.2.1.4.2). 그사이 다른 멤버가 먼저 열었으면 서버가 합류로 받고 암묵 요청은 받지 않는다(§14.3.5) — 코어가 명시 Transmission Request 로 잇는다.
    /// 코어는 호 성립 전에는 송출 이벤트를 내지 않으므로 «여는 중…» 은 앱이 그린다. 실패는 다시 열지 않는다 — 사용자가 다시 누른다.</summary>
    public Result OpenVideoTx(GroupInfo g)
    {
        if (!HasCamera) return Show(Result.Fail(-1, "카메라 없음"), ResponseText.Area.Video);
        if (Ptt is not { } ptt || !CanOpenVideo(g)) return Result.Fail(-1, "영상 호를 열 수 없는 상태");
        if (Sessions.Count(s => s.IsMcVideo && s.IsLive) + _videoJoining.Count >= McVideoN6)
            return Show(Result.Fail(486, $"동시 영상 호 한도 N6 = {McVideoN6}"), ResponseText.Area.Video);
        var r = ptt.JoinVideoGroupCall(g.Id, new VideoGroupCallOptions { Prearranged = true, ImplicitTransmissionRequest = true, Queueing = true });
        if (!r.Ok) { Log.Warn($"mcvideo open {g.Id}: {r}"); return Show(Result.Fail(r.Code, r.Reason), ResponseText.Area.Video); }
        _pendingOps[r.Value.Id] = Operation.VideoJoin;
        _videoJoining.Add(g.Id); _videoOpening.Add(r.Value.Id);
        Log.Info($"mcvideo open prearranged {g.Id} → call {r.Value.Id} (implicit transmission request)");
        if (Find(r.Value.Id) is null && r.Value.Info.IsLive) OnCallState(r.Value.Info);
        if (g.VideoSession is null) g.VideoNote = OpeningNote;
        VideoChanged?.Invoke(this, g);
        return Result.Success;
    }

    /// <summary>«여는 중…» 에 다시 누름 — 개시를 거둔다(hangup → CANCEL). 실패 토스트(487)는 내지 않는다.</summary>
    public Result CancelVideoOpen(SessionItem s)
    {
        _quietVideoEnd.Add(s.CallId);
        var r = Engine.GetCall(s.CallId).Hangup();
        return Show(r, ResponseText.Area.Video);
    }

    /// <summary>끝난 영상 호의 실패 토스트를 내지 않을 것인가(내가 거둔 개시·내가 떠난 채널) — 호 종료 처리가 한 번 묻는다.</summary>
    private bool QuietVideoEnd(int callId) => _quietVideoEnd.Remove(callId);

    /// <summary>[보내기 끝]·[요청 취소]·[대기 취소] — Transmission End Request(§6.2.4.5.3·§6.2.4.4.7·§6.2.4.9.4). 끝나면 코어가 카메라를 닫는다.</summary>
    public Result ReleaseVideoTx(SessionItem s)
    {
        var r = Engine.GetCall(s.CallId).ReleaseTransmission();
        RefreshTransmission(s);
        return Show(r, ResponseText.Area.Video);
    }

    /// <summary>영상을 보내는(요청·대기 포함) 영상 호가 있다.</summary>
    private bool SendingVideo => Sessions.Any(v => v.IsMcVideo && v.IsLive && v.Transmission.State is TransmissionState.Permitted or TransmissionState.PendingRequest or TransmissionState.Queued);

    /// <summary>D12 영상 우선 — 영상을 보내는 동안 무전 발언 요청을 막는다(긴급·임박 채널은 늘 음성 — TS 22.280 R-8.3-004). 막으면 한 번 알린다.</summary>
    private bool BlocksTalkForVideo(SessionItem s)
    {
        if (Settings.Current.VideoMicPolicy != "video" || s.IsMcVideo || !SendingVideo) return false;
        if (s.Info.Condition.Emergency || s.Info.Condition.ImminentPeril) return false;
        if (DateTime.Now - _videoBlockNotedAt > TimeSpan.FromSeconds(2))
        {
            _videoBlockNotedAt = DateTime.Now;
            Notify.Warn("영상을 보내는 중 — 무전 발언을 막았습니다", "설정 «영상 보내는 중 무전» = 영상 우선. 긴급·임박 채널은 그대로 말할 수 있습니다.");
        }
        return true;
    }

    /// <summary>D12 음성 우선(마이크 경합 — TS 22.280 R-8.3-003) — 내가 무전 발언(요청·대기·발언) 중인 동안 영상 송출 호의 음성만 멈추고(코어 setMuted =
    /// 오디오 인코더 정지, 영상은 계속 — ue_sdk.md §4.2), 끝나면 되돌린다. 영상 우선이라도 긴급·임박 발언은 여기를 지난다(마이크가 두 호로 겹치지 않게).</summary>
    private void SyncVideoMic()
    {
        bool talking = Sessions.Any(x => !x.IsMcVideo && x.IsLive && x.Info.IsMcptt && x.Floor.State is FloorState.Requesting or FloorState.Speaking or FloorState.Queued);
        bool yielded = false;
        foreach (var v in Sessions.Where(x => x.IsMcVideo && x.IsLive).ToList())
        {
            bool mute = talking && v.Transmission.State == TransmissionState.Permitted;
            yielded |= mute;
            if (mute == _videoMicMuted.Contains(v.CallId)) continue;
            var r = Engine.GetCall(v.CallId).SetMuted(mute);
            if (!r.Ok) { Log.Warn($"video mic #{v.CallId} {mute}: {r}"); continue; }
            if (mute) _videoMicMuted.Add(v.CallId); else _videoMicMuted.Remove(v.CallId);
            Log.Info($"mcvideo #{v.CallId} audio {(mute ? "yielded to voice talk" : "resumed")} (D12)");
        }
        if (yielded == VideoMicYielded) return;
        VideoMicYielded = yielded;
        foreach (var g in Groups.Where(x => x.McVideo)) VideoChanged?.Invoke(this, g);
    }

    // ── 관제 동작 ──

    /// <summary>[보기]·[바꿔 보기](§10.4) — Receive Media Request(TS 24.581 §6.2.5.3.3). 한 번에 하나 — 보던 송출이 있으면 먼저 그만 본다(1차 수신 상한 1).</summary>
    public Result AcceptVideo(SessionItem s, string transmitterId)
    {
        var call = Engine.GetCall(s.CallId);
        // 보는 중 = 받고 있거나(Receiving·PendingRelease) **요청해 둔**(PendingRequest) 송출 — 요청 중인 것을 빼면 [보기] 를 잇달아 눌렀을 때
        //   앞 요청이 살아남아 둘을 받게 된다(1차 수신 상한 1).
        if (Watching(s) is { } cur)
        {
            if (string.Equals(cur.UserId, transmitterId, StringComparison.OrdinalIgnoreCase)) return Result.Success;
            var e = call.EndReception(cur.UserId);
            if (!e.Ok) Log.Warn($"mcvideo end reception {cur.UserId}: {e}");
        }
        foreach (var other in Sessions.Where(x => x.IsMcVideo && x != s && Watching(x) is not null).ToList())   // 다른 채널에서 보던 것도(한 번에 하나)
            Engine.GetCall(other.CallId).EndReception(Watching(other)!.UserId);
        VideoFrames.Feed(s.CallId).Reset();                            // 앞 사람의 마지막 장이 남지 않게
        var r = call.AcceptReception(transmitterId);
        RefreshTransmission(s);
        return Show(r, ResponseText.Area.Video);
    }

    /// <summary>이 영상 호에서 보고 있는(또는 보기를 요청해 둔) 송출 — 없으면 null.</summary>
    private static VideoTransmitter? Watching(SessionItem s) =>
        s.Receiving ?? s.Transmission.Transmitters.FirstOrDefault(t => t.State == ReceptionState.PendingRequest);

    /// <summary>[그만 보기] — Media Reception End Request(§6.2.5.5). 영상 호는 남는다 — 다른 송출을 다시 고를 수 있다.</summary>
    public Result EndVideo(SessionItem s, string transmitterId)
    {
        var r = Engine.GetCall(s.CallId).EndReception(transmitterId);
        VideoFrames.Feed(s.CallId).Reset();
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
        var owner = GroupOf(s);
        // 붙을 «영상» 절이 없는 호 — 영상 채널이 아닌 그룹의 편성 초대(코어가 자동 수락한다), 또는 같은 그룹의 영상 호가 이미 있다(내가 여는 사이
        //   멤버의 초대가 먼저 붙었다 — 한 그룹에 영상 호는 하나). 화면 없이 남기지 않고 조용히 나간다.
        //   영상 채널인가는 자격의 그룹 목록(MCVideo user profile)으로 가른다 — 그룹 목록을 아직 못 받은 기동 직후의 초대를 버리지 않게.
        var live = owner?.VideoSession is { IsLive: true } cur && cur != s ? cur : null;
        bool channel = (_mcvideoProfile?.Groups ?? Array.Empty<string>()).Any(u => string.Equals(UserPartConverter.UserPart(u), s.Info.GroupId, StringComparison.OrdinalIgnoreCase));
        if (!channel || live is not null)
        {
            Log.Info($"mcvideo session #{s.CallId} {s.Info.GroupId}: {(live is not null ? $"duplicate of #{live.CallId}" : "not a video channel")} — leaving");
            bool mineOpening = _videoOpening.Contains(s.CallId);
            _videoLeaving.Add(s.CallId); _quietVideoEnd.Add(s.CallId);
            Engine.GetCall(s.CallId).Hangup();
            // 보내려고 연 호였다 — 남은 호가 성립해 있으면 거기에 명시 송출 요청으로 잇는다(TS 24.581 §6.2.4.3.2). 아니면 성립 뒤 다시 누른다.
            if (live is not null && mineOpening)
            {
                if (live.IsActive && live.Transmission.State == TransmissionState.NoPermission && HasCamera) RequestVideoTx(live);
                else Notify.Info($"{live.Title} — 영상 호에 합류하는 중입니다", "다른 멤버가 먼저 열었습니다 — 연결되면 [영상 보내기] 를 다시 누르세요");
            }
            return;
        }
        _videoJoining.Remove(s.Info.GroupId);
        if (GroupOf(s) is { } g)
        {
            g.VideoSession = s; g.VideoNote = _videoOpening.Contains(s.CallId) ? OpeningNote : "";
            VideoChanged?.Invoke(this, g);
        }
        RefreshTransmission(s);
        Log.Info($"mcvideo session + #{s.CallId} {s.Info.GroupId} {(s.Info.Dir == CallDir.Incoming ? "invited" : _videoOpening.Contains(s.CallId) ? "opening" : "joined")}");
    }

    private void OnVideoSessionEnded(SessionItem s, string dur)
    {
        var ci = s.Info;
        bool mine = _videoLeaving.Remove(s.CallId);
        bool opened = _videoOpening.Remove(s.CallId);                  // 내가 연 편성 호 — 성립 전에 끝났어도 다시 열지 않는다(사유는 호 종료 토스트)
        _videoJoining.Remove(ci.GroupId);
        if (Notify.VideoBannerOf(s) is { } b) Notify.RemoveBanner(b);
        Unduck(ci.GroupId);
        VideoFrames.Remove(s.CallId);
        _videoMicMuted.Remove(s.CallId);
        if (!Sessions.Any(x => x.IsMcVideo && x != s && x.Transmission.State == TransmissionState.Permitted)) VideoFrames.Feed(VideoFrames.SelfView).Reset();
        Log.Info($"mcvideo session - #{s.CallId} {ci.GroupId} code={ci.LastCode} {ci.LastReason} mine={mine} opened={opened} connected={s.ConnectedAt is not null}");
        if (GroupOf(s) is not { } g) return;
        if (g.VideoSession == s) g.VideoSession = null;
        // 채널은 남아 있다(D10) — 관제사가 끝낸 게 아니면 다시 합류한다(chat). 서지 못한 합류(4xx~6xx)는 물러나고, 섰다가 끝난 호(TNG3·서버 해제)는 곧바로
        if (!mine && McVideoEnabled && g.McVideo && g.McVideoType == "chat")
            VideoBackoff(g, failed: s.ConnectedAt is null || ci.LastCode >= 300, ci.LastCode);
        else if (g.McVideoType == "prearranged") g.VideoNote = PrearrangedNote;
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

    /// <summary>내 송출(§6.2.4) — 허가(카메라·영상 호 마이크는 코어가 연다)·대기·거절·회수·끝. 보는 사람 = Media Reception Notification(§6.2.4.4.8)이 올 때만 센다.</summary>
    private void OnTransmission(TransmissionEvent e)
    {
        if (Find(e.CallId) is not { IsMcVideo: true } s) return;
        Log.Info($"mcvideo tx #{e.CallId} {e.Kind} state={e.State} cause={e.Cause} {e.CauseText} queue={e.QueuePosition} rx={e.ReceiverId}");
        switch (e.Kind)
        {
            case TransmissionEventKind.Granted:
                s.TxSince = DateTime.Now; s.TxReceivers.Clear();
                Activity.Add(ActivityPanel.Ptt, ActivityKind.Video, $"{s.Title} 내 영상 보내기 시작", "");
                break;
            case TransmissionEventKind.ReceiverJoined when e.ReceiverId.Length > 0:
                s.TxReceivers.Add(UserPartConverter.UserPart(e.ReceiverId));
                break;
            case TransmissionEventKind.Rejected:
            case TransmissionEventKind.Revoked:
                Notify.Warn($"{s.Title} — {ResponseText.VideoTransmissionText(e.Kind, e.Cause, GroupOf(s)?.McVideoMaxTransmitters ?? 0)}", $"#{e.Cause} {e.CauseText}");
                if (e.Kind == TransmissionEventKind.Revoked) Activity.Add(ActivityPanel.Ptt, ActivityKind.Error, $"{s.Title} 내 영상 보내기 회수", e.CauseText);
                break;
            case TransmissionEventKind.RequestTimeout:
                Notify.Warn($"{s.Title} — 영상 보내기 요청에 응답이 없습니다");
                break;
        }
        if (e.State == TransmissionState.NoPermission && s.TxSince is not null)
        {
            Activity.Add(ActivityPanel.Ptt, ActivityKind.Video, $"{s.Title} 내 영상 보내기 끝", DispatchSession.Fmt(DateTime.Now - s.TxSince.Value));
            s.TxSince = null; s.TxReceivers.Clear();
            if (!Sessions.Any(x => x.IsMcVideo && x != s && x.Transmission.State == TransmissionState.Permitted)) VideoFrames.Feed(VideoFrames.SelfView).Reset();
        }
        RefreshTransmission(s);
        SyncVideoMic();
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
                if (s.Receiving is null) VideoFrames.Feed(s.CallId).Reset();
                break;
            case ReceptionEventKind.Released:
                Activity.Add(ActivityPanel.Ptt, ActivityKind.Video, $"{s.Title} {who} 영상 그만 보기", "", number: num);
                if (s.Receiving is null) VideoFrames.Feed(s.CallId).Reset();
                break;
            case ReceptionEventKind.EndRequested:
                Notify.Info($"{s.Title} — {who} 영상 보기가 끝났습니다", $"#{e.Cause} {e.CauseText}");
                Activity.Add(ActivityPanel.Ptt, ActivityKind.Video, $"{s.Title} {who} 영상 보기 끝(서버)", e.CauseText, number: num);
                if (s.Receiving is null) VideoFrames.Feed(s.CallId).Reset();
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
