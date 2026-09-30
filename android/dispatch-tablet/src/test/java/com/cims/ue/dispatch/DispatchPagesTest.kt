// 관제의 면 일곱 장 — 한 줄의 차례 (android_dispatch_tablet.md §6.3)
//
// 면은 [무전] 셋(채널·메시지·이벤트) 다음에 [통화] 넷(통화·주소록·메시지·통화내역)을 한 줄로 꿴다(`DISPATCH_PAGES`). 좌우로 밀면
// 옆 장이 오고, 무전의 끝 면에서 더 밀면 통화의 첫 면이다 — 그 이음매와 모드 전환을 VM 규칙(`toPage`)으로 걸어 본다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.ui.CallPane
import com.cims.ue.dispatch.ui.DISPATCH_PAGES
import com.cims.ue.dispatch.ui.DispatchMode
import com.cims.ue.dispatch.ui.DispatchPage
import com.cims.ue.dispatch.ui.NavState
import com.cims.ue.dispatch.ui.PttPane
import com.cims.ue.dispatch.ui.pageOf
import com.cims.ue.dispatch.ui.toPage
import org.junit.Assert.assertEquals
import org.junit.Test

class DispatchPagesTest {

    @Test fun `일곱 장이 한 줄이다 — 무전 셋 다음 통화 넷`() {
        assertEquals(7, DISPATCH_PAGES.size)
        assertEquals(List(3) { DispatchMode.PTT } + List(4) { DispatchMode.CALL }, DISPATCH_PAGES.map { it.mode })
    }

    /** 모드 안의 면 순서는 탭 순서와 같다 — 두 곳에 적으면 한쪽만 고치게 된다. */
    @Test fun `면 순서는 탭 순서다`() {
        assertEquals(PttPane.entries.toList(), DISPATCH_PAGES.mapNotNull { it.pttPane })
        assertEquals(CallPane.entries.toList(), DISPATCH_PAGES.mapNotNull { it.callPane })
    }

    @Test fun `차례 번호와 면이 서로 맞다`() {
        DISPATCH_PAGES.forEachIndexed { i, p -> assertEquals(i, p.index) }
        assertEquals(0, pageOf(PttPane.CHANNELS).index)
        assertEquals(3, pageOf(CallPane.CALLS).index)
    }

    /** 이음매 — 무전›«이벤트» 에서 앞으로 밀면 통화›«통화», 그 반대도(모드가 함께 바뀐다). */
    @Test fun `무전의 끝 면에서 밀면 통화의 첫 면이다`() {
        val atEvents = NavState().toPage(pageOf(PttPane.EVENTS))
        val next = atEvents.toPage(DISPATCH_PAGES[atEvents.page.index + 1])
        assertEquals(DispatchMode.CALL, next.mode)
        assertEquals(CallPane.CALLS, next.callPane)
        assertEquals("무전에서 보던 면은 남는다", PttPane.EVENTS, next.pttPane)
        val back = next.toPage(DISPATCH_PAGES[next.page.index - 1])
        assertEquals(DispatchPage(DispatchMode.PTT, PttPane.EVENTS.ordinal), back.page)
    }

    /** 끝까지 밀어 가면 일곱 장을 차례대로 지난다. */
    @Test fun `계속 밀면 차례대로 지난다`() {
        var s = NavState()
        val walked = mutableListOf(s.page)
        repeat(DISPATCH_PAGES.size - 1) { s = s.toPage(DISPATCH_PAGES[s.page.index + 1]); walked += s.page }
        assertEquals(DISPATCH_PAGES, walked)
    }
}
