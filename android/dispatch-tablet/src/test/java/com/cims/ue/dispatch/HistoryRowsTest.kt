// [이력] 목록의 순수 로직 — 통화 결과 태그·행 표시값·빈 세션 묶음·시간대 묶음·밴드·요약·영상 판정
// (android_dispatch_tablet.md §6.11, dispatch_desktop_ui.md §4.6)
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.RecordingInfo
import com.cims.ue.dispatch.session.RecordingSegment
import com.cims.ue.dispatch.session.SegmentTrack
import com.cims.ue.dispatch.ui.history.BandMode
import com.cims.ue.dispatch.ui.history.HistoryNames
import com.cims.ue.dispatch.ui.history.HistoryUi
import com.cims.ue.dispatch.ui.history.ListItem
import com.cims.ue.dispatch.ui.history.ResultTone
import com.cims.ue.dispatch.ui.history.RowKind
import com.cims.ue.dispatch.ui.history.ServiceFilter
import com.cims.ue.dispatch.ui.history.bundleSilent
import com.cims.ue.dispatch.ui.history.callResultOf
import com.cims.ue.dispatch.ui.history.filterRows
import com.cims.ue.dispatch.ui.history.groupByHour
import com.cims.ue.dispatch.ui.history.historyListOf
import com.cims.ue.dispatch.ui.history.rowOf
import com.cims.ue.dispatch.ui.history.sessionCells
import com.cims.ue.dispatch.ui.history.summaryOf
import com.cims.ue.dispatch.ui.history.turnCells
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.LocalDate
import java.time.ZoneId

class HistoryRowsTest {

    private val day: LocalDate = LocalDate.of(2026, 9, 8)

    private fun at(h: Int, m: Int = 0, s: Int = 0): Long =
        day.atTime(h, m, s).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli()

    private val u1 = "+821310002001"
    private val u2 = "+821310002002"
    private val u3 = "+821310002003"
    private val u4 = "+821310002004"

    private fun call(invite: Long, answer: Long? = null, end: Long? = null, reason: String = "", state: String = "ended",
                     dur: Int = 0, type: String = "volte") = HistoryEntry(
        id = "c$invite", atMs = end ?: invite, kind = HistoryKind.CALL, from = "tel:$u1", to = "tel:$u2", durationSec = dur,
        state = state, callType = type, inviteAtMs = invite, answerAtMs = answer, endAtMs = end, endReason = reason)

    private fun ptt(id: String, start: Long, sec: Int, turns: Int = 0, rec: Boolean = false, from: String = u3,
                    group: String = "g003", service: String = "ptt", counted: Boolean = true, live: Boolean = false,
                    speechMs: Int = 0, kind: String = "group", people: List<String> = emptyList()) = HistoryEntry(
        id = id, atMs = start + sec * 1000L, kind = HistoryKind.PTT, from = "tel:$from", group = "tel:$group", durationSec = sec,
        recordingId = if (rec) "ptt/1/$id" else "", hasRecording = rec, state = if (live) "active" else "ended",
        sessionKind = kind, startAtMs = start, endAtMs = if (live) null else start + sec * 1000L, groupName = "야간 순찰",
        turnCount = turns, hasTurnCount = counted, totalSpeechMs = speechMs, people = people, service = service)

    // ── 통화 결과 태그 ──────────────────────────────────────────────────────

    @Test fun `결과 태그 — 진행 중이면 상태가, 응답 시각이 있으면 응답이 정한다`() {
        assertEquals("통화 중", callResultOf(call(at(9), state = "active")).text)
        assertEquals(ResultTone.TALK, callResultOf(call(at(9), state = "active")).tone)
        assertEquals("호출 중", callResultOf(call(at(9), state = "ringing")).text)
        val answered = callResultOf(call(at(9), answer = at(9, 0, 5), end = at(9, 1), reason = "normal"))
        assertEquals("응답", answered.text)
        assertEquals(ResultTone.NEUTRAL, answered.tone)
    }

