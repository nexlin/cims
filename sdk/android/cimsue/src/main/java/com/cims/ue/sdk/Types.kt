// libcimsue Kotlin 파사드 — 값 타입 (docs/design/features/android_dispatch_tablet.md §4)
//
// 코어의 공개 타입을 Kotlin 관용구(enum class·data class)로 옮긴다. 앱은 이 타입만 보고
// `com.cims.ue.sdk.jni.*` 는 보지 않는다. 이름·의미는 Windows .NET 파사드와 같은 묶음을 쓴다
// (sdk/windows/dotnet/CimsUe/Types.cs) — 두 플랫폼 앱이 같은 형태로 읽히게 하기 위해서다.
package com.cims.ue.sdk

import com.cims.ue.sdk.jni.AudioDeviceVector
import com.cims.ue.sdk.jni.DriveAccountVector
import com.cims.ue.sdk.jni.MediaSourceVector
import com.cims.ue.sdk.jni.RosterVector
import com.cims.ue.sdk.jni.TalkerVector
import com.cims.ue.sdk.jni.VideoDeviceVector
import com.cims.ue.sdk.jni.AccountConfig as JniAccountConfig
import com.cims.ue.sdk.jni.AudioDeviceInfo as JniAudioDeviceInfo
import com.cims.ue.sdk.jni.CallInfo as JniCallInfo
import com.cims.ue.sdk.jni.CallOptions as JniCallOptions
import com.cims.ue.sdk.jni.DeviceLinkConfig as JniDeviceLinkConfig
import com.cims.ue.sdk.jni.DialogInfo as JniDialogInfo
import com.cims.ue.sdk.jni.DriveAccount as JniDriveAccount
import com.cims.ue.sdk.jni.DriveOptions as JniDriveOptions
import com.cims.ue.sdk.jni.EmergencyAlert as JniEmergencyAlert
import com.cims.ue.sdk.jni.LinkState as JniLinkState
import com.cims.ue.sdk.jni.EngineConfig as JniEngineConfig
import com.cims.ue.sdk.jni.FdFile as JniFdFile
import com.cims.ue.sdk.jni.FloorEvent as JniFloorEvent
import com.cims.ue.sdk.jni.FloorInfo as JniFloorInfo
import com.cims.ue.sdk.jni.GroupCallOptions as JniGroupCallOptions
import com.cims.ue.sdk.jni.McpttCondition as JniMcpttCondition
import com.cims.ue.sdk.jni.McpttInfo as JniMcpttInfo
import com.cims.ue.sdk.jni.MediaSource as JniMediaSource
import com.cims.ue.sdk.jni.RegInfo as JniRegInfo
import com.cims.ue.sdk.jni.RequestResult as JniRequestResult
import com.cims.ue.sdk.jni.Result as JniResult
import com.cims.ue.sdk.jni.SdsMessage as JniSdsMessage
import com.cims.ue.sdk.jni.SdsSend as JniSdsSend
import com.cims.ue.sdk.jni.StreamStats as JniStreamStats
import com.cims.ue.sdk.jni.CallQuality as JniCallQuality
import com.cims.ue.sdk.jni.QualityDirection as JniQualityDirection
import com.cims.ue.sdk.jni.StringVector
import com.cims.ue.sdk.jni.TlsPeerExpiry as JniTlsPeerExpiry
import com.cims.ue.sdk.jni.ReceptionEvent as JniReceptionEvent
import com.cims.ue.sdk.jni.TransmissionEvent as JniTransmissionEvent
import com.cims.ue.sdk.jni.TransmissionInfo as JniTransmissionInfo
import com.cims.ue.sdk.jni.VideoGroupCallOptions as JniVideoGroupCallOptions
import com.cims.ue.sdk.jni.VideoTransmitter as JniVideoTransmitter
import com.cims.ue.sdk.jni.VideoTransmitterVector

// ── 명령 결과 ────────────────────────────────────────────────────────────────
/** 명령의 즉시 결과(인자·상태 오류). 프로토콜 결과는 이벤트로 온다.
 *  이름이 `CimsResult` 인 것은 Kotlin 표준 `Result` 와 겹치지 않게 하기 위해서다. */
data class CimsResult<out T>(val ok: Boolean, val code: Int, val reason: String, val value: T?) {
    val failed: Boolean get() = !ok
    /** 실패면 null. 성공했는데 값이 없는 명령(Unit)은 Unit 을 돌려준다. */
    fun getOrNull(): T? = if (ok) value else null
    inline fun <R> map(f: (T) -> R): CimsResult<R> =
        if (ok && value != null) CimsResult(true, code, reason, f(value)) else CimsResult(false, code, reason, null)

    companion object {
        fun <T> ok(value: T): CimsResult<T> = CimsResult(true, 0, "", value)
        fun <T> fail(code: Int, reason: String): CimsResult<T> = CimsResult(false, code, reason, null)
        internal fun of(r: JniResult): CimsResult<Unit> =
            if (r.ok) ok(Unit) else fail(r.code, r.reason)
        internal fun <T> of(r: JniResult, value: T): CimsResult<T> =
            if (r.ok) ok(value) else fail(r.code, r.reason)
    }
}

