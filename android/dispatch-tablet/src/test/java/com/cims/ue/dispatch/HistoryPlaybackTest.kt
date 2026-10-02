// 녹취 재생 바의 순수 로직 — 막대의 한 점(벽시계 시각) ↔ 세그먼트 + 파일 안 오프셋, 발언 막대, 이전·다음 발언
// (android_dispatch_tablet.md §6.11, dispatch_desktop_ui.md §4.6 «녹취 재생 바»)
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.RecordingInfo
import com.cims.ue.dispatch.session.RecordingSegment
import com.cims.ue.dispatch.session.SegmentTrack
import com.cims.ue.dispatch.session.SpeakerSpan
import com.cims.ue.dispatch.ui.history.HistoryNames
import com.cims.ue.dispatch.ui.history.PlayerUi
import com.cims.ue.dispatch.ui.history.buildSessionPane
import com.cims.ue.dispatch.ui.history.playerBarOf
import com.cims.ue.dispatch.ui.history.rowOf
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.LocalDate
import java.time.ZoneId

class HistoryPlaybackTest {

    private val t0: Long = LocalDate.of(2026, 9, 8).atTime(9, 10).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli()
    private fun s(sec: Int): Long = t0 + sec * 1000L

    private val u1 = "+821310002001"
    private val u2 = "+821310002002"
    private val u3 = "+821310002003"
    private val names = HistoryNames(who = { mapOf(u1 to "김관제", u2 to "이당직", u3 to "박현장")[it.substringAfter(':')] ?: it })

    // ── 통화 — 막대 = 첫 세그먼트 시작 ~ 마지막 끝 ─────────────────────────

    private val call = HistoryEntry(id = "c1", atMs = s(80), kind = HistoryKind.CALL, from = "tel:$u1", to = "tel:$u2",
        durationSec = 80, recordingId = "volte/c1.d", hasRecording = true, state = "ended", callType = "volte",
        inviteAtMs = t0 - 5_000, answerAtMs = t0, endAtMs = s(80), endReason = "normal")

    /** 두 세그먼트 — 0~42초, 60~80초. 그 사이 18초는 녹취가 없다(보류). */
    private val callRec = RecordingInfo(id = "volte/c1.d", startAtMs = t0, segments = listOf(
        RecordingSegment(1, "volte", startAtMs = t0, durationMs = 42_000),
        RecordingSegment(2, "volte", startAtMs = s(60), durationMs = 20_000)))

    private val bar = playerBarOf(rowOf(call), callRec)

    @Test fun `통화 막대 — 첫 세그먼트 시작부터 마지막 끝까지`() {
        assertEquals(t0, bar.t0)
        assertEquals(80_000L, bar.spanMs)
        assertEquals(s(80), bar.endMs)
        assertEquals(listOf(1, 2), bar.segs.map { it.seq })
        assertFalse(bar.ptt)
        assertTrue(bar.blocks.isEmpty())
    }

    @Test fun `막대의 한 점 — 그 시각을 담은 세그먼트와 파일 안 오프셋으로 푼다`() {
        val hit = bar.locate(s(10), wantPlay = true, skipGaps = true)
        assertEquals(1, hit.seg?.seq)
        assertEquals(10_000, hit.offsetMs)
        assertEquals(s(10), hit.atMs)
        val second = bar.locate(s(65), wantPlay = false, skipGaps = false)
        assertEquals(2, second.seg?.seq)
        assertEquals(5_000, second.offsetMs)
        // 세그먼트의 끝 시각은 그 세그먼트에 들지 않는다 — 다음으로 넘어가는 경계다
        assertNull(bar.segAt(s(42)))
        assertEquals(1, bar.segAt(s(42) - 1)?.seq)
    }

    @Test fun `말 없는 구간을 누르면 — 재생 중이고 건너뛰기면 다음 세그먼트의 시작으로`() {
        val skip = bar.locate(s(50), wantPlay = true, skipGaps = true)
        assertEquals(2, skip.seg?.seq)
        assertEquals(s(60), skip.atMs)
        assertEquals(0, skip.offsetMs)
        // 건너뛰기를 끄면 헤드만 그 자리에 선다 — 시계로 흘려 다음 세그먼트에서 이어 튼다
        val stay = bar.locate(s(50), wantPlay = true, skipGaps = false)
        assertNull(stay.seg)
        assertEquals(s(50), stay.atMs)
        assertTrue(bar.hasAfter(stay.atMs))
        // 멈춘 채 옮기는 것은 뛰지 않는다
        assertNull(bar.locate(s(50), wantPlay = false, skipGaps = true).seg)
    }

