package com.cims.ue.core.account

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** [TokenRetry] — 만료 임박 선갱신 · 401 갱신 후 1회 재시도 · 그 외 예외 투과 · 갱신 불가 시 원 실패 보존. */
class TokenRetryTest {

    private class Auth(val code: Int) : RuntimeException("http $code")
    private val is401: (Throwable) -> Boolean = { it is Auth && it.code == 401 }
    private val never: (String) -> Boolean = { false }

    @Test fun fresh_token_succeeds_without_refresh() {
        var refreshes = 0
        val r = TokenRetry.run("t1", refresh = { refreshes++; "t2" }, isAuthFailure = is401, isExpiring = never) { "ok:$it" }
        assertEquals("ok:t1", r.value); assertEquals("t1", r.token); assertFalse(r.refreshed); assertEquals(0, refreshes)
    }

    @Test fun auth_failure_refreshes_and_retries_once() {
        val used = mutableListOf<String>()
        val r = TokenRetry.run("stale", refresh = { assertEquals("stale", it); "fresh" }, isAuthFailure = is401, isExpiring = never) {
            used += it
            if (it == "stale") throw Auth(401) else "ok"
        }
        assertEquals(listOf("stale", "fresh"), used)
        assertEquals("fresh", r.token); assertTrue(r.refreshed)
    }

    @Test fun expiring_token_is_refreshed_before_the_call() {
        val used = mutableListOf<String>()
        val r = TokenRetry.run("old", refresh = { "new" }, isAuthFailure = is401, isExpiring = { it == "old" }) { used += it; "ok" }
        assertEquals(listOf("new"), used); assertTrue(r.refreshed)
    }

    @Test fun second_401_after_refresh_propagates() {
        var calls = 0
        try {
            TokenRetry.run("a", refresh = { "b" }, isAuthFailure = is401, isExpiring = never) { calls++; throw Auth(401) }
            fail("expected Auth")
        } catch (e: Auth) {
            assertEquals(401, e.code)
        }
        assertEquals(2, calls)   // 정확히 1회 재시도
    }

    @Test fun refresh_unavailable_keeps_original_failure() {
        val boom = Auth(401)
        try {
            TokenRetry.run("a", refresh = { null }, isAuthFailure = is401, isExpiring = never) { throw boom }
            fail("expected Auth")
        } catch (e: Auth) { assertSame(boom, e) }
        // 같은 토큰을 돌려주는 갱신도 재시도 안 함(무한 401 루프 방지)
        var calls = 0
        try {
            TokenRetry.run("a", refresh = { "a" }, isAuthFailure = is401, isExpiring = never) { calls++; throw boom }
            fail("expected Auth")
        } catch (e: Auth) { assertSame(boom, e) }
        assertEquals(1, calls)
    }

    @Test fun non_auth_failure_is_not_retried() {
        var refreshes = 0
        var calls = 0
        try {
            TokenRetry.run("a", refresh = { refreshes++; "b" }, isAuthFailure = is401, isExpiring = never) { calls++; throw Auth(500) }
            fail("expected Auth")
        } catch (e: Auth) { assertEquals(500, e.code) }
        assertEquals(1, calls); assertEquals(0, refreshes)
    }

    @Test fun expiring_but_unrefreshable_tries_the_old_token() {
        // 시계가 틀린 단말: 만료로 보이지만 갱신이 안 되면 옛 토큰으로 그냥 시도한다
        val r = TokenRetry.run("old", refresh = { null }, isAuthFailure = is401, isExpiring = { true }) { "ok:$it" }
        assertEquals("ok:old", r.value); assertFalse(r.refreshed)
    }
}
