// [이력] 화면의 순수 로직 — 날짜 창·시간대 밴드·검색·발언 턴·문구 사전 (dispatch_desktop_ui.md §4.6)
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.RecordingInfo
import com.cims.ue.dispatch.session.RecordingSegment
import com.cims.ue.dispatch.session.SegmentTrack
import com.cims.ue.dispatch.session.SpeakerSpan
import com.cims.ue.dispatch.ui.history.HistoryNames
import com.cims.ue.dispatch.ui.history.HistoryViewModel
import com.cims.ue.dispatch.ui.history.axisStepSec
import com.cims.ue.dispatch.ui.history.denyReasonText
import com.cims.ue.dispatch.ui.history.endReasonText
import com.cims.ue.dispatch.ui.history.eventTypeText
import com.cims.ue.dispatch.ui.history.floorOpText
import com.cims.ue.dispatch.ui.history.fmtDur
import com.cims.ue.dispatch.ui.history.fmtSpeech
import com.cims.ue.dispatch.ui.history.matches
import com.cims.ue.dispatch.ui.history.mmss
import com.cims.ue.dispatch.ui.history.rowOf
import com.cims.ue.dispatch.ui.history.turnsOf
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
        val row = rowOf(entry("a", 9, from = "tel:+821011112222", group = "tel:g003", people = listOf("tel:1001", "김순경")))
        assertTrue(matches(row, ""))
        assertTrue(matches(row, "1111"))
        assertTrue(matches(row, "g003"))
        assertTrue(matches(row, "김순경"))
        assertFalse(matches(row, "없는번호"))
    }

    @Test fun `검색 — 주소록 이름과 번호의 다른 표기에도 걸린다`() {
        val names = HistoryNames(who = { if (it.contains("11112222")) "박현장" else it })
        val row = rowOf(entry("a", 9, from = "tel:+821011112222"), names)
        assertTrue("이름으로 찾는다", matches(row, "박현장"))
        assertTrue("국내 표기로 쳐도 E.164 로 저장된 번호에 걸린다", matches(row, "01011112222"))
        assertFalse(matches(row, "01099998888"))
    }

    @Test fun `발언 턴 — 트랙의 화자 구간이 턴의 원자다`() {
        val rec = RecordingInfo(
            id = "r", startAtMs = at(9),
            segments = listOf(
                RecordingSegment(seq = 1, startAtMs = at(9), durationMs = 4000, tracks = listOf(
                    SegmentTrack(0, speakers = listOf(SpeakerSpan("1001", 0, 4000))),
                    SegmentTrack(1, speakers = listOf(SpeakerSpan("1002", 1000, 900))))),
                RecordingSegment(seq = 2, startAtMs = at(9, 1), durationMs = 2000, speakerId = "1003")))

        val turns = turnsOf(rec)
        assertEquals(3, turns.size)
        // 벽시계 시각으로 한 축에 놓는다
        assertEquals(listOf(0L, 1000L, 60_000L), turns.map { it.startMs - at(9) })
        assertEquals(listOf(4000, 900, 2000), turns.map { it.durMs })
        assertEquals(listOf("1001", "1002", "1003"), turns.map { it.speaker })
        assertEquals(listOf(0, 1, 0), turns.map { it.slot })
        // 세그먼트에 턴이 여럿이면(동시 발언) multi — 트랙 없는 세그먼트는 그 자체가 한 턴이다
        assertEquals(listOf(true, true, false), turns.map { it.multi })
    }

    @Test fun `발언 턴 — 음성 없이 영상만 있는 송출 구간은 영상 트랙에서 읽는다`() {
        val rec = RecordingInfo(id = "r", segments = listOf(
            RecordingSegment(seq = 1, type = "mcvideo", startAtMs = at(9), durationMs = 5000, tracks = listOf(
                SegmentTrack(0, "audio", listOf(SpeakerSpan("1001", 0, 5000))),
                SegmentTrack(0, "video", listOf(SpeakerSpan("1001", 0, 5000))))),
            RecordingSegment(seq = 2, type = "mcvideo", startAtMs = at(9, 1), durationMs = 3000, speakerId = "1002", tracks = listOf(
                SegmentTrack(0, "video", listOf(SpeakerSpan("", 0, 3000)))))))
        val turns = turnsOf(rec)
        assertEquals("음성 트랙이 있으면 영상 트랙은 따로 세지 않는다", 2, turns.size)
        assertEquals("화자가 비면 세그먼트의 대표 화자", listOf("1001", "1002"), turns.map { it.speaker })
    }

    @Test fun `발언 턴 — 녹음 중인 세그먼트는 아직 틀 수 없고 시작 시각이 없으면 뺀다`() {
        val rec = RecordingInfo(id = "r", segments = listOf(
            RecordingSegment(seq = 1, startAtMs = at(9), durationMs = 1000, speakerId = "1001", status = "recording"),
            RecordingSegment(seq = 2, durationMs = 1000, speakerId = "1002")))
        val turns = turnsOf(rec)
        assertEquals(1, turns.size)
        assertFalse(turns[0].playable)
        assertTrue(turnsOf(RecordingInfo(id = "r")).isEmpty())
    }

    @Test fun `문구 사전 — 데스크톱과 같은 문장`() {
        assertEquals("무응답", endReasonText("no_answer"))
        assertEquals("비정상 종료(기록 없음)", endReasonText("incomplete"))
        assertEquals("—", endReasonText(""))
        assertEquals("알수없음", endReasonText("알수없음"))   // 모르는 값은 그대로
        assertEquals("회수 통지", floorOpText("revoke"))
        assertEquals("발언 종료", floorOpText("RELEASE"))
        assertEquals("입장", eventTypeText("member_join"))
        assertEquals("수신전용(ambient)", denyReasonText("recv_only"))
    }

    @Test fun `길이·발화 시간·재생 위치 표기`() {
        assertEquals("—", fmtDur(0))
        assertEquals("42초", fmtDur(42))
        assertEquals("2분 40초", fmtDur(160))
        assertEquals("0초", fmtSpeech(0))
        assertEquals("3.5초", fmtSpeech(3500))
        assertEquals("9초", fmtSpeech(9000))
        assertEquals("12초", fmtSpeech(12_400))
        assertEquals("1분 1초", fmtSpeech(61_000))
        assertEquals("00:30", mmss(30_000))
        assertEquals("02:40", mmss(160_000))
        assertEquals("음수는 0 으로", "00:00", mmss(-5))
    }

    // ── 타임라인 배율 — 눈금 간격(데스크톱 `RebuildAxisTicks` 와 같은 규칙) ──
    @Test fun `배율을 올리면 눈금이 촘촘해진다 — 대여섯 개가 보이게`() {
        val hour = 3_600_000
        assertEquals(600, axisStepSec(hour, 1f))     // 1시간 ÷ 6 = 10분
        assertEquals(10, axisStepSec(hour, 64f))     // ×64 → 9.4초 → 10초
        assertEquals(1, axisStepSec(3_000, 1f))      // 짧은 세션은 1초
        assertEquals(3600, axisStepSec(100 * hour, 1f))  // 가장 큰 간격에서 멈춘다
        assertEquals("×1 아래로는 내려가지 않는다", axisStepSec(hour, 1f), axisStepSec(hour, 0.5f))
    }
}
