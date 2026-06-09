package com.cims.ue.core.config

/**
 * SIP 단말 접속/계정 설정. **앱 첫 실행 시 사용자가 입력**하며 하드코딩하지 않는다.
 * volte-client / ptt-client 공용.
 *
 * SIP 매핑:
 *  - 공개 ID(AOR/IMPU) = `sip:<msisdn>@<domain>`  (From/To/Contact user part)
 *  - Digest username    = authId (비우면 msisdn 사용)
 *  - Registrar          = serverHost:serverPort (transport)
 *  - realm              = domain
 */
data class SipAccountConfig(
    val serverHost: String = "",        // CSP IP/FQDN  (예: 121.161.164.47)
    val serverPort: Int = 5060,         // SIP 포트      (예: 15060)
    val transport: Transport = Transport.UDP,
    val domain: String = "",            // realm/도메인 (예: ims.mnc033.mcc450.3gppnetwork.org)
    val msisdn: String = "",            // 전화번호(공개 ID)
    val displayName: String = "",       // 이름
    val loginId: String = "",           // 로그인 ID
    val authId: String = "",            // Digest username (비우면 msisdn)
    val password: String = "",
    val expiresSec: Int = 3600,
) {
    val effectiveAuthId: String get() = authId.ifBlank { msisdn }

    /** sip:<msisdn>@<domain> */
    val aor: String get() = "sip:$msisdn@$domain"

    /** 등록/통화에 필요한 필수값이 모두 채워졌는가 */
    fun isComplete(): Boolean =
        serverHost.isNotBlank() &&
        serverPort in 1..65535 &&
        domain.isNotBlank() &&
        msisdn.isNotBlank() &&
        password.isNotBlank()

    enum class Transport { UDP, TCP, TLS }
}
