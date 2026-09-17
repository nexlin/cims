// libcimsue Kotlin 파사드 — 엔진 (docs/design/features/android_dispatch_tablet.md §4, ue_sdk.md §4.2·§5.1)
//
// 코어 API 는 **명령 / 상태 스냅샷 / 이벤트** 세 갈래이고 파사드는 이 셋을 Kotlin 관용구로 옮기기만 한다.
// 프로토콜 판단을 여기에 두지 않는다 — 판단은 전부 코어에 있다.
//
//   명령  = `suspend fun` — 코어의 `runSync` 는 제어 스레드에 일을 넘기고 `fut.get()` 으로 **호출자를
//           기다린다**(engine.cpp:61~67). 그 안에서 DNS 해석 같은 동기 I/O 가 일어날 수 있으므로
//           (dial → makeCall → sip_resolve 의 getaddrinfo) 메인 스레드에서 부르면 ANR 이 된다.
//   상태  = 동기 조회 fun — 잠금 스냅샷 읽기만 하는 것(`callInfo`·`calls`·`regInfo`)에 한정한다.
//           제어 스레드를 기다리는 조회(`floorInfo`·`streamStats`·`audioDevices`)는 명령과 같이 suspend 다.
//   이벤트 = Flow — 유실 정책이 종류마다 다르다(§ 아래 "이벤트 전달").
//
// 앱은 이 파일의 공개면만 쓴다. `com.cims.ue.sdk.jni.*` 와 `org.pjsip.*` 는 내부다(AGENTS.md §7).
package com.cims.ue.sdk

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.withContext
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import com.cims.ue.sdk.jni.CallInfo as JniCallInfo
import com.cims.ue.sdk.jni.DialogInfo as JniDialogInfo
import com.cims.ue.sdk.jni.Engine as JniEngine
import com.cims.ue.sdk.jni.FloorEvent as JniFloorEvent
import com.cims.ue.sdk.jni.Listener as JniListener
import com.cims.ue.sdk.jni.RegInfo as JniRegInfo
import com.cims.ue.sdk.jni.RequestResult as JniRequestResult
import com.cims.ue.sdk.jni.RosterVector
import com.cims.ue.sdk.jni.SdsMessage as JniSdsMessage

/** 네이티브 적재 — 파사드를 처음 만들 때 한 번. 앱이 직접 부를 필요는 없다. */
internal object NativeLib {
    private val loaded = AtomicBoolean(false)
    fun ensure() {
        if (loaded.compareAndSet(false, true)) System.loadLibrary("cimsue")
    }
}

/** 엔진이 이미 닫힌 뒤의 호출. 크래시(0 포인터 역참조) 대신 이 결과를 돌려준다. */
private fun <T> closedResult(): CimsResult<T> = CimsResult.fail(-99, "engine closed")

/**
 * 단말 SDK 엔진. 프로세스당 하나만 만든다.
 *
 * 수명은 **Foreground Service** 가 갖는다(§6.1) — Activity 가 재생성돼도 등록·세션이 끊기지 않게.
 * 프로세스가 회수되면 이 객체와 코어 상태가 함께 사라지므로 이전 호는 복원 대상이 아니다.
 *
 * **수명 규칙**: [close] 는 신규 호출을 먼저 막고, 진행 중인 JNI 호출이 끝나기를 기다린 뒤 네이티브를
 * 해제한다. 해제 뒤의 호출은 `CimsResult.fail("engine closed")` 로 떨어진다(생성 코드가 0 포인터를
 * 검사 없이 역참조하므로 막지 않으면 프로세스 크래시다).
 */
class CimsUe(private val io: CoroutineDispatcher = Dispatchers.IO) : AutoCloseable {

    private val engine: JniEngine
    private val listener: JniListener

    init {
        NativeLib.ensure()
        engine = JniEngine()
        // director 프록시는 **Kotlin 강참조**(이 필드)로 살린다. swigTakeOwnership 은 반대로 네이티브 쪽
        // 참조를 Weak 으로 바꾸고, swigReleaseOwnership 은 GlobalRef 로 영구 고정해 객체 그래프를
        // 누수시킨다(cimsue_wrap.cpp 의 java_change_ownership). 둘 다 부르지 않는다.
        listener = FlowListener()
    }

