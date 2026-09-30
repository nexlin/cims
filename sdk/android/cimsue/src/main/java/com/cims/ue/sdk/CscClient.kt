// libcimsue Kotlin 파사드 — CSC 설정 평면 (docs/design/features/android_dispatch_tablet.md §4,
//                                        ue_sdk.md §4.1 csc·§4.4, android_ue_provisioning.md §3)
//
// 코어가 프로토콜(PKCE·Bearer·XCAP 경로·ETag/304·프로비저닝 파싱)을 갖고 동기로 돈다. 파사드가 하는 일은
// **스레드를 옮기는 것뿐**이다 — 코어 호출을 Dispatchers.IO 로 보내 suspend 로 낸다. 경로·JSON 해석을
// 여기에 두지 않는다(코어가 모델링하지 않은 엔드포인트는 request() 로 앱이 직접 다룬다).
//
// 전송은 기본 OpenSSL 하나다 — 주입 통로는 아직 없다(§3.2).
package com.cims.ue.sdk

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import com.cims.ue.sdk.jni.Capabilities as JniCapabilities
import com.cims.ue.sdk.jni.CmsEntry as JniCmsEntry
import com.cims.ue.sdk.jni.CscClient as JniCscClient
import com.cims.ue.sdk.jni.CscEndpoint as JniCscEndpoint
import com.cims.ue.sdk.jni.FdUpload as JniFdUpload
import com.cims.ue.sdk.jni.GroupDoc as JniGroupDoc
import com.cims.ue.sdk.jni.GroupMember as JniGroupMember
import com.cims.ue.sdk.jni.GroupMemberVector
import com.cims.ue.sdk.jni.GroupSummaryVector
import com.cims.ue.sdk.jni.HttpResult as JniHttpResult
import com.cims.ue.sdk.jni.Profile as JniProfile
import com.cims.ue.sdk.jni.ServiceConfigDoc as JniServiceConfigDoc
import com.cims.ue.sdk.jni.StringVector
import com.cims.ue.sdk.jni.TokenSet as JniTokenSet
import com.cims.ue.sdk.jni.UeInitConfigDoc as JniUeInitConfigDoc
import com.cims.ue.sdk.jni.UserProfileDoc as JniUserProfileDoc
import com.cims.ue.sdk.jni.XcapDoc as JniXcapDoc

// ── 값 타입 ──────────────────────────────────────────────────────────────────
/** CSC 접속점. scope 기본값은 코어 헤더의 MC 서비스 8종 카탈로그(TS 33.180 B.4.2.2). */
data class CscEndpoint(
    val host: String,
    val port: Int = 4430,
    val clientId: String = "MCPTT_UE",
    val redirectUri: String = "https://localhost/callback",
    val scope: String? = null,
    /** 신뢰 앵커(PEM). 비면 시스템 기본. */
    val caPem: String = "",
    val verifyServer: Boolean = true,
) {
    internal fun toJni(): JniCscEndpoint = JniCscEndpoint().also {
        it.host = host; it.port = port; it.clientId = clientId; it.redirectUri = redirectUri
        if (scope != null) it.scope = scope
        it.caPem = caPem; it.verifyServer = verifyServer
    }
}

data class TokenSet(val accessToken: String, val tokenType: String, val refreshToken: String,
                    val idToken: String, val scope: String, val expiresInSec: Int) {
    internal companion object {
        fun of(t: JniTokenSet) = TokenSet(t.accessToken, t.tokenType, t.refreshToken, t.idToken, t.scope, t.expiresInSec)
    }
}

data class ServiceEndpoint(val transport: Transport, val port: Int)

/** 프로비저닝 프로파일의 접속서비스 하나 — 그대로 계정으로 바뀐다(`toAccount`). */
data class ServiceProfile(
    /** volte(이동) | voip(유선) | ptt — 접속환경 클래스(sip_service_model.md §2-9). */
    val kind: String,
    val sipHost: String, val sipPort: Int, val transport: Transport,
    val transports: List<ServiceEndpoint>, val enforced: Boolean,
    val mediaSecurity: MediaSecurity,
    val domain: String, val msisdn: String, val imsi: String, val authId: String,
    val sipHa1: String, val mcpttId: String,
    val authScheme: AuthScheme, val akaK: String, val akaOpc: String, val akaAmf: String,
    val secMechanisms: List<String>, val maxPayloadSdsCplaneBytes: Int,
    /** UDP→TCP 승격 비활성(`sip.udpNoTcpSwitch`) — 엔진 전역(`EngineConfig.udpNoTcpSwitch`)이라 앱이 서비스들에서 골라 넣는다. */
    val udpNoTcpSwitch: Boolean = false,
    /** 외부망 SMS/LMS 게이트웨이 연결(`capabilities.smsGateway`) — 관제 앱의 외부 번호 [문자] 활성 조건. */
    val smsGateway: Boolean = false,
) {
    /** 이 서비스로 등록할 계정 설정 — 프로파일 값 그대로(loginPw 는 sipHa1 부재 시 평문 폴백). */
    fun toAccountConfig(loginPw: String = ""): AccountConfig = AccountConfig(
        serverHost = sipHost, serverPort = sipPort, transport = transport, domain = domain,
        msisdn = msisdn, imsi = imsi, authId = authId, ha1 = sipHa1,
        password = if (sipHa1.isEmpty()) loginPw else "",
        authScheme = authScheme, akaK = akaK, akaOpc = akaOpc, akaAmf = akaAmf,
        secMechanisms = secMechanisms, mediaSecurity = mediaSecurity, mcpttId = mcpttId,
        maxSdsCplaneBytes = maxPayloadSdsCplaneBytes)
}

