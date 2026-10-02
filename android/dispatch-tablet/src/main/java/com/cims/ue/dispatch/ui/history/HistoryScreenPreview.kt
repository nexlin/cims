// [이력] 화면 Preview — 통화·무전·영상 세션·빈 세션 묶음 (android_dispatch_tablet.md §6.11)
//
// 세션·VM 없이 `HistoryScreenContent` 만으로 선다. 표본은 Windows 관제 앱의 `--ui-preview-history` 와 같은 하루다 —
// 무전: 그룹 세션(동시 발언·녹취) · 긴급 세션 · 진행 중 · 전이중 개별 호 · 영상 세션(MCVideo) · 13시의 빈 세션 반복 9건(가운데
// 발언 1건이 묶음을 둘로 끊는다) / 통화: 응답·부재·영상 통화(긴급)·통화 중·통화중 실패.
package com.cims.ue.dispatch.ui.history

import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
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
import com.cims.ue.dispatch.ui.PreviewBody
import com.cims.ue.dispatch.ui.PreviewFrame
import java.time.LocalDate
import java.time.ZoneId

private val DAY: LocalDate = LocalDate.of(2026, 9, 8)

private fun at(h: Int, m: Int, s: Int = 0): Long =
    DAY.atTime(h, m, s).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli()

private const val U1 = "+821310002001"
private const val U2 = "+821310002002"
private const val U3 = "+821310002003"
private const val U4 = "+821310002004"

private val NAMES = HistoryNames(
    who = { u ->
        when (u.substringAfter(':')) {
            U1 -> "김관제"; U2 -> "이당직"; U3 -> "박현장"; U4 -> "최순찰"
            else -> u.substringAfter(':')
        }
    })

private fun ptt(id: String, start: Long, sec: Int, from: String, group: String, name: String, turns: Int,
                speechMs: Int = 0, rec: Boolean = turns > 0, kind: String = "group", service: String = "ptt",
                emergency: Boolean = false, live: Boolean = false, floor: String = "on", policy: String = "single",
                people: List<String> = emptyList(), members: Int = 0, concurrent: Int = 0, mcvType: String = "",
                mcvMax: Int = 0) = HistoryEntry(
    id = id, atMs = start + sec * 1000L, kind = HistoryKind.PTT, event = if (live) "ptt.session.start" else "ptt.session.end",
    from = "tel:$from", group = "tel:$group", durationSec = sec, emergency = emergency,
    recordingId = if (rec) "ptt/1/2026/09/08/$id" else "", hasRecording = rec,
    state = if (live) "active" else "ended", sessionKind = kind, startAtMs = start, endAtMs = if (live) null else start + sec * 1000L,
    groupName = name, memberCount = members, turnCount = turns, hasTurnCount = true, speakerCount = minOf(turns, 3),
    totalSpeechMs = speechMs, talkMs = speechMs, maxConcurrent = concurrent, floorControl = floor, floorPolicy = policy,
    maxTalkers = if (policy == "dual") 2 else 0, people = people, service = service, mcvSessionType = mcvType,
    mcvMaxTransmitters = mcvMax)

/** 무전 하루 — 최근이 위. */
private val SESSIONS: List<HistoryEntry> = buildList {
    add(ptt("ses-1", at(9, 10), 160, U1, "g002", "1팀 무전", 7, 61_000, policy = "dual", concurrent = 2, members = 6,
        people = listOf(U1, U2, U3, U4)))
    add(ptt("ses-2", at(11, 2, 40), 25, U2, "g002", "1팀 무전", 2, 9_000, emergency = true))
    add(ptt("ses-3", at(11, 40), 0, U3, "g003", "야간 순찰", 1, 3_000, rec = false, live = true))
    add(ptt("ses-4", at(14, 20), 30, U1, "priv-1", "", 0, rec = false, kind = "private", floor = "off", people = listOf(U1, U4)))
    // 영상 세션(MCVideo 그룹 호) — 같은 그룹의 영상 호. 세는 것은 송출, 재생하면 영상 칸.
    add(ptt("mcv-1", at(15, 5), 200, U2, "g002", "1팀 무전", 2, 95_000, service = "mcvideo", mcvType = "chat", mcvMax = 2,
        members = 6, people = listOf(U1, U2, U3)))
    // 빈 세션 반복 — 한 사람이 1분마다 호를 열었다 말없이 닫는다(유지 시간 T4 30초). 13:24 의 발언 세션이 묶음을 둘로 끊는다.
    repeat(9) { i ->
        val spoke = i == 4
        add(ptt("rep-$i", at(13, 20 + i), if (spoke) 46 else 30 + i % 2, U3, "g003", "야간 순찰", if (spoke) 1 else 0,
            if (spoke) 3_000 else 0, members = 10))
    }
}.sortedByDescending { it.atMs }

