// [이력] 화면 Preview — 통화 표와 PTT 세션 카드, 시간대 밴드 (android_dispatch_tablet.md §6.11)
//
// 이 화면은 **표 밀도**가 가장 중요한 자리다 — 한 화면에 몇 건이 보이는지가 조회의 쓸모를 정한다.
package com.cims.ue.dispatch.ui.history

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.ui.PreviewBody
import com.cims.ue.dispatch.ui.PreviewFrame
import com.cims.ue.dispatch.ui.Type

private val T0 = 1_790_000_000_000L

private fun call(i: Int, from: String, to: String, sec: Int, state: String = "answered",
                 rec: Boolean = false, reason: String = "") = HistoryEntry(
    id = "c$i", atMs = T0 - i * 900_000L, kind = HistoryKind.CALL, from = from, to = to,
    durationSec = sec, hasRecording = rec, recordingId = if (rec) "r$i" else "",
    state = state, callType = "voip",
    inviteAtMs = T0 - i * 900_000L - 40_000,
    answerAtMs = if (state == "answered") T0 - i * 900_000L - 30_000 else null,
    endAtMs = T0 - i * 900_000L, endReason = reason)

private val CALLS = listOf(
    call(1, "01055551111", "1001", 143, rec = true),
    call(2, "01055552222", "7000", 0, state = "missed", reason = "no answer"),
    call(3, "1001", "0212223333", 420),
    call(4, "01055554444", "1002", 62, rec = true),
    call(5, "01055555555", "7000", 0, state = "rejected", reason = "busy"),
    call(6, "1003", "01055556666", 215),
    call(7, "01055557777", "1001", 38),
    call(8, "01055558888", "7000", 91, rec = true),
    call(9, "1002", "0311234567", 17),
    call(10, "01055550000", "1004", 0, state = "missed"),
)

private fun ptt(i: Int, name: String, members: Int, turns: Int, sec: Int, emg: Boolean = false) =
    HistoryEntry(id = "p$i", atMs = T0 - i * 1_800_000L, kind = HistoryKind.PTT,
        group = "g00$i", groupName = name, memberCount = members, turnCount = turns,
        speakerCount = minOf(members, 4), durationSec = sec, emergency = emg,
        hasRecording = true, recordingId = "pr$i", sessionKind = "prearranged",
        startAtMs = T0 - i * 1_800_000L - sec * 1000L)

private val SESSIONS = listOf(
    ptt(1, "순찰1", 12, 34, 620),
    ptt(2, "상황실", 8, 9, 180, emg = true),
    ptt(3, "교통1", 15, 51, 1240),
    ptt(4, "야간순찰", 9, 12, 300),
    ptt(5, "정비반", 6, 4, 95),
)

/** 24칸 시간대 밴드 — 업무 시간에 몰린 흔한 모양. */
private val BAND = intArrayOf(0, 0, 0, 0, 0, 0, 1, 3, 12, 18, 22, 15,
                              9, 14, 20, 24, 19, 11, 6, 3, 2, 1, 0, 0)

@Composable
private fun Stub(text: String) =
    Box(Modifier.fillMaxSize().padding(16.dp), contentAlignment = Alignment.Center) {
        Text(text, fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }

@Preview(name = "이력 — 통화 표", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewHistoryCalls() = PreviewFrame {
    HistoryScreenContent(HistoryUi(kind = HistoryKind.CALL, rows = CALLS, band = BAND))
}

@Preview(name = "이력 — 통화(녹취 선택)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewHistoryCallSelected() = PreviewFrame {
    HistoryScreenContent(
        HistoryUi(kind = HistoryKind.CALL, rows = CALLS, band = BAND, selected = CALLS[0],
            hourFilter = 14),
        recordingStrip = { Stub("녹취 재생 띠 — 따로 Preview") })
}

@Preview(name = "이력 — PTT 세션", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewHistoryPtt() = PreviewFrame {
    HistoryScreenContent(
        HistoryUi(kind = HistoryKind.PTT, rows = SESSIONS, band = BAND, selected = SESSIONS[0]),
        sessionPane = { Stub("세션 상세 — 참여자·발언 타임라인") })
}

/** 상한에 걸린 경우 — 조용히 일부만 보여 주지 않는다는 규약이 눈에 보이는지. */
@Preview(name = "이력 — 상한 초과", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewHistoryTruncated() = PreviewFrame {
    HistoryScreenContent(HistoryUi(kind = HistoryKind.CALL, rows = CALLS, band = BAND, truncated = true))
}

@Preview(name = "이력 — 그날 없음", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewHistoryEmpty() = PreviewFrame {
    HistoryScreenContent(HistoryUi(kind = HistoryKind.CALL, rows = emptyList(), band = IntArray(24)))
}
