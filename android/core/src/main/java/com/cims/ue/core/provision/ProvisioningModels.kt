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
    val transport: String,            // UDP/TCP/TLS — 서버 권장 기본값(`sip.default`)
    /** 가용 transport 목록(`sip.transports`) — 단말이 이 중에서 고른다. 목록을 안 주는 구 서버
     *  응답이면 **빈 목록**이 되고, 단말은 선택 UI 를 숨긴 채 단일 필드([transport]/[sipPort])로 동작한다. */
    val transports: List<SipAccountConfig.TransportEndpoint> = emptyList(),
    val domain: String,
    val msisdn: String,
    val imsi: String,
    val authId: String = "",
    /** SIP Digest H(A1)=MD5(IMPI:realm:pw) hex32 — 평문 비번 없이 response 계산(sip_access_security.md §4.7).
     *  있으면 [sipPassword] 보다 우선. 구 서버 응답이면 null. */
    val sipHa1: String? = null,
    val sipPassword: String? = null,  // 과도기 평문(passwd 소거 후 항상 null). null 이면 로그인 비번 재사용
    /** 인증 체계(`account.authScheme`) — "digest" | "aka". aka 면 [akaK]/[akaOpc] 소프트-USIM 자격
     *  (sip_access_security.md §8.2 — 토큰 인증 + TLS 채널로만 내려온다). */
    val authScheme: String = "digest",
    val akaK: String = "",
    val akaOpc: String = "",
    val akaAmf: String = "8000",
    /** 서버 제시 채널 보호 목록(`sip.security`, RFC 3329) — ["tls"] | ["tls","ipsec-3gpp"]. */
    val secMechanisms: List<String> = emptyList(),
    /** 미디어 SRTP(SDES) 정책(`sip.mediaSecurity`) — "off"|"optional"|"required".
     *  서버 접속서비스 media_srtp 와 같은 값(media_security.md §7.2). 구 서버 응답이면 "off". */
    val mediaSecurity: String = "off",
    val mcpttId: String? = null,      // PTT 전용
    /** MCData C-plane SDS payload 상한(byte) — 초과 시 MSRP 미디어평면 발신(TS 24.282 §9.2.1.1).
     *  0/미수신 = 무제한(항상 C-plane MESSAGE). 서버 `services[].mcdata.maxPayloadSdsCplaneBytes`. */
    val maxPayloadSdsCplaneBytes: Int = 0,
) {
    /**
     * 이 서비스 프로파일을 [SipAccountConfig] 로 매핑. SIP Digest 자료는 [sipHa1](H(A1)) 최우선,
     * 평문은 [sipPassword] 우선·없으면 [loginPassword](로그인 비번) 재사용.
     * [countryCode] = 프로비저닝 응답 홈 국가코드.
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
            transports = transports,
            domain = domain,
            msisdn = msisdn,
            imsi = imsi,
            displayName = displayName,
            loginId = loginId,
            authId = authId,
            sipHa1 = sipHa1?.takeIf { it.isNotBlank() }.orEmpty(),
            password = sipPassword?.takeIf { it.isNotBlank() } ?: loginPassword,
            authScheme = authScheme,
            akaK = akaK,
            akaOpc = akaOpc,
            akaAmf = akaAmf,
            secMechanisms = secMechanisms,
            mediaSecurity = mediaSecurity,
            countryCode = countryCode,
            maxPayloadSdsCplaneBytes = maxPayloadSdsCplaneBytes,
        )
}