    @Test fun `결과 태그 — 응답이 없으면 종료 사유가 정한다`() {
        fun of(reason: String) = callResultOf(call(at(9), end = at(9, 0, 20), reason = reason))
        assertEquals("부재" to ResultTone.WARN, of("no_answer").let { it.text to it.tone })
        assertEquals("부재" to ResultTone.WARN, of("timeout").let { it.text to it.tone })
        assertEquals("실패" to ResultTone.WARN, of("busy").let { it.text to it.tone })
        assertEquals("거절" to ResultTone.BAD, of("rejected").let { it.text to it.tone })
        assertEquals("오류" to ResultTone.BAD, of("error").let { it.text to it.tone })
        assertEquals("오류" to ResultTone.BAD, of("incomplete").let { it.text to it.tone })
        // 응답 전에 발신자가 끊었다 — 사유가 비었거나 normal 이어도 응답 시각이 없으면 취소다
        assertEquals("취소" to ResultTone.WARN, of("").let { it.text to it.tone })
        assertEquals("취소" to ResultTone.WARN, of("normal").let { it.text to it.tone })
    }

    // ── 통화 행 ─────────────────────────────────────────────────────────────

    @Test fun `통화 카드 — 응답한 통화는 통화 시간과 울림, 진행 막대는 울림 대 통화`() {
        val r = rowOf(call(at(9, 4, 25), answer = at(9, 4, 30), end = at(9, 5, 12), reason = "normal", dur = 42))
        assertEquals("5초", r.ringText)
        assertEquals("42초", r.durationText)
        assertEquals("42초 · 울림 5초", r.callSub)
        assertEquals(5f, r.ringWeight, 0.01f)
        assertEquals(42f, r.talkWeight, 0.01f)
        assertEquals("09:04", r.startClock)
        assertEquals(9, r.hour)
        assertEquals("정상종료", r.endReasonText)
        assertEquals("음성", r.typeText)
    }

    @Test fun `통화 카드 — 응답이 없으면 끝난 이유와 울림, 막대는 울림만`() {
        val r = rowOf(call(at(9, 30, 40), end = at(9, 31), reason = "no_answer"))
        assertEquals("20초", r.ringText)
        assertEquals("—", r.durationText)
        assertEquals("무응답 · 울림 20초", r.callSub)
        assertEquals(0f, r.talkWeight, 0f)
        assertTrue(r.ringWeight > 0f)
    }

    @Test fun `통화 카드 — 진행 중`() {
        assertEquals("통화 중 · 울림 4초", rowOf(call(at(13, 40), answer = at(13, 40, 4), state = "active")).callSub)
        assertEquals("호출 중", rowOf(call(at(13, 40), state = "ringing")).callSub)
        assertTrue(rowOf(call(at(13, 40), state = "ringing")).live)
    }

    @Test fun `통화 — 이름이 있으면 이름 아래 번호, 없으면 번호가 곧 이름`() {
        val names = HistoryNames(who = { if (it.endsWith("2001")) "김관제" else it.substringAfter(':') })
        val r = rowOf(call(at(9)), names)
        assertEquals("김관제", r.caller)
        assertEquals("이름 아래 번호는 국내 표기", com.cims.ue.dispatch.session.localNumber(u1), r.callerNumber)
        assertEquals(u2, r.callee)
        assertEquals("이름이 없으면 둘째 줄을 비운다", "", r.calleeNumber)
        assertEquals("김관제 → $u2", r.parties)
    }

    // ── 무전 행 ─────────────────────────────────────────────────────────────

