// 무전 세션 패널의 순수 로직 — 틈 줄임 시간축·눈금·세션 패널(레인·참여자·숫자 칸)·이벤트 묶기·floor 부가 정보
// (android_dispatch_tablet.md §6.11, dispatch_desktop_ui.md §4.6)
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.PttEvent
import com.cims.ue.dispatch.session.PttFloorEvent
import com.cims.ue.dispatch.session.PttParticipant
import com.cims.ue.dispatch.session.PttSessionDetail
import com.cims.ue.dispatch.session.RecordingInfo
import com.cims.ue.dispatch.session.RecordingSegment
import com.cims.ue.dispatch.session.SegmentTrack
import com.cims.ue.dispatch.session.SpeakerSpan
import com.cims.ue.dispatch.ui.history.DotTone
import com.cims.ue.dispatch.ui.history.TimeAxis
import com.cims.ue.dispatch.ui.history.TimelineItem
import com.cims.ue.dispatch.ui.history.buildSessionPane
import com.cims.ue.dispatch.ui.history.floorDetail
import com.cims.ue.dispatch.ui.history.rowOf
import com.cims.ue.dispatch.ui.history.timelineShown
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.LocalDate
import java.time.ZoneId

class HistoryAxisTest {

    private val zone: ZoneId = ZoneId.systemDefault()
    private val t0: Long = LocalDate.of(2026, 9, 8).atTime(9, 10).atZone(zone).toInstant().toEpochMilli()
    private fun s(sec: Int): Long = t0 + sec * 1000L

    /** 160초 세션 — 발언 2~14 · 20~45(안에 28~38 동시 발언) · 60~84 · 100~150. 발언 시간 합 111초. */
    private val turns = listOf(s(2) to s(14), s(20) to s(45), s(28) to s(38), s(60) to s(84), s(100) to s(150))
    private val span = 160_000.0

    // ── 틈 줄임 ─────────────────────────────────────────────────────────────

    @Test fun `접는 상한 — 발언 시간 합의 5퍼센트, 1초 반에서 15초 사이`() {
        assertEquals(5_550.0, TimeAxis.gapCapMs(111_000.0), 0.001)
        assertEquals("짧은 세션은 아래로 1.5초", 1_500.0, TimeAxis.gapCapMs(10_000.0), 0.001)
        assertEquals("긴 세션은 위로 15초", 15_000.0, TimeAxis.gapCapMs(1_000_000.0), 0.001)
    }

    @Test fun `틈 줄임 — 상한보다 긴 말 없는 구간만 접는다`() {
        val axis = TimeAxis.build(t0, span, turns, compact = true)
        assertTrue(axis.compacted)
        // 0~2초 틈은 상한(5.55초)보다 짧아 그대로다. 14~20 · 45~60 · 84~100 · 150~160(세션 꼬리) 넷을 접는다.
        assertEquals(listOf(s(14) to s(20), s(45) to s(60), s(84) to s(100), s(150) to s(160)), axis.gaps.map { it.fromMs to it.toMs })
        // 접힌 틈은 모두 같은 폭(상한)이다
        val w = axis.gaps.map { it.width }
        assertTrue(w.all { kotlin.math.abs(it - w[0]) < 1e-4f })
        assertEquals(5.55f / 135.2f, w[0], 1e-4f)                        // 표시 길이 = 2+12+5.55+25+5.55+24+5.55+50+5.55
    }

    @Test fun `틈 줄임 — 발언 막대 길이끼리는 여전히 시간에 비례한다`() {
        val axis = TimeAxis.build(t0, span, turns, compact = true)
        val first = axis.ratioOf(s(14)) - axis.ratioOf(s(2))            // 12초
        val last = axis.ratioOf(s(150)) - axis.ratioOf(s(100))          // 50초
        assertEquals(12f / 50f, first / last, 1e-3f)
        // 말한 구간이 넓어졌다 — 선형 축에서보다 크다
        assertTrue(last > 50f / 160f)
        assertEquals(0f, axis.ratioOf(t0), 0f)
        assertEquals(1f, axis.ratioOf(s(160)), 1e-4f)
    }

    @Test fun `틈 줄임 — 위치와 시각은 서로의 역이다`() {
        val axis = TimeAxis.build(t0, span, turns, compact = true)
        listOf(0, 1, 2, 10, 17, 30, 50, 70, 90, 120, 155, 160).forEach { sec ->
            assertEquals("${sec}초", s(sec).toDouble(), axis.timeAt(axis.ratioOf(s(sec))).toDouble(), 20.0)
        }
        // 축 밖은 끝으로 가둔다
        assertEquals(0f, axis.ratioOf(t0 - 5_000), 0f)
        assertEquals(1f, axis.ratioOf(s(500)), 1e-4f)
        assertEquals(t0, axis.timeAt(-1f))
        assertEquals(s(160), axis.timeAt(2f))
    }