    @Test fun `막대 밖은 끝으로 가둔다`() {
        val before = bar.locate(t0 - 30_000, wantPlay = true, skipGaps = true)
        assertEquals(t0, before.atMs)
        assertEquals(1, before.seg?.seq)
        assertEquals(0, before.offsetMs)
        val after = bar.locate(s(500), wantPlay = true, skipGaps = true)
        assertEquals(s(80), after.atMs)
        assertNull("끝에는 틀 것이 없다", after.seg)
        assertFalse(bar.hasAfter(after.atMs))
    }

    @Test fun `세그먼트가 끝나면 다음 세그먼트로 잇는다`() {
        assertEquals(2, bar.nextAfter(bar.segs[0])?.seq)
        assertNull(bar.nextAfter(bar.segs[1]))
        assertEquals(1, bar.segOf(1)?.seq)
        assertNull(bar.segOf(null))
    }

    @Test fun `막대 위치와 시각은 서로의 역이다`() {
        assertEquals(0.5f, bar.ratioOf(s(40)), 1e-5f)
        assertEquals(s(40), bar.timeAt(0.5f))
        assertEquals(0f, bar.ratioOf(t0 - 1), 0f)
        assertEquals(1f, bar.ratioOf(s(999)), 0f)
        assertEquals(t0, bar.timeAt(-0.2f))
        assertEquals(s(80), bar.timeAt(1.7f))
    }

    @Test fun `시작 시각이 없는 세그먼트는 앞 세그먼트의 끝에 이어 붙인다`() {
        val rec = RecordingInfo(id = "r", segments = listOf(
            RecordingSegment(2, durationMs = 5_000), RecordingSegment(1, durationMs = 10_000)))
        val b = playerBarOf(rowOf(call), rec)
        // 녹취 시작 시각도 없으면 응답 시각부터
        assertEquals(listOf(t0 to s(10), s(10) to s(15)), b.segs.map { it.startMs to it.endMs })
        assertEquals(15_000L, b.spanMs)
    }

    @Test fun `녹취가 없으면 막대도 없다`() {
        val empty = playerBarOf(rowOf(call), null)
        assertTrue(empty.segs.isEmpty())
        assertFalse(PlayerUi(bar = empty).has)
        assertFalse(PlayerUi().has)
        assertTrue(PlayerUi(bar = bar).has)
        assertFalse("받는 중에는 누르지 못한다", PlayerUi(bar = bar, loading = true).canPlay)
    }

    @Test fun `재생 위치 표기 — 지금 · 전체 · 실제 시각`() {
        val ui = PlayerUi(bar = bar, headMs = s(30))
        assertEquals("00:30", ui.posText)
        assertEquals("01:20", ui.lenText)
        assertEquals("09:10:30", ui.wallText)
        assertEquals(0.375f, ui.headRatio, 1e-5f)
        assertEquals("00:10 · 09:10:10", bar.hoverText(0.125f))
    }

    @Test fun `영상이 있는 세그먼트를 틀 때만 영상 칸이 열린다`() {
        val rec = RecordingInfo(id = "r", startAtMs = t0, segments = listOf(
            RecordingSegment(1, startAtMs = t0, durationMs = 10_000, hasVideo = true),
            RecordingSegment(2, startAtMs = s(20), durationMs = 10_000)))
        val b = playerBarOf(rowOf(call), rec)
        assertTrue(b.segs[0].hasVideo)
        assertFalse(b.segs[1].hasVideo)
    }

    // ── 무전 — 막대 = 세션 시간축(발언 레인과 같은 축) ────────────────────

    private val session = HistoryEntry(id = "ses-1", atMs = s(160), kind = HistoryKind.PTT, from = "tel:$u1", group = "tel:g002",
        durationSec = 160, recordingId = "ptt/1/ses-1", hasRecording = true, state = "ended", sessionKind = "group",
        startAtMs = t0, endAtMs = s(160), groupName = "1팀 무전", turnCount = 5, hasTurnCount = true, service = "ptt")

