// CimsUe — Account 래퍼: 계정 id 에 걸린 명령(engine.h 의 accountId 인자 함수들) + 그 계정에서 시작하는 호·구독·SDS.
using CimsUe.Native;
using static CimsUe.Native.NativeMethods;

namespace CimsUe;

public sealed unsafe class Account
{
    public Engine Engine { get; }
    /// <summary>코어 accountId.</summary>
    public int Id { get; }

    internal Account(Engine engine, int id) { Engine = engine; Id = id; }

    /// <summary>등록 상태 스냅샷.</summary>
    public RegInfo RegInfo => Engine.RegInfoOf(Id);

    // ── 등록 ──
    public Result Register() => Engine.Status(cimsue_engine_register_account(Engine.Handle, Id));
    public Result Unregister() => Engine.Status(cimsue_engine_unregister_account(Engine.Handle, Id));
    /// <summary>즉시 재-REGISTER(네트워크 복귀·서버 재기동 뒤 복구).</summary>
    public Result RefreshRegistration() => Engine.Status(cimsue_engine_refresh_registration(Engine.Handle, Id));
    /// <summary>발언권 참여자 타이머를 바꾼다(ue-init-config 가 바뀌었을 때) — 다음 MCPTT 호부터.</summary>
    public Result SetFloorTimers(FloorTimers t)
    {
        var n = Engine.ToNative(t);
        return Engine.Status(cimsue_engine_set_floor_timers(Engine.Handle, Id, in n));
    }
    public Result Remove() => Engine.Status(cimsue_engine_remove_account(Engine.Handle, Id));

    // ── 호 (VoLTE 1:1) ──
    /// <summary>발신. target 은 번호(도메인 자동 결합) 또는 sip: URI.</summary>
    public Result<Call> Dial(string target, CallOptions? opts = null)
    {
        cimsue_call_options_t o = ToNative(opts);
        return Engine.CallResult(cimsue_engine_dial(Engine.Handle, Id, target, &o));
    }

    // ── MCPTT 그룹콜·사설콜 (TS 24.379) ──
    /// <summary>그룹콜 참여. groupId 는 bare id. 이미 같은 그룹 세션이 있으면 그 호. ListenOnly = 청취 전용(관제 PTT 청취).
    /// Broadcast = 일제 통화 개시(개시자만 발언, 발언을 놓으면 코어가 호를 해제).</summary>
    public Result<Call> JoinGroupCall(string groupId, GroupCallOptions? opts = null)
    {
        using var s = new NativeStrings();
        cimsue_group_call_options_t o = ToNative(opts, s);
        return Engine.CallResult(cimsue_engine_join_group_call(Engine.Handle, Id, groupId, &o));
    }

    /// <summary>1:1 사설콜(session-type=private). peer 는 bare 번호. FullDuplex 면 mc_no_floor_ctrl.</summary>
    public Result<Call> StartPrivateCall(string peer, GroupCallOptions? opts = null)
    {
        using var s = new NativeStrings();
        cimsue_group_call_options_t o = ToNative(opts, s);
        return Engine.CallResult(cimsue_engine_start_private_call(Engine.Handle, Id, peer, &o));
    }

    /// <summary>긴급 경보 발신·취소(TS 24.379 §12.1.1.1·§12.1.1.2) — SIP MESSAGE(mcptt-info alert-ind). groupId bare.
    /// originatedBy = 다른 사용자의 경보를 취소할 때 그 사용자 MCPTT ID(§12.1.1.2 4)e)), cancelGroupEmergency = 취소와 함께 그룹의 진행 중 긴급도 해제.
    /// 반환 token — RequestCompleted(MESSAGE) 로 상관. 인가는 서버가 판정한다(앱은 Capabilities 로 선차단).</summary>
    public Result<long> SendEmergencyAlert(string groupId, bool activate, string? originatedBy = null, bool cancelGroupEmergency = false)
    {
        long t = cimsue_engine_send_emergency_alert(Engine.Handle, Id, groupId, Engine.B(activate), NullIfEmpty(originatedBy), Engine.B(cancelGroupEmergency));
        return t < 0 ? Result<long>.Fail(-1, Engine.LastError()) : Result<long>.Success(t);
    }

