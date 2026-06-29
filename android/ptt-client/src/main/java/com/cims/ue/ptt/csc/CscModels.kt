package com.cims.ue.ptt.csc

/** IdMS 토큰 응답 (POST /idms/tokenreq). */
data class TokenSet(
    val accessToken: String,
    val tokenType: String,
    val refreshToken: String?,
    val idToken: String?,
    val expiresInSec: Int,
    val scope: String?,
)

/** GMS 그룹 목록 항목 (GET /org.openmobilealliance.groups/users/{me}). */
data class GroupSummary(
    val uri: String,
    val displayName: String?,
    val etag: String?,
    val memberCount: Int?,
)

/** ETag 캐시가 적용되는 XCAP 문서 조회 결과. [notModified] 면 [body] 는 캐시 사용. */
data class XcapDoc(
    val notModified: Boolean,
    val body: String?,
    val etag: String?,
    val contentType: String?,
)

/** CSC 접속 설정. */
data class CscConfig(
    val host: String,
    val port: Int = 4430,
    val clientId: String = "MCPTT_UE",
    val redirectUri: String = "https://localhost/callback",
    val scope: String = "openid 3gpp:mcptt:ptt_server",
) {
    val baseUrl: String get() = "https://$host:$port"
}