/** 관제 그룹원·감시 대상(dispatch members[]) — groupId 가 내 그룹이면 그룹원 띠, 그 밖은 감시 전용. */
data class DispatchMember(val userId: String, val name: String, val volteAor: String,
                          val pttId: String, val extension: String, val groupId: String)

/** 청취 대상 PTT 그룹(dispatch pttTargets[]). */
data class DispatchTarget(val id: String, val uri: String, val name: String)

/** 관제 데스크(dispatch_center.md §8.4). present=false 면 앱은 소프트폰 모드. */
data class DispatchProfile(
    val present: Boolean, val groupId: String, val groupName: String, val pilotId: String,
    val monitorScope: String, val pttListen: String, val listenVisibility: String,
    val directoryAdmin: String, val orgCode: String,
    val members: List<DispatchMember>, val pttTargets: List<DispatchTarget>,
) {
    /** 조직·구성원·번호 관리 범위가 있는가(none|own|all). */
    val canAdminDirectory: Boolean get() = directoryAdmin == "own" || directoryAdmin == "all"
    companion object {
        val NONE = DispatchProfile(false, "", "", "", "none", "none", "hidden", "none", "", emptyList(), emptyList())
    }
}

data class Profile(
    val displayName: String, val loginId: String, val countryCode: String,
    val cscHost: String, val cscPort: Int,
    val services: List<ServiceProfile>, val dispatch: DispatchProfile,
    /** GMS 그룹 생성 자격(ptt_user_profile.allow_create_group). */
    val allowGroupCreation: Boolean,
) {
    fun service(kind: String): ServiceProfile? = services.firstOrNull { it.kind == kind }
    /** 전화 회선 — 유선 `voip` 우선, 없으면 이동 `volte`. 관제 앱의 전화 계정 선택 규칙. */
    val phoneService: ServiceProfile? get() = service("voip") ?: service("volte")
    val pttService: ServiceProfile? get() = service("ptt")
}

/** GMS 목록 항목. isOwner = 토큰 주체가 authorized user(편집·삭제 가능). */
data class GroupSummary(val uri: String, val displayName: String, val etag: String,
                        val memberCount: Int, val isOwner: Boolean)

data class GroupMember(val uri: String, val name: String = "",
                       val role: String = "participant", val priority: Int = 5,
                       /** 직함 `<cims:user-title>`(사이트 확장) — 읽기 전용, PUT 에 싣지 않는다. */
                       val title: String = "",
                       /** 필수 멤버 `<on-network-required>`(TS 24.481 §7.2.4.2) — 읽은 값을 되돌려야 콘솔 설정이 남는다. */
                       val required: Boolean = false)

/**
 * GMS 그룹 문서(OMA list-service + TS 24.481 mcpttgi) — GET 응답·PUT 본문의 단일 모델.
 *
 * 끝의 다섯(그룹 호 타이머·참가자 정보·MCData 크기 한도)은 **`null` = 미기재**다 — PUT 에 싣지 않아 서버가 기존값을
 * 유지한다. 폼에서 이 칸을 다루지 않는 앱이 새 문서를 지어 저장해도 콘솔이 정한 값을 덮지 않게 하려는 것이다.
 * `0` 은 값이다(hang 0 = 미사용, 크기 0 = 무제한).
 */
