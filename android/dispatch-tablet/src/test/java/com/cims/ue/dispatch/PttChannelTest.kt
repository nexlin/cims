// [PTT] 탭 계약 시험 — JVM, 기기 불필요 (android_dispatch_tablet.md §9)
//
// 노리는 것은 **조용히 어긋나면 관제사가 엉뚱한 채널로 송출하는** 계약들이다.
// 특히 «포커스(보는 채널) ≠ 발언 대상(말하는 채널)» — 이게 섞이면 사고가 난다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.AccountKind
import com.cims.ue.dispatch.session.GroupInfo
import com.cims.ue.dispatch.session.Operation
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.session.SessionKind
import com.cims.ue.dispatch.ui.ptt.CardKind
import com.cims.ue.dispatch.ui.ptt.ChannelCard
import com.cims.ue.dispatch.ui.ptt.PttChannelsViewModel
import com.cims.ue.dispatch.ui.ptt.ScopeFilter
import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.FloorInfo
import com.cims.ue.sdk.FloorState
import com.cims.ue.sdk.McpttInfo
import com.cims.ue.sdk.RosterEntry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PttChannelTest {

    // ── 세션 종류 판정 — 카드가 어느 패널에 서는지를 정한다 ──
    private fun call(
        id: Int = 1, mcptt: Boolean = true, listenOnly: Boolean = false,
        privateCall: Boolean = false, groupId: String = "g001",
        joinedDialog: String = "", state: CallState = CallState.ACTIVE,
        noFloorCtrl: Boolean = false, emergency: Boolean = false,
    ) = CallInfo(
        callId = id, accountId = 0, dir = CallDir.OUTGOING, state = state,
        remoteUri = "sip:x@d", calledParty = "", video = false, mediaActive = true,
        muted = false, listen = true, playbackRoute = 0, lastCode = 0, lastReason = "",
        sources = emptyList(), isMcptt = mcptt, groupId = groupId,
        mcptt = McpttInfo(mcptt, "", "", "", "", emergency, false, privateCall, noFloorCtrl),
        halfDuplex = !noFloorCtrl, listenOnly = listenOnly, joinedDialog = joinedDialog)

    @Test fun `세션 종류가 화면 배치를 정한다`() {
        assertEquals(SessionKind.PTT_CHANNEL, SessionKind.of(call()))
        assertEquals(SessionKind.PTT_PRIVATE, SessionKind.of(call(privateCall = true)))
        assertEquals(SessionKind.PTT_ADHOC, SessionKind.of(call(groupId = "adhoc-me-123")))
        assertEquals(SessionKind.PTT_LISTEN, SessionKind.of(call(listenOnly = true)))
        assertEquals(SessionKind.PHONE_CALL, SessionKind.of(call(mcptt = false, groupId = "")))
        assertEquals(SessionKind.PHONE_MONITOR,
            SessionKind.of(call(mcptt = false, groupId = "", listenOnly = true, joinedDialog = "d1")))
    }

    @Test fun `청취 전용은 카드가 아니라 시트다`() {
        // ② 에서 [청취] 로 연 세션이 ① 카드로 서면 발언 대상이 될 수 있어 위험하다.
        assertTrue(SessionKind.PTT_LISTEN.isSheet)
        assertFalse(SessionKind.PTT_LISTEN.isPttCard)
        assertTrue(SessionKind.PTT_CHANNEL.isPttCard)
        assertTrue(SessionKind.PHONE_MONITOR.isSheet)
    }

    // ── 발언 대상 자격 ──
    private fun session(c: CallInfo, floor: FloorInfo? = null) =
        SessionItem(c.callId, AccountKind.PTT, Operation.PTT_JOIN, c, floor = floor)

    private fun card(kind: CardKind = CardKind.MEMBER, s: SessionItem? = null, g: GroupInfo? = null) =
        ChannelCard(id = "g001", kind = kind, title = "순찰1", group = g, session = s)

    @Test fun `참여하지 않은 채널은 발언 대상이 될 수 없다`() {
        assertFalse(card(g = GroupInfo("g001", "tel:g001", "순찰1")).canCheck)
    }

    @Test fun `전이중 사설콜은 발언 대상이 될 수 없다`() {
        // 마이크가 늘 열려 있어 floor 가 없다 — 음소거로 다룬다.
        val s = session(call(privateCall = true, noFloorCtrl = true))
        assertFalse(card(CardKind.PRIVATE, s).canCheck)
    }

    @Test fun `참여 중 반이중이면 발언 대상이 된다`() {
        assertTrue(card(s = session(call())).canCheck)
    }

    @Test fun `끝난 세션은 발언 대상이 될 수 없다`() {
        assertFalse(card(s = session(call(state = CallState.DISCONNECTED))).canCheck)
    }

    // ── 다중 발언 상한 ──
    @Test fun `팬아웃 전에는 발언 대상이 하나다`() {
        // 3GPP 에 UE 다중 그룹 동시 발언 절차가 없어 단말 팬아웃으로 푼다. 코어 API 가 들어오면
        // 이 상수만 바꾸면 되도록 발언 바·칩·게이지는 집합 기준으로 만들어 뒀다.
        assertFalse(PttChannelsViewModel.MULTI_TALK_SUPPORTED)
    }

    // ── 카드 표시 ──
    @Test fun `참여하지 않아도 로스터로 진행을 안다`() {
        val g = GroupInfo("g001", "tel:g001", "순찰1")
            .withRoster(listOf(RosterEntry("tel:+8250", "connected")))
        val c = card(g = g)
        assertTrue(c.hasSession)
        assertFalse(c.joined)
        assertEquals("진행(미참여)", c.stateText)
    }

    @Test fun `대기 중인 멤버 카드는 멤버 수를 보여준다`() {
        val g = GroupInfo("g001", "tel:g001", "순찰1", memberCount = 12)
        assertEquals("대기", card(g = g).stateText)
        assertEquals("멤버 12", card(g = g).line2)
    }

    @Test fun `발언자가 있으면 2줄에 나온다`() {
        val s = session(call()).copy(speaker = "김순경")
        assertTrue(card(s = s).line2.startsWith("발언 김순경"))
    }

    @Test fun `참여 중 발언자가 없으면 그렇게 쓴다`() {
        assertEquals("발언 없음", card(s = session(call())).line2)
    }

    // ── 로스터 → 세션 시작 시각 ──
    @Test fun `로스터에 접속자가 생기면 세션 시작 시각이 남는다`() {
        val g0 = GroupInfo("g001", "tel:g001", "순찰1")
        assertNull(g0.sessionSinceMs)
        val g1 = g0.withRoster(listOf(RosterEntry("tel:+8250", "connected")))
        assertTrue(g1.hasSession)
        assertTrue(g1.sessionSinceMs != null)
        // 참가자가 늘어도 시작 시각은 그대로여야 한다(경과가 초기화되면 안 된다)
        val g2 = g1.withRoster(listOf(RosterEntry("tel:+8250", "connected"), RosterEntry("tel:+8251", "connected")))
        assertEquals(g1.sessionSinceMs, g2.sessionSinceMs)
        assertEquals(2, g2.connectedCount)
        // 전원이 나가면 지워진다
        val g3 = g2.withRoster(emptyList())
        assertFalse(g3.hasSession)
        assertNull(g3.sessionSinceMs)
    }

    @Test fun `연결 중이 아닌 참가자는 세지 않는다`() {
        val g = GroupInfo("g001", "tel:g001", "순찰1")
            .withRoster(listOf(RosterEntry("tel:+8250", "on-hold"), RosterEntry("tel:+8251", "connected")))
        assertEquals(1, g.connectedCount)
    }

    // ── floor 상태 → 카드 ──
    private fun floor(st: FloorState, canRequest: Boolean = true) =
        FloorInfo(st, emptyList(), canRequest, 0, 0, 0, "", 0, 0, 0, 0)

    @Test fun `floor 상태가 카드에 그대로 뜬다`() {
        assertTrue(session(call(), floor(FloorState.SPEAKING)).isSpeaking)
        assertTrue(session(call(), floor(FloorState.REQUESTING)).isRequesting)
        assertTrue(session(call(), floor(FloorState.QUEUED)).isQueued)
        assertFalse(session(call(), floor(FloorState.IDLE)).isSpeaking)
    }

    @Test fun `청취 중에는 발언 버튼이 막힌다`() {
        // Floor Taken 의 Permission to Request the Floor = 0 (TS 24.380) — 앱은 버튼을 비활성한다.
        assertFalse(session(call(listenOnly = true), floor(FloorState.LISTENING, canRequest = false)).canRequestFloor)
        assertTrue(session(call(), floor(FloorState.LISTENING, canRequest = true)).canRequestFloor)
    }

    // ── ② 필터 축 ──
    @Test fun `범위 채널 필터는 네 축이다`() {
        assertEquals(listOf("전체", "활성", "긴급", "청취 중"), ScopeFilter.entries.map { it.label })
    }
}

