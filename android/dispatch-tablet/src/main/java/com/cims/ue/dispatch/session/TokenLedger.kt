// 발신 token 과 최종 응답의 만남 (android_dispatch_tablet.md §6.2e)
package com.cims.ue.dispatch.session

/**
 * 발신 token 과 최종 응답의 만남 — **어느 쪽이 먼저 와도 한 번 맞춘다.**
 *
 * token 은 발신 명령이 돌아와야 안다(명령은 IO 스레드에서 돌고, 끝나야 메인으로 돌아온다). 최종 응답은 이벤트 코루틴이
 * 받는다. 둘 다 메인에서 처리되지만 **도착 순서는 보장되지 않는다** — 응답이 먼저 오면 짝을 못 찾아 버려지고, 말풍선은
 * «보내는 중» 에 멈춘다(재기동 때에야 실패로 마감된다).
 *
 * 그래서 **발신 명령이 도는 동안**([begin]~[end])에 짝을 못 찾은 응답을 들고 있다가, 명령이 돌아와 token 을 알게 되면
 * 그 응답을 꺼내 준다. 크기로 버리지 않는다 — 버리면 응답이 몰릴 때 아직 짝이 올 응답이 빠진다. 대신 도는 발신이
 * 하나도 없게 되면 나머지를 버린다 — 그때 남은 것은 짝이 오지 않을 응답(구독·PUBLISH 등 말풍선이 없는 요청)이다.
 *
 * 메인 스레드에서만 쓴다(세션·화면 VM 의 스코프가 모두 `Dispatchers.Main.immediate`) — 잠금이 없다.
 */
internal class TokenLedger<R> {
    private val early = HashMap<Long, R>()
    private var inflight = 0

    /** 발신 명령을 보낸다 — 돌아올 때까지 그 사이에 온 응답을 든다. */
    fun begin() { inflight++ }

    /**
     * 발신 명령이 돌아왔다(token 을 알게 됐다 — 명령이 실패했으면 0). 먼저 와 있던 그 응답을 꺼내 준다(한 번만).
     * 도는 발신이 더 없으면 들고 있던 나머지를 버린다.
     */
    fun end(token: Long): R? {
        val r = if (token > 0) early.remove(token) else null
        if (inflight > 0) inflight--
        if (inflight == 0) early.clear()
        return r
    }

    /** 짝을 못 찾은 응답 — 도는 발신이 있을 때만 든다(그 발신의 응답일 수 있다). */
    fun park(token: Long, result: R) {
        if (inflight > 0 && token > 0) early[token] = result
    }

    /** 로그아웃 — 끝난 계정의 응답은 더 맞출 곳이 없다. */
    fun clear() { early.clear(); inflight = 0 }
}
