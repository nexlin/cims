// ① 카드 3줄 로스터 미리보기 계약 시험 — JVM, 기기 불필요 (android_dispatch_tablet.md §9)
//
// 노리는 것 하나 — **발언자가 `+n` 에 접혀 사라지는 것.** 그러면 이 줄의 존재 이유가 없어진다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.ui.ptt.rosterPreview
import com.cims.ue.sdk.RosterEntry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RosterPreviewTest {

    private fun r(n: String, status: String = "connected") = RosterEntry("sip:$n@d", status)

    @Test fun `접속한 사람만 담는다`() {
        val p = rosterPreview(
            listOf(r("1001"), r("1002", "disconnected"), r("1003", "on-hold")),
            speaker = "", me = "")
        assertEquals(1, p.chips.size)
        assertEquals("1001", p.chips[0].number)
    }

    // 상한을 넘겨도 발언자는 반드시 보인다 — 서버 순서상 맨 뒤에 있어도 앞으로 끌어온다.
    @Test fun `발언자는 접히지 않는다`() {
        val many = (1..10).map { r("100$it") }
        val p = rosterPreview(many, speaker = "sip:10010@d", me = "", max = 3)
        assertEquals(3, p.chips.size)
        assertTrue(p.chips.first().speaking)
        assertEquals("10010", p.chips.first().number)
        assertEquals(7, p.more)
    }

    @Test fun `나는 발언자 다음으로 앞에 선다`() {
        val many = (1..6).map { r("100$it") }
        val p = rosterPreview(many, speaker = "sip:1006@d", me = "sip:1005@d", max = 2)
        assertEquals("1006", p.chips[0].number)
        assertTrue(p.chips[0].speaking)
        assertEquals("1005", p.chips[1].number)
        assertTrue(p.chips[1].isMe)
    }

    // 전화번호부의 `010…` 과 로스터의 `+8210…` 이 같은 사람이어야 «나»·«발언 중» 이 붙는다.
    @Test fun `번호는 정규형으로 맞춘다`() {
        val p = rosterPreview(listOf(RosterEntry("tel:+821011112222", "connected")),
                              speaker = "01011112222", me = "01011112222")
        assertTrue(p.chips[0].speaking)
        assertTrue(p.chips[0].isMe)
    }

    @Test fun `이름이 있으면 이름을 보인다`() {
        val p = rosterPreview(listOf(r("1001")), speaker = "", me = "",
                              nameOf = { if (it == "1001") "이순경" else "" })
        assertEquals("이순경", p.chips[0].label)
    }

    @Test fun `이름이 없으면 번호를 보인다`() {
        assertEquals("1001", rosterPreview(listOf(r("1001")), "", "").chips[0].label)
    }

    @Test fun `빈 로스터는 빈 미리보기다`() {
        val p = rosterPreview(emptyList(), "", "")
        assertTrue(p.chips.isEmpty())
        assertEquals(0, p.more)
    }

    @Test fun `발언자가 없으면 아무도 발언 표시가 붙지 않는다`() {
        val p = rosterPreview(listOf(r("1001"), r("1002")), speaker = "", me = "")
        assertFalse(p.chips.any { it.speaking })
    }

    // 같은 등급끼리는 서버 순서를 지킨다 — 매 갱신마다 칩이 뒤섞이면 읽을 수 없다.
    @Test fun `같은 등급은 서버 순서를 지킨다`() {
        val p = rosterPreview(listOf(r("1003"), r("1001"), r("1002")), speaker = "", me = "")
        assertEquals(listOf("1003", "1001", "1002"), p.chips.map { it.number })
    }
}
