package com.cims.ue.core.provision

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import java.util.Base64

/** [IdToken] — ID token claim 검증(TS 33.180 B.11.1 → OIDC Core §3.1.3.7). 서명은 보지 않으므로 헤더·서명부는 임의 문자열이다. */
class IdTokenTest {

    private fun jwt(payload: String): String {
        val b64 = Base64.getUrlEncoder().withoutPadding()
        return b64.encodeToString("""{"alg":"RS256","kid":"k"}""".toByteArray()) + "." + b64.encodeToString(payload.toByteArray()) + ".sig"
    }

    private fun claims(iss: String = "idms.ptt.example", aud: String = "\"MCPTT_UE\"", exp: Long = 2000, nonce: String? = "n-1") =
        jwt("""{"mcptt_id":"tel:+8250001","iss":"$iss","sub":"u","aud":$aud,"exp":$exp,"iat":1""" +
            (nonce?.let { ""","nonce":"$it"""" } ?: "") + "}")

    private fun reason(token: String?, nonce: String? = "n-1", now: Long = 1000) =
        IdToken.rejectReason(token, "idms.ptt.example", "MCPTT_UE", nonce, now)

    @Test fun valid_token_passes() {
        assertNull(reason(claims()))
        assertNull(reason(claims(aud = """["MCPTT_UE"]""")))
        assertNull("refresh 응답은 nonce 를 보지 않는다", reason(claims(nonce = null), nonce = null))
    }

    @Test fun issuer_audience_expiry_nonce_are_checked() {
        assertEquals("iss does not match the issuer", reason(claims(iss = "evil.example")))
        assertEquals("aud does not match the client_id", reason(claims(aud = "\"OTHER\"")))
        assertEquals("aud does not match the client_id", reason(claims(aud = """["MCPTT_UE","other"]""")))
        assertEquals("aud does not match the client_id", reason(claims(aud = "[]")))
        assertEquals("nonce does not match", reason(claims(nonce = "stale")))
        assertEquals("nonce does not match", reason(claims(nonce = null)))
        assertEquals("exp missing", reason(jwt("""{"iss":"idms.ptt.example","aud":"MCPTT_UE"}""")))
    }

    @Test fun expiry_allows_thirty_seconds_of_clock_skew() {
        assertNull(reason(claims(exp = 2000), now = 2029))
        assertNotNull(reason(claims(exp = 2000), now = 2030))
    }

    @Test fun missing_or_malformed_token_is_rejected() {
        assertEquals("id_token missing", reason(null))
        assertEquals("id_token missing", reason(""))
        assertEquals("id_token is not a JWT", reason("opaque"))
        assertNotNull(reason("a.%%%.c"))
        assertNotNull("발급자를 모르면 통과시키지 않는다", IdToken.rejectReason(claims(), "", "MCPTT_UE", "n-1", 1000))
    }
}