private fun call(id: String, invite: Long, ringSec: Int, talkSec: Int, from: String, to: String, reason: String = "normal",
                 video: Boolean = false, emergency: Boolean = false, rec: Boolean = false, state: String = "ended") = HistoryEntry(
    id = id, atMs = invite + (ringSec + talkSec) * 1000L, kind = HistoryKind.CALL,
    event = if (talkSec > 0 || state == "active") "call.answered" else "call.missed", from = "tel:$from", to = "tel:$to",
    durationSec = talkSec, emergency = emergency, recordingId = if (rec) "volte/2026/09/08/$id.d" else "", hasRecording = rec,
    state = state, callType = if (video) "volte_video" else "volte", inviteAtMs = invite,
    answerAtMs = if (talkSec > 0 || state == "active") invite + ringSec * 1000L else null,
    endAtMs = if (state == "active") null else invite + (ringSec + talkSec) * 1000L, endReason = if (state == "active") "" else reason)

/** 통화 하루 — 최근이 위. */
private val CALLS: List<HistoryEntry> = listOf(
    call("c1", at(9, 4, 25), 5, 42, U1, "+821310009999", rec = true),
    call("c2", at(9, 30, 40), 20, 0, U2, U1, reason = "no_answer"),
    call("c3", at(13, 6, 50), 8, 305, U3, "0212345678", video = true, emergency = true, rec = true),
    call("c4", at(13, 40), 4, 0, U1, U2, state = "active"),
    call("c5", at(16, 1, 30), 30, 0, "+821310009999", U2, reason = "busy"),
    call("c6", at(16, 20), 3, 0, U4, U1, reason = "rejected"),
).sortedByDescending { it.atMs }

private fun bandOf(items: List<HistoryEntry>) = HistoryViewModel.bandOf(items, emptyMap())

// ── 고른 세션의 상세 ────────────────────────────────────────────────────────

private val S0 = at(9, 10)

/** ses-1 의 녹취 — 동시 발언 세그먼트(슬롯 둘)와 변환 중 세그먼트가 섞여 있다. */
private val REC = RecordingInfo(
    id = "ptt/1/2026/09/08/ses-1", startAtMs = S0, status = "ready", service = "ptt",
    segments = listOf(
        RecordingSegment(1, "ptt", U1, S0 + 2_000, S0 + 14_000, 12_000, status = "ready"),
        RecordingSegment(2, "ptt", U2, S0 + 20_000, S0 + 45_000, 25_000, status = "ready", tracks = listOf(
            SegmentTrack(0, "audio", listOf(SpeakerSpan(U2, 0, 25_000))),
            SegmentTrack(1, "audio", listOf(SpeakerSpan(U3, 8_000, 10_000))))),
        RecordingSegment(3, "ptt", U1, S0 + 60_000, S0 + 84_000, 24_000, status = "ready"),
        RecordingSegment(4, "ptt", U3, S0 + 100_000, S0 + 150_000, 50_000, status = "transcoding")))

private fun floor(sec: Int, op: String, user: String, slot: Int? = null, talkers: Int? = null, reason: String = "",
                  owner: String = "", pos: Int? = null, qsize: Int? = null, grace: Int? = null, idle: Int? = null) =
    PttFloorEvent(S0 + sec * 1000L, op, user, slot = slot, talkers = talkers, reason = reason, owner = owner, pos = pos,
        qsize = qsize, graceSec = grace, idleMs = idle)