    // ── 수명 게이트 ───────────────────────────────────────────────────────────
    private val closed = AtomicBoolean(false)
    private val inFlight = AtomicInteger(0)
    private val gate = Object()

    val isClosed: Boolean get() = closed.get()

    /** 네이티브 호출을 게이트 안에서 돌린다 — 닫혔으면 실행하지 않고, 도는 동안 해제되지 않는다. */
    private inline fun <T> guarded(block: () -> T): T? {
        if (closed.get()) return null
        inFlight.incrementAndGet()
        try {
            if (closed.get()) return null      // 진입 직후 닫힌 경우
            return block()
        } finally {
            if (inFlight.decrementAndGet() == 0) synchronized(gate) { (gate as Object).notifyAll() }
        }
    }

    internal suspend fun <T> command(block: () -> CimsResult<T>): CimsResult<T> =
        withContext(io) { guarded(block) ?: closedResult() }

    // ── 이벤트 전달 ───────────────────────────────────────────────────────────
    // 코어 이벤트 스레드를 막으면 모든 이벤트가 밀리므로 방출은 절대 블록하지 않는다. 그렇다고 전부
    // 버려도 되는 것은 아니라 **유실 정책을 종류로 가른다**.
    //
    //   ① 버려도 되는 것(로그) — DROP_OLDEST. 수집자가 없으면 그냥 사라진다.
    //   ② 최신만 맞으면 되는 것(등록·호·미디어·floor·로스터·dialog) — DROP_OLDEST 로 방출하되
    //      **권위는 스냅샷**이다(`registrations`·`callIds` + `callInfo()` 재조회). 화면 복원 경로가
    //      스냅샷 재조회라는 §6.7 계약이 여기서 성립한다.
    //   ③ 버리면 안 되는 것(SDS 수신·요청 최종 응답) — 무제한 Channel. 수집자가 아직 없어도 쌓이고
    //      한 번만 소비된다. 메시지 유실·발신 상태 고착을 막는 유일한 방법이다.
    //
    // ②에서 실제로 버려진 횟수는 [droppedEvents] 로 센다 — 0 이 아니면 화면이 스냅샷 재조회로
    // 스스로를 고쳐야 한다는 신호다.
    private val _dropped = AtomicLong(0)
    /** ② 정책에서 버려진 이벤트 수(누적). 0 이 아니면 스냅샷 재조회가 필요하다. */
    val droppedEvents: Long get() = _dropped.get()

    private fun <T> lossy() =
        MutableSharedFlow<T>(extraBufferCapacity = 256, onBufferOverflow = BufferOverflow.DROP_OLDEST)

    private fun <T> MutableSharedFlow<T>.emitLossy(v: T) { if (!tryEmit(v)) _dropped.incrementAndGet() }

