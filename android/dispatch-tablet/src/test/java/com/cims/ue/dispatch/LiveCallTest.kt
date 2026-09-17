// ⑥ 진행 중 행의 결합 규칙 (dispatch_desktop_ui.md §4.4, dispatch_center.md §5.3)
//
// 감시 대상 둘이 서로 통화하면 RFC 4235 dialog 가 **leg 마다 하나씩** 온다(Call-ID 가 다르다).
// 그건 통화 하나이므로 한 행으로 묶어야 한다 — 두 행으로 두면 같은 통화를 두 번 감청하게 된다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.DialogRow
import com.cims.ue.dispatch.ui.call.combineDialogs
import com.cims.ue.sdk.DialogInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LiveCallTest {

    private fun row(watched: String, remote: String, state: String, id: String,
                    startedAtMs: Long = 0L, direction: String = "initiator"): DialogRow =
        DialogRow(
            watched = watched, id = id,
            info = DialogInfo(
                accountId = 0, watched = watched, id = id, callId = "call-$id",
                localTag = "l$id", remoteTag = "r$id",
                direction = direction, state = state, remoteIdentity = remote, full = true),
            startedAtMs = startedAtMs)

    @Test fun `서로를 가리키는 두 leg 은 한 행`() {
        val g = combineDialogs(listOf(
            row("tel:1001", "tel:1002", "confirmed", "a"),
            row("tel:1002", "tel:1001", "confirmed", "b", direction = "recipient")))
        assertEquals(1, g.size)
        assertEquals(2, g[0].size)
    }

    @Test fun `한쪽만 감시 대상이면 leg 하나로 남는다`() {
        val g = combineDialogs(listOf(row("tel:1001", "tel:+821099998888", "confirmed", "a")))
        assertEquals(1, g.size)
        assertEquals(1, g[0].size)
    }

    @Test fun `서로 다른 통화는 묶이지 않는다`() {
        val g = combineDialogs(listOf(
            row("tel:1001", "tel:1002", "confirmed", "a"),
            row("tel:1003", "tel:1004", "confirmed", "b")))
        assertEquals(2, g.size)
        assertTrue(g.all { it.size == 1 })
    }

    @Test fun `종료된 leg 은 빠진다`() {
        val g = combineDialogs(listOf(
            row("tel:1001", "tel:1002", "terminated", "a"),
            row("tel:1003", "tel:1004", "confirmed", "b")))
        assertEquals(1, g.size)
        assertEquals("tel:1003", g[0][0].watched)
    }

    @Test fun `URI 표기가 달라도 같은 사람이면 묶인다`() {
        val g = combineDialogs(listOf(
            row("sip:1001@cims.local", "tel:1002", "confirmed", "a"),
            row("tel:1002;user=phone", "sip:1001@cims.local", "confirmed", "b",
                direction = "recipient")))
        assertEquals(1, g.size)
        assertEquals(2, g[0].size)
    }

    @Test fun `링잉이 위로, 그다음 시작 역순`() {
        val g = combineDialogs(listOf(
            row("tel:1001", "tel:1002", "confirmed", "a", startedAtMs = 300),
            row("tel:1003", "tel:1004", "early", "b", startedAtMs = 100),
            row("tel:1005", "tel:1006", "confirmed", "c", startedAtMs = 500)))
        assertEquals(listOf("tel:1003", "tel:1005", "tel:1001"), g.map { it[0].watched })
    }

    @Test fun `한 leg 이 두 행에 겹쳐 들어가지 않는다`() {
        val rows = listOf(
            row("tel:1001", "tel:1002", "confirmed", "a"),
            row("tel:1002", "tel:1001", "confirmed", "b", direction = "recipient"),
            row("tel:1002", "tel:1001", "confirmed", "c", direction = "recipient"))   // 중복 통지
        val g = combineDialogs(rows)
        val keys = g.flatten().map { it.key }
        assertEquals(keys.size, keys.toSet().size)
        assertEquals(3, keys.size)
    }

    // ── 결합을 **하지 않아야** 하는 경우 ─────────────────────────────────────
    //
    // 번호 상호 일치만으로 묶으면 같은 두 사람의 서로 다른 통화가 섞인다. 그 행은 talking=true 가
    // 되어 새 착신의 [지정 픽업]이 사라지고, 한쪽의 감청 상태가 다른 호로 번진다.

    @Test fun `같은 두 사람의 다른 통화는 묶지 않는다 — 진행 단계가 다르다`() {
        val g = combineDialogs(listOf(
            row("tel:1001", "tel:1002", "confirmed", "a"),                       // 통화 중
            row("tel:1002", "tel:1001", "early", "b", direction = "recipient"))) // 두 번째로 걸려온 호
        assertEquals(2, g.size)
        assertTrue(g.all { it.size == 1 })
    }

    @Test fun `방향이 같으면 묶지 않는다 — 한 통화의 두 leg 이 아니다`() {
        val g = combineDialogs(listOf(
            row("tel:1001", "tel:1002", "confirmed", "a", direction = "initiator"),
            row("tel:1002", "tel:1001", "confirmed", "b", direction = "initiator")))
        assertEquals(2, g.size)
    }

    @Test fun `관측 시각이 멀면 묶지 않는다`() {
        val g = combineDialogs(listOf(
            row("tel:1001", "tel:1002", "confirmed", "a", startedAtMs = 0),
            row("tel:1002", "tel:1001", "confirmed", "b", direction = "recipient",
                startedAtMs = 60_000)))
        assertEquals(2, g.size)
    }

    @Test fun `짝 후보가 둘 이상이면 추측하지 않고 홀로 둔다`() {
        val g = combineDialogs(listOf(
            row("tel:1001", "tel:1002", "confirmed", "a"),
            row("tel:1002", "tel:1001", "confirmed", "b", direction = "recipient"),
            row("tel:1002", "tel:1001", "confirmed", "c", direction = "recipient")))
        // 후보가 둘(b·c)이라 a 는 홀로 남고, b·c 도 서로 방향이 같아 묶이지 않는다
        assertEquals(3, g.size)
    }
}