private val DETAIL = PttSessionDetail(
    recordingId = REC.id,
    participants = listOf(
        PttParticipant(U1, "initiator", S0, S0 + 160_000), PttParticipant(U2, "member", S0 + 1_000, S0 + 160_000),
        PttParticipant(U3, "member", S0 + 1_000, null), PttParticipant(U4, "member", S0 + 30_000, S0 + 90_000)),
    events = listOf(
        PttEvent(S0, "session_start"), PttEvent(S0, "member_join", U1, "initiator"),
        PttEvent(S0 + 1_000, "member_join", U2, "member"), PttEvent(S0 + 1_000, "member_join", U3, "member"),
        PttEvent(S0 + 30_000, "member_join", U4, "member"), PttEvent(S0 + 90_000, "member_leave", U4, "member"),
        PttEvent(S0 + 160_000, "session_end", durationSec = 160)),
    floor = listOf(
        floor(2, "GRANT", U1, 0, 1), floor(14, "RELEASE", U1, 0, idle = 300), floor(20, "GRANT", U2, 0, 1),
        floor(28, "GRANT", U3, 1, 2), floor(31, "DENY", U4, reason = "recv_only", owner = U2), floor(38, "RELEASE", U3, 1),
        floor(45, "RELEASE", U2, 0), floor(60, "GRANT", U1, 0, 1), floor(70, "QUEUE", U3, pos = 1, qsize = 1),
        floor(84, "RELEASE", U1, 0), floor(100, "GRANT", U3, 0, 1), floor(150, "REVOKE", U3, 0, grace = 3)),
    hasRecording = true)

/** mcv-1 의 녹취 — 송출 구간 둘. 두 번째는 음성 없이 영상만(송출 중 무전으로 마이크를 넘긴 동안). */
private val V0 = at(15, 5)
private val VIDEO_REC = RecordingInfo(
    id = "ptt/1/2026/09/08/mcv-1", startAtMs = V0, status = "ready", service = "mcvideo",
    segments = listOf(
        RecordingSegment(1, "mcvideo", U2, V0 + 6_000, V0 + 66_000, 60_000, hasVideo = true, status = "ready", tracks = listOf(
            SegmentTrack(0, "audio", listOf(SpeakerSpan(U2, 0, 60_000)), hasVideo = true),
            SegmentTrack(0, "video", listOf(SpeakerSpan(U2, 0, 60_000)), hasVideo = true))),
        RecordingSegment(2, "mcvideo", U3, V0 + 120_000, V0 + 155_000, 35_000, hasVideo = true, status = "ready", tracks = listOf(
            SegmentTrack(0, "video", listOf(SpeakerSpan(U3, 0, 35_000)), hasVideo = true)))))

private fun paneOf(e: HistoryEntry, detail: PttSessionDetail?, rec: RecordingInfo?, compact: Boolean = true) =
    buildSessionPane(rowOf(e, NAMES), detail, rec, compact, NAMES)

private fun playerOf(e: HistoryEntry, rec: RecordingInfo?, pane: SessionPaneUi?, headSec: Int, playing: Boolean = false,
                     video: Boolean = false): PlayerUi {
    val bar = playerBarOf(rowOf(e, NAMES), rec, pane, NAMES)
    return PlayerUi(bar = bar, headMs = bar.t0 + headSec * 1000L, playing = playing, mediaOpen = playing,
        status = rec?.let { "녹취 ${it.segments.size}개 중 #1 (표본)" } ?: "녹취 없음", video = video, videoAspect = 3f / 4f)
}

private fun entry(id: String) = (SESSIONS + CALLS).first { it.id == id }

// ── 화면 ────────────────────────────────────────────────────────────────────