    private val sessionRec = RecordingInfo(id = "ptt/1/ses-1", startAtMs = t0, segments = listOf(
        RecordingSegment(1, "ptt", u1, s(2), s(14), 12_000),
        RecordingSegment(2, "ptt", u2, s(20), s(45), 25_000, tracks = listOf(
            SegmentTrack(0, "audio", listOf(SpeakerSpan(u2, 0, 25_000))),
            SegmentTrack(1, "audio", listOf(SpeakerSpan(u3, 8_000, 10_000))))),
        RecordingSegment(3, "ptt", u1, s(60), s(84), 24_000),
        RecordingSegment(4, "ptt", u3, s(100), s(150), 50_000)))

    private val row = rowOf(session, names)
    private val pane = buildSessionPane(row, null, sessionRec, compact = true, names = names)
    private val pttBar = playerBarOf(row, sessionRec, pane, names)

    @Test fun `무전 막대 — 세션 시간축을 쓰고 틈 줄임과 무관하게 시간에 비례한다`() {
        assertTrue(pane.axis.compacted)
        assertEquals(t0, pttBar.t0)
        assertEquals(160_000L, pttBar.spanMs)
        assertTrue(pttBar.ptt)
        assertEquals(0.5f, pttBar.ratioOf(s(80)), 1e-5f)                 // 발언 레인(줄인 축)과 달리 선형이다
        assertEquals(4, pttBar.segs.size)
    }

    @Test fun `발언 막대 — 화자 레인 색, 겹치는 동시 발언은 두 번째 줄`() {
        assertEquals(5, pttBar.blocks.size)
        assertEquals(listOf(0, 0, 1, 0, 0), pttBar.blocks.map { it.row })
        assertEquals(listOf(0, 1, 2, 0, 2), pttBar.blocks.map { it.lane })
        assertEquals(2f / 160f, pttBar.blocks[0].left, 1e-5f)
        assertEquals(12f / 160f, pttBar.blocks[0].width, 1e-5f)
    }

    @Test fun `이전·다음 발언 — 발언 막대의 시작으로`() {
        assertEquals(s(60), pttBar.prevTurnStart(s(70)))
        // 막 시작한 발언에서 누르면 그 앞 발언으로 — 1초 안쪽은 «지금 발언» 으로 치지 않는다
        assertEquals(s(28), pttBar.prevTurnStart(s(60) + 500))
        assertEquals("앞에 발언이 없으면 막대의 처음", t0, pttBar.prevTurnStart(s(1)))
        assertEquals(s(100), pttBar.nextTurnStart(s(70)))
        assertEquals(s(28), pttBar.nextTurnStart(s(20)))
        assertNull("뒤에 발언이 없으면 그대로", pttBar.nextTurnStart(s(120)))
    }

    @Test fun `지금 누가 말하는가 — 동시 발언이면 둘 다, 틈이면 말 없는 구간`() {
        assertEquals("지금 이당직 + 박현장 · 발언 #2", pttBar.nowText(s(30)))
        assertEquals("지금 김관제 · 발언 #1", pttBar.nowText(s(5)))
        assertEquals("말 없는 구간", pttBar.nowText(s(50)))
        assertTrue(PlayerUi(bar = pttBar, headMs = s(30)).speaking)
        assertFalse(PlayerUi(bar = pttBar, headMs = s(50)).speaking)
    }

    @Test fun `막대를 짚은 지점의 말풍선 — 무전은 그때 말한 사람까지`() {
        assertEquals("00:30 · 09:10:30 · 이당직, 박현장", pttBar.hoverText(30f / 160f))
        assertEquals("00:50 · 09:10:50 · 말 없음", pttBar.hoverText(50f / 160f))
    }

    @Test fun `발언 사이의 말 없는 구간 — 건너뛰기면 다음 발언 세그먼트로`() {
        val hit = pttBar.locate(s(50), wantPlay = true, skipGaps = true)
        assertEquals(3, hit.seg?.seq)
        assertEquals(s(60), hit.atMs)
        // 동시 발언 구간을 누르면 그 세그먼트(믹스)의 그 지점
        val both = pttBar.locate(s(30), wantPlay = true, skipGaps = true)
        assertEquals(2, both.seg?.seq)
        assertEquals(10_000, both.offsetMs)
    }

    @Test fun `발언 레인이 없는 무전 세션은 세그먼트 구간이 막대다`() {
        val noTurns = buildSessionPane(row, null, null)
        val b = playerBarOf(row, RecordingInfo(id = "r", segments = listOf(
            RecordingSegment(1, startAtMs = s(10), durationMs = 5_000))), noTurns, names)
        assertEquals(s(10), b.t0)
        assertEquals(5_000L, b.spanMs)
    }
}
