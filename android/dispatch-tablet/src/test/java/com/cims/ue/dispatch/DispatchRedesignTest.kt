// 관제 재구성(레일 + 사이드 패널)의 판정들 — JVM, 기기 불필요 (android_dispatch_tablet.md §6.3·§6.3a·§6.9a·§6.12)
//
// 화면이 «무엇을 그릴지» 를 정하는 순수 함수만 본다: 채널 카드의 네 줄(시안 E1)과 오른쪽 아래 조작(한 카드에 하나), 채널 추가
// 패널의 사용자 목록(나 빼기·상태는 아는 것만·거르기)과 아래 줄 자격, 통화 고정 칸의 밀림, 대화 목록 미리보기, 날짜 칸,
// 이벤트 상세의 앞뒤 이벤트. 어긋나면 «누를 수 없는 버튼» 이 서거나 «모르는 오프라인» 을 단정하게 된다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.AccountKind
import com.cims.ue.dispatch.session.ActivityKind
import com.cims.ue.dispatch.session.ActivityRow
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.session.GroupInfo
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.session.Operation
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.ui.fixedShift
import com.cims.ue.dispatch.ui.ptt.addChannelActions
import com.cims.ue.dispatch.ui.ptt.CardControl
import com.cims.ue.dispatch.ui.ptt.CardKind
import com.cims.ue.dispatch.ui.ptt.ChannelCard
import com.cims.ue.dispatch.ui.ptt.FILTER_ALL
import com.cims.ue.dispatch.ui.ptt.FILTER_ONLINE
import com.cims.ue.dispatch.ui.ptt.aroundOf
import com.cims.ue.dispatch.ui.ptt.dayLabel
import com.cims.ue.dispatch.ui.ptt.filterBy
import com.cims.ue.dispatch.ui.ptt.kindLabel
import com.cims.ue.dispatch.ui.ptt.personMeta
import com.cims.ue.dispatch.ui.ptt.threadChips
import com.cims.ue.dispatch.ui.ptt.toCardUi
import com.cims.ue.dispatch.ui.ptt.userFilters
import com.cims.ue.dispatch.ui.ptt.userRows
import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.McpttCondition
import com.cims.ue.sdk.McpttInfo
import com.cims.ue.sdk.RosterEntry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Calendar

class DispatchRedesignTest {

    // ── 채널 카드의 조작 — 한 카드에 하나 ──
    private fun call(privateCall: Boolean = false, noFloorCtrl: Boolean = false, groupId: String = "g1") = CallInfo(
        callId = 1, accountId = 0, dir = CallDir.OUTGOING, state = CallState.ACTIVE,
        remoteUri = "sip:x@d", calledParty = "", video = false, mediaActive = true,
        muted = false, listen = true, playbackRoute = 0, lastCode = 0, lastReason = "",
        sources = emptyList(), isMcptt = true, groupId = groupId,
        mcptt = McpttInfo(true, "", "", "", "", false, false, privateCall, noFloorCtrl, false),
        halfDuplex = !noFloorCtrl, listenOnly = false, joinedDialog = "", condition = McpttCondition())

    private fun session(c: CallInfo) = SessionItem(c.callId, AccountKind.PTT, Operation.PTT_JOIN, c)
    private val group = GroupInfo("g1", "tel:g1", "순찰1", memberCount = 12)
    private fun ui(c: ChannelCard, pin: Int = 0) =
        c.toCardUi(pin = pin, targeted = false, selected = false, me = "5001", nameOf = { "" })

    @Test fun `참여 전 멤버 그룹은 시안 E1 의 네 줄이다 — 대기·멤버 수 · 미참여 · 참여·긴급`() {
        val u = ui(ChannelCard("g1", CardKind.MEMBER, "교통1", group = group), pin = 3)
        assertEquals("3. 교통1", u.title)
        assertEquals("대기 · 멤버 12", u.sub)
        assertEquals("미참여", u.roster)
        assertEquals("참여 전이면 4줄 왼쪽은 [참여]·[긴급] 자리다", "", u.meta)
        assertEquals(CardControl.JOIN, u.control)
    }

    @Test fun `남이 진행 중인 그룹은 참여 전이라도 진행 중이라고 적는다`() {
        val live = group.copy(roster = listOf(RosterEntry("tel:5002", "connected")))
        assertTrue(live.hasSession)
        assertEquals("진행 중 · 멤버 12", ui(ChannelCard("g1", CardKind.MEMBER, "교통1", group = live)).sub)
    }

    @Test fun `핀 번호는 내 채널의 차례다 — 0 이면 달지 않는다`() {
        assertEquals("1. 순찰1", ui(ChannelCard("g1", CardKind.MEMBER, "순찰1", group = group), pin = 1).title)
        assertEquals("순찰1", ui(ChannelCard("g1", CardKind.MEMBER, "순찰1", group = group)).title)
        assertEquals("이름만 — 접근성 라벨", "순찰1", ui(ChannelCard("g1", CardKind.MEMBER, "순찰1", group = group), pin = 2).name)
    }