    private val _log = MutableSharedFlow<LogLine>(extraBufferCapacity = 128, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    private val _regState = lossy<RegInfo>()
    private val _incomingCall = lossy<CallInfo>()
    private val _callState = lossy<CallInfo>()
    private val _callMedia = lossy<CallInfo>()
    private val _floor = lossy<FloorEvent>()
    private val _roster = lossy<RosterUpdate>()
    private val _dialogInfo = lossy<DialogInfo>()
    private val _stopped = lossy<Unit>()

    // ③ 유실 불가 — 무제한 버퍼. 소비는 한 번뿐이라 수집자를 하나만 둔다(Service 의 세션).
    private val _sds = Channel<SdsMessage>(Channel.UNLIMITED)
    private val _requestResult = Channel<RequestResult>(Channel.UNLIMITED)
    private val _message = Channel<SipMessage>(Channel.UNLIMITED)

    val log: SharedFlow<LogLine> = _log.asSharedFlow()
    val regState: SharedFlow<RegInfo> = _regState.asSharedFlow()
    /** 착신 — 180 은 코어가 이미 보냈다. MCPTT 착신은 autoAnswerMcptt 면 200 까지 코어가 보낸다. */
    val incomingCall: SharedFlow<CallInfo> = _incomingCall.asSharedFlow()
    val callState: SharedFlow<CallInfo> = _callState.asSharedFlow()
    val callMedia: SharedFlow<CallInfo> = _callMedia.asSharedFlow()
    /** floor 상태 전이(TS 24.380 §6.2.4). 마이크 게이트는 코어가 이미 처리했다. */
    val floor: SharedFlow<FloorEvent> = _floor.asSharedFlow()
    val roster: SharedFlow<RosterUpdate> = _roster.asSharedFlow()
    /** 감시 대상 dialog(RFC 4235) — Join 대상 선택의 입력. */
    val dialogInfo: SharedFlow<DialogInfo> = _dialogInfo.asSharedFlow()
    val stopped: SharedFlow<Unit> = _stopped.asSharedFlow()

    /** MCData SDS 수신. **유실되지 않는다** — 수집자가 붙기 전 것도 쌓인다. 수집자는 하나만 둔다. */
    val sds: Flow<SdsMessage> = _sds.receiveAsFlow()
    /** 임의 요청의 최종 응답 — SDS·SMS 발신을 token 으로 여기에 맞춘다. **유실되지 않는다**. */
    val requestResult: Flow<RequestResult> = _requestResult.receiveAsFlow()
    /** MCData 가 아닌 MESSAGE/NOTIFY 본문(xcap-diff 등). **유실되지 않는다**. */
    val message: Flow<SipMessage> = _message.receiveAsFlow()

    // ── 상태(권위) ────────────────────────────────────────────────────────────
    private val _running = MutableStateFlow(false)
    val running: StateFlow<Boolean> = _running.asStateFlow()

    private val _registrations = MutableStateFlow<Map<Int, RegInfo>>(emptyMap())
    /** 계정별 최신 등록 상태. 이벤트가 버려져도 여기는 맞다. */
    val registrations: StateFlow<Map<Int, RegInfo>> = _registrations.asStateFlow()

    private val _callIds = MutableStateFlow<List<Int>>(emptyList())
    /** 살아 있는 호 목록. 화면 재구성은 여기서 `callInfo(id)` 로 다시 그린다. */
    val callIds: StateFlow<List<Int>> = _callIds.asStateFlow()

    // ── 엔진 ──────────────────────────────────────────────────────────────────
    suspend fun start(cfg: EngineConfig): CimsResult<Unit> = command {
        CimsResult.of(engine.start(cfg.toJni(), listener)).also { if (it.ok) _running.value = true }
    }

    suspend fun stop() {
        withContext(io) { guarded { engine.stop() } }
        _running.value = false
        _callIds.value = emptyList()
        _registrations.value = emptyMap()
        accountCache.clear()
        callCache.clear()
    }

    /**
     * 엔진 종료 + 네이티브 해제. 멱등이다.
     *
     * 신규 호출을 먼저 막고 진행 중인 JNI 호출이 끝나기를 기다린다 — 그러지 않으면 다른 스레드가
     * 네이티브를 쓰는 중에 해제돼 use-after-free 가 된다.
     */
    override fun close() {
        if (!closed.compareAndSet(false, true)) return
        // 게이트를 닫은 뒤 진행 중인 호출이 빠지기를 기다린다.
        synchronized(gate) {
            while (inFlight.get() > 0) {
                runCatching { (gate as Object).wait(CLOSE_WAIT_MS) }.getOrElse { return@synchronized }
            }
        }
        runCatching { engine.stop() }          // 콜백을 멈춘다 — director 가 살아 있는 동안
        _running.value = false
        accountCache.clear()
        callCache.clear()
        _sds.close(); _requestResult.close(); _message.close()
        runCatching { engine.delete() }
        runCatching { listener.delete() }      // 소유권을 건드리지 않았으므로 네이티브 director 가 실제로 해제된다
    }

    // ── 계정 ──────────────────────────────────────────────────────────────────
    private val accountCache = ConcurrentHashMap<Int, Account>()
    private val callCache = ConcurrentHashMap<Int, Call>()
    /** 호 세대 — pjsua 가 callId 를 순환 재사용하므로(pjsua_call.c alloc_call_id) 낡은 핸들이 다른 호에
     *  명령을 보내지 않도록 id 마다 세대를 매긴다. */
    private val callEpoch = ConcurrentHashMap<Int, Long>()
    private val nextEpoch = AtomicLong(1)

    suspend fun addAccount(cfg: AccountConfig): CimsResult<Account> = command {
        val id = engine.addAccount(cfg.toJni())
        if (id < 0) CimsResult.fail(-1, "addAccount failed") else CimsResult.ok(account(id))
    }

    /** id 를 감싼 계정 핸들. 같은 id 면 같은 객체다. */
    fun account(accountId: Int): Account = accountCache.getOrPut(accountId) { Account(this, accountId) }
    fun regInfo(accountId: Int): RegInfo =
        guarded { RegInfo.of(engine.regInfo(accountId)) } ?: RegInfo(accountId, RegState.UNREGISTERED, 0, "closed", 0)

    // ── 호 ────────────────────────────────────────────────────────────────────
    /** id 를 감싼 호 핸들. 같은 id·같은 세대면 같은 객체다. */
    fun call(callId: Int): Call = callCache.getOrPut(callId) { Call(this, callId, epochOf(callId)) }

    private fun epochOf(callId: Int): Long = callEpoch.getOrPut(callId) { nextEpoch.getAndIncrement() }

    /** 새 호가 나타났다 — 이전 세대의 핸들을 무효화한다. */
    private fun newEpoch(callId: Int) {
        callEpoch[callId] = nextEpoch.getAndIncrement()
        callCache.remove(callId)
    }

    internal fun isCurrent(callId: Int, epoch: Long): Boolean = callEpoch[callId] == epoch

    /** 잠금 스냅샷 읽기 — 제어 스레드를 기다리지 않으므로 어느 스레드에서 불러도 된다. */
    fun callInfo(callId: Int): CallInfo? = guarded { CallInfo.of(engine.callInfo(callId)) }
    fun calls(): List<Int> = guarded { engine.calls().let { v -> List(v.size) { v[it] } } } ?: emptyList()

    /** 제어 스레드를 기다린다 — 메인 스레드에서 부르지 않는다. */
    suspend fun floorInfo(callId: Int): FloorInfo? = withContext(io) { guarded { FloorInfo.of(engine.floorInfo(callId)) } }
    suspend fun streamStats(callId: Int): StreamStats? = withContext(io) { guarded { StreamStats.of(engine.streamStats(callId)) } }

    // ── 장치 ──────────────────────────────────────────────────────────────────
    suspend fun audioDevices(): List<AudioDeviceInfo> =
        withContext(io) { guarded { AudioDeviceInfo.list(engine.audioDevices()) } } ?: emptyList()

    suspend fun refreshAudioDevices(): CimsResult<Unit> = command { CimsResult.of(engine.refreshAudioDevices()) }

    /** 캡처/재생 장치 선택(pjmedia id). -1=기본 캡처, -2=기본 재생. */
    suspend fun setAudioDevices(captureDev: Int, playbackDev: Int): CimsResult<Unit> =
        command { CimsResult.of(engine.setAudioDevices(captureDev, playbackDev)) }

    /**
     * 추가 재생 라우트(ue_sdk.md §6.3). 반환 routeId ≥ 1.
     *
     * **Android 에서는 이것만으로 물리 출력이 갈라지지 않는다** — pjmedia Android 백엔드는 장치를
     * 하나만 노출하므로(`android_jni_dev.c` `android_get_dev_count` = 1) 라우트를 더 열어도 같은 sink 로
     * 나간다. 스트림별 분리는 백엔드의 `PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE` 로만 되며 코어에 그 통로가
     * 아직 없다(§11 미해결).
     */
    suspend fun addPlaybackRoute(playbackDev: Int): CimsResult<Int> = command {
        engine.addPlaybackRoute(playbackDev).let {
            if (it < 0) CimsResult.fail(-1, "addPlaybackRoute failed") else CimsResult.ok(it)
        }
    }

    suspend fun removePlaybackRoute(routeId: Int): CimsResult<Unit> =
        command { CimsResult.of(engine.removePlaybackRoute(routeId)) }

    /** SIP TLS 서버 인증서 만료 관측 — 아직 TLS 연결이 없으면 valid=false. */
    fun tlsPeerExpiry(): TlsPeerExpiry? = guarded { TlsPeerExpiry.of(engine.tlsPeerExpiry()) }

    // ── 내부 — Account/Call 이 쓴다 ──────────────────────────────────────────
    internal val jni: JniEngine get() = engine
    internal fun toJniDialog(d: DialogInfo): JniDialogInfo = d.toJni()
    internal fun headers(map: Map<String, String>): com.cims.ue.sdk.jni.StringMap =
        com.cims.ue.sdk.jni.StringMap().apply { map.forEach { (k, v) -> put(k, v) } }

    internal fun refreshCallIds() { _callIds.value = calls() }

    internal suspend fun callCommand(callId: Int, block: () -> CimsResult<Call>): CimsResult<Call> =
        command { block() }

    /** 코어 이벤트 스레드에서 오는 director 콜백 — 기록하고 방출만 한다. 여기서 블록하면 안 된다. */
    private inner class FlowListener : JniListener() {
        override fun onLog(level: Int, msg: String) { _log.tryEmit(LogLine(level, msg)) }

        override fun onRegState(info: JniRegInfo) {
            val r = RegInfo.of(info)
            // 이벤트는 코어가 한 스레드로 직렬화한다(ue_sdk.md §4.3) — read-modify-write 가 안전하다.
            _registrations.value = _registrations.value + (r.accountId to r)
            _regState.emitLossy(r)
        }

        override fun onIncomingCall(info: JniCallInfo) {
            val c = CallInfo.of(info)
            newEpoch(c.callId)
            refreshCallIds()
            _incomingCall.emitLossy(c)
        }

        override fun onCallState(info: JniCallInfo) {
            val c = CallInfo.of(info)
            if (c.state == CallState.OUTGOING) newEpoch(c.callId)
            refreshCallIds()
            if (c.ended) callCache.remove(c.callId)
            _callState.emitLossy(c)
        }

        override fun onCallMedia(info: JniCallInfo) { _callMedia.emitLossy(CallInfo.of(info)) }
        override fun onFloor(ev: JniFloorEvent) { _floor.emitLossy(FloorEvent.of(ev)) }

        override fun onRoster(accountId: Int, groupId: String, users: RosterVector, full: Boolean) {
            _roster.emitLossy(RosterUpdate.of(accountId, groupId, users, full))
        }

        override fun onDialogInfo(d: JniDialogInfo) { _dialogInfo.emitLossy(DialogInfo.of(d)) }

        // ③ 유실 불가 — trySend 는 UNLIMITED 채널이라 닫히지 않은 한 실패하지 않는다.
        override fun onSds(msg: JniSdsMessage) { _sds.trySend(SdsMessage.of(msg)) }
        override fun onRequestResult(r: JniRequestResult) { _requestResult.trySend(RequestResult.of(r)) }
        override fun onMessage(accountId: Int, fromUri: String, contentType: String, body: String) {
            _message.trySend(SipMessage(accountId, fromUri, contentType, body))
        }

        override fun onEngineStopped() {
            _running.value = false
            _stopped.emitLossy(Unit)
        }
    }

    private companion object {
        const val CLOSE_WAIT_MS = 2_000L
    }
}

/**
 * 계정 핸들(접속서비스 kind 당 하나). 공개 표면은 id 지만 앱 편의를 위해 객체로 감싼다 —
 * 이름·의미는 Windows .NET `Account` 와 같다.
 *
 * 모든 명령이 `suspend` 인 이유는 코어의 `runSync` 가 제어 스레드를 기다리고, 그 안에서 DNS 해석 같은
 * 동기 I/O 가 일어날 수 있기 때문이다(CimsUe 머리말).
 */
class Account internal constructor(private val ue: CimsUe, val id: Int) {
    /** 잠금 스냅샷 읽기 — 어느 스레드에서 불러도 된다. */
    val regInfo: RegInfo get() = ue.regInfo(id)