@Preview(name = "이력 — 무전(세션 패널·녹취 재생 바)", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewHistoryPtt() = PreviewFrame {
    val e = entry("ses-1")
    val pane = paneOf(e, DETAIL, REC)
    HistoryScreenContent(
        HistoryUi(kind = HistoryKind.PTT, rows = SESSIONS, band = bandOf(SESSIONS), date = DAY, selected = e, serviceAxis = true),
        pane = pane, player = playerOf(e, REC, pane, headSec = 30, playing = true), names = NAMES)
}

@Preview(name = "이력 — 무전(틈 줄임 끔·어둡게)", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewHistoryPttLinear() = PreviewFrame(dark = true) {
    val e = entry("ses-1")
    val pane = paneOf(e, DETAIL, REC, compact = false)
    HistoryScreenContent(
        HistoryUi(kind = HistoryKind.PTT, rows = SESSIONS, band = bandOf(SESSIONS), date = DAY, selected = e, serviceAxis = true,
            compactGaps = false, bandMode = BandMode.TURNS, hourFilter = 9),
        pane = pane, player = playerOf(e, REC, pane, headSec = 70), names = NAMES)
}

@Preview(name = "이력 — 무전(빈 세션 묶음 펼침)", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewHistoryBundles() = PreviewFrame {
    val rows = SESSIONS.map { rowOf(it, NAMES) }
    val open = bundleSilent(rows, enabled = true).filter { it.kind == RowKind.BUNDLE }.take(1).map { it.bundleKey }.toSet()
    HistoryScreenContent(
        HistoryUi(kind = HistoryKind.PTT, rows = SESSIONS, band = bandOf(SESSIONS), date = DAY, serviceAxis = true,
            openBundles = open, selected = entry("rep-7")),
        names = NAMES)
}

@Preview(name = "이력 — 영상 세션(영상 칸)", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewHistoryVideoSession() = PreviewFrame {
    val e = entry("mcv-1")
    val pane = paneOf(e, null, VIDEO_REC)
    HistoryScreenContent(
        HistoryUi(kind = HistoryKind.PTT, rows = SESSIONS, band = bandOf(SESSIONS), date = DAY, selected = e, serviceAxis = true,
            service = ServiceFilter.ALL),
        pane = pane, player = playerOf(e, VIDEO_REC, pane, headSec = 20, playing = true, video = true), names = NAMES)
}

@Preview(name = "이력 — 통화(상세·녹취 재생 바)", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewHistoryCall() = PreviewFrame {
    val e = entry("c1")
    val rec = RecordingInfo(id = e.recordingId, callType = "volte", startAtMs = e.answerAtMs, status = "ready",
        segments = listOf(RecordingSegment(1, "volte", U1, e.answerAtMs, e.endAtMs, 42_000, status = "ready")))
    HistoryScreenContent(
        HistoryUi(kind = HistoryKind.CALL, rows = CALLS, band = bandOf(CALLS), date = DAY, selected = e),
        player = playerOf(e, rec, null, headSec = 12), names = NAMES)
}

@Preview(name = "이력 — 영상 통화(긴급·영상 칸)", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewHistoryVideoCall() = PreviewFrame {
    val e = entry("c3")
    val rec = RecordingInfo(id = e.recordingId, callType = "volte_video", startAtMs = e.answerAtMs, status = "ready",
        segments = listOf(RecordingSegment(1, "volte", U3, e.answerAtMs, e.endAtMs, 305_000, hasVideo = true, status = "ready")))
    HistoryScreenContent(
        HistoryUi(kind = HistoryKind.CALL, rows = CALLS, band = bandOf(CALLS), date = DAY, selected = e, hourFilter = 13),
        player = playerOf(e, rec, null, headSec = 95, playing = true, video = true).copy(videoAspect = 4f / 3f), names = NAMES)
}

/** 상한에 걸린 경우 — 조용히 일부만 보여 주지 않는다는 규약이 눈에 보이는지. */
@Preview(name = "이력 — 상한 초과", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewHistoryTruncated() = PreviewFrame {
    HistoryScreenContent(HistoryUi(kind = HistoryKind.CALL, rows = CALLS, band = bandOf(CALLS), date = DAY, truncated = true),
        names = NAMES)
}

@Preview(name = "이력 — 그날 없음", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewHistoryEmpty() = PreviewFrame {
    HistoryScreenContent(HistoryUi(kind = HistoryKind.PTT, rows = emptyList(), band = IntArray(24), date = DAY))
}