// ── enum ─────────────────────────────────────────────────────────────────────
// SWIG 은 Java enum 이 아니라 typesafe-enum 클래스를 낸다(swigValue()). 서수는 코어 헤더 순서 그대로다.
enum class Transport { UDP, TCP, TLS }
enum class AuthScheme { DIGEST, AKA }
enum class MediaSecurity { OFF, OPTIONAL, REQUIRED }
enum class RegState { UNREGISTERED, REGISTERING, REGISTERED, FAILED }
enum class CallState { NULL, OUTGOING, INCOMING, ACTIVE, HELD, DISCONNECTED }
enum class CallDir { OUTGOING, INCOMING }
enum class FloorState { IDLE, REQUESTING, SPEAKING, LISTENING, QUEUED }
/** floor 이벤트 종류 — 상태만으로는 Denied·Revoked·코어 시한(RequestTimeout·TalkLimit)을 가를 수 없다. 서수 = 코어 FloorEvent::Kind. */
enum class FloorEventKind {
    GRANTED, DENIED, IDLE, TAKEN, TALKER_LEFT, REVOKED, QUEUE_POSITION, QUEUE_CANCELLED,
    /** 요청 후 응답 없음(코어 타이머) → Idle 복귀. */
    REQUEST_TIMEOUT,
    /** Granted Duration 마감 임박/도달 — 코어가 스스로 Release. */
    TALK_LIMIT,
    OTHER,
}
/** MCPTT 세션 조건 변화의 계기(서수 = 코어 ConditionCause) — LOCAL = setCondition 을 보내며 반영, CONFIRMED = 그 re-INVITE 2xx,
 *  DENIED = 4xx~6xx(이전 값 복원, 미인가 상향 403), ADVERTISED = 서버 재광고(TS 24.379 §6.3.3.1.6·§6.3.3.1.10·§6.3.3.1.15). */
enum class ConditionCause { LOCAL, CONFIRMED, DENIED, ADVERTISED }
/** Floor Indicator 비트(TS 24.380 §8.2.3.15) — `FloorEvent.indicator`·`FloorInfo.indicator` 판정용. 값의 정본은 floor 정의 테이블
 *  (docs/design/features/mcptt_floor_defs.yaml)이고 `scripts/gen_floor_defs.py --check` 가 이 블록을 대조한다. */
object FloorIndicator {
    const val NORMAL = 0x8000
    const val BROADCAST_GROUP = 0x4000
    const val SYSTEM = 0x2000
    const val EMERGENCY = 0x1000
    const val IMMINENT_PERIL = 0x0800
    const val QUEUEING = 0x0400
    const val DUAL_FLOOR = 0x0200
    const val TEMPORARY_GROUP = 0x0100
    const val MULTI_TALKER = 0x0080
}

/** 오디오 라우트 — 입력의 EARPIECE = 내장 기본(하단) 마이크 고정, DEFAULT = 정책(고정 해제). 서수 = 코어 AudioRoute. */
enum class AudioRoute { DEFAULT, EARPIECE, LOUDSPEAKER }
/** MC 서비스(서수 = 코어 McService) — 호·affiliation 은 서비스마다 따로다(TS 23.280 §5.2.5). */
enum class McService { MCPTT, MCVIDEO }
/** MCVideo 내 송출 상태(TS 24.581 §6.2.4 'U: …', 서수 = 코어 TransmissionState). */
enum class TransmissionState { NO_PERMISSION, PENDING_REQUEST, PERMITTED, PENDING_END, QUEUED }
/** 한 송출의 내 수신 상태(§6.2.5, 서수 = 코어 ReceptionState). */
enum class ReceptionState { NOTIFIED, PENDING_REQUEST, RECEIVING, PENDING_RELEASE, ENDED }
/** 송출 제어 이벤트 종류(서수 = 코어 TransmissionEvent::Kind). */
enum class TransmissionEventKind {
    GRANTED, REJECTED, REVOKED, QUEUE_POSITION, END_REQUESTED, ENDED, RECEIVER_JOINED, IDLE, QUEUE_CANCELLED, REQUEST_TIMEOUT, OTHER,
}
/** 수신 제어 이벤트 종류(서수 = 코어 ReceptionEvent::Kind). */
enum class ReceptionEventKind { NOTIFIED, GRANTED, REJECTED, ENDED, RELEASED, END_REQUESTED, REQUEST_TIMEOUT, OTHER }
/** 계측 링크 상태(cimsue/drive.h) — REFUSED(연결 키 거절·지문 불일치)는 다시 붙지 않는다. */
enum class LinkState { IDLE, CONNECTING, CONNECTED, DISCONNECTED, REFUSED }

private inline fun <reified E : Enum<E>> ordinalOf(v: Int): E {
    val vs = enumValues<E>()
    return if (v in vs.indices) vs[v] else vs[0]
}

