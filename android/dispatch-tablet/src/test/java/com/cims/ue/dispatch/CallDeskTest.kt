// [일반통화] 탭 계약 시험 — JVM, 기기 불필요 (android_dispatch_tablet.md §9)
//
// 노리는 것은 **관제사가 남의 통화를 잘못 다루게 되는** 자리들이다 —
// 내 회선과 감시 회선의 구분, 픽업·청취 자격, 대표번호 포크의 묶음.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.CallLogKind
import com.cims.ue.dispatch.session.CallLogRow
import com.cims.ue.dispatch.session.DialogRow
import com.cims.ue.dispatch.ui.call.DESK_ALL
import com.cims.ue.dispatch.ui.call.MemberChip
import com.cims.ue.dispatch.ui.call.keepInDesk
import com.cims.ue.dispatch.ui.call.userPartOf
import com.cims.ue.sdk.DialogInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CallDeskTest {

    private fun dlg(
        watched: String = "sip:1001@d", state: String = "confirmed",
        direction: String = "recipient", remote: String = "sip:02123@d", id: String = "d1",
    ) = DialogRow(watched, id, DialogInfo(0, watched, id, "c1", "lt", "rt", direction, state, remote, true))

    // ── dialog 상태 판정 (RFC 4235) ──
    @Test fun `early 계열이 링잉이다`() {
        listOf("early", "proceeding", "trying").forEach {
            assertTrue(it, dlg(state = it).isEarly)
            assertFalse(it, dlg(state = it).isConfirmed)
        }
        assertTrue(dlg(state = "confirmed").isConfirmed)
        assertTrue(dlg(state = "terminated").isTerminated)
    }

    @Test fun `착신 leg 만 픽업 대상이다`() {
        // RFC 4235 direction — 내가 건 호(initiator)를 당겨받을 수는 없다.
        assertTrue(dlg(direction = "recipient").isIncomingLeg)
        assertFalse(dlg(direction = "initiator").isIncomingLeg)
    }

    @Test fun `상태가 바뀌면 경과가 초기화된다`() {
        val d0 = dlg(state = "early")
        Thread.sleep(5)
        val d1 = d0.apply(d0.info.copy(state = "confirmed"))
        assertTrue("상태 전이 시각이 갱신돼야 한다", d1.stateSinceMs >= d0.stateSinceMs)
        assertTrue(d1.wasConfirmed)
    }

    @Test fun `한 번 확립되면 그 사실이 남는다`() {
        // 대표번호 «부재» 판정의 근거 — 전원 무응답이었는지 누가 받았는지.
        val d = dlg(state = "early").let { it.apply(it.info.copy(state = "confirmed")) }
        val ended = d.apply(d.info.copy(state = "terminated"))
        assertTrue(ended.wasConfirmed)
        val never = dlg(state = "early").let { it.apply(it.info.copy(state = "terminated")) }
        assertFalse(never.wasConfirmed)
    }

    // ── 그룹원 칩 자격 ──
    private fun chip(isMe: Boolean = false, state: String? = null, monitoring: Boolean = false) =
        MemberChip("sip:1001@d", "1001", "최순경", isMe,
                   state?.let { dlg(state = it) }, monitoring)

    @Test fun `내 회선은 당겨받지 않는다`() {
        // 내 링잉은 «응답» 이지 «당겨받기» 가 아니다.
        assertFalse(chip(isMe = true, state = "early").canPickup)
        assertTrue(chip(isMe = false, state = "early").canPickup)
    }

    @Test fun `통화 중인 회선만 청취할 수 있다`() {
        assertFalse(chip(state = "early").canMonitor)          // 링잉은 아직 통화가 아니다
        assertTrue(chip(state = "confirmed").canMonitor)
        assertFalse(chip(isMe = true, state = "confirmed").canMonitor)   // 내 통화는 청취 대상이 아니다
        assertFalse(chip(state = "confirmed", monitoring = true).canMonitor)  // 이미 듣고 있다
    }

    @Test fun `대기 중 회선은 아무 조작도 없다`() {
        val c = chip()
        assertTrue(c.idle)
        assertFalse(c.canPickup)
        assertFalse(c.canMonitor)
        assertEquals("대기", c.stateText)
    }

    @Test fun `회선 상태 문구가 셋이다`() {
        assertEquals("링잉", chip(state = "early").stateText)
        assertEquals("통화", chip(state = "confirmed").stateText)
        assertEquals("대기", chip().stateText)
    }

    // ── URI 번호부 추출 ──
    @Test fun `URI 에서 번호부만 뽑는다`() {
        assertEquals("1001", userPartOf("sip:1001@cims.local"))
        assertEquals("+821012345678", userPartOf("tel:+821012345678"))
        assertEquals("1001", userPartOf("sip:1001@d;user=phone"))
        assertEquals("1001", userPartOf("1001"))
    }
}

