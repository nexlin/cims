package com.cims.ue.ptt.csc

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** PKCE S256 검증 — RFC 7636 Appendix B 표준 벡터. */
class PkceTest {

    @Test fun rfc7636Vector() {
        // RFC 7636 §B: verifier → challenge(S256)
        val verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
        val expected = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
        assertEquals(expected, Pkce.challenge(verifier))
    }

    @Test fun verifierIsUrlSafeAndLongEnough() {
        val v = Pkce.newVerifier()
        assertTrue("length ${v.length}", v.length in 43..128)
        assertTrue(v.all { it.isLetterOrDigit() || it == '-' || it == '_' })   // base64url, no padding
    }

    @Test fun challengeIsUnpaddedBase64Url() {
        val c = Pkce.challenge(Pkce.newVerifier())
        assertTrue(c.none { it == '=' || it == '+' || it == '/' })
    }
}
