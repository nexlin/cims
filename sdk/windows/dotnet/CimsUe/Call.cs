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
    /// <summary>호 품질 — 손실·폐기·지터·RTD·E-model MOS(ue_voice_quality.md §3). 종료된 호는 마지막 값.</summary>
    public CallQuality Quality => Engine.CallQualityOf(Id);

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
    /// <summary>마이크 → 호 송신 차단/복구. 반이중 MCPTT 세션에서는 floor 가 마이크를 게이트하므로 무시되고, 전이중 개별 통화(mc_no_floor_ctrl)에는 적용된다.</summary>
    public Result SetMuted(bool muted) => Engine.Status(cimsue_engine_set_muted(Engine.Handle, Id, Engine.B(muted)));
    /// <summary>호 → 스피커 청취 on/off.</summary>
    public Result SetListen(bool listen) => Engine.Status(cimsue_engine_set_listen(Engine.Handle, Id, Engine.B(listen)));
    /// <summary>수신 음량(1.0=원음, 0=무음).</summary>
    public Result SetRxLevel(float level) => Engine.Status(cimsue_engine_set_rx_level(Engine.Handle, Id, level));
    public Result SendDtmf(string digits) => Engine.Status(cimsue_engine_send_dtmf(Engine.Handle, Id, digits));
    /// <summary>통화 중 영상 전환(1:1 호, re-INVITE) — on = 추가 요청(결과는 Engine.VideoRequestChanged), false = 제거(묻지 않는다).</summary>
    public Result SetVideo(bool on) => Engine.Status(cimsue_engine_set_call_video(Engine.Handle, Id, Engine.B(on)));
    /// <summary>상대의 영상 추가 요청에 답한다 — accept = 영상을 받는 200 OK, false = m=video port 0(음성은 그대로).</summary>
    public Result AnswerVideoRequest(bool accept) => Engine.Status(cimsue_engine_answer_video_request(Engine.Handle, Id, Engine.B(accept)));

    // ── MCPTT ──
    /// <summary>세션 이탈(BYE).</summary>
    public Result LeaveGroupCall() => Engine.Status(cimsue_engine_leave_group_call(Engine.Handle, Id));
    /// <summary>PTT down — Floor Request. 응답은 FloorChanged(Granted/Denied/QueuePosition). priority&lt;0 = 미기재.</summary>
    public Result FloorRequest(int priority = -1) => Engine.Status(cimsue_engine_floor_request(Engine.Handle, Id, priority));
    /// <summary>PTT up — Floor Release(대기 중이면 Queued Cancel 선행).</summary>
    public Result FloorRelease() => Engine.Status(cimsue_engine_floor_release(Engine.Handle, Id));
    public Result FloorQueueCancel() => Engine.Status(cimsue_engine_floor_queue_cancel(Engine.Handle, Id));
    /// <summary>진행 중 그룹콜의 조건 상향·하향(TS 24.379 §10.1.1.2.1.3~5) — in-dialog re-INVITE. 결과는 McpttConditionChanged(Local → Confirmed/Denied),
    /// 거절은 호를 끊지 않는다. emergency·imminentPeril 을 함께 true 로 줄 수 없다. 사설콜·응답 대기 중·성립 전 호는 실패.</summary>
    public Result SetCondition(bool emergency, bool imminentPeril) =>
        Engine.Status(cimsue_engine_set_call_condition(Engine.Handle, Id, Engine.B(emergency), Engine.B(imminentPeril)));

    // ── MCVideo 전송 제어 (TS 24.581) ──
    /// <summary>전송 제어 상태 스냅샷(MCVideo 그룹 호).</summary>
    public TransmissionInfo TransmissionInfo => Engine.TransmissionInfoOf(Id);
    /// <summary>[영상 보내기] — Transmission Request(§6.2.4.3.2). 결과는 TransmissionChanged(Granted·Rejected·QueuePosition). priority&lt;0 = 미기재.</summary>
    public Result RequestTransmission(int priority = -1) => Engine.Status(cimsue_engine_request_transmission(Engine.Handle, Id, priority));
    /// <summary>[보내기 끝] — Transmission End Request(§6.2.4.5.3). 대기·요청 중이면 요청을 거둔다. 완료 = TransmissionChanged(Ended).</summary>
    public Result ReleaseTransmission() => Engine.Status(cimsue_engine_release_transmission(Engine.Handle, Id));
    /// <summary>[받기] — Receive Media Request(§6.2.5.3.3). transmitterId = ReceptionChanged(Notified) 의 Transmitter.UserId.</summary>
    public Result AcceptReception(string transmitterId, int priority = -1) =>
        Engine.Status(cimsue_engine_accept_reception(Engine.Handle, Id, transmitterId, priority));
    /// <summary>[그만 보기] — Media Reception End Request(§6.2.5.5). 완료 = ReceptionChanged(Released).</summary>
    public Result EndReception(string transmitterId) => Engine.Status(cimsue_engine_end_reception(Engine.Handle, Id, transmitterId));
    /// <summary>내 영상 송출 허용(CallInfo.VideoSend, 기본 true) — MCPTT 반이중은 허용이면서 발언권을 가진 동안만, MCVideo 는 송출 허가에서만
    /// 실제로 보낸다(재협상 없음). 영상 없는 빌드·호면 실패.</summary>
    public Result SetVideoSend(bool on) => Engine.Status(cimsue_engine_set_video_send(Engine.Handle, Id, Engine.B(on)));

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
