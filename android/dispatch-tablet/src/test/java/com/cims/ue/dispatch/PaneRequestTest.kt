// 면 이동 요청의 수명(`PaneRequest`) — JVM, 기기 불필요 (android_dispatch_tablet.md §6.3)
//
// 노리는 것은 **요청이 영영 남아 좌표 알림을 막는** 결함이다. 탭을 누른 직후 손가락으로 끌면 이동 애니메이션이 끊기는데,
// 그때 요청을 내려놓지 않으면 그 뒤로 면을 넘겨도 VM 이 모른다(뒤로가기·메뉴 복귀가 옛 면으로 간다).
package com.cims.ue.dispatch

import com.cims.ue.dispatch.ui.PaneRequest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PaneRequestTest {

    @Test fun `장이 설 때의 첫 값은 요청이 아니다`() {
        // 밀어서 막 들어온 장이 옛 기억값으로 튕겨 나가지 않게.
        val r = PaneRequest()
        r.onWant(0)
        assertNull(r.pending)
        assertTrue(r.canReport)
    }

    @Test fun `그 뒤 VM 이 바꾼 면은 요청이고 든 동안은 알리지 않는다`() {
        val r = PaneRequest()
        r.onWant(0)
        r.onWant(2)
        assertEquals(2, r.pending)
        assertFalse(r.canReport)
    }

    @Test fun `닿거나 손가락이 끊으면 내려놓는다`() {
        val r = PaneRequest()
        r.onWant(0)
        r.onWant(2)
        r.finish(2)                          // goTo 가 정상 반환했든 CancellationException 으로 끝났든 finally 가 부른다
        assertNull(r.pending)
        assertTrue("끊긴 뒤에는 손가락이 멈춘 면을 알린다", r.canReport)
    }

    @Test fun `새 요청이 대신했으면 옛 끝맺음은 새 요청을 건드리지 않는다`() {
        val r = PaneRequest()
        r.onWant(0)
        r.onWant(2)
        r.onWant(1)                          // 가는 중에 다른 탭 — 효과가 다시 시작되며 옛 이동은 취소된다
        r.finish(2)                          // 취소된 옛 이동의 finally
        assertEquals(1, r.pending)
        r.finish(1)
        assertNull(r.pending)
    }
}
