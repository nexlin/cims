package com.cims.ue.ptt.floor

import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import kotlin.concurrent.thread

/** 단말 floor 상태머신 (TS 24.380 / 설계서 §5.3). */
enum class FloorState { IDLE, REQUESTING, SPEAKING, LISTENING, QUEUED }

/** 서버에서 수신한 floor 사건 → UI/오디오 제어 트리거.
 *  [indicator]=Floor Indicator 비트([FloorIndicator]) — CMP 가 발언자의 긴급/임박 tier 를 방송. */
sealed interface FloorEvent {
    data class Granted(val durationSec: Int?, val indicator: Int? = null) : FloorEvent
    data class Denied(val cause: Int?, val text: String?) : FloorEvent
    data object Idle : FloorEvent
    data class Taken(val speaker: String?, val indicator: Int? = null) : FloorEvent
    data class Revoked(val cause: Int?, val text: String?) : FloorEvent
    data class QueuePosition(val position: Int?) : FloorEvent
    data class Other(val type: Int) : FloorEvent
}

/**
 * MCPTT Floor 제어 클라이언트 — PJSIP 밖 **별도 UDP 소켓**에서 TS 24.380 RTCP-APP "MCPT" 송수신.
 *
 * 목적지(`remoteHost:remotePort`)는 **그룹 INVITE 200 OK SDP 의 `m=application` 포트**에서 학습한다
 * (RTP+1 고정 금지 — 설계서 §5.1). [ssrc] 는 내 floor participant SSRC, [userId] 는 MCPTT ID.
 *
 * 스레딩: 수신 전용 스레드가 디코드→상태/이벤트 갱신. [onEvent] 콜백은 수신 스레드에서 호출되므로
 * UI 는 main 으로 디스패치할 것. 현재 상태는 [state] StateFlow.
 */
