package com.cims.ue.ptt.floor

import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.InetAddress
import kotlin.concurrent.thread

/** 단말 floor 상태머신 (TS 24.380 / 설계서 §5.3). */
enum class FloorState { IDLE, REQUESTING, SPEAKING, LISTENING, QUEUED }

/** 서버에서 수신한 floor 사건 → UI/오디오 제어 트리거.
 *  [indicator]=Floor Indicator 비트([FloorIndicator]) — CMP 가 발언자의 긴급/임박 tier 를 방송. */
sealed interface FloorEvent {
    /** [durationSec]=이번 발언 허용 시간(T2, §8.2.3.3) — 초과 전 단말이 스스로 종료해야 회수를 면한다. */
    data class Granted(val durationSec: Int?, val indicator: Int? = null) : FloorEvent
    data class Denied(val cause: Int?, val text: String?) : FloorEvent
    data object Idle : FloorEvent
    /** [permission]=Permission to Request the Floor(§8.2.3.7, 0=요청 불가) ·
     *  [speakerSsrc]=화자 RTP SSRC(§8.2.3.16 — 헤더 SSRC 는 서버 것이라 화자 식별에 못 쓴다) ·
     *  [talkers]=현재 발언자 전체(동시 발언이면 2명 이상) · [meSpeaking]=그 안에 내가 있다. */
    data class Taken(
        val speaker: String?,
        val indicator: Int? = null,
        val permission: Int? = null,
        val speakerSsrc: Long? = null,
        val talkers: List<FloorTalker> = emptyList(),
        val meSpeaking: Boolean = false,
    ) : FloorEvent
    /** Floor Release Multi Talker(0x0F, §8.2.14) — 동시 발언 중 [id] 한 명만 발언을 끝냈다.
     *  잔여 화자가 있으므로 Idle 이 아니다 — [talkers] 로 목록·재생만 갱신한다. */
    data class TalkerLeft(
        val id: String?,
        val ssrc: Long?,
        val talkers: List<FloorTalker>,
        val meSpeaking: Boolean,
    ) : FloorEvent
    data class Revoked(val cause: Int?, val text: String?) : FloorEvent
    data class QueuePosition(val position: Int?) : FloorEvent
    /** 대기 요청이 사라졌다 — 내 취소의 결과(Cancel Result) 또는 서버/의장 취소 통지(§8.2.15). */
    data class QueueCancelled(val result: Int?, val byMe: Boolean) : FloorEvent
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
    // IPv4 전용(AF_INET) 소켓 + 채널 API 직접 사용. 서버(CMP)는 IPv4 전용이라 v4 바인딩으로
    // 잃는 것이 없고, 채널의 socket() 어댑터 경유 send 는 예외 없이 패킷이 유실되는 동작이
    // 실기기(W999/MTK13·MF52/QC15 공통)에서 관측돼 send/receive 모두 채널 메서드를 직접 쓴다.
    private val channel = java.nio.channels.DatagramChannel
        .open(java.net.StandardProtocolFamily.INET)
        // INET 채널에 무인자 와일드카드(InetSocketAddress(port))를 주면 v6 "::" 로 해석돼
        // UnsupportedAddressTypeException — v4 와일드카드(0.0.0.0)를 명시해야 한다.
        .bind(java.net.InetSocketAddress(
            java.net.InetAddress.getByAddress(byteArrayOf(0, 0, 0, 0)), localPort))
        .also { Log.i(TAG, "floor socket bound v4 :${(it.localAddress as java.net.InetSocketAddress).port}") }

    // 원격(CMP floor) 목적지는 그룹 INVITE 200 OK SDP 의 m=application 에서 학습 → connectRemote 로 설정.
    @Volatile private var remoteAddr: InetAddress? = null
    @Volatile private var remotePort: Int = 0

    private val _state = MutableStateFlow(FloorState.IDLE)
    val state: StateFlow<FloorState> = _state.asStateFlow()

    // 현재 발언자 집합 — 서버가 **증분**으로 알린다: Taken=전체 집합, 0x0F=한 명 이탈,
    //   Idle=비었음. 한 명이 빠질 때 서버는 Taken 을 다시 보내지 않으므로(§6.3.4.4.6-5)
    //   단말이 집합을 들고 있어야 화자 목록·SSRC 별 재생이 유지된다.
    private val _talkers = MutableStateFlow<List<FloorTalker>>(emptyList())
    /** 동시 발언 중인 화자 전체(단일 발언이면 1명, 유휴면 빈 목록). */
    val talkers: StateFlow<List<FloorTalker>> = _talkers.asStateFlow()

