package com.cims.ue.core.provision

import java.security.MessageDigest
import java.security.SecureRandom

/**
 * OAuth2 PKCE (RFC 7636, **S256만**) — MCPTT IdMS 로그인 (3GPP TS 33.180).
 * 공유 위치(core): volte-client·ptt-client 둘 다 로그인에 사용.
 *
 * code_verifier = [43,128] unreserved 문자열, code_challenge = BASE64URL(SHA-256(verifier)).
 */
object Pkce {

    private val rnd = SecureRandom()

    /** code_verifier — 32바이트 난수의 base64url(=43자). */
    fun newVerifier(): String {
        val bytes = ByteArray(32)
        rnd.nextBytes(bytes)
        return base64Url(bytes)
    }

    /** code_challenge = BASE64URL(SHA-256(verifier)). */
    fun challenge(verifier: String): String {
        val sha = MessageDigest.getInstance("SHA-256").digest(verifier.toByteArray(Charsets.US_ASCII))
        return base64Url(sha)
    }

    /** CSRF state 값. */
    fun newState(): String {
        val bytes = ByteArray(16)
        rnd.nextBytes(bytes)
        return base64Url(bytes)
    }

    private fun base64Url(b: ByteArray): String =
        java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(b)
}
