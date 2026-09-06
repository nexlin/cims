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
    public Result Remove() => Engine.Status(cimsue_engine_remove_account(Engine.Handle, Id));

    // ── 호 (VoLTE 1:1) ──
    /// <summary>발신. target 은 번호(도메인 자동 결합) 또는 sip: URI.</summary>
    public Result<Call> Dial(string target, CallOptions? opts = null)
    {
        cimsue_call_options_t o = ToNative(opts);
        return Engine.CallResult(cimsue_engine_dial(Engine.Handle, Id, target, &o));
    }

    // ── MCPTT 그룹콜·사설콜 (TS 24.379) ──
    /// <summary>그룹콜 참여. groupId 는 bare id. 이미 같은 그룹 세션이 있으면 그 호. ListenOnly = 청취 전용(관제 PTT 청취).</summary>
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

    /// <summary>affiliation PUBLISH(Event: mcptt). on=false 면 Expires:0. 반환 token — RequestCompleted 로 상관.</summary>
    public Result<long> Affiliate(string groupId, bool on)
    {
        long t = cimsue_engine_affiliate(Engine.Handle, Id, groupId, Engine.B(on));
        return t < 0 ? Result<long>.Fail(-1, Engine.LastError()) : Result<long>.Success(t);
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
    /// Token = 최종 응답 RequestCompleted(MESSAGE, Token) 과 상관(통지 발신의 완료 이벤트와 구분).</summary>
    public Result<SdsSend> SendGroupSds(string groupId, string text, bool requestDelivery = true)
    {
        byte* buf = stackalloc byte[64];
        long token = -1;
        int st = cimsue_engine_send_group_sds(Engine.Handle, Id, groupId, text, Engine.B(requestDelivery), buf, 64, &token);
        if (st != 0) return Result<SdsSend>.Fail(st, Engine.LastError());
        return Result<SdsSend>.Success(new SdsSend(Utf8.Str(buf), token));
    }

    /// <summary>SDS disposition 통지(1:1 대상 peer bare 번호). notifType 1~4. Value = 요청 token.</summary>
    public Result<long> SendSdsNotification(string peer, string convId, string msgId, int notifType)
    {
        long token = -1;
        int st = cimsue_engine_send_sds_notification(Engine.Handle, Id, peer, convId, msgId, notifType, &token);
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
        return n;
    }

    public override string ToString() => $"Account#{Id}";
}