    private static string? NullIfEmpty(string? s) => string.IsNullOrEmpty(s) ? null : s;

    /// <summary>affiliation PUBLISH — 서비스마다 따로다. MCPTT = Event: mcptt(on=false 면 Expires:0), MCVideo = 관심 그룹 전부를 한 PUBLISH 로
    /// (TS 24.281 §8.2.1.2 — Event: presence, 그룹이 없으면 Expires:0). 반환 token — RequestCompleted 로 상관.</summary>
    public Result<long> Affiliate(string groupId, bool on, McService service = McService.Mcptt)
    {
        long t = cimsue_engine_affiliate_service(Engine.Handle, Id, groupId, Engine.B(on), (int)service);
        return t < 0 ? Result<long>.Fail(-1, Engine.LastError()) : Result<long>.Success(t);
    }

    // ── MCVideo 그룹 호 (TS 24.281 · TS 24.581) ──
    /// <summary>MCVideo 그룹 호 개시·합류 — Request-URI = AccountConfig.McvideoServerUri(재합류는 opts.SessionUri). 나가기 = Call.Hangup
    /// (MCPTT 호와 독립). 송출·수신은 Call.RequestTransmission·AcceptReception, 진행은 Engine.TransmissionChanged·ReceptionChanged.</summary>
    public Result<Call> JoinVideoGroupCall(string groupId, VideoGroupCallOptions? opts = null)
    {
        using var s = new NativeStrings();
        cimsue_video_group_call_options_t o = ToNative(opts, s);
        return Engine.CallResult(cimsue_engine_join_video_group_call(Engine.Handle, Id, groupId, &o));
    }

    /// <summary>그룹 로스터 구독(RFC 4575 conference) — 확인 신호는 RosterChanged.</summary>
    public Result SubscribeConference(string groupId, bool on) =>
        Engine.Status(cimsue_engine_subscribe_conference(Engine.Handle, Id, groupId, Engine.B(on)));

    /// <summary>문서 변경 구독(RFC 5875 xcap-diff). 본문은 MessageReceived 로.</summary>
    public Result SubscribeXcapDiff(string psiUri, bool on) =>
        Engine.Status(cimsue_engine_subscribe_xcap_diff(Engine.Handle, Id, psiUri, Engine.B(on)));

    /// <summary>임의 SIP 요청(MESSAGE/PUBLISH/SUBSCRIBE …). 반환 token — 최종 응답은 RequestCompleted.</summary>
    public Result<long> SendRequest(string method, string targetUri, string contentType, string body,
                                    IReadOnlyDictionary<string, string>? headers = null)
    {
        using var s = new NativeStrings();
        int n = headers?.Count ?? 0;
        cimsue_header_t* h = null;
        if (n > 0)
        {
            h = (cimsue_header_t*)s.Alloc(sizeof(cimsue_header_t) * n);
            int i = 0;
            foreach (var kv in headers!) { h[i].name = s.Add(kv.Key); h[i].value = s.Add(kv.Value); ++i; }
        }
        long t = cimsue_engine_send_request(Engine.Handle, Id, method, targetUri, contentType, body, h, n);
        return t < 0 ? Result<long>.Fail(-1, Engine.LastError()) : Result<long>.Success(t);
    }

    // ── 관제 (dispatch_center.md §5, volte_supplementary_services.md §5·§6) ──
    /// <summary>대상 AoR 의 dialog 이벤트 구독(RFC 4235). NOTIFY → DialogInfoReceived.</summary>
    public Result DialogWatch(string targetAor, bool on) =>
        Engine.Status(cimsue_engine_dialog_watch(Engine.Handle, Id, targetAor, Engine.B(on)));