/**
 * 구독 성립 신호가 «진행 중 통화» 로 둔갑하지 않아야 한다.
 *
 * 코어는 dialog 가 하나도 없는 full 스냅샷을 **id·state 가 빈** `DialogInfo` 로 낸다
 * (`engine.cpp` — "초기 full 스냅샷에 dialog 없음"). 이걸 행으로 만들면 상태 전이가 영영 오지 않아
 * ⑥ 에 «연결 중» 이 무한히 남는다 — 대표번호 AoR 이 실제로 그렇게 보였다.
 */
class GhostDialogTest {

    private fun info(watched: String, id: String, state: String) = DialogInfo(
        accountId = 0, watched = watched, id = id, callId = if (id.isEmpty()) "" else "c-$id",
        localTag = "", remoteTag = "", direction = "initiator", state = state,
        remoteIdentity = "", full = true)

    @Test fun `상태가 빈 행은 세우지 않는다`() {
        val ghost = DialogRow(watched = "tel:+82210001000", id = "",
            info = info("tel:+82210001000", "", ""))
        assertFalse(ghost.isLive)
        assertTrue(combineDialogs(listOf(ghost)).isEmpty())
    }

    @Test fun `id 가 비어도 state 가 있으면 진짜 통화다`() {
        // RFC 4235 상 id 는 필수지만 서버가 빠뜨릴 수 있다. state 로만 판정해야 멀쩡한 통화를 안 지운다.
        val r = DialogRow(watched = "tel:1001", id = "", info = info("tel:1001", "", "confirmed"))
        assertTrue(r.isLive)
        assertEquals(1, combineDialogs(listOf(r)).size)
    }

    @Test fun `모르는 상태도 세우지 않는다`() {
        val odd = DialogRow(watched = "tel:1001", id = "x", info = info("tel:1001", "x", "weird"))
        assertFalse(odd.isLive)
        assertTrue(combineDialogs(listOf(odd)).isEmpty())
    }

    @Test fun `아는 상태는 그대로 선다`() {
        listOf("trying", "proceeding", "early", "confirmed").forEach { st ->
            val r = DialogRow(watched = "tel:1001", id = "x", info = info("tel:1001", "x", st))
            assertTrue("$st 는 진행 중이어야 한다", r.isLive)
        }
        val gone = DialogRow(watched = "tel:1001", id = "x", info = info("tel:1001", "x", "terminated"))
        assertFalse(gone.isLive)
    }
}