    /** 바인드된 로컬 floor 포트(송신 SDP m=application 에 광고). */
    val localPort: Int get() = (channel.localAddress as java.net.InetSocketAddress).port

    @Volatile private var running = true
    private val rx = thread(name = "floor-rx", start = true) { receiveLoop() }

    // 송신 전용 스레드 — pttDown/Up 은 UI(main) 스레드에서 호출되는데 main 에서의
    // socket.send 는 NetworkOnMainThreadException 으로 즉시 실패한다.
    private val tx = java.util.concurrent.Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "floor-tx") }
    private var ackTask: java.util.concurrent.ScheduledFuture<*>? = null

    // Floor Revoke 응답(Floor Release)의 T100 재전송 태스크와 회수 진행 플래그.
    //   서버가 T8 로 Revoke 를 재전송하는 동안 이벤트를 여러 번 올리지 않기 위한 가드다.
    private var releaseRetx: java.util.concurrent.ScheduledFuture<*>? = null
    @Volatile private var revokePending = false

    /** 마지막으로 수용한 Taken/Idle 의 Message Sequence Number(§8.2.3.10, 65535 순환). */
    @Volatile private var lastMsgSeq: Int? = null

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

    /** PTT down → Floor Request. GRANT 수신 후에만 실제 발화 확정(콜백).
     *  [priority] 는 명시 요청이 있을 때만 싣는다 — 근거는 [FloorCodec.request]. */
    fun requestFloor(priority: Int? = null, indicator: Int? = null) {
        cancelReleaseRetx()
        send(FloorCodec.request(ssrc, userId, priority, indicator))
        _state.value = FloorState.REQUESTING
    }

    /** PTT up → Floor Release. */
    fun releaseFloor() {
        cancelReleaseRetx()
        send(FloorCodec.release(ssrc, userId))
        // 내 발언만 끝난다 — 동시 발언 중이면 남은 화자는 그대로 듣는다(서버 Idle 을 기다리지 않는다).
        val rest = _talkers.value.filterNot { it.self }
        _talkers.value = rest
        _state.value = if (rest.isEmpty()) FloorState.IDLE else FloorState.LISTENING
    }

    fun requestQueuePosition() = send(FloorCodec.queuePositionRequest(ssrc, userId))

    /** 대기열에서 내 요청을 뺀다(§8.2.15) — PTT 버튼을 뗄 때. Floor Release 만으로는 서버가
     *  대기 요청을 지우지 않아(발언 중이 아니면 무시) 유령 대기자가 남는다. */
    fun cancelQueuedRequest() {
        send(FloorCodec.cancelQueuedRequest(ssrc))
        _state.value = FloorState.IDLE
    }

    /** Floor Ack(User ID 포함) — 참여 직후 1회 + 주기([ACK_PERIOD_SEC]) 송신으로
     *  NAT 유입 매핑을 열고 서버 latch 를 유지한다. 서버 상태를 바꾸지 않아 부작용 없음. */
    fun sendAck() = send(FloorCodec.ack(ssrc, userId))

    private fun send(pkt: ByteArray) {
        if (remoteAddr == null) { Log.w(TAG, "floor send before remote learned"); return }
        tx.execute { sendNow(pkt) }
    }

    /** tx 스레드에서의 실제 송신 — 이미 tx 위에 있는 호출(재전송 태스크)은 이 쪽을 쓴다. */
    private fun sendNow(pkt: ByteArray) {
        val addr = remoteAddr ?: return
        val port = remotePort
        runCatching {
            val n = channel.send(java.nio.ByteBuffer.wrap(pkt), java.net.InetSocketAddress(addr, port))
            Log.d(TAG, "floor tx ${n}B → ${addr.hostAddress}:$port")
        }.onFailure { Log.w(TAG, "floor send failed: ${it.javaClass.simpleName}: ${it.message}") }
    }

    // ── 수신 ──

    private fun receiveLoop() {
        val buf = java.nio.ByteBuffer.allocate(1500)
        while (running) {
            try {
                buf.clear()
                channel.receive(buf) ?: continue
                val msg = FloorCodec.decode(buf.array(), buf.position()) ?: continue
                handle(msg)
            } catch (e: Exception) {
                if (running) Log.w(TAG, "floor rx: ${e.message}")
            }
        }
    }

    private fun handle(msg: FloorMessage) {
        // ── Ack 요구 변종(§8.2.2) — 상태 처리보다 먼저 확인부터 회신한다. 확인이 없으면
        //   상대는 T100 이 만료할 때까지 같은 메시지를 재전송한다(§6.2.4.5.3).
        if (msg.ackRequired) {
            send(FloorCodec.ackOf(ssrc, msg.type or FloorMsgType.ACK_REQUIRED_BIT))
        }
        // ── Message Sequence Number(§8.2.3.10) — Taken/Idle 의 순서 식별. 재전송·경로 역전으로
        //   오래된 것이 뒤늦게 오면 화자 표시가 되돌아가므로 폐기한다(ack 회신은 이미 했다).
        if (msg.type == FloorMsgType.TAKEN || msg.type == FloorMsgType.IDLE) {
            val seq = msg.msgSeq
            if (seq != null) {
                if (isStaleSeq(seq)) {
                    Log.i(TAG, "floor recv ${msg.typeName()} seq=$seq stale (last=$lastMsgSeq) — dropped")
                    return
                }
                lastMsgSeq = seq
            }
        }

        val ev: FloorEvent = when (msg.type) {
            FloorMsgType.GRANTED -> {
                revokePending = false
                cancelReleaseRetx()
                // 내 GRANT 는 나에게만 온다(다른 멤버는 Taken 을 받는다) — 집합에 나를 넣는다.
                if (_talkers.value.none { it.self })
                    _talkers.value = _talkers.value + FloorTalker(userId, ssrc, self = true)
                _state.value = FloorState.SPEAKING
                FloorEvent.Granted(msg.durationSec, msg.floorIndicator)
            }
            FloorMsgType.DENY -> { _state.value = FloorState.IDLE; FloorEvent.Denied(msg.rejectCause, FloorCause.REJECT[msg.rejectCause]) }
            FloorMsgType.IDLE -> {
                revokePending = false
                cancelReleaseRetx()
                _talkers.value = emptyList()
                if (_state.value != FloorState.SPEAKING) _state.value = FloorState.IDLE
                FloorEvent.Idle
            }
            // Floor Taken 은 **화자 집합 전체**를 싣는다(동시 발언이면 리스트 필드). 서버는
            //   화자에게 자기 Taken 을 보내지 않지만, 동시 발언에서 **뒤에 승급한 화자의
            //   Taken 은 먼저 말하던 화자에게도** 가고 그 목록엔 내가 들어 있다 — 그때 나를
            //   LISTENING 으로 강등하면 내 마이크가 닫힌다.
            FloorMsgType.TAKEN -> {
                val list = markSelf(msg.talkers)
                _talkers.value = list
                val meSpeaking = list.any { it.self }
                if (!meSpeaking) {
                    revokePending = false
                    cancelReleaseRetx()
                    _state.value = FloorState.LISTENING
                } else {
                    _state.value = FloorState.SPEAKING
                }
                val other = list.firstOrNull { !it.self }
                FloorEvent.Taken(other?.id ?: msg.grantedParty, msg.floorIndicator, msg.permission,
                    other?.ssrc ?: msg.speakerSsrc, list, meSpeaking)
            }
            // Floor Release Multi Talker(§8.2.14) — 한 명만 빠졌다. 서버는 이때 Taken 을 다시
            //   보내지 않으므로 단말이 집합에서 그 화자만 걷어낸다.
            FloorMsgType.RELEASE_MULTI -> {
                val goneId = msg.userId
                val goneSsrc = msg.speakerSsrc
                val list = _talkers.value.filterNot {
                    (goneId != null && sameUser(it.id, goneId)) ||
                        (goneId == null && goneSsrc != null && it.ssrc == goneSsrc)
                }
                _talkers.value = list
                val meSpeaking = list.any { it.self }
                _state.value = when {
                    meSpeaking -> FloorState.SPEAKING
                    list.isNotEmpty() -> FloorState.LISTENING
                    else -> FloorState.IDLE
                }
                FloorEvent.TalkerLeft(goneId, goneSsrc, list, meSpeaking)
            }
            // Floor Revoke(§6.2.4.5.4) — mic 차단(상위 콜백)과 함께 **Floor Release 로 응답**해야
            //   서버가 유예(T3)를 다 쓰지 않고 다음 화자를 즉시 승급시킨다.
            FloorMsgType.REVOKE -> {
                sendRevokeRelease(msg.floorIndicator)
                if (revokePending) return          // 서버 T8 재전송 — Release 만 다시 보내고 이벤트는 1회
                revokePending = true
                val rest = _talkers.value.filterNot { it.self }
                _talkers.value = rest
                _state.value = if (rest.isEmpty()) FloorState.IDLE else FloorState.LISTENING
                FloorEvent.Revoked(msg.rejectCause, FloorCause.REVOKE[msg.rejectCause])
            }
            FloorMsgType.QUEUE_POS_INFO -> { _state.value = FloorState.QUEUED; FloorEvent.QueuePosition(msg.queuePosition) }
            // Queued Floor Requests(§8.2.15) — 서버가 보내는 것은 결과/통지뿐이고, 둘 다
            //   "네 대기 요청은 이제 없다"는 뜻이다. Cancel Request 는 단말→서버 방향이라 무시.
            FloorMsgType.QUEUED_CANCEL -> {
                val purpose = msg.queuedPurpose
                if (purpose == FloorQueuedPurpose.CANCEL_REQUEST) return
                if (_state.value == FloorState.QUEUED) _state.value = FloorState.IDLE
                FloorEvent.QueueCancelled(msg.queuedResult, purpose == FloorQueuedPurpose.CANCEL_RESULT)
            }
            else -> FloorEvent.Other(msg.type)
        }
        // INFO 레벨 — 일부 벤더 단말(MTK)이 D 레벨을 기본 억제(log.tag=I)해 현장 진단이 막힌다.
        Log.i(TAG, "floor recv ${msg.typeName()}${if (msg.ackRequired) "(ack-req)" else ""} → state=${_state.value}")
        runCatching { onEvent(ev) }
    }

    /**
     * MSN 역전·중복 판정 — 65535 순환이라 차이를 모듈로로 본다(같은 값 = 재전송).
     * **직전 [SEQ_REORDER_WINDOW]개 안쪽으로만 되돌아간 것**을 폐기한다 — 그보다 멀리 뒤로 간 값은
     * 경로 역전이 아니라 서버측 카운터 초기화(그룹 재생성)이므로, 폐기하면 floor 표시가 영영
     * 얼어붙는다. 그 경우엔 새 기준으로 재동기한다.
     */
    private fun isStaleSeq(seq: Int): Boolean {
        val last = lastMsgSeq ?: return false
        return ((last - seq) and 0xffff) in 0 until SEQ_REORDER_WINDOW
    }

    /**
     * Floor Revoke 응답(§6.2.4.5.4) — Floor Release 를 보내고 T100 으로 재전송해 도달을 보장한다.
     * dual floor 의 G-bit 는 회수 통지에 실려 온 것을 그대로 되싣는다(같은 floor 를 가리키게).
     */
    private fun sendRevokeRelease(indicator: Int?) {
        val g = (indicator ?: 0) and FloorIndicator.DUAL_FLOOR
        val pkt = FloorCodec.release(ssrc, userId, if (g != 0) g else null)
        send(pkt)
        synchronized(this) {
            releaseRetx?.cancel(false)
            var left = RELEASE_RETX_MAX
            releaseRetx = tx.scheduleWithFixedDelay({
                if (left-- <= 0) { cancelReleaseRetx(); return@scheduleWithFixedDelay }
                runCatching { sendNow(pkt) }
            }, RELEASE_RETX_MS, RELEASE_RETX_MS, java.util.concurrent.TimeUnit.MILLISECONDS)
        }
    }

    @Synchronized private fun cancelReleaseRetx() {
        releaseRetx?.cancel(false)
        releaseRetx = null
    }

    /** 화자 목록에 "나" 표시 — 서버 표기(MCPTT ID URI 또는 가입자 번호)와 무관하게 [sameUser] 로 판정. */
    private fun markSelf(list: List<FloorTalker>): List<FloorTalker> =
        list.map { it.copy(self = sameUser(it.id, userId)) }

    /** MCPTT ID 동일성 — "tel:+82..@dom"/"sip:.."/"+82.." 표기 차이를 무시하고 비교. */
    private fun sameUser(a: String?, b: String?): Boolean {
        if (a == null || b == null) return false
        fun bare(s: String) = s.substringAfter(':').substringBefore('@')
        return bare(a) == bare(b)
    }

    fun close() {
        running = false
        cancelReleaseRetx()
        runCatching { tx.shutdownNow() }
        runCatching { channel.close() }
        runCatching { rx.join(500) }
    }

    private companion object {
        const val TAG = "FloorClient"
        /** floor Ack keepalive 주기(초) — NAT UDP 매핑 유지 요건 ≤20s 에 여유를 둔 값. */
        const val ACK_PERIOD_SEC = 15L

        // Floor Revoke 응답 Release 의 재전송(T100). 서버는 Revoke 후 유예 T3(기본 3초) 동안
        //   Release 를 기다리며 T8(1초)로 Revoke 를 재전송하므로, 그 창 안에서 끝나야 의미가
        //   있다 — 800ms 간격 2회면 유실 1~2회를 3초 안에 흡수한다.
        const val RELEASE_RETX_MS = 800L
        const val RELEASE_RETX_MAX = 2

        /** MSN 역전 폐기 창 — 이 개수만큼 뒤로 간 것만 "오래된 것"으로 본다([isStaleSeq]). */
        const val SEQ_REORDER_WINDOW = 64
    }
}