    suspend fun register(): CimsResult<Unit> = ue.command { CimsResult.of(ue.jni.registerAccount(id)) }
    suspend fun unregister(): CimsResult<Unit> = ue.command { CimsResult.of(ue.jni.unregisterAccount(id)) }
    /** 즉시 재-REGISTER — 서버 재기동 등으로 등록을 잃었을 때. */
    suspend fun refreshRegistration(): CimsResult<Unit> = ue.command { CimsResult.of(ue.jni.refreshRegistration(id)) }
    suspend fun remove(): CimsResult<Unit> = ue.command { CimsResult.of(ue.jni.removeAccount(id)) }

    private fun callOrFail(callId: Int, what: String): CimsResult<Call> =
        if (callId < 0) CimsResult.fail(-1, "$what failed")
        else CimsResult.ok(ue.call(callId).also { ue.refreshCallIds() })

    /** 발신. target 은 번호(도메인 자동 결합) 또는 sip: URI. */
    suspend fun dial(target: String, opts: CallOptions = CallOptions()): CimsResult<Call> =
        ue.command { callOrFail(ue.jni.dial(id, target, opts.toJni()), "dial") }

    /** 그룹콜 참여. groupId 는 bare id(예 "g001"). 같은 그룹 세션이 있으면 그 호를 돌려준다. */
    suspend fun joinGroupCall(groupId: String, opts: GroupCallOptions = GroupCallOptions()): CimsResult<Call> =
        ue.command { callOrFail(ue.jni.joinGroupCall(id, groupId, opts.toJni()), "joinGroupCall") }

