package com.cims.ue.core.account

/**
 * 토큰이 필요한 서버 호출을 "만료면 먼저 갱신, 401 이면 갱신 후 1회 재시도" 로 감싼다 — Android
 * AccountManager 계약(거절된 토큰은 invalidate 하고 다시 받는다)을 호출자가 매번 손으로 쓰지 않게 한다.
 *
 * 플랫폼 무관(순수 Kotlin)이라 JVM 단위시험 대상이다. 갱신 함수·인증 실패 판정은 호출자가 준다:
 *  - [refresh]      : 실패한(또는 만료 임박) 토큰을 받아 새 토큰을 돌려준다. null = 갱신 불가(재로그인 필요).
 *  - [isAuthFailure]: 예외가 "토큰 거절"(HTTP 401)인가. 그 외 예외는 재시도하지 않고 그대로 던진다.
 *  - [isExpiring]   : 호출 전에 토큰이 만료 임박인가(기본 [JwtClaims.isExpiring]).
 *
 * 재시도는 정확히 한 번이다 — 갱신한 토큰도 거절되면 그것은 만료가 아니라 계정 문제라 예외를 그대로 낸다.
 */
object TokenRetry {

    /** 실행 결과와 **실제로 성공에 쓰인 토큰**(갱신됐으면 새 것) — 호출자는 이 토큰을 보관해 다음 호출에 쓴다. */
    data class Outcome<T>(val value: T, val token: String, val refreshed: Boolean)

    fun <T> run(
        token: String,
        refresh: (stale: String) -> String?,
        isAuthFailure: (Throwable) -> Boolean,
        isExpiring: (String) -> Boolean = { JwtClaims.isExpiring(it) },
        block: (String) -> T,
    ): Outcome<T> {
        var current = token
        var refreshed = false
        if (isExpiring(current)) {
            val fresh = refresh(current)
            if (!fresh.isNullOrEmpty()) { current = fresh; refreshed = true }
            // 갱신 불가면 옛 토큰으로 그냥 시도한다 — 시계가 틀린 단말에서 멀쩡한 토큰을 버리지 않기 위해.
        }
        return try {
            Outcome(block(current), current, refreshed)
        } catch (e: Throwable) {
            if (refreshed || !isAuthFailure(e)) throw e
            val fresh = refresh(current)
            if (fresh.isNullOrEmpty() || fresh == current) throw e
            Outcome(block(fresh), fresh, true)
        }
    }
}