data class GroupDoc(
    val uri: String, val displayName: String = "", val etag: String = "",
    val members: List<GroupMember> = emptyList(),
    val sessionType: String = "prearranged",
    val videoEnabled: Boolean = false, val encryption: Boolean = false,
    val emergencyCall: Boolean = true, val emergencyAlert: Boolean = true,
    val allowSds: Boolean = true, val allowFd: Boolean = false,
    val requireAffiliation: Boolean = true,
    val priority: Int = 5, val maxParticipants: Int = 0,
    val orgCode: String = "", val authorizedUser: String = "",
    /** on-network-hang-timer (T4, 초) — 0 = 미사용. */
    val hangTimerSec: Int? = null,
    /** on-network-maximum-duration (TNG3, 초) — 0 = 무제한. */
    val maxDurationSec: Int? = null,
    /** on-network-allow-conference-state — 멤버의 참가자 정보(conference 이벤트) 구독 허용. */
    val allowConferenceState: Boolean? = null,
    /** mcdata-on-network-max-data-size-for-SDS (octet) — 0 = 무제한. */
    val maxSdsSize: Int? = null,
    /** mcdata-on-network-max-data-size-auto-recv (octet) — 0 = 무제한. */
    val maxAutoRecv: Int? = null,
    /** on-network-minimum-number-to-start — 개시자 200 OK 전 멤버 200 수(0 = 기다리지 않음). */
    val minNumberToStart: Int? = null,
    /** on-network-timeout-for-acknowledgement-of-required-members (TNG1, 초). */
    val ackTimeoutSec: Int? = null,
    /** TNG1 만료 동작 proceed | abandon. */
    val ackAction: String? = null,
) {
    internal fun toJni(): JniGroupDoc = JniGroupDoc().also { d ->
        d.uri = uri; d.displayName = displayName; d.etag = etag
        d.members = GroupMemberVector().apply {
            members.forEach { m -> add(JniGroupMember().also { it.uri = m.uri; it.name = m.name; it.role = m.role; it.priority = m.priority; it.required = m.required }) }
        }
        d.sessionType = sessionType; d.videoEnabled = videoEnabled; d.encryption = encryption
        d.emergencyCall = emergencyCall; d.emergencyAlert = emergencyAlert
        d.allowSds = allowSds; d.allowFd = allowFd; d.requireAffiliation = requireAffiliation
        d.priority = priority; d.maxParticipants = maxParticipants
        d.orgCode = orgCode; d.authorizedUser = authorizedUser
        // null → 코어의 kUnset(-1). 음수는 받지 않는다(서버도 범위 밖은 400) — 미기재로 떨어뜨린다.
        d.hangTimerSec = hangTimerSec.orUnset(); d.maxDurationSec = maxDurationSec.orUnset()
        d.allowConferenceState = when (allowConferenceState) { null -> UNSET; true -> 1; false -> 0 }
        d.maxSdsSize = maxSdsSize.orUnset(); d.maxAutoRecv = maxAutoRecv.orUnset()
        d.minNumberToStart = minNumberToStart.orUnset(); d.ackTimeoutSec = ackTimeoutSec.orUnset()
        d.ackAction = ackAction ?: ""
    }
    internal companion object {
        /** 코어 `GroupDoc::kUnset`. */
        private const val UNSET = -1
        private fun Int?.orUnset(): Int = if (this == null || this < 0) UNSET else this
        private fun Int.orNull(): Int? = if (this < 0) null else this

        fun of(d: JniGroupDoc) = GroupDoc(d.uri, d.displayName, d.etag,
            d.members.let { v -> List(v.size) { i -> v[i].let { GroupMember(it.uri, it.name, it.role, it.priority, it.title, it.required) } } },
            d.sessionType, d.videoEnabled, d.encryption, d.emergencyCall, d.emergencyAlert,
            d.allowSds, d.allowFd, d.requireAffiliation, d.priority, d.maxParticipants,
            d.orgCode, d.authorizedUser,
            hangTimerSec = d.hangTimerSec.orNull(), maxDurationSec = d.maxDurationSec.orNull(),
            allowConferenceState = d.allowConferenceState.let { if (it < 0) null else it != 0 },
            maxSdsSize = d.maxSdsSize.orNull(), maxAutoRecv = d.maxAutoRecv.orNull(),
            minNumberToStart = d.minNumberToStart.orNull(), ackTimeoutSec = d.ackTimeoutSec.orNull(),
            ackAction = d.ackAction.ifEmpty { null })
    }
}

data class XcapDoc(val body: String, val etag: String, val notModified: Boolean)

/** CMS 문서의 대상 항목 — EntryType(TS 24.484 §8.3.2.7): uri = `<uri-entry>`, mode = `entry-info`
 *  (그룹 = DedicatedGroup | UseCurrentlySelectedGroup, 사설 수신자 = UsePreConfigured | LocallyDetermined). 대상 선택 정책은 앱 몫. */
