// libcimsue Kotlin 파사드 — 값 타입 (docs/design/features/android_dispatch_tablet.md §4)
//
// 코어의 공개 타입을 Kotlin 관용구(enum class·data class)로 옮긴다. 앱은 이 타입만 보고
// `com.cims.ue.sdk.jni.*` 는 보지 않는다. 이름·의미는 Windows .NET 파사드와 같은 묶음을 쓴다
// (sdk/windows/dotnet/CimsUe/Types.cs) — 두 플랫폼 앱이 같은 형태로 읽히게 하기 위해서다.
package com.cims.ue.sdk

import com.cims.ue.sdk.jni.AudioDeviceVector
import com.cims.ue.sdk.jni.MediaSourceVector
import com.cims.ue.sdk.jni.RosterVector
import com.cims.ue.sdk.jni.TalkerVector
import com.cims.ue.sdk.jni.AccountConfig as JniAccountConfig
import com.cims.ue.sdk.jni.AudioDeviceInfo as JniAudioDeviceInfo
import com.cims.ue.sdk.jni.CallInfo as JniCallInfo
import com.cims.ue.sdk.jni.CallOptions as JniCallOptions
import com.cims.ue.sdk.jni.DialogInfo as JniDialogInfo
import com.cims.ue.sdk.jni.EngineConfig as JniEngineConfig
import com.cims.ue.sdk.jni.FloorEvent as JniFloorEvent
import com.cims.ue.sdk.jni.FloorInfo as JniFloorInfo
import com.cims.ue.sdk.jni.GroupCallOptions as JniGroupCallOptions
import com.cims.ue.sdk.jni.McpttInfo as JniMcpttInfo
import com.cims.ue.sdk.jni.MediaSource as JniMediaSource
import com.cims.ue.sdk.jni.RegInfo as JniRegInfo
import com.cims.ue.sdk.jni.RequestResult as JniRequestResult
import com.cims.ue.sdk.jni.Result as JniResult
import com.cims.ue.sdk.jni.SdsMessage as JniSdsMessage
import com.cims.ue.sdk.jni.SdsSend as JniSdsSend
import com.cims.ue.sdk.jni.StreamStats as JniStreamStats
import com.cims.ue.sdk.jni.StringVector
import com.cims.ue.sdk.jni.TlsPeerExpiry as JniTlsPeerExpiry

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
    val noVad: Boolean = false,
    val udpPort: Int = 0,
    val tcpPort: Int = 0,
    val tlsPort: Int = 0,
    val clockRate: Long = 16000L,
) {
    internal fun toJni(): JniEngineConfig = JniEngineConfig().also {
        it.userAgent = userAgent; it.logLevel = logLevel
        it.tlsCaPem = tlsCaPem; it.tlsVerifyServer = tlsVerifyServer
        it.nullAudioDevice = nullAudioDevice; it.noVad = noVad; it.udpNoTcpSwitch = udpNoTcpSwitch
        it.udpPort = udpPort; it.tcpPort = tcpPort; it.tlsPort = tlsPort
        it.clockRate = clockRate
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
        it.autoAnswerMcptt = autoAnswerMcptt
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
) {
    internal fun toJni(): JniGroupCallOptions = JniGroupCallOptions().also {
        it.emergency = emergency; it.imminentPeril = imminentPeril
        it.listenOnly = listenOnly; it.fullDuplex = fullDuplex
        it.members = StringVector().apply { members.forEach { m -> add(m) } }
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
) {
    internal companion object {
        fun of(m: JniMcpttInfo) = McpttInfo(m.present, m.sessionType, m.requestUri, m.callingUserId,
            m.callingGroupId, m.emergency, m.imminentPeril, m.privateCall, m.noFloorCtrl)
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
) {
    val active: Boolean get() = state == CallState.ACTIVE
    val ended: Boolean get() = state == CallState.DISCONNECTED
    internal companion object {
        fun of(c: JniCallInfo) = CallInfo(
            c.callId, c.accountId, ordinalOf(c.dir.swigValue()), ordinalOf(c.state.swigValue()),
            c.remoteUri, c.calledParty, c.video, c.mediaActive, c.muted, c.listen,
            c.playbackRoute, c.lastCode, c.lastReason, MediaSource.list(c.sources),
            c.isMcptt, c.groupId, McpttInfo.of(c.mcptt), c.halfDuplex, c.listenOnly, c.joinedDialog)
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
    val callId: Int, val state: FloorState, val durationSec: Int,
    val cause: Int, val causeText: String, val indicator: Int,
    /** Floor Taken 의 Permission to Request the Floor — 0 이면 앱이 발언 버튼을 비활성한다. */
    val permission: Int, val queuePosition: Int, val meSpeaking: Boolean,
    val talkers: List<Talker>, val rawType: Int,
) {
    internal companion object {
        fun of(e: JniFloorEvent) = FloorEvent(e.callId, ordinalOf(e.state.swigValue()), e.durationSec,
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
) {
    internal companion object {
        fun of(m: JniSdsMessage) = SdsMessage(m.accountId, m.fromUri, m.groupUri, m.convId, m.msgId,
            m.timeSec, m.dispositionReq, m.text, m.notification, m.notifType,
            m.fd, m.fileUrl, m.fileName, m.fileType, m.fileSize)
    }
}

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
