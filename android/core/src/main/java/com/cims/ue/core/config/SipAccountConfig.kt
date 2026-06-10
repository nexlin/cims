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
    val serverPort: Int = 5060,         // SIP 포트      (예: 15060)
    val transport: Transport = Transport.UDP,
    val domain: String = "",            // 홈/서비스 도메인 (예: ims.mnc033.mcc450.3gppnetwork.org)
    val msisdn: String = "",            // 전화번호(공개 ID / AOR user part)
    val imsi: String = "",              // IMSI — Digest username(IMPI) 합성용 (IMSI@domain)
    val displayName: String = "",       // 이름
    val loginId: String = "",           // 로그인 ID (CSC 등)
    val authId: String = "",            // 전체 IMPI 직접 지정(고급). 비우면 imsi@domain 합성
    val password: String = "",
    val expiresSec: Int = 3600,         // 희망 등록 주기(서버는 200 OK 에서 3600 하드코딩으로 덮어씀)
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
        password.isNotBlank()

    enum class Transport { UDP, TCP, TLS }
}