/**
 * 사설콜·애드혹(§4.1) — 임시 그룹 id 규약과 대상 후보.
 *
 * `adhoc-`·`priv-` 는 편성 그룹 **예약어**다(mcptt_emergency_modes.md §6). id 를 앱이 만드는 이유는
 * 서버에 편성이 없는 임시 세션이기 때문이고, 그래서 규약을 어기면 편성 그룹과 충돌한다.
 */
class AdhocIdTest {

    @Test fun `id 는 adhoc-번호-epoch 이고 스킴을 벗긴다`() {
        assertEquals("adhoc-5001-1700000000",
            com.cims.ue.dispatch.session.adhocIdOf("tel:5001", 1_700_000_000))
        assertEquals("adhoc-5001-1700000000",
            com.cims.ue.dispatch.session.adhocIdOf("sip:5001@cims.local", 1_700_000_000))
        assertEquals("adhoc-5001-1700000000",
            com.cims.ue.dispatch.session.adhocIdOf("5001", 1_700_000_000))
        assertEquals("adhoc-5001-1700000000",
            com.cims.ue.dispatch.session.adhocIdOf("TEL:5001;phone-context=x", 1_700_000_000))
    }

    @Test fun `만든 id 는 애드혹으로 인식된다 — 세션 종류 판정과 같은 접두사`() {
        val id = com.cims.ue.dispatch.session.adhocIdOf("tel:5001", 1)
        assertTrue(com.cims.ue.dispatch.session.isAdhocId(id))
        assertTrue(id.startsWith(com.cims.ue.dispatch.session.SessionKind.ADHOC_PREFIX))
        assertFalse(com.cims.ue.dispatch.session.isAdhocId("g001"))
    }

    @Test fun `후보는 이미 고른 사람을 뺀다`() {
        val book = com.cims.ue.dispatch.session.DirectoryBook(entries = listOf(
            com.cims.ue.dispatch.session.DirectoryEntry("T", "김순경", "5001"),
            com.cims.ue.dispatch.session.DirectoryEntry("T", "이순경", "5002")))
        val picked = listOf(com.cims.ue.dispatch.session.DirectoryEntry("T", "김순경", "5001"))
        assertEquals(listOf("5002"),
            com.cims.ue.dispatch.ui.ptt.pttCandidates(book, "", picked).map { it.msisdn })
    }
}