data class CmsEntry(val uri: String = "", val mode: String = "") {
    internal fun toJni(): JniCmsEntry = JniCmsEntry().also { it.uri = uri; it.mode = mode }
    internal companion object { fun of(e: JniCmsEntry) = CmsEntry(e.uri, e.mode) }
}

/**
 * MCPTT user profile(TS 24.484 §8.3.2) — 코어가 해석한 요소. 인가(allow-*)는 요소가 없으면 허용으로 읽는다
 * (서버가 최종 판정 — UX 선차단용, ue_sdk.md §4.2). 판정 스냅샷은 [Capabilities.of].
 */
data class UserProfileDoc(
    val etag: String = "", val userUri: String = "",
    val emergencyGroup: CmsEntry = CmsEntry(), val imminentPerilGroup: CmsEntry = CmsEntry(),
    val emergencyAlertGroup: CmsEntry = CmsEntry(), val emergencyPrivateRecipient: CmsEntry = CmsEntry(),
    /** OnNetwork/MCPTTGroupInfo — 제휴 가능 그룹 URI. */
    val groups: List<String> = emptyList(),
    val implicitAffiliations: List<String> = emptyList(),
    /** OnNetwork/MaxAffiliationsN2 — null = 미기재. */
    val maxAffiliationsN2: Int? = null,
    val allowEmergencyGroupCall: Boolean = true, val allowImminentPerilCall: Boolean = true,
    val allowActivateEmergencyAlert: Boolean = true, val allowCancelEmergencyAlert: Boolean = true,
    val allowEmergencyPrivateCall: Boolean = true, val allowAdhocGroupCall: Boolean = true,
    /** allow-private-call (§8.3.2.7). */
    val allowPrivateCall: Boolean = true,
    /** allow-cancel-group-emergency (§8.3.2.1 11)xiv)) — 서버 판정 = 개시자 ∨ 이 값(TS 24.379 §6.3.3.1.13.4). */
    val allowCancelGroupEmergency: Boolean = true,
    /** allow-cancel-imminent-peril (11)xvii)) — 개시자 예외 없음(§6.2.8.1.10). */
    val allowCancelImminentPeril: Boolean = true,
) {
    internal fun toJni(): JniUserProfileDoc = JniUserProfileDoc().also { d ->
        d.etag = etag; d.userUri = userUri
        d.emergencyGroup = emergencyGroup.toJni(); d.imminentPerilGroup = imminentPerilGroup.toJni()
        d.emergencyAlertGroup = emergencyAlertGroup.toJni(); d.emergencyPrivateRecipient = emergencyPrivateRecipient.toJni()
        d.groups = StringVector().apply { groups.forEach { add(it) } }
        d.implicitAffiliations = StringVector().apply { implicitAffiliations.forEach { add(it) } }
        d.maxAffiliationsN2 = maxAffiliationsN2 ?: -1
        d.allowEmergencyGroupCall = allowEmergencyGroupCall; d.allowImminentPerilCall = allowImminentPerilCall
        d.allowActivateEmergencyAlert = allowActivateEmergencyAlert; d.allowCancelEmergencyAlert = allowCancelEmergencyAlert
        d.allowEmergencyPrivateCall = allowEmergencyPrivateCall; d.allowAdhocGroupCall = allowAdhocGroupCall
        d.allowPrivateCall = allowPrivateCall
        d.allowCancelGroupEmergency = allowCancelGroupEmergency; d.allowCancelImminentPeril = allowCancelImminentPeril
    }
    internal companion object {
        fun of(d: JniUserProfileDoc) = UserProfileDoc(d.etag, d.userUri,
            CmsEntry.of(d.emergencyGroup), CmsEntry.of(d.imminentPerilGroup),
            CmsEntry.of(d.emergencyAlertGroup), CmsEntry.of(d.emergencyPrivateRecipient),
            d.groups.let { v -> List(v.size) { v[it] } }, d.implicitAffiliations.let { v -> List(v.size) { v[it] } },
            d.maxAffiliationsN2.takeIf { it >= 0 },
            d.allowEmergencyGroupCall, d.allowImminentPerilCall, d.allowActivateEmergencyAlert,
            d.allowCancelEmergencyAlert, d.allowEmergencyPrivateCall, d.allowAdhocGroupCall, d.allowPrivateCall,
            d.allowCancelGroupEmergency, d.allowCancelImminentPeril)
    }
}

/**
 * MCS UE initial configuration(TS 24.484 §7.2) — 로그인 전 문서. 코어가 쓰는 것은 참여 기능 PSI(`<anyExt>` 의 *-Service-Details/Server-URI).
 * 광고하지 않은 서비스는 빈 값 — [AccountConfig.mcpttServerUri]·[AccountConfig.mcdataServerUri] 에 그대로 넣는다.
 */
