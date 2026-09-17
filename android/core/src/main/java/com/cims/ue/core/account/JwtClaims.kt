package com.cims.ue.core.account

/**
 * JWT 페이로드의 `exp` 만 읽는다 — 서명 검증은 하지 않는다(그것은 서버의 일). 목적은 하나, **만료된
 * access token 을 서버에 보내기 전에 걸러 refresh 를 먼저 돌리는 것**(sip_tls_signaling 과 무관한 OAuth 계층).
 *
 * 배경: AccountManager 는 `setAuthToken` 으로 넣은 토큰을 누가 `invalidateAuthToken` 하기 전까지 그대로
 * 돌려준다. IdMS access token 은 1시간(서버 `IdMs.AccessTokenTtl`)이라, 인증기가 캐시를 그대로 내주면
 * 1시간 뒤부터 모든 XCAP/GMS 호출이 401(`Signature has expired`)로 끝난다 — 실측 09-17.
 *
 * 파서는 `org.json` 에 기대지 않는다(JVM 단위시험에서 android 스텁이 막힘) — 페이로드는 평면 JSON 이고
 * `exp` 는 정수라 정규식으로 충분하다. 형식이 아니면(불투명 토큰·손상) null 을 돌려 호출자가 "모른다" 로
 * 처리하게 한다 — 모르는 토큰을 만료로 단정해 멀쩡한 세션을 끊지 않는다.
 */
object JwtClaims {

    private val EXP = Regex(""""exp"\s*:\s*(\d+)""")

    /** `exp`(epoch 초). JWT 형식이 아니거나 `exp` 가 없으면 null. */
    fun expEpoch(token: String?): Long? {
        if (token.isNullOrBlank()) return null
        val parts = token.split('.')
        if (parts.size < 2) return null
        val payload = try {
            String(java.util.Base64.getUrlDecoder().decode(padB64Url(parts[1])), Charsets.UTF_8)
        } catch (_: IllegalArgumentException) {
            return null
        }
        return EXP.find(payload)?.groupValues?.get(1)?.toLongOrNull()
    }

    /**
     * 지금부터 [marginSec] 안에 만료하는가. `exp` 를 모르면 false(그대로 쓴다).
     * 여유 60초 = 단말↔서버 시계 차이 + 요청 왕복 — 만료 직전 토큰을 보내 401 로 되돌아오는 낭비를 막는다.
     */
    fun isExpiring(token: String?, marginSec: Long = DEFAULT_MARGIN_SEC, nowEpoch: Long = now()): Boolean {
        val exp = expEpoch(token) ?: return false
        return exp - nowEpoch <= marginSec
    }

    const val DEFAULT_MARGIN_SEC = 60L

    fun now(): Long = System.currentTimeMillis() / 1000L

    /** base64url 은 패딩을 생략하므로(RFC 7515 §2) 디코더 앞에서 채운다. */
    private fun padB64Url(s: String): String {
        val r = s.length % 4
        return if (r == 0) s else s + "=".repeat(4 - r)
    }
}