    @Test fun `무전 카드 세 줄 — 대상 · 개시와 참여 · 길이와 발언`() {
        val r = rowOf(ptt("s1", at(9, 10), 160, turns = 7, rec = true, from = u1, speechMs = 61_000,
            people = listOf(u1, u2, u3, u4)))
        assertEquals("야간 순찰", r.target)
        assertEquals("그룹", r.kindText)
        assertEquals("개시 $u1 · 참여 4명", r.whoLine)
        assertEquals("2분 40초 · 발언 7회 · 말한 시간 1분 1초", r.statLine)
        assertEquals("09:10:00 ~ 09:12:40 · 2분 40초", r.rangeText)
        assertEquals(160, r.durSec)
        assertEquals("09:10", r.startClock)
    }

    @Test fun `무전 대상 — 그룹 목록의 이름이 항목의 이름보다 먼저다`() {
        val names = HistoryNames(group = { if (it == "tel:g003") "3팀 무전" else "" })
        assertEquals("3팀 무전", rowOf(ptt("s1", at(9), 30), names).target)
        assertEquals("목록에 없으면 항목의 이름", "야간 순찰", rowOf(ptt("s1", at(9), 30, group = "g999"), names).target)
    }

    @Test fun `무전 대상 — 개별 호는 A ↔ B, 애드혹은 A 외 n명`() {
        val priv = rowOf(ptt("p", at(14, 20), 30, from = u1, kind = "private", people = listOf(u1, u4)))
        assertEquals("$u1 ↔ $u4", priv.target)
        assertTrue(priv.isPrivate)
        val adhoc = rowOf(ptt("a", at(14, 30), 30, from = u1, kind = "adhoc", people = listOf(u1, u2, u3)))
        assertEquals("$u1 외 2명", adhoc.target)
        assertTrue(adhoc.isAdhoc)
    }

    @Test fun `진행 중인 세션 — 길이 자리에 진행 중`() {
        val r = rowOf(ptt("s", at(11, 40), 0, turns = 1, live = true))
        assertTrue(r.live)
        assertEquals("진행 중 · 발언 1회", r.statLine)
        assertEquals("11:40:00 ~ 진행중", r.rangeText)
    }

    // ── 영상 판정 ───────────────────────────────────────────────────────────

    @Test fun `영상 세션 — 낱말이 발언에서 송출로 바뀐다`() {
        val e = ptt("v", at(15, 5), 200, turns = 2, rec = true, service = "mcvideo", speechMs = 95_000)
            .copy(mcvSessionType = "chat", mcvMaxTransmitters = 2)
        val r = rowOf(e)
        assertTrue(e.isMcVideo)
        assertEquals("송출", r.turnWord)
        assertEquals("3분 20초 · 송출 2회 · 보낸 시간 1분 35초", r.statLine)
        assertEquals("chat · 동시 송출 2", r.mcvText)
        assertEquals("음성 세션에는 영상 속성이 없다", "", rowOf(ptt("s", at(9), 30)).mcvText)
    }

    @Test fun `영상 통화 — callType 이 volte_video`() {
        val r = rowOf(call(at(13), answer = at(13, 0, 8), end = at(13, 5), dur = 292, type = "volte_video"))
        assertTrue(r.isVideoCall)
        assertEquals("영상", r.typeText)
        assertFalse(rowOf(call(at(13))).isVideoCall)
    }

    @Test fun `영상 세션은 항목에 서비스 축이 없어도 녹취 메타로 안다`() {
        assertTrue(RecordingInfo(id = "r", service = "mcvideo").isMcVideo)
        assertTrue(RecordingInfo(id = "r", segments = listOf(RecordingSegment(1, type = "mcvideo"))).isMcVideo)
        assertFalse(RecordingInfo(id = "r", service = "ptt", segments = listOf(RecordingSegment(1, type = "ptt"))).isMcVideo)
    }

    @Test fun `영상 칸은 세그먼트에 영상이 있을 때만 열린다`() {
        assertTrue(RecordingSegment(1, hasVideo = true).showsVideo)
        assertTrue(RecordingSegment(1, tracks = listOf(SegmentTrack(0, "video"))).showsVideo)
        assertFalse(RecordingSegment(1, tracks = listOf(SegmentTrack(0, "audio"))).showsVideo)
    }