data class UeInitConfigDoc(
    val etag: String = "", val domain: String = "",
    val mcpttServerUri: String = "", val mcdataServerUri: String = "",
) {
    internal companion object {
        fun of(d: JniUeInitConfigDoc) = UeInitConfigDoc(d.etag, d.domain, d.mcpttServerUri, d.mcdataServerUri)
    }
}

/**
 * MCPTT service configuration(TS 24.484 §8.4) — 시스템 전역 문서. 인가 요소는 없다(인가 = user profile·그룹 문서).
 * Resource-Priority r-value(`mcpttp.15` 형식, TS 24.379 §6.2.8.1.15)는 비면 미기재 — 받으면 AccountConfig.rp* 에 넣는다.
 */
data class ServiceConfigDoc(
    val etag: String = "",
    /** service-configuration-params@domain */
    val domain: String = "",
    /** common/broadcast-group 계층 수 — null = 미기재. */
    val numLevelsGroupHierarchy: Int? = null, val numLevelsUserHierarchy: Int? = null,
    val rpEmergency: String = "", val rpImminentPeril: String = "", val rpNormal: String = "",
) {
    internal fun toJni(): JniServiceConfigDoc = JniServiceConfigDoc().also { d ->
        d.etag = etag; d.domain = domain
        d.numLevelsGroupHierarchy = numLevelsGroupHierarchy ?: -1; d.numLevelsUserHierarchy = numLevelsUserHierarchy ?: -1
        d.rpEmergency = rpEmergency; d.rpImminentPeril = rpImminentPeril; d.rpNormal = rpNormal
    }
    internal companion object {
        fun of(d: JniServiceConfigDoc) = ServiceConfigDoc(d.etag, d.domain,
            d.numLevelsGroupHierarchy.takeIf { it >= 0 }, d.numLevelsUserHierarchy.takeIf { it >= 0 },
            d.rpEmergency, d.rpImminentPeril, d.rpNormal)
    }
}

/**
 * 정책 게이트 스냅샷(ue_sdk.md §4.2) — user profile ruleset 인가, **받지 못한 문서는 허용**. UX 선차단(버튼 숨김)용이고
 * 최종 판정은 서버(403·Floor Deny)다. 규칙은 코어 한 곳(`Capabilities::of`)이다.
 */
data class Capabilities(
    val userProfileKnown: Boolean = false, val serviceConfigKnown: Boolean = false,
    val privateCall: Boolean = true, val emergencyGroupCall: Boolean = true, val imminentPerilCall: Boolean = true,
    val emergencyPrivateCall: Boolean = true, val emergencyAlert: Boolean = true, val cancelEmergencyAlert: Boolean = true,
    val adhocGroupCall: Boolean = true,
    /** 0 = 미지정. N2 는 경고만 한다(강제하지 않는다). */
    val maxAffiliationsN2: Int = 0,
    /** up.allow-cancel-group-emergency — 앱은 «내가 올린 조건(McpttCondition.mine)» 과 OR 해서 [긴급 해제] 를 연다. */
    val cancelGroupEmergency: Boolean = true,
    /** up.allow-cancel-imminent-peril. */
    val cancelImminentPeril: Boolean = true,
) {
    companion object {
        /** null = 그 문서를 아직 못 받음. */
        fun of(userProfile: UserProfileDoc?, serviceConfig: ServiceConfigDoc?): Capabilities {
            NativeLib.ensure()
            val c = JniCapabilities.of(userProfile?.toJni(), serviceConfig?.toJni())
            return Capabilities(c.userProfileKnown, c.serviceConfigKnown, c.privateCall, c.emergencyGroupCall,
                c.imminentPerilCall, c.emergencyPrivateCall, c.emergencyAlert, c.cancelEmergencyAlert,
                c.adhocGroupCall, c.maxAffiliationsN2, c.cancelGroupEmergency, c.cancelImminentPeril)
        }
    }
}

/** 임의 HTTP 요청 산출. body 는 **바이트 그대로** — 녹취 오디오(MP4/AAC)가 이 경로로 온다. */
data class HttpResponse(val status: Int, val contentType: String, val etag: String, val body: ByteArray) {
    val notModified: Boolean get() = status == 304
    /** 텍스트(JSON 등) 응답의 편의 — 이진 응답에는 쓰지 않는다. */
    val text: String get() = body.toString(Charsets.UTF_8)