// ── 설정 ─────────────────────────────────────────────────────────────────────
/** 엔진(프로세스당 1개) 설정. */
data class EngineConfig(
    val userAgent: String = "CIMS-UE",
    val logLevel: Int = 3,
    /** SIP TLS·HTTPS 가 같이 쓰는 신뢰 앵커(PEM). 비면 시스템 기본. */
    val tlsCaPem: String = "",
    val tlsVerifyServer: Boolean = true,
    val nullAudioDevice: Boolean = false,
    /** UDP→TCP 승격(RFC 3261 §18.1.1) 비활성 — 통제된 망 전용 사이트 옵션(libcimsue EngineConfig.udpNoTcpSwitch). */
    val udpNoTcpSwitch: Boolean = false,
    /** VAD(무음 억제) 비활성 — 침묵 중에도 RTP 를 연속 송신해 NAT flow 를 유지한다. 코어·.NET 기본값과 같다(true). */
    val noVad: Boolean = true,
    val udpPort: Int = 0,
    val tcpPort: Int = 0,
    val tlsPort: Int = 0,
    val clockRate: Long = 16000L,
    /** Floor Granted 뒤 마이크 개방 지연(ms) — 앱의 승인 톤 길이(«삑 후 말하기»). 그 사이 발언을 잃으면 열지 않는다. 0 = 즉시. */
    val grantMicDelayMs: Int = 0,
) {
    internal fun toJni(): JniEngineConfig = JniEngineConfig().also {
        it.userAgent = userAgent; it.logLevel = logLevel
        it.tlsCaPem = tlsCaPem; it.tlsVerifyServer = tlsVerifyServer
        it.nullAudioDevice = nullAudioDevice; it.noVad = noVad; it.udpNoTcpSwitch = udpNoTcpSwitch
        it.udpPort = udpPort; it.tcpPort = tcpPort; it.tlsPort = tlsPort
        it.clockRate = clockRate
        it.grantMicDelayMs = grantMicDelayMs
    }
}

/** 접속서비스 kind 당 계정 하나. 인증 자료는 H(A1) 우선 — 평문 비밀번호가 필요 없다. */
data class AccountConfig(
    val serverHost: String,
    val serverPort: Int = 5060,
    val transport: Transport = Transport.UDP,
    val domain: String,
    val msisdn: String,
    val imsi: String = "",
    val authId: String = "",
    val displayName: String = "",
    val ha1: String = "",
    val password: String = "",
    val authScheme: AuthScheme = AuthScheme.DIGEST,
    val akaK: String = "",
    val akaOpc: String = "",
    val akaAmf: String = "8000",
    val secMechanisms: List<String> = emptyList(),
    val mediaSecurity: MediaSecurity = MediaSecurity.OFF,
    val expiresSec: Int = 3600,
    val contactParams: String = "",
    val videoAutoTransmit: Boolean = false,
    val mcpttId: String = "",
    /** MCPTT 착신 자동 수락. 관제석은 그룹콜 자동 + 사설콜 수동이 맞지만 코어가 아직 공통이다(§11). */
    val autoAnswerMcptt: Boolean = true,
    /** REGISTER Contact +sip.instance(TS 24.229 §5.1.1.2) — 꺾쇠 없는 URN. 빈 값이면 pjsip 기본값(호스트명 해시). */
    val instanceId: String = "",
    /** MCPTT client ID(TS 24.379 §4.10) — 처음 쓸 때 만든 UUID URN 을 보존해 넘긴다. 비면 instanceId 가 urn:uuid: 일 때 그것. */
    val mcpttClientId: String = "",
    /** Resource-Priority(RFC 8101) — service-config OnNetwork 의 *-resource-priority(TS 24.379 §6.2.8.1.15). 기본 = CSP 와 같은 mcpttp 서열. */
    val rpEmergency: String = "mcpttp.15",
    val rpImminentPeril: String = "mcpttp.8",
    val rpNormal: String = "mcpttp.0",
    /** 그룹 SDS 의 시그널링 평면 상한(octet, 프로비저닝 mcdata.maxPayloadSdsCplaneBytes) — 넘으면 `sendGroupSds` 가 MSRP(TS 24.282 §9.2.3)로
     *  보내고 최종 결과는 `requestResult` 의 method "MSRP" 로 온다. 0 = 제한 없음. */
    val maxSdsCplaneBytes: Int = 0,
    /** 서버발 MSRP 배포 수신 — REGISTER Contact 에 ICSI mcdata.sds 를 싣는다(코어가 contactParams 에 합친다). 끄면 서버가 FILEURL 로 폴백. */
    val mcdataMsrp: Boolean = false,
    /** 참여 MCPTT 기능 PSI — 긴급 경보 Request-URI(TS 24.379 §12.1.1.1 8)). 정본 = ue-init-config [UeInitConfigDoc.mcpttServerUri]
     *  (TS 24.484 §7.2). 비면 그룹 URI(옛 서버 전환기). */
    val mcpttServerUri: String = "",
    /** 참여 MCData 기능 PSI — SDS disposition 통지 Request-URI(TS 24.282 §12.2.1.1). 정본 = [UeInitConfigDoc.mcdataServerUri].
     *  비면 원 발신자 AoR 로 곧장(CSP 0.2.180 전 서버 전환기). */
    val mcdataServerUri: String = "",
    /** MCVideo 서비스 사용 — REGISTER Contact 에 MCVideo 태그(TS 24.281 §7.2.1AA). 빼고 다시 등록하면 MCVideo 로그오프. */
    val mcvideoEnabled: Boolean = false,
    /** 참여 MCVideo 기능 PSI — MCVideo 그룹 호·affiliation Request-URI. 정본 = [UeInitConfigDoc.mcvideoServerUri]. */
    val mcvideoServerUri: String = "",
    /** MCVideo 멤버 초대(prearranged) 자동 수락(§6.2.3.1.2) — 수락은 세션 합류일 뿐, 영상 보기는 수신 제어([Call.acceptReception]). */
    val autoAnswerMcvideo: Boolean = true,
) {
    internal fun toJni(): JniAccountConfig = JniAccountConfig().also {
        it.serverHost = serverHost; it.serverPort = serverPort
        it.transport = com.cims.ue.sdk.jni.Transport.swigToEnum(transport.ordinal)
        it.domain = domain; it.msisdn = msisdn; it.imsi = imsi; it.authId = authId
        it.displayName = displayName; it.ha1 = ha1; it.password = password
        it.authScheme = com.cims.ue.sdk.jni.AuthScheme.swigToEnum(authScheme.ordinal)
        it.akaK = akaK; it.akaOpc = akaOpc; it.akaAmf = akaAmf
        it.secMechanisms = StringVector().apply { secMechanisms.forEach { s -> add(s) } }
        it.mediaSecurity = com.cims.ue.sdk.jni.MediaSecurity.swigToEnum(mediaSecurity.ordinal)
        it.expiresSec = expiresSec; it.contactParams = contactParams
        it.videoAutoTransmit = videoAutoTransmit; it.mcpttId = mcpttId
        it.autoAnswerMcptt = autoAnswerMcptt; it.instanceId = instanceId
        it.mcpttClientId = mcpttClientId
        it.rpEmergency = rpEmergency; it.rpImminentPeril = rpImminentPeril; it.rpNormal = rpNormal
        it.maxSdsCplaneBytes = maxSdsCplaneBytes; it.mcdataMsrp = mcdataMsrp
        it.mcpttServerUri = mcpttServerUri; it.mcdataServerUri = mcdataServerUri
        it.mcvideoEnabled = mcvideoEnabled; it.mcvideoServerUri = mcvideoServerUri; it.autoAnswerMcvideo = autoAnswerMcvideo
    }
}