    /** 1:1 사설콜(session-type=private). peer 는 bare 번호. */
    suspend fun startPrivateCall(peer: String, opts: GroupCallOptions = GroupCallOptions()): CimsResult<Call> =
        ue.command { callOrFail(ue.jni.startPrivateCall(id, peer, opts.toJni()), "startPrivateCall") }

    /** affiliation PUBLISH(TS 24.379 §9). 반환 token 으로 `requestResult` 에서 확인한다. */
    suspend fun affiliate(groupId: String, on: Boolean): CimsResult<Long> = ue.command {
        ue.jni.affiliate(id, groupId, on).let {
            if (it < 0) CimsResult.fail(-1, "affiliate failed") else CimsResult.ok(it)
        }
    }

    /** 그룹 로스터 구독(RFC 4575 conference) — 확인 신호는 `roster` NOTIFY. */
    suspend fun subscribeConference(groupId: String, on: Boolean): CimsResult<Unit> =
        ue.command { CimsResult.of(ue.jni.subscribeConference(id, groupId, on)) }

    /** 문서 변경 구독(RFC 5875 xcap-diff) — 본문은 `message` 로 온다. */
    suspend fun subscribeXcapDiff(psiUri: String, on: Boolean): CimsResult<Unit> =
        ue.command { CimsResult.of(ue.jni.subscribeXcapDiff(id, psiUri, on)) }

