package com.cims.ue.core.account

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Base64

/** [JwtClaims] — exp 파싱과 만료 임박 판정. 서명은 보지 않으므로 헤더·서명부는 임의 문자열이다. */
class JwtClaimsTest {

    private fun jwt(payload: String): String {
        val b64 = Base64.getUrlEncoder().withoutPadding()
        val h = b64.encodeToString("""{"alg":"HS256","typ":"JWT"}""".toByteArray())
        val p = b64.encodeToString(payload.toByteArray())
        return "$h.$p.sig"
    }

    @Test fun exp_parsed_from_flat_payload() {
        assertEquals(1_800_000_000L,
            JwtClaims.expEpoch(jwt("""{"sub":"tel:+8250001","scope":"3gpp:mc:ptt_service","exp":1800000000,"iat":1799996400}""")))
    }

    @Test fun exp_parsed_with_spaces_and_unpadded_base64url() {
        // 길이가 4 의 배수가 아닌 페이로드(패딩 생략) — 디코더 앞에서 채워야 한다
        assertEquals(1234L, JwtClaims.expEpoch(jwt("""{ "exp" : 1234 }""")))
    }

    @Test fun no_exp_or_not_a_jwt_is_unknown() {
        assertNull(JwtClaims.expEpoch(jwt("""{"sub":"x"}""")))
        assertNull(JwtClaims.expEpoch("opaque-token"))
        assertNull(JwtClaims.expEpoch("a.%%%not-base64%%%.c"))
        assertNull(JwtClaims.expEpoch(null))
        assertNull(JwtClaims.expEpoch(""))
    }

    @Test fun expiring_within_margin() {
        val now = 1_800_000_000L
        val t = jwt("""{"exp":${now + 30}}""")
        assertTrue(JwtClaims.isExpiring(t, marginSec = 60, nowEpoch = now))      // 30초 남음 < 60초 여유
        assertTrue(JwtClaims.isExpiring(jwt("""{"exp":${now - 3600}}"""), 60, now)) // 이미 만료
        assertFalse(JwtClaims.isExpiring(jwt("""{"exp":${now + 3500}}"""), 60, now)) // 여유 있음
    }

    @Test fun unknown_exp_is_not_expiring() {
        // 불투명 토큰·손상 토큰을 만료로 단정해 멀쩡한 세션을 끊지 않는다
        assertFalse(JwtClaims.isExpiring("opaque-token", 60, 1_800_000_000L))
        assertFalse(JwtClaims.isExpiring(null))
    }
}