data class CallOptions(val video: Boolean = false, val emergency: Boolean = false) {
    internal fun toJni(): JniCallOptions = JniCallOptions().also {
        it.video = video; it.emergency = emergency
    }
}

data class GroupCallOptions(
    val emergency: Boolean = false,
    val imminentPeril: Boolean = false,
    /** 청취 전용 합류(a=recvonly) — 관제 PTT 청취. */
    val listenOnly: Boolean = false,
    /** 사설콜 전이중(mc_no_floor_ctrl). */
    val fullDuplex: Boolean = false,
    /** 애드혹 참가자 목록(resource-lists). */
    val members: List<String> = emptyList(),
    /** 일제 통화 개시(mcptt-info broadcast-ind, TS 24.379 §4.12) — 개시자만 발언, 발언을 놓으면 코어가 호를 해제. */
    val broadcast: Boolean = false,
    /** 암묵적 발언 요청(TS 24.380 §14.2.5 mc_implicit_request + §14.2.4 mc_granted) — 개시 INVITE 가 발언 요청을 싣는다. */
    val implicitFloorRequest: Boolean = false,
) {
    internal fun toJni(): JniGroupCallOptions = JniGroupCallOptions().also {
        it.emergency = emergency; it.imminentPeril = imminentPeril
        it.listenOnly = listenOnly; it.fullDuplex = fullDuplex; it.broadcast = broadcast
        it.implicitFloorRequest = implicitFloorRequest
        it.members = StringVector().apply { members.forEach { m -> add(m) } }
    }
}

/** MCVideo 그룹 호 개시·합류 옵션(TS 24.281 §9.2.1 prearranged · §9.2.2 chat, 제어 채널 fmtp TS 24.581 §14.2). */
data class VideoGroupCallOptions(
    /** session-type prearranged(false = chat). 그룹 문서 mcvideo-on-network-invite-members 와 맞아야 한다(어긋나면 404 Warning 117·118). */
    val prearranged: Boolean = false,
    /** 송출 요청 대기열 지원(mc_queueing). */
    val queueing: Boolean = false,
    /** 요청할 최대 송출 우선순위 1~255(mc_priority), <0 = 미기재. */
    val maxPriority: Int = -1,
    /** 요청할 최대 수신 우선순위 1~255(mc_reception_priority), <0 = 미기재. */
    val maxReceptionPriority: Int = -1,
    /** 호 성립과 함께 송출 요청(mc_implicit_request + mc_granted). 서버가 받지 않으면 코어가 명시 요청으로 잇는다. */
    val implicitTransmissionRequest: Boolean = false,
    /** 진행 중 세션 재합류(§9.2.1.2.4) — 앞 호의 [CallInfo.sessionUri]. 빈 값 = 새 합류. */
    val sessionUri: String = "",
) {
    internal fun toJni(): JniVideoGroupCallOptions = JniVideoGroupCallOptions().also {
        it.prearranged = prearranged; it.queueing = queueing
        it.maxPriority = maxPriority; it.maxReceptionPriority = maxReceptionPriority
        it.implicitTransmissionRequest = implicitTransmissionRequest; it.sessionUri = sessionUri
    }
}

// ── 스냅샷·이벤트 ────────────────────────────────────────────────────────────
data class LogLine(val level: Int, val message: String)

data class RegInfo(val accountId: Int, val state: RegState, val code: Int, val reason: String, val expiresSec: Int) {
    val registered: Boolean get() = state == RegState.REGISTERED
    internal companion object {
        fun of(r: JniRegInfo) = RegInfo(r.accountId, ordinalOf(r.state.swigValue()), r.code, r.reason, r.expiresSec)
    }
}