    @Test fun `틈 줄임을 끄면 시간에 비례한 축`() {
        val axis = TimeAxis.build(t0, span, turns, compact = false)
        assertFalse(axis.compacted)
        assertTrue(axis.gaps.isEmpty())
        assertEquals(0.5f, axis.ratioOf(s(80)), 1e-4f)
        assertEquals(s(40), axis.timeAt(0.25f))
    }

    @Test fun `겹치거나 0점3초 안에 이어진 발언은 한 덩어리다 — 사이를 틈으로 보지 않는다`() {
        val close = listOf(t0 to t0 + 1_000, t0 + 1_200 to t0 + 2_000)
        assertFalse(TimeAxis.build(t0, 2_000.0, close, compact = true).compacted)
        // 발언이 하나도 없으면 접을 기준이 없다 — 선형이다
        assertFalse(TimeAxis.build(t0, 600_000.0, emptyList(), compact = true).compacted)
    }

    @Test fun `긴 세션에서도 위치가 0과 1 사이다`() {
        val hours8 = 8 * 3_600_000L
        val long = listOf(t0 + 10_000 to t0 + 20_000, t0 + hours8 - 30_000 to t0 + hours8 - 5_000)
        listOf(true, false).forEach { compact ->
            val axis = TimeAxis.build(t0, hours8.toDouble(), long, compact)
            listOf(0L, 15_000L, hours8 / 2, hours8 - 10_000, hours8).forEach { off ->
                val r = axis.ratioOf(t0 + off)
                assertTrue("compact=$compact off=$off → $r", r in 0f..1f)
            }
        }
    }

    // ── 눈금 ────────────────────────────────────────────────────────────────

    @Test fun `눈금 — 선형 축은 벽시계의 간격 배수에`() {
        val ticks = TimeAxis.build(t0, span, turns, compact = false).ticks(1f, zone)
        // 160초 ÷ 6 = 26.7초 → 30초 간격. 09:10:00 에 시작했으니 09:10:30 부터.
        assertEquals(listOf("09:10:30", "09:11:00", "09:11:30", "09:12:00", "09:12:30"), ticks.map { it.label })
        assertEquals(30f / 160f, ticks[0].ratio, 1e-4f)
        // 1분 이상 간격이면 초를 적지 않는다
        val hour = TimeAxis.build(t0, 3_600_000.0, emptyList(), compact = false).ticks(1f, zone)
        assertEquals("09:20", hour.first().label)
    }

    @Test fun `눈금 — 틈을 줄인 축은 접힌 틈 뒤 발언 덩어리가 시작하는 시각`() {
        val ticks = TimeAxis.build(t0, span, turns, compact = true).ticks(1f, zone)
        assertEquals(listOf("09:10:00", "09:10:20", "09:11:00", "09:11:40"), ticks.map { it.label })
        assertEquals(0f, ticks[0].ratio, 0f)
        assertTrue(ticks.zipWithNext().all { (a, b) -> b.ratio > a.ratio })
    }

    // ── 세션 패널 ───────────────────────────────────────────────────────────

    private val u1 = "+821310002001"
    private val u2 = "+821310002002"
    private val u3 = "+821310002003"
    private val u4 = "+821310002004"

    private fun session(service: String = "ptt", turns: Int = 7) = HistoryEntry(
        id = "ses-1", atMs = s(160), kind = HistoryKind.PTT, from = "tel:$u1", group = "tel:g002", durationSec = 160,
        recordingId = "ptt/1/ses-1", hasRecording = true, state = "ended", sessionKind = "group", startAtMs = t0, endAtMs = s(160),
        groupName = "1팀 무전", turnCount = turns, hasTurnCount = true, totalSpeechMs = 61_000, talkMs = 66_000, maxConcurrent = 2,
        floorControl = "on", floorPolicy = "dual", maxTalkers = 2, service = service)

