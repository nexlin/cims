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
import com.cims.ue.ptt.floor.FloorState
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
enum class PttEventKind { JOIN, LEAVE, TALK_ME, TALK_OTHER, EMERGENCY, EMERGENCY_IN, EMERGENCY_END }

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

    /** 이어폰(유선/BT) 장치 열거·지정 — 서비스가 주입. */
    var audioRouter: com.cims.ue.ptt.audio.AudioRouter? = null

    /** 라우팅 선택 영속화 — 서비스가 주입(리부팅/재기동 복원). */
    var routePrefs: com.cims.ue.ptt.audio.AudioRoutePrefs? = null

    /** 통화이력 이벤트 훅 — 서비스가 주입(HistoryStore 영속). 컨트롤러 스레드에서 호출되므로 가볍게. */
    var onEvent: ((PttEvent) -> Unit)? = null
    private fun emit(kind: PttEventKind, groupId: String, peer: String? = null, durationMs: Long = 0) {
        runCatching { onEvent?.invoke(PttEvent(kind, groupId, peer, durationMs)) }
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

    private inner class Session(val groupId: String) {
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
        val floor: FloorClient = FloorClient(ssrc, mcpttId, localPort = 0,
            onEvent = { ev -> onFloorEvent(groupId, ev) })

        fun toState() = GroupCallState(groupId, callId, active, role, floorState, speaker, participants.toMap(),
            audible, emergency, emergencyMine, volume)
        fun close() { runCatching { floor.close() } }
    }

    private val lock = Any()
    private val sessionMap = LinkedHashMap<String, Session>()   // groupId → Session (참여 순서 유지)

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

    private val _affiliated = MutableStateFlow<Set<String>>(emptySet())
    /** affiliate PUBLISH 를 보낸 그룹(낙관적 로컬 추적 — 서버 상태 구독은 후속). */
    val affiliated: StateFlow<Set<String>> = _affiliated.asStateFlow()

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

        // 학습된 CMP floor 목적지(호별) → 해당 세션 FloorClient 연결 + Ack 1회(NAT latch 유도)
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
        // 호 상태 → 세션 매핑 + MCPTT 그룹콜 착신 자동 수락(ptt_ue.md §12.3)
        scope.launch {
            sip.callState.collect { st ->
                when (st) {
                    is CallState.Outgoing -> bindCall(bareId(st.remote), st.id)
                    is CallState.Active -> {
                        bindCall(bareId(st.remote), st.id, active = true)
                        applyAudioRoute()                           // 통화별 라우팅 재적용
                        applyListenPolicy()
                    }
                    is CallState.Disconnected -> onCallEnded(st.id)
                    is CallState.Incoming -> if (st.mcptt) autoJoinGroupCall(st)
                    else -> Unit
                }
            }
        }
        // 참가자 목록 — in-dialog conference NOTIFY(RFC 4575), 호별
        scope.launch {
            sip.conferenceInfo.collect { (callId, xml) ->
                runCatching { sessionByCall(callId)?.let { onConferenceInfo(it, xml) } }
            }
        }
        // 등록 완료 시 선택 그룹 자동 affiliation — CSP 는 affiliation 된 멤버에게만 INVITE fan-out
        scope.launch {
            sip.regState.collect { r ->
                if (r is RegState.Registered) _selectedGroup.value?.let { ensureAffiliated(it) }
            }
        }
    }

    // ── 세션 헬퍼 ──

    private fun sessionByCall(callId: Int): Session? =
        synchronized(lock) { sessionMap.values.firstOrNull { it.callId == callId } }

    private fun bindCall(groupId: String, callId: Int, active: Boolean = false) {
        synchronized(lock) {
            val s = sessionMap[groupId] ?: return
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
        applyListenPolicy()
    }

    /** 주채널 해제 — 일반 참여로 강등(다른 채널 자동 승격 없음, 주채널 없는 상태 허용). */
    fun clearPrimary(groupId: String) {
        synchronized(lock) {
            val s = sessionMap[groupId] ?: return
            if (s.role != ChannelRole.PRIMARY) return
            s.role = ChannelRole.NONE
        }
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
                val on = policy == ListenPolicy.ALL || s.role != ChannelRole.NONE
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

    /** 현재 라우팅 적용/재적용 — 통화 성립(pjsua2 스트림 개방) 시에도 호출(라우팅 리셋 대비). */
    private fun applyAudioRoute() {
        if (_audioRoute.value == AUDIO_ROUTE_HEADSET) {
            sip.setAudioRoute(SipController.AUDIO_ROUTE_DEFAULT)   // pjsua2 강제 라우팅 해제
            audioRouter?.select(_headsetId.value)
        } else {
            audioRouter?.clear()
            sip.setAudioRoute(_audioRoute.value)
        }
    }

    private fun ensureAffiliated(groupId: String) {
        if (!_affiliated.value.contains(groupId)) affiliate(groupId, true)
    }

    // ── 그룹콜 참여/이탈 ──

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
        ensureAffiliated(groupId)
        val appSdp = "m=application ${s.floor.localPort} UDP MCPTT\r\na=floorid:0 mstrm:audio"
        val parts = ArrayList<SipBodyPart>()
        parts.add(SipBodyPart("application", "vnd.3gpp.mcptt-info+xml",
            McpttXml.mcpttInfo(McpttXml.SessionType.PREARRANGED, "tel:$groupId", mcpttId, "tel:$groupId",
                emergency = if (emergency) true else null)))
        if (members.isNotEmpty())
            parts.add(SipBodyPart("application", "resource-lists+xml", McpttXml.resourceLists(members)))
        sip.makeGroupCall("sip:$groupId@${sipConfig.domain}", parts, appSdp)
        _status.value = if (emergency) "🚨 긴급 그룹콜 개시 $groupId" else "그룹콜 참여 $groupId"
        emit(PttEventKind.JOIN, groupId)
        if (emergency) emit(PttEventKind.EMERGENCY, groupId)
        publish()
    }

    /** 그룹콜 착신 자동 수락 — 미참여 그룹이면 세션 생성 후 응답 SDP 에 m=application 주입.
     *  fan-out INVITE 의 emergency-ind → 긴급 표시 + 경고 톤. */
    private fun autoJoinGroupCall(inc: CallState.Incoming) {
        val groupId = bareId(inc.remote)
        if (groupId.isBlank()) return
        val s = synchronized(lock) {
            if (sessionMap.containsKey(groupId)) return          // 이미 참여 중
            Session(groupId).also {
                it.callId = inc.id
                it.role = if (sessionMap.values.none { v -> v.role == ChannelRole.PRIMARY }) ChannelRole.PRIMARY
                else ChannelRole.NONE
                it.emergency = inc.emergency
                sessionMap[groupId] = it
            }
        }
        sip.answerGroupCall(inc.id, "m=application ${s.floor.localPort} UDP MCPTT\r\na=floorid:0 mstrm:audio")
        if (inc.emergency) feedback?.emergencyTone()
        _status.value = if (inc.emergency) "🚨 긴급 그룹콜 자동 참여: $groupId" else "그룹콜 자동 참여: $groupId"
        emit(PttEventKind.JOIN, groupId)
        if (inc.emergency) emit(PttEventKind.EMERGENCY_IN, groupId)
        publish()
    }

    /** 그룹별 나가기. */
    fun leaveGroup(groupId: String) {
        val callId = synchronized(lock) { sessionMap[groupId]?.callId ?: return }
        if (callId >= 0) sip.hangup(callId) else {
            synchronized(lock) { sessionMap.remove(groupId)?.close() }
            publish()
        }
    }

    /** RFC 4575 conference-info 파싱 — 해당 세션의 참가자 맵 갱신. */
    private fun onConferenceInfo(s: Session, xml: String) {
        val full = Regex("<conference-info\\b[^>]*state=\"full\"").containsMatchIn(xml)
        val cur = if (full) mutableMapOf() else s.participants
        val userRe = Regex("<user\\b[^>]*entity=\"([^\"]+)\"[^>]*>(.*?)</user>", RegexOption.DOT_MATCHES_ALL)
        val stRe = Regex("<status>\\s*([A-Za-z-]+)\\s*</status>")
        for (m in userRe.findAll(xml)) {
            val id = bareId(m.groupValues[1])
            if (id.isBlank()) continue
            val status = stRe.find(m.groupValues[2])?.groupValues?.get(1) ?: "connected"
            if (status.equals("disconnected", ignoreCase = true)) cur.remove(id) else cur[id] = status
        }
        cur[bareId(mcpttId)] = "connected"                  // 본인은 항상 포함
        s.participants = cur
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
                if (_selectedGroup.value == null) {
                    list.firstOrNull()?.let { bareId(it.uri) }?.also { g ->
                        _selectedGroup.value = g
                        if (regState.value is RegState.Registered) ensureAffiliated(g)
                    }
                }
                _status.value = "그룹 ${list.size}개"
            }
            .onFailure { _status.value = "그룹 조회 실패: ${it.message}" }
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

    /** affiliation PUBLISH (TS 24.379 §9). on=false → de-affiliate(Expires:0). */
    fun affiliate(groupId: String, on: Boolean = true) {
        val groupSip = "sip:$groupId@${sipConfig.domain}"
        sip.sendRequest(
            method = "PUBLISH",
            targetUri = groupSip,
            contentType = McpttXml.CT_AFFILIATION,
            body = McpttXml.affiliationCommand("tel:$groupId", on),
            // Event: mcptt 필수 — TS 24.379 §9 (없으면 CSP 가 489 Bad Event 거부)
            headers = mapOf("Event" to "mcptt", "Expires" to if (on) "3600" else "0"),
        )
        _affiliated.value = if (on) _affiliated.value + groupId else _affiliated.value - groupId
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
     * 서버(CSP)가 그룹 capability(emergency_call) 미허용이면 normal 로 하향 수용된다.
     */
    fun startEmergency() {
        val s = primarySession()
        if (s == null) {
            val gid = _selectedGroup.value ?: run { _status.value = "긴급: 대상 그룹 없음"; return }
            joinGroupCall(gid, emergency = true)
            feedback?.emergencyTone()
            return
        }
        if (s.emergency) { _status.value = "[${s.groupId}] 이미 긴급 상태"; return }
        s.emergency = true
        s.emergencyMine = true
        if (s.callId >= 0) sendConditionReinvite(s, emergency = true)
        feedback?.emergencyTone()
        _status.value = "🚨 [${s.groupId}] 긴급 개시"
        emit(PttEventKind.EMERGENCY, s.groupId)
        publish()
    }

    /** 긴급 해제 — 개시자만 유효(서버는 비개시자의 취소 re-INVITE 를 무시, TS 24.379). */
    fun cancelEmergency() {
        val s = synchronized(lock) { sessionMap.values.firstOrNull { it.emergency && it.emergencyMine } }
            ?: run { _status.value = "해제할 긴급 없음(개시자만 해제 가능)"; return }
        s.emergency = false
        s.emergencyMine = false
        if (s.callId >= 0) sendConditionReinvite(s, emergency = false)
        _status.value = "[${s.groupId}] 긴급 해제"
        emit(PttEventKind.EMERGENCY_END, s.groupId)
        publish()
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
    fun pttDown() {
        val s = primarySession() ?: run { _status.value = "그룹콜을 먼저 시작하세요"; return }
        when (s.floorState) {
            FloorState.SPEAKING, FloorState.REQUESTING -> return
            FloorState.LISTENING -> { feedback?.denyTone(); _status.value = "다른 사용자가 발언 중"; return }
            else -> Unit
        }
        pttHeld = true
        // 긴급 세션의 발언은 Floor Indicator 에 emergency 비트 — CMP tier 상향/선점(TS 24.380)
        s.floor.requestFloor(priority = 0,
            indicator = if (s.emergency) FloorIndicator.EMERGENCY else null)
        s.floorState = FloorState.REQUESTING
        publish()
        // GRANT/DENY 무응답 방어 — 타임아웃 시 IDLE 복귀 + 거부 톤
        requestTimeout?.cancel()
        requestTimeout = scope.launch {
            delay(REQUEST_TIMEOUT_MS)
            val p = primarySession() ?: return@launch
            if (p.floorState == FloorState.REQUESTING) {
                p.floorState = FloorState.IDLE
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
        val s = primarySession() ?: return
        val wasSpeaking = s.floorState == FloorState.SPEAKING
        if (wasSpeaking && s.mySpeakStartMs > 0) {
            emit(PttEventKind.TALK_ME, s.groupId,
                durationMs = SystemClock.elapsedRealtime() - s.mySpeakStartMs)
            s.mySpeakStartMs = 0
        }
        s.floor.releaseFloor()
        if (s.callId >= 0) sip.setMicEnabled(s.callId, false)
        if (s.floorState != FloorState.LISTENING) s.floorState = FloorState.IDLE
        if (s.speaker?.self == true) s.speaker = null
        if (wasSpeaking) feedback?.releaseTone()
        publish()
    }

    private fun onFloorEvent(groupId: String, ev: FloorEvent) {
        val s = synchronized(lock) { sessionMap[groupId] } ?: return
        val isPrimary = s.role == ChannelRole.PRIMARY
        when (ev) {
            is FloorEvent.Granted -> {
                requestTimeout?.cancel()
                if (!isPrimary || !pttHeld) {        // 늦은 GRANT/비주채널 — 즉시 반납
                    s.floor.releaseFloor()
                    s.floorState = FloorState.IDLE
                    publish()
                    return
                }
                s.floorState = FloorState.SPEAKING
                s.speaker = Speaker(mcpttId, self = true, sinceMs = SystemClock.elapsedRealtime())
                s.mySpeakStartMs = SystemClock.elapsedRealtime()
                _status.value = "발언권 획득"
                // "삑 후 말하기": 승인 톤 재생이 끝난 뒤 mic 개방(톤이 그룹으로 송출되지 않게)
                scope.launch {
                    delay(feedback?.grantTone() ?: 0L)
                    val p = primarySession()
                    if (pttHeld && p?.floorState == FloorState.SPEAKING && p.callId >= 0)
                        sip.setMicEnabled(p.callId, true)
                }
            }
            is FloorEvent.Denied -> {
                if (isPrimary) {
                    requestTimeout?.cancel()
                    if (s.callId >= 0) sip.setMicEnabled(s.callId, false)
                    feedback?.denyTone()
                    _status.value = "발언권 거부: ${ev.text ?: ev.cause}"
                }
                s.floorState = FloorState.IDLE
            }
            is FloorEvent.Revoked -> {
                if (s.callId >= 0) sip.setMicEnabled(s.callId, false)
                s.floorState = FloorState.IDLE
                if (s.speaker?.self == true && s.mySpeakStartMs > 0) {
                    emit(PttEventKind.TALK_ME, s.groupId,
                        durationMs = SystemClock.elapsedRealtime() - s.mySpeakStartMs)
                    s.mySpeakStartMs = 0
                }
                if (s.speaker?.self == true) s.speaker = null
                if (isPrimary) { feedback?.revokeTone(); _status.value = "발언권 회수: ${ev.text ?: ev.cause}" }
            }
            is FloorEvent.Taken -> {
                s.floorState = FloorState.LISTENING
                s.speaker = Speaker(ev.speaker ?: "?", self = false, sinceMs = SystemClock.elapsedRealtime())
                s.otherSpeaker?.let { prev ->      // 발언자 교대 — 직전 수신 발언 마감
                    emit(PttEventKind.TALK_OTHER, s.groupId, peer = prev,
                        durationMs = SystemClock.elapsedRealtime() - s.otherSpeakStartMs)
                }
                s.otherSpeaker = ev.speaker?.let { bareId(it) } ?: "?"
                s.otherSpeakStartMs = SystemClock.elapsedRealtime()
                // CMP 는 긴급 tier 발언자의 TAKEN 에 emergency 비트를 방송 — 수신측 긴급 표시 latch
                // (CSP 는 상향을 fan-out 하지 않으므로 이것이 in-call 수신 경로의 유일한 신호)
                if ((ev.indicator ?: 0) and FloorIndicator.EMERGENCY != 0 && !s.emergency) {
                    s.emergency = true
                    feedback?.emergencyTone()
                    emit(PttEventKind.EMERGENCY_IN, s.groupId, peer = s.otherSpeaker)
                    if (isPrimary) _status.value = "🚨 [${s.groupId}] 긴급 발언 수신"
                }
            }
            FloorEvent.Idle -> {
                if (s.floorState != FloorState.SPEAKING) s.floorState = FloorState.IDLE
                if (s.speaker?.self != true) s.speaker = null
                s.otherSpeaker?.let { prev ->      // 수신 발언 종료 — 이력 마감
                    emit(PttEventKind.TALK_OTHER, s.groupId, peer = prev,
                        durationMs = SystemClock.elapsedRealtime() - s.otherSpeakStartMs)
                    s.otherSpeaker = null
                }
            }
            is FloorEvent.QueuePosition -> { s.floorState = FloorState.QUEUED; _status.value = "대기열 ${ev.position}" }
            is FloorEvent.Other -> Log.d(TAG, "floor other ${ev.type}")
        }
        publish()
    }

    fun shutdown() {
        requestTimeout?.cancel()
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

        /** 오디오 라우팅: 이어폰(유선/BT) — [SipController.AUDIO_ROUTE_DEFAULT]/EARPIECE/SPEAKER(0~2) 확장. */
        const val AUDIO_ROUTE_HEADSET = 3

        /** MCData ICSI (TS 24.282) — MSRP INVITE Accept-Contact·PR4 수신 광고 공용. */
        const val MCDATA_ICSI = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds"
        /** MSRP INVITE 발신 → 200 OK(a=path) 대기 시한. */
        private const val MSRP_INVITE_TIMEOUT_MS = 15_000L
        /** MSRP 전송 완료 후 서버 BYE 대기 시한(초과 시 로컬 hangup — 서버 스위퍼가 안전망). */
        private const val MSRP_BYE_TIMEOUT_MS = 10_000L

        /** URI("tel:g001"/"sip:g001@dom"/"\"이름\" <sip:..>") → 번호부("g001"). */
        fun bareId(uri: String): String {
            val m = Regex("(?:tel:|sips?:)([^@>;\\s]+)").find(uri)
            return (m?.groupValues?.get(1) ?: uri.trim()).substringBefore('@')
        }

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
