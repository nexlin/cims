// 이동 규칙 — 내비·뒤로가기 (android_dispatch_tablet.md §6.3)
//
// 지키는 불변 하나: **`onBack()` 이 값을 돌려주면 화면이 눈에 보이게 바뀐다.** 어긋나면 뒤로가기를
// 먹고도 아무 일이 없어 관제사는 앱이 멈춘 줄 안다 — 가로채기 판정이 같은 함수를 쓰는 이유다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.ui.AppScreen
import com.cims.ue.dispatch.ui.CallPane
import com.cims.ue.dispatch.ui.MoreItem
import com.cims.ue.dispatch.ui.NavState
import com.cims.ue.dispatch.ui.PttPane
import com.cims.ue.dispatch.ui.onBack
import com.cims.ue.dispatch.ui.onNav
import com.cims.ue.dispatch.ui.openChannel
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test

class NavigationTest {

    private val home = NavState(AppScreen.HISTORY)

    // ── 내비 ──────────────────────────────────────────────────────────────

    /** **두 칸 이상 떨어져 있어도 한 번에** 그 메뉴로. 중간 메뉴를 거치지 않는다. */
    @Test fun 내비는_멀어도_한번에_간다() {
        assertEquals(AppScreen.MORE, home.onNav(AppScreen.MORE).screen)
        assertEquals(AppScreen.HISTORY, home.onNav(AppScreen.MORE).onNav(AppScreen.HISTORY).screen)
    }

    /** 메뉴를 옮겨도 **그 메뉴에서 보던 면**은 남는다. */
    @Test fun 면은_메뉴마다_기억된다() {
        val s = NavState(AppScreen.CALLS, callPane = CallPane.LOG, pttPane = PttPane.EVENTS)
            .onNav(AppScreen.PTT).onNav(AppScreen.CALLS)
        assertEquals(CallPane.LOG, s.callPane)
        assertEquals(PttPane.EVENTS, s.pttPane)
    }

    /** 같은 항목 재탭 = **그 메뉴의** 안쪽 닫기. 다른 메뉴가 기억한 안쪽은 그대로. */
    @Test fun 같은_항목_재탭은_제_메뉴만_닫는다() {
        val s = NavState(AppScreen.MORE, channel = "g001", more = MoreItem.ADMIN)
        val after = s.onNav(AppScreen.MORE)
        assertNull(after.more)
        assertEquals("g001", after.channel)          // 무전의 채널은 남는다
    }

    /** 다른 메뉴로 옮기는 것은 안쪽 화면을 닫지 않는다 — 돌아오면 보던 자리가 있어야 한다. */
    @Test fun 메뉴_이동은_안쪽을_닫지_않는다() {
        val s = NavState(AppScreen.PTT, channel = "g001").onNav(AppScreen.CALLS)
        assertEquals("g001", s.channel)
    }

    // ── 뒤로가기 ───────────────────────────────────────────────────────────

    /** 순서 — ① 안쪽 화면 → ② 그 메뉴의 첫 면 → ③ 첫 화면 → ④ 없음. */
    @Test fun 한_겹씩_되돌린다() {
        var s = NavState(AppScreen.MORE, more = MoreItem.PTT_GROUPS)
        s = s.onBack()!!; assertNull(s.more); assertEquals(AppScreen.MORE, s.screen)
        s = s.onBack()!!; assertEquals(AppScreen.HISTORY, s.screen)
        assertNull(s.onBack())
    }

    @Test fun 면부터_되돌린다() {
        var s = NavState(AppScreen.CALLS, callPane = CallPane.LOG)
        s = s.onBack()!!; assertEquals(CallPane.CALLS, s.callPane); assertEquals(AppScreen.CALLS, s.screen)
        s = s.onBack()!!; assertEquals(AppScreen.HISTORY, s.screen)
    }

    /**
     * **핵심 회귀** — 보고 있지 않은 메뉴의 안쪽 화면을 닫아 «보이지 않는 한 걸음» 을 만들면 안 된다.
     * [무전] 에 채널을 열어 둔 채 [통화] 에서 뒤로가기를 눌렀을 때가 그 자리였다.
     */
    @Test fun 되돌릴_때마다_화면이_바뀐다() {
        val starts = listOf(
            NavState(AppScreen.CALLS, channel = "g001"),                       // 남의 메뉴 안쪽
            NavState(AppScreen.HISTORY, channel = "g001", more = MoreItem.ADMIN),
            NavState(AppScreen.PTT, channel = "g001", pttPane = PttPane.EVENTS),
            NavState(AppScreen.CALLS, more = MoreItem.ADMIN, callPane = CallPane.MESSAGES),
            NavState(AppScreen.MORE, more = MoreItem.PTT_GROUPS, pttPane = PttPane.MESSAGES))
        starts.forEach { start ->
            var s = start
            var guard = 0
            while (guard++ < 12) {
                val next = s.onBack() ?: break
                assertNotEquals("onBack() 이 값을 줬는데 좌표가 그대로다: $s", s, next)
                s = next
            }
            assertEquals("끝까지 되돌리면 첫 화면이어야 한다", AppScreen.HISTORY, s.screen)
            assertNull("첫 화면에서는 가로채지 않는다", s.onBack())
        }
    }

    /**
     * 채널 열기는 **어디서든 «채널» 면**으로 간다 — 채널 화면은 그 면에만 그려진다.
     * «메시지» 면에 있었거나(또는 다른 메뉴에서 [무전] 이 «이벤트» 면을 기억하고 있을 때) 긴급 배너를 눌러도 채널이 보여야 한다.
     */
    @Test fun 채널_열기는_어디서든_채널_면으로() {
        listOf(
            NavState(AppScreen.PTT, pttPane = PttPane.MESSAGES),
            NavState(AppScreen.CALLS, pttPane = PttPane.EVENTS, callPane = CallPane.LOG),
            NavState(AppScreen.MORE, more = MoreItem.entries.first()),
        ).forEach { start ->
            val s = start.openChannel("g1")
            assertEquals(AppScreen.PTT, s.screen)
            assertEquals(PttPane.CHANNELS, s.pttPane)
            assertEquals("g1", s.channel)
            assertEquals("다른 메뉴가 기억한 자리는 건드리지 않는다", start.callPane, s.callPane)
            assertEquals(start.more, s.more)
        }
    }

    /** 첫 화면·첫 면·안쪽 없음 = 되돌릴 것이 없다(가로채면 앱을 벗어날 수 없다). */
    @Test fun 첫_화면에서는_없음() {
        assertNull(home.onBack())
        // 다른 메뉴의 면이 첫 면이 아니어도, 보고 있지 않으면 되돌릴 것이 아니다.
        assertNull(NavState(AppScreen.HISTORY, callPane = CallPane.LOG, pttPane = PttPane.EVENTS).onBack())
    }
}