    private val rec = RecordingInfo(id = "ptt/1/ses-1", startAtMs = t0, status = "ready", segments = listOf(
        RecordingSegment(1, "ptt", u1, s(2), s(14), 12_000, status = "ready"),
        RecordingSegment(2, "ptt", u2, s(20), s(45), 25_000, status = "ready", tracks = listOf(
            SegmentTrack(0, "audio", listOf(SpeakerSpan(u2, 0, 25_000))),
            SegmentTrack(1, "audio", listOf(SpeakerSpan(u3, 8_000, 10_000))))),
        RecordingSegment(3, "ptt", u1, s(60), s(84), 24_000, status = "ready"),
        RecordingSegment(4, "ptt", u3, s(100), s(150), 50_000, status = "ready")))

    private val detail = PttSessionDetail(
        recordingId = "ptt/1/ses-1",
        participants = listOf(PttParticipant(u1, "initiator", t0, s(160)), PttParticipant(u2, "member", s(1), s(160)),
            PttParticipant(u4, "member", s(30), s(90))),
        events = listOf(PttEvent(t0, "session_start"), PttEvent(t0, "member_join", u1, "initiator"),
            PttEvent(s(1), "member_join", u2, "member"), PttEvent(s(1), "member_join", u3, "member"),
            PttEvent(s(1), "member_join", u4, "member"), PttEvent(s(160), "session_end", durationSec = 160)),
        floor = listOf(PttFloorEvent(s(2), "GRANT", u1, slot = 0, talkers = 1), PttFloorEvent(s(14), "RELEASE", u1, slot = 0)),
        hasRecording = true)

    @Test fun `세션 패널 — 화자 레인은 처음 말한 차례, 참여자는 입퇴장 기록과 화자의 합`() {
        val pane = buildSessionPane(rowOf(session()), detail, rec)
        assertEquals(listOf(u1, u2, u3), pane.lanes.map { it.speaker })
        assertEquals(listOf(0, 1, 2), pane.lanes.map { it.color })
        assertEquals(listOf(2, 1, 2), pane.lanes.map { it.bars.size })
        // 참여자 — 기록의 셋(u1·u2·u4) + 기록에 없지만 말한 u3
        assertEquals(listOf(u1, u2, u4, u3), pane.participants.map { it.id })
        val by = pane.participants.associateBy { it.id }
        assertTrue(by.getValue(u1).initiator)
        assertEquals(2, by.getValue(u1).turns)
        assertEquals(0, by.getValue(u1).lane)
        assertFalse("참가만 한 사람은 발언 0", by.getValue(u4).spoke)
        assertEquals(0, by.getValue(u4).turns)
        assertEquals(2, by.getValue(u3).lane)
        assertEquals("09:10:30 ~ 09:11:30 · 발언 0회 · 말한 시간 0초", by.getValue(u4).tip)
    }

    @Test fun `세션 패널 — 숫자 칸은 길이·참여·발언·말한 시간, 있을 때만 동시 발언과 녹취`() {
        val pane = buildSessionPane(rowOf(session()), detail, rec)
        assertEquals(listOf("길이", "참여", "발언", "말한 시간", "최대 동시 발언", "녹취"), pane.metrics.map { it.key })
        assertEquals(listOf("2분 40초", "4", "7", "1분 1초", "2", "4"), pane.metrics.map { it.value })
        assertEquals("발언", pane.turnWord)
        assertEquals(2, pane.floorCount)
        assertEquals(6, pane.memberEventCount)
        assertEquals("09:10:00", pane.axisStartText)
        assertEquals("09:12:40", pane.axisEndText)
    }

    @Test fun `세션 패널 — 막대 위치는 축을 따른다, 틈 줄임을 끄면 시간 비례`() {
        val linear = buildSessionPane(rowOf(session()), detail, rec, compact = false)
        val bar = linear.lanes[0].bars[0]                                // u1 의 2~14초
        assertEquals(2f / 160f, bar.left, 1e-4f)
        assertEquals(12f / 160f, bar.width, 1e-4f)
        val compact = buildSessionPane(rowOf(session()), detail, rec, compact = true)
        assertTrue("틈을 줄이면 같은 발언이 넓게 보인다", compact.lanes[0].bars[0].width > bar.width)
        assertTrue(compact.axis.compacted)
    }

    @Test fun `세션 패널 — 영상 세션은 낱말만 바뀐다(발언 → 송출)`() {
        val pane = buildSessionPane(rowOf(session(service = "mcvideo", turns = 2)), detail, rec)
        assertTrue(pane.video)
        assertEquals("송출", pane.turnWord)
        assertEquals(listOf("길이", "참여", "송출", "보낸 시간", "최대 동시 송출", "녹취"), pane.metrics.map { it.key })
        assertEquals("송출", pane.participants.first().word)
        assertEquals("이 세션에는 녹취된 송출이 없습니다", pane.noTurnsText)
    }

