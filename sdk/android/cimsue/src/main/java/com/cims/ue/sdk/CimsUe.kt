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

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.view.Surface
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
import com.cims.ue.sdk.jni.EmergencyAlert as JniEmergencyAlert
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
    private val _condition = lossy<ConditionChange>()
    private val _transmission = lossy<TransmissionEvent>()
    private val _reception = lossy<ReceptionEvent>()
    private val _videoRequest = lossy<VideoRequestEvent>()
    private val _nonAcknowledged = lossy<CallInfo>()
    private val _stopped = lossy<Unit>()

    // ③ 유실 불가 — 무제한 버퍼. 소비는 한 번뿐이라 수집자를 하나만 둔다(Service 의 세션).
    private val _sds = Channel<SdsMessage>(Channel.UNLIMITED)
    private val _requestResult = Channel<RequestResult>(Channel.UNLIMITED)
    private val _message = Channel<SipMessage>(Channel.UNLIMITED)
    private val _emergencyAlert = Channel<EmergencyAlert>(Channel.UNLIMITED)

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
    /** MCPTT 세션 조건 변화(긴급·임박, TS 24.379 §10.1.1.2.1.3~6) — 권위는 `callInfo().condition` 스냅샷(② 정책). */
    val condition: SharedFlow<ConditionChange> = _condition.asSharedFlow()
    /** MCVideo 송출 제어(TS 24.581 §6.2.4) — 허가·거절·회수·대기·종료. 송출 게이트는 코어가 이미 처리했다. */
    val transmission: SharedFlow<TransmissionEvent> = _transmission.asSharedFlow()
    /** MCVideo 수신 제어(§6.2.5) — 새 송출 알림(manual 이면 [Call.acceptReception])·수신 허가·종료. */
    val reception: SharedFlow<ReceptionEvent> = _reception.asSharedFlow()
    /** 통화 중 영상 전환(1:1 호, RFC 3264 §8.1) — 상대의 요청(RECEIVED → [Call.answerVideoRequest])·내 요청의 결과. 권위는
     *  `callInfo().videoRequest` 스냅샷(② 정책). */
    val videoRequest: SharedFlow<VideoRequestEvent> = _videoRequest.asSharedFlow()
    /** 내가 연 그룹 통화에 필수 멤버가 응답하지 않은 채 진행됐다(TS 24.379 §6.3.3.3) — `nonAcknowledgedUsers` 가 그 목록. */
    val nonAcknowledged: SharedFlow<CallInfo> = _nonAcknowledged.asSharedFlow()
    val stopped: SharedFlow<Unit> = _stopped.asSharedFlow()

    /** MCData SDS 수신. **유실되지 않는다** — 수집자가 붙기 전 것도 쌓인다. 수집자는 하나만 둔다. */
    val sds: Flow<SdsMessage> = _sds.receiveAsFlow()
    /** 임의 요청의 최종 응답 — SDS·SMS 발신을 token 으로 여기에 맞춘다. **유실되지 않는다**. */
    val requestResult: Flow<RequestResult> = _requestResult.receiveAsFlow()
    /** MCData 가 아닌 MESSAGE/NOTIFY 본문(xcap-diff 등). **유실되지 않는다**. */
    val message: Flow<SipMessage> = _message.receiveAsFlow()
    /** 긴급 경보·취소·그룹 긴급 통지 수신(TS 24.379 §12.1.1.3). **유실되지 않는다** — 경보는 스냅샷이 없다. */
    val emergencyAlert: Flow<EmergencyAlert> = _emergencyAlert.receiveAsFlow()

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
    /**
     * 엔진 기동. [context] 를 주면 영상 캡처(카메라)를 쓸 수 있게 Camera2 `CameraManager` 를 엔진 영상 장치에 넘긴다 — 장치 열거가
     * 기동 때 한 번이라 **기동 전에** 넣어야 한다(없으면 카메라가 목록에 없다 — 수신 영상은 된다).
     */
    suspend fun start(cfg: EngineConfig, context: Context? = null): CimsResult<Unit> = command {
        context?.let { ctx ->
            (ctx.getSystemService(Context.CAMERA_SERVICE) as? CameraManager)?.let { org.pjsip.PjCameraInfo2.SetCameraManager(it) }
        }
        CimsResult.of(engine.start(cfg.toJni(), listener)).also {
            if (it.ok) _running.value = true
            if (it.ok && context != null) applyCaptureRotation(context, 0)   // 화면 자연 방향(세로 고정 휴대폰 앱)
        }
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
        // 계측 링크는 엔진을 참조한다 — 엔진을 멈추기 전에 멈추고 해제한다(구동 호 정리도 엔진이 살아 있어야 한다).
        links.forEach { it.release() }
        links.clear()
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
    /** MCVideo 호의 전송 제어 현재값. */
    suspend fun transmissionInfo(callId: Int): TransmissionInfo? =
        withContext(io) { guarded { TransmissionInfo.of(engine.transmissionInfo(callId)) } }
    suspend fun streamStats(callId: Int): StreamStats? = withContext(io) { guarded { StreamStats.of(engine.streamStats(callId)) } }
    /** 호 품질(손실·폐기·지터·RTD·E-model MOS — ue_voice_quality.md §3). */
    suspend fun callQuality(callId: Int): CallQuality? = withContext(io) { guarded { CallQuality.of(engine.callQuality(callId)) } }

    // ── 장치 ──────────────────────────────────────────────────────────────────
    suspend fun audioDevices(): List<AudioDeviceInfo> =
        withContext(io) { guarded { AudioDeviceInfo.list(engine.audioDevices()) } } ?: emptyList()

    suspend fun refreshAudioDevices(): CimsResult<Unit> = command { CimsResult.of(engine.refreshAudioDevices()) }

    /**
     * 망이 바뀌었다(기본 망 전환·끊겼다 복귀) — 코어가 TCP/TLS 연결을 닫고 등록을 켠 계정마다 다시 등록한다
     * (`Engine::handleNetworkChange` — 앞 등록이 걸려 있으면 끝난 뒤 한 번 더). 앱은 망 콜백에서 «복귀·전환» 을 판정해 부르기만 한다.
     */
    suspend fun handleNetworkChange(): CimsResult<Unit> = command { CimsResult.of(engine.handleNetworkChange()) }

    /** 캡처/재생 장치 선택(pjmedia id). -1=기본 캡처, -2=기본 재생. */
    suspend fun setAudioDevices(captureDev: Int, playbackDev: Int): CimsResult<Unit> =
        command { CimsResult.of(engine.setAudioDevices(captureDev, playbackDev)) }

    /**
     * 캡처 게이트 — false 면 캡처 스트림(AudioRecord)을 열지 않고 재생만 한다. 다른 앱에 마이크를 양보하는 구간
     * (`CimsSuite` 마이크 양보)이나 PTT 유휴·청취에서 OS 동시 캡처 중재에서 빠질 때. 호 음소거·floor 게이트와 별개, 기본 true.
     */
    suspend fun setCaptureEnabled(on: Boolean): CimsResult<Unit> = command { CimsResult.of(engine.setCaptureEnabled(on)) }
    val captureEnabled: Boolean get() = guarded { engine.captureEnabled() } ?: true

    /**
     * 장치 단 음량(ue_audio_level.md §2·§6) — [speaker] = 스피커 배율(1 = 원음), [micTargetDbov] = 마이크 AGC 목표(-40..-10 dBov,
     * 기본 [MIC_AGC_TARGET_DBOV]). 마이크는 배율이 아니라 AGC 목표로 옮긴다 — 단말마다 마이크 디지털 레벨이 34 dB 넘게 달라 크기는 AGC 가
     * 맞춘다. 코어가 값을 기억해 게이트 전환·재오픈·호 결선 뒤 다시 건다. 호별 듣는 크기는 [Call.setRxLevel].
     */
    suspend fun setDeviceAudioLevels(speaker: Float, micTargetDbov: Double = MIC_AGC_TARGET_DBOV): CimsResult<Unit> =
        command { CimsResult.of(engine.setDeviceAudioLevels(speaker, micTargetDbov)) }

    /**
     * 엔진 오디오 라우트 — [output] 스피커폰·수화기·기본, [input] 마이크(EARPIECE = 내장 기본 마이크 고정, 재오픈에도 유지).
     * 무전(반이중)은 단말 스피커·수화기로 들을 때 입력을 EARPIECE 로 고정한다(스피커 출력이면 정책이 후면 마이크를 골라 ~20 dB 작다),
     * 전이중 스피커폰은 에코 때문에 DEFAULT. 라우트를 무시하는 단말은 [com.cims.ue.sdk.platform.AudioRouter] 를 병행한다.
     */
    suspend fun setAudioRoute(output: AudioRoute, input: AudioRoute = AudioRoute.DEFAULT): CimsResult<Unit> = command {
        CimsResult.of(engine.setAudioRoute(com.cims.ue.sdk.jni.AudioRoute.swigToEnum(output.ordinal),
            com.cims.ue.sdk.jni.AudioRoute.swigToEnum(input.ordinal)))
    }

    /** 사운드 장치 재오픈 — 라우팅 중이던 출력 장치(BT·이어폰)가 사라진 뒤 재생 트랙에 시스템 뮤트가 남는 단말 대응. 닫혀 있으면 무동작. */
    suspend fun reopenAudioDevice(): CimsResult<Unit> = command { CimsResult.of(engine.reopenAudioDevice()) }

    // ── 영상 (ue_sdk.md §4.5 — 코어는 창을 열지 않는다) ──
    /** 수신 영상을 그릴 Surface(null = 해제). 활성 영상 호에 곧바로, 뒤에 영상이 활성되는 호에도 쓴다. */
    suspend fun setVideoSurface(surface: Surface?): CimsResult<Unit> = command { CimsResult.of(engine.setVideoWindow(surface)) }

    /**
     * 셀프뷰(내 카메라) Surface(null = 해제). 카메라를 두 번 열지 않는다 — 엔진 캡처가 연 카메라의 세션에 출력으로 더한다
     * (Android 카메라 단일 개방 제약, CIMS PjCamera2 패치). 통화 전에 걸어 두면 캡처 시작 때 붙고, 통화 중이면 세션이 다시 구성된다.
     */
    fun setPreviewSurface(surface: Surface?) { runCatching { org.pjsip.PjCamera2.SetPreviewSurface(surface) } }

    /**
     * 카메라 프레임을 화면 방향으로 세운다(ue_sdk.md §4.5 — 인코딩은 480x640 세로). Android 카메라 센서는 대개 가로라 걸지 않으면 가로 그림을
     * 세로 틀에 줄여 넣어 위아래가 검게 간다. 카메라마다 Camera2 `SENSOR_ORIENTATION` 으로 계산한다 — 앞 = (센서 + 화면) mod 360,
     * 뒤 = (센서 − 화면) mod 360. [displayRotation] = 화면이 자연 방향에서 시계 방향으로 돈 각도(0·90·180·270, `Surface.ROTATION_*`).
     * [start] 가 0 으로 한 번 건다 — 화면을 돌리는 앱은 회전마다 다시 부른다.
     */
    suspend fun setCaptureRotation(context: Context, displayRotation: Int = 0): CimsResult<Unit> =
        command { applyCaptureRotation(context, displayRotation) }

    private fun applyCaptureRotation(ctx: Context, displayRotation: Int): CimsResult<Unit> {
        val cm = ctx.getSystemService(Context.CAMERA_SERVICE) as? CameraManager ?: return CimsResult.fail(-1, "no camera service")
        fun sensorOf(facing: Int): Int? = runCatching {
            cm.cameraIdList.map { cm.getCameraCharacteristics(it) }
                .firstOrNull { it.get(CameraCharacteristics.LENS_FACING) == facing }
                ?.get(CameraCharacteristics.SENSOR_ORIENTATION)
        }.getOrNull()
        val disp = ((displayRotation % 360) + 360) % 360
        var last: CimsResult<Unit> = CimsResult.ok(Unit)
        for (d in VideoDeviceInfo.list(engine.videoDevices())) {
            if (!d.capture || d.driver != "Android") continue
            val front = d.name.contains("Front", ignoreCase = true)          // pjmedia android_dev 이름 "Front camera"/"Back camera"
            val sensor = sensorOf(if (front) CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK)
                ?: continue
            val rot = if (front) (sensor + disp) % 360 else (sensor - disp + 360) % 360
            CimsResult.of(engine.setCaptureRotation(d.id, rot)).let { if (!it.ok) last = it }
        }
        return last
    }

    suspend fun videoDevices(): List<VideoDeviceInfo> =
        withContext(io) { guarded { VideoDeviceInfo.list(engine.videoDevices()) } } ?: emptyList()

    /**
     * 추가 재생 라우트(ue_sdk.md §6.3). 반환 routeId ≥ 1.
     *
     * **Android 에서는 이것만으로 물리 출력이 갈라지지 않는다** — pjmedia Android 백엔드는 장치를
     * 하나만 노출하므로(`android_jni_dev.c` `android_get_dev_count` = 1) 라우트를 더 열어도 같은 sink 로
     * 나간다. 스트림별 분리 통로는 코어에 아직 없다(§11 미해결) — 장치 전체의 출력 라우트는 [setAudioRoute]
     * (백엔드 `PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE`) 다.
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

    // ── 시험 모드 (ue_voice_quality.md §4·§5) ────────────────────────────────
    /**
     * 송출 원천 — 빈 문자열 = 마이크, 경로 = WAV(PCM16, 16 kHz mono 권장) 반복 재생을 마이크 대신 모든 호로(기준 음원).
     * 진행 중 호에도 즉시 적용된다. 음소거·floor 게이트는 원천과 무관하게 그대로다. 계측기의 `media` 명령도 같은 곳을 바꾼다.
     */
    suspend fun setTxSource(wavPath: String): CimsResult<Unit> = command { CimsResult.of(engine.setTxSource(wavPath)) }

    private val links: MutableSet<DeviceLink> = ConcurrentHashMap.newKeySet()

    /** 계측기 워커 링크 핸들을 만든다(아직 연결하지 않는다 — [DeviceLink.start]). 엔진이 닫혔으면 실패. */
    fun deviceLink(): CimsResult<DeviceLink> =
        guarded { DeviceLink(this).also { links.add(it) } }?.let { CimsResult.ok(it) } ?: closedResult()

    internal fun forgetLink(link: DeviceLink) { links.remove(link) }

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
        override fun onTransmission(ev: com.cims.ue.sdk.jni.TransmissionEvent) { _transmission.emitLossy(TransmissionEvent.of(ev)) }
        override fun onReception(ev: com.cims.ue.sdk.jni.ReceptionEvent) { _reception.emitLossy(ReceptionEvent.of(ev)) }
        override fun onVideoRequest(ev: com.cims.ue.sdk.jni.VideoRequestEvent) { _videoRequest.emitLossy(VideoRequestEvent.of(ev)) }

        override fun onRoster(accountId: Int, groupId: String, users: RosterVector, full: Boolean) {
            _roster.emitLossy(RosterUpdate.of(accountId, groupId, users, full))
        }

        override fun onDialogInfo(d: JniDialogInfo) { _dialogInfo.emitLossy(DialogInfo.of(d)) }
        override fun onMcpttCondition(info: JniCallInfo, cause: com.cims.ue.sdk.jni.ConditionCause) {
            _condition.emitLossy(ConditionChange(CallInfo.of(info), ConditionCause.entries.getOrElse(cause.swigValue()) { ConditionCause.LOCAL }))
        }
        override fun onEmergencyAlert(alert: JniEmergencyAlert) { _emergencyAlert.trySend(EmergencyAlert.of(alert)) }
        override fun onNonAcknowledgedUsers(info: JniCallInfo) { _nonAcknowledged.emitLossy(CallInfo.of(info)) }

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

    companion object {
        private const val CLOSE_WAIT_MS = 2_000L

        /** 마이크 AGC 기본 목표(ITU-T P.56 활성 레벨, ue_audio_level.md §4) — 코어 kMicAgcTargetDbov. */
        const val MIC_AGC_TARGET_DBOV = -26.0

        /** REGISTER `User-Agent`(RFC 3261 §20.41) 규약 `<제품>/<앱 버전> (<OS>; <모델>)` — 규칙은 코어 하나(ue_sdk.md §4.2). */
        fun userAgentOf(product: String, version: String, os: String, model: String): String {
            NativeLib.ensure()
            return com.cims.ue.sdk.jni.cimsue.userAgentOf(product, version, os, model)
        }

        /** IMEI → `+sip.instance` URN `urn:gsma:imei:TTTTTTTT-SSSSSS-0`(RFC 7254). 자릿수·검사 숫자가 틀리면 빈 문자열. */
        fun imeiUrn(imei: String): String {
            NativeLib.ensure()
            return com.cims.ue.sdk.jni.cimsue.imeiUrn(imei)
        }
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

    /** affiliation PUBLISH — 서비스마다 따로다. MCPTT = TS 24.379 §9, MCVideo = 관심 그룹 전부를 한 PUBLISH 로(TS 24.281 §8.2.1.2).
     *  반환 token 으로 `requestResult` 에서 확인한다. */
    suspend fun affiliate(groupId: String, on: Boolean, service: McService = McService.MCPTT): CimsResult<Long> = ue.command {
        ue.jni.affiliate(id, groupId, on, com.cims.ue.sdk.jni.McService.swigToEnum(service.ordinal)).let {
            if (it < 0) CimsResult.fail(-1, "affiliate failed") else CimsResult.ok(it)
        }
    }

    /** MCVideo 그룹 호 개시·합류(TS 24.281 §9.2.1·§9.2.2) — Request-URI = [AccountConfig.mcvideoServerUri](재합류는 opts.sessionUri).
     *  나가기 = [Call.hangup](MCPTT 호와 독립). 송출·수신은 [Call.requestTransmission]·[Call.acceptReception]. */
    suspend fun joinVideoGroupCall(groupId: String, opts: VideoGroupCallOptions = VideoGroupCallOptions()): CimsResult<Call> =
        ue.command { callOrFail(ue.jni.joinVideoGroupCall(id, groupId, opts.toJni()), "joinVideoGroupCall") }

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

    // ── 긴급 경보 (TS 24.379 §12) ──
    /**
     * 긴급 경보 발신·취소 — SIP MESSAGE(mcptt-info alert-ind). originatedBy = 다른 사용자의 경보를 취소할 때 그 사용자 MCPTT ID,
     * cancelGroupEmergency = 취소와 함께 그룹의 진행 중 긴급 상태도 해제. 최종 응답은 `requestResult` 에 같은 token 으로 온다.
     * 인가는 서버가 판정한다(미인가 경보는 전파되지 않는다) — 앱은 `Capabilities.emergencyAlert` 로 선차단한다.
     */
    suspend fun sendEmergencyAlert(groupId: String, activate: Boolean, originatedBy: String = "",
                                   cancelGroupEmergency: Boolean = false): CimsResult<Long> = ue.command {
        ue.jni.sendEmergencyAlert(id, groupId, activate, originatedBy, cancelGroupEmergency)
            .let { if (it < 0) CimsResult.fail(-1, "sendEmergencyAlert failed") else CimsResult.ok(it) }
    }

    // ── MCData SDS (TS 24.282) ──
    /** 그룹 SDS 발신. 최종 응답은 `requestResult` 에 같은 token 으로 온다(상한 초과 = MSRP — method "MSRP").
     *  msgId = 재전송이면 처음의 message ID(hex32 — 수신 측 대조), 비면 코어가 만든다. 앱이 먼저 저장하고 보낼 때도 그 ID 를 넘긴다. */
    suspend fun sendGroupSds(groupId: String, text: String, requestDelivery: Boolean = true,
                             msgId: String = ""): CimsResult<SdsSend> =
        ue.command { SdsSend.of(ue.jni.sendGroupSds(id, groupId, text, requestDelivery, msgId)) }

    /**
     * 1:1 SDS 발신(request-type one-to-one-sds). peer 는 상대 bare 번호.
     *
     * 그룹 SDS 와 **갈라 두는 이유**: request-type·Request-URI·conversation ID 가 다르다. 1:1 을 그룹
     * 경로로 보내면 서버가 그룹 게이트를 거쳐 받는 쪽 스레드 귀속도 틀어진다(mcdata_messaging.md §4).
     */
    suspend fun sendSds(peer: String, text: String, requestDelivery: Boolean = true,
                        msgId: String = ""): CimsResult<SdsSend> =
        ue.command { SdsSend.of(ue.jni.sendSds(id, peer, text, requestDelivery, msgId)) }

    // ── MCData FD (TS 24.282 §10.2 — 파일은 먼저 CscClient.uploadFd 로 올린다) ──
    /** 그룹 FD 알림 — file 은 `uploadFd(groupId 지정)` 결과. 최종 응답은 `requestResult` 에 같은 token 으로 온다. */
    suspend fun sendGroupFd(groupId: String, file: FdFile): CimsResult<SdsSend> =
        ue.command { SdsSend.of(ue.jni.sendGroupFd(id, groupId, file.toJni())) }

    /** 1:1 FD 알림(request-type one-to-one-fd). peer 는 상대 bare 번호. */
    suspend fun sendFd(peer: String, file: FdFile): CimsResult<SdsSend> =
        ue.command { SdsSend.of(ue.jni.sendFd(id, peer, file.toJni())) }

    /**
     * SDS disposition 통지(TS 24.282 §12.2.1.1, notifType 1~4) — peer = 받은 SDS 의 `fromUri`(mcdata-calling-user-id),
     * groupUri = 받은 SDS 의 `groupUri`(mcdata-calling-group-id, 1:1 이면 빈 값). 계정 `mcdataServerUri` 가 있으면 규격형
     * (Request-URI = PSI · resource-lists · 그룹 id), 없으면 원 발신자 직행.
     */
    suspend fun sendSdsNotification(peer: String, convId: String, msgId: String, notifType: Int,
                                    groupUri: String = ""): CimsResult<SdsSend> =
        ue.command { SdsSend.of(ue.jni.sendSdsNotification(id, peer, convId, msgId, notifType, groupUri)) }

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
    suspend fun quality(): CallQuality? = if (isStale) null else ue.callQuality(id)

    private suspend fun cmd(block: () -> CimsResult<Unit>): CimsResult<Unit> =
        if (isStale) CimsResult.fail(-98, "stale call handle") else ue.command(block)

    suspend fun answer(opts: CallOptions = CallOptions()): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.answer(id, opts.toJni())) }
    suspend fun reject(statusCode: Int = 486): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.reject(id, statusCode)) }
    suspend fun hangup(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.hangup(id)) }
    suspend fun hold(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.hold(id)) }
    suspend fun resume(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.resume(id)) }
    /** 마이크 차단/복구. 반이중 MCPTT 세션에서는 floor 가 게이트하므로 무시되고, 전이중 사설콜(mc_no_floor_ctrl)에서는
     *  앱의 PTT 로컬 게이트다(누르면 승인 톤 뒤 false, 떼면 true). MCVideo 호는 송출 허가 중 음성 송신만 멈춘다(영상은 계속). */
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

    // ── MCVideo 전송 제어 (TS 24.581) ──
    suspend fun transmissionInfo(): TransmissionInfo? = if (isStale) null else ue.transmissionInfo(id)
    /** [영상 보내기] — Transmission Request(§6.2.4.3.2). 결과는 `transmission` 이벤트. priority<0 = 미기재. */
    suspend fun requestTransmission(priority: Int = -1): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.requestTransmission(id, priority)) }
    /** [보내기 끝] — Transmission End Request(§6.2.4.5.3). 대기·요청 중이면 요청을 거둔다. */
    suspend fun releaseTransmission(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.releaseTransmission(id)) }
    /** [받기] — Receive Media Request(§6.2.5.3.3). transmitterId = `reception`(NOTIFIED) 의 transmitter.userId. */
    suspend fun acceptReception(transmitterId: String, priority: Int = -1): CimsResult<Unit> =
        cmd { CimsResult.of(ue.jni.acceptReception(id, transmitterId, priority)) }
    /** [그만 보기] — Media Reception End Request(§6.2.5.5). */
    suspend fun endReception(transmitterId: String): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.endReception(id, transmitterId)) }

    /**
     * 진행 중 그룹콜의 조건 상향·하향(TS 24.379 §10.1.1.2.1.3~5) — re-INVITE(mcptt-info + Resource-Priority). 결과는 `condition`
     * 이벤트: LOCAL(곧바로 반영) → CONFIRMED(2xx) 또는 DENIED(이전 값 복원 — 미인가 상향 403, 호는 유지). 둘을 함께 true 로 줄 수 없다.
     * 대상 선택·403 뒤 정책(경보 정합 등)은 앱 몫이다.
     */
    suspend fun setCondition(emergency: Boolean, imminentPeril: Boolean = false): CimsResult<Unit> =
        cmd { CimsResult.of(ue.jni.setCallCondition(id, emergency, imminentPeril)) }

    /** 호 전달 blind — REFER(RFC 3515). */
    suspend fun transfer(target: String): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.transfer(id, target)) }
    /** 호 전달 attended — Refer-To 에 Replaces(상담 호의 dialog). */
    suspend fun transferAttended(consult: Call): CimsResult<Unit> =
        if (consult.isStale) CimsResult.fail(-98, "stale consult handle")
        else cmd { CimsResult.of(ue.jni.transferAttended(id, consult.id)) }

    /** 캡처 카메라 전환(전면↔후면) — 이후 호의 기본 카메라로도 쓴다. */
    suspend fun switchCamera(): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.switchCamera(id)) }

    /** 내 영상 송출 허용 — MCVideo 호는 허용이면서 송출 허가를 가진 동안만 보내고(허가 = 송출 시작·키프레임, 종료 = 정지·카메라 닫힘),
     *  그 밖의 호는 곧바로 시작·정지한다. 재협상 없음. */
    suspend fun setVideoSend(on: Boolean): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.setVideoSend(id, on)) }

    /** 통화 중 영상 전환(1:1 호, re-INVITE — RFC 3264 §8.1·§8.2). on = 추가 **요청** — 결과는 `videoRequest` 이벤트(ACCEPTED·DECLINED·
     *  FAILED)와 [CallInfo.video], off = 제거(묻지 않는다). 성립 전·보류 중·진행 중인 요청이 있으면 실패. */
    suspend fun setVideo(on: Boolean): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.setCallVideo(id, on)) }
    /** 상대의 영상 추가 요청(`videoRequest` RECEIVED)에 답한다 — accept = 영상을 받는 200 OK, false = m=video port 0(음성은 그대로). */
    suspend fun answerVideoRequest(accept: Boolean): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.answerVideoRequest(id, accept)) }

    /** 수신 음성을 재생할 라우트(0=기본). 활성 호면 즉시 재결선. */
    suspend fun setRoute(routeId: Int): CimsResult<Unit> = cmd { CimsResult.of(ue.jni.setCallRoute(id, routeId)) }

    override fun toString(): String = "Call#$id@$epoch"
}
