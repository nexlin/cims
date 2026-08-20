package com.cims.ue.core.config

/**
 * SIP 단말 접속/계정 설정. **앱 첫 실행 시 사용자가 입력**하며 하드코딩하지 않는다.
 * volte-client / ptt-client 공용.
 *
 * SIP 식별자 매핑 (공개 ID 와 인증 ID 는 서로 다른 값임에 주의):
 *  - 공개 ID(AOR/IMPU) = `sip:<msisdn>@<domain>`     (From/To/Contact user part)
 *  - **Digest username(IMPI)** = `<imsi>@<domain>`    (Authorization username)
 *  - Registrar           = serverHost:serverPort (transport)
 *  - realm               = 서버 challenge realm 추종(클라이언트가 박지 않음)
 *
 * ### 서버 인증 계약 (csp/CscfModule.cpp 직독 검증, high conf.)
 * CSCF 는 Authorization username 을 `IMSI + "@" + service.domain`(full IMPI)으로 재구성해
 * **정확 일치**를 요구하고, 불일치 시 **즉시 403 Forbidden(401 재챌린지 아님)** 을 단발 응답한다
 * (`CscfModule.cpp:133,140,243`). msisdn(공개 ID)은 가입자 조회 키로만 쓰이고 username 으로는
 * 수용되지 않는다 — 따라서 **msisdn 폴백은 즉시 403** 이므로 금지한다(구 `effectiveAuthId` 제거).
 * IMSI 가 비면 폴백 없이 거부되므로 IMSI(또는 전체 IMPI=authId) 입력은 필수다.
 */
data class SipAccountConfig(
    val serverHost: String = "",        // CSP IP/FQDN  (예: 121.161.164.47)
    /** 접속 포트 — **[transport] 의 포트**다(예: 15060). transport 마다 포트가 다르므로 둘은 항상
     *  같이 움직여야 한다. 직접 대입하지 말고 [withTransport] 로 바꾼다. */
    val serverPort: Int = 5060,
    val transport: Transport = Transport.UDP,
    /** 서버가 알린 가용 transport 목록(프로비저닝 `sip.transports`). 단말은 이 중에서 고른다 —
     *  서버는 세 transport 를 동시에 청취하며 강제하지 않는다(sip_tls_signaling.md §7.1).
     *  빈 목록 = 목록을 모르는 구 서버 응답/미프로비저닝 상태(선택 UI 를 띄우지 않는다). */
    val transports: List<TransportEndpoint> = emptyList(),
    val domain: String = "",            // 홈/서비스 도메인 (예: ims.mnc033.mcc450.3gppnetwork.org)
    val msisdn: String = "",            // 전화번호(공개 ID / AOR user part)
    val imsi: String = "",              // IMSI — Digest username(IMPI) 합성용 (IMSI@domain)
    val displayName: String = "",       // 이름
    val loginId: String = "",           // 로그인 ID (CSC 등)
    val authId: String = "",            // 전체 IMPI 직접 지정(고급). 비우면 imsi@domain 합성
    /** SIP Digest H(A1)=MD5(IMPI:realm:pw) hex32 — 프로비저닝 수신(sipHa1). 있으면 [password] 보다
     *  우선해 PJSIP_CRED_DATA_DIGEST 로 response 를 계산한다(평문 비번 불요, sip_access_security.md §4.7). */
    val sipHa1: String = "",
    val password: String = "",
    val expiresSec: Int = 3600,         // 희망 등록 주기(서버는 200 OK 에서 3600 하드코딩으로 덮어씀)
    val countryCode: String = "",       // 홈 국가코드(digits, 예 "82") — 프로비저닝 수신, 번호 로컬 표기용(표시 전용)
    /** MCData C-plane SDS payload 상한(byte, 프로비저닝 수신) — 초과 시 MSRP 미디어평면 발신.
     *  0=무제한(항상 C-plane). 서버 CSP `Setup.McData.MaxPayloadSizeSdsCplaneBytes` 와 운영자 동기화. */
    val maxPayloadSdsCplaneBytes: Int = 0,
) {
    /**
     * Digest 인증 username (= full IMPI). msisdn 폴백 없음.
     *  - [authId] 가 있으면 전체 IMPI 로 그대로 사용(IMPI 도메인부가 [domain] 과 다른 예외 대비)
     *  - 없으면 `imsi@domain` 합성
     *  - 둘 다 없으면 빈 문자열 → [isComplete] 가 미완성으로 판정(등록 차단, 서버 403 사전 차단)
     */
    val digestUsername: String
        get() = authId.ifBlank { if (imsi.isNotBlank()) "$imsi@$domain" else "" }

    /** sip:<msisdn>@<domain> (공개 ID) */
    val aor: String get() = "sip:$msisdn@$domain"

    /** 등록/통화에 필요한 필수값이 모두 채워졌는가 */
    fun isComplete(): Boolean =
        serverHost.isNotBlank() &&
        serverPort in 1..65535 &&
        domain.isNotBlank() &&
        msisdn.isNotBlank() &&
        (imsi.isNotBlank() || authId.isNotBlank()) &&   // Digest username 확보 (msisdn 폴백 금지 — 없으면 즉시 403)
        (sipHa1.isNotBlank() || password.isNotBlank())  // Digest 자료 — H(A1) 또는 평문 비번

    /**
     * 가용 목록에서 [t] 를 선택 — transport 와 **포트를 함께** 갱신한다. transport 만 바꾸면
     * 옛 포트에 새 프로토콜로 붙어 등록이 실패한다(같은 포트로 평문/TLS 를 겸하지 않는다).
     * 목록에 없는 transport(구 서버 응답 등)면 포트를 유지한 채 transport 만 바꾼다.
     */
    fun withTransport(t: Transport): SipAccountConfig =
        copy(transport = t, serverPort = transports.firstOrNull { it.transport == t }?.port ?: serverPort)

    /**
     * 계정 재생성(프로세스 재시작)이 필요한 차이인가 — 가용 목록은 선택지 표시용이라 제외한다.
     * 서버가 목록만 늘려도 재시작하지 않기 위함.
     */
    fun sameRegistration(other: SipAccountConfig?): Boolean =
        other != null && copy(transports = emptyList()) == other.copy(transports = emptyList())

    enum class Transport { UDP, TCP, TLS }

    /** transport ↔ 그 transport 의 접속 포트 쌍. 같은 포트로 평문과 TLS 를 겸하지 않으므로 쌍으로 다룬다. */
    data class TransportEndpoint(val transport: Transport, val port: Int)
}
