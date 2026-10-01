package com.cims.ue.ptt

import android.content.Context
import android.os.SystemClock
import android.util.Log
import com.cims.ue.core.config.SipAccountConfig
import com.cims.ue.core.sip.CimsTrustStore
import com.cims.ue.core.sip.RegState
import com.cims.ue.ptt.audio.PttFeedback
import com.cims.ue.ptt.csc.CscConfig
import com.cims.ue.ptt.csc.GroupDoc
import com.cims.ue.ptt.csc.GroupSummary
import com.cims.ue.sdk.Account
import com.cims.ue.sdk.AccountConfig
import com.cims.ue.sdk.AudioRoute
import com.cims.ue.sdk.AuthScheme
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.CimsUe
import com.cims.ue.sdk.CscClient
import com.cims.ue.sdk.CscEndpoint
import com.cims.ue.sdk.EngineConfig
import com.cims.ue.sdk.FloorIndicator
import com.cims.ue.sdk.FloorState
import com.cims.ue.sdk.MediaSecurity
import com.cims.ue.sdk.RegInfo
import com.cims.ue.sdk.SdsMessage
import com.cims.ue.sdk.Transport
import com.cims.ue.sdk.UeInitConfigDoc
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.launch
import com.cims.ue.sdk.RegState as SdkRegState

/** 현재 발언자 — 내 GRANT([self]=true) 또는 타인 TAKEN. [sinceMs]=elapsedRealtime(경과시간 표시용).
 *  [groupId]=발언이 들리는 그룹(멀티그룹 모니터링에서 주채널 밖 발언 구분). */
data class Speaker(val id: String, val self: Boolean, val sinceMs: Long, val groupId: String? = null)

/** 채널 지정 — 주(발언 대상, 1개)/부(모니터링)/일반. */
enum class ChannelRole { PRIMARY, NONE }

/** 듣기 정책 — 주채널만 / 참여한 모든 그룹. */
enum class ListenPolicy { CHANNELS_ONLY, ALL }

/** 통화이력용 이벤트 종별. */
enum class PttEventKind { JOIN, LEAVE, TALK_ME, TALK_OTHER, EMERGENCY, EMERGENCY_IN, EMERGENCY_END, ALERT, ALERT_IN, ALERT_END }

/** 활성 긴급경보 (TS 24.379 §12 emergency alert) — 통화와 별개인 위험 통지 상태.
 *  [mine]=내가 발신(취소 MESSAGE 는 SOS 해제와 함께 나간다). */
data class ActiveAlert(val groupId: String, val userId: String, val atMs: Long, val mine: Boolean)

/** 통화이력용 이벤트 — [PttController.onEvent] 로 방출(서비스가 HistoryStore 에 영속). */
data class PttEvent(val kind: PttEventKind, val groupId: String, val peer: String? = null, val durationMs: Long = 0)

/** 참여 중인 그룹 세션의 UI 상태. */
data class GroupCallState(
    val groupId: String,
    val callId: Int,                      // -1 = 협상 중
    val active: Boolean,                  // 통화 성립 여부
    val role: ChannelRole,
    val floorState: FloorState,
    val speaker: Speaker?,
    val participants: Map<String, String>,
    val audible: Boolean,                 // 듣기 정책 적용 결과
    val emergency: Boolean = false,       // 긴급 상태(내 개시 또는 수신 감지)
    val emergencyMine: Boolean = false,   // 내가 개시자(취소 권한 — 서버는 개시자 취소만 수용)
    val volume: Float = 1f,               // 채널별 수신 음량(0~2, 1=원음)
    /** 발언 요청 가능 여부 — Floor Taken 의 Permission to Request the Floor(TS 24.380 §8.2.3.7).
     *  일제 통화·ambient(recv_only) 청취 leg 는 0 이 와서 PTT 버튼을 비활성화한다. */
    val canRequestFloor: Boolean = true,
    /** 내 발언 마감 시각(elapsedRealtime ms) — Granted Duration(T2) 기반 잔여시간 표시. 0=제한 없음. */
    val speakDeadlineMs: Long = 0,
    /** 내 대기열 위치 — Queue Position Info(TS 24.380 §8.2.3.5). null=대기 중 아님. */
    val queuePosition: Int? = null,
    /** 동시 발언(dual/multi) 중인 화자 전체. 단일 발언이면 1명, 유휴면 빈 목록.
     *  [speaker] 는 이 중 대표(내가 있으면 나, 아니면 첫 타인)다. */
    val talkers: List<Speaker> = emptyList(),
    /** 이 세션의 Floor Indicator 비트 — dual floor(G)/multi-talker(I) 표시용. */
    val floorIndicator: Int = 0,
    /** 1:1 private call(TS 24.379 §11.1) — groupId 자리에 상대 번호. 채널 편성과 무관한 즉석 세션. */
    val privatePeer: Boolean = false,
    /** 전이중 1:1(mc_no_floor_ctrl 협상) — floor 없음, PTT 가 로컬 마이크 게이트. PTT 버튼 대신 통화 UI. */
    val fullDuplex: Boolean = false,
)

/**
 * MCPTT 그룹 PTT 세션 — 단말 SDK(`:cimsue`) 위의 **앱 세션 층**(ue_sdk.md §5.3 P3).
 *
 * 규격 절차(floor participant·affiliation PUBLISH ETag·긴급 re-INVITE·경보·MSRP·CMS 해석)는 코어가 하고,
 * 여기는 **정책**(affiliation 목표 집합과 재시도·채널 복원·듣기 정책·긴급 대상 선택·로스터/문서 구독 재확인)과
 * 화면 상태 투영만 한다. 평면별 동작은 같은 패키지의 PttGroups·PttFloor·PttMessaging·PttEmergency 가 나눠 든다.
 *
 * **멀티그룹 동시 참여**(TS 22.179 group scanning): 그룹마다 독립 호를 유지하고, 발언(PTT)은 **주채널**(또는
 * 활성 1:1·애드혹)로만, 수신은 코어 conference bridge 가 믹싱하되 [ListenPolicy] 에 따라 비채널 그룹을 음소거한다.
 */