    // ── 빈 세션 ─────────────────────────────────────────────────────────────

    @Test fun `빈 세션 — 발언 수 0 이 실려 왔고 녹취도 없는 종료 세션`() {
        assertTrue(rowOf(ptt("s", at(13), 30)).silent)
        assertFalse("발언 수가 안 실려 왔으면 0 은 모름이다", rowOf(ptt("s", at(13), 30, counted = false)).silent)
        assertFalse("스캔 폴백은 발언 수가 늘 0 이라 녹취로 한 번 더 가른다", rowOf(ptt("s", at(13), 30, rec = true)).silent)
        assertFalse("진행 중이면 아니다", rowOf(ptt("s", at(13), 30, live = true)).silent)
        assertFalse(rowOf(ptt("s", at(13), 30, turns = 1)).silent)
        assertFalse("통화에는 빈 세션이 없다", rowOf(call(at(13))).silent)
    }

    /** 한 사람이 1분마다 호를 열었다 말없이 닫는다(30~31초) — 13:24 만 발언이 있다. 최근이 위. */
    private fun repeated() = (0 until 9).map { i ->
        val spoke = i == 4
        rowOf(ptt("rep-$i", at(13, 20 + i), if (spoke) 46 else 30 + i % 2, turns = if (spoke) 1 else 0, rec = spoke,
            speechMs = if (spoke) 3_000 else 0))
    }.sortedByDescending { it.e.atMs }

    @Test fun `빈 세션 묶음 — 연달아 나오는 2건 이상을 한 장으로, 사이에 낀 세션에서 끊긴다`() {
        val lines = bundleSilent(repeated(), enabled = true)
        assertEquals(listOf(RowKind.BUNDLE, RowKind.CARD, RowKind.BUNDLE), lines.map { it.kind })
        assertEquals(listOf("rep-8", "rep-7", "rep-6", "rep-5"), lines[0].members.map { it.e.id })
        assertEquals("rep-4", lines[1].row.e.id)
        assertEquals(listOf("rep-3", "rep-2", "rep-1", "rep-0"), lines[2].members.map { it.e.id })
        // 머리 카드 — 고르면 가장 최근 세션, 시각은 가장 이른 ~ 가장 늦은 시작
        assertEquals("rep-8", lines[0].row.e.id)
        assertEquals("13:25 ~ 13:28", lines[0].clock)
        assertEquals("빈 세션 4건", lines[0].tagText)
        assertEquals("각 30~31초 · 발언 0회", lines[0].statLine)
        assertEquals("4건 펼치기", lines[0].toggleText)
        // 펼침의 열쇠 = 시간대 + 가장 이른 세션
        assertEquals("13|rep-5", lines[0].bundleKey)
        assertEquals("13|rep-0", lines[2].bundleKey)
    }

    @Test fun `빈 세션 묶음 — 한 건뿐이면 묶지 않고, 끄면 한 장씩`() {
        val one = listOf(rowOf(ptt("a", at(13, 1), 30)), rowOf(ptt("b", at(13), 30, turns = 2, rec = true)))
        assertEquals(listOf(RowKind.CARD, RowKind.CARD), bundleSilent(one, enabled = true).map { it.kind })
        assertTrue(bundleSilent(repeated(), enabled = false).all { it.kind == RowKind.CARD })
        assertEquals(9, bundleSilent(repeated(), enabled = false).size)
    }