class FloorClient(
    private val ssrc: Long,
    private val userId: String,
    localPort: Int = 0,
    private val onEvent: (FloorEvent) -> Unit = {},
) {
    private val socket = DatagramSocket(localPort)

    // 원격(CMP floor) 목적지는 그룹 INVITE 200 OK SDP 의 m=application 에서 학습 → connectRemote 로 설정.
    @Volatile private var remoteAddr: InetAddress? = null
    @Volatile private var remotePort: Int = 0

    private val _state = MutableStateFlow(FloorState.IDLE)
    val state: StateFlow<FloorState> = _state.asStateFlow()

    /** 바인드된 로컬 floor 포트(송신 SDP m=application 에 광고). */
    val localPort: Int get() = socket.localPort

    @Volatile private var running = true
    private val rx = thread(name = "floor-rx", start = true) { receiveLoop() }

    // 송신 전용 스레드 — pttDown/Up 은 UI(main) 스레드에서 호출되는데 main 에서의
    // socket.send 는 NetworkOnMainThreadException 으로 즉시 실패한다.
    private val tx = java.util.concurrent.Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "floor-tx") }
    private var ackTask: java.util.concurrent.ScheduledFuture<*>? = null

    /** SDP 에서 학습한 CMP floor 목적지 설정(송신 가능해짐) + Ack keepalive 시작. */
    fun connectRemote(host: String, port: Int) {
        remoteAddr = InetAddress.getByName(host)
        remotePort = port
        startAckKeepalive()
    }

    // Ack 주기 송신 — 청취 전용 멤버는 자발적 상향이 없어 이 Ack 가 floor 소켓의 유일한
    //   상향 트래픽이다. NAT 유입 매핑과 서버 latch 를 유지한다 (ue_nat_traversal.md §7.1,
    //   요건 ≤20s). 발언 중에도 유지 — 상향 RTP 는 별도(audio) 소켓이라 floor 매핑과 무관.
    @Synchronized private fun startAckKeepalive() {
        if (ackTask != null) return
        ackTask = tx.scheduleWithFixedDelay(
            { runCatching { sendAck() } },
            ACK_PERIOD_SEC, ACK_PERIOD_SEC, java.util.concurrent.TimeUnit.SECONDS,
        )
    }

    val hasRemote: Boolean get() = remoteAddr != null

    // ── 송신 (PTT down/up) ──

    /** PTT down → Floor Request. GRANT 수신 후에만 실제 발화 확정(콜백). */
    fun requestFloor(priority: Int = 0, indicator: Int? = null) {
        send(FloorCodec.request(ssrc, userId, priority, indicator))
        _state.value = FloorState.REQUESTING
    }

    /** PTT up → Floor Release. */
    fun releaseFloor() {
        send(FloorCodec.release(ssrc, userId))
        _state.value = FloorState.IDLE
    }

    fun requestQueuePosition() = send(FloorCodec.queuePositionRequest(ssrc, userId))

    /** Floor Ack(User ID 포함) — 참여 직후 1회 + 주기([ACK_PERIOD_SEC]) 송신으로
     *  NAT 유입 매핑을 열고 서버 latch 를 유지한다. 서버 상태를 바꾸지 않아 부작용 없음. */
    fun sendAck() = send(FloorCodec.ack(ssrc, userId))

    private fun send(pkt: ByteArray) {
        val addr = remoteAddr ?: run { Log.w(TAG, "floor send before remote learned"); return }
        val port = remotePort
        tx.execute {
            runCatching { socket.send(DatagramPacket(pkt, pkt.size, addr, port)) }
                .onFailure { Log.w(TAG, "floor send failed: ${it.javaClass.simpleName}: ${it.message}") }
        }
    }

    // ── 수신 ──

    private fun receiveLoop() {
        val buf = ByteArray(1500)
        while (running) {
            try {
                val dp = DatagramPacket(buf, buf.size)
                socket.receive(dp)
                val msg = FloorCodec.decode(buf, dp.length) ?: continue
                handle(msg)
            } catch (e: Exception) {
                if (running) Log.w(TAG, "floor rx: ${e.message}")
            }
        }
    }

    private fun handle(msg: FloorMessage) {
        val ev: FloorEvent = when (msg.type) {
            FloorMsgType.GRANTED -> { _state.value = FloorState.SPEAKING; FloorEvent.Granted(msg.durationSec, msg.floorIndicator) }
            FloorMsgType.DENY -> { _state.value = FloorState.IDLE; FloorEvent.Denied(msg.rejectCause, FloorCause.REJECT[msg.rejectCause]) }
            FloorMsgType.IDLE -> { if (_state.value != FloorState.SPEAKING) _state.value = FloorState.IDLE; FloorEvent.Idle }
            FloorMsgType.TAKEN -> {
                val sp = msg.grantedParty ?: msg.userId
                // 서버가 전체 멤버에게 방송하는 TAKEN 에 내 GRANT 도 포함 — 화자=나면
                // 발언 상태를 강등하지 않는다(마이크 개방 유지).
                if (sameUser(sp, userId)) return
                _state.value = FloorState.LISTENING
                FloorEvent.Taken(sp, msg.floorIndicator)
            }
            FloorMsgType.REVOKE -> { _state.value = FloorState.IDLE; FloorEvent.Revoked(msg.rejectCause, FloorCause.REVOKE[msg.rejectCause]) }
            FloorMsgType.QUEUE_POS_INFO -> { _state.value = FloorState.QUEUED; FloorEvent.QueuePosition(msg.queuePosition) }
            else -> FloorEvent.Other(msg.type)
        }
        Log.d(TAG, "floor recv ${msg.typeName()} → state=${_state.value}")
        runCatching { onEvent(ev) }
    }

    /** MCPTT ID 동일성 — "tel:+82..@dom"/"sip:.."/"+82.." 표기 차이를 무시하고 비교. */
    private fun sameUser(a: String?, b: String?): Boolean {
        if (a == null || b == null) return false
        fun bare(s: String) = s.substringAfter(':').substringBefore('@')
        return bare(a) == bare(b)
    }

    fun close() {
        running = false
        runCatching { tx.shutdownNow() }
        runCatching { socket.close() }
        runCatching { rx.join(500) }
    }

    private companion object {
        const val TAG = "FloorClient"
        /** floor Ack keepalive 주기(초) — NAT UDP 매핑 유지 요건 ≤20s 에 여유를 둔 값. */
        const val ACK_PERIOD_SEC = 15L
    }
}
