package com.cims.ue.core.provision

import com.cims.ue.core.account.JwtClaims

/**
 * ID token 검증 (TS 33.180 B.11.1 → OpenID Connect Core §3.1.3.7) — 코어 `sdk/core/src/csc/id_token.cpp` 와 같은 규칙.
 *
 * 단말은 토큰 응답의 ID token 을 검증한다: `iss` 가 발급자(discovery `issuer`)와 정확히 같고, `aud` 에 자기 client_id 가 있고,
 * 지금이 `exp` 전이고, 인증 요청에 nonce 를 보냈으면 같은 nonce 가 실려 있어야 한다. 서명은 보지 않는다 — 인가 코드 흐름의
 * ID token 은 토큰 엔드포인트와의 TLS 로 직접 받으므로 TLS 서버 검증([com.cims.ue.core.net.CimsTls])이 서명 검증을 갈음한다(§3.1.3.7 6)).
 *
 * 파서는 `org.json` 에 기대지 않는다(JVM 단위시험 — [JwtClaims] 와 같은 이유). 페이로드는 평면 JSON 이다.
 */
object IdToken {

    /** 시계 차 허용(초) — TS 33.180 표 B.2.1.2-1 `exp`: «not to exceed 30 seconds». */
    const val CLOCK_LEEWAY_SEC = 30L

    private fun str(name: String) = Regex("\"" + name + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"")
    private val AUD_ARRAY = Regex("\"aud\"\\s*:\\s*\\[(.*?)]", RegexOption.DOT_MATCHES_ALL)
    private val STRING = Regex("\"((?:[^\"\\\\]|\\\\.)*)\"")
    private val EXP = Regex("\"exp\"\\s*:\\s*(\\d+)")

    /**
     * 거절 사유 — 통과하면 null. [nonce] 가 null 이면 nonce 는 보지 않는다(refresh 응답 — OIDC Core §12.2).
     */
    fun rejectReason(idToken: String?, issuer: String, clientId: String, nonce: String?, nowEpoch: Long = JwtClaims.now()): String? {
        if (idToken.isNullOrBlank()) return "id_token missing"
        val parts = idToken.split('.')
        if (parts.size != 3) return "id_token is not a JWT"
        val payload = try {
            val p = parts[1]
            String(java.util.Base64.getUrlDecoder().decode(if (p.length % 4 == 0) p else p + "=".repeat(4 - p.length % 4)), Charsets.UTF_8)
        } catch (_: IllegalArgumentException) {
            return "id_token payload is not base64url"
        }
        // iss — 발급자와 정확히 같아야 한다(§3.1.3.7 2))
        if (issuer.isEmpty() || str("iss").find(payload)?.groupValues?.get(1) != issuer) return "iss does not match the issuer"
        // aud — 내 client_id 가 있어야 하고, 다른 audience 는 믿지 않는다(§3.1.3.7 3))
        val audiences = AUD_ARRAY.find(payload)?.let { m -> STRING.findAll(m.groupValues[1]).map { it.groupValues[1] }.toList() }
            ?: listOfNotNull(str("aud").find(payload)?.groupValues?.get(1))
        if (clientId !in audiences || audiences.any { it != clientId }) return "aud does not match the client_id"
        // exp — 지금이 그 전이어야 한다(§3.1.3.7 9))
        val exp = EXP.find(payload)?.groupValues?.get(1)?.toLongOrNull() ?: return "exp missing"
        if (nowEpoch >= exp + CLOCK_LEEWAY_SEC) return "id_token expired (check the device clock)"
        // nonce — 보냈으면 같은 값이 실려 있어야 한다(§3.1.3.7 11))
        if (nonce != null && str("nonce").find(payload)?.groupValues?.get(1) != nonce) return "nonce does not match"
        return null
    }
}