    @Test fun `참여 중 반이중은 발언 대상 ✓ 를 단다`() {
        assertEquals(CardControl.TARGET, ui(ChannelCard("g1", CardKind.MEMBER, "순찰1", group = group, session = session(call()))).control)
    }

    @Test fun `전이중 개별 통화는 발언 대상 대신 음소거다 — 이름 앞에 종류, 접속자 줄은 상대`() {
        val c = ChannelCard("p1", CardKind.PRIVATE, "김반장", session = session(call(privateCall = true, noFloorCtrl = true)))
        assertEquals(CardControl.MUTE, ui(c).control)
        assertEquals("4. 개별 · 김반장", ui(c, pin = 4).title)
        assertEquals("김반장", ui(c).roster)
        assertEquals("1:1 은 참가 수 없이 경과만", "00:00", ui(c).meta)
    }

    @Test fun `긴급 세션은 2줄 앞에 긴급을 적는다`() {
        val c = ChannelCard("g1", CardKind.MEMBER, "순찰1", group = group,
            session = session(call().copy(condition = McpttCondition(emergency = true))))
        val u = ui(c)
        assertTrue(u.emergency)
        assertTrue(u.sub, u.sub.startsWith("긴급 · "))
    }

    @Test fun `접속자 줄은 발언자·나를 먼저 세우고 셋까지 편다`() {
        val roster = (1..6).map { RosterEntry("tel:500$it", "connected") }
        val u = ChannelCard("g1", CardKind.MEMBER, "순찰1", group = group.withRoster(roster), session = session(call()))
            .toCardUi(pin = 1, targeted = false, selected = false, me = "5003",
                nameOf = { n -> mapOf("5001" to "김관제", "5003" to "박현장")[n].orEmpty() })
        assertTrue(u.roster, u.roster.startsWith("박현장(나)"))
        assertTrue(u.roster, u.roster.endsWith("+3"))
    }

    // ── 채널 추가 패널의 사용자 목록 ──
    private val book = DirectoryBook(
        orgs = listOf(OrgNode("HQ", "본부"), OrgNode("OPS", "관제과", "HQ"), OrgNode("FLD", "현장과", "HQ")),
        entries = listOf(
            DirectoryEntry("OPS", "김관제", "5001"),
            DirectoryEntry("OPS", "이당직", "5002"),
            DirectoryEntry("FLD", "박현장", "5003"),
            DirectoryEntry("FLD", "최순찰", "5004"),
            DirectoryEntry("FLD", "최순찰", "5004"),                       // 중복 — 한 줄
            DirectoryEntry("", "민원인", "01055551111", external = true)))    // 외부망 — PTT 사용자가 아니다

    @Test fun `나·외부망·중복은 빼고 접속한 사람이 먼저다`() {
        val rows = userRows(book, me = "5001", present = setOf(DirectoryBook.normalize("5003")))
        assertEquals(listOf("박현장", "이당직", "최순찰"), rows.map { it.name })
        assertEquals("접속", rows.first().status)
        assertEquals("모르면 오프라인이라 단정하지 않는다", "", rows[1].status)
        assertEquals("PTT 5003 · 현장과", rows.first().meta)
    }

    @Test fun `거르기는 전체·접속·사람 많은 조직 순이다`() {
        val rows = userRows(book, me = "5001", present = emptySet())
        assertEquals(listOf(FILTER_ALL, FILTER_ONLINE, "현장과", "관제과"), userFilters(rows))
        assertEquals(listOf("박현장", "최순찰"), rows.filterBy("현장과", "").map { it.name })
        assertEquals(listOf("이당직"), rows.filterBy(FILTER_ALL, "5002").map { it.name })
        assertTrue(rows.filterBy(FILTER_ONLINE, "").isEmpty())
    }

    @Test fun `사람 메타는 PTT 번호와 소속 잎 이름이다`() {
        assertEquals("PTT 5002 · 관제과", personMeta("5002", book))
        assertEquals("주소록에 없으면 번호만", "PTT 5999", personMeta("5999", book))
    }

    // ── 채널 추가 패널의 아래 줄 ──
    @Test fun `한 명이면 개별·애드혹·그룹 추가가 모두 선다`() {
        val a = addChannelActions(book, "", listOf("5003"), canCreate = true, holding = false)
        assertEquals("5003", a.privateTarget)
        assertTrue(a.canPrivate && a.canAdhoc && a.canBroadcast && a.canGroup)
    }

    @Test fun `여럿이면 개별 통화는 서지 않는다 — 일대일이 아니다`() {
        val a = addChannelActions(book, "", listOf("5003", "5004"), canCreate = true, holding = false)
        assertEquals(null, a.privateTarget)
        assertFalse(a.canPrivate)
        assertTrue(a.canAdhoc && a.canGroup)
    }

