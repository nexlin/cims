// 이어폰 고르기·선호 이어폰 자동 복귀 — JVM, 기기 불필요 (android_dispatch_tablet.md §8)
//
// 노리는 것은 **소리가 엉뚱한 곳으로 나가는** 결함이다 — 이어폰이 둘일 때 고른 것이 아니라 첫 것으로 가는 것, 스피커를
// 골라 둔 사람의 경로를 이어폰이 다시 붙었다고 빼앗는 것, 자동 복귀를 꺼 뒀는데 되돌리는 것.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.Settings
import com.cims.ue.dispatch.session.pickHeadset
import com.cims.ue.dispatch.session.returnToPreferred
import com.cims.ue.sdk.platform.Headset
import com.cims.ue.sdk.platform.Route
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AudioPrefsTest {

    private val bt1 = Headset(10, "Jabra", wireless = true)
    private val bt2 = Headset(11, "Plantronics", wireless = true)
    private val wired = Headset(12, "USB-C", wireless = false)

    @Test fun `여럿이면 선호 이어폰을 고른다 — 없으면 그 종류의 첫 것`() {
        assertEquals(11, pickHeadset(Route.BLUETOOTH, listOf(bt1, bt2, wired), "Plantronics")?.id)
        assertEquals(10, pickHeadset(Route.BLUETOOTH, listOf(bt1, bt2), "Sony")?.id)
        assertEquals(12, pickHeadset(Route.HEADSET, listOf(bt1, wired), "Jabra")?.id)   // 종류가 먼저다
    }

    @Test fun `스피커·수화부 경로에는 이어폰을 고르지 않는다`() {
        assertNull(pickHeadset(Route.SPEAKER, listOf(bt1), "Jabra"))
        assertNull(pickHeadset(Route.EARPIECE, listOf(bt1), "Jabra"))
        assertNull("그 종류가 없으면 경로 요청만", pickHeadset(Route.HEADSET, listOf(bt1), ""))
    }

    @Test fun `선호 이어폰이 다시 붙으면 되돌린다 — 이어폰 경로일 때만`() {
        val s = Settings(audioRoute = Route.BLUETOOTH, preferredHeadset = "Jabra")
        assertTrue(returnToPreferred(s, setOf("Jabra")))
        assertFalse("다른 이어폰이 붙었다", returnToPreferred(s, setOf("Sony")))
        assertFalse("스피커를 고른 뒤에는 빼앗지 않는다", returnToPreferred(s.copy(audioRoute = Route.SPEAKER), setOf("Jabra")))
        assertFalse("자동 복귀를 껐다", returnToPreferred(s.copy(autoReturnHeadset = false), setOf("Jabra")))
        assertFalse("선호가 없다", returnToPreferred(s.copy(preferredHeadset = ""), setOf("Jabra")))
    }
}
