// 긴급·임박 — 배너 스택·전이·표시 규약 시험 — JVM, 기기 불필요 (android_dispatch_tablet.md §6.2a-1)
//
// 노리는 것은 **긴급을 놓치거나 잘못 알리는** 계약들이다 — 개별 통화가 채널 배너로 새는 것, 같은 채널에 배너가 둘
// 서는 것, 갱신마다 경과가 0 으로 돌아가는 것, 임박이 긴급을 가리는 것.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.AccountKind
import com.cims.ue.dispatch.session.AlertChange
import com.cims.ue.dispatch.session.AlertKind
import com.cims.ue.dispatch.session.Operation
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.session.alertStack
import com.cims.ue.dispatch.session.alertTransition
import com.cims.ue.dispatch.ui.ptt.CardKind
import com.cims.ue.dispatch.ui.ptt.ChannelCard
import com.cims.ue.dispatch.ui.ptt.channelHead
import com.cims.ue.dispatch.ui.ptt.toRowUi
import com.cims.ue.dispatch.ui.ALERT_VISIBLE
import com.cims.ue.dispatch.ui.alertFold
import com.cims.ue.dispatch.ui.toAlertBannerUi
import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.McpttInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class EmergencyAlertTest {

    private fun session(
        id: Int = 1, groupId: String = "g1", emergency: Boolean = false, peril: Boolean = false,
        privateCall: Boolean = false, mcptt: Boolean = true, listenOnly: Boolean = false,
        caller: String = "", state: CallState = CallState.ACTIVE, since: Long? = null, started: Long = 1_000L,
    ): SessionItem {
        val info = CallInfo(
            callId = id, accountId = 0, dir = CallDir.INCOMING, state = state,
            remoteUri = "sip:x@d", calledParty = "", video = false, mediaActive = true,
            muted = false, listen = true, playbackRoute = 0, lastCode = 0, lastReason = "",
            sources = emptyList(), isMcptt = mcptt, groupId = if (mcptt) groupId else "",
            mcptt = McpttInfo(mcptt, "", "", caller, "", emergency, peril, privateCall, false),
            halfDuplex = true, listenOnly = listenOnly, joinedDialog = "")
        return SessionItem(id, AccountKind.PTT, Operation.PTT_JOIN, info,
            startedAtMs = started, title = "순찰1", alertSinceMs = since)
    }

    // ── 종류 판정 ──
    @Test fun `긴급이 임박보다 앞선다`() {
        assertEquals(AlertKind.EMERGENCY, session(emergency = true, peril = true).alertKind)
        assertEquals(AlertKind.IMMINENT_PERIL, session(peril = true).alertKind)
        assertNull(session().alertKind)
    }

    @Test fun `개별 통화와 전화는 채널 배너가 아니다`() {
        // 개별 통화 착신은 착신 배너가 받는다 — 채널 배너로 두 번 알리면 어느 것을 눌러야 하는지 흐려진다.
        assertNull(session(emergency = true, privateCall = true).alertKind)
        assertNull(session(emergency = true, mcptt = false).alertKind)
    }

    @Test fun `청취 세션·애드혹의 긴급도 채널 배너다`() {
        assertEquals(AlertKind.EMERGENCY, session(emergency = true, listenOnly = true).alertKind)
        assertEquals(AlertKind.EMERGENCY, session(emergency = true, groupId = "adhoc-1003-1").alertKind)
    }

    // ── 전이 ──
    @Test fun `처음 보는 긴급은 개시다`() {
        val (since, change) = alertTransition(null, session(emergency = true), nowMs = 5_000)
        assertEquals(5_000L, since)
        assertEquals(AlertChange.Started(AlertKind.EMERGENCY), change)
    }

    @Test fun `같은 긴급이 다시 오면 경과를 이어 간다`() {
        // 스냅샷은 호 상태·미디어마다 다시 온다 — 그때마다 0 으로 돌아가면 «얼마나 됐나» 를 알 수 없다.
        val prev = session(emergency = true, since = 2_000)
        val (since, change) = alertTransition(prev, session(emergency = true), nowMs = 9_000)
        assertEquals(2_000L, since)
        assertEquals(AlertChange.None, change)
    }

    @Test fun `임박에서 긴급으로 오르면 새 개시다`() {
        val prev = session(peril = true, since = 2_000)
        val (since, change) = alertTransition(prev, session(emergency = true), nowMs = 9_000)
        assertEquals(9_000L, since)
        assertEquals(AlertChange.Started(AlertKind.EMERGENCY), change)
    }

    @Test fun `조건이 풀리면 해제다`() {
        val prev = session(peril = true, since = 2_000)
        val (since, change) = alertTransition(prev, session(), nowMs = 9_000)
        assertNull(since)
        assertEquals(AlertChange.Cleared(AlertKind.IMMINENT_PERIL), change)
    }

    @Test fun `긴급이 아닌 세션은 아무것도 남기지 않는다`() {
        assertEquals(null to AlertChange.None, alertTransition(null, session(), nowMs = 9_000))
    }

    // ── 스택 ──
    @Test fun `채널마다 하나 최신이 위 — 같은 채널이면 처음 본 것`() {
        val stack = alertStack(listOf(
            session(id = 1, groupId = "g1", emergency = true, since = 1_000),
            session(id = 2, groupId = "g2", peril = true, since = 3_000),
            session(id = 3, groupId = "g1", emergency = true, since = 2_000),   // 같은 채널 — 채널의 긴급은 1_000 부터
            session(id = 4, groupId = "g3"),                                     // 긴급 아님
            session(id = 5, groupId = "g4", emergency = true, privateCall = true),
        ))
        assertEquals(listOf(2, 1), stack.map { it.callId })
    }

    @Test fun `배너는 두 장까지 펴고 나머지는 접는다`() {
        // 닫기 없는 배너가 쌓여 본문·착신 배너 자리를 다 먹으면 [채널로 이동] 을 눌러도 대응할 화면이 없다.
        assertEquals(1, alertFold(1, expanded = false))
        assertEquals(ALERT_VISIBLE, alertFold(5, expanded = false))
        assertEquals(5, alertFold(5, expanded = true))
    }

    @Test fun `끝난 세션은 스택에서 빠진다`() {
        assertTrue(alertStack(listOf(session(emergency = true, state = CallState.DISCONNECTED))).isEmpty())
    }

    // ── 배너 표시 ──
    @Test fun `배너는 카드와 같은 채널 id 로 간다`() {
        val b = session(emergency = true, caller = "sip:1003@d", since = 7_000)
            .toAlertBannerUi { "1003 이순경" }!!
        assertEquals("g1", b.channelId)
        assertEquals("순찰1", b.title)
        assertEquals("1003 이순경", b.initiator)
        assertEquals(7_000L, b.sinceMs)
    }

    @Test fun `개시자를 모르면 비운다`() {
        // 내가 건 긴급은 코어가 개시자를 싣지 않는다 — 빈 값을 번호처럼 풀면 «개시 » 만 남는다.
        assertEquals("", session(emergency = true).toAlertBannerUi { "?" }!!.initiator)
        assertNull(session().toAlertBannerUi { it })
    }

    // ── 행·머리 ──
    private fun card(s: SessionItem) = ChannelCard(id = "g1", kind = CardKind.MEMBER, title = "순찰1", session = s)

    @Test fun `행과 머리에 임박이 선다 — 긴급이면 긴급만`() {
        assertTrue(card(session(peril = true)).toRowUi(targeted = false).imminentPeril)
        assertTrue(channelHead("g1", card(session(peril = true)), range = null, targeted = false).imminentPeril)
        val both = card(session(emergency = true, peril = true))
        assertTrue(both.toRowUi(targeted = false).emergency)
        assertFalse(both.toRowUi(targeted = false).imminentPeril)
        assertFalse(channelHead("g1", both, range = null, targeted = false).imminentPeril)
    }
}