    @Test fun `세션 패널 — 항목에 서비스 축이 없어도 녹취 메타로 영상 세션임을 안다`() {
        val videoRec = rec.copy(service = "mcvideo")
        val pane = buildSessionPane(rowOf(session(service = "")), detail, videoRec)
        assertTrue(pane.video)
        assertEquals("송출", pane.turnWord)
        assertFalse(buildSessionPane(rowOf(session(service = "")), detail, rec).video)
    }

    @Test fun `세션 패널 — 상세·녹취가 오기 전에도 머리와 숫자 칸은 선다`() {
        val pane = buildSessionPane(rowOf(session()), null, null, status = "세션 상세 조회 중…")
        assertTrue(pane.lanes.isEmpty())
        assertEquals(listOf("길이", "참여", "발언", "말한 시간", "최대 동시 발언"), pane.metrics.map { it.key })
        assertEquals("세션 상세 조회 중…", pane.status)
        // 녹취 식별자조차 없으면 세션 기록이 없는 것이다
        val none = buildSessionPane(rowOf(session().copy(recordingId = "", hasRecording = false)), null, null)
        assertEquals("세션 기록 없음", none.status)
    }

    // ── 이벤트 ──────────────────────────────────────────────────────────────

    @Test fun `이벤트 — 발언권과 입퇴장을 시간순으로, 같은 초의 같은 입퇴장은 한 줄`() {
        val pane = buildSessionPane(rowOf(session()), detail, rec)
        val all = timelineShown(pane.timeline, floor = true, member = true)
        assertEquals(listOf("세션 시작", "입장", "입장", "발언권 부여", "발언 종료", "세션 종료"), all.map { it.text })
        // 개시자의 입장은 «개시자» 가 붙어 따로 서고, 같은 초의 세 사람은 한 줄로
        assertEquals("개시자", all[1].detail)
        assertEquals("$u2 외 2명", all[2].who)
        assertEquals("2분 40초", all.last().detail)
    }

    @Test fun `이벤트 — 층 토글`() {
        val pane = buildSessionPane(rowOf(session()), detail, rec)
        assertEquals(listOf("발언권 부여", "발언 종료"), timelineShown(pane.timeline, floor = true, member = false).map { it.text })
        assertTrue(timelineShown(pane.timeline, floor = false, member = true).none { it.floor })
        assertTrue(timelineShown(pane.timeline, floor = false, member = false).isEmpty())
    }

    @Test fun `이벤트 — 다른 초이거나 부가 정보가 있으면 묶지 않는다`() {
        fun item(sec: Int, who: String, detail: String = "") =
            TimelineItem(s(sec), false, DotTone.HELD, who, "입장", detail)
        assertEquals(2, timelineShown(listOf(item(1, "A"), item(2, "B")), floor = true, member = true).size)
        assertEquals(2, timelineShown(listOf(item(1, "A"), item(1, "B", "개시자")), floor = true, member = true).size)
        assertEquals("A 외 1명", timelineShown(listOf(item(1, "A"), item(1, "B")), floor = true, member = true).single().who)
    }

    @Test fun `floor 부가 정보 — 사유·대기 순번·회수 유예·동시 발언`() {
        assertEquals("수신전용(ambient) · 발언 중 $u2", floorDetail(PttFloorEvent(t0, "DENY", u4, reason = "recv_only", owner = "tel:$u2")))
        assertEquals("대기 1/3", floorDetail(PttFloorEvent(t0, "QUEUE", u3, pos = 1, qsize = 3)))
        assertEquals("대기 2번", floorDetail(PttFloorEvent(t0, "QUEUE", u3, pos = 2)))
        assertEquals("3초 후 회수", floorDetail(PttFloorEvent(t0, "REVOKE", u3, graceSec = 3)))
        assertEquals("동시 2명 · 슬롯 1", floorDetail(PttFloorEvent(t0, "GRANT", u3, slot = 1, talkers = 2)))
        assertEquals("선점 — $u1 회수", floorDetail(PttFloorEvent(t0, "GRANT", u3, slot = 0, talkers = 1, preempt = true, preemptedFrom = u1)))
        assertEquals("무음 0.3초", floorDetail(PttFloorEvent(t0, "RELEASE", u1, idleMs = 300)))
        assertEquals("", floorDetail(PttFloorEvent(t0, "RELEASE", u1)))
    }
}