/** U10 서브스트림 — 감청 leg 은 RFC 5576 `a=ssrc … label` 로 발신자/착신자가 구분된다. */
data class MediaSource(val ssrc: Long, val label: String, val active: Boolean, val level: Float) {
    internal companion object {
        fun of(m: JniMediaSource) = MediaSource(m.ssrc, m.label, m.active, m.level)
        fun list(v: MediaSourceVector): List<MediaSource> = List(v.size) { of(v[it]) }
    }
}

/** MCPTT 세션 신원 — 발신 첫 스냅샷부터 확정돼 있고 이후 바뀌지 않는다(ue_sdk.md §4.2). */
data class McpttInfo(
    val present: Boolean, val sessionType: String, val requestUri: String,
    val callingUserId: String, val callingGroupId: String,
    val emergency: Boolean, val imminentPeril: Boolean,
    val privateCall: Boolean, val noFloorCtrl: Boolean,
    /** broadcast-ind — 일제 통화(그룹 종류가 아니라 호 속성). */
    val broadcast: Boolean = false,
) {
    internal companion object {
        fun of(m: JniMcpttInfo) = McpttInfo(m.present, m.sessionType, m.requestUri, m.callingUserId,
            m.callingGroupId, m.emergency, m.imminentPeril, m.privateCall, m.noFloorCtrl, m.broadcast)
    }
}

data class CallInfo(
    val callId: Int, val accountId: Int, val dir: CallDir, val state: CallState,
    val remoteUri: String, val calledParty: String,
    val video: Boolean, val mediaActive: Boolean, val muted: Boolean, val listen: Boolean,
    val playbackRoute: Int, val lastCode: Int, val lastReason: String,
    val sources: List<MediaSource>,
    val isMcptt: Boolean, val groupId: String, val mcptt: McpttInfo,
    val halfDuplex: Boolean, val listenOnly: Boolean,
    /** Join(RFC 3911)으로 합류한 감청 leg 이면 대상 dialog id. */
    val joinedDialog: String,
    /** 세션 조건 현재값(긴급·임박) — 바뀌면 `condition` 이벤트. [mcptt] 는 개시·착신 INVITE 의 값(불변)이다. */
    val condition: McpttCondition = McpttCondition(),
    /** 이 호에서 듣는 크기(setRxLevel) — 코어가 기억해 재결선마다 다시 건다. */
    val rxLevel: Float = 1f,
    /** 내 영상 송출 허용([Call.setVideoSend]) — MCPTT 반이중은 허용이면서 발언권을 가진 동안만 보낸다. [video] 는 협상된 영상 활성. */
    val videoSend: Boolean = true,
    /** MC 호의 서비스 — MCVideo 그룹 호면 MCVIDEO(그때 [isMcptt] 는 false, 제어는 전송 제어 — `transmission`·`reception` 이벤트). */
    val service: McService = McService.MCPTT,
    /** MC 세션 식별자 — 제어 기능 Contact(isfocus)의 세션 URI, 재합류([VideoGroupCallOptions.sessionUri])에 쓴다. */
    val sessionUri: String = "",
) {
    val active: Boolean get() = state == CallState.ACTIVE
    val ended: Boolean get() = state == CallState.DISCONNECTED
    internal companion object {
        fun of(c: JniCallInfo) = CallInfo(
            c.callId, c.accountId, ordinalOf(c.dir.swigValue()), ordinalOf(c.state.swigValue()),
            c.remoteUri, c.calledParty, c.video, c.mediaActive, c.muted, c.listen,
            c.playbackRoute, c.lastCode, c.lastReason, MediaSource.list(c.sources),
            c.isMcptt, c.groupId, McpttInfo.of(c.mcptt), c.halfDuplex, c.listenOnly, c.joinedDialog,
            McpttCondition.of(c.condition), c.rxLevel, c.videoSend,
            ordinalOf(c.service.swigValue()), c.sessionUri)
    }
}

/** MCPTT 세션 조건 — 그룹의 긴급·임박 상태를 이 호에서 본 값(TS 24.379 §6.2.8.1). mine = 이 단말이 올린 조건. */
data class McpttCondition(
    val emergency: Boolean = false, val imminentPeril: Boolean = false,
    val mine: Boolean = false, val pending: Boolean = false,
    /** 마지막 상향·하향 re-INVITE 최종 응답(CONFIRMED·DENIED). */
    val lastCode: Int = 0,
) {
    internal companion object {
        fun of(c: JniMcpttCondition) = McpttCondition(c.emergency, c.imminentPeril, c.mine, c.pending, c.lastCode)
    }
}

/** MCVideo 한 송출 — 송출자 한 명의 audio·video 흐름 쌍(Media Transmission Notification §9.2.13). userId 가 [Call.acceptReception] 인자. */
data class VideoTransmitter(
    val userId: String, val audioSsrc: Long, val videoSsrc: Long, val functionalAlias: String,
    /** Reception Mode '0' — 서버가 곧바로 수신 허가(긴급·임박·방송·system 호). */
    val automatic: Boolean, val state: ReceptionState,
) {
    internal companion object {
        fun of(t: JniVideoTransmitter) = VideoTransmitter(t.userId, t.audioSsrc, t.videoSsrc, t.functionalAlias, t.automatic,
            ordinalOf(t.state.swigValue()))
        fun list(v: VideoTransmitterVector): List<VideoTransmitter> = List(v.size) { of(v[it]) }
    }
}

