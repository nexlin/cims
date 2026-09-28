// 닿는 차례와 이음매 (android_dispatch_tablet.md §6.3)
//
// 화면은 pager 둘이 겹쳐 있고(바깥=메뉴, 안쪽=면), 안쪽이 끝에 닿으면 바깥으로 넘어간다. 그 결과가
// `APP_PAGES` 한 줄이 되는지를 **이음매 규칙(`entryPane`)으로 실제로 걸어 보며** 확인한다 —
// 차례표만 검사하면 표가 자기 자신을 확인하는 꼴이 된다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.ui.APP_PAGES
import com.cims.ue.dispatch.ui.AppPage
import com.cims.ue.dispatch.ui.AppScreen
import com.cims.ue.dispatch.ui.CallPane
import com.cims.ue.dispatch.ui.PttPane
import com.cims.ue.dispatch.ui.entryPane
import com.cims.ue.dispatch.ui.paneIndexOf
import org.junit.Assert.assertEquals
import org.junit.Test

class AppPagesTest {

    /**
     * 한 걸음 — 지금 장에서 [forward] 쪽으로 민 결과.
     *
     * 화면의 두 pager 가 하는 일을 그대로 흉내 낸다: 안쪽에 갈 곳이 있으면 면만 옮기고, 없으면
     * 옆 메뉴로 넘어가면서 [entryPane] 이 정한 면에 선다. 끝에서 더 밀면 제자리.
     */
    private fun step(p: AppPage, forward: Boolean): AppPage {
        val next = p.pane + if (forward) 1 else -1
        if (next in 0 until p.screen.paneCount) return AppPage(p.screen, next)
        val mi = p.screen.ordinal + if (forward) 1 else -1
        val menu = AppScreen.entries.getOrNull(mi) ?: return p
        return AppPage(menu, entryPane(menu, from = p.screen))
    }

    /** 첫 장에서 계속 밀면 `APP_PAGES` 를 순서대로 지난다 — 겹친 pager 둘의 결과가 한 줄이다. */
    @Test fun 계속_밀면_차례대로_지난다() {
        val walked = generateSequence(APP_PAGES.first()) { step(it, forward = true) }
            .take(APP_PAGES.size).toList()
        assertEquals(APP_PAGES, walked)
    }

    /** 이력 1 + 무전 3 + 통화 4 + 더보기 1 = 9장. */
    @Test fun 아홉_장이_한_줄이다() {
        assertEquals(9, APP_PAGES.size)
        assertEquals(
            listOf("이력", "무전", "무전", "무전", "통화", "통화", "통화", "통화", "더보기"),
            APP_PAGES.map { it.screen.label })
    }

    /** 메뉴 안의 면 순서는 탭 순서와 같다 — 두 곳에 적으면 한쪽만 고치게 된다. */
    @Test fun 면_순서는_탭_순서다() {
        assertEquals(PttPane.entries.toList(), APP_PAGES.mapNotNull { it.pttPane })
        assertEquals(CallPane.entries.toList(), APP_PAGES.mapNotNull { it.callPane })
    }

    /** 이음매 — 통화›«통화» 에서 뒤로 밀면 무전›«이벤트»(사용자가 말한 그 자리). */
    @Test fun 통화의_첫_면에서_뒤로_밀면_무전의_끝_면() {
        assertEquals(AppPage(AppScreen.PTT, PttPane.EVENTS.ordinal),
            step(AppPage(AppScreen.CALLS, CallPane.CALLS.ordinal), forward = false))
    }

    /** 이음매 — 통화›«통화내역» 에서 앞으로 밀면 [더보기]. */
    @Test fun 통화의_끝_면에서_앞으로_밀면_더보기() {
        assertEquals(AppPage(AppScreen.MORE),
            step(AppPage(AppScreen.CALLS, CallPane.LOG.ordinal), forward = true))
    }

    /** **왕복이 된다** — 밀어서 넘어간 뒤 되밀면 원래 자리. 기억한 면에 세우면 이게 깨진다. */
    @Test fun 이음매를_왕복할_수_있다() {
        APP_PAGES.forEach { p ->
            val fwd = step(p, forward = true)
            if (fwd != p) assertEquals(p, step(fwd, forward = false))
            val back = step(p, forward = false)
            if (back != p) assertEquals(p, step(back, forward = true))
        }
    }

    /** 양 끝에서 더 밀면 제자리 — 넘어갈 곳이 없다. */
    @Test fun 양_끝에서는_제자리다() {
        assertEquals(APP_PAGES.first(), step(APP_PAGES.first(), forward = false))
        assertEquals(APP_PAGES.last(), step(APP_PAGES.last(), forward = true))
    }

    /** 면 번호는 보고 있는 메뉴에서만 읽는다 — 면이 없는 메뉴는 늘 0. */
    @Test fun 면없는_메뉴는_언제나_0() {
        assertEquals(0, paneIndexOf(AppScreen.HISTORY, PttPane.EVENTS, CallPane.LOG))
        assertEquals(0, paneIndexOf(AppScreen.MORE, PttPane.EVENTS, CallPane.LOG))
        assertEquals(PttPane.EVENTS.ordinal, paneIndexOf(AppScreen.PTT, PttPane.EVENTS, CallPane.LOG))
        assertEquals(CallPane.LOG.ordinal, paneIndexOf(AppScreen.CALLS, PttPane.EVENTS, CallPane.LOG))
    }
}
