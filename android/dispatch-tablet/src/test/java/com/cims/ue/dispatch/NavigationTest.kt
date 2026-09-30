// 이동 규칙 — 레일·탭·모드·사이드 패널·뒤로가기 (android_dispatch_tablet.md §6.3)
//
// 지키는 불변 하나: **`onBack()` 이 값을 돌려주면 화면이 눈에 보이게 바뀐다.** 어긋나면 뒤로가기를 먹고도 아무 일이 없어
// 관제사는 앱이 멈춘 줄 안다 — 가로채기 판정이 같은 함수를 쓰는 이유다. 패널 규칙(시안): 같은 대상 = 닫기, 다른 대상 = 바꾸기,
// 고정하면 탭을 옮겨도 남고 아니면 닫힌다, 패널 안에서 들어간 것만 ← 로 돌아간다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.ui.AppScreen
import com.cims.ue.dispatch.ui.CallPane
import com.cims.ue.dispatch.ui.DispatchMode
import com.cims.ue.dispatch.ui.MoreItem
import com.cims.ue.dispatch.ui.NavState
import com.cims.ue.dispatch.ui.PttPane
import com.cims.ue.dispatch.ui.SidePanel
import com.cims.ue.dispatch.ui.closePanel
import com.cims.ue.dispatch.ui.onBack
import com.cims.ue.dispatch.ui.onNav
import com.cims.ue.dispatch.ui.openChannel
import com.cims.ue.dispatch.ui.pageOf
import com.cims.ue.dispatch.ui.showPanel
import com.cims.ue.dispatch.ui.toMode
import com.cims.ue.dispatch.ui.togglePanel
import com.cims.ue.dispatch.ui.togglePin
import com.cims.ue.dispatch.ui.toPage
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class NavigationTest {

    private val home = NavState()
    private val ch = SidePanel.Channel("g1")

    // ── 레일 ──────────────────────────────────────────────────────────────

    @Test fun `첫 화면은 관제 › 무전 › 채널이다`() {
        assertEquals(AppScreen.DISPATCH, home.screen)
        assertEquals(DispatchMode.PTT, home.mode)
        assertEquals(PttPane.CHANNELS, home.pttPane)
        assertNull(home.panel)
    }

    @Test fun `레일은 멀어도 한 번에 간다`() {
        assertEquals(AppScreen.MORE, home.onNav(AppScreen.MORE).screen)
        assertEquals(AppScreen.DISPATCH, home.onNav(AppScreen.MORE).onNav(AppScreen.DISPATCH).screen)
    }

    @Test fun `레일을 옮기면 고정하지 않은 패널은 닫히고 고정한 패널은 남는다`() {
        assertNull(home.togglePanel(ch).onNav(AppScreen.HISTORY).panel)
        val pinned = home.togglePanel(ch).togglePin().onNav(AppScreen.HISTORY)
        assertEquals(ch, pinned.panel)
        assertEquals("돌아오면 그대로", ch, pinned.onNav(AppScreen.DISPATCH).panel)
    }

    /** 같은 항목 재탭 = **그 메뉴의** 안쪽 닫기 — 관제는 패널, 더보기는 안쪽 화면. */
    @Test fun `같은 레일 재탭은 제 메뉴의 안쪽만 닫는다`() {
        assertNull(home.togglePanel(ch).onNav(AppScreen.DISPATCH).panel)
        val more = NavState(AppScreen.MORE, more = MoreItem.ADMIN)
        assertNull(more.onNav(AppScreen.MORE).more)
        assertEquals(home, home.onNav(AppScreen.DISPATCH))
    }

    // ── 모드·면 ───────────────────────────────────────────────────────────

    @Test fun `모드마다 보던 면을 기억한다`() {
        val s = home.toPage(pageOf(PttPane.EVENTS)).toMode(DispatchMode.CALL).toPage(pageOf(CallPane.LOG))
            .toMode(DispatchMode.PTT)
        assertEquals(PttPane.EVENTS, s.pttPane)
        assertEquals(CallPane.LOG, s.toMode(DispatchMode.CALL).callPane)
    }

    @Test fun `탭을 옮기면 고정하지 않은 패널은 닫힌다 — 고정은 남는다`() {
        assertNull(home.togglePanel(ch).toPage(pageOf(PttPane.MESSAGES)).panel)
        assertEquals(ch, home.togglePanel(ch).togglePin().toPage(pageOf(PttPane.MESSAGES)).panel)
        assertEquals(ch, home.togglePanel(ch).togglePin().toMode(DispatchMode.CALL).panel)
    }

    @Test fun `같은 면을 다시 누르면 패널은 그대로다`() {
        assertEquals(ch, home.togglePanel(ch).toPage(pageOf(PttPane.CHANNELS)).panel)
    }

    // ── 사이드 패널 ────────────────────────────────────────────────────────

    @Test fun `같은 대상은 닫고 다른 대상은 바꾼다`() {
        val open = home.togglePanel(ch)
        assertEquals(ch, open.panel)
        assertNull(open.togglePanel(ch).panel)
        assertEquals(SidePanel.AddChannel, open.togglePanel(SidePanel.AddChannel).panel)
    }

    @Test fun `패널이 닫히면 고정도 풀린다`() {
        val pinned = home.togglePanel(ch).togglePin()
        assertTrue(pinned.pinned)
        assertFalse(pinned.togglePanel(ch).pinned)
        assertFalse(pinned.closePanel().pinned)
        assertFalse("패널이 없으면 고정할 것도 없다", home.togglePin().pinned)
    }

    /** 찾아가서 연다 — 긴급 배너·검색·[PTT 그룹] 은 어디서 눌러도 무전 › 채널 면에 그 채널 상세. */
    @Test fun `채널 찾아가기는 어디서든 무전 채널 면에 연다`() {
        listOf(
            home.toPage(pageOf(PttPane.MESSAGES)),
            home.toPage(pageOf(CallPane.LOG)),
            NavState(AppScreen.MORE, more = MoreItem.PTT_GROUPS),
            NavState(AppScreen.HISTORY),
        ).forEach { start ->
            val s = start.openChannel("g1")
            assertEquals(AppScreen.DISPATCH, s.screen)
            assertEquals(DispatchMode.PTT, s.mode)
            assertEquals(PttPane.CHANNELS, s.pttPane)
            assertEquals(ch, s.panel)
            assertEquals("다른 모드가 기억한 면은 건드리지 않는다", start.callPane, s.callPane)
        }
    }

    /** 메시지 [채널 정보] — 보던 대화 옆에 선다(면을 옮기지 않는다). */
    @Test fun `채널 정보는 면을 옮기지 않는다`() {
        val s = home.toPage(pageOf(PttPane.MESSAGES)).showPanel(ch)
        assertEquals(PttPane.MESSAGES, s.pttPane)
        assertEquals(ch, s.panel)
        assertEquals("다시 눌러도 닫지 않는다", ch, s.showPanel(ch).panel)
    }

    // ── 뒤로가기 ───────────────────────────────────────────────────────────

    @Test fun `패널 안의 한 겹부터 되돌린다`() {
        var s = home.togglePanel(SidePanel.AddChannel).showPanel(SidePanel.NewGroup)
        s = s.onBack()!!; assertEquals(SidePanel.AddChannel, s.panel)
        s = s.onBack()!!; assertNull(s.panel)
        assertNull(s.onBack())
    }

    @Test fun `한 겹씩 첫 화면까지 되돌린다`() {
        var s = home.toPage(pageOf(PttPane.EVENTS)).toMode(DispatchMode.CALL).toPage(pageOf(CallPane.LOG))
        s = s.onBack()!!; assertEquals(CallPane.CALLS, s.callPane); assertEquals(DispatchMode.CALL, s.mode)
        s = s.onBack()!!; assertEquals(DispatchMode.PTT, s.mode)
        s = s.onBack()!!; assertEquals(PttPane.CHANNELS, s.pttPane)
        assertNull(s.onBack())
    }

    /**
     * **핵심 회귀** — 보고 있지 않은 메뉴의 것을 닫아 «보이지 않는 한 걸음» 을 만들면 안 된다. [이력] 에서 뒤로가기를 눌렀는데
     * 고정해 둔 관제 패널이 조용히 닫히면 화면은 그대로다.
     */
    @Test fun `되돌릴 때마다 화면이 바뀐다`() {
        val starts = listOf(
            home.togglePanel(ch).togglePin().onNav(AppScreen.HISTORY),
            NavState(AppScreen.MORE, more = MoreItem.ADMIN, panel = ch, pinned = true),
            home.toPage(pageOf(CallPane.MESSAGES)).togglePanel(SidePanel.Event(3)).togglePin(),
            home.togglePanel(SidePanel.AddChannel).showPanel(SidePanel.NewGroup),
            NavState(AppScreen.HISTORY, callPane = CallPane.LOG, pttPane = PttPane.EVENTS))
        starts.forEach { start ->
            var s = start
            var guard = 0
            while (guard++ < 16) {
                val next = s.onBack() ?: break
                assertNotEquals("onBack() 이 값을 줬는데 좌표가 그대로다: $s", s, next)
                s = next
            }
            assertEquals("끝까지 되돌리면 첫 화면이어야 한다", AppScreen.DISPATCH, s.screen)
            assertNull("첫 화면에서는 가로채지 않는다", s.onBack())
        }
    }

    @Test fun `보이지 않는 패널은 뒤로가기로 닫지 않는다`() {
        val s = home.togglePanel(ch).togglePin().onNav(AppScreen.HISTORY)
        val back = s.onBack()!!
        assertEquals(AppScreen.DISPATCH, back.screen)
        assertEquals("관제로 돌아가면 고정해 둔 패널이 그대로 보인다", ch, back.panel)
    }
}