/**
 * ⑥ 통화 내역 한 줄이 드는 축 — 시작 · 응답 · 종료 · 통화시간 (dispatch_desktop_ui.md §4.4·§4.6).
 *
 * **통화시간과 울린 시간을 섞지 않는다.** 못 받은 호의 «통화 3분» 은 거짓이다.
 */
class CallLogRowTest {

    private val t0 = 1_700_000_000_000L      // 시작
    private val t1 = t0 + 12_000L            // 응답 (12초 울림)
    private val t2 = t1 + 93_000L            // 종료 (1분 33초 통화)

    @Test fun `응답한 호 — 통화시간은 응답부터, 울림은 시작부터 응답까지`() {
        val r = com.cims.ue.dispatch.session.CallLogRow(
            atMs = t2, peer = "이순경", number = "1002", text = "통화 종료",
            kind = com.cims.ue.dispatch.session.CallLogKind.ANSWERED,
            startedAtMs = t0, answeredAtMs = t1)
        assertTrue(r.answered)
        assertEquals(93, r.durationSec)
        assertEquals(12, r.ringSec)
        assertEquals(t2, r.endedAtMs)
    }

    @Test fun `못 받은 호 — 통화시간 0, 울림은 시작부터 종료까지`() {
        val r = com.cims.ue.dispatch.session.CallLogRow(
            atMs = t0 + 30_000L, peer = "김순경", number = "1001", text = "부재",
            kind = com.cims.ue.dispatch.session.CallLogKind.MISSED,
            startedAtMs = t0, answeredAtMs = null)
        assertFalse(r.answered)
        assertEquals(0, r.durationSec)
        assertEquals(30, r.ringSec)
    }

    @Test fun `이름과 번호를 같이 — 같거나 비면 하나만`() {
        fun row(peer: String, number: String) = com.cims.ue.dispatch.session.CallLogRow(
            atMs = t0, peer = peer, number = number, text = "",
            kind = com.cims.ue.dispatch.session.CallLogKind.OUTGOING)
        assertEquals("이순경 1002", row("이순경", "1002").label)
        assertEquals("1002", row("1002", "1002").label)      // 이름이 안 잡히면 번호가 peer 다
        assertEquals("1002", row("", "1002").label)
        assertEquals("이순경", row("이순경", "").label)         // 번호를 모르면 이름만
    }

    @Test fun `시각이 뒤집혀도 음수를 내지 않는다`() {
        // 종료가 응답보다 빠르다(시계 보정·이벤트 순서 뒤집힘) → 통화시간 0
        val ended = com.cims.ue.dispatch.session.CallLogRow(
            atMs = t0, peer = "x", number = "1", text = "",
            kind = com.cims.ue.dispatch.session.CallLogKind.ANSWERED,
            startedAtMs = t0 - 5_000L, answeredAtMs = t0 + 9_000L)
        assertEquals(0, ended.durationSec)

        // 응답이 시작보다 빠르다 → 울린 시간 0
        val rang = com.cims.ue.dispatch.session.CallLogRow(
            atMs = t2, peer = "x", number = "1", text = "",
            kind = com.cims.ue.dispatch.session.CallLogKind.ANSWERED,
            startedAtMs = t0 + 9_000L, answeredAtMs = t0)
        assertEquals(0, rang.ringSec)
    }

    // ── 오늘 데스크 칩 → ⑥ 필터 (데스크톱 CallActivityViewModel.Refilter 와 같은 규칙) ──
    private fun row(kind: CallLogKind) = CallLogRow(atMs = 0, peer = "p", text = "t", kind = kind)

    @Test fun `전체는 모든 종류를 통과시킨다`() {
        CallLogKind.entries.forEach { assertTrue(it.name, keepInDesk(row(it), DESK_ALL)) }
    }

    @Test fun `각 필터는 그 종류만 남긴다`() {
        mapOf("missed" to CallLogKind.MISSED, "outgoing" to CallLogKind.OUTGOING,
              "transfer" to CallLogKind.TRANSFER, "monitor" to CallLogKind.MONITOR)
            .forEach { (f, keep) ->
                CallLogKind.entries.forEach { k ->
                    assertEquals("$f/$k", k == keep, keepInDesk(row(k), f))
                }
            }
    }

    // «응대» 칩은 필터가 아니라 **해제**다 — 데스크톱도 그 칩에 all 을 건다(CallDeskPanel.xaml 툴팁 "⑥ 전체").
    //   여기서 ANSWERED 만 남기도록 바꾸면 당겨받기(PICKUP)가 사라져 집계와 목록이 어긋난다.
    @Test fun `모르는 필터 값은 전체로 본다`() {
        assertTrue(keepInDesk(row(CallLogKind.PICKUP), "answered"))
        assertTrue(keepInDesk(row(CallLogKind.ANSWERED), ""))
    }
}