    @Test fun `빈 세션 묶음 — 시간대·서비스·그룹·개시자가 다르면 이어 붙지 않는다`() {
        fun kinds(a: HistoryEntry, b: HistoryEntry) = bundleSilent(listOf(rowOf(a), rowOf(b)), enabled = true).map { it.kind }
        val base = ptt("a", at(13, 1), 30)
        assertEquals(listOf(RowKind.BUNDLE), kinds(base, ptt("b", at(13), 30)))
        assertEquals("시간대", listOf(RowKind.CARD, RowKind.CARD), kinds(base, ptt("b", at(12, 59), 30)))
        assertEquals("서비스", listOf(RowKind.CARD, RowKind.CARD), kinds(base, ptt("b", at(13), 30, service = "mcvideo")))
        assertEquals("그룹", listOf(RowKind.CARD, RowKind.CARD), kinds(base, ptt("b", at(13), 30, group = "g004")))
        assertEquals("개시자", listOf(RowKind.CARD, RowKind.CARD), kinds(base, ptt("b", at(13), 30, from = u1)))
        // 같은 사람의 다른 표기는 한 사람이다
        assertEquals(listOf(RowKind.BUNDLE), kinds(base, ptt("b", at(13), 30).copy(from = "sip:$u3@ptt.cims")))
    }

    @Test fun `빈 세션 묶음 — 펼치면 머리 아래에 한 줄씩, 펼침은 묶음마다`() {
        val lines = bundleSilent(repeated(), enabled = true, open = setOf("13|rep-5"))
        assertEquals(listOf(RowKind.BUNDLE, RowKind.MEMBER, RowKind.MEMBER, RowKind.MEMBER, RowKind.MEMBER, RowKind.CARD,
            RowKind.BUNDLE), lines.map { it.kind })
        assertTrue(lines[0].expanded)
        assertEquals("접기", lines[0].toggleText)
        assertFalse(lines[6].expanded)
        assertEquals("발언 0회", lines[1].memberStat)
        // 한 세션이 머리와 펼친 줄 두 곳에 서도 열쇠가 겹치지 않는다
        assertEquals(lines.size, lines.map { it.key }.toSet().size)
    }

    @Test fun `새 세션이 위에 붙어도 펼친 묶음은 펼친 채다`() {
        val more = (listOf(rowOf(ptt("rep-9", at(13, 29), 30))) + repeated())
        val lines = bundleSilent(more, enabled = true, open = setOf("13|rep-5"))
        assertEquals(5, lines[0].members.size)
        assertTrue("열쇠가 가장 이른 세션이라 이어진다", lines[0].expanded)
    }

    @Test fun `영상 세션의 묶음은 송출로 센다`() {
        val rows = listOf(rowOf(ptt("a", at(13, 1), 30, service = "mcvideo")), rowOf(ptt("b", at(13), 30, service = "mcvideo")))
        assertEquals("각 30초 · 송출 0회", bundleSilent(rows, enabled = true).single().statLine)
    }

    @Test fun `선택 표시 — 접힌 묶음은 안의 어느 세션이 골라져도 머리가 받고, 펼치면 그 줄이 받는다`() {
        val closed = bundleSilent(repeated(), enabled = true)
        assertTrue(closed[0].holds("rep-6"))
        assertFalse(closed[2].holds("rep-6"))
        assertFalse(closed[0].holds(null))
        val open = bundleSilent(repeated(), enabled = true, open = setOf("13|rep-5"))
        assertFalse("펼친 묶음의 머리는 받지 않는다", open[0].holds("rep-6"))
        assertEquals(listOf("rep-6"), open.filter { it.holds("rep-6") }.map { it.row.e.id })
    }

    // ── 시간대 묶음 · 요약 ──────────────────────────────────────────────────

    @Test fun `시간대 묶음 — 머리의 건수는 줄 수가 아니라 세션 수`() {
        val rows = repeated() + rowOf(ptt("early", at(9, 10), 160, turns = 7, rec = true))
        val items = groupByHour(bundleSilent(rows, enabled = true))
        val heads = items.filterIsInstance<ListItem.Hour>()
        assertEquals(listOf(13, 9), heads.map { it.hour })               // 최근이 위
        assertEquals(listOf(9, 1), heads.map { it.count })               // 묶음 머리 = 묶은 수
        assertEquals("13시", heads[0].title)
        assertEquals(3, items.filterIsInstance<ListItem.Line>().count { it.row.row.hour == 13 })
        // 펼쳐도 건수는 그대로다 — 펼친 한 줄은 머리가 이미 셌다
        val opened = groupByHour(bundleSilent(rows, enabled = true, open = setOf("13|rep-5")))
        assertEquals(9, opened.filterIsInstance<ListItem.Hour>().first().count)
        assertEquals(opened.size, opened.map { it.key }.toSet().size)
    }