/** MCVideo 송출 제어 이벤트(TS 24.581 §6.2.4) — 송출(마이크·카메라) 게이트는 코어가 이미 처리했다. */
data class TransmissionEvent(
    val kind: TransmissionEventKind, val callId: Int, val state: TransmissionState,
    val cause: Int, val causeText: String, val durationSec: Int, val priority: Int, val queuePosition: Int, val indicator: Int,
    val audioSsrc: Long, val videoSsrc: Long, val receiverId: String, val rawType: Int,
) {
    internal companion object {
        fun of(e: JniTransmissionEvent) = TransmissionEvent(ordinalOf(e.kind.swigValue()), e.callId, ordinalOf(e.state.swigValue()),
            e.cause, e.causeText, e.durationSec, e.priority, e.queuePosition, e.indicator, e.audioSsrc, e.videoSsrc, e.receiverId, e.rawType)
    }
}

/** MCVideo 수신 제어 이벤트(§6.2.5) — 새 송출 알림(manual 이면 앱이 [받기])·수신 허가·종료. */
data class ReceptionEvent(
    val kind: ReceptionEventKind, val callId: Int, val transmitter: VideoTransmitter, val cause: Int, val causeText: String, val rawType: Int,
) {
    internal companion object {
        fun of(e: JniReceptionEvent) = ReceptionEvent(ordinalOf(e.kind.swigValue()), e.callId, VideoTransmitter.of(e.transmitter),
            e.cause, e.causeText, e.rawType)
    }
}

/** MCVideo 호의 전송 제어 현재값 — transmitters = 알려진 송출(내 것 제외). */
data class TransmissionInfo(
    val state: TransmissionState, val transmitters: List<VideoTransmitter>, val queuePosition: Int,
    val localPort: Int, val remoteIp: String, val remotePort: Int,
) {
    internal companion object {
        fun of(t: JniTransmissionInfo) = TransmissionInfo(ordinalOf(t.state.swigValue()), VideoTransmitter.list(t.transmitters),
            t.queuePosition, t.localPort, t.remoteIp, t.remotePort)
    }
}

/** 조건 변화 이벤트 — call.condition 이 새 값. */
data class ConditionChange(val call: CallInfo, val cause: ConditionCause)

/**
 * 긴급 경보·취소·그룹 긴급 통지 수신(TS 24.379 §12.1.1.3). 지시자는 1 = true, -1 = false, 0 = 요소 없음.
 * alertInd 0 은 경보 없는 그룹 상태 통지다. self = 발신자가 이 계정(에코).
 */
data class EmergencyAlert(
    val accountId: Int, val groupId: String, val userId: String, val originatedBy: String, val mcOrg: String,
    val alertInd: Int, val emergencyInd: Int, val imminentPerilInd: Int, val self: Boolean,
) {
    internal companion object {
        fun of(a: JniEmergencyAlert) = EmergencyAlert(a.accountId, a.groupId, a.userId, a.originatedBy, a.mcOrg,
            a.alertInd, a.emergencyInd, a.imminentPerilInd, a.self)
    }
}

/** 다중 화자(U10) — self 는 내 발언. */
data class Talker(val id: String, val ssrc: Long, val self: Boolean) {
    internal companion object {
        fun list(v: TalkerVector): List<Talker> = List(v.size) {
            val t = v[it]; Talker(t.id, t.ssrc, t.self)
        }
    }
}

data class FloorEvent(
    val kind: FloorEventKind,
    val callId: Int, val state: FloorState, val durationSec: Int,
    val cause: Int, val causeText: String, val indicator: Int,
    /** Floor Taken 의 Permission to Request the Floor — 0 이면 앱이 발언 버튼을 비활성한다. */
    val permission: Int, val queuePosition: Int, val meSpeaking: Boolean,
    val talkers: List<Talker>, val rawType: Int,
) {
    internal companion object {
        fun of(e: JniFloorEvent) = FloorEvent(ordinalOf(e.kind.swigValue()), e.callId, ordinalOf(e.state.swigValue()), e.durationSec,
            e.cause, e.causeText, e.indicator, e.permission, e.queuePosition, e.meSpeaking,
            Talker.list(e.talkers), e.rawType)
    }
}

data class FloorInfo(
    val state: FloorState, val talkers: List<Talker>, val canRequest: Boolean,
    val indicator: Int, val queuePosition: Int,
    val localPort: Int, val remoteIp: String, val remotePort: Int,
    val grantedCount: Long, val takenCount: Long, val denyCount: Long,
) {
    internal companion object {
        fun of(f: JniFloorInfo) = FloorInfo(ordinalOf(f.state.swigValue()), Talker.list(f.talkers),
            f.canRequest, f.indicator, f.queuePosition, f.localPort, f.remoteIp, f.remotePort,
            f.grantedCount, f.takenCount, f.denyCount)
    }
}

