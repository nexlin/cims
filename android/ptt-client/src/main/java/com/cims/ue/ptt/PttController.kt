package com.cims.ue.ptt

import android.os.SystemClock
import android.util.Log
import com.cims.ue.core.config.SipAccountConfig
import com.cims.ue.core.sip.CallState
import com.cims.ue.core.sip.RegState
import com.cims.ue.core.sip.SipBodyPart
import com.cims.ue.core.sip.SipController
import com.cims.ue.ptt.audio.PttFeedback
import com.cims.ue.ptt.csc.CscClient
import com.cims.ue.ptt.csc.CscConfig
import com.cims.ue.ptt.csc.GroupDoc
import com.cims.ue.ptt.csc.GroupSummary
import com.cims.ue.ptt.csc.TokenSet
import com.cims.ue.ptt.floor.FloorClient
import com.cims.ue.ptt.floor.FloorEvent
import com.cims.ue.ptt.floor.FloorIndicator
import com.cims.ue.ptt.floor.FloorPermission
import com.cims.ue.ptt.floor.FloorState
import com.cims.ue.ptt.floor.FloorTalker
import com.cims.ue.core.sip.MsrpEvent
import com.cims.ue.ptt.mcdata.McDataCodec
import com.cims.ue.ptt.mcdata.msrp.MsrpCodec
import com.cims.ue.ptt.mcdata.msrp.MsrpSession
import com.cims.ue.ptt.mcptt.McpttXml
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.util.UUID

/** 현재 발언자 — 내 GRANT([self]=true) 또는 타인 TAKEN. [sinceMs]=elapsedRealtime(경과시간 표시용).
 *  [groupId]=발언이 들리는 그룹(멀티그룹 모니터링에서 주채널 밖 발언 구분). */
data class Speaker(val id: String, val self: Boolean, val sinceMs: Long, val groupId: String? = null)

/** 채널 지정 — 주(발언 대상, 1개)/부(모니터링)/일반. */
enum class ChannelRole { PRIMARY, NONE }

/** 듣기 정책 — 주채널만 / 참여한 모든 그룹. */
enum class ListenPolicy { CHANNELS_ONLY, ALL }

/** 통화이력용 이벤트 종별. */
enum class PttEventKind { JOIN, LEAVE, TALK_ME, TALK_OTHER, EMERGENCY, EMERGENCY_IN, EMERGENCY_END, ALERT, ALERT_IN, ALERT_END }

/** 활성 긴급경보 (TS 24.379 emergency alert) — 통화와 별개인 위험 통지 상태.
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
     *  broadcast 그룹·ambient(recv_only) 청취 leg 는 0 이 와서 PTT 버튼을 비활성화한다. */
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
    /** 전이중 1:1(mc_no_floor_ctrl 협상) — floor 없음, 마이크 상시 개방. PTT 버튼 대신 통화 UI. */
    val fullDuplex: Boolean = false,
)

/**
 * MCPTT 그룹 PTT 오케스트레이션 — 비-PJSIP 코어(CSC/floor/XML)와 core PJSIP `SipController` 를 묶는다.
 *
 * **멀티그룹 동시 참여**(TS 22.179 group scanning): 그룹마다 독립 SIP 호 + floor 소켓을 유지한다.
 * 발언(PTT)은 항상 **주채널** 로만, 수신은 PJSIP conference bridge 가 자동 믹싱하되
 * [ListenPolicy] 에 따라 비채널 그룹을 음소거(setCallListen)한다.
 */
