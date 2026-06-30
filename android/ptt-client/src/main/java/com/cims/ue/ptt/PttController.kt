package com.cims.ue.ptt

import android.util.Log
import com.cims.ue.core.config.SipAccountConfig
import com.cims.ue.core.sip.CallState
import com.cims.ue.core.sip.RegState
import com.cims.ue.core.sip.SipBodyPart
import com.cims.ue.core.sip.SipController
import com.cims.ue.ptt.csc.CscClient
import com.cims.ue.ptt.csc.CscConfig
import com.cims.ue.ptt.csc.GroupSummary
import com.cims.ue.ptt.csc.TokenSet
import com.cims.ue.ptt.floor.FloorClient
import com.cims.ue.ptt.floor.FloorEvent
import com.cims.ue.ptt.floor.FloorState
import com.cims.ue.ptt.mcptt.McpttXml
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * MCPTT 그룹 PTT 오케스트레이션 — 비-PJSIP 코어(CSC/floor/XML)와 core PJSIP `SipController` 를 묶는다.
 *
 * 부팅 순서(설계서 §7): **CSC 인증 → 그룹 조회 → SIP REGISTER → affiliation PUBLISH → 키업 그룹 INVITE**.
 * 키업 INVITE 는 multipart(mcptt-info + resource-lists) + SDP `m=application`(floor) 주입; 응답 SDP 에서
 * CMP floor 포트를 학습해 [FloorClient] 에 연결. floor GRANT/RELEASE 가 mic 송신을 토글(반이중).
 *
 * 모든 floor/SIP 규약은 3GPP TS(24.380/24.379/33.180) 정합 — 서버측 규격 정렬 전엔 interop 안 될 수 있음.
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

    private val _floorState = MutableStateFlow(FloorState.IDLE)
    val floorState: StateFlow<FloorState> = _floorState.asStateFlow()

    private val _groups = MutableStateFlow<List<GroupSummary>>(emptyList())
    val groups: StateFlow<List<GroupSummary>> = _groups.asStateFlow()

    private val _status = MutableStateFlow("대기")
    val status: StateFlow<String> = _status.asStateFlow()

    private var csc: CscClient? = cscConfig?.let { CscClient(it, allowInsecureTls) }
    @Volatile private var token: TokenSet? = null
    @Volatile private var floor: FloorClient? = null
    @Volatile private var activeCallId: Int = -1
    private val ssrc: Long = (mcpttId.hashCode().toLong() and 0xffffffffL).let { if (it == 0L) 1L else it }

    init {
        // 학습된 CMP floor 목적지 → FloorClient 연결
        scope.launch {
            sip.floorRemote.collect { rem -> rem?.let { (ip, port) -> floor?.connectRemote(ip, port); _status.value = "floor 연결 $ip:$port" } }
        }
        // 활성 호 id 추적
        scope.launch {
            sip.callState.collect { st -> if (st is CallState.Active) activeCallId = st.id else if (st is CallState.Disconnected) activeCallId = -1 }
        }
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
    }

    fun loadGroups() = scope.launch {
        val c = csc ?: return@launch
        val t = token?.accessToken ?: run { _status.value = "토큰 없음"; return@launch }
        runCatching { withContext(Dispatchers.IO) { c.listGroups(t, mcpttId) } }
            .onSuccess { _groups.value = it; _status.value = "그룹 ${it.size}개" }
            .onFailure { _status.value = "그룹 조회 실패: ${it.message}" }
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
            headers = mapOf("Expires" to if (on) "3600" else "0"),
        )
        _status.value = if (on) "affiliate $groupId" else "de-affiliate $groupId"
    }

    /** 키업 그룹콜 — multipart INVITE + floor SDP 주입. [members] 비우면 resource-lists 생략. */
    fun startGroupCall(groupId: String, members: List<McpttXml.ResourceEntry> = emptyList()) {
        floor?.close()
        val f = FloorClient(ssrc, mcpttId, localPort = 0, onEvent = ::onFloorEvent).also { floor = it }
        val appSdp = "m=application ${f.localPort} UDP MCPTT\r\na=floorid:0 mstrm:audio"

        val parts = ArrayList<SipBodyPart>()
        parts.add(SipBodyPart("application", "vnd.3gpp.mcptt-info+xml",
            McpttXml.mcpttInfo(McpttXml.SessionType.PREARRANGED, "tel:$groupId", mcpttId, "tel:$groupId")))
        if (members.isNotEmpty())
            parts.add(SipBodyPart("application", "resource-lists+xml", McpttXml.resourceLists(members)))

        _floorState.value = FloorState.IDLE
        sip.makeGroupCall("sip:$groupId@${sipConfig.domain}", parts, appSdp)
        _status.value = "그룹콜 $groupId (floor localPort=${f.localPort})"
    }

    fun hangup() {
        if (activeCallId >= 0) sip.hangup(activeCallId)
        floor?.close(); floor = null
        _floorState.value = FloorState.IDLE
    }

    // ── PTT 버튼 ──

    /** PTT down — Floor Request. GRANT 수신 시에만 실제 발화(mic on). */
    fun pttDown() {
        floor?.requestFloor(priority = 0)
        _floorState.value = FloorState.REQUESTING
    }

    /** PTT up — Floor Release + mic off. */
    fun pttUp() {
        floor?.releaseFloor()
        if (activeCallId >= 0) sip.setMicEnabled(activeCallId, false)
        _floorState.value = FloorState.IDLE
    }

    private fun onFloorEvent(ev: FloorEvent) {
        when (ev) {
            is FloorEvent.Granted -> { if (activeCallId >= 0) sip.setMicEnabled(activeCallId, true); _floorState.value = FloorState.SPEAKING; _status.value = "발언권 획득" }
            is FloorEvent.Denied -> { if (activeCallId >= 0) sip.setMicEnabled(activeCallId, false); _floorState.value = FloorState.IDLE; _status.value = "발언권 거부: ${ev.text ?: ev.cause}" }
            is FloorEvent.Revoked -> { if (activeCallId >= 0) sip.setMicEnabled(activeCallId, false); _floorState.value = FloorState.IDLE; _status.value = "발언권 회수: ${ev.text ?: ev.cause}" }
            is FloorEvent.Taken -> { _floorState.value = FloorState.LISTENING; _status.value = "화자: ${ev.speaker ?: "?"}" }
            FloorEvent.Idle -> { _floorState.value = FloorState.IDLE }
            is FloorEvent.QueuePosition -> { _floorState.value = FloorState.QUEUED; _status.value = "대기열 ${ev.position}" }
            is FloorEvent.Other -> Log.d(TAG, "floor other ${ev.type}")
        }
    }

    fun shutdown() {
        floor?.close(); floor = null
        sip.shutdown()
    }

    private companion object { const val TAG = "PttController" }
}