class PttController(
    private val context: Context,
    internal val sipConfig: SipAccountConfig,
    /** MCPTT ID (tel: URI, 예 "tel:+82571900001") — CSC userUri·calling-user-id. */
    val mcpttId: String,
    cscConfig: CscConfig? = null,
    /** REGISTER User-Agent(mcptt_management_views.md §4.1) — DeviceIdentity.userAgent. */
    private val userAgent: String = "CIMS-PTT",
    /** REGISTER Contact +sip.instance — DeviceIdentity.instanceUrn. */
    private val instanceId: String? = null,
) {
    private val errors = CoroutineExceptionHandler { _, e -> Log.e(TAG, "controller 예외", e) }
    internal val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default + errors)
    /** 코어 명령 줄 — 호출 순서대로 한 줄에서(등록 뒤 발신 같은 순서가 뒤집히지 않게). 코어 명령은 제어 스레드를 기다리는
     *  동기 호출이라 메인 스레드에서 부르지 않는다(CimsUe 머리말). */
    @OptIn(ExperimentalCoroutinesApi::class)
    internal val ctl = CoroutineScope(SupervisorJob() + Dispatchers.IO.limitedParallelism(1) + errors)
    internal val ue = CimsUe()
    @Volatile internal var account: Account? = null

    /** 코어 명령 하나 — 실패는 로그만(프로토콜 결과는 이벤트로 온다). */
    internal fun cmd(what: String, block: suspend () -> CimsResult<*>) = ctl.launch {
        val r = block()
        if (!r.ok) Log.w(TAG, "$what: ${r.code} ${r.reason}")
    }

    private val _reg = MutableStateFlow<RegState>(RegState.Idle)
    val regState: StateFlow<RegState> = _reg.asStateFlow()

    /** 사용자 피드백(톤+진동) — 서비스가 Context 로 생성해 주입. */
    var feedback: PttFeedback? = null

    /** 그룹별 수신 음량 영속화 — 서비스가 주입. 신규 그룹 기본=최대([GroupVolumeStore.DEFAULT]). */
    var volumeStore: com.cims.ue.ptt.audio.GroupVolumeStore? = null

    /** 참여 채널 영속화 — 서비스가 주입. 프로세스 재시작 후 등록 완료 시 자동 재조인.
     *  ⚠️접근성(PttKeyService) 리바인드의 헤드리스 재기동에선 등록이 이 배선보다 먼저 끝난다 —
     *  배선 시점에 복원을 재시도해야 복원 기회가 증발하지 않는다([maybeRestoreChannels] 가드와 한 쌍). */
    var channelStore: ChannelStore? = null
        set(value) {
            field = value
            if (value != null && regState.value is RegState.Registered) maybeRestoreChannels()
        }

    /** 이어폰(유선/BT) 장치 열거·지정 — 서비스가 주입. */
    var audioRouter: com.cims.ue.ptt.audio.AudioRouter? = null

    /** 그룹콜 중 근접 센서 화면 꺼짐 — [applyProximity]. */
    var proximityLock: com.cims.ue.core.power.ProximityScreenLock? = null

    /** 근접 센서 화면 꺼짐 적용 — 그룹콜 참여 중(활성 세션 하나 이상)이고 **하드웨어 PTT 키 단말**일 때만 잡는다.
     *  측면 키로 발언하며 귀에 대면 얼굴이 화면을 눌러 오조작된다(채널 음량 슬라이더 끌림 실측). 화면 PTT
     *  단말은 화면이 꺼지면 누르고 있던 PTT 버튼 터치가 취소되어 발언이 끊기므로 잡지 않는다. */
    fun applyProximity() {
        val want = HwPtt.present.value && synchronized(lock) { sessionMap.values.any { it.active } }
        if (want) proximityLock?.acquire() else proximityLock?.release()
    }

    /** 라우팅 선택 영속화 — 서비스가 주입(리부팅/재기동 복원). */
    var routePrefs: com.cims.ue.ptt.audio.AudioRoutePrefs? = null

    /** 통화이력 이벤트 훅 — 서비스가 주입(HistoryStore 영속). 컨트롤러 스레드에서 호출되므로 가볍게. */
    var onEvent: ((PttEvent) -> Unit)? = null
    internal fun emit(kind: PttEventKind, groupId: String, peer: String? = null, durationMs: Long = 0) {
        runCatching { onEvent?.invoke(PttEvent(kind, groupId, peer, durationMs)) }
    }

    /** 발언 마이크 핸드오프 훅 — 서비스가 주입(volte 앱에 MIC_YIELD/RESUME 브로드캐스트). */
    var micHandoff: ((Boolean) -> Unit)? = null

    /** 발언 캡처 게이트 현재 상태 — 중복 전환 방지. 캡처 = PTT 발언([floorCapture]) 또는 MCVideo 송출([videoCapture]). */
    private var talkCapture = false
    private var floorCapture = false
    private var videoCapture = false
    private val captureLock = Any()

    /**
     * 발언 캡처 게이트 — 유휴/청취=스피커 전용(마이크 미보유), 발언 시도~종료=전이중(코어 `setCaptureEnabled`, ue_sdk.md §4.5).
     * 마이크를 발언 구간에만 보유해야 OS 동시 캡처 중재가 통화(volte) 앱 캡처를 무음화하지 않는다.
     * volte 협조 핸드오프([micHandoff])와 장치 모드 전환을 한 지점에서 묶는다 — PTT down 에서 floor 요청과 병렬로 시작해
     * 승인 톤이 끝날 때(코어가 마이크를 여는 시점)면 전환이 끝나 있다.
     */
    internal fun setTalkCapture(on: Boolean) {
        applyCapture { floorCapture = on }
        videoPlane.onVoiceTalk(on)          // 무전 발언 동안 영상 호 음성 송신을 멈춘다(mcvideo.md §7 D12)
    }

    /** MCVideo 송출 캡처 — 송출 요청·대기·허가 동안 마이크를 확보한다(허가 뒤 코어가 연다). PTT 발언 게이트와 합쳐 건다. */
    internal fun setVideoCapture(on: Boolean) = applyCapture { videoCapture = on }

    private fun applyCapture(change: () -> Unit) {
        val on = synchronized(captureLock) {
            change()
            val want = floorCapture || videoCapture
            if (talkCapture == want) return
            talkCapture = want
            want
        }
        runCatching { micHandoff?.invoke(on) }
        cmd("setCaptureEnabled($on)") { ue.setCaptureEnabled(on) }
    }

    /** MCData SDS 수신(시그널링 평면·media plane·FD·disposition 통지) — 코어가 해석한 그대로. **유실되지 않는다**
     *  (소비자는 서비스 하나 — MessageStore 영속·통지). */
    val incomingSds: Flow<SdsMessage> get() = ue.sds

    /** MCData 가 아닌 문자(text/plain — 구버전 앱 호환) — 서비스가 발신자 스레드로 저장. **유실되지 않는다**. */
    internal val _incomingText = Channel<com.cims.ue.core.sip.ImMessage>(Channel.UNLIMITED)
    val incomingText: Flow<com.cims.ue.core.sip.ImMessage> = _incomingText.receiveAsFlow()

    /** MSRP 발신 진행 — (msgId, 송신 바이트, 전체 바이트). 코어가 진행률을 내지 않아 지금은 방출하지 않는다(완료만 [sendResult]). */
    data class SendProgress(val msgId: String, val sent: Int, val total: Int)
    private val _sendProgress = MutableSharedFlow<SendProgress>(extraBufferCapacity = 64)
    val sendProgress: SharedFlow<SendProgress> = _sendProgress.asSharedFlow()

    /** 문자 발신 결과 — (msgId, 성공 여부). 시그널링 평면은 MESSAGE 최종 응답(2xx), media plane 은 저장소 수신(MSRP 200).
     *  서비스가 MessageStore 상태(SENT/FAILED) 반영. */
    internal val _sendResult = MutableSharedFlow<Pair<String, Boolean>>(extraBufferCapacity = 64)
    val sendResult: SharedFlow<Pair<String, Boolean>> = _sendResult.asSharedFlow()

    // ── 그룹별 세션 ──

    internal inner class Session(val groupId: String) {
        var callId: Int = -1
        var active: Boolean = false
        var role: ChannelRole = ChannelRole.NONE
        var floorState: FloorState = FloorState.IDLE
        var speaker: Speaker? = null
        var participants: MutableMap<String, String> = mutableMapOf(bareId(mcpttId) to "connected")
        var audible: Boolean = true
        // 영속 저장된 그룹별 음량으로 시작 — 저장값 없는(신규) 그룹은 최대
        var volume: Float = volumeStore?.get(groupId) ?: 1f
        var emergency: Boolean = false
        var emergencyMine: Boolean = false
        var mySpeakStartMs: Long = 0          // 이력용 — 내 발언 시작(elapsedRealtime)
        var otherSpeaker: String? = null      // 이력용 — 수신 중 발언자
        var otherSpeakStartMs: Long = 0
        // Floor Taken 의 Permission(§8.2.3.7)=0 이면 이 세션에서는 발언 요청이 불가하다
        // (일제 통화·ambient 청취 leg). 눌러도 Deny 만 받으므로 버튼을 미리 막는다.
        var canRequestFloor: Boolean = true
        var speakDeadlineMs: Long = 0         // Granted Duration(T2) 마감(elapsedRealtime), 0=무제한
        var talkWarn: Job? = null             // 마감 임박 알림(자체 종료는 코어가 한다 — FloorEvent TALK_LIMIT)
        var queuePosition: Int? = null        // Queue Position Info — 대기 중일 때만
        var queueCancelByMe: Boolean = false  // 내가 대기 취소를 보냈다 — 취소 통지에 거부음을 내지 않는다
        var talkers: List<Speaker> = emptyList()   // 동시 발언 화자 전체(§8.2.3.17~18)
        var talkerSsrc: Map<String, Long> = emptyMap()  // 화자 → RTP SSRC (SSRC 별 재생용, U10)
        var floorIndicator: Int = 0           // 마지막 수신 Floor Indicator (G/I 비트 표시)
        var privatePeer: Boolean = false      // 1:1 private call — groupId=상대 번호
        var fullDuplex: Boolean = false       // 전이중 1:1 — floor 없음, PTT 가 로컬 마이크 게이트(setMuted)

        /** 이 세션이 동시 발언을 허용하는가 — 서버 Floor Indicator 의 I-bit(multi-talker)/
         *  G-bit(dual floor) 로 판정한다(TS 24.380 §8.2.3.15). multi 정책은 모든 floor 메시지에
         *  I-bit 가 실리고, dual 은 실제로 2명이 말할 때 G-bit 가 실린다. 참이면 남이 발언 중
         *  이어도 요청을 보내고 정원 판단은 서버에 맡긴다. */
        fun multiTalkerSession(): Boolean =
            (floorIndicator and (FloorIndicator.MULTI_TALKER or FloorIndicator.DUAL_FLOOR)) != 0

        fun toState() = GroupCallState(groupId, callId, active, role, floorState, speaker, participants.toMap(),
            audible, emergency, emergencyMine, volume, canRequestFloor, speakDeadlineMs,
            // 대기 위치는 QUEUED 상태에서만 의미가 있다 — 상태로 파생해 지난 값이 새지 않게 한다.
            queuePosition.takeIf { floorState == FloorState.QUEUED }, talkers, floorIndicator,
            privatePeer, fullDuplex)
        fun close() { talkWarn?.cancel() }
    }

    internal val lock = Any()
    internal val sessionMap = LinkedHashMap<String, Session>()   // groupId → Session (참여 순서 유지)

    // 구독 상태는 **서버 확인 기반**으로 관리한다 — SUBSCRIBE 를 보냈다는 사실만으로 "구독 중"
    // 으로 취급하면, 서버가 구독을 잃고(예: CSP 재기동으로 in-memory 구독 소멸) 단말이 등록
    // 끊김을 관측하지 못한 경우 멱등 가드가 재발행을 영구히 막아 로스터·편성 push 가 앱 재시작
    // 전까지 얼어붙는다(실측). affiliation 의 [affiliated] 와 같은 원칙이다.
    //   확인 신호 = **NOTIFY 도착**(코어 onRoster·onMessage). 나아가 확인 상태에 **재확인 주기**를 둔다 — 구독 소멸을
    //   앱이 감지할 수단이 아직 없다(ue_sdk.md §11 «구독 종료 사유»). [SUB_REASSERT_MS] 마다 SUBSCRIBE 를 다시
    //   던지면 살아 있는 구독은 엔진이 in-dialog 갱신으로 흡수하고, 죽은 구독은 새로 만들어진다.
    internal val confirmedRosters = mutableMapOf<String, Long>() // groupId → 마지막 확인/재확인 시각(ms)
    internal val pendingRosters = mutableMapOf<String, Long>()   // groupId → 최초 SUBSCRIBE 발행 시각(ms)
    // xcap-diff 구독은 **문서 축마다 하나**다 — 서버 PSI 가 축별로 다르고(sip:gms_psi=편성,
    //   sip:cms_psi=사용자 프로파일·시스템 설정), CSP 는 SUBSCRIBE 의 Request-URI 로 축을 가른다.
    internal val xcapConfirmedAt = mutableMapOf<String, Long>()  // kind → 마지막 확인/재확인 시각(ms)
    internal val xcapPendingAt = mutableMapOf<String, Long>()    // kind → 최초 SUBSCRIBE 발행 시각(ms)
    internal val rosterMap = mutableMapOf<String, Map<String, String>>()  // groupId → 접속 인원(미조인 포함)

    internal val _sessions = MutableStateFlow<List<GroupCallState>>(emptyList())
    /** 참여 중인 그룹 세션들(참여 순). */
    val sessions: StateFlow<List<GroupCallState>> = _sessions.asStateFlow()

    private val _listenPolicy = MutableStateFlow(ListenPolicy.ALL)
    /** 듣기 정책 — 주채널만/전체. */
    val listenPolicy: StateFlow<ListenPolicy> = _listenPolicy.asStateFlow()

    private val _audioRoute = MutableStateFlow(AUDIO_ROUTE_SPEAKER)
    /** 오디오 출력 라우팅(전역) — 스피커폰(기본)/수화기/이어폰([AUDIO_ROUTE_HEADSET]). */
    val audioRoute: StateFlow<Int> = _audioRoute.asStateFlow()

    private val _headsetId = MutableStateFlow(-1)
    /** 이어폰 라우팅일 때 선택 장치 id ([com.cims.ue.ptt.audio.AudioRouter.Headset.id]). */
    val headsetId: StateFlow<Int> = _headsetId.asStateFlow()

    private val _spkGain = MutableStateFlow(com.cims.ue.ptt.audio.AudioRoutePrefs.DEFAULT_SPK_GAIN)
    /** 무전 스피커 출력 게인(장치단 ×1.0~×3.0) — 설정 화면 슬라이더. */
    val spkGain: StateFlow<Float> = _spkGain.asStateFlow()

    private val _micGain = MutableStateFlow(com.cims.ue.ptt.audio.AudioRoutePrefs.DEFAULT_MIC_GAIN)
    /** 무전 마이크 송신 게인(장치단 ×1.0~×3.0). */
    val micGain: StateFlow<Float> = _micGain.asStateFlow()

    // ── 주채널 파생 상태 (발언자 카드·PTT 버튼용) ──

    private val _floorState = MutableStateFlow(FloorState.IDLE)
    /** 주채널 floor 상태. */
    val floorState: StateFlow<FloorState> = _floorState.asStateFlow()

    private val _speaker = MutableStateFlow<Speaker?>(null)
    /** 현재 들리는 발언자(주채널 우선, 없으면 가청 그룹 중 첫 발언자 — groupId 로 구분). */
    val speaker: StateFlow<Speaker?> = _speaker.asStateFlow()

    internal val _groups = MutableStateFlow<List<GroupSummary>>(emptyList())
    val groups: StateFlow<List<GroupSummary>> = _groups.asStateFlow()

    internal val _groupDocs = MutableStateFlow<Map<String, GroupDoc>>(emptyMap())
    /** 그룹 문서(TS 24.481) 캐시 — groupId → 멤버(이름·번호·역할·우선순위)·그룹 속성. [loadGroupDetail] 로 적재. */
    val groupDocs: StateFlow<Map<String, GroupDoc>> = _groupDocs.asStateFlow()

    internal val _selectedGroup = MutableStateFlow<String?>(null)
    /** 그룹 목록에서 선택된 그룹(참여 전 하이라이트·affiliation 대상). */
    val selectedGroup: StateFlow<String?> = _selectedGroup.asStateFlow()

    /** 활성 긴급경보 — 수신(fan-out)+내 발신. 취소 MESSAGE 수신/발신 시 해제. */
    internal val _alerts = MutableStateFlow<List<ActiveAlert>>(emptyList())
    val alerts: StateFlow<List<ActiveAlert>> = _alerts.asStateFlow()

    internal val _affiliated = MutableStateFlow<Set<String>>(emptySet())
    /** 서버가 2xx 로 확인한 affiliation 그룹(응답 기반 — PUBLISH 송신만으로는 포함하지 않음). */
    val affiliated: StateFlow<Set<String>> = _affiliated.asStateFlow()

    internal val _channelRosters = MutableStateFlow<Map<String, Map<String, String>>>(emptyMap())
    /** 채널별 접속 인원 — groupId → (참가자ID → status). conference 구독(RFC 4575) NOTIFY 로 갱신되며
     *  **미조인 채널도 포함**한다(제휴 채널 전체를 구독하므로). 참여 중인 채널의 로스터는
     *  [sessions] 의 participants 와 같은 값이다. */
    val channelRosters: StateFlow<Map<String, Map<String, String>>> = _channelRosters.asStateFlow()

    // ── affiliation 목표 집합(TS 24.379 §9) — 편성 채널 전체를 서버 확인 기반으로 유지(정책은 앱, PUBLISH·ETag 는 코어) ──
    internal val affPending = java.util.concurrent.ConcurrentHashMap<Long, Pair<String, Boolean>>()
    internal val affExpireAt = java.util.concurrent.ConcurrentHashMap<String, Long>()   // 확정 만료(elapsedRealtime)
    internal val affAttempts = java.util.concurrent.ConcurrentHashMap<String, Int>()
    internal val affBackoffUntil = java.util.concurrent.ConcurrentHashMap<String, Long>()  // 백오프 대기 종료 시각
    /** 시그널링 평면·media plane SDS 의 코어 token → msgId — 최종 응답을 [sendResult] 로 대응. */
    internal val sdsPending = java.util.concurrent.ConcurrentHashMap<Long, String>()
    /** 짝(token)을 알기 전에 온 최종 응답 — 코어 token 은 요청을 보낸 뒤에야 안다. */
    internal val earlyResults = java.util.concurrent.ConcurrentHashMap<Long, com.cims.ue.sdk.RequestResult>()
    /** 403(등록 소실) 대응 재-REGISTER 스로틀 — 연속 실패마다 재등록하지 않도록. */
    @Volatile internal var affReRegisterAt = 0L

    internal val _status = MutableStateFlow("대기")
    val status: StateFlow<String> = _status.asStateFlow()

    // ── CSC(설정 평면) — IdMS 토큰·GMS·CMS·FD (코어 CscClient) ──
    internal val csc: CscClient? = cscConfig?.let {
        CscClient(CscEndpoint(host = it.host, port = it.port, caPem = CimsTrustStore.CA_BUNDLE, verifyServer = true))
    }
    @Volatile internal var token: String? = null
    /** CSC 토큰 보유 여부 — 서비스의 SSO 주입 중복 방지용(주입은 [setAccessToken]). */
    val hasAccessToken: Boolean get() = token != null

    /**
     * SSO 토큰 갱신 훅 — 서버가 토큰을 거절(401)하면 [withToken] 이 이것으로 새 토큰을 받아 1회 재시도한다.
     * 호스트(PttService)가 AccountManager 갱신(`CimsAccounts.renewToken`)을 꽂는다. null 이면(수동 CSC
     * 로그인 모드) 재시도 없이 원래 실패를 낸다. 블로킹 — IO 에서 부른다.
     */
    @Volatile var tokenRefresher: ((stale: String) -> String?)? = null
    private val renewLock = Any()

    /** 갱신 직렬화 — 동시에 401 을 받은 호출들(로그인 직후 XCAP 여러 건)이 refresh 를 겹쳐 돌리지 않게 한다. */
    internal fun renewToken(stale: String): String? = synchronized(renewLock) {
        val cur = token
        if (cur != null && cur != stale) return cur          // 다른 호출이 이미 갱신했다
        val fresh = tokenRefresher?.invoke(stale) ?: return null
        token = fresh
        Log.i(TAG, "CSC 토큰 갱신 반영")
        fresh
    }

    /** 토큰이 필요한 CSC 호출 — 401 이면 갱신 후 1회 재시도. 토큰이 없으면 실패(-1). */
    internal suspend fun <T> withToken(block: suspend (CscClient, String) -> CimsResult<T>): CimsResult<T> {
        val c = csc ?: return CimsResult.fail(-1, "CSC 미설정")
        val t = token ?: return CimsResult.fail(-1, "토큰 없음")
        val r = block(c, t)
        if (r.ok || r.code != 401) return r
        val fresh = kotlinx.coroutines.withContext(Dispatchers.IO) { renewToken(t) } ?: return r
        return block(c, fresh)
    }

    @Volatile internal var pttHeld = false

    // ── 평면(ue_sdk.md §5.3 — 세션 + 평면) — 상태는 이 세션이 들고, 평면은 그 위의 동작을 나눠 든다 ──
    internal val groupsPlane = GroupPlane(this)
    internal val floorPlane = FloorPlane(this)
    internal val messaging = MessagingPlane(this)
    internal val emergencyPlane = EmergencyPlane(this)
    internal val videoPlane = VideoPlane(this, context)

    init {
        // 번호 로컬 표기(+82→0…)용 홈 국가코드 — 프로비저닝 countryCode 우선, 내 msisdn 유도 폴백
        homeCountryCode = sipConfig.countryCode.ifBlank { countryCodeOf(mcpttId) ?: "" }.ifBlank { null }

        // 코어 이벤트 — 구독을 먼저 걸고(UNDISPATCHED) 등록한다. SharedFlow 는 늦게 붙은 수집자에게 지난 이벤트를 주지 않는다.
        fun <T> on(flow: Flow<T>, f: suspend (T) -> Unit) =
            scope.launch(start = CoroutineStart.UNDISPATCHED) { flow.collect { runCatching { f(it) }.onFailure { e -> Log.w(TAG, "event", e) } } }
        on(ue.log) { Log.println(pjPriority(it.level), "PJ", it.message.trimEnd()) }
        on(ue.regState) { onReg(it) }
        on(ue.incomingCall) { if (VideoPlane.isVideo(it)) videoPlane.onIncomingCall(it) else groupsPlane.onIncomingCall(it) }
        on(ue.callState) { if (VideoPlane.isVideo(it)) videoPlane.onCallState(it) else onCallState(it) }
        on(ue.floor) { floorPlane.onFloorEvent(it) }
        on(ue.transmission) { videoPlane.onTransmission(it) }
        on(ue.reception) { videoPlane.onReception(it) }
        on(ue.roster) { groupsPlane.onRoster(it) }
        on(ue.condition) { emergencyPlane.onCondition(it) }
        on(ue.emergencyAlert) { emergencyPlane.onAlert(it) }
        on(ue.message) { groupsPlane.onSipMessage(it) }
        on(ue.requestResult) { onRequestResult(it) }

        // 주기 갱신 — TTL 절반 경과 그룹 재-PUBLISH(1h 만료 방치로 fan-out 이 조용히 죽는 것 방지)
        scope.launch {
            while (true) {
                delay(60_000)
                if (regState.value is RegState.Registered) {
                    groupsPlane.affiliateAll()
                    groupsPlane.syncRosterSubs()   // 편성 변경으로 채널이 늘/줄었으면 구독도 따라간다
                }
            }
        }
    }

    // ── 등록 ──

    /** 엔진 기동 → 계정 → REGISTER. 유휴 기본은 스피커 전용(마이크 미보유) — 발언([setTalkCapture])에서만 전이중. */
    fun register() = ctl.launch {
        if (account != null) return@launch
        _reg.value = RegState.Registering
        val started = ue.start(
            EngineConfig(
                userAgent = userAgent,
                logLevel = 4,
                tlsCaPem = CimsTrustStore.CA_BUNDLE,              // APK 동봉 앵커(sip_tls_signaling.md §8.5)
                tlsVerifyServer = true,
                udpNoTcpSwitch = sipConfig.udpNoTcpSwitch,
                // «삑 후 말하기» — 승인 톤이 그룹으로 나가지 않게 코어가 톤 길이만큼 마이크 개방을 미룬다(android_ue_client.md)
                grantMicDelayMs = PttFeedback.GRANT_TONE_MS.toInt(),
            ),
            context,
        )
        if (!started.ok) { _reg.value = RegState.Failed("${started.code} ${started.reason}"); return@launch }
        ue.setCaptureEnabled(false)
        // 참여 기능 PSI = UE initial configuration(TS 24.484 §7.2.2.1 10)·14)) — 로그인 전 문서(토큰 없음). 못 받으면 PSI 없이
        //   (경보 = 그룹 URI, disposition 통지 = 원 발신자 직행 — 코어 전환기 경로).
        val ueInit = instanceId?.takeIf { it.isNotEmpty() }?.let { id -> csc?.fetchUeInitConfig(id)?.getOrNull() }
        videoPlane.setServer(ueInit?.mcvideoServerUri.orEmpty())
        val acc = ue.addAccount(accountConfig(ueInit)).getOrNull()
            ?: run { _reg.value = RegState.Failed("addAccount"); return@launch }
        account = acc
        applyAudioRouteNow()
        val r = acc.register()
        if (!r.ok) _reg.value = RegState.Failed("${r.code} ${r.reason}")
    }

    /** 등록 해제(명시 종료·설정 변경 재시작). */
    fun unregister() = ctl.launch { account?.unregister() }

    /** 기존 설정(SipAccountConfig) → 코어 계정 — 매핑 규칙(Digest·SRTP·sec-agree)은 코어(account_map.cpp)가 한다. */
    private fun accountConfig(ueInit: UeInitConfigDoc? = null): AccountConfig {
        val c = sipConfig
        return AccountConfig(
            serverHost = c.serverHost, serverPort = c.serverPort,
            transport = when (c.transport) {
                SipAccountConfig.Transport.TCP -> Transport.TCP
                SipAccountConfig.Transport.TLS -> Transport.TLS
                else -> Transport.UDP
            },
            domain = c.domain, msisdn = c.msisdn, imsi = c.imsi, authId = c.authId,
            displayName = c.displayName, ha1 = c.sipHa1, password = c.password,
            authScheme = if (c.authScheme.equals("aka", ignoreCase = true)) AuthScheme.AKA else AuthScheme.DIGEST,
            akaK = c.akaK, akaOpc = c.akaOpc, akaAmf = c.akaAmf,
            secMechanisms = c.secMechanisms,
            mediaSecurity = when (c.mediaSecurity.lowercase()) {
                "required" -> MediaSecurity.REQUIRED
                "optional" -> MediaSecurity.OPTIONAL
                else -> MediaSecurity.OFF
            },
            expiresSec = c.expiresSec,
            mcpttId = mcpttId,
            autoAnswerMcptt = true,                                // 그룹콜·사설콜 착신 자동 수락(ptt_ue.md §12.3)
            instanceId = instanceId.orEmpty(),
            maxSdsCplaneBytes = c.maxPayloadSdsCplaneBytes,        // 넘는 그룹 SDS 는 media plane(TS 24.282 §9.2.3)
            mcdataMsrp = true,                                     // 서버발 MSRP 배포 수신(REGISTER Contact ICSI mcdata.sds)
            mcpttServerUri = ueInit?.mcpttServerUri.orEmpty(),     // 경보 Request-URI(TS 24.379 §12.1.1.1 8))
            mcdataServerUri = ueInit?.mcdataServerUri.orEmpty(),   // disposition 통지 Request-URI(TS 24.282 §12.2.1.1)
            // MCVideo(TS 24.281) — 서버가 PSI 를 내줄 때만 등록 태그를 싣는다(영상 = MCVideo 호, MCPTT 호는 음성만 — mcvideo.md §7 D9).
            //   멤버 초대(prearranged)는 자동 수락 — 합류일 뿐이고 영상 보기는 [받기](manual 수신)가 따로 정한다.
            mcvideoEnabled = !ueInit?.mcvideoServerUri.isNullOrEmpty(),
            mcvideoServerUri = ueInit?.mcvideoServerUri.orEmpty(),
            autoAnswerMcvideo = true,
        )
    }

    private fun onReg(r: RegInfo) {
        val was = _reg.value
        val now = when (r.state) {
            SdkRegState.REGISTERED -> RegState.Registered(r.code)
            SdkRegState.REGISTERING -> RegState.Registering
            SdkRegState.UNREGISTERED -> if (r.code in 200..299) RegState.Unregistered else RegState.Idle
            SdkRegState.FAILED -> RegState.Failed("${r.code} ${r.reason}")
        }
        _reg.value = now
        if (now is RegState.Registered) {
            if (was is RegState.Registered) return
            // 등록 완료 — 편성 채널 전체 affiliation(CSP 는 affiliation 된 멤버에게만 fan-out), 로스터·문서 구독, 채널 복원
            groupsPlane.affiliateAll()
            groupsPlane.syncRosterSubs()
            groupsPlane.subscribeXcap(XCAP_GMS, true)
            groupsPlane.subscribeXcap(XCAP_CMS, true)
            maybeRestoreChannels()
        } else if (was is RegState.Registered) {
            // 등록이 끊기면 서버측 구독도 사라진다 — 확인 상태를 비워 재등록 시 다시 걸리게 한다.
            synchronized(lock) { groupsPlane.clearSubStateLocked() }
            groupsPlane.publishRosters()
        }
    }

    // ── 참여 채널 자동 복원 ──

    private var channelsRestored = false

    /** 프로세스 재시작(강제종료·재설치·리부팅) 후 참여 채널 자동 재조인 — 등록 완료 시 1회, **진행 중 세션에만**(late entry).
     *  prearranged INVITE 는 세션이 없으면 새로 개시해 affiliate 멤버 전원에게 fan-out 하므로(TS 24.379 §10.1.1) 진행 여부를
     *  먼저 본다 — 판단 근거는 그룹 conference 구독의 NOTIFY(확립 leg 만 싣는다, ptt_flows.md). 명단이 비었으면 복원하지 않고
     *  참여 의도는 그대로 둔다(누가 세션을 열면 fan-out 착신으로 자동 합류한다). NOTIFY 가 [RESTORE_ROSTER_WAIT_MS] 안에
     *  오지 않은 채널도 복원하지 않는다 — 모르는 채로 새 세션을 열지 않는다.
     *  재로그인 경로는 affiliation 후 서버 fan-out INVITE 가 먼저 올 수 있어 [RESTORE_YIELD_MS] 양보하고,
     *  그 사이 생긴 세션은 존중한다([joinGroupCall] 이 중복 참여 무시). */
    internal fun maybeRestoreChannels() {
        if (channelsRestored) return
        // 스토어 배선 전(접근성 헤드리스 재기동)이면 복원 기회를 소모하지 않는다 —
        // 배선 시점에 channelStore setter 가 재호출(플래그는 실제 복원 착수에서만 소모).
        val st = channelStore ?: return
        channelsRestored = true
        val want = st.joined
        if (want.isEmpty()) return
        val primary = st.primary
        scope.launch {
            delay(RESTORE_YIELD_MS)
            val deadline = SystemClock.elapsedRealtime() + RESTORE_ROSTER_WAIT_MS
            while (SystemClock.elapsedRealtime() < deadline && synchronized(lock) { want.any { it !in confirmedRosters } })
                delay(200)
            val ordered = if (primary != null) listOf(primary) + (want - primary) else want
            for (g in ordered) {
                val (joined, known, ongoing) = synchronized(lock) {
                    Triple(sessionMap.containsKey(g), g in confirmedRosters, rosterMap[g]?.isNotEmpty() == true)
                }
                if (joined) continue
                if (!known) { Log.i(TAG, "채널 복원 보류 $g — 로스터 미확인(진행 여부 모름)"); continue }
                if (!ongoing) { Log.i(TAG, "채널 복원 생략 $g — 진행 중 세션 없음"); continue }
                _status.value = "채널 자동 복원: $g"
                groupsPlane.joinGroupCall(g, takePrimary = primary == null || g == primary)
                delay(300)
            }
            primary?.let { p -> if (synchronized(lock) { sessionMap.containsKey(p) }) setPrimary(p) }
        }
    }

    // ── 호 상태 ──

    private fun onCallState(c: CallInfo) {
        if (!c.isMcptt) return
        when (c.state) {
            CallState.OUTGOING -> bindCall(c.groupId, c.callId)
            CallState.ACTIVE -> {
                bindCall(c.groupId, c.callId, active = true)
                enterCallAudio()
                applyListenPolicy()
                applyProximity()                              // 귀에 대면 화면 꺼짐(하드웨어 PTT 단말)
            }
            CallState.DISCONNECTED -> {
                emergencyPlane.handleEmergencyDenied(c.callId, c.lastCode)   // 긴급 개시 403 → normal 재발신 폴백
                onCallEnded(c.callId)
            }
            else -> Unit
        }
    }

    /** 캡처 카메라 전환(전면↔후면) — 송출 중인 영상 호의 카메라를 바꾸고 이후 송출의 기본 카메라로도 쓴다. */
    fun switchCamera(callId: Int) = cmd("switchCamera") { ue.call(callId).switchCamera() }

    /** 수신 영상을 그릴 Surface(null = 해제) — 주채널 화면의 영상 칸. 코어 창은 하나라 영상 호도 하나만 둔다([VideoPlane]). */
    fun setVideoSurface(surface: android.view.Surface?) = cmd("setVideoSurface") { ue.setVideoSurface(surface) }

    /** 내 카메라 미리보기 Surface(null = 해제) — 카메라는 코어 캡처가 연 것에 출력만 더한다(카메라 2중 개방 없음). */
    fun setPreviewSurface(surface: android.view.Surface?) = ue.setPreviewSurface(surface)

    /** 코어 요청의 최종 응답 — affiliation PUBLISH 는 그룹 평면, SDS(MESSAGE·MSRP)는 메시징 평면. 짝(token)을 아직 모르면 잠시 둔다. */
    private fun onRequestResult(r: com.cims.ue.sdk.RequestResult) {
        if (groupsPlane.onAffiliationResult(r)) return
        if (messaging.onSendResult(r)) return
        if (emergencyPlane.onAlertResult(r)) return
        earlyResults[r.token] = r
        if (earlyResults.size > 256) earlyResults.keys.take(64).forEach { earlyResults.remove(it) }
    }

    /** 방금 받은 코어 token 의 결과가 먼저 와 있었으면 꺼낸다. */
    internal fun takeEarly(token: Long): com.cims.ue.sdk.RequestResult? = earlyResults.remove(token)

    // ── 공개 명령 — 평면에 넘긴다(화면·서비스가 보는 계약은 이 클래스 하나) ──

    /** 키업 그룹콜 참여(발신). 이미 참여 중이면 무시. 첫 세션은 주채널. [members] = 애드혹 참가자 tel: URI.
     *  [emergency] = 긴급 그룹콜 개시, [broadcast] = 일제 통화 개시(TS 24.379 §4.12 — 코어가 B-bit·해제를 한다). */
    fun joinGroupCall(groupId: String, members: List<String> = emptyList(), emergency: Boolean = false,
                      broadcast: Boolean = false) = groupsPlane.joinGroupCall(groupId, members, emergency, broadcast)
    fun startAdhocCall(members: List<String>) = groupsPlane.startAdhocCall(members)
    fun startPrivateCall(peer: String, fullDuplex: Boolean = false, emergency: Boolean = false) =
        groupsPlane.startPrivateCall(peer, fullDuplex, emergency)
    fun startEmergencyPrivateCall(peer: String, fullDuplex: Boolean = false) =
        emergencyPlane.startEmergencyPrivateCall(peer, fullDuplex)
    fun leaveGroup(groupId: String) { groupsPlane.leaveGroup(groupId); videoPlane.requestSync() }   // 채널을 나가면 영상 호도(D10)
    fun login(userName: String, password: String) = groupsPlane.login(userName, password)
    fun setAccessToken(accessToken: String) = groupsPlane.setAccessToken(accessToken)
    fun loadGroups() = groupsPlane.loadGroups()
    fun loadGroupDetail(groupId: String) = groupsPlane.loadGroupDetail(groupId)
    fun loadUserProfile() = groupsPlane.loadUserProfile()
    fun loadServiceConfig() = groupsPlane.loadServiceConfig()
    fun selectGroup(groupId: String) = groupsPlane.selectGroup(groupId)
    fun affiliate(groupId: String, on: Boolean = true) = groupsPlane.affiliate(groupId, on)
    val userProfile: StateFlow<UserProfile?> get() = groupsPlane.userProfile
    val serviceConfig: StateFlow<ServiceConfig?> get() = groupsPlane.serviceConfig

    fun isGroupId(id: String): Boolean = _groups.value.any { bareId(it.uri) == id }
    fun sendSds(peer: String, text: String, msgId: String = newMessageId()): String = messaging.sendSds(peer, text, msgId)
    suspend fun sendAttachment(peer: String, data: ByteArray, fileName: String, mime: String): FdSent? =
        messaging.sendAttachment(peer, data, fileName, mime)
    suspend fun downloadAttachment(url: String): ByteArray? = messaging.downloadAttachment(url)
    fun sendSdsNotification(peerId: String, convId: String, msgId: String, notifType: Int, groupUri: String = "") =
        messaging.sendSdsNotification(peerId, convId, msgId, notifType, groupUri)

    fun startEmergency() = emergencyPlane.startEmergency()
    fun cancelEmergency() = emergencyPlane.cancelEmergency()
    fun cancelAlert(groupId: String) = emergencyPlane.cancelAlert(groupId)
    fun dismissAlert(groupId: String, userId: String) = emergencyPlane.dismissAlert(groupId, userId)
    fun dismissEmergency(groupId: String) = emergencyPlane.dismissEmergency(groupId)

    fun pttDown() = floorPlane.pttDown()
    fun pttUp() = floorPlane.pttUp()

    // ── MCVideo(TS 24.281·24.581) — 영상은 그룹의 MCVideo 호, [PTT] 는 MCPTT 호 그대로(mcvideo.md §7 D6) ──
    /** MCVideo 그룹 호들(한 번에 하나). */
    val videoCalls: StateFlow<List<VideoCallState>> get() = videoPlane.state
    /** 서버가 MCVideo 를 낸다(ue-init-config PSI) — 그룹 문서의 MCVideo 몫과 함께 영상 채널을 정한다. 영상 호 합류·나가기는 주채널을
     *  따라 평면이 한다(D10 — 명령 없음). */
    val mcvideoAvailable: StateFlow<Boolean> get() = videoPlane.available
    /** 영상 채널(사용자가 고른 주채널) — 무전 세션이 없을 때도 영상 호가 이어진다(D10). */
    val videoChannel: String? get() = videoPlane.channelGroup
    /** [영상 보내기] 토글 — 송출 요청/끝내기. */
    fun setVideoTransmit(groupId: String, on: Boolean) = videoPlane.setTransmit(groupId, on)
    /** «새 영상» [받기] / [그만 보기]. */
    fun acceptVideo(groupId: String, transmitterId: String) = videoPlane.accept(groupId, transmitterId)
    fun stopVideo(groupId: String, transmitterId: String) = videoPlane.stopViewing(groupId, transmitterId)
    /** 영상 보내는 중 무전 마이크 정책(D12) — 영속(ptt_video/mic_policy). */
    val videoMicPolicy: StateFlow<VideoMicPolicy> get() = videoPlane.micPolicy
    fun setVideoMicPolicy(p: VideoMicPolicy) = videoPlane.setMicPolicy(p)

    /** 사용자 MCPTT 프로파일(TS 24.484) 요약 — SOS 대상 결정 모드·전용 긴급그룹·긴급 사설콜·개시 인가(코어 UserProfileDoc 의 투영). */
    data class UserProfile(
        val emergencyGroupMode: String,   // DedicatedGroup | UseCurrentlySelectedGroup
        val emergencyGroupId: String?,    // 전용 긴급그룹 (bare id, DedicatedGroup 모드 대상)
        val allowEmergencyCall: Boolean,
        val allowEmergencyAlert: Boolean,
        val allowAdhocCall: Boolean,
        val allowPrivateCall: Boolean,            // allow-private-call (1:1 개시 인가, §8.3.2.7)
        val maxAffiliations: Int,                 // OnNetwork/MaxAffiliationsN2 (0=미지정)
        val allowEmergencyPrivateCall: Boolean,   // allow-emergency-private-call (긴급 1:1 개시 인가)
        val privateEmergencyMode: String,         // MCPTTPrivateRecipient: LocallyDetermined | UsePreConfigured
        val emergencyPrivateRecipient: String?,   // 사전 지정 긴급 수신자 (bare id, UsePreConfigured 모드 대상)
    )

    /** 시스템 서비스 설정(TS 24.484 §8.4 service-config) — 시스템 전역 문서. 인가 요소는 없다(인가 = [UserProfile]
     *  ruleset·그룹 문서). 단말이 쓰는 값은 on-network Resource-Priority(TS 24.379 §6.2.8.1.15, 코어 해석). */
    data class ServiceConfig(
        val rpEmergency: String?,          // "<namespace>.<priority>" — null = 미기재
        val rpImminentPeril: String?,
        val rpNormal: String?,
    )

    /** FD 전송 결과 — 로컬 이력 저장용. */
    data class FdSent(val msgId: String, val url: String, val size: Long)

    // ── 세션 헬퍼 ──

    internal fun sessionByCall(callId: Int): Session? =
        synchronized(lock) { sessionMap.values.firstOrNull { it.callId == callId } }

    internal fun bindCall(groupId: String, callId: Int, active: Boolean = false) {
        synchronized(lock) {
            val s = sessionMap[groupId]
                // 방어: 키 불일치(URI 표기 차이) 시 private 세션을 번호 동치로 매칭
                ?: sessionMap.values.firstOrNull {
                    it.privatePeer && bareId(it.groupId).trimStart('+') == bareId(groupId).trimStart('+')
                }
                ?: run { Log.w(TAG, "bindCall miss: key=$groupId call=$callId"); return }
            s.callId = callId
            if (active) s.active = true
        }
        publish()
    }

    private fun onCallEnded(callId: Int) {
        val gid = synchronized(lock) {
            val s = sessionMap.values.firstOrNull { it.callId == callId } ?: return
            sessionMap.remove(s.groupId)
            s.close()
            // 주채널이 사라지면 남은 첫 세션을 주채널로 승격 — 사용자가 고른 주채널이 남아 있으면(세션만 T4 등으로 끝났다) 승격하지
            //   않는다: 다른 그룹 세션은 듣기만 하고 선택 그룹은 그대로다(TS 22.179 그룹 스캐닝, autoJoinGroupCall 과 같은 규칙).
            //   사용자가 나간 경우는 leaveGroup 이 저장값을 먼저 지운다.
            if (s.role == ChannelRole.PRIMARY && channelStore?.primary == null)
                sessionMap.values.firstOrNull()?.role = ChannelRole.PRIMARY
            s.groupId
        }
        leaveCallAudioIfIdle()
        applyProximity()
        groupsPlane.onSessionLeft(gid)
        _status.value = "[$gid] 그룹콜 종료"
        emit(PttEventKind.LEAVE, gid)
        publish()
    }

    /** 통화 오디오 진입 — VoIP 오디오 모드(라우팅·음량 전제) + 무전 체감 음량 보강 + 통화별 라우팅 재적용. MCPTT·MCVideo 호 성립 때. */
    internal fun enterCallAudio() {
        audioRouter?.setInCall(true)
        applyDeviceLevels(_spkGain.value, _micGain.value)
        applyAudioRoute()
    }

    /** 활성 통화(무전 세션·영상 호)가 모두 끝나면 VoIP 오디오 모드 해제(MODE_NORMAL 복원) + 장치 음량 원복. */
    internal fun leaveCallAudioIfIdle() {
        if (synchronized(lock) { sessionMap.values.any { it.active } } || videoPlane.state.value.any { it.active }) return
        audioRouter?.setInCall(false)
        applyDeviceLevels(1f, 1f)
    }

    /** 세션 스냅샷 발행 + 주채널 파생 상태(floor/speaker) 갱신. */
    internal fun publish() {
        val list = synchronized(lock) { sessionMap.values.map { it.toState() } }
        _sessions.value = list
        val primary = list.firstOrNull { it.role == ChannelRole.PRIMARY }
        _floorState.value = primary?.floorState ?: FloorState.IDLE
        _speaker.value = primary?.speaker?.copy(groupId = null)
            ?: list.firstOrNull { it.audible && it.speaker != null }?.let { it.speaker!!.copy(groupId = it.groupId) }
        videoPlane.requestSync()           // 주채널이 바뀌었을 수 있다 — 영상 호를 맞춘다(D10)
    }

    internal fun primarySession(): Session? =
        synchronized(lock) { sessionMap.values.firstOrNull { it.role == ChannelRole.PRIMARY } }

    // ── 채널/듣기/오디오 설정 ──

    /** [groupId] 를 주채널로 — 기존 주채널은 일반 참여로 강등. */
    fun setPrimary(groupId: String) {
        synchronized(lock) {
            val s = sessionMap[groupId] ?: return
            sessionMap.values.firstOrNull { it.role == ChannelRole.PRIMARY }?.let {
                if (it !== s) it.role = ChannelRole.NONE
            }
            s.role = ChannelRole.PRIMARY
        }
        channelStore?.primary = groupId
        videoPlane.requestSync()            // 고른 주채널 = 영상 채널(mcvideo.md §7 D10)
        _selectedGroup.value = groupId   // 선택 그룹 = 주채널 (SOS UseCurrentlySelectedGroup 대상)
        applyListenPolicy()
    }

    /** 주채널 해제 — 일반 참여로 강등(다른 채널 자동 승격 없음, 주채널 없는 상태 허용). */
    fun clearPrimary(groupId: String) {
        synchronized(lock) {
            val s = sessionMap[groupId] ?: return
            if (s.role != ChannelRole.PRIMARY) return
            s.role = ChannelRole.NONE
        }
        channelStore?.let { if (it.primary == groupId) it.primary = null }
        videoPlane.requestSync()
        applyListenPolicy()
    }

    fun setListenPolicy(p: ListenPolicy) {
        _listenPolicy.value = p
        applyListenPolicy()
    }

    /** 듣기 정책 적용 — 비채널 그룹은 참여 유지하되 수신 음소거. 채널별 수신 음량도 건다(코어가 호에 기억해 재결선마다 다시 건다).
     *  MCVideo 영상을 받는 동안은 무전 소리를 [VIDEO_DUCK] 배로 줄인다 — 두 호의 소리가 겹치면 영상 호 음성 우선(mcvideo.md §7 D6). */
    internal fun applyListenPolicy() {
        val policy = _listenPolicy.value
        val duck = if (videoPlane.receivingAny()) VIDEO_DUCK else 1f
        val ops = ArrayList<Triple<Int, Boolean, Float>>()
        synchronized(lock) {
            for (s in sessionMap.values) {
                // 1:1(private)은 사용자가 명시적으로 건/받은 대화 — 채널 소음 제어용 듣기
                // 정책(CHANNELS_ONLY)의 대상이 아니다. 주채널 비점유(role=NONE)라도 항상 수신.
                val on = policy == ListenPolicy.ALL || s.role != ChannelRole.NONE || s.privatePeer
                s.audible = on
                if (s.callId >= 0) ops.add(Triple(s.callId, on, s.volume * duck))
            }
        }
        ops.forEach { (id, on, vol) ->
            cmd("setListen($id)") { ue.call(id).setListen(on) }
            cmd("setRxLevel($id)") { ue.call(id).setRxLevel(vol) }
        }
        publish()
    }

    /** 채널별 수신 음량(0~2, 1=원음) — 이 호에서 듣는 크기. 영속 저장(리부팅 유지). */
    fun setChannelVolume(groupId: String, level: Float) {
        volumeStore?.set(groupId, level)
        val id = synchronized(lock) {
            val s = sessionMap[groupId] ?: return
            s.volume = level
            s.callId
        }
        val duck = if (videoPlane.receivingAny()) VIDEO_DUCK else 1f
        if (id >= 0) cmd("setRxLevel($id)") { ue.call(id).setRxLevel(level * duck) }
        publish()
    }

    /** 오디오 출력 라우팅(전역) — 스피커폰/수화기(엔진) 또는 이어폰([AUDIO_ROUTE_HEADSET]+[deviceId]).
     *  선택은 [routePrefs] 로 영속(리부팅/재기동 복원). */
    fun setAudioRoute(route: Int, deviceId: Int = -1) {
        _audioRoute.value = route
        _headsetId.value = deviceId
        routePrefs?.let { it.route = route; it.headsetId = deviceId }
        applyAudioRoute()
    }

    /** 출력 장치 소멸(이어폰/BT 해제) 복구 — 사운드 장치를 실제로 닫았다 다시 열어 재생 트랙을 재생성한다(코어
     *  `reopenAudioDevice`). 일부 단말(MF52/A15 실측)이 장치 소멸 순간 무전 트랙에 시스템 뮤트를 건 채 해제하지 않는다
     *  (android_ue_client.md). */
    fun recoverFromDeviceLoss() = cmd("reopenAudioDevice") { ue.reopenAudioDevice() }

    /** 무전 장치 음량(스피커 출력 배율 / 마이크 AGC 목표 보정, ×1.0~×3.0) — 영속 + 통화 중이면 즉시 적용. */
    fun setAudioGain(spk: Float, mic: Float) {
        val s = spk.coerceIn(com.cims.ue.ptt.audio.AudioRoutePrefs.GAIN_MIN, com.cims.ue.ptt.audio.AudioRoutePrefs.GAIN_MAX)
        val m = mic.coerceIn(com.cims.ue.ptt.audio.AudioRoutePrefs.GAIN_MIN, com.cims.ue.ptt.audio.AudioRoutePrefs.GAIN_MAX)
        _spkGain.value = s
        _micGain.value = m
        routePrefs?.let { it.spkGain = s; it.micGain = m }
        if (synchronized(lock) { sessionMap.values.any { it.active } }) applyDeviceLevels(s, m)
    }

    /** 장치 단 음량 — 스피커 배율, 마이크는 AGC 목표 = -26 + 20·log10(배율) dBov(ue_audio_level.md §6 — 환산은 앱). */
    private fun applyDeviceLevels(spk: Float, mic: Float) {
        val target = CimsUe.MIC_AGC_TARGET_DBOV + 20.0 * kotlin.math.log10(mic.coerceAtLeast(0.01f).toDouble())
        cmd("setDeviceAudioLevels") { ue.setDeviceAudioLevels(spk, target) }
    }

    /** 현재 라우팅 적용/재적용 — 통화 성립 시에도 호출(라우팅 리셋 대비). */
    internal fun applyAudioRoute() {
        applyAudioRouteNow()
        // 볼륨 인덱스는 장치별 — 적용 직후와 재라우팅이 가라앉은 뒤 현재 장치 축의 음량을 확보
        audioRouter?.ensureRxVolume()
        scope.launch { delay(800); audioRouter?.ensureRxVolume() }
    }

    private fun applyAudioRouteNow() {
        if (_audioRoute.value == AUDIO_ROUTE_HEADSET) {
            cmd("setAudioRoute") { ue.setAudioRoute(AudioRoute.DEFAULT, AudioRoute.DEFAULT) }   // 강제 라우팅 해제 — 이어폰 마이크
            audioRouter?.select(_headsetId.value)
        } else {
            audioRouter?.clear()
            // 단말 스피커·수화기로 들을 때는 입력을 내장 기본(하단) 마이크에 고정한다 — 스피커 출력이면 정책이 후면 마이크를
            //   골라 입에 대고 말하는 무전이 ~20 dB 작아진다(ue_audio_level.md §3).
            val out = if (_audioRoute.value == AUDIO_ROUTE_EARPIECE) AudioRoute.EARPIECE else AudioRoute.LOUDSPEAKER
            cmd("setAudioRoute") { ue.setAudioRoute(out, AudioRoute.EARPIECE) }
            // 엔진 라우트를 무시하는 백엔드 대비 — AudioManager 직접 적용 병행
            audioRouter?.setSpeakerphone(_audioRoute.value == AUDIO_ROUTE_SPEAKER)
        }
    }

    fun shutdown() {
        pttHeld = false
        setTalkCapture(false)    // 발언 중 종료 대비 — volte 마이크 복귀 통지
        synchronized(lock) {
            sessionMap.values.forEach { it.close() }
            sessionMap.clear()
        }
        feedback?.close(); feedback = null
        ctl.launch {
            runCatching { ue.stop() }
            runCatching { ue.close() }
            runCatching { csc?.close() }
            account = null
            scope.cancel()
            ctl.cancel()
        }
    }

    companion object {
        internal const val TAG = "PttController"

        /** Granted Duration(T2) 마감 임박 알림 시점 — 마감 이 시간 전에 톤·진동으로 알린다(자체 종료는 코어). */
        internal const val TALK_WARN_MS = 5000L

        /** MCVideo 영상을 받는 동안 무전(MCPTT) 수신 배율 — 영상 호 음성 우선(mcvideo.md §7 D6). */
        internal const val VIDEO_DUCK = 0.3f

        /** 오디오 라우팅 — 저장값(AudioRoutePrefs)과 같은 수. 0~2 는 엔진 라우트, 3 = 이어폰(유선/BT 장치 지정). */
        const val AUDIO_ROUTE_DEFAULT = 0   // 자동(이어폰 연결 시 이어폰)
        const val AUDIO_ROUTE_EARPIECE = 1  // 수화구
        const val AUDIO_ROUTE_SPEAKER = 2   // 외장 스피커
        const val AUDIO_ROUTE_HEADSET = 3

        /** affiliation PUBLISH 유지 수명(초) — 코어 PUBLISH Expires 와 같다. 잔여 수명이 절반 미만이면 주기 루프가 재발행. */
        internal const val AFF_EXPIRES_SEC = 3600L

        /** 최초 SUBSCRIBE 발행 후 확인(NOTIFY) 대기 시한 — 초과하면 재발행 대상으로 되돌린다.
         *  CSP 는 구독 수락 직후 초기 NOTIFY 를 보내므로 정상 경로는 수십 ms 다. 이 창은
         *  생성 경합(중복 구독)을 막는 용도이므로 짧게 두되, 패킷 유실·일시 지연은 흡수하는 값. */
        internal const val SUB_CONFIRM_TIMEOUT_MS = 15_000L

        /** 구독 재확인 주기 — 이 시간마다 SUBSCRIBE 를 다시 던진다(살아 있으면 엔진이 in-dialog 갱신으로 흡수,
         *  죽었으면 새로 생성). 서버가 구독을 잃어도 최대 이 시간 안에 복구된다. */
        internal const val SUB_REASSERT_MS = 600_000L

        /** 채널 복원 전 양보 — 재로그인 경로에서 서버 fan-out INVITE 가 먼저 오면 그 세션을 쓴다. */
        internal const val RESTORE_YIELD_MS = 3000L

        /** 채널 복원이 로스터 NOTIFY(진행 여부)를 기다리는 상한 — 양보 뒤부터. 정상 경로는 등록 직후 수십 ms 안에 온다. */
        internal const val RESTORE_ROSTER_WAIT_MS = 5000L

        /** xcap-diff 문서 축 — 서버 PSI 이름(`{kind}_psi`)·CSP 이벤트 판별 키와 같은 문자열. */
        internal const val XCAP_GMS = "gms"
        internal const val XCAP_CMS = "cms"

        /** URI("tel:g001"/"sip:g001@dom"/"\"이름\" <sip:..>") → 번호부("g001"). */
        fun bareId(uri: String): String {
            val m = Regex("(?:tel:|sips?:)([^@>;\\s]+)").find(uri)
            return (m?.groupValues?.get(1) ?: uri.trim()).substringBefore('@')
        }

        /** 애드혹 임시 그룹 ID 판별 — `adhoc-` 접두사는 편성 그룹 ID 로 예약 거부(CSC)돼
         *  기존 그룹·가입자 번호와 충돌하지 않는다. 편성 채널 저장/구독/affiliation 제외 기준. */
        fun isAdhocId(id: String): Boolean = id.startsWith("adhoc-")

        /** 새 message ID — UUID hex32(TS 24.282 SDS SIGNALLING PAYLOAD Message ID). 저장을 먼저 하고 보낼 때 앱이 만든다. */
        fun newMessageId(): String = java.util.UUID.randomUUID().toString().replace("-", "")

        /** 홈 국가코드(digits) — 프로비저닝 countryCode, 없으면 내 msisdn ITU 규칙 유도(VoLTE 앱과 동일). */
        var homeCountryCode: String? = null

        /** ITU 자릿수 규칙 E.164 국가코드 추정 — 프로비저닝 미수신 fallback 전용.
         *  1(NANP)/7=1자리, 유효 2자리 셋, 그 외 3자리. */
        fun countryCodeOf(msisdn: String): String? {
            val d = msisdn.trim().removePrefix("tel:").removePrefix("+").filter { it.isDigit() }
            if (d.length < 4) return null
            if (d[0] == '1' || d[0] == '7') return d.take(1)
            val two = d.take(2)
            val twoDigit = setOf(
                "20", "27", "30", "31", "32", "33", "34", "36", "39", "40", "41", "43", "44", "45",
                "46", "47", "48", "49", "51", "52", "53", "54", "55", "56", "57", "58", "60", "61",
                "62", "63", "64", "65", "66", "81", "82", "84", "86", "90", "91", "92", "93", "94",
                "95", "98",
            )
            return if (two in twoDigit) two else d.take(3)
        }

        /** 홈 국가코드(+82 등)와 같은 국제표기 번호는 로컬 표기(0…)로 축약. 타국 번호는 그대로(표시 전용). */
        fun fmtNumber(number: String): String {
            val cc = homeCountryCode ?: return number
            val n = number.trim().removePrefix("tel:")
            val digits = n.removePrefix("+")
            return if (n.startsWith("+") && digits.startsWith(cc)) "0" + digits.removePrefix(cc) else number
        }

        private fun pjPriority(level: Int): Int = when {
            level <= 1 -> Log.ERROR
            level == 2 -> Log.WARN
            else -> Log.INFO
        }
    }
}