/** 감시 대상 dialog(RFC 4235) — 관제 BLF·Join 대상 식별. */
data class DialogInfo(
    val accountId: Int, val watched: String, val id: String,
    val callId: String, val localTag: String, val remoteTag: String,
    val direction: String, val state: String, val remoteIdentity: String, val full: Boolean,
) {
    internal companion object {
        fun of(d: JniDialogInfo) = DialogInfo(d.accountId, d.watched, d.id, d.callId, d.localTag,
            d.remoteTag, d.direction, d.state, d.remoteIdentity, d.full)
    }
    internal fun toJni(): JniDialogInfo = JniDialogInfo().also {
        it.accountId = accountId; it.watched = watched; it.id = id
        it.callId = callId; it.localTag = localTag; it.remoteTag = remoteTag
        it.direction = direction; it.state = state; it.remoteIdentity = remoteIdentity; it.full = full
    }
}

data class RosterEntry(val uri: String, val status: String)
/** 그룹 로스터 갱신(RFC 4575). full=전체 스냅샷, 아니면 부분 갱신. */
data class RosterUpdate(val accountId: Int, val groupId: String, val users: List<RosterEntry>, val full: Boolean) {
    internal companion object {
        fun of(accountId: Int, groupId: String, v: RosterVector, full: Boolean) = RosterUpdate(
            accountId, groupId, List(v.size) { val r = v[it]; RosterEntry(r.uri, r.status) }, full)
    }
}

data class SdsMessage(
    val accountId: Int, val fromUri: String, val groupUri: String,
    val convId: String, val msgId: String, val timeSec: Long,
    val dispositionReq: Int, val text: String,
    val notification: Boolean, val notifType: Int,
    val fd: Boolean, val fileUrl: String, val fileName: String, val fileType: String, val fileSize: Long,
    /** media plane(MSRP) 배포로 받았다(TS 24.282 §9.2.3). */
    val mediaPlane: Boolean = false,
) {
    internal companion object {
        fun of(m: JniSdsMessage) = SdsMessage(m.accountId, m.fromUri, m.groupUri, m.convId, m.msgId,
            m.timeSec, m.dispositionReq, m.text, m.notification, m.notifType,
            m.fd, m.fileUrl, m.fileName, m.fileType, m.fileSize, m.mediaPlane)
    }
}

/** MCData FD 로 알릴 파일(TS 24.282 FD SIGNALLING — FILEURL·Metadata). url = [CscClient.uploadFd] 결과, type = MIME(빈 값 = application/octet-stream). */
data class FdFile(val url: String, val name: String, val type: String = "", val size: Long = 0) {
    internal fun toJni(): JniFdFile = JniFdFile().also { it.url = url; it.name = name; it.type = type; it.size = size }
}

/** FD 업로드 결과(POST /mcdata/fd 201) — url 을 [FdFile.url] 로 넘긴다. */
data class FdUpload(val id: String, val url: String, val name: String, val size: Long)

/** SDS 발신의 즉시 결과 — 최종 응답은 `requestResult` 에 같은 token 으로 온다. */
data class SdsSend(val msgId: String, val token: Long) {
    internal companion object {
        fun of(s: JniSdsSend): CimsResult<SdsSend> =
            if (s.ok) CimsResult.ok(SdsSend(s.msgId, s.token)) else CimsResult.fail(s.code, s.reason)
    }
}

/** 임의 요청(PUBLISH/MESSAGE/SUBSCRIBE)의 최종 응답 — token 으로 발신과 상관한다. */
data class RequestResult(val accountId: Int, val token: Long, val method: String,
                         val code: Int, val reason: String, val etag: String) {
    internal companion object {
        fun of(r: JniRequestResult) = RequestResult(r.accountId, r.token, r.method, r.code, r.reason, r.etag)
    }
}

/** MCData 가 아닌 MESSAGE/NOTIFY 본문(xcap-diff 등) — 앱이 해석한다. */
data class SipMessage(val accountId: Int, val fromUri: String, val contentType: String, val body: String)

data class StreamStats(val rxPackets: Long, val rxBytes: Long, val rxLoss: Long, val rxDiscard: Long,
                       val txPackets: Long, val txBytes: Long, val valid: Boolean) {
    internal companion object {
        fun of(s: JniStreamStats) = StreamStats(s.rxPackets, s.rxBytes, s.rxLoss, s.rxDiscard,
            s.txPackets, s.txBytes, s.valid)
    }
}

/** 호 품질 한 방향 — rx = 내가 받은 스트림, remote = 상대가 받은 내 스트림(상대 RTCP RR·XR). 비율 %, 값 없음 = -1, 레벨 127 = 없음. */
data class QualityDirection(val valid: Boolean, val packets: Long, val lost: Long, val discarded: Long,
                            val lossPct: Double, val discardPct: Double, val jitterMs: Double, val jitterMaxMs: Double,
                            val burstDensityPct: Double, val gapDensityPct: Double, val burstMs: Int, val gapMs: Int,
                            val signalDbm: Int, val noiseDbm: Int) {
    internal companion object {
        fun of(d: JniQualityDirection) = QualityDirection(d.valid, d.packets, d.lost, d.discarded, d.lossPct, d.discardPct,
            d.jitterMs, d.jitterMaxMs, d.burstDensityPct, d.gapDensityPct, d.burstMs, d.gapMs, d.signalDbm, d.noiseDbm)
    }
}