    @Test fun `요약 — 건수·녹취·발화 합·빈 세션 n건 → m묶음`() {
        val list = historyListOf(repeated(), HistoryKind.PTT)
        assertEquals("2026-09-08 · 9건 · 녹취 1건 · 발화 합 3초 · 빈 세션 8건 → 2묶음",
            summaryOf(list, HistoryKind.PTT, day, hour = null, speechOnly = false))
        // 묶기를 끄면 묶음 수는 적지 않는다
        assertEquals("2026-09-08 · 9건 · 녹취 1건 · 발화 합 3초 · 빈 세션 8건",
            summaryOf(historyListOf(repeated(), HistoryKind.PTT, groupSilent = false), HistoryKind.PTT, day, null, false))
    }

    @Test fun `요약 — 시간대·발언 있는 세션·진행중·영상`() {
        val rows = listOf(
            rowOf(ptt("live", at(13, 50), 0, turns = 1, live = true)),
            rowOf(ptt("mcv", at(13, 5), 200, turns = 2, rec = true, service = "mcvideo", speechMs = 95_000)))
        assertEquals("2026-09-08 13시 발언 있는 세션 · 2건 · 진행중 1 · 녹취 1건 · 영상 1건",
            summaryOf(historyListOf(rows, HistoryKind.PTT), HistoryKind.PTT, day, hour = 13, speechOnly = true))
        // 통화에는 무전의 항(영상 세션·발화·빈 세션)이 없다
        val calls = listOf(rowOf(call(at(9), answer = at(9, 0, 5), end = at(9, 1), dur = 55)))
        assertEquals("2026-09-08 · 1건", summaryOf(historyListOf(calls, HistoryKind.CALL), HistoryKind.CALL, day, null, false))
    }

    // ── 거르기 ──────────────────────────────────────────────────────────────

    @Test fun `서비스 거르기 — 무전과 영상을 가른다`() {
        val all = listOf(ptt("a", at(9), 30, turns = 1), ptt("v", at(10), 30, turns = 1, service = "mcvideo"),
            ptt("old", at(11), 30, turns = 1, service = ""))
        fun ids(f: ServiceFilter) = filterRows(all, HistoryKind.PTT, f).map { it.e.id }
        assertEquals(listOf("a", "v", "old"), ids(ServiceFilter.ALL))
        assertEquals("서비스 축이 없는 항목은 무전으로 읽는다", listOf("a", "old"), ids(ServiceFilter.PTT))
        assertEquals(listOf("v"), ids(ServiceFilter.MCVIDEO))
    }

    @Test fun `발언 수 밴드의 칸을 누르면 그 시간대의 발언 있는 세션만`() {
        val all = listOf(ptt("spoke", at(13), 30, turns = 2), ptt("empty", at(13, 1), 30),
            ptt("rec-only", at(13, 2), 30, rec = true))                 // 스캔 폴백 — 발언 수 0 이지만 녹취가 있다
        assertEquals(listOf("spoke", "rec-only"), filterRows(all, HistoryKind.PTT, speechOnly = true).map { it.e.id })
        assertEquals(3, filterRows(all, HistoryKind.PTT).size)
    }

