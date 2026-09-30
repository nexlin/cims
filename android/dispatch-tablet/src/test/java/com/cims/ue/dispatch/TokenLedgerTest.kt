// 발신 token 과 최종 응답의 만남(`TokenLedger`) — JVM, 기기 불필요 (android_dispatch_tablet.md §6.2e)
//
// 노리는 것은 **응답이 token 보다 먼저 와서 버려지는** 결함이다 — 말풍선이 «보내는 중» 에 멈추고, 전달 확인 회신의 거절이
// 어디에도 남지 않는다. 응답이 몰려도 아직 짝이 올 응답을 버리지 않아야 하고, 짝 없는 응답이 끝없이 쌓여도 안 된다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.TokenLedger
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class TokenLedgerTest {

    @Test fun `명령이 도는 동안 온 응답은 명령이 돌아올 때 꺼낸다 — 한 번만`() {
        val l = TokenLedger<Int>()
        l.begin()
        l.park(7, 480)                                   // 응답이 먼저 왔다
        assertEquals(480, l.end(7))
    }

    @Test fun `응답이 몰려도 아직 짝이 올 응답은 버리지 않는다`() {
        val l = TokenLedger<Int>()
        l.begin()
        l.park(7, 480)
        (100L..400L).forEach { l.park(it, 200) }         // 말풍선 없는 요청의 응답이 잔뜩 온다
        assertEquals(480, l.end(7))
    }

    @Test fun `도는 명령이 없으면 들지 않는다`() {
        val l = TokenLedger<Int>()
        l.park(7, 200)                                   // 구독 갱신 등 — 기다리는 발신이 없다
        l.begin()
        assertNull(l.end(7))
    }

    @Test fun `마지막 명령이 돌아오면 남은 것을 버린다`() {
        val l = TokenLedger<Int>()
        l.begin(); l.begin()
        l.park(8, 200)                                   // 두 번째 명령의 응답
        assertNull(l.end(7))                             // 첫 명령은 짝이 없다 — 아직 하나가 돈다
        assertEquals(200, l.end(8))
        l.begin()
        l.park(9, 200)
        l.end(0)                                         // 명령 실패(token 없음) — 도는 명령이 없어졌다
        l.begin()
        assertNull("앞 창에서 남은 것은 버렸다", l.end(9))
    }

    @Test fun `로그아웃하면 비운다`() {
        val l = TokenLedger<Int>()
        l.begin()
        l.park(5, 200)
        l.clear()
        l.begin()
        assertNull(l.end(5))
    }
}
