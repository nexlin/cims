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
import com.cims.ue.ptt.mcptt.McpttXml
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** 현재 발언자 — 내 GRANT([self]=true) 또는 타인 TAKEN. [sinceMs]=elapsedRealtime(경과시간 표시용).
 *  [groupId]=발언이 들리는 그룹(멀티그룹 모니터링에서 주채널 밖 발언 구분). */
data class Speaker(val id: String, val self: Boolean, val sinceMs: Long, val groupId: String? = null)

/** 채널 지정 — 주(발언 대상, 1개)/부(모니터링)/일반. */
enum class ChannelRole { PRIMARY, SECONDARY, NONE }

/** 듣기 정책 — 주·부채널만 / 참여한 모든 그룹. */
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

    /** 통화이력 이벤트 훅 — 서비스가 주입(HistoryStore 영속). 컨트롤러 스레드에서 호출되므로 가볍게. */
    var onEvent: ((PttEvent) -> Unit)? = null
    private fun emit(kind: PttEventKind, groupId: String, peer: String? = null, durationMs: Long = 0) {
        runCatching { onEvent?.invoke(PttEvent(kind, groupId, peer, durationMs)) }
    }

    /** 수신 문자(SIP MESSAGE) — core 흐름 그대로 노출(서비스가 MessageStore 에 영속). */
    val incomingMessage get() = sip.incomingMessage

    // ── 그룹별 세션 ──

    private inner class Session(val groupId: String) {
        var callId: Int = -1
        var active: Boolean = false
        var role: ChannelRole = ChannelRole.NONE
        var floorState: FloorState = FloorState.IDLE
        var speaker: Speaker? = null
        var participants: MutableMap<String, String> = mutableMapOf(bareId(mcpttId) to "connected")
        var audible: Boolean = true
        var emergency: Boolean = false
        var emergencyMine: Boolean = false
        var mySpeakStartMs: Long = 0          // 이력용 — 내 발언 시작(elapsedRealtime)
        var otherSpeaker: String? = null      // 이력용 — 수신 중 발언자
        var otherSpeakStartMs: Long = 0
        val floor: FloorClient = FloorClient(ssrc, mcpttId, localPort = 0,
            onEvent = { ev -> onFloorEvent(groupId, ev) })

        fun toState() = GroupCallState(groupId, callId, active, role, floorState, speaker, participants.toMap(),
            audible, emergency, emergencyMine)
        fun close() { runCatching { floor.close() } }
    }

    private val lock = Any()
    private val sessionMap = LinkedHashMap<String, Session>()   // groupId → Session (참여 순서 유지)

    private val _sessions = MutableStateFlow<List<GroupCallState>>(emptyList())
    /** 참여 중인 그룹 세션들(참여 순). */
    val sessions: StateFlow<List<GroupCallState>> = _sessions.asStateFlow()

    private val _listenPolicy = MutableStateFlow(ListenPolicy.ALL)
    /** 듣기 정책 — 주·부채널만/전체. */
    val listenPolicy: StateFlow<ListenPolicy> = _listenPolicy.asStateFlow()

    private val _audioRoute = MutableStateFlow(SipController.AUDIO_ROUTE_DEFAULT)
    /** 오디오 출력 라우팅(전역) — 일반(자동)/수화구/스피커. */
    val audioRoute: StateFlow<Int> = _audioRoute.asStateFlow()

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
    @Volatile private var pttHeld = false
    private var requestTimeout: Job? = null
    private val ssrc: Long = (mcpttId.hashCode().toLong() and 0xffffffffL).let { if (it == 0L) 1L else it }

    init {
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
                        sip.setAudioRoute(_audioRoute.value)        // 통화별 라우팅 재적용
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

    /** [groupId] 를 주채널로 — 기존 주채널은 부채널로 강등. */
    fun setPrimary(groupId: String) {
        synchronized(lock) {
            val s = sessionMap[groupId] ?: return
            sessionMap.values.firstOrNull { it.role == ChannelRole.PRIMARY }?.let {
                if (it !== s) it.role = ChannelRole.SECONDARY
            }
            s.role = ChannelRole.PRIMARY
        }
        applyListenPolicy()
    }

    /** 부채널 토글(주채널에는 적용 안 함). */
    fun toggleSecondary(groupId: String) {
        synchronized(lock) {
            val s = sessionMap[groupId] ?: return
            if (s.role == ChannelRole.PRIMARY) return
            s.role = if (s.role == ChannelRole.SECONDARY) ChannelRole.NONE else ChannelRole.SECONDARY
        }
        applyListenPolicy()
    }

    fun setListenPolicy(p: ListenPolicy) {
        _listenPolicy.value = p
        applyListenPolicy()
    }

    /** 듣기 정책 적용 — 비채널 그룹은 참여 유지하되 수신 음소거. */
    private fun applyListenPolicy() {
        val policy = _listenPolicy.value
        synchronized(lock) {
            for (s in sessionMap.values) {
                val on = policy == ListenPolicy.ALL || s.role != ChannelRole.NONE
                if (s.audible != on) {
                    s.audible = on
                    if (s.callId >= 0) sip.setCallListen(s.callId, on)
                }
            }
        }
        publish()
    }

    /** 오디오 출력 라우팅(전역) — [SipController.AUDIO_ROUTE_DEFAULT]/EARPIECE/SPEAKER. */
    fun setAudioRoute(route: Int) {
        _audioRoute.value = route
        sip.setAudioRoute(route)
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

    /** 그룹 문자(SIP MESSAGE) 발신 — CSP 가 그룹 URI 수신 시 멤버 fan-out. 로컬 저장은 서비스 몫. */
    fun sendGroupMessage(groupId: String, text: String) {
        if (text.isBlank()) return
        sip.sendRequest(
            method = "MESSAGE",
            targetUri = "sip:$groupId@${sipConfig.domain}",
            contentType = "text/plain",
            body = text,
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

        /** URI("tel:g001"/"sip:g001@dom"/"\"이름\" <sip:..>") → 번호부("g001"). */
        fun bareId(uri: String): String {
            val m = Regex("(?:tel:|sips?:)([^@>;\\s]+)").find(uri)
            return (m?.groupValues?.get(1) ?: uri.trim()).substringBefore('@')
        }
    }
}