/** 호 품질(ue_voice_quality.md §3) — 손실·폐기·지터·RTD(RTCP)·단말 지연과 ITU-T G.107/G.107.1 E-model 추정 R·MOS
 *  (LQ = 지연 손상 제외, CQ = 지연 포함). 값 없음 = -1. 종료된 호는 마지막 값. */
data class CallQuality(val valid: Boolean, val codec: String, val clockRate: Long, val wideband: Boolean,
                       val rx: QualityDirection, val remote: QualityDirection,
                       val rtdMs: Double, val esdMs: Double, val oneWayMs: Double,
                       val rLq: Double, val rCq: Double, val mosLq: Double, val mosCq: Double,
                       val startEpochMs: Long, val durationMs: Long) {
    internal companion object {
        fun of(q: JniCallQuality) = CallQuality(q.valid, q.codec, q.clockRate, q.wideband,
            QualityDirection.of(q.rx), QualityDirection.of(q.remote), q.rtdMs, q.esdMs, q.oneWayMs,
            q.rLq, q.rCq, q.mosLq, q.mosCq, q.startEpochMs, q.durationMs)
    }
}

// ── 시험 모드 계측 링크 (ue_voice_quality.md §5) ─────────────────────────────
/** 링크로 내보낼 회선 — 계측기는 풀의 service(volte·voip·ptt)로 `use` 한다. 첫 항목이 기본 회선. */
data class DriveAccount(val service: String, val accountId: Int, val aor: String, val msisdn: String) {
    internal fun toJni(): JniDriveAccount = JniDriveAccount().also {
        it.service = service; it.accountId = accountId; it.aor = aor; it.msisdn = msisdn
    }
}

/**
 * 계측기 워커 링크 설정(시험 모드 메뉴 — ue_voice_quality.md §4.1). 워커가 서버, 단말이 먼저 연결한다.
 * [verifyServer] 를 끄면 [pinFile] 에 최초 지문을 고정한다(TOFU). [sampleFile] = `media sample` 의 기본 WAV(PCM16).
 */
data class DeviceLinkConfig(
    val host: String,
    val port: Int = 7120,
    val pairKey: String = "",
    val verifyServer: Boolean = false,
    val caPem: String = "",
    val pinFile: String = "",
    /** 설치 고유 id — `+sip.instance` 와 같은 원천. 같은 id 로 두 번 붙으면 워커가 옛 연결을 닫는다. */
    val deviceId: String,
    val app: String,
    val appVersion: String,
    val platform: String,
    val model: String,
    val reconnectMaxSec: Int = 30,
    val sampleFile: String = "",
) {
    internal fun toJni(): JniDeviceLinkConfig = JniDeviceLinkConfig().also {
        it.host = host; it.port = port; it.pairKey = pairKey
        it.verifyServer = verifyServer; it.caPem = caPem; it.pinFile = pinFile
        it.deviceId = deviceId; it.app = app; it.appVersion = appVersion; it.platform = platform; it.model = model
        it.reconnectMaxSec = reconnectMaxSec
    }

    /** 등록 소유·호 정리 범위는 코어 링크가 고정한다(appOwnedRegistration=true·Driven) — 여기서는 회선과 기준 음원만. */
    internal fun driveOptions(accounts: List<DriveAccount>): JniDriveOptions = JniDriveOptions().also {
        it.accounts = DriveAccountVector().apply { accounts.forEach { a -> add(a.toJni()) } }
        it.sampleFile = sampleFile
    }
}

/** 링크 상태 + 사유 — detail = 워커 이름(CONNECTED) 또는 끊김·거절 사유(DISCONNECTED·REFUSED). */
data class LinkStatus(val state: LinkState, val detail: String = "") {
    internal companion object {
        fun of(s: JniLinkState, detail: String) = LinkStatus(ordinalOf(s.swigValue()), detail)
    }
}

/** 영상 장치(pjmedia videodev) — Android 카메라 driver = "Android"(Camera2). */
data class VideoDeviceInfo(val id: Int, val name: String, val driver: String, val capture: Boolean, val render: Boolean) {
    internal companion object {
        fun list(v: VideoDeviceVector): List<VideoDeviceInfo> = List(v.size) {
            val d = v[it]; VideoDeviceInfo(d.id, d.name, d.driver, d.capture, d.render)
        }
    }
}

data class AudioDeviceInfo(val id: Int, val name: String, val driver: String,
                           val inputCount: Long, val outputCount: Long) {
    internal companion object {
        fun list(v: AudioDeviceVector): List<AudioDeviceInfo> = List(v.size) {
            val d = v[it]; AudioDeviceInfo(d.id, d.name, d.driver, d.inputCount, d.outputCount)
        }
    }
}

/** SIP TLS 서버 인증서 만료 관측(sip_tls_signaling.md §8.6) — 요약 띠 경고의 입력. */
data class TlsPeerExpiry(val valid: Boolean, val notAfterEpoch: Long, val observedEpoch: Long,
                         val subject: String, val remote: String) {
    /** 관측 시점 기준 잔여 일수. valid=false 면 null. */
    val daysLeft: Int? get() = if (!valid) null
        else ((notAfterEpoch - System.currentTimeMillis() / 1000L) / 86400L).toInt()
    internal companion object {
        fun of(t: JniTlsPeerExpiry) = TlsPeerExpiry(t.valid, t.notAfterEpoch, t.observedEpoch, t.subject, t.remote)
    }
}
