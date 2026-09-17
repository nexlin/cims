// [이력] 화면의 순수 로직 — 날짜 창·시간대 밴드·검색·발언 막대 (dispatch_desktop_ui.md §4.6)
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.RecordingInfo
import com.cims.ue.dispatch.session.RecordingSegment
import com.cims.ue.dispatch.session.SegmentTrack
import com.cims.ue.dispatch.session.SpeakerSpan
import com.cims.ue.dispatch.ui.history.HistoryViewModel
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.LocalDate
import java.time.ZoneId

class HistoryLogicTest {

    private fun at(h: Int, m: Int = 0): Long =
        LocalDate.of(2026, 9, 15).atTime(h, m).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli()

    private fun entry(id: String, hour: Int, from: String = "", group: String = "",
                      people: List<String> = emptyList()) =
        HistoryEntry(id = id, atMs = at(hour), kind = HistoryKind.CALL, from = from, group = group,
                     people = people)

    @Test fun `날짜 창은 그날 0시부터 다음날 0시까지 — 서버 48시간 상한 안`() {
        val (from, to) = HistoryViewModel.dayWindow(LocalDate.of(2026, 9, 15))
        assertEquals(at(0), from)
        assertEquals(24 * 3600_000L, to - from)
    }

    @Test fun `시간대 밴드 — 서버 hours 가 정본`() {
        val band = HistoryViewModel.bandOf(listOf(entry("a", 9)), mapOf("09" to 5, "23" to 1, "bad" to 3))
        assertEquals(5, band[9])
        assertEquals(1, band[23])
        assertEquals(0, band[0])
    }

    @Test fun `시간대 밴드 — hours 가 없으면 항목에서 센다`() {
        val band = HistoryViewModel.bandOf(listOf(entry("a", 9), entry("b", 9), entry("c", 14)), emptyMap())
        assertEquals(2, band[9])
        assertEquals(1, band[14])
    }

    @Test fun `검색 — 상대·그룹·참여자에 걸린다`() {
        val e = entry("a", 9, from = "tel:+821011112222", group = "tel:g003",
                      people = listOf("tel:1001", "김순경"))
        assertTrue(HistoryViewModel.matches(e, ""))
        assertTrue(HistoryViewModel.matches(e, "1111"))
        assertTrue(HistoryViewModel.matches(e, "g003"))
        assertTrue(HistoryViewModel.matches(e, "김순경"))
        assertFalse(HistoryViewModel.matches(e, "없는번호"))
    }

    @Test fun `발언 막대 — 트랙의 화자 구간이 턴의 원자다`() {
        val rec = RecordingInfo(
            id = "r", startAtMs = at(9),
            segments = listOf(
                RecordingSegment(seq = 1, startAtMs = at(9), durationMs = 4000, tracks = listOf(
                    SegmentTrack(0, speakers = listOf(SpeakerSpan("1001", 0, 4000))),
                    SegmentTrack(1, speakers = listOf(SpeakerSpan("1002", 1000, 900))))),
                RecordingSegment(seq = 2, startAtMs = at(9, 1), durationMs = 2000, speakerId = "1003")))

        val turns = HistoryViewModel.turnsOf(rec)
        assertEquals(3, turns.size)
        // 세션 시작 기준으로 옮겨 한 축에 놓는다
        assertEquals(listOf(0, 1000, 60_000), turns.map { it.offsetMs })
        assertEquals(listOf("1001", "1002", "1003"), turns.map { it.speaker })
        // 재생은 (seq, slot) 로 건다 — 트랙 없는 세그먼트는 믹스(null)
        assertEquals(listOf(0, 1, null), turns.map { it.slot })
    }

    @Test fun `발언 막대 — 시작 시각이 없으면 빈 목록`() {
        assertTrue(HistoryViewModel.turnsOf(RecordingInfo(id = "r")).isEmpty())
    }

    @Test fun `문구 사전 — 데스크톱과 같은 문장`() {
        assertEquals("무응답", HistoryViewModel.endReasonText("no_answer"))
        assertEquals("비정상 종료", HistoryViewModel.endReasonText("incomplete"))
        assertEquals("알수없음", HistoryViewModel.endReasonText("알수없음"))   // 모르는 값은 그대로
        assertEquals("1:1", HistoryViewModel.sessionKindText("private"))
        assertEquals("영상", HistoryViewModel.callTypeText("volte_video"))
        assertEquals("발언권 회수", HistoryViewModel.floorOpText("revoke"))
        assertEquals("입장", HistoryViewModel.eventTypeText("member_join"))
        assertEquals("대기열 가득참", HistoryViewModel.denyReasonText("queue_full"))
    }
}

/**
 * 발언 타임라인의 픽셀 위치 — **긴 세션에서 Int 오버플로로 죽지 않아야 한다.**
 *
 * `widthDp * offsetMs` 를 Int 로 곱하면 640 × 3,360,000(56분)에서 이미 Int.MAX 를 넘어 음수가 된다.
 * 음수 padding 은 Compose 가 예외로 거절하므로, 한 시간 넘는 세션을 **여는 것만으로** 앱이 죽었다.
 */
class TimelinePosTest {

    private val W = 640

    @Test fun `한 시간 넘는 세션에서도 음수가 되지 않는다`() {
        listOf(30, 56, 60, 120, 480).forEach { minutes ->
            val off = minutes * 60 * 1000
            val x = com.cims.ue.dispatch.ui.history.barPos(W, off, off + 3000)
            assertTrue("$minutes 분 지점이 음수다: $x", x >= 0)
            assertTrue("$minutes 분 지점이 폭을 넘었다: $x", x <= W)
        }
    }

    @Test fun `비율이 맞는다`() {
        assertEquals(0, com.cims.ue.dispatch.ui.history.barPos(W, 0, 1000))
        assertEquals(W / 2, com.cims.ue.dispatch.ui.history.barPos(W, 500, 1000))
        assertEquals(W, com.cims.ue.dispatch.ui.history.barPos(W, 1000, 1000))
    }

    @Test fun `전체 길이가 0 이어도 죽지 않는다`() {
        assertEquals(0, com.cims.ue.dispatch.ui.history.barPos(W, 100, 0))
        assertEquals(0, com.cims.ue.dispatch.ui.history.barPos(W, 100, -5))
    }

    @Test fun `값이 전체를 넘어도 폭 안으로 가둔다`() {
        assertEquals(W, com.cims.ue.dispatch.ui.history.barPos(W, 5000, 1000))
    }
}
