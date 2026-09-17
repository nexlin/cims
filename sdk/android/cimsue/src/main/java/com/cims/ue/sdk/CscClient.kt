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
import com.cims.ue.sdk.jni.CscClient as JniCscClient
import com.cims.ue.sdk.jni.CscEndpoint as JniCscEndpoint
import com.cims.ue.sdk.jni.GroupDoc as JniGroupDoc
import com.cims.ue.sdk.jni.GroupMember as JniGroupMember
import com.cims.ue.sdk.jni.GroupMemberVector
import com.cims.ue.sdk.jni.GroupSummaryVector
import com.cims.ue.sdk.jni.HttpResult as JniHttpResult
import com.cims.ue.sdk.jni.Profile as JniProfile
import com.cims.ue.sdk.jni.TokenSet as JniTokenSet
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
) {
    /** 이 서비스로 등록할 계정 설정 — 프로파일 값 그대로(loginPw 는 sipHa1 부재 시 평문 폴백). */
    fun toAccountConfig(loginPw: String = ""): AccountConfig = AccountConfig(
        serverHost = sipHost, serverPort = sipPort, transport = transport, domain = domain,
        msisdn = msisdn, imsi = imsi, authId = authId, ha1 = sipHa1,
        password = if (sipHa1.isEmpty()) loginPw else "",
        authScheme = authScheme, akaK = akaK, akaOpc = akaOpc, akaAmf = akaAmf,
        secMechanisms = secMechanisms, mediaSecurity = mediaSecurity, mcpttId = mcpttId)
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
                       val role: String = "participant", val priority: Int = 5)

/** GMS 그룹 문서(OMA list-service + TS 24.481 mcpttgi) — GET 응답·PUT 본문의 단일 모델. */
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
) {
    internal fun toJni(): JniGroupDoc = JniGroupDoc().also { d ->
        d.uri = uri; d.displayName = displayName; d.etag = etag
        d.members = GroupMemberVector().apply {
            members.forEach { m -> add(JniGroupMember().also { it.uri = m.uri; it.name = m.name; it.role = m.role; it.priority = m.priority }) }
        }
        d.sessionType = sessionType; d.videoEnabled = videoEnabled; d.encryption = encryption
        d.emergencyCall = emergencyCall; d.emergencyAlert = emergencyAlert
        d.allowSds = allowSds; d.allowFd = allowFd; d.requireAffiliation = requireAffiliation
        d.priority = priority; d.maxParticipants = maxParticipants
        d.orgCode = orgCode; d.authorizedUser = authorizedUser
    }
    internal companion object {
        fun of(d: JniGroupDoc) = GroupDoc(d.uri, d.displayName, d.etag,
            d.members.let { v -> List(v.size) { i -> v[i].let { GroupMember(it.uri, it.name, it.role, it.priority) } } },
            d.sessionType, d.videoEnabled, d.encryption, d.emergencyCall, d.emergencyAlert,
            d.allowSds, d.allowFd, d.requireAffiliation, d.priority, d.maxParticipants,
            d.orgCode, d.authorizedUser)
    }
}

data class XcapDoc(val body: String, val etag: String, val notModified: Boolean)

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
                    maxPayloadSdsCplaneBytes = s.maxPayloadSdsCplaneBytes)
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