    /** 임의 SIP 요청(MESSAGE/PUBLISH/SUBSCRIBE …). 반환 token 으로 `requestResult` 와 상관한다. */
    suspend fun sendRequest(method: String, targetUri: String, contentType: String, body: String,
                            headers: Map<String, String> = emptyMap()): CimsResult<Long> = ue.command {
        ue.jni.sendRequest(id, method, targetUri, contentType, body, ue.headers(headers))
            .let { if (it < 0) CimsResult.fail(-1, "sendRequest failed") else CimsResult.ok(it) }
    }

    // ── 관제 (dispatch_center.md §5) ──
    /** 대상 AoR 의 dialog 구독(RFC 4235, 인가 = 역할 monitorCall). NOTIFY → `dialogInfo`. */
    suspend fun dialogWatch(targetAor: String, on: Boolean): CimsResult<Unit> =
        ue.command { CimsResult.of(ue.jni.dialogWatch(id, targetAor, on)) }

    /** 통화 청취 합류 — INVITE-with-Join(RFC 3911) + a=recvonly. dlg 는 `dialogInfo` 로 학습한 대상. */
    suspend fun join(targetUri: String, dlg: DialogInfo): CimsResult<Call> =
        ue.command { callOrFail(ue.jni.join(id, targetUri, ue.toJniDialog(dlg)), "join") }

    /** 당겨받기 — 그룹 픽업은 피처코드만, 지정 픽업은 번호까지(TS 24.239). */
    suspend fun pickup(featureCode: String, number: String = ""): CimsResult<Call> =
        ue.command { callOrFail(ue.jni.pickup(id, featureCode, number), "pickup") }

