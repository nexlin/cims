// CimsUe — Call 래퍼: 호 id 에 걸린 명령(engine.h 의 callId 인자 함수들). 상태는 Info 스냅샷과 Engine 이벤트로 본다.
using static CimsUe.Native.NativeMethods;

namespace CimsUe;

public sealed class Call
{
    public Engine Engine { get; }
    /// <summary>코어 callId(엔진 발급).</summary>
    public int Id { get; }

    internal Call(Engine engine, int id) { Engine = engine; Id = id; }

    /// <summary>호 상태 스냅샷(없는 호는 CallId=-1).</summary>
    public CallInfo Info => Engine.CallInfoOf(Id);
    /// <summary>floor 상태 스냅샷(그룹콜/사설콜).</summary>
    public FloorInfo FloorInfo => Engine.FloorInfoOf(Id);
    /// <summary>오디오 스트림 RTP/RTCP 통계. 종료된 호는 소멸 시점의 최종 통계.</summary>
    public StreamStats StreamStats => Engine.StreamStatsOf(Id);

    // ── 호 제어 ──
    public unsafe Result Answer(CallOptions? opts = null)
    {
        var o = Account.ToNative(opts);
        return Engine.Status(cimsue_engine_answer(Engine.Handle, Id, &o));
    }
    public Result Reject(int statusCode = 486) => Engine.Status(cimsue_engine_reject(Engine.Handle, Id, statusCode));
    public Result Hangup() => Engine.Status(cimsue_engine_hangup(Engine.Handle, Id));
    public Result Hold() => Engine.Status(cimsue_engine_hold(Engine.Handle, Id));
    public Result Resume() => Engine.Status(cimsue_engine_resume(Engine.Handle, Id));
    /// <summary>마이크 → 호 송신 차단/복구. MCPTT 세션에서는 floor 가 마이크를 게이트하므로 무시된다.</summary>
    public Result SetMuted(bool muted) => Engine.Status(cimsue_engine_set_muted(Engine.Handle, Id, Engine.B(muted)));
    /// <summary>호 → 스피커 청취 on/off.</summary>
    public Result SetListen(bool listen) => Engine.Status(cimsue_engine_set_listen(Engine.Handle, Id, Engine.B(listen)));
    /// <summary>수신 음량(1.0=원음, 0=무음).</summary>
    public Result SetRxLevel(float level) => Engine.Status(cimsue_engine_set_rx_level(Engine.Handle, Id, level));
    public Result SendDtmf(string digits) => Engine.Status(cimsue_engine_send_dtmf(Engine.Handle, Id, digits));

    // ── MCPTT ──
    /// <summary>세션 이탈(BYE).</summary>
    public Result LeaveGroupCall() => Engine.Status(cimsue_engine_leave_group_call(Engine.Handle, Id));
    /// <summary>PTT down — Floor Request. 응답은 FloorChanged(Granted/Denied/QueuePosition). priority&lt;0 = 미기재.</summary>
    public Result FloorRequest(int priority = -1) => Engine.Status(cimsue_engine_floor_request(Engine.Handle, Id, priority));
    /// <summary>PTT up — Floor Release(대기 중이면 Queued Cancel 선행).</summary>
    public Result FloorRelease() => Engine.Status(cimsue_engine_floor_release(Engine.Handle, Id));
    public Result FloorQueueCancel() => Engine.Status(cimsue_engine_floor_queue_cancel(Engine.Handle, Id));

    // ── 관제 ──
    /// <summary>호 전달 blind — REFER(RFC 3515). 진행은 CallStateChanged(REFER 수락 후 서버가 BYE).</summary>
    public Result Transfer(string target) => Engine.Status(cimsue_engine_transfer(Engine.Handle, Id, target));
    /// <summary>호 전달 attended — Refer-To 에 Replaces(상담 호의 dialog).</summary>
    public Result TransferAttended(Call consult)
    {
        ArgumentNullException.ThrowIfNull(consult);
        return Engine.Status(cimsue_engine_transfer_attended(Engine.Handle, Id, consult.Id));
    }

    // ── 장치 ──
    /// <summary>수신 음성을 재생할 라우트(0=기본 재생 장치, 그 외 Engine.AddPlaybackRoute 의 id). 활성 호면 즉시 재결선.</summary>
    public Result SetRoute(int routeId) => Engine.Status(cimsue_engine_set_call_route(Engine.Handle, Id, routeId));

    public override string ToString() => $"Call#{Id}";
}