    @Test fun `아무도 안 골랐으면 입력한 번호로 개별 통화만 선다`() {
        val a = addChannelActions(book, "1099", emptyList(), canCreate = true, holding = false)
        assertEquals("1099", a.privateTarget)
        assertTrue(a.canPrivate)
        assertFalse(a.canAdhoc || a.canGroup || a.canBroadcast)
        assertFalse("모르는 이름은 걸지 않는다", addChannelActions(book, "박주임", emptyList(), true, false).canPrivate)
    }

    @Test fun `그룹 추가는 생성 자격이 있어야 하고, 일제 통화 중에는 다른 조작이 잠긴다`() {
        assertFalse(addChannelActions(book, "", listOf("5003"), canCreate = false, holding = false).canGroup)
        val held = addChannelActions(book, "", listOf("5003", "5004"), canCreate = true, holding = true)
        assertFalse(held.canPrivate || held.canAdhoc || held.canGroup)
        assertTrue("누르는 동안은 [일제 통화] 가 남는다", held.canBroadcast)
    }

    // ── 통화 고정 칸의 밀림 ──
    @Test fun `고정 칸은 통화 면끼리는 제자리, 무전에서 넘어올 때만 함께 민다`() {
        // 통화 면 = 3..5
        assertEquals(0f, fixedShift(3f, 3, 5), 0f)
        assertEquals(0f, fixedShift(4.5f, 3, 5), 0f)
        assertEquals("무전 «이벤트» → 통화 «통화» 중간", 0.25f, fixedShift(2.75f, 3, 5), 1e-6f)
        assertEquals("무전 면에서는 화면 밖(오른쪽)", 1f, fixedShift(0f, 3, 5), 0f)
        assertEquals("뒤로 나가면 왼쪽", -0.5f, fixedShift(5.5f, 3, 5), 1e-6f)
    }

    // ── 대화 목록 ──
    private fun msg(id: String, text: String, at: Long, out: Boolean = false, from: String = "", read: Boolean = true) =
        Message(id = id, groupId = "k", fromUri = "", fromName = from, text = text, atMs = at, outgoing = out, read = read)

    @Test fun `미리보기는 마지막 한 통 — 그룹이면 보낸 사람을 붙인다`() {
        val all = mapOf(
            "g1" to listOf(msg("1", "현장 도착", 1, from = "박현장"), msg("2", "확인 바랍니다", 2, out = true)),
            "g2" to listOf(msg("3", "교대 요청", 3, from = "이당직", read = false)),
            "5003" to listOf(msg("4", "이상 없습니다", 4, from = "박현장")))
        val chips = threadChips(all, isGroup = { it.startsWith("g") }) { it }
        assertEquals(listOf("5003", "g2", "g1"), chips.map { it.key })
        assertEquals("이상 없습니다", chips[0].last)
        assertEquals("이당직: 교대 요청", chips[1].last)
        assertEquals("나: 확인 바랍니다", chips[2].last)
        assertEquals(listOf(false, true, true), chips.map { it.group })
        assertEquals(1, chips[1].unread)
    }

    @Test fun `날짜 칸은 오늘·어제·월일이다`() {
        val now = Calendar.getInstance().apply { set(2026, Calendar.SEPTEMBER, 30, 14, 0) }.timeInMillis
        assertEquals("오늘", dayLabel(now - 3_600_000L, now))
        assertEquals("어제", dayLabel(now - 86_400_000L, now))
        assertEquals("9월 20일", dayLabel(now - 10 * 86_400_000L, now))
    }

    // ── 이벤트 상세 ──
    private fun ev(id: Long, at: Long, ch: String = "g1", kind: ActivityKind = ActivityKind.TALK) =
        ActivityRow(at, ch, ch, "e$id", kind, id = id)

    @Test fun `앞뒤 이벤트는 같은 채널의 앞 둘·뒤 둘이다`() {
        val all = listOf(ev(1, 10), ev(2, 20), ev(3, 30, ch = "g2"), ev(4, 40), ev(5, 50), ev(6, 60), ev(7, 70)).reversed()
        assertEquals(listOf(1L, 2L, 4L, 5L, 6L), aroundOf(all.first { it.id == 4L }, all).map { it.id })
        assertEquals("처음이면 뒤만", listOf(1L, 2L, 4L), aroundOf(all.first { it.id == 1L }, all).map { it.id })
        assertEquals("다른 채널은 섞지 않는다", listOf(3L), aroundOf(all.first { it.id == 3L }, all).map { it.id })
    }

    @Test fun `종류 낱말`() {
        assertEquals(listOf("발언", "입장", "퇴장", "긴급", "SDS", "오류"), ActivityKind.entries.map(::kindLabel))
    }
}