    override fun equals(other: Any?): Boolean = this === other ||
        (other is HttpResponse && status == other.status && contentType == other.contentType &&
         etag == other.etag && body.contentEquals(other.body))
    override fun hashCode(): Int =
        ((status * 31 + contentType.hashCode()) * 31 + etag.hashCode()) * 31 + body.contentHashCode()
}

// ── 클라이언트 ────────────────────────────────────────────────────────────────
/**
 * CSC(IdMS·GMS·CMS·프로비저닝) 클라이언트. 코어 호출이 동기라 모든 메서드가 `suspend` 이며
 * [io] 디스패처에서 돈다. 엔진과 독립이라 로그인만 먼저 해도 된다.
 */
class CscClient(
    endpoint: CscEndpoint,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) : AutoCloseable {

    private val jni: JniCscClient

    init {
        NativeLib.ensure()
        jni = JniCscClient(endpoint.toJni())
    }

    // ── 수명 게이트 ───────────────────────────────────────────────────────────
    // 생성 코드의 delete() 만 synchronized 이고 요청 호출은 swigCPtr 를 보호 없이 읽는다
    // (jni/CscClient.java). 로그아웃·서비스 종료가 진행 중인 HTTP 요청과 겹치면 네이티브가 해제된 채
    // 쓰이므로(csc_client.cpp 의 Impl) use-after-free 다. 코루틴 취소만으로는 이미 들어간 동기 JNI
    // 호출을 되돌릴 수 없다 — 그래서 신규 호출을 막고 진행 중인 것이 빠지기를 기다린 뒤 해제한다.
    private val closed = AtomicBoolean(false)
    private val inFlight = AtomicInteger(0)
    private val gate = Object()

    val isClosed: Boolean get() = closed.get()

    private inline fun <T> guarded(block: () -> T): T? {
        if (closed.get()) return null
        inFlight.incrementAndGet()
        try {
            if (closed.get()) return null
            return block()
        } finally {
            if (inFlight.decrementAndGet() == 0) synchronized(gate) { (gate as Object).notifyAll() }
        }
    }

    private suspend fun <T> call(block: () -> CimsResult<T>): CimsResult<T> =
        withContext(io) { guarded(block) ?: CimsResult.fail(-99, "csc client closed") }

    /** 멱등. 진행 중인 요청이 끝나기를 기다린 뒤 네이티브를 해제한다. */
    override fun close() {
        if (!closed.compareAndSet(false, true)) return
        synchronized(gate) {
            while (inFlight.get() > 0) {
                runCatching { (gate as Object).wait(CLOSE_WAIT_MS) }.getOrElse { return@synchronized }
            }
        }
        runCatching { jni.delete() }
    }

    /** HTTPS 서버 인증서 만료 관측 — 아직 요청이 없으면 valid=false. */
    fun tlsPeerExpiry(): TlsPeerExpiry? = guarded { TlsPeerExpiry.of(jni.tlsPeerExpiry()) }

    /** IdMS PKCE(S256) 로그인. */
    suspend fun login(userName: String, password: String): CimsResult<TokenSet> = call {
        val out = JniTokenSet()
        CimsResult.of(jni.login(userName, password, out), TokenSet.of(out))
    }

    suspend fun refresh(refreshToken: String): CimsResult<TokenSet> = call {
        val out = JniTokenSet()
        CimsResult.of(jni.refresh(refreshToken, out), TokenSet.of(out))
    }

    /** GET /provisioning/me — 접속서비스·관제 데스크 블록. */
    suspend fun fetchProfile(accessToken: String): CimsResult<Profile> = call {
        val out = JniProfile()
        CimsResult.of(jni.fetchProfile(accessToken, out), profileOf(out))
    }

    /** GMS 그룹 목록. userUri 예 `tel:+8250...`. */
    suspend fun listGroups(accessToken: String, userUri: String): CimsResult<List<GroupSummary>> = call {
        val out = GroupSummaryVector()
        CimsResult.of(jni.listGroups(accessToken, userUri, out),
            List(out.size) { out[it].let { g -> GroupSummary(g.uri, g.displayName, g.etag, g.memberCount, g.isOwner) } })
    }

    // ── GMS 그룹 관리(TS 24.481 — 생성·수정·삭제 주체 = authorized user) ──
    suspend fun getGroup(accessToken: String, userUri: String, groupUri: String): CimsResult<GroupDoc> = call {
        val out = JniGroupDoc()
        CimsResult.of(jni.getGroup(accessToken, userUri, groupUri, out), GroupDoc.of(out))
    }

    /** 생성(신규 uri)/수정(기존 uri). ifMatch 가 비지 않으면 조건부(412 = 충돌).
     *  실패 code = HTTP(403 자격·소유, 409 타인 소유, 412) — 앱이 본문 `error` 로 문구를 분기한다. */
    suspend fun putGroup(accessToken: String, userUri: String, doc: GroupDoc,
                         ifMatch: String = ""): CimsResult<GroupDoc> = call {
        val out = JniGroupDoc()
        CimsResult.of(jni.putGroup(accessToken, userUri, doc.toJni(), ifMatch, out), GroupDoc.of(out))
    }

    /** 삭제 — 본인 소유만(403). */
    suspend fun deleteGroup(accessToken: String, userUri: String, groupUri: String): CimsResult<Unit> = call {
        CimsResult.of(jni.deleteGroup(accessToken, userUri, groupUri))
    }

    // ── XCAP ──
    suspend fun xcapGet(accessToken: String, path: String, accept: String,
                        ifNoneMatch: String = ""): CimsResult<XcapDoc> = call {
        val out = JniXcapDoc()
        CimsResult.of(jni.xcapGet(accessToken, path, accept, ifNoneMatch, out), xcapOf(out))
    }

    /** CMS user-profile(TS 24.484) — 정책 게이트의 입력. */
    suspend fun getUserProfile(accessToken: String, userUri: String, etag: String = ""): CimsResult<XcapDoc> = call {
        val out = JniXcapDoc()
        CimsResult.of(jni.getUserProfile(accessToken, userUri, etag, out), xcapOf(out))
    }

    /** CMS service-config — user-profile 과 AND 게이트. */
    suspend fun getServiceConfig(accessToken: String, userUri: String, etag: String = ""): CimsResult<XcapDoc> = call {
        val out = JniXcapDoc()
        CimsResult.of(jni.getServiceConfig(accessToken, userUri, etag, out), xcapOf(out))
    }

    /** CMS user-profile 조회 + 해석(코어). etag 를 주면 If-None-Match — **값 null = 304**(가진 사본 유지). 해석 실패 code -2. */
    suspend fun fetchUserProfile(accessToken: String, userUri: String, etag: String = ""): CimsResult<UserProfileDoc?> = call {
        val out = JniUserProfileDoc()
        CimsResult.of(jni.fetchUserProfile(accessToken, userUri, etag, out), if (out.notModified) null else UserProfileDoc.of(out))
    }

    /**
     * UE initial configuration 조회 + 해석(TS 24.484 §7.2.1.1) — mcsUeId = 단말 instance ID(`AccountConfig.instanceId`, urn:uuid:…).
     * 로그인 전 문서라 토큰 없이 부른다. **값 null = 304**. 해석 실패 code -2.
     */
    suspend fun fetchUeInitConfig(mcsUeId: String, etag: String = ""): CimsResult<UeInitConfigDoc?> = call {
        val out = JniUeInitConfigDoc()
        CimsResult.of(jni.fetchUeInitConfig(mcsUeId, etag, out), if (out.notModified) null else UeInitConfigDoc.of(out))
    }

    /** CMS service-config 조회 + 해석 — [fetchUserProfile] 과 같은 규약. */
    suspend fun fetchServiceConfig(accessToken: String, userUri: String, etag: String = ""): CimsResult<ServiceConfigDoc?> = call {
        val out = JniServiceConfigDoc()
        CimsResult.of(jni.fetchServiceConfig(accessToken, userUri, etag, out), if (out.notModified) null else ServiceConfigDoc.of(out))
    }

    // ── MCData FD 콘텐츠 서버(TS 24.282 §10.2, mcdata_messaging.md §4.5) ──
    /**
     * 파일 업로드(POST /mcdata/fd) — 결과 [FdUpload.url] 을 [FdFile.url] 로 넘겨 `Account.sendGroupFd`/`sendFd` 로 알린다.
     * groupId 를 주면 서버가 그 그룹의 FD 게이트를 적용하고, 비우면 1:1. 413 = 서버 상한 초과. 실패 code = HTTP 상태(전송 실패 -1).
     */
    suspend fun uploadFd(accessToken: String, data: ByteArray, name: String, mime: String = "",
                         groupId: String = ""): CimsResult<FdUpload> = call {
        val out = JniFdUpload()
        CimsResult.of(jni.uploadFd(accessToken, data, name, mime, groupId, out), FdUpload(out.id, out.url, out.name, out.size))
    }

    /** 받은 FD 의 FILEURL 다운로드 — 경로만 취해 자기 CSC 로 보낸다(Bearer 를 다른 호스트로 보내지 않는다). 본문 = 파일 바이트. */
    suspend fun downloadFd(accessToken: String, url: String): CimsResult<HttpResponse> = call {
        val out = JniHttpResult()
        val r = jni.downloadFd(accessToken, url, out)
        val v = HttpResponse(out.status, out.contentType, out.etag, out.body ?: ByteArray(0))
        if (r.ok) CimsResult.ok(v) else CimsResult(false, r.code, r.reason, v)
    }

    /**
     * 코어가 모델링하지 않은 CSC 엔드포인트용 범용 요청(Bearer) — 관제 관리 API
     * `/provisioning/directory/…`·녹취 `/provisioning/recordings/…`(이진)·이력 창 조회.
     * 경로·JSON 은 앱(ManagementClient)이 갖고 인증·전송만 코어가 한다.
     * 2xx·304 는 성공이고 그 밖의 HTTP 상태는 실패지만 **본문은 채워진다**(앱이 오류 JSON 을 읽는다).
     */
    suspend fun request(accessToken: String, method: String, path: String,
                        contentType: String = "", body: ByteArray? = null, accept: String = "",
                        ifMatch: String = "", ifNoneMatch: String = ""): CimsResult<HttpResponse> = call {
        val out = JniHttpResult()
        val r = jni.request(accessToken, method, path, contentType, body ?: ByteArray(0), accept, ifMatch, ifNoneMatch, out)
        val v = HttpResponse(out.status, out.contentType, out.etag, out.body ?: ByteArray(0))
        if (r.ok) CimsResult.ok(v) else CimsResult(false, r.code, r.reason, v)
    }

    /** JSON 편의 — Accept/Content-Type = application/json. */
    suspend fun requestJson(accessToken: String, method: String, path: String, json: String? = null,
                            ifMatch: String = "", ifNoneMatch: String = ""): CimsResult<HttpResponse> =
        request(accessToken, method, path, if (json != null) "application/json" else "",
            json?.toByteArray(Charsets.UTF_8), "application/json", ifMatch, ifNoneMatch)

    companion object {
        private const val CLOSE_WAIT_MS = 5_000L

        /** XCAP 그룹 문서 경로. */
        fun groupPath(userUri: String, groupUri: String): String = JniCscClient.groupPath(userUri, groupUri)
        fun urlEncode(s: String): String = JniCscClient.enc(s)

        /** /provisioning/me 응답 JSON → Profile (시험용). */
        fun parseProfile(json: String): Profile? {
            NativeLib.ensure()
            val out = JniProfile()
            return if (JniCscClient.parseProfile(json, out)) profileOf(out) else null
        }

        private fun xcapOf(d: JniXcapDoc) = XcapDoc(d.body, d.etag, d.notModified)

        private fun profileOf(p: JniProfile): Profile {
            val sv = p.services
            val services = List(sv.size) { i ->
                val s = sv[i]
                ServiceProfile(
                    kind = s.kind, sipHost = s.sipHost, sipPort = s.sipPort,
                    transport = Transport.entries[s.transport.swigValue()],
                    transports = s.transports.let { tv ->
                        List(tv.size) { j -> tv[j].let { ServiceEndpoint(Transport.entries[it.transport.swigValue()], it.port) } }
                    },
                    enforced = s.enforced,
                    mediaSecurity = MediaSecurity.entries[s.mediaSecurity.swigValue()],
                    domain = s.domain, msisdn = s.msisdn, imsi = s.imsi, authId = s.authId,
                    sipHa1 = s.sipHa1, mcpttId = s.mcpttId,
                    authScheme = AuthScheme.entries[s.authScheme.swigValue()],
                    akaK = s.akaK, akaOpc = s.akaOpc, akaAmf = s.akaAmf,
                    secMechanisms = s.secMechanisms.let { mv -> List(mv.size) { j -> mv[j] } },
                    maxPayloadSdsCplaneBytes = s.maxPayloadSdsCplaneBytes,
                    udpNoTcpSwitch = s.udpNoTcpSwitch, smsGateway = s.smsGateway)
            }
            val d = p.dispatch
            val dispatch = DispatchProfile(
                present = d.present, groupId = d.groupId, groupName = d.groupName, pilotId = d.pilotId,
                monitorScope = d.monitorScope, pttListen = d.pttListen, listenVisibility = d.listenVisibility,
                directoryAdmin = d.directoryAdmin, orgCode = d.orgCode,
                members = d.members.let { v -> List(v.size) { i -> v[i].let { DispatchMember(it.userId, it.name, it.volteAor, it.pttId, it.extension, it.groupId) } } },
                pttTargets = d.pttTargets.let { v -> List(v.size) { i -> v[i].let { DispatchTarget(it.id, it.uri, it.name) } } })
            return Profile(p.displayName, p.loginId, p.countryCode, p.cscHost, p.cscPort,
                services, dispatch, p.allowGroupCreation)
        }
    }
}