    // ── MCData SDS (TS 24.282) ──
    /** 그룹 SDS 발신. 최종 응답은 `requestResult` 에 같은 token 으로 온다. */
    suspend fun sendGroupSds(groupId: String, text: String, requestDelivery: Boolean = true): CimsResult<SdsSend> =
        ue.command { SdsSend.of(ue.jni.sendGroupSds(id, groupId, text, requestDelivery)) }

    /** SDS disposition 통지(notifType 1~4). */
    suspend fun sendSdsNotification(peer: String, convId: String, msgId: String, notifType: Int): CimsResult<SdsSend> =
        ue.command { SdsSend.of(ue.jni.sendSdsNotification(id, peer, convId, msgId, notifType)) }

    override fun toString(): String = "Account#$id"
}

/**
 * 호 핸들. 이름·의미는 Windows .NET `Call` 과 같다.
 *
 * **세대 검사**: pjsua 는 callId 를 순환 재사용한다(`pjsua_call.c` `alloc_call_id`). 종료된 호의 낡은
 * 핸들이 같은 id 를 받은 **다른 호**에 명령을 보내면 엉뚱한 통화를 끊는다. 그래서 핸들은 만들어질 때의
 * 세대를 들고 있고, 세대가 바뀌었으면 명령이 `stale call handle` 로 떨어진다.
 */
class Call internal constructor(private val ue: CimsUe, val id: Int, private val epoch: Long) {
    /** 이 핸들이 아직 그 호를 가리키는가. */
    val isStale: Boolean get() = !ue.isCurrent(id, epoch)

    /** 잠금 스냅샷 읽기. 세대가 지났으면 null. */
    val info: CallInfo? get() = if (isStale) null else ue.callInfo(id)

    suspend fun floorInfo(): FloorInfo? = if (isStale) null else ue.floorInfo(id)
    suspend fun streamStats(): StreamStats? = if (isStale) null else ue.streamStats(id)

    private suspend fun cmd(block: () -> CimsResult<Unit>): CimsResult<Unit> =
        if (isStale) CimsResult.fail(-98, "stale call handle") else ue.command(block)

    suspend fun answer(opts: CallOptions = CallOptions()): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.answer(id, opts.toJni())) }
    suspend fun reject(statusCode: Int = 486): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.reject(id, statusCode)) }
    suspend fun hangup(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.hangup(id)) }
    suspend fun hold(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.hold(id)) }
    suspend fun resume(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.resume(id)) }
    /** 마이크 차단/복구. MCPTT 세션에서는 floor 가 마이크를 게이트하므로 무시된다. */
    suspend fun setMuted(muted: Boolean): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.setMuted(id, muted)) }
    /** 호 → 스피커 청취 on/off (여러 채널 듣기 정책). */
    suspend fun setListen(listen: Boolean): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.setListen(id, listen)) }
    suspend fun setRxLevel(level: Float): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.setRxLevel(id, level)) }
    suspend fun sendDtmf(digits: String): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.sendDtmf(id, digits)) }
    suspend fun leaveGroupCall(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.leaveGroupCall(id)) }

    /** PTT 누름 — Floor Request. 응답은 `floor` 이벤트(Granted/Denied/Queued). priority<0 = 미기재. */
    suspend fun floorRequest(priority: Int = -1): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.floorRequest(id, priority)) }
    /** PTT 뗌 — Floor Release(대기 중이면 Queued Cancel 선행). */
    suspend fun floorRelease(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.floorRelease(id)) }
    suspend fun floorQueueCancel(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.floorQueueCancel(id)) }

    /** 호 전달 blind — REFER(RFC 3515). */
    suspend fun transfer(target: String): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.transfer(id, target)) }
    /** 호 전달 attended — Refer-To 에 Replaces(상담 호의 dialog). */
    suspend fun transferAttended(consult: Call): CimsResult<Unit> =
        if (consult.isStale) CimsResult.fail(-98, "stale consult handle")
        else cmd { CimsResult.of(ue.jni.transferAttended(id, consult.id)) }

    /** 수신 음성을 재생할 라우트(0=기본). 활성 호면 즉시 재결선. */
    suspend fun setRoute(routeId: Int): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.setCallRoute(id, routeId)) }

    override fun toString(): String = "Call#$id@$epoch"
}
