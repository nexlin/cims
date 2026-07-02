package com.cims.ue.core.provision

import com.cims.ue.core.config.SipAccountConfig

/** IdMS 토큰 응답 (POST /idms/tokenreq). */
data class TokenSet(
    val accessToken: String,
    val tokenType: String,
    val refreshToken: String?,
    val idToken: String?,
    val expiresInSec: Int,
    val scope: String?,
)

/** 로그인 대상 CSC(IdMS/프로비저닝) 접속 설정. */
data class CscEndpoint(
    val host: String,
    val port: Int = 4430,
    val clientId: String = "MCPTT_UE",
    val redirectUri: String = "https://localhost/callback",
    // 로그인 시 두 scope 함께 grant — 이후 AccountManager 가 용도별로 좁혀 발급.
    //   cims:provisioning=부트스트랩(/provisioning/me), 3gpp:mcptt:ptt_server=MCPTT 서비스(TS 33.180).
    val scope: String = "openid cims:provisioning 3gpp:mcptt:ptt_server",
) {
    val baseUrl: String get() = "https://$host:$port"
}

/** `GET /provisioning/me` 전체 응답 (서비스별 프로파일 목록). */
data class ProvisioningProfile(
    val displayName: String?,
    val loginId: String?,
    val countryCode: String?,         // 홈 국가코드(digits, 예 "82") — 번호 로컬 표기 SoT. 구서버는 null
    val services: List<ServiceProfile>,
) {
    /** 주어진 kind("volte"/"ptt")의 서비스 프로파일(없으면 null). */
    fun service(kind: String): ServiceProfile? = services.firstOrNull { it.kind.equals(kind, ignoreCase = true) }
}

/** 한 서비스(VoLTE=CSP / PTT=PSP) 의 접속·계정 — 서버마다 다를 수 있음. */
data class ServiceProfile(
    val kind: String,                 // "volte" | "ptt"
    val sipHost: String,
    val sipPort: Int,
    val transport: String,            // UDP/TCP/TLS
    val domain: String,
    val msisdn: String,
    val imsi: String,
    val authId: String = "",
    val sipPassword: String? = null,  // null 이면 로그인 비번 재사용
    val mcpttId: String? = null,      // PTT 전용
) {
    /**
     * 이 서비스 프로파일을 [SipAccountConfig] 로 매핑. SIP Digest 비번은 [sipPassword] 우선,
     * 없으면 [loginPassword](로그인 비번) 재사용. [countryCode] = 프로비저닝 응답 홈 국가코드.
     */
    fun toSipAccountConfig(
        loginId: String,
        displayName: String,
        loginPassword: String,
        countryCode: String = "",
    ): SipAccountConfig =
        SipAccountConfig(
            serverHost = sipHost,
            serverPort = sipPort,
            transport = runCatching { SipAccountConfig.Transport.valueOf(transport.uppercase()) }
                .getOrDefault(SipAccountConfig.Transport.UDP),
            domain = domain,
            msisdn = msisdn,
            imsi = imsi,
            displayName = displayName,
            loginId = loginId,
            authId = authId,
            password = sipPassword?.takeIf { it.isNotBlank() } ?: loginPassword,
            countryCode = countryCode,
        )
}