    /// <summary>통화 청취 합류 — INVITE-with-Join(RFC 3911) + a=recvonly. dlg 는 DialogInfoReceived 로 학습한 대상 dialog.</summary>
    public Result<Call> Join(string targetUri, DialogInfo dlg)
    {
        ArgumentNullException.ThrowIfNull(dlg);
        using var s = new NativeStrings();
        cimsue_dialog_info_t d = Engine.ToNative(dlg, s);
        return Engine.CallResult(cimsue_engine_join(Engine.Handle, Id, targetUri, &d));
    }

    /// <summary>당겨받기 — 그룹 픽업 = featureCode 만, 지정 픽업 = featureCode + number. 결과는 호 상태(200/403/404/489).</summary>
    public Result<Call> Pickup(string featureCode, string? number = null) =>
        Engine.CallResult(cimsue_engine_pickup(Engine.Handle, Id, featureCode, string.IsNullOrEmpty(number) ? null : number));

    // ── MCData SDS (TS 24.282 §9.2.2 C-plane) ──
    /// <summary>그룹 SDS 발신(MESSAGE multipart). MsgId(UUID hex32) = SdsReceived 의 disposition 통지와 상관,
    /// Token = 최종 응답 RequestCompleted(MESSAGE, Token) 과 상관(통지 발신의 완료 이벤트와 구분). 본문이 계정의 MaxSdsCplaneBytes 를 넘으면
    /// media plane(MSRP)으로 가고 최종 결과는 RequestCompleted method "MSRP" 로 온다(Token 상관은 같다).
    /// msgId = 재전송이면 처음의 message ID(받는 쪽이 같은 메시지로 대조한다), null 이면 새로 만든다.</summary>
    public Result<SdsSend> SendGroupSds(string groupId, string text, bool requestDelivery = true, string? msgId = null)
    {
        byte* buf = stackalloc byte[64];
        long token = -1;
        int st = cimsue_engine_send_group_sds(Engine.Handle, Id, groupId, text, Engine.B(requestDelivery), NullIfEmpty(msgId), buf, 64, &token);
        if (st != 0) return Result<SdsSend>.Fail(st, Engine.LastError());
        return Result<SdsSend>.Success(new SdsSend(Utf8.Str(buf), token));
    }

    /// <summary>1:1 SDS 발신(request-type one-to-one-sds). peer = 상대 bare 번호.
    /// 그룹과 다른 것은 셋 — request-type·Request-URI·conversation ID(쌍 정렬). 1:1 을 그룹 경로로 보내면
    /// 서버가 그룹 게이트를 거치고 받는 쪽 스레드 귀속도 틀어진다(mcdata_messaging.md §4).</summary>
    public Result<SdsSend> SendSds(string peer, string text, bool requestDelivery = true, string? msgId = null)
    {
        byte* buf = stackalloc byte[64];
        long token = -1;
        int st = cimsue_engine_send_sds(Engine.Handle, Id, peer, text, Engine.B(requestDelivery), NullIfEmpty(msgId), buf, 64, &token);
        if (st != 0) return Result<SdsSend>.Fail(st, Engine.LastError());
        return Result<SdsSend>.Success(new SdsSend(Utf8.Str(buf), token));
    }

    // ── MCData FD (TS 24.282 §10.2 — 파일은 먼저 CscClient.UploadFd 로 올린다) ──
    /// <summary>그룹 FD 알림 발신(request-type group-fd) — file = 그룹 지정 업로드 결과. 반환·상관 규약은 SendGroupSds 와 같다.</summary>
    public Result<SdsSend> SendGroupFd(string groupId, FdFile file)
    {
        ArgumentNullException.ThrowIfNull(file);
        using var s = new NativeStrings();
        var f = ToNative(file, s);
        byte* buf = stackalloc byte[64];
        long token = -1;
        int st = cimsue_engine_send_group_fd(Engine.Handle, Id, groupId, &f, buf, 64, &token);
        if (st != 0) return Result<SdsSend>.Fail(st, Engine.LastError());
        return Result<SdsSend>.Success(new SdsSend(Utf8.Str(buf), token));
    }