class PttController(
    private val sipConfig: SipAccountConfig,
    /** MCPTT ID (tel: URI, 예 "tel:+82571900001") — CSC userUri·calling-user-id. */
    val mcpttId: String,
    private val cscConfig: CscConfig? = null,
    private val allowInsecureTls: Boolean = false,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    val sip = SipController(sipConfig)
    val regState: StateFlow<RegState> get() = sip.regState
    val callState: StateFlow<CallState> get() = sip.callState

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

    /** 라우팅 선택 영속화 — 서비스가 주입(리부팅/재기동 복원). */
    var routePrefs: com.cims.ue.ptt.audio.AudioRoutePrefs? = null

    /** 통화이력 이벤트 훅 — 서비스가 주입(HistoryStore 영속). 컨트롤러 스레드에서 호출되므로 가볍게. */
    var onEvent: ((PttEvent) -> Unit)? = null
    private fun emit(kind: PttEventKind, groupId: String, peer: String? = null, durationMs: Long = 0) {
        runCatching { onEvent?.invoke(PttEvent(kind, groupId, peer, durationMs)) }
    }

    /** 발언 마이크 핸드오프 훅 — 서비스가 주입(volte 앱에 MIC_YIELD/RESUME 브로드캐스트). */
    var micHandoff: ((Boolean) -> Unit)? = null

    /** 발언 캡처 게이트 현재 상태 — 중복 전환 방지. */
    private var talkCapture = false

    /**
     * 발언 캡처 게이트 — 유휴/청취=스피커 전용(마이크 미보유), 발언 시도~종료=전이중.
     * 마이크를 발언 구간에만 보유해야 OS 동시 캡처 중재가 통화(volte) 앱 캡처를 무음화하지 않는다.
     * volte 협조 핸드오프([micHandoff])와 snd dev 모드 전환을 한 지점에서 묶는다 — PTT down 에서
     * floor 요청과 병렬로 시작해 GRANT 톤이 끝날 때(mic 개방)면 전환이 끝나 있다.
     */
    private fun setTalkCapture(on: Boolean) {
        if (talkCapture == on) return
        talkCapture = on
        runCatching { micHandoff?.invoke(on) }
        sip.setCaptureEnabled(on)
    }

    /** 수신 문자(SIP MESSAGE) — core 흐름 그대로 노출(서비스가 MessageStore 에 영속). */
    val incomingMessage get() = sip.incomingMessage

    /** MSRP 미디어평면으로 수신한 SDS — [MediaSds] (서비스가 MessageStore 영속·통지). */
    data class MediaSds(val groupId: String, val sender: String, val msg: McDataCodec.SdsMessage)
    private val _incomingSds = MutableSharedFlow<MediaSds>(extraBufferCapacity = 16)
    val incomingSds: SharedFlow<MediaSds> = _incomingSds.asSharedFlow()

    /** MSRP 발신 진행 — (msgId, 송신 바이트, 전체 바이트). 청크 200 수리 시마다 방출. */
    data class SendProgress(val msgId: String, val sent: Int, val total: Int)
    private val _sendProgress = MutableSharedFlow<SendProgress>(extraBufferCapacity = 64)
    val sendProgress: SharedFlow<SendProgress> = _sendProgress.asSharedFlow()

    /** MSRP 발신 결과 — (msgId, 성공 여부). 서비스가 MessageStore 상태(SENT/FAILED) 반영. */
    private val _sendResult = MutableSharedFlow<Pair<String, Boolean>>(extraBufferCapacity = 16)
    val sendResult: SharedFlow<Pair<String, Boolean>> = _sendResult.asSharedFlow()

    /** [text] 가 C-plane 임계를 초과해 MSRP 미디어평면으로 발신되는가 — 초기 말풍선 상태 판단용. */
    fun willUseMsrp(text: String): Boolean =
        sipConfig.maxPayloadSdsCplaneBytes > 0 &&
            McDataCodec.sdsPayloadSize(text) > sipConfig.maxPayloadSdsCplaneBytes

    // ── 그룹별 세션 ──

    private inner class Session(val groupId: String, preFloor: FloorClient? = null) {
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
        // (broadcast 그룹·ambient 청취 leg). 눌러도 Deny 만 받으므로 버튼을 미리 막는다.
        var canRequestFloor: Boolean = true
        var speakDeadlineMs: Long = 0         // Granted Duration(T2) 마감(elapsedRealtime), 0=무제한
        var talkLimit: Job? = null            // 마감 임박 알림 + 자체 종료 타이머
        var queuePosition: Int? = null        // Queue Position Info — 대기 중일 때만
        var talkers: List<Speaker> = emptyList()   // 동시 발언 화자 전체(§8.2.3.17~18)
        var talkerSsrc: Map<String, Long> = emptyMap()  // 화자 → RTP SSRC (SSRC 별 재생용, U10)
        var floorIndicator: Int = 0           // 마지막 수신 Floor Indicator (G/I 비트 표시)
        var privatePeer: Boolean = false      // 1:1 private call — groupId=상대 번호
        var fullDuplex: Boolean = false       // 전이중 1:1 — floor 없음, mic 상시 개방
        // 착신은 INVITE 수신 시점에 선바인드된 floor 소켓([pendingFloors])을 인수한다 —
        //   응답 SDP 가 그보다 먼저 만들어지므로 여기서 새로 만들면 광고할 포트가 없다.
        val floor: FloorClient = preFloor ?: FloorClient(ssrc, mcpttId, localPort = 0,
            onEvent = { ev -> onFloorEvent(groupId, ev) })

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
        fun close() { talkLimit?.cancel(); runCatching { floor.close() } }
    }

    private val lock = Any()
    private val sessionMap = LinkedHashMap<String, Session>()   // groupId → Session (참여 순서 유지)

    /** 착신 INVITE 시점에 선바인드한 floor 소켓 (callId → FloorClient). 수락 시 Session 이 인수하고,
     *  수락 없이 끝난 호는 [releasePendingFloor] 로 회수한다 — 미회수는 소켓·수신 스레드 누수다. */
    private val pendingFloors = java.util.concurrent.ConcurrentHashMap<Int, FloorClient>()

    /** 선바인드 floor 소켓 인수(수락 경로) — 없으면 null(발신·재통지 등). */
    private fun takePendingFloor(callId: Int): FloorClient? = pendingFloors.remove(callId)

    /** 인수되지 않은 선바인드 floor 소켓 폐기. */
    private fun releasePendingFloor(callId: Int) {
        pendingFloors.remove(callId)?.let { runCatching { it.close() } }
    }
    // 구독 상태는 **서버 확인 기반**으로 관리한다 — SUBSCRIBE 를 보냈다는 사실만으로 "구독 중"
    // 으로 취급하면, 서버가 구독을 잃고(예: CSP 재기동으로 in-memory 구독 소멸) 단말이 등록
    // 끊김을 관측하지 못한 경우 멱등 가드가 재발행을 영구히 막아 로스터·편성 push 가 앱 재시작
    // 전까지 얼어붙는다(실측). affiliation 의 [affiliated] 와 같은 원칙이다.
    //   확인 신호 = **NOTIFY 도착**. 네이티브는 SUBSCRIBE 응답을 앱에 올려주지 않고, CSP 는
    //   구독 수락 직후 초기 NOTIFY 를 항상 보낸다(conference 는 로스터가 비어도, gms 는 그룹별로).
    //   나아가 확인 상태에 **재확인 주기**를 둔다 — 구독 소멸을 앱이 감지할 수단이 없기 때문이다
    //   (네이티브 evsub 이 in-dialog 갱신을 하다 481 을 받아 구독을 접어도 앱에는 통지가 없다).
    //   [SUB_REASSERT_MS] 마다 SUBSCRIBE 를 다시 던지면 살아 있는 구독은 native `cims_conf_find`
    //   가 in-dialog 갱신으로 흡수하고, 죽은 구독은 새로 만들어진다 — 감지 없이 수렴한다.
    //   affiliation 을 TTL 절반마다 재-PUBLISH 하는 것과 같은 형태다.
    private val confirmedRosters = mutableMapOf<String, Long>() // groupId → 마지막 확인/재확인 시각(ms)
    private val pendingRosters = mutableMapOf<String, Long>()   // groupId → 최초 SUBSCRIBE 발행 시각(ms)
    private var gmsConfirmedAt = 0L                             // xcap-diff(GMS) 마지막 확인/재확인 시각(ms)
    private var gmsPendingAt = 0L                               // GMS 최초 SUBSCRIBE 발행 시각(ms)
    private val rosterMap = mutableMapOf<String, Map<String, String>>()  // groupId → 접속 인원(미조인 포함)

    private val _sessions = MutableStateFlow<List<GroupCallState>>(emptyList())
    /** 참여 중인 그룹 세션들(참여 순). */
    val sessions: StateFlow<List<GroupCallState>> = _sessions.asStateFlow()

    private val _listenPolicy = MutableStateFlow(ListenPolicy.ALL)
    /** 듣기 정책 — 주채널만/전체. */
    val listenPolicy: StateFlow<ListenPolicy> = _listenPolicy.asStateFlow()

    private val _audioRoute = MutableStateFlow(SipController.AUDIO_ROUTE_SPEAKER)
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

    private val _groups = MutableStateFlow<List<GroupSummary>>(emptyList())
    val groups: StateFlow<List<GroupSummary>> = _groups.asStateFlow()

    private val _groupDocs = MutableStateFlow<Map<String, GroupDoc>>(emptyMap())
    /** 그룹 문서(TS 24.481) 캐시 — groupId → 멤버(이름·번호·역할·우선순위)·그룹 속성. [loadGroupDetail] 로 적재. */
    val groupDocs: StateFlow<Map<String, GroupDoc>> = _groupDocs.asStateFlow()

    private val _selectedGroup = MutableStateFlow<String?>(null)
    /** 그룹 목록에서 선택된 그룹(참여 전 하이라이트·affiliation 대상). */
    val selectedGroup: StateFlow<String?> = _selectedGroup.asStateFlow()

    /** 활성 긴급경보 — 수신(fan-out)+내 발신. 취소 MESSAGE 수신/발신 시 해제. */
    private val _alerts = MutableStateFlow<List<ActiveAlert>>(emptyList())
    val alerts: StateFlow<List<ActiveAlert>> = _alerts.asStateFlow()

    private val _affiliated = MutableStateFlow<Set<String>>(emptySet())
    /** 서버가 2xx 로 확인한 affiliation 그룹(응답 기반 — PUBLISH 송신만으로는 포함하지 않음). */
    val affiliated: StateFlow<Set<String>> = _affiliated.asStateFlow()

    private val _channelRosters = MutableStateFlow<Map<String, Map<String, String>>>(emptyMap())
    /** 채널별 접속 인원 — groupId → (참가자ID → status). conference 구독(RFC 4575) NOTIFY 로 갱신되며
     *  **미조인 채널도 포함**한다(제휴 채널 전체를 구독하므로). 참여 중인 채널의 로스터는
     *  [sessions] 의 participants 와 같은 값이다. */
    val channelRosters: StateFlow<Map<String, Map<String, String>>> = _channelRosters.asStateFlow()

    // ── affiliation 상태 머신(TS 24.379 §9) — 희망 집합(편성 채널 전체)을 서버 확인 기반으로 유지 ──
    //   PUBLISH 는 token 으로 최종 응답과 상관: 2xx=확정(만료 기록), 실패=백오프 재시도(그룹 편성이
    //   PUBLISH 보다 늦는 레이스·일시 오류 자가 치유). TTL 절반 경과 시 주기 루프가 재발행(만료 방치 방지).
    private val affPending = java.util.concurrent.ConcurrentHashMap<Long, Pair<String, Boolean>>()
    private val affExpireAt = java.util.concurrent.ConcurrentHashMap<String, Long>()   // 확정 만료(elapsedRealtime)
    private val affAttempts = java.util.concurrent.ConcurrentHashMap<String, Int>()
    private val affBackoffUntil = java.util.concurrent.ConcurrentHashMap<String, Long>()  // 백오프 대기 종료 시각
    /** 서버가 준 `SIP-ETag`(RFC 3903) — 갱신 PUBLISH 의 `SIP-If-Match` 로 되돌려 refresh 로 처리되게 한다. */
    private val affEtag = java.util.concurrent.ConcurrentHashMap<String, String>()
    private val affSeq = java.util.concurrent.atomic.AtomicLong(1)
    /** 403(등록 소실) 대응 재-REGISTER 스로틀 — 연속 실패마다 재등록하지 않도록. */
    @Volatile private var affReRegisterAt = 0L

    private val _status = MutableStateFlow("대기")
    val status: StateFlow<String> = _status.asStateFlow()

    private var csc: CscClient? = cscConfig?.let { CscClient(it, allowInsecureTls) }
    @Volatile private var token: TokenSet? = null
    /** CSC 토큰 보유 여부 — 서비스의 SSO 주입 중복 방지용(주입은 [setAccessToken]). */
    val hasAccessToken: Boolean get() = token != null
    @Volatile private var pttHeld = false
    private var requestTimeout: Job? = null
    private val ssrc: Long = (mcpttId.hashCode().toLong() and 0xffffffffL).let { if (it == 0L) 1L else it }

    init {
        // 번호 로컬 표기(+82→0…)용 홈 국가코드 — 프로비저닝 countryCode 우선, 내 msisdn 유도 폴백
        homeCountryCode = sipConfig.countryCode.ifBlank { countryCodeOf(mcpttId) ?: "" }.ifBlank { null }

        // MCData MSRP 수신 capability 광고(TS 24.282 §6.3) — 서버가 이 태그를 보고
        // 그룹 SDS 미디어평면 배포 레그(INVITE+MSRP)를 이 단말로 보낸다.
        sip.contactParams = ";+g.3gpp.icsi-ref=\"$MCDATA_ICSI\""

        // 서버발 MSRP 배포 INVITE — 수락·수신·저장 (통화 UI 와 무관, 격리 처리)
        scope.launch {
            sip.msrpEvents.collect { ev ->
                if (ev is MsrpEvent.Incoming) launch { runCatching { handleIncomingMsrp(ev) } }
            }
        }

        // 학습된 CMP floor 목적지(호별) → 해당 세션 FloorClient 연결 + 즉시 Ack 1회
        //   (이후 주기 Ack keepalive 는 FloorClient 가 자체 수행 — NAT 매핑·latch 유지)
        scope.launch {
            sip.floorRemote.collect { rem ->
                rem?.let { (callId, ip, port) ->
                    sessionByCall(callId)?.let { s ->
                        s.floor.connectRemote(ip, port)
                        s.floor.sendAck()
                        _status.value = "[${s.groupId}] floor 연결 $ip:$port"
                    }
                }
            }
        }
        // 착신 floor 소켓 선바인드 — INVITE 수신 즉시(응답 SDP 생성 전) 호출된다.
        //   여기서 만든 FloorClient 를 autoJoin* 의 Session 이 그대로 인수한다(재바인드 금지 —
        //   광고한 포트와 실제 수신 소켓이 어긋나면 floor 가 도달하지 않는다).
        sip.incomingFloorSdp = { info ->
            val gid = bareId(info.callerId.ifBlank { info.remote })
            if (gid.isBlank()) null else {
                val fc = FloorClient(ssrc, mcpttId, localPort = 0,
                    onEvent = { ev -> onFloorEvent(gid, ev) })
                pendingFloors.put(info.callId, fc)?.let { runCatching { it.close() } }  // 재통지 방어
                floorSdp(fc.localPort, info.noFloorCtrl)
            }
        }
        // 호 상태 → 세션 매핑 + MCPTT 그룹콜 착신 자동 수락(ptt_ue.md §12.3)
        scope.launch {
            sip.callState.collect { st ->
                when (st) {
                    is CallState.Outgoing -> bindCall(bareId(st.remote), st.id)
                    is CallState.Active -> {
                        bindCall(bareId(st.remote), st.id, active = true)
                        audioRouter?.setInCall(true)                // VoIP 오디오 모드 — 라우팅·음량 전제
                        sip.setDeviceAudioBoost(_spkGain.value, _micGain.value) // 무전 체감 음량 보강
                        applyAudioRoute()                           // 통화별 라우팅 재적용
                        applyListenPolicy()
                    }
                    is CallState.Disconnected -> {
                        releasePendingFloor(st.id)   // 수락 전 종료(취소·거절) — 선바인드 소켓 회수
                        handleEmergencyDenied(st.id, st.code)  // 긴급 개시 403 → normal 재발신 폴백
                        onCallEnded(st.id)
                    }
                    is CallState.Incoming ->
                        if (st.mcptt) { if (st.privateCall) autoJoinPrivateCall(st) else autoJoinGroupCall(st) }
                    else -> Unit
                }
            }
        }
        // 미인가 in-call 긴급 상향 거절 (403 + emergency-ind=false, TS 24.379 §6.3.3.1.14) —
        //   재-INVITE 거절은 통화를 끊지 않으므로 낙관 latch 만 되돌린다. 선발신된 경보는 별개
        //   기능(자체 게이트)이라 회수하지 않는다 — 취소는 사용자의 SOS 해제로.
        scope.launch {
            sip.emergencyDenied.collect { cid ->
                val s = synchronized(lock) {
                    sessionMap.values.firstOrNull { it.callId == cid && it.emergency && it.emergencyMine }
                } ?: return@collect
                s.emergency = false
                s.emergencyMine = false
                _status.value = "[${s.groupId}] 긴급 상향 미인가"
                publish()
            }
        }
        // 참가자 목록 — 정식 구독 경로(RFC 4575 conference 이벤트). NOTIFY 는 native 구독이
        // 200 으로 수용한 뒤 본문만 올려주므로 그룹 AoR(=conference focus)로 그룹을 식별한다.
        // 제휴 채널 전체를 구독하므로 **참여하지 않은 채널의 NOTIFY 도 온다** — 세션 유무와
        // 무관하게 rosterMap 을 갱신하고, 세션이 있으면 그 participants 도 함께 맞춘다.
        scope.launch {
            sip.incomingMessage.collect { im ->
                if (im.contentType.contains("xcap-diff", ignoreCase = true)) {
                    synchronized(lock) { gmsPendingAt = 0L; gmsConfirmedAt = SystemClock.elapsedRealtime() }
                    runCatching { onXcapDiff(im.body) }
                    return@collect
                }
                // 긴급경보 (TS 24.379 emergency alert) — mcptt-info MESSAGE(alert-ind).
                //   서버 fan-out 은 원본 본문 그대로 중계(From=발신자)라 그룹은 본문에서 읽는다.
                if (im.contentType.contains("mcptt-info", ignoreCase = true)) {
                    runCatching { onAlertMessage(im.fromUri, im.body) }
                    return@collect
                }
                if (!im.contentType.contains("conference-info", ignoreCase = true)) return@collect
                val gid = bareId(im.fromUri)
                // 구독 확인 — 이 경로(그룹 AoR 발신 NOTIFY)만 구독의 증거다. 아래 in-dialog
                // 폴백 경로는 통화 다이얼로그로 오므로 구독 확인으로 쓰면 안 된다.
                if (gid.isNotBlank()) synchronized(lock) {
                    pendingRosters.remove(gid)
                    confirmedRosters[gid] = SystemClock.elapsedRealtime()
                }
                runCatching { onConferenceInfo(gid, im.body) }
            }
        }
        // 참가자 목록 — in-dialog NOTIFY 폴백(구독자 0 인 구 APK 호환 경로). 서버가 구독을
        // 우선하고 구독자가 없을 때만 통화 dialog 로 보낸다. 본문은 항상 full 스냅샷이라
        // 두 경로가 겹쳐도 결과가 같다.
        scope.launch {
            sip.conferenceInfo.collect { (callId, xml) ->
                runCatching { sessionByCall(callId)?.let { onConferenceInfo(it.groupId, xml) } }
            }
        }
        // 등록 완료 시 편성 채널 전체 자동 affiliation — CSP 는 affiliation 된 멤버에게만 INVITE fan-out.
        // (주채널 1개 한정이던 최소 구현을 희망 집합 전체로 확장 — 로그인만으로 전 채널 fan-out 수신)
        scope.launch {
            sip.regState.collect { r ->
                if (r is RegState.Registered) {
                    affiliateAll()
                    syncRosterSubs()   // 편성 채널 전체 로스터 구독 (미조인 채널 인원 표시)
                    subscribeGms(true) // 편성 변경 push (관리자 변경 즉시 반영)
                    maybeRestoreChannels()
                } else {
                    // 등록이 끊기면 서버측 구독도 사라진다 — 확인 상태를 비워 재등록 시 다시 걸리게 한다.
                    synchronized(lock) { clearSubStateLocked() }
                    publishRosters()
                }
            }
        }
        // affiliation PUBLISH 최종 응답 — 2xx 확정, 실패는 지수 백오프 재시도. token 당 1회 처리
        // (같은 트랜잭션의 COMPLETED/TERMINATED 중복 통지는 affPending remove 로 자연 dedupe).
        scope.launch {
            sip.sendReqResults.collect { r ->
                val (g, on) = affPending.remove(r.token) ?: return@collect
                if (r.code in 200..299) {
                    affAttempts.remove(g)
                    affBackoffUntil.remove(g)
                    if (on) {
                        r.etag?.takeIf { it.isNotBlank() }?.let { affEtag[g] = it }   // 갱신 시 If-Match 근거
                        affExpireAt[g] = SystemClock.elapsedRealtime() + AFF_EXPIRES_SEC * 1000L
                        _affiliated.value = _affiliated.value + g
                    } else {
                        affEtag.remove(g)
                        affExpireAt.remove(g)
                        _affiliated.value = _affiliated.value - g
                    }
                } else if (on) {
                    _affiliated.value = _affiliated.value - g
                    val n = ((affAttempts[g] ?: 0) + 1).also { affAttempts[g] = it }
                    if (r.code == 412 && n <= 2) {
                        // ETag 불일치(서버 재기동 등으로 event state 소실·타 발행이 덮음) — ETag 를 버리고
                        // 즉시 초기 publication 으로 재발행한다(RFC 3903 §6). 조건이 바뀌었으므로 대기 불필요.
                        affEtag.remove(g)
                        Log.w(TAG, "affiliate $g 412 — ETag 폐기 후 초기 publication 재발행")
                        affiliate(g, true)
                    } else if (r.code == 403) {
                        // RFC 3261 §21.4.4 — 403 은 "수정 없이 반복하지 말 것". 조건이 바뀌어야 낫는
                        // 실패(등록 소실 / 그룹 비멤버)이므로 타이머 재시도를 걸지 않고, 조건 변화
                        // 이벤트(등록 성공→affiliateAll, 그룹목록 재적재, 채널 선택·키업)에 맡긴다.
                        // 등록 소실이 원인일 수 있으므로 등록 갱신만 트리거해 조건 자체를 바꾼다(60s 스로틀).
                        Log.w(TAG, "affiliate $g 403 ${r.reason} — 타이머 재시도 없음(조건 변화 대기)")
                        if (SystemClock.elapsedRealtime() - affReRegisterAt > 60_000L) {
                            affReRegisterAt = SystemClock.elapsedRealtime()
                            Log.w(TAG, "affiliate 403 — 등록 소실 추정, 등록 갱신 요청")
                            // 등록이 서버에서 사라졌다면 구독도 함께 사라졌다. refreshRegistration()
                            // 은 성공 시 pjsua 계정 상태를 Registered 에서 내리지 않으므로 regState
                            // 전이에 기대면 구독 확인 상태가 그대로 남아 재발행이 막힌다 — 여기서
                            // 직접 비워 다음 syncRosterSubs(주기 60s·조인·그룹목록 적재)가 재발행하게 한다.
                            synchronized(lock) { clearSubStateLocked() }
                            publishRosters()
                            sip.refreshRegistration()
                        }
                    } else {
                        // 일시적 실패(타임아웃·5xx 등)는 재시도가 정당 — 지수 백오프.
                        val backoffMs = (30_000L shl (n - 1).coerceAtMost(3)).coerceAtMost(300_000L)
                        affBackoffUntil[g] = SystemClock.elapsedRealtime() + backoffMs
                        Log.w(TAG, "affiliate $g 실패 ${r.code} ${r.reason} — ${backoffMs / 1000}s 후 재시도(#$n)")
                        launch {
                            delay(backoffMs)
                            if (regState.value is RegState.Registered && g in desiredAffiliations() && !affValid(g))
                                affiliate(g, true)
                        }
                    }
                }
            }
        }
        // 주기 갱신 — TTL 절반 경과 그룹 재-PUBLISH(1h 만료 방치로 fan-out 이 조용히 죽는 것 방지)
        scope.launch {
            while (true) {
                delay(60_000)
                if (sip.regState.value is RegState.Registered) {
                    affiliateAll()
                    syncRosterSubs()   // 편성 변경으로 채널이 늘/줄었으면 구독도 따라간다
                }
            }
        }
    }

    // ── 참여 채널 자동 복원 ──

    private var channelsRestored = false

    /** 프로세스 재시작(강제종료·재설치·리부팅) 후 참여 채널 자동 재조인 — 등록 완료 시 1회.
     *  재로그인 경로는 affiliation 후 서버 fan-out INVITE 가 먼저 올 수 있어 3s 양보하고,
     *  그 사이 생긴 세션은 존중한다([joinGroupCall] 이 중복 참여 무시). */
    private fun maybeRestoreChannels() {
        if (channelsRestored) return
        // 스토어 배선 전(접근성 헤드리스 재기동)이면 복원 기회를 소모하지 않는다 —
        // 배선 시점에 channelStore setter 가 재호출(플래그는 실제 복원 착수에서만 소모).
        val st = channelStore ?: return
        channelsRestored = true
        val want = st.joined
        if (want.isEmpty()) return
        val primary = st.primary
        scope.launch {
            delay(3000)
            val ordered = if (primary != null) listOf(primary) + (want - primary) else want
            for (g in ordered) {
                if (synchronized(lock) { sessionMap.containsKey(g) }) continue
                _status.value = "채널 자동 복원: $g"
                joinGroupCall(g)
                delay(300)
            }
            primary?.let { p -> if (synchronized(lock) { sessionMap.containsKey(p) }) setPrimary(p) }
        }
    }

    // ── 세션 헬퍼 ──

    /** 그룹 AoR — INVITE/PUBLISH/SUBSCRIBE/MESSAGE 공통 Request-URI. */
    private fun groupAor(groupId: String) = "sip:$groupId@${sipConfig.domain}"

    /** 채널 참가자 로스터 구독 시작/해지 (RFC 4575 conference 이벤트).
     *  갱신은 native 구독이 in-dialog 로 자동 수행하므로 여기서는 시작·해지만 다룬다.
     *
     *  구독 대상은 **참여 채널이 아니라 제휴(편성) 채널 전체**다 — 참여하지 않은 채널의 접속 인원도
     *  목록에 표시하기 위함. 따라서 채널 이탈은 구독을 끊지 않는다([syncRosterSubs] 가 제휴 집합
     *  기준으로만 정리).
     *
     *  ⚠️멱등이 필요하다 — 등록·제휴·조인이 각자 이 함수를 호출하므로 가드가 없으면 같은 그룹에
     *  SUBSCRIBE 가 동시에 두 번 나가 서버에 구독이 중복 생성된다(실측됨). native 의
     *  `cims_conf_find` 는 URI 로 기존 구독을 찾아 in-dialog 갱신하지만, 첫 구독이 테이블에
     *  등록되기 전에 두 번째 호출이 들어오면 경합으로 새 다이얼로그가 하나 더 생긴다.
     *
     *  다만 그 가드는 **발행 후 확인까지의 창**에만 걸린다 — 확인(NOTIFY) 없이
     *  [SUB_CONFIRM_TIMEOUT_MS] 가 지나면 재발행 대상으로 되돌린다. 서버가 구독을 잃은 경우
     *  (CSP 재기동 등) 조인·주기 루프가 스스로 복구하는 유일한 경로다. */
    private fun subscribeRoster(groupId: String, on: Boolean) {
        if (on) {
            val now = SystemClock.elapsedRealtime()
            synchronized(lock) {
                val confirmedAt = confirmedRosters[groupId]
                if (confirmedAt != null) {
                    if (now - confirmedAt < SUB_REASSERT_MS) return    // 아직 유효 — 재확인 시점 아님
                    confirmedRosters[groupId] = now                    // 재확인 발행 — 다음 주기까지 유효 취급
                } else {
                    val at = pendingRosters[groupId]
                    if (at != null && now - at < SUB_CONFIRM_TIMEOUT_MS) return   // 첫 확인 대기 중
                    pendingRosters[groupId] = now
                }
            }
        } else {
            synchronized(lock) {
                val wasConfirmed = confirmedRosters.remove(groupId) != null
                val wasPending = pendingRosters.remove(groupId) != null
                if (!wasConfirmed && !wasPending) return   // 걸어둔 적 없는 구독 — 해지 불필요
                rosterMap.remove(groupId)
            }
            publishRosters()
        }
        Log.i(TAG, "conference 구독 ${if (on) "발행" else "해지"} $groupId")
        runCatching { sip.subscribeConference(groupAor(groupId), if (on) SipController.CONF_SUB_EXPIRES_SEC else 0) }
            .onFailure {
                Log.w(TAG, "conference 구독($on) 실패 $groupId: ${it.message}")
                synchronized(lock) { if (on) pendingRosters.remove(groupId) }
            }
    }

    /** 특정 그룹의 구독 확인만 무효화 — 다음 [syncRosterSubs] 가 재확인(SUBSCRIBE)을 발행한다.
     *  해지가 아니다. 구독이 살아 있으면 in-dialog 갱신으로 흡수되므로 여분 트래픽이 거의 없다. */
    private fun invalidateRosterConfirm(groupId: String) {
        if (groupId.isBlank()) return
        synchronized(lock) { confirmedRosters.remove(groupId); pendingRosters.remove(groupId) }
    }

    /** 구독 확인 상태 일괄 초기화 — 서버가 우리 상태를 잃었다고 판단했을 때만 호출.
     *  호출자는 [lock] 을 보유해야 한다. */
    private fun clearSubStateLocked() {
        confirmedRosters.clear()
        pendingRosters.clear()
        rosterMap.clear()
        gmsConfirmedAt = 0L
        gmsPendingAt = 0L
    }

    /** GMS 문서 변경 구독 (RFC 5875 xcap-diff) — 서버 PSI 하나에 대한 단일 구독.
     *  관리자가 편성(멤버·우선순위·채널 추가/삭제)을 바꾸면 서버가 밀어준다. */
    private fun subscribeGms(on: Boolean) {
        val now = SystemClock.elapsedRealtime()
        synchronized(lock) {
            if (on) {
                if (gmsConfirmedAt != 0L) {
                    if (now - gmsConfirmedAt < SUB_REASSERT_MS) return   // 아직 유효
                    gmsConfirmedAt = now                                 // 재확인 발행
                } else {
                    if (gmsPendingAt != 0L && now - gmsPendingAt < SUB_CONFIRM_TIMEOUT_MS) return
                    gmsPendingAt = now
                }
            } else {
                if (gmsConfirmedAt == 0L && gmsPendingAt == 0L) return
                gmsConfirmedAt = 0L
                gmsPendingAt = 0L
            }
        }
        runCatching { sip.subscribeXcapDiff(gmsPsiAor(), if (on) SipController.CONF_SUB_EXPIRES_SEC else 0) }
            .onFailure {
                Log.w(TAG, "gms 구독($on) 실패: ${it.message}")
                synchronized(lock) { if (on) gmsPendingAt = 0L }
            }
    }

    private fun gmsPsiAor() = "sip:gms_psi@${sipConfig.domain}"

    /** xcap-diff NOTIFY — "어느 문서가 바뀌었다"는 신호만 온다. 실제 내용은 XCAP HTTP 로 재조회.
     *
     *  본문 예: `<document new-etag=".." sel="org.openmobilealliance.groups/users/tel:{나}/tel:{그룹}"/>`
     *  `loadGroups()` 는 편성 집합 자체의 변화(채널 추가/삭제)를 반영하고, 이어서 제휴·로스터 구독까지
     *  다시 맞춘다. 바뀐 그룹은 문서까지 재조회한다(둘 다 ETag 캐시라 실제 변화 없으면 저렴). */
    private fun onXcapDiff(xml: String) {
        val changed = Regex("sel=\"([^\"]+)\"").findAll(xml)
            .map { it.groupValues[1] }
            .filter { it.contains("openmobilealliance.groups", ignoreCase = true) }
            .mapNotNull { it.substringAfterLast("tel:").takeIf { g -> g.isNotBlank() } }
            .toList()
        Log.i(TAG, "xcap-diff NOTIFY — 변경 그룹 $changed")
        _status.value = if (changed.isEmpty()) "편성 변경 통지" else "편성 변경: ${changed.joinToString()}"
        loadGroups()
        changed.forEach { loadGroupDetail(it) }
    }

    /** 제휴(편성) 채널 집합에 로스터 구독을 맞춘다 — 새로 편성된 채널은 구독하고, 빠진 채널은 해지.
     *  등록 완료·그룹 목록 적재·제휴 주기 루프에서 호출한다. */
    private fun syncRosterSubs() {
        if (regState.value !is RegState.Registered) return
        val want = desiredAffiliations()
        val have = synchronized(lock) { confirmedRosters.keys + pendingRosters.keys }
        val drop = have - want
        if (drop.isNotEmpty()) {
            // 해지는 편성에서 빠질 때만 정당하다 — 오해지는 로스터를 조용히 죽이므로 근거를 남긴다.
            Log.i(TAG, "syncRosterSubs 해지 대상=$drop (want=$want have=$have " +
                    "groups=${_groups.value.map { bareId(it.uri) }} " +
                    "joined=${channelStore?.joined} sel=${_selectedGroup.value})")
        }
        // 발행 여부 판단은 subscribeRoster 단일 지점에 맡긴다 — 미확인·확인대기 만료면 재발행한다.
        want.forEach { subscribeRoster(it, true) }
        drop.forEach { subscribeRoster(it, false) }
    }

    private fun publishRosters() {
        _channelRosters.value = synchronized(lock) { rosterMap.mapValues { it.value.toMap() } }
    }

    private fun sessionByCall(callId: Int): Session? =
        synchronized(lock) { sessionMap.values.firstOrNull { it.callId == callId } }

    private fun bindCall(groupId: String, callId: Int, active: Boolean = false) {
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
            // 주채널이 사라지면 남은 첫 세션을 주채널로 승격
            if (s.role == ChannelRole.PRIMARY) sessionMap.values.firstOrNull()?.role = ChannelRole.PRIMARY
            s.groupId
        }
        // 활성 통화가 모두 끝나면 VoIP 오디오 모드 해제(MODE_NORMAL 복원) + 장치 gain 원복
        if (synchronized(lock) { sessionMap.values.none { it.active } }) {
            audioRouter?.setInCall(false)
            sip.setDeviceAudioBoost(1f, 1f)
        }
        // 세션 종료 시점은 서버측 구독이 사라진 채 발견된 실측 지점이다(단말이 구독은 살아있다고
        // 믿는 동안 서버엔 없어 로스터가 죽는다). 확인을 무효화해 아래 sync 가 즉시 재확인하게 한다 —
        // 살아 있으면 in-dialog 갱신으로 흡수되므로 비용이 없고, 죽었으면 여기서 되살아난다.
        invalidateRosterConfirm(gid)
        syncRosterSubs()   // 이탈해도 편성 채널이면 구독 유지 — 인원수는 계속 보여야 한다
        _status.value = "[$gid] 그룹콜 종료"
        emit(PttEventKind.LEAVE, gid)
        publish()
    }

    /** 세션 스냅샷 발행 + 주채널 파생 상태(floor/speaker) 갱신. */
    private fun publish() {
        val list = synchronized(lock) { sessionMap.values.map { it.toState() } }
        _sessions.value = list
        val primary = list.firstOrNull { it.role == ChannelRole.PRIMARY }
        _floorState.value = primary?.floorState ?: FloorState.IDLE
        _speaker.value = primary?.speaker?.copy(groupId = null)
            ?: list.firstOrNull { it.audible && it.speaker != null }?.let { it.speaker!!.copy(groupId = it.groupId) }
    }

    private fun primarySession(): Session? =
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
        applyListenPolicy()
    }

    fun setListenPolicy(p: ListenPolicy) {
        _listenPolicy.value = p
        applyListenPolicy()
    }

    /** 듣기 정책 적용 — 비채널 그룹은 참여 유지하되 수신 음소거. 채널별 수신 음량도 재적용(미디어 재협상 시 리셋). */
    private fun applyListenPolicy() {
        val policy = _listenPolicy.value
        synchronized(lock) {
            for (s in sessionMap.values) {
                // 1:1(private)은 사용자가 명시적으로 건/받은 대화 — 채널 소음 제어용 듣기
                // 정책(CHANNELS_ONLY)의 대상이 아니다. 주채널 비점유(role=NONE)라도 항상 수신.
                val on = policy == ListenPolicy.ALL || s.role != ChannelRole.NONE || s.privatePeer
                if (s.audible != on) {
                    s.audible = on
                    if (s.callId >= 0) sip.setCallListen(s.callId, on)
                }
                if (s.callId >= 0 && s.active) sip.setCallRxLevel(s.callId, s.volume)
            }
        }
        publish()
    }

    /** 채널별 수신 음량(0~2, 1=원음) — conference bridge 유입 레벨. 영속 저장(리부팅 유지). */
    fun setChannelVolume(groupId: String, level: Float) {
        volumeStore?.set(groupId, level)
        synchronized(lock) {
            val s = sessionMap[groupId] ?: return
            s.volume = level
            if (s.callId >= 0 && s.active) sip.setCallRxLevel(s.callId, level)
        }
        publish()
    }

    /** 오디오 출력 라우팅(전역) — 스피커폰/수화기(PJSIP) 또는 이어폰([AUDIO_ROUTE_HEADSET]+[deviceId]).
     *  선택은 [routePrefs] 로 영속(리부팅/재기동 복원). */
    fun setAudioRoute(route: Int, deviceId: Int = -1) {
        _audioRoute.value = route
        _headsetId.value = deviceId
        routePrefs?.let { it.route = route; it.headsetId = deviceId }
        applyAudioRoute()
    }

    /** 출력 장치 소멸(이어폰/BT 해제) 복구 — 라우트 폴백 후 사운드 장치를 재오픈해 재생 트랙을
     *  재생성한다. 일부 단말(MF52/A15 실측)이 장치 소멸 순간 무전 트랙에 시스템 뮤트를 건 채
     *  해제하지 않아 무전이 무음이 되는 상태의 유일한 앱측 해제 수단(상세: [SipController.bounceSndDev]). */
    fun recoverFromDeviceLoss() = sip.bounceSndDev()

    /** 무전 장치 게인(스피커 출력/마이크 송신, ×1.0~×3.0) — 영속 + 통화 중이면 즉시 적용. */
    fun setAudioGain(spk: Float, mic: Float) {
        val s = spk.coerceIn(com.cims.ue.ptt.audio.AudioRoutePrefs.GAIN_MIN, com.cims.ue.ptt.audio.AudioRoutePrefs.GAIN_MAX)
        val m = mic.coerceIn(com.cims.ue.ptt.audio.AudioRoutePrefs.GAIN_MIN, com.cims.ue.ptt.audio.AudioRoutePrefs.GAIN_MAX)
        _spkGain.value = s
        _micGain.value = m
        routePrefs?.let { it.spkGain = s; it.micGain = m }
        if (synchronized(lock) { sessionMap.values.any { it.active } }) sip.setDeviceAudioBoost(s, m)
    }

    /** 현재 라우팅 적용/재적용 — 통화 성립(pjsua2 스트림 개방) 시에도 호출(라우팅 리셋 대비). */
    private fun applyAudioRoute() {
        if (_audioRoute.value == AUDIO_ROUTE_HEADSET) {
            sip.setAudioRoute(SipController.AUDIO_ROUTE_DEFAULT)   // pjsua2 강제 라우팅 해제
            audioRouter?.select(_headsetId.value)
        } else {
            audioRouter?.clear()
            sip.setAudioRoute(_audioRoute.value)
            // pjsua setOutputRoute 미지원 백엔드 대비 — AudioManager 직접 적용 병행
            audioRouter?.setSpeakerphone(_audioRoute.value == SipController.AUDIO_ROUTE_SPEAKER)
        }
        // 볼륨 인덱스는 장치별 — 적용 직후와 재라우팅이 가라앉은 뒤 현재 장치 축의 음량을 확보
        audioRouter?.ensureRxVolume()
        scope.launch { delay(800); audioRouter?.ensureRxVolume() }
    }

    /** 희망 affiliation 집합 — 편성 채널 전체(CSC 목록 + 영속 참여 채널 + 선택 채널). */
    private fun desiredAffiliations(): Set<String> = buildSet {
        _groups.value.forEach { add(bareId(it.uri)) }
        channelStore?.joined?.forEach { add(it) }
        _selectedGroup.value?.let { add(it) }
    }

    /** 서버 확정이 아직 신선한가 — 잔여 수명이 TTL 절반 미만이면 재발행 대상. */
    private fun affValid(groupId: String): Boolean =
        (affExpireAt[groupId] ?: 0L) - SystemClock.elapsedRealtime() > AFF_EXPIRES_SEC * 500L

    /** 확정이 없거나 낡았고, in-flight·백오프 대기 중도 아닐 때만 발행(트리거 중복 억제).
     *  백오프 확인이 없으면 주기 루프와 백오프 타이머가 각자 발사해 실패 상태에서 PUBLISH 가 겹친다. */
    private fun ensureAffiliated(groupId: String) {
        if (affValid(groupId)) return
        if (affPending.values.any { it.first == groupId && it.second }) return
        if ((affBackoffUntil[groupId] ?: 0L) > SystemClock.elapsedRealtime()) return
        affiliate(groupId, true)
    }

    /** 등록 상태에서 희망 집합 전체를 보장 — 등록 성공·그룹 목록 적재·주기 루프에서 호출. */
    private fun affiliateAll() {
        if (regState.value !is RegState.Registered) return
        desiredAffiliations().forEach { ensureAffiliated(it) }
    }

    // ── 그룹콜 참여/이탈 ──

    /**
     * 그룹콜 SDP 의 floor 섹션 (TS 24.380 §12.1) — offer(발신)·answer(착신 자동 수락) 공용.
     * `c=` 는 주입 시점에 `CimsCall.withConnLine` 이 세션 IP 로 채운다.
     *
     * **fmtp 협상**(§12.1.2.3, §6.3.5.4.4):
     * - `mc_queueing` — 큐잉 지원 선언. 미협상 멤버의 비선점 요청은 서버가 **Deny #1** 로 끊는다
     *   (CSP 도 INVITE 에 같은 속성을 광고한다). 단말은 Queue Position Info 를 소비한다.
     * - `mc_priority` 는 **싣지 않는다** — 협상하면 유효 우선순위가 그 값으로 clamp 되는데
     *   ([FloorCodec.request] 참조로 우선순위 필드 자체를 안 보내므로) 제어평면이 준 멤버
     *   우선순위를 그대로 쓰는 편이 항상 같거나 높다.
     * - `mc_granted`(호 성립 시 초기 발언권) 도 싣지 않는다 — 채널 참여는 발언 요청이 아니다.
     *   발언은 언제나 PTT down 의 Floor Request 로 시작한다.
     */
    private fun floorSdp(s: Session): String = floorSdp(s.floor.localPort, s.fullDuplex)

    /** floor 섹션 조립 — 세션 성립 전(착신 선바인드) 경로도 같은 문자열을 쓰도록 포트/모드만 받는다. */
    private fun floorSdp(localPort: Int, fullDuplex: Boolean): String =
        "m=application $localPort UDP MCPTT\r\n" +
            "a=floorid:0 mstrm:audio\r\n" +
            // 전이중 1:1 은 mc_no_floor_ctrl 로 floor 없는 세션을 협상한다(G17) — CSP 가
            // PTT_GROUP_ADD floor_control:"off" 로 변환.
            (if (fullDuplex) "a=fmtp:MCPTT mc_queueing;mc_no_floor_ctrl" else "a=fmtp:MCPTT mc_queueing")

    /** 키업 그룹콜 참여(발신). 이미 참여 중이면 무시. 첫 세션은 주채널.
     *  [emergency]=true 면 긴급 그룹콜로 개시(INVITE mcptt-info emergency-ind, TS 24.379). */
    fun joinGroupCall(
        groupId: String,
        members: List<McpttXml.ResourceEntry> = emptyList(),
        emergency: Boolean = false,
    ) {
        val s = synchronized(lock) {
            if (sessionMap.containsKey(groupId)) return
            Session(groupId).also {
                it.role = if (sessionMap.values.none { v -> v.role == ChannelRole.PRIMARY }) ChannelRole.PRIMARY
                else ChannelRole.NONE
                it.emergency = emergency
                it.emergencyMine = emergency
                sessionMap[groupId] = it
            }
        }
        // 애드혹 임시 그룹은 편성 자산이 아니다 — affiliation(사전 가입 없음)·채널 영속·
        //   로스터 구독(참가자는 in-dialog NOTIFY 폴백으로 수신)을 모두 건너뛴다.
        val adhoc = isAdhocId(groupId)
        if (!adhoc) {
            ensureAffiliated(groupId)
            channelStore?.let { st -> st.add(groupId); if (s.role == ChannelRole.PRIMARY) st.primary = groupId }
            if (s.role == ChannelRole.PRIMARY) _selectedGroup.value = groupId
        }
        val appSdp = floorSdp(s)
        val parts = ArrayList<SipBodyPart>()
        parts.add(SipBodyPart("application", "vnd.3gpp.mcptt-info+xml",
            McpttXml.mcpttInfo(McpttXml.SessionType.PREARRANGED, "tel:$groupId", mcpttId, "tel:$groupId",
                emergency = if (emergency) true else null)))
        if (members.isNotEmpty())
            parts.add(SipBodyPart("application", "resource-lists+xml", McpttXml.resourceLists(members)))
        sip.makeGroupCall(groupAor(groupId), parts, appSdp)
        if (!adhoc) subscribeRoster(groupId, true)
        _status.value = if (emergency) "🚨 긴급 그룹콜 개시 $groupId" else "그룹콜 참여 $groupId"
        emit(PttEventKind.JOIN, groupId)
        if (emergency) emit(PttEventKind.EMERGENCY, groupId)
        publish()
    }

    /** 애드혹 그룹통화 발신 (TS 22.179 Rel-18) — 편성 없이 즉석 멤버 지정. 임시 그룹 ID 로
     *  INVITE 에 resource-lists 멤버 명단을 실어 보내면 서버(CSP)가 비영속 임시 그룹을 합성해
     *  전원 fan-out 한다. 그룹은 통화 종료와 함께 소멸(양쪽 모두 ephemeral). */
    fun startAdhocCall(members: List<String>) {
        val me = bareId(mcpttId)
        val peers = members.map { bareId(it) }.filter { it.isNotBlank() && it != me }.distinct()
        if (peers.isEmpty()) { _status.value = "애드혹: 대상 없음"; return }
        // 사용자 단위 ad hoc 개시 인가 (프로파일) — 서버(403)가 최종 판정이나 UX 를 위해 선차단.
        if (_userProfile.value?.allowAdhocCall == false) {
            _status.value = "애드혹: 개시 권한 없음"
            feedback?.blocked("애드혹 개시 권한이 없습니다")
            return
        }
        // 임시 ID = adhoc-<발신자>-<epoch초> — 가입자 번호(숫자)·편성 그룹(접두사 예약)과 충돌 불가.
        val gid = "adhoc-${me.trimStart('+')}-${System.currentTimeMillis() / 1000}"
        joinGroupCall(gid, members = peers.map { McpttXml.ResourceEntry("tel:$it") })
        _status.value = "애드혹 그룹통화 개시 — ${peers.size}명"
    }

    /** 1:1 private call 발신 (TS 24.379 §11.1 on-demand — mcptt-info session-type=private).
     *  사전 그룹편성·affiliation 불요(서버가 멤버십 게이트 우회). 세션 키=[peer](상대 번호).
     *  [fullDuplex]=true 면 mc_no_floor_ctrl 을 협상해 floor 없는 전이중(마이크 상시 개방)으로
     *  연다 — 서버 PTT_GROUP_ADD floor_control:"off". false 면 2인 floor(반이중 무전) 세션. */
    fun startPrivateCall(peer: String, fullDuplex: Boolean = false) {
        val target = bareId(peer)
        if (target.isBlank() || target == bareId(mcpttId)) return
        val s = synchronized(lock) {
            if (sessionMap.containsKey(target)) return            // 이미 그 상대와 세션 중
            Session(target).also {
                it.privatePeer = true
                it.fullDuplex = fullDuplex
                // 1:1 은 주채널을 점유하지 않는다 — 활성 1:1 이 있는 동안 PTT 키가 1:1 에
                // 우선하는 규칙(talkSession)으로 발언을 라우팅하고, 끝나면 주채널로 복귀한다.
                it.role = ChannelRole.NONE
                sessionMap[target] = it
            }
        }
        // 편성 채널이 아니므로 channelStore/affiliation/roster 구독 없음 — 즉석 세션.
        val parts = listOf(SipBodyPart("application", "vnd.3gpp.mcptt-info+xml",
            McpttXml.mcpttInfo(McpttXml.SessionType.PRIVATE, "tel:$target", mcpttId, "tel:$target")))
        // C안: 멀티(mc_no_floor_ctrl 협상)여도 오디오는 반이중 규칙 — mic 은 PTT 게이트로만 연다.
        // 발신 call id 는 콜백으로 즉시 바인딩 — remote URI 파싱(bindCall) 의존을 없앤다
        // (미바인딩이면 PTT 게이트의 mic 결선이 스킵되어 TX 0 = 무음, 08-04 tcpdump 실측).
        sip.makeGroupCall(groupAor(target), parts, floorSdp(s)) { cid -> bindCall(target, cid) }
        _status.value = if (fullDuplex) "1:1 통화 발신 $target" else "1:1 무전 발신 $target"
        emit(PttEventKind.JOIN, target)
        publish()
    }

    /** 1:1 private call 착신 자동 수락 — 세션 키=발신자 번호(mcptt-calling-user-id).
     *  전이중(INVITE fmtp mc_no_floor_ctrl)이면 answer 에도 같은 fmtp 를 에코하고 마이크를
     *  상시 개방한다(auto commencement — 그룹콜 fan-out 과 동일한 즉시 연결 모델). */
    private fun autoJoinPrivateCall(inc: CallState.Incoming) {
        val peer = bareId(inc.callerId.ifBlank { inc.remote })
        if (peer.isBlank()) { releasePendingFloor(inc.id); return }
        val stale = synchronized(lock) {
            val old = sessionMap[peer]
            when {
                old == null -> null
                old.callId == inc.id -> return                    // 같은 호 재통지 — 인수 없이 유지
                else -> {
                    // 상대가 새 호를 걸었다 = 이전 1:1 은 끝난 것. 종료 경합(BYE 지연)으로
                    // 남은 세션을 정리하고 새 호를 받는다 — 방치하면 새 INVITE 가 무응답된다.
                    sessionMap.remove(peer)?.also { it.close() }
                }
            }
        }
        stale?.let { if (it.callId >= 0) sip.hangup(it.callId) }
        val pre = takePendingFloor(inc.id)
        val s = synchronized(lock) {
            if (sessionMap.containsKey(peer)) {                   // 경합 재확인
                pre?.let { runCatching { it.close() } }
                return
            }
            Session(peer, pre).also {
                it.callId = inc.id
                it.privatePeer = true
                it.fullDuplex = inc.noFloorCtrl
                it.role = ChannelRole.NONE                        // 1:1 은 주채널 비점유
                sessionMap[peer] = it
            }
        }
        sip.answerGroupCall(inc.id, floorSdp(s))
        _status.value = if (s.fullDuplex) "1:1 통화 수신: $peer" else "1:1 무전 수신: $peer"
        emit(PttEventKind.JOIN, peer)
        publish()
    }

    /** 그룹콜 착신 자동 수락 — 미참여 그룹이면 세션 생성 후 응답 SDP 에 m=application 주입.
     *  fan-out INVITE 의 emergency-ind → 긴급 표시 + 경고 톤. */
    private fun autoJoinGroupCall(inc: CallState.Incoming) {
        val groupId = bareId(inc.remote)
        if (groupId.isBlank()) { releasePendingFloor(inc.id); return }
        val pre = takePendingFloor(inc.id)
        val s = synchronized(lock) {
            if (sessionMap.containsKey(groupId)) {               // 이미 참여 중
                pre?.let { runCatching { it.close() } }
                return
            }
            Session(groupId, pre).also {
                it.callId = inc.id
                it.role = if (sessionMap.values.none { v -> v.role == ChannelRole.PRIMARY }) ChannelRole.PRIMARY
                else ChannelRole.NONE
                it.emergency = inc.emergency
                sessionMap[groupId] = it
            }
        }
        sip.answerGroupCall(inc.id, floorSdp(s))
        // 애드혹 임시 그룹 수신 — 편성 채널이 아니므로 영속/구독 제외 (발신측 joinGroupCall 과 대칭)
        if (!isAdhocId(groupId)) {
            subscribeRoster(groupId, true)
            channelStore?.let { st -> st.add(groupId); if (s.role == ChannelRole.PRIMARY) st.primary = groupId }
            if (s.role == ChannelRole.PRIMARY) _selectedGroup.value = groupId
        }
        if (inc.emergency) feedback?.emergencyTone()
        _status.value = if (inc.emergency) "🚨 긴급 그룹콜 자동 참여: $groupId" else "그룹콜 자동 참여: $groupId"
        emit(PttEventKind.JOIN, groupId)
        if (inc.emergency) emit(PttEventKind.EMERGENCY_IN, groupId)
        publish()
    }

    /** 그룹별 나가기. */
    fun leaveGroup(groupId: String) {
        channelStore?.remove(groupId)                 // 명시적 이탈 = 재조인 의도 해제
        val callId = synchronized(lock) { sessionMap[groupId]?.callId ?: return }
        if (callId >= 0) sip.hangup(callId) else {
            synchronized(lock) { sessionMap.remove(groupId)?.close() }
            invalidateRosterConfirm(groupId)
            syncRosterSubs()   // 편성 채널이면 구독 유지 (인원수 표시 계속)
            publish()
        }
    }

    /** RFC 4575 conference-info 파싱 — [groupId] 의 접속 인원 갱신.
     *
     *  참여 중인 채널이면 세션 participants 도 같은 값으로 맞춘다. 참여하지 않은 채널은
     *  rosterMap 에만 남아 목록 화면의 인원수로 쓰인다.
     *  ⚠️"본인은 항상 접속"은 **참여 중일 때만** 성립한다 — 미조인 채널에 자신을 넣으면
     *  참여하지도 않은 채널에 내가 있는 것으로 보인다. */
    private fun onConferenceInfo(groupId: String, xml: String) {
        val full = Regex("<conference-info\\b[^>]*state=\"full\"").containsMatchIn(xml)
        val userRe = Regex("<user\\b[^>]*entity=\"([^\"]+)\"[^>]*>(.*?)</user>", RegexOption.DOT_MATCHES_ALL)
        val stRe = Regex("<status>\\s*([A-Za-z-]+)\\s*</status>")
        synchronized(lock) {
            val s = sessionMap[groupId]
            val base = if (full) mutableMapOf() else (s?.participants ?: rosterMap[groupId]?.toMutableMap()
                ?: mutableMapOf())
            for (m in userRe.findAll(xml)) {
                val id = bareId(m.groupValues[1])
                if (id.isBlank()) continue
                val status = stRe.find(m.groupValues[2])?.groupValues?.get(1) ?: "connected"
                if (status.equals("disconnected", ignoreCase = true)) base.remove(id) else base[id] = status
            }
            if (s != null) base[bareId(mcpttId)] = "connected"   // 참여 중이면 본인은 항상 포함
            s?.participants = base
            rosterMap[groupId] = base.toMap()
        }
        publishRosters()
        publish()
    }

    // ── CSC (선택) ──

    fun login(userName: String, password: String) = scope.launch {
        val c = csc ?: run { _status.value = "CSC 미설정"; return@launch }
        runCatching { withContext(Dispatchers.IO) { c.authenticate(userName, password) } }
            .onSuccess { token = it; _status.value = "CSC 인증 성공" }
            .onFailure { _status.value = "CSC 인증 실패: ${it.message}" }
    }

    /** SSO: CIMS 공유 계정에서 받은 MCPTT(TS 33.180) access_token 직접 적용 — 수동 CSC 로그인 대체. */
    fun setAccessToken(accessToken: String) {
        token = TokenSet(accessToken = accessToken, tokenType = "Bearer",
                         refreshToken = null, idToken = null, expiresInSec = 3600, scope = null)
        _status.value = "CIMS 계정 토큰 적용"
        loadGroups()
    }

    fun loadGroups() = scope.launch {
        val c = csc ?: return@launch
        val t = token?.accessToken ?: run { _status.value = "토큰 없음"; return@launch }
        runCatching { withContext(Dispatchers.IO) { c.listGroups(t, mcpttId) } }
            .onSuccess { list ->
                _groups.value = list
                // 선택 그룹(TS 24.484 currently-selected group) 복원 — 마지막 주채널 우선,
                // 이력이 없거나 편성에서 빠졌으면 목록 첫 그룹(최초 1회 폴백).
                if (_selectedGroup.value == null) {
                    val ids = list.map { bareId(it.uri) }
                    _selectedGroup.value =
                        channelStore?.lastPrimary?.takeIf { it in ids } ?: ids.firstOrNull()
                }
                affiliateAll()   // 편성 채널 전체 affiliation (등록 전이면 등록 완료 트리거가 수행)
                syncRosterSubs() // 목록이 채워졌으니 로스터 구독도 그 집합으로 맞춘다
                _status.value = "그룹 ${list.size}개"
            }
            .onFailure { _status.value = "그룹 조회 실패: ${it.message}" }
        loadUserProfile()
    }

    /** 사용자 MCPTT 프로파일 (TS 24.484) — SOS 대상 결정 모드·전용 긴급그룹·개시 인가. */
    data class UserProfile(
        val emergencyGroupMode: String,   // DedicatedGroup | UseCurrentlySelectedGroup
        val emergencyGroupId: String?,    // 전용 긴급그룹 (bare id, DedicatedGroup 모드 대상)
        val allowEmergencyCall: Boolean,
        val allowEmergencyAlert: Boolean,
        val allowAdhocCall: Boolean,
    )

    private val _userProfile = MutableStateFlow<UserProfile?>(null)
    val userProfile: StateFlow<UserProfile?> = _userProfile.asStateFlow()

    /** user-profile 문서 조회 — 실패해도 치명 아님(서버 게이트가 최종 판정, 앱은 현행 동작 유지). */
    fun loadUserProfile() = scope.launch {
        val c = csc ?: return@launch
        val t = token?.accessToken ?: return@launch
        runCatching { withContext(Dispatchers.IO) { c.getUserProfile(t, mcpttId) } }
            .onSuccess { doc -> doc.body?.let { _userProfile.value = parseUserProfile(it) } }
            .onFailure { Log.d(TAG, "user-profile 조회 실패(프로파일 없이 동작): ${it.message}") }
    }

    private fun parseUserProfile(xml: String): UserProfile {
        val giBlock = Regex("<MCPTTGroupInitiation>(.*?)</MCPTTGroupInitiation>", RegexOption.DOT_MATCHES_ALL)
            .find(xml)?.groupValues?.get(1) ?: ""
        val mode = Regex("entry-info=\"([^\"]+)\"").find(giBlock)?.groupValues?.get(1) ?: "DedicatedGroup"
        val egid = Regex("<uri-entry>([^<]+)</uri-entry>").find(giBlock)?.groupValues?.get(1)
            ?.let { bareId(it) }?.takeIf { it.isNotBlank() }
        fun flag(tag: String) =
            Regex("<$tag>\\s*(true|false)\\s*</$tag>").find(xml)?.groupValues?.get(1)?.toBoolean() ?: true
        return UserProfile(
            emergencyGroupMode = mode,
            emergencyGroupId = egid,
            allowEmergencyCall = flag("allow-emergency-group-call"),
            allowEmergencyAlert = flag("allow-activate-emergency-alert"),
            allowAdhocCall = flag("cims:allow-adhoc-group-call"),
        )
    }

    /** 그룹 문서(TS 24.481, GMS XCAP) 조회 — 채널 상세 진입 시 호출. ETag(If-None-Match) 캐시. */
    fun loadGroupDetail(groupId: String) = scope.launch {
        val c = csc ?: return@launch
        val t = token?.accessToken ?: return@launch
        val uri = _groups.value.firstOrNull { bareId(it.uri) == groupId }?.uri ?: "tel:$groupId"
        val cached = _groupDocs.value[groupId]
        runCatching { withContext(Dispatchers.IO) { c.getGroupDoc(t, mcpttId, uri, cached?.etag) } }
            .onSuccess { doc ->
                if (!doc.notModified) doc.body?.let { body ->
                    _groupDocs.value = _groupDocs.value + (groupId to GroupDoc.parse(uri, body, doc.etag))
                }
            }
            .onFailure { Log.d(TAG, "group doc $groupId 조회 실패: ${it.message}") }
    }

    fun selectGroup(groupId: String) {
        _selectedGroup.value = groupId
        if (regState.value is RegState.Registered) ensureAffiliated(groupId)
    }

    // ── SIP ──

    fun register() = sip.register()

    /** affiliation PUBLISH (TS 24.379 §9). on=false → de-affiliate(Expires:0).
     *  성공 여부는 [affiliated] 에 낙관 기록하지 않는다 — token 상관 응답(2xx)에서만 확정.
     *  보유 ETag 가 있으면 `SIP-If-Match` 를 실어 **갱신(refresh)** 으로 처리되게 한다(RFC 3903 §4) —
     *  없으면 매 발행이 초기 publication 이라 서버가 event state 를 새로 만든다. */
    fun affiliate(groupId: String, on: Boolean = true) {
        val token = affSeq.getAndIncrement()
        affPending[token] = groupId to on
        val groupSip = "sip:$groupId@${sipConfig.domain}"
        val hdrs = mutableMapOf(
            // Event: mcptt 필수 — TS 24.379 §9 (없으면 CSP 가 489 Bad Event 거부)
            "Event" to "mcptt",
            "Expires" to if (on) "$AFF_EXPIRES_SEC" else "0",
        )
        affEtag[groupId]?.let { hdrs["SIP-If-Match"] = it }
        sip.sendRequest(
            method = "PUBLISH",
            targetUri = groupSip,
            contentType = McpttXml.CT_AFFILIATION,
            body = McpttXml.affiliationCommand("tel:$groupId", on),
            headers = hdrs,
            token = token,
        )
        // 응답이 영영 없는 경우(계정 미생성·전송 실패) pending 이 남아 재발행을 막는 것 방지 —
        // PUBLISH 트랜잭션 타임아웃(Timer B ≈32s)보다 넉넉히 기다린 뒤 회수(주기 루프가 재시도).
        scope.launch {
            delay(40_000)
            if (affPending.remove(token) != null)
                Log.w(TAG, "affiliate $groupId 응답 없음(타임아웃) — 주기 갱신 루프가 재시도")
        }
        _status.value = if (on) "affiliate $groupId" else "de-affiliate $groupId"
    }

    /**
     * 그룹 문자 발신 — MCData 그룹 SDS (TS 24.282 §9.2.2). multipart/mixed 본문
     * (mcdata-info + SDS SIGNALLING PAYLOAD + DATA PAYLOAD)으로 CSP(MCDATA-AS)가
     * 게이트(allow-SDS·멤버십·크기) 후 affiliate 멤버에게 fan-out. 로컬 저장은 서비스 몫.
     *
     * payload 가 프로비저닝 임계([SipAccountConfig.maxPayloadSdsCplaneBytes], 0=무제한)를
     * 초과하면 C-plane 대신 **MSRP 미디어평면**(§9.2.3)으로 발신한다 — 초과 MESSAGE 는
     * 서버가 403+Warning 203 으로 거절하는 표준 동작.
     * @return message ID (UUID hex 32자, delivered 통지 대사용) — 빈 문자열이면 미발신
     */
    fun sendGroupMessage(groupId: String, text: String): String {
        if (text.isBlank()) return ""
        val convId = McDataCodec.conversationIdOf(groupId)
        val msgId = McDataCodec.newMessageId()
        if (willUseMsrp(text)) {
            scope.launch { sendGroupMessageMsrp(groupId, text, convId, msgId) }
            return msgId
        }
        val (ct, body) = McDataCodec.buildGroupSds(
            groupUri = "tel:$groupId", text = text, convId = convId, msgId = msgId,
        )
        sip.sendRequest(
            method = "MESSAGE",
            targetUri = "sip:$groupId@${sipConfig.domain}",
            contentType = ct,
            body = body,
        )
        return msgId
    }

    // ── MCData MSRP 미디어평면 송신 (TS 24.282 §9.2.3) ──

    private val msrpMutex = Mutex()   // MSRP 발신 직렬화 — msrpEvents 의 호 대응 모호성 제거

    /**
     * 대용량 SDS 를 MSRP 미디어평면으로 발신 — INVITE(더미 오디오+m=message) → 200 OK 의
     * 서버 a=path 로 TCP 접속 → SIGNALLING/PAYLOAD TLV SEND → 서버 BYE(완료 신호).
     * 서버(cmdp)가 종단 저장 후 하이브리드 fan-out(MSRP 수신 단말=INVITE, 그 외=FILEURL 폴백).
     */
    private suspend fun sendGroupMessageMsrp(
        groupId: String,
        text: String,
        convId: String,
        msgId: String,
    ): Unit = msrpMutex.withLock {
        val sessionId = UUID.randomUUID().toString().replace("-", "").take(12)
        val localIp = withContext(Dispatchers.IO) { localIpFor(sipConfig.serverHost) }
            ?: run {
                _status.value = "[$groupId] MSRP 발신 실패: 로컬 IP 확인 불가"
                _sendResult.tryEmit(msgId to false)
                return
            }
        // a=path 포트는 광고용(리슨 안 함) — 서버가 항상 passive, 단말이 out-connect(NAT)
        val localPath = "msrp://$localIp:2855/$sessionId;tcp"
        val msrpSdp = listOf(
            "m=message 2855 TCP/MSRP *",
            "a=path:$localPath",
            "a=accept-types:${MsrpCodec.ACCEPT_TYPES}",
            "a=setup:actpass",
            "a=sendonly",
        ).joinToString("\r\n")

        // 이벤트 수집을 INVITE 발신 전에 개시(UNDISPATCHED — 구독 등록 후 재개, 유실 방지)
        val events = Channel<MsrpEvent>(Channel.UNLIMITED)
        val collector = scope.launch(start = CoroutineStart.UNDISPATCHED) {
            sip.msrpEvents.collect { events.trySend(it) }
        }
        var callId = -1
        try {
            sip.makeMsrpInvite(
                targetUri = "sip:$groupId@${sipConfig.domain}",
                msrpSdp = msrpSdp,
                headers = mapOf(
                    "Accept-Contact" to "*;+g.3gpp.icsi-ref=\"$MCDATA_ICSI\";require;explicit",
                    "P-Preferred-Service" to "urn:urn-7:3gpp-service.ims.icsi.mcdata.sds",
                ),
            )
            var serverPath: String? = null
            withTimeoutOrNull(MSRP_INVITE_TIMEOUT_MS) {
                for (ev in events) {
                    when (ev) {
                        is MsrpEvent.Started -> callId = ev.callId
                        is MsrpEvent.PathReady -> if (callId < 0 || ev.callId == callId) {
                            serverPath = ev.path; return@withTimeoutOrNull
                        }
                        is MsrpEvent.Closed -> if (callId >= 0 && ev.callId == callId) {
                            Log.w(TAG, "MSRP INVITE 거절: ${ev.code} ${ev.reason}")
                            return@withTimeoutOrNull
                        }
                        else -> Unit    // Incoming/Answered = 수신 레그 이벤트
                    }
                }
            }
            val path = serverPath ?: run {
                _status.value = "[$groupId] MSRP 발신 실패: 서버 응답 없음"
                if (callId >= 0) sip.hangup(callId)
                _sendResult.tryEmit(msgId to false)
                return
            }

            // 진행률 확인용 디버그 감속 — adb 로만 켬(릴리스 무영향):
            //   setprop debug.cims.msrp.slow 300   (청크 사이 ms, 0=끔)
            //   setprop debug.cims.msrp.chunk 256  (청크 크기 축소 — 청크 수 증가)
            val slowMs = debugProp("debug.cims.msrp.slow")
            val chunk = debugProp("debug.cims.msrp.chunk").takeIf { it in 64..65536 } ?: 16 * 1024
            val sig = McDataCodec.buildSdsSignallingTlv(convId, msgId)
            val payload = McDataCodec.buildSdsPayloadTlv(text)
            val total = sig.size + payload.size
            val sent = withContext(Dispatchers.IO) {
                runCatching {
                    MsrpSession(path, localPath, chunkSize = chunk).use { sess ->
                        sess.connect()
                        sess.sendMessage("s$sessionId", McDataCodec.CT_SIGNALLING, sig,
                            onProgress = { _sendProgress.tryEmit(SendProgress(msgId, it, total)) },
                            chunkDelayMs = slowMs.toLong()) &&
                            sess.sendMessage("p$sessionId", McDataCodec.CT_PAYLOAD, payload,
                                successReport = true,
                                onProgress = { _sendProgress.tryEmit(SendProgress(msgId, sig.size + it, total)) },
                                chunkDelayMs = slowMs.toLong())
                    }
                }.onFailure { Log.w(TAG, "MSRP 전송 실패: ${it.message}") }.getOrDefault(false)
            }
            if (!sent) {
                _status.value = "[$groupId] MSRP 전송 실패"
                if (callId >= 0) sip.hangup(callId)
                _sendResult.tryEmit(msgId to false)
                return
            }
            _sendResult.tryEmit(msgId to true)
            // 서버가 수신 완료 시 BYE 로 정리 — 미도착이면 우리가 hangup
            val closed = withTimeoutOrNull(MSRP_BYE_TIMEOUT_MS) {
                for (ev in events) if (ev is MsrpEvent.Closed && ev.callId == callId) return@withTimeoutOrNull true
                false
            }
            if (closed != true && callId >= 0) sip.hangup(callId)
            _status.value = "[$groupId] 대용량 문자 전송 완료 (${McDataCodec.sdsPayloadSize(text)}B)"
        } finally {
            collector.cancel()
            events.close()
        }
    }

    /** `debug.cims.*` 시스템 속성(int) — adb `setprop` 으로 켜는 시험용 노브. 실패/미설정=0. */
    private fun debugProp(name: String): Int = runCatching {
        val cls = Class.forName("android.os.SystemProperties")
        (cls.getMethod("getInt", String::class.java, Int::class.javaPrimitiveType)
            .invoke(null, name, 0) as Int)
    }.getOrDefault(0)

    /** [host] 로 나가는 기본 로컬 IP — MSRP a=path 광고용(UDP connect 트릭, 실송신 없음). */
    private fun localIpFor(host: String): String? = runCatching {
        java.net.DatagramSocket().use { s ->
            s.connect(java.net.InetAddress.getByName(host), 9)
            s.localAddress.hostAddress
        }
    }.getOrNull()

    /**
     * 서버발 MSRP 배포 INVITE 처리 — 200 answer(m=message 교체, a=setup:active/recvonly) →
     * 서버 a=path 로 TCP out-connect → 조립 수신 → [incomingSds] 방출. 그룹/발신자는
     * INVITE 의 mcdata-info(request-uri/calling-user-id), 폴백=From(그룹).
     */
    private suspend fun handleIncomingMsrp(ev: MsrpEvent.Incoming) {
        val serverPath = Regex("a=path:(\\S+)").find(ev.inviteMsg)?.groupValues?.get(1) ?: run {
            Log.w(TAG, "MSRP INVITE 에 a=path 없음 — 거절")
            sip.rejectMsrpCall(ev.callId)
            return
        }
        val (groupUri, callingUser) = McDataCodec.parseInfoUris(ev.inviteMsg)
        val groupId = groupUri?.let { bareId(it) }?.takeUnless { it.isBlank() } ?: bareId(ev.remote)
        val sender = callingUser?.let { bareId(it) }?.takeUnless { it.isBlank() } ?: groupId

        val localIp = withContext(Dispatchers.IO) { localIpFor(sipConfig.serverHost) } ?: run {
            sip.rejectMsrpCall(ev.callId)
            return
        }
        val sessionId = UUID.randomUUID().toString().replace("-", "").take(12)
        val localPath = "msrp://$localIp:2855/$sessionId;tcp"
        // 완전한 answer SDP (answer_with_sdp) — 오퍼와 같은 m-line 순서:
        // 더미 오디오(오퍼 payload 에코, inactive) + m=message(active/recvonly)
        val offeredAudio = Regex("m=audio \\d+ RTP/AVP ([0-9 ]+)")
            .find(ev.inviteMsg)?.groupValues?.get(1)?.trim() ?: "0 8"
        val answerSdp = listOf(
            "v=0",
            "o=- 3 3 IN IP4 $localIp",
            "s=-",
            "c=IN IP4 $localIp",
            "t=0 0",
            "m=audio 9 RTP/AVP $offeredAudio",
            "a=inactive",
            "m=message 2855 TCP/MSRP *",
            "a=path:$localPath",
            "a=accept-types:${MsrpCodec.ACCEPT_TYPES}",
            "a=setup:active",
            "a=recvonly",
            "",
        ).joinToString("\r\n")

        val events = Channel<MsrpEvent>(Channel.UNLIMITED)
        val collector = scope.launch(start = CoroutineStart.UNDISPATCHED) {
            sip.msrpEvents.collect { events.trySend(it) }
        }
        try {
            sip.acceptMsrpCall(ev.callId, answerSdp)
            val answered = withTimeoutOrNull(MSRP_INVITE_TIMEOUT_MS) {
                for (e in events) {
                    if (e is MsrpEvent.Answered && e.callId == ev.callId) return@withTimeoutOrNull true
                    if (e is MsrpEvent.Closed && e.callId == ev.callId) return@withTimeoutOrNull false
                }
                false
            }
            if (answered != true) {
                Log.w(TAG, "MSRP 수신: answer 실패/종료 (group=$groupId)")
                sip.rejectMsrpCall(ev.callId)
                return
            }

            val received = withContext(Dispatchers.IO) {
                runCatching {
                    MsrpSession(serverPath, localPath).use { sess ->
                        sess.connect()
                        sess.receiveMessage()
                    }
                }.onFailure { Log.w(TAG, "MSRP 수신 실패: ${it.message}") }.getOrNull()
            }
            if (received == null) {
                sip.rejectMsrpCall(ev.callId)
                return
            }
            val (ct, body) = received
            // raw 바이너리 파트 보존 위해 ISO_8859_1 (McDataCodec Part.bytes 가 동일 인코딩으로 복원)
            when (val p = McDataCodec.parse(ct, String(body, Charsets.ISO_8859_1))) {
                is McDataCodec.SdsMessage -> {
                    _incomingSds.tryEmit(MediaSds(groupId, sender, p))
                    _status.value = "[$groupId] 대용량 문자 수신 (${body.size}B)"
                }
                else -> Log.w(TAG, "MSRP 수신 본문 파싱 실패/비 SDS (ct=$ct, ${body.size}B)")
            }
            // 서버가 SEND_RESULT 후 BYE — 미도착 시 우리가 정리
            val closed = withTimeoutOrNull(MSRP_BYE_TIMEOUT_MS) {
                for (e in events) if (e is MsrpEvent.Closed && e.callId == ev.callId) return@withTimeoutOrNull true
                false
            }
            if (closed != true) sip.hangup(ev.callId)
        } finally {
            collector.cancel()
            events.close()
        }
    }

    /** 실패 문자 재전송 — 같은 msgId 재사용(수신측 중복 대사는 msgId 기준). 결과는 [sendResult]. */
    fun resendGroupMessage(groupId: String, text: String, msgId: String) {
        if (text.isBlank() || msgId.isBlank()) return
        val convId = McDataCodec.conversationIdOf(groupId)
        if (willUseMsrp(text)) {
            scope.launch { sendGroupMessageMsrp(groupId, text, convId, msgId) }
        } else {
            val (ct, body) = McDataCodec.buildGroupSds(
                groupUri = "tel:$groupId", text = text, convId = convId, msgId = msgId,
            )
            sip.sendRequest("MESSAGE", "sip:$groupId@${sipConfig.domain}", ct, body)
            _sendResult.tryEmit(msgId to true)      // C-plane 은 종전대로 fire-and-forget
        }
    }

    /** FD 전송 결과 — 로컬 이력 저장용. */
    data class FdSent(val msgId: String, val url: String, val size: Long)

    /**
     * 그룹 파일전송 — FD via HTTP (TS 23.282): CSC 콘텐츠 서버 업로드 → FD SIGNALLING
     * PAYLOAD(SIP MESSAGE) 전파. 블로킹(업로드 HTTP) — Dispatchers.IO 에서 호출할 것.
     * @return null 이면 실패 (토큰 없음/업로드 거부 — allow_fd·크기 게이트는 서버가 판정)
     */
    fun sendGroupAttachment(groupId: String, data: ByteArray, fileName: String, mime: String): FdSent? {
        val c = csc ?: return null
        val t = token?.accessToken ?: run { _status.value = "첨부: 토큰 없음"; return null }
        val up = runCatching { c.uploadFd(t, data, fileName, mime, groupId) }
            .onFailure { Log.w(TAG, "FD 업로드 실패: ${it.message}"); _status.value = "첨부 업로드 실패" }
            .getOrNull() ?: return null
        val convId = McDataCodec.conversationIdOf(groupId)
        val msgId = McDataCodec.newMessageId()
        val (ct, body) = McDataCodec.buildGroupFd(
            groupUri = "tel:$groupId", fileUrl = up.url, fileName = up.name,
            fileSize = up.size, mime = mime, convId = convId, msgId = msgId,
        )
        sip.sendRequest(
            method = "MESSAGE",
            targetUri = "sip:$groupId@${sipConfig.domain}",
            contentType = ct,
            body = body,
        )
        return FdSent(msgId, up.url, up.size)
    }

    /** FD 첨부 다운로드 — 블로킹, Dispatchers.IO 에서 호출. */
    fun downloadAttachment(url: String): ByteArray? {
        val c = csc ?: return null
        val t = token?.accessToken ?: return null
        return runCatching { c.downloadFd(t, url) }
            .onFailure { Log.w(TAG, "FD 다운로드 실패: ${it.message}") }
            .getOrNull()
    }

    /** SDS disposition 통지(TS 24.282 §12.2) — 수신 메시지의 원 발신자에게 1:1 전송. */
    fun sendSdsNotification(peerId: String, convId: String, msgId: String, notifType: Int) {
        val (ct, body) = McDataCodec.buildNotification(convId, msgId, notifType)
        sip.sendRequest(
            method = "MESSAGE",
            targetUri = "sip:$peerId@${sipConfig.domain}",
            contentType = ct,
            body = body,
        )
    }

    // ── 긴급(SOS) — TS 24.379 in-call emergency ──

    /**
     * 긴급(SOS) 개시 — 하드웨어 SOS 키/화면 SOS 버튼.
     * 주채널 통화 중이면 re-INVITE(mcptt-info emergency-ind=true)로 상향, 미참여면 긴급 그룹콜 발신.
     * 새 긴급콜 대상은 프로파일 entry-info(TS 24.484)가 결정: DedicatedGroup=전용 긴급그룹,
     * UseCurrentlySelectedGroup=선택 그룹(마지막 주채널). 서버가 미인가로 403 거절하면
     * normal 재발신 폴백(개시)·latch 복원(상향)한다.
     */
    fun startEmergency() {
        val s = primarySession()
        if (s == null) {
            val gid = emergencyTargetGroup() ?: return
            sendAlert(gid, true)   // 규격 시퀀스: 경보 먼저(말 못 해도 신원·그룹은 전파), 통화 다음
            joinGroupCall(gid, emergency = true)
            feedback?.emergencyTone()
            return
        }
        if (s.emergency) { _status.value = "[${s.groupId}] 이미 긴급 상태"; return }
        s.emergency = true
        s.emergencyMine = true
        sendAlert(s.groupId, true)
        if (s.callId >= 0) sendConditionReinvite(s, emergency = true)
        feedback?.emergencyTone()
        _status.value = "🚨 [${s.groupId}] 긴급 개시"
        emit(PttEventKind.EMERGENCY, s.groupId)
        publish()
    }

    /** SOS 새 긴급콜 대상 결정 (TS 24.484 MCPTTGroupInitiation entry-info).
     *  DedicatedGroup(기본)=프로비저닝된 전용 긴급그룹 — 미지정이면 불발(서버도 미인가 403).
     *  UseCurrentlySelectedGroup=선택 그룹(마지막 주채널). 프로파일 미수신이면 선택 그룹으로
     *  현행 동작 유지(서버 게이트가 최종 판정). null 반환 시 상태 메시지는 이미 표시됨. */
    private fun emergencyTargetGroup(): String? {
        val p = _userProfile.value
        if (p != null && p.emergencyGroupMode == "DedicatedGroup") {
            return p.emergencyGroupId ?: run {
                _status.value = "긴급: 전용 긴급그룹 미지정 — 관리자에게 문의"
                feedback?.blocked("긴급 불가: 전용 긴급그룹 미지정 — 관리자에게 문의")
                null
            }
        }
        return _selectedGroup.value ?: run {
            _status.value = "긴급: 대상 그룹 없음"
            feedback?.blocked("긴급 불가: 대상 그룹 없음")
            null
        }
    }

    /** 미인가 긴급콜 403 (TS 24.379 §6.3.3.1.14) — 성립 전 거절된 긴급 개시를 normal 재발신으로
     *  폴백한다. 긴급이 아닌 403(스캐너 차단 등)은 비대상. [onCallEnded] 전에 호출되어야 세션
     *  플래그를 볼 수 있다. */
    private fun handleEmergencyDenied(callId: Int, code: Int) {
        if (code != 403) return
        val gid = synchronized(lock) {
            sessionMap.values.firstOrNull { it.callId == callId && it.emergency && it.emergencyMine }?.groupId
        } ?: return
        if (isAdhocId(gid)) return
        _status.value = "[$gid] 긴급 미인가 — 일반 통화로 전환"
        scope.launch {
            delay(300)             // 거절 세션 teardown(onCallEnded) 정리 후 재발신
            joinGroupCall(gid)
        }
    }

    /** 긴급 해제 — 개시자만 유효(서버는 비개시자의 취소 re-INVITE 를 무시, TS 24.379). */
    fun cancelEmergency() {
        val s = synchronized(lock) { sessionMap.values.firstOrNull { it.emergency && it.emergencyMine } }
        if (s == null) {
            // 호 성립 전 SOS(경보만 나감)·호 실패 잔존 — 내 경보만이라도 취소한다.
            val mine = _alerts.value.firstOrNull { it.mine }
                ?: run { _status.value = "해제할 긴급 없음(개시자만 해제 가능)"; return }
            sendAlert(mine.groupId, false)
            _status.value = "[${mine.groupId}] 긴급경보 해제"
            publish()
            return
        }
        s.emergency = false
        s.emergencyMine = false
        sendAlert(s.groupId, false)
        if (s.callId >= 0) sendConditionReinvite(s, emergency = false)
        _status.value = "[${s.groupId}] 긴급 해제"
        emit(PttEventKind.EMERGENCY_END, s.groupId)
        publish()
    }

    /** 긴급경보 MESSAGE 발신/취소 — SOS 개시/해제와 한 쌍 (TS 24.379 emergency alert).
     *  통화(INVITE)와 독립 경로라 호 성립 여부와 무관하게 신원·그룹이 전파된다. */
    private fun sendAlert(groupId: String, activate: Boolean) {
        runCatching {
            sip.sendRequest(
                method = "MESSAGE",
                targetUri = "sip:$groupId@${sipConfig.domain}",
                contentType = McpttXml.CT_MCPTT_INFO,
                body = McpttXml.alertInfo("tel:$groupId", mcpttId, activate),
            )
        }.onFailure { Log.w(TAG, "긴급경보 ${if (activate) "발신" else "취소"} 실패: ${it.message}") }
        val me = bareId(mcpttId)
        if (activate) {
            addAlert(ActiveAlert(groupId, me, System.currentTimeMillis(), mine = true))
            emit(PttEventKind.ALERT, groupId)
        } else {
            removeAlert(groupId, me)
            emit(PttEventKind.ALERT_END, groupId)
        }
    }

    private fun addAlert(a: ActiveAlert) {
        _alerts.value = _alerts.value.filterNot { it.groupId == a.groupId && it.userId == a.userId } + a
    }

    private fun removeAlert(groupId: String, userId: String) {
        _alerts.value = _alerts.value.filterNot { it.groupId == groupId && it.userId == userId }
    }

    /** 수신 경보 배너 수동 닫기 — 이 단말의 표시만 제거(발신측 경보 상태와 무관). */
    fun dismissAlert(groupId: String, userId: String) = removeAlert(groupId, userId)

    /** 수신측 세션 긴급 배너 수동 닫기 — 이 단말의 표시 latch 만 해제(개시자 상태와 무관).
     *  경보 취소 MESSAGE 유실 시의 탈출구 — 개시자 자신은 cancelEmergency 로만 해제한다. */
    fun dismissEmergency(groupId: String) {
        synchronized(lock) {
            sessionMap[groupId]?.takeIf { it.emergency && !it.emergencyMine }?.emergency = false
        }
        publish()
    }

    /** 수신 긴급경보 MESSAGE — 서버 fan-out 은 원본 본문 그대로라 그룹·발신자를 본문에서 읽는다. */
    private fun onAlertMessage(fromUri: String, body: String) {
        val info = McpttXml.parseMcpttInfo(body)
        val activate = info.alertInd ?: return   // alert-ind 없는 mcptt-info 는 경보가 아니다
        val gid = info.requestUri?.let { bareId(it) }?.takeIf { it.isNotBlank() } ?: return
        val user = bareId(info.callingUserId ?: fromUri)
        if (user == bareId(mcpttId)) return      // 내 발신 에코(서버는 발신자 제외 — 방어)
        if (activate) {
            addAlert(ActiveAlert(gid, user, System.currentTimeMillis(), mine = false))
            feedback?.emergencyTone()
            _status.value = "🚨 [$gid] 긴급경보 수신 — $user"
            emit(PttEventKind.ALERT_IN, gid, peer = user)
        } else {
            removeAlert(gid, user)
            // 세션 긴급 표시 un-latch — CSP 는 하향 re-INVITE 를 멤버에 전파하지 않으므로
            // (mcptt_emergency_modes.md 로드맵) 경보 취소가 멤버에 닿는 유일한 해제 신호다.
            // 같은 그룹에 다른 활성 경보가 남아 있으면 유지한다.
            if (_alerts.value.none { it.groupId == gid }) {
                synchronized(lock) {
                    sessionMap[gid]?.takeIf { it.emergency && !it.emergencyMine }?.emergency = false
                }
                publish()
            }
            _status.value = "[$gid] 긴급경보 해제 — $user"
            emit(PttEventKind.ALERT_END, gid, peer = user)
        }
    }

    /** in-dialog re-INVITE 로 긴급 상태 상향/하향 광고 — CSP ApplyInCallCondition 경로. */
    private fun sendConditionReinvite(s: Session, emergency: Boolean) {
        sip.reinviteWithBody(s.callId, listOf(
            SipBodyPart("application", "vnd.3gpp.mcptt-info+xml",
                McpttXml.mcpttInfo(McpttXml.SessionType.PREARRANGED, "tel:${s.groupId}", mcpttId,
                    "tel:${s.groupId}", emergency = emergency)),
        ))
    }

    // ── PTT 버튼 (주채널 전용) ──

    /** PTT down — 주채널에 Floor Request. GRANT 수신 시에만 실제 발화(mic on). */
    /** PTT 발언 대상 — 활성 1:1 이 있으면 그것이 우선(전화>무전 규칙), 없으면 주채널. */
    private fun talkSession(): Session? = synchronized(lock) {
        sessionMap.values.firstOrNull { v -> v.privatePeer && v.callId >= 0 }
            // 애드혹 진행 중엔 전용 오버레이가 전면이라 PTT 도 애드혹 세션을 향한다(주채널보다 우선).
            ?: sessionMap.values.firstOrNull { v -> isAdhocId(v.groupId) && v.callId >= 0 }
    } ?: primarySession()

    fun pttDown() {
        val s = talkSession() ?: run { _status.value = "그룹콜을 먼저 시작하세요"; return }
        // 멀티 1:1(mc_no_floor_ctrl — floor 절차 없음, TS 24.379): PTT 는 서버 요청 없이
        // 로컬 마이크 게이트로만 동작한다. 양쪽이 같이 누르면 동시 발화(서버는 상시 중계).
        if (s.fullDuplex) {
            pttHeld = true
            setTalkCapture(true)
            s.floorState = FloorState.SPEAKING     // 로컬 표시 (서버 GRANT 아님)
            s.mySpeakStartMs = SystemClock.elapsedRealtime()
            // 그룹 GRANT 경로와 동일한 "삑 후 말하기" — 톤 재생 뒤 mic 결선. 같은 틱에 열면
            // snd dev 재오픈(setCaptureEnabled)과 경합해 두 번째 press 부터 캡처가 안 열린다(실측).
            scope.launch {
                delay(feedback?.grantTone() ?: 100L)
                val p = talkSession()
                if (pttHeld && p?.fullDuplex == true && p.callId >= 0)
                    sip.setMicEnabled(p.callId, true)
            }
            publish()
            return
        }
        // Floor Taken 이 Permission=0 을 실어 온 세션(broadcast 그룹·ambient 청취 leg)은
        // 요청해봐야 Deny 뿐이다 — 요청 자체를 막고 이유를 알린다(TS 24.380 §6.3.4.4.2-3d).
        if (!s.canRequestFloor) { feedback?.denyTone(); _status.value = "이 채널은 청취 전용"; return }
        when (s.floorState) {
            // QUEUED = 이미 요청이 대기열에 있다(버튼 유지 중) — 다시 보낼 것이 없다.
            FloorState.SPEAKING, FloorState.REQUESTING, FloorState.QUEUED -> return
            // 남이 발언 중 — 동시 발언 세션이면 **서버가 정원을 보고 판단**하므로 요청을 보낸다.
            //   로컬에서 막으면 multi 정책의 남은 슬롯을 영원히 못 쓴다(실측: 요청 미발신 +
            //   버튼을 뗄 때 Release 만 나가 서버 로그에 고아 Release 가 쌓임). 동시 발언
            //   불가(single)일 때만 종전처럼 즉시 거부음으로 알린다.
            //   긴급 개시자도 예외 — 선점(REVOKE→GRANT, TS 24.380 §6.3.4.4.7)은 CMP 의
            //   tier 판정이므로 요청이 서버에 도달해야 발동한다(로컬 차단 = 선점 불가).
            FloorState.LISTENING -> if (!s.multiTalkerSession() && !(s.emergency && s.emergencyMine)) {
                feedback?.denyTone(); _status.value = "다른 사용자가 발언 중"; return
            }
            else -> Unit
        }
        pttHeld = true
        setTalkCapture(true)     // 마이크 확보 개시 — volte 양보 + 전이중 전환(floor 요청과 병렬 진행)
        // 긴급 세션의 발언은 Floor Indicator 에 emergency 비트 — CMP tier 상향/선점(TS 24.380).
        // Floor Priority 는 싣지 않는다 — 유효 우선순위가 요청값으로 깎이지 않게(§6.3.5.4.4-1a).
        s.floor.requestFloor(indicator = if (s.emergency) FloorIndicator.EMERGENCY else null)
        s.floorState = FloorState.REQUESTING
        publish()
        // GRANT/DENY 무응답 방어 — 타임아웃 시 IDLE 복귀 + 거부 톤
        requestTimeout?.cancel()
        requestTimeout = scope.launch {
            delay(REQUEST_TIMEOUT_MS)
            val p = primarySession() ?: return@launch
            if (p.floorState == FloorState.REQUESTING) {
                p.floorState = FloorState.IDLE
                setTalkCapture(false)
                feedback?.denyTone()
                _status.value = "발언권 응답 없음"
                publish()
            }
        }
    }

    /** PTT up — 주채널 Floor Release + mic off. */
    fun pttUp() {
        pttHeld = false
        requestTimeout?.cancel()
        val s = talkSession() ?: return
        // 멀티 1:1 — 로컬 마이크 게이트 닫기 (pttDown 과 쌍, 서버 메시지 없음).
        if (s.fullDuplex) {
            if (s.callId >= 0) sip.setMicEnabled(s.callId, false)
            setTalkCapture(false)
            if (s.mySpeakStartMs > 0) {
                emit(PttEventKind.TALK_ME, s.groupId,
                    durationMs = SystemClock.elapsedRealtime() - s.mySpeakStartMs)
                s.mySpeakStartMs = 0
            }
            s.floorState = FloorState.IDLE
            publish()
            return
        }
        val wasSpeaking = s.floorState == FloorState.SPEAKING
        clearTalkLimit(s)
        if (wasSpeaking && s.mySpeakStartMs > 0) {
            emit(PttEventKind.TALK_ME, s.groupId,
                durationMs = SystemClock.elapsedRealtime() - s.mySpeakStartMs)
            s.mySpeakStartMs = 0
        }
        // 대기 중이었다면 대기 요청부터 취소한다(§8.2.15) — 서버는 발언 중이 아닌 leg 의
        // Floor Release 를 무시하므로, Release 만 보내면 유령 대기자로 남아 나중에 엉뚱한
        // 시점에 발언권을 받는다(그때는 버튼을 뗀 뒤라 즉시 반납 — 승급 한 턴이 낭비된다).
        if (s.floorState == FloorState.QUEUED) s.floor.cancelQueuedRequest()
        // 요청/점유한 적이 있을 때만 Release 를 보낸다 — 요청조차 안 한 상태(LISTENING/IDLE)의
        //   Release 는 서버가 무시하는 고아 메시지일 뿐이고, 반복 누름마다 쌓여 로그를 오염시킨다.
        if (s.floorState == FloorState.SPEAKING || s.floorState == FloorState.REQUESTING ||
            s.floorState == FloorState.QUEUED) {
            s.floor.releaseFloor()
        }
        s.queuePosition = null
        if (s.callId >= 0) sip.setMicEnabled(s.callId, false)
        setTalkCapture(false)    // 발언 종료 — 스피커 전용 복귀 + volte 마이크 복귀
        // 내 발언만 끝난다 — 동시 발언 중이면 남은 화자를 계속 듣는 상태로 남는다.
        s.talkers = s.talkers.filterNot { it.self }
        s.floorState = if (s.talkers.isEmpty()) FloorState.IDLE else FloorState.LISTENING
        if (s.speaker?.self == true) s.speaker = s.talkers.firstOrNull()
        if (wasSpeaking) feedback?.releaseTone()
        publish()
    }

    /**
     * Granted Duration(TS 24.380 §8.2.3.3 = 서버 T2) 준수 — 허용 시간을 넘기면 서버가
     * Revoke #2(Media burst too long)로 끊는다. 마감 [TALK_WARN_MS] 전에 알리고, 마감
     * 직전([TALK_END_MARGIN_MS])에 **스스로 발언을 끝낸다** — 회수당하는 것보다 낫다.
     * Duration 이 없거나 0(서버 FloorStopTalkSec=0 = 무제한)이면 타이머를 걸지 않는다.
     */
    private fun armTalkLimit(s: Session, durationSec: Int?) {
        clearTalkLimit(s)
        val d = (durationSec ?: 0).toLong() * 1000L
        if (d <= TALK_END_MARGIN_MS) return
        s.speakDeadlineMs = SystemClock.elapsedRealtime() + d
        s.talkLimit = scope.launch {
            val warnAt = d - TALK_WARN_MS
            if (warnAt > 0) {
                delay(warnAt)
                if (s.floorState == FloorState.SPEAKING) feedback?.talkLimitTone()
            }
            delay(maxOf(0L, d - maxOf(0L, warnAt) - TALK_END_MARGIN_MS))
            if (s.floorState != FloorState.SPEAKING) return@launch
            Log.i(TAG, "talk limit reached (${durationSec}s) — self release")
            _status.value = "발언 시간 초과 — 발언 종료"
            // 주채널이면 평소 발언 종료 경로(mic·캡처 게이트·이력까지) 그대로, 아니면 floor 만 반납.
            if (primarySession() === s) pttUp() else { s.floor.releaseFloor(); clearTalkLimit(s) }
        }
    }

    private fun clearTalkLimit(s: Session) {
        s.talkLimit?.cancel()
        s.talkLimit = null
        s.speakDeadlineMs = 0
    }

    /**
     * 동시 발언 화자 집합 반영 (TS 24.380 §8.2.3.17~18) — Floor Taken 의 화자 리스트나
     * 0x0F 이탈 후 잔여 목록을 세션 상태로 옮긴다.
     *
     * **이미 말하고 있던 화자의 [Speaker.sinceMs] 는 보존한다** — 다른 사람이 끼어들 때마다
     * 목록이 다시 오는데, 매번 새로 만들면 발언 경과시간이 0 으로 되돌아간다.
     * 화자별 SSRC 는 [Session.talkerSsrc] 에 남긴다(SSRC 별 재생 분리 = U10 의 입력).
     */
    private fun applyTalkers(s: Session, list: List<FloorTalker>) {
        val now = SystemClock.elapsedRealtime()
        val prev = s.talkers.associateBy { bareId(it.id) }
        s.talkers = list.map { t ->
            prev[bareId(t.id)]
                ?: Speaker(t.id, self = t.self, sinceMs = now, groupId = s.groupId)
        }
        s.talkerSsrc = list.mapNotNull { t -> t.ssrc?.let { bareId(t.id) to it } }.toMap()
        // 대표 화자 = 내가 말하고 있으면 나, 아니면 첫 타인(단일 발언이면 종전과 동일).
        s.speaker = s.talkers.firstOrNull { it.self } ?: s.talkers.firstOrNull()
    }

    private fun onFloorEvent(groupId: String, ev: FloorEvent) {
        val s = synchronized(lock) { sessionMap[groupId] } ?: return
        // 발언 주체 판정은 **pttDown 과 같은 규칙([talkSession])** 이어야 한다 — 1:1 은 주채널을
        //   점유하지 않으므로(role=NONE) role 로만 보면 내가 요청해 받은 GRANT 를 "비주채널"로
        //   오판해 즉시 반납한다(실측: GRANT 16ms 뒤 Release, 버튼은 누른 채). 마이크 결선·해제
        //   같은 장치 조작은 종전대로 주채널일 때만 수행한다.
        val isPrimary = s.role == ChannelRole.PRIMARY
        val isTalkTarget = s === talkSession()
        when (ev) {
            is FloorEvent.Granted -> {
                requestTimeout?.cancel()
                if (!isTalkTarget || !pttHeld) {     // 늦은 GRANT/발언 대상 아님 — 즉시 반납
                    s.floor.releaseFloor()
                    if (isPrimary) setTalkCapture(false)
                    s.floorState = FloorState.IDLE
                    publish()
                    return
                }
                s.floorState = FloorState.SPEAKING
                s.floorIndicator = ev.indicator ?: 0
                // 동시 발언이면 이미 말하던 화자를 남겨 둔 채 나를 더한다(dual/multi 2번째 자리).
                val me = s.talkers.firstOrNull { it.self }
                    ?: Speaker(mcpttId, self = true, sinceMs = SystemClock.elapsedRealtime(), groupId = s.groupId)
                s.talkers = listOf(me) + s.talkers.filterNot { it.self }
                s.speaker = me
                s.mySpeakStartMs = SystemClock.elapsedRealtime()
                armTalkLimit(s, ev.durationSec)
                _status.value = "발언권 획득"
                // "삑 후 말하기": 승인 톤 재생이 끝난 뒤 mic 개방(톤이 그룹으로 송출되지 않게).
                //   결선 대상은 GRANT 를 받은 그 세션 — primarySession() 으로 잡으면 1:1(주채널
                //   비점유)에서 엉뚱한 세션(또는 null)을 보고 마이크가 열리지 않는다.
                scope.launch {
                    delay(feedback?.grantTone() ?: 0L)
                    if (pttHeld && s.floorState == FloorState.SPEAKING && s.callId >= 0)
                        sip.setMicEnabled(s.callId, true)
                }
            }
            is FloorEvent.Denied -> {
                clearTalkLimit(s)
                // 거부 피드백은 **요청한 세션** 기준 — 1:1 은 주채널이 아니라 role 로 보면
                //   거부음·사유 표시가 통째로 누락된다.
                if (isTalkTarget) {
                    requestTimeout?.cancel()
                    if (s.callId >= 0) sip.setMicEnabled(s.callId, false)
                    setTalkCapture(false)
                    feedback?.denyTone()
                    _status.value = "발언권 거부: ${ev.text ?: ev.cause}"
                }
                s.floorState = FloorState.IDLE
            }
            is FloorEvent.Revoked -> {
                clearTalkLimit(s)
                // Floor Release 회신은 FloorClient 가 이미 보냈다(§6.2.4.5.4) — 여기선 mic·UI 만.
                if (s.callId >= 0) sip.setMicEnabled(s.callId, false)
                if (isTalkTarget) setTalkCapture(false)
                s.floorState = FloorState.IDLE
                if (s.speaker?.self == true && s.mySpeakStartMs > 0) {
                    emit(PttEventKind.TALK_ME, s.groupId,
                        durationMs = SystemClock.elapsedRealtime() - s.mySpeakStartMs)
                    s.mySpeakStartMs = 0
                }
                // 내 발언만 회수된다 — 동시 발언 중이면 남은 화자는 계속 들린다.
                s.talkers = s.talkers.filterNot { it.self }
                s.speaker = s.talkers.firstOrNull()
                if (s.talkers.isNotEmpty()) s.floorState = FloorState.LISTENING
                if (isTalkTarget) { feedback?.revokeTone(); _status.value = "발언권 회수: ${ev.text ?: ev.cause}" }
            }
            is FloorEvent.Taken -> {
                // Permission to Request the Floor(§8.2.3.7) — 이 leg 의 발언 요청 가부. 서버가
                // broadcast 그룹·ambient 청취 leg 에만 0 을 보내므로, 값이 올 때만 갱신한다.
                ev.permission?.let { s.canRequestFloor = it != FloorPermission.DENIED }
                s.floorIndicator = ev.indicator ?: 0
                applyTalkers(s, ev.talkers)
                // 동시 발언(dual/multi)에서 뒤에 승급한 화자의 Taken 은 **먼저 말하던 나에게도**
                // 오고 목록에 내가 있다 — 그때 강등하면 내 마이크가 닫힌다(§6.3.4.4.7a).
                if (ev.meSpeaking) {
                    s.floorState = FloorState.SPEAKING
                } else {
                    clearTalkLimit(s)
                    s.floorState = FloorState.LISTENING
                }
                val other = s.talkers.firstOrNull { !it.self }?.let { bareId(it.id) }
                if (other != s.otherSpeaker) {
                    s.otherSpeaker?.let { prev ->  // 발언자 교대 — 직전 수신 발언 마감
                        emit(PttEventKind.TALK_OTHER, s.groupId, peer = prev,
                            durationMs = SystemClock.elapsedRealtime() - s.otherSpeakStartMs)
                    }
                    s.otherSpeaker = other
                    s.otherSpeakStartMs = SystemClock.elapsedRealtime()
                }
                // CMP 는 긴급 tier 발언자의 TAKEN 에 emergency 비트를 방송 — 수신측 긴급 표시 latch
                // (CSP 는 상향을 fan-out 하지 않으므로 이것이 in-call 수신 경로의 유일한 신호)
                if ((ev.indicator ?: 0) and FloorIndicator.EMERGENCY != 0 && !s.emergency) {
                    s.emergency = true
                    feedback?.emergencyTone()
                    emit(PttEventKind.EMERGENCY_IN, s.groupId, peer = s.otherSpeaker)
                    if (isPrimary) _status.value = "🚨 [${s.groupId}] 긴급 발언 수신"
                }
            }
            // 동시 발언 중 한 명만 발언 종료(0x0F) — Idle 이 아니므로 목록만 줄인다.
            is FloorEvent.TalkerLeft -> {
                applyTalkers(s, ev.talkers)
                if (ev.meSpeaking) s.floorState = FloorState.SPEAKING
                else {
                    clearTalkLimit(s)
                    s.floorState = if (ev.talkers.isEmpty()) FloorState.IDLE else FloorState.LISTENING
                }
                val goneBare = ev.id?.let { bareId(it) }
                if (goneBare != null && goneBare == s.otherSpeaker) {  // 수신 발언 마감(이력)
                    emit(PttEventKind.TALK_OTHER, s.groupId, peer = goneBare,
                        durationMs = SystemClock.elapsedRealtime() - s.otherSpeakStartMs)
                    s.otherSpeaker = s.talkers.firstOrNull { !it.self }?.let { bareId(it.id) }
                    s.otherSpeakStartMs = SystemClock.elapsedRealtime()
                }
            }
            FloorEvent.Idle -> {
                if (s.floorState != FloorState.SPEAKING) {
                    clearTalkLimit(s)
                    s.floorState = FloorState.IDLE
                    s.talkers = emptyList()
                    s.talkerSsrc = emptyMap()
                }
                if (s.speaker?.self != true) s.speaker = null
                s.otherSpeaker?.let { prev ->      // 수신 발언 종료 — 이력 마감
                    emit(PttEventKind.TALK_OTHER, s.groupId, peer = prev,
                        durationMs = SystemClock.elapsedRealtime() - s.otherSpeakStartMs)
                    s.otherSpeaker = null
                }
            }
            // 대기열 진입·위치 변동(§8.2.11) — 버튼을 계속 누르고 있으면 승급을 기다리고,
            // 떼면 pttUp 의 Floor Release 가 대기 요청을 취소한다(§8.2.15 취소 경로).
            is FloorEvent.QueuePosition -> {
                s.floorState = FloorState.QUEUED
                s.queuePosition = ev.position
                if (isTalkTarget) _status.value = ev.position?.let { "발언 대기 ${it}번째" } ?: "발언 대기 중"
            }
            // 대기 요청 소멸 — 내 취소의 결과이거나 서버/의장이 지운 통지. 어느 쪽이든 IDLE.
            is FloorEvent.QueueCancelled -> {
                s.queuePosition = null
                if (s.floorState == FloorState.QUEUED) s.floorState = FloorState.IDLE
                if (isPrimary && !ev.byMe) {
                    feedback?.denyTone()
                    _status.value = "발언 대기 취소됨"
                }
            }
            is FloorEvent.Other -> Log.d(TAG, "floor other ${ev.type}")
        }
        publish()
    }

    fun shutdown() {
        requestTimeout?.cancel()
        setTalkCapture(false)    // 발언 중 종료 대비 — volte 마이크 복귀 통지
        synchronized(lock) {
            sessionMap.values.forEach { it.close() }
            sessionMap.clear()
        }
        feedback?.close(); feedback = null
        sip.shutdown()
    }

    companion object {
        private const val TAG = "PttController"
        /** Floor Request 후 GRANT/DENY 무응답 시 IDLE 복귀 시한. */
        private const val REQUEST_TIMEOUT_MS = 3000L

        /** Granted Duration(T2) 마감 임박 알림 시점 — 마감 이 시간 전에 톤·진동으로 알린다. */
        private const val TALK_WARN_MS = 5000L
        /** 자체 종료 여유 — 마감 이 시간 전에 Release 를 보내 서버 Revoke #2 를 앞지른다. */
        private const val TALK_END_MARGIN_MS = 300L

        /** 오디오 라우팅: 이어폰(유선/BT) — [SipController.AUDIO_ROUTE_DEFAULT]/EARPIECE/SPEAKER(0~2) 확장. */
        const val AUDIO_ROUTE_HEADSET = 3


        /** MCData ICSI (TS 24.282) — MSRP INVITE Accept-Contact·PR4 수신 광고 공용. */
        const val MCDATA_ICSI = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds"
        /** affiliation PUBLISH Expires(초) — 잔여 수명이 절반 미만이면 주기 루프가 재발행. */
        private const val AFF_EXPIRES_SEC = 3600L

        /** 최초 SUBSCRIBE 발행 후 확인(NOTIFY) 대기 시한 — 초과하면 재발행 대상으로 되돌린다.
         *  CSP 는 구독 수락 직후 초기 NOTIFY 를 보내므로 정상 경로는 수십 ms 다. 이 창은
         *  생성 경합(중복 구독)을 막는 용도이므로 짧게 두되, 패킷 유실·일시 지연은 흡수하는 값. */
        private const val SUB_CONFIRM_TIMEOUT_MS = 15_000L

        /** 구독 재확인 주기 — 이 시간마다 SUBSCRIBE 를 다시 던진다(살아 있으면 native 가
         *  in-dialog 갱신으로 흡수, 죽었으면 새로 생성). 구독 소멸을 앱이 감지할 수단이
         *  없으므로(evsub 481 종료는 앱에 통지되지 않음) **감지 대신 주기적 재확인**으로
         *  수렴시킨다 — 서버가 구독을 잃어도 최대 이 시간 안에 복구된다.
         *  `SipController.CONF_SUB_EXPIRES_SEC`(3600s)보다 충분히 짧게 둔다. */
        private const val SUB_REASSERT_MS = 600_000L

        /** MSRP INVITE 발신 → 200 OK(a=path) 대기 시한. */
        private const val MSRP_INVITE_TIMEOUT_MS = 15_000L
        /** MSRP 전송 완료 후 서버 BYE 대기 시한(초과 시 로컬 hangup — 서버 스위퍼가 안전망). */
        private const val MSRP_BYE_TIMEOUT_MS = 10_000L

        /** URI("tel:g001"/"sip:g001@dom"/"\"이름\" <sip:..>") → 번호부("g001"). */
        fun bareId(uri: String): String {
            val m = Regex("(?:tel:|sips?:)([^@>;\\s]+)").find(uri)
            return (m?.groupValues?.get(1) ?: uri.trim()).substringBefore('@')
        }

        /** 애드혹 임시 그룹 ID 판별 — `adhoc-` 접두사는 편성 그룹 ID 로 예약 거부(CSC)돼
         *  기존 그룹·가입자 번호와 충돌하지 않는다. 편성 채널 저장/구독/affiliation 제외 기준. */
        fun isAdhocId(id: String): Boolean = id.startsWith("adhoc-")

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
    }
}