    @Test fun `발언 있는 세션만 — 발언 수 모드에서 시간대를 골랐을 때만 켜진다`() {
        assertTrue(HistoryUi(kind = HistoryKind.PTT, bandMode = BandMode.TURNS, hourFilter = 13).speechOnly)
        assertFalse(HistoryUi(kind = HistoryKind.PTT, bandMode = BandMode.TURNS).speechOnly)
        assertFalse(HistoryUi(kind = HistoryKind.PTT, bandMode = BandMode.SESSIONS, hourFilter = 13).speechOnly)
        assertFalse(HistoryUi(kind = HistoryKind.CALL, bandMode = BandMode.TURNS, hourFilter = 13).speechOnly)
    }

    // ── 시간대 밴드 ─────────────────────────────────────────────────────────

    @Test fun `세션 수 밴드 — 농도는 그날 가장 많은 칸 대비, 0 칸은 비운다`() {
        val band = IntArray(24).also { it[9] = 5; it[14] = 10 }
        val cells = sessionCells(band)
        assertEquals(24, cells.size)
        assertEquals(0.45f, cells[14].ratio, 0.001f)
        assertEquals(0.265f, cells[9].ratio, 0.001f)
        assertEquals(0f, cells[0].ratio, 0f)
        assertEquals("", cells[0].countText)
        assertEquals("10", cells[14].countText)
    }

    @Test fun `발언 수 밴드 — 세션의 발언 턴을 세션 시작 시간대에 더한다`() {
        val day = listOf(ptt("a", at(9, 10), 3000, turns = 3), ptt("b", at(9, 40), 30, turns = 4), ptt("c", at(14), 30, turns = 2))
        val band = IntArray(24).also { it[9] = 2; it[14] = 1 }
        val cells = turnCells(day, band)
        assertEquals("09:10 에 시작해 10시에 끝난 세션도 9시에 센다", 7, cells[9].count)
        assertEquals(2, cells[14].count)
        assertFalse(cells[9].partial)
        assertEquals(0.45f, cells[9].ratio, 0.001f)
        assertEquals("7", cells[9].countText)
    }

    @Test fun `발언 수 밴드 — 하루 상한을 넘어 덜 센 시간대는 더하기 표시`() {
        val day = listOf(ptt("a", at(9, 10), 30, turns = 3), ptt("b", at(9, 40), 30, turns = 4))
        val band = IntArray(24).also { it[9] = 5; it[3] = 4 }          // 서버 hours = 절삭 전 세션 수
        val cells = turnCells(day, band)
        assertTrue(cells[9].partial)
        assertEquals("7+", cells[9].countText)
        assertTrue("받은 세션이 하나도 없는 시간대", cells[3].partial)
        assertEquals("0+", cells[3].countText)
        // 그 시간대를 좁혀 다시 받았으면 그 결과로 센다
        val hour9 = (1..5).map { ptt("h$it", at(9, it), 30, turns = it) }
        val again = turnCells(day, band, mapOf(9 to hour9))
        assertEquals(15, again[9].count)
        assertFalse(again[9].partial)
    }

    // 치다 만 국내 표기(`010333`)는 정규형으로 올릴 수 없다 — 번호의 어느 표기에든 들었으면 걸린다(주소록 제안과 같은 규칙)
    @Test fun `검색 — 치다 만 국내 표기도 E164 번호에 걸린다`() {
        val r = rowOf(HistoryEntry(id = "s1", atMs = at(9), kind = HistoryKind.CALL, from = "tel:+821033334444", to = "tel:1001"))
        assertTrue(com.cims.ue.dispatch.ui.history.matches(r, "010333"))
        assertTrue(com.cims.ue.dispatch.ui.history.matches(r, "010-3333-4444"))
        assertTrue(com.cims.ue.dispatch.ui.history.matches(r, "+82103333"))
        assertTrue(com.cims.ue.dispatch.ui.history.matches(r, "010.3333.4444"))
        assertFalse(com.cims.ue.dispatch.ui.history.matches(r, "010999"))
        // 이름에 든 숫자 하나로 온 번호가 걸리지 않는다
        assertFalse(com.cims.ue.dispatch.ui.history.matches(r, "순찰3"))
    }
}