    /// <summary>1:1 FD 알림 발신(request-type one-to-one-fd) — file = 그룹 없이 올린 결과, peer = 상대 bare 번호.</summary>
    public Result<SdsSend> SendFd(string peer, FdFile file)
    {
        ArgumentNullException.ThrowIfNull(file);
        using var s = new NativeStrings();
        var f = ToNative(file, s);
        byte* buf = stackalloc byte[64];
        long token = -1;
        int st = cimsue_engine_send_fd(Engine.Handle, Id, peer, &f, buf, 64, &token);
        if (st != 0) return Result<SdsSend>.Fail(st, Engine.LastError());
        return Result<SdsSend>.Success(new SdsSend(Utf8.Str(buf), token));
    }

    private static cimsue_fd_file_t ToNative(FdFile f, NativeStrings s) =>
        new() { url = s.Add(f.Url), name = s.Add(f.Name), type = s.Add(f.Type), size = f.Size };

    /// <summary>SDS disposition 통지(TS 24.282 §12.2.1.1) — peer = 받은 SDS 의 <see cref="SdsMessage.FromUri"/>(mcdata-calling-user-id),
    /// groupUri = 받은 SDS 의 <see cref="SdsMessage.GroupUri"/>(mcdata-calling-group-id, 1:1 이면 null). notifType 1~4.
    /// 계정 <see cref="AccountConfig.McdataServerUri"/> 가 있으면 규격형(PSI·resource-lists), 없으면 원 발신자 직행. Value = 요청 token.</summary>
    public Result<long> SendSdsNotification(string peer, string convId, string msgId, int notifType, string? groupUri = null)
    {
        long token = -1;
        int st = cimsue_engine_send_sds_notification(Engine.Handle, Id, peer, convId, msgId, groupUri, notifType, &token);
        return st != 0 ? Result<long>.Fail(st, Engine.LastError()) : Result<long>.Success(token);
    }

    // ── 변환 ──
    internal static cimsue_call_options_t ToNative(CallOptions? o)
    {
        cimsue_call_options_t n;
        cimsue_call_options_default(&n);
        if (o is null) return n;
        n.video = Engine.B(o.Video);
        n.emergency = Engine.B(o.Emergency);
        return n;
    }

    internal static cimsue_group_call_options_t ToNative(GroupCallOptions? o, NativeStrings s)
    {
        cimsue_group_call_options_t n;
        cimsue_group_call_options_default(&n);
        if (o is null) return n;
        n.emergency = Engine.B(o.Emergency);
        n.imminent_peril = Engine.B(o.ImminentPeril);
        n.listen_only = Engine.B(o.ListenOnly);
        n.full_duplex = Engine.B(o.FullDuplex);
        n.members = s.AddArray(o.Members, out n.member_count);
        n.broadcast = Engine.B(o.Broadcast);
        n.implicit_floor_request = Engine.B(o.ImplicitFloorRequest);
        return n;
    }

    internal static cimsue_video_group_call_options_t ToNative(VideoGroupCallOptions? o, NativeStrings s)
    {
        cimsue_video_group_call_options_t n;
        cimsue_video_group_call_options_default(&n);
        if (o is null) return n;
        n.prearranged = Engine.B(o.Prearranged);
        n.queueing = Engine.B(o.Queueing);
        n.max_priority = o.MaxPriority;
        n.max_reception_priority = o.MaxReceptionPriority;
        n.implicit_transmission_request = Engine.B(o.ImplicitTransmissionRequest);
        n.session_uri = s.Add(o.SessionUri);
        return n;
    }

    public override string ToString() => $"Account#{Id}";
}
