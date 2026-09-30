// ④ PTT 메시지 · ⑤ PTT 이벤트 Preview (android_dispatch_tablet.md §6.9a)
//
// 둘 다 [메시지] 화면과 [채널] 화면의 한 면으로 쓰이므로, **전체 화면 높이**와 **한 면 높이** 둘 다 본다.
package com.cims.ue.dispatch.ui.ptt

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.session.ActivityKind
import com.cims.ue.dispatch.session.ActivityRow
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.session.SendState
import com.cims.ue.dispatch.ui.PreviewBody
import com.cims.ue.dispatch.ui.PreviewFrame

private val T0 = 1_790_000_000_000L   // 고정 시각 — Preview 가 돌 때마다 시간이 달라지지 않게

private fun msg(i: Int, text: String, out: Boolean = false, from: String = "김관제",
                state: SendState = SendState.NONE) =
    Message(id = "m$i", groupId = "g1", fromUri = "sip:100$i@cims", fromName = from,
        text = text, atMs = T0 + i * 60_000L, outgoing = out, state = state)

private val THREAD = listOf(
    msg(1, "현장 도착했습니다"),
    msg(2, "3번 게이트 확인 바랍니다", out = true, state = SendState.DELIVERED),
    msg(3, "확인했습니다. 이상 없습니다", from = "박현장"),
    msg(4, "수고하셨습니다", out = true, state = SendState.READ),
    msg(5, "순찰 2조 교대 요청합니다 — 인원 2명 부족합니다. 지원 가능한지 회신 부탁드립니다", from = "이당직"),
    msg(6, "확인 중", out = true, state = SendState.PENDING),
    msg(7, "전송 실패한 메시지", out = true, state = SendState.FAILED),
)

private val CHIPS = listOf(
    ThreadChip("g1", "순찰1", 0, T0),
    ThreadChip("g2", "상황실", 3, T0),
    ThreadChip("+821012345678", "박현장", 1, T0),
    ThreadChip("g3", "교통1", 0, T0),
)

@Preview(name = "④ 메시지 — 전체 화면", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewMessages() = PreviewFrame {
    Box(Modifier.fillMaxSize().padding(8.dp)) {
        MessagesContent(thread = THREAD, title = "순찰1", follow = true, groupId = "g1",
            threads = CHIPS, isGroup = true)
    }
}

/** 1:1 대화 — 머리에 «그룹 전원» 라벨·[채널 정보] 가 없고 빠른 답장도 같다. */
@Preview(name = "④ 메시지 — 1:1 대화", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewMessagesPane() = PreviewFrame {
    Box(Modifier.fillMaxSize().padding(8.dp)) {
        MessagesContent(thread = THREAD, title = "박현장", follow = false, groupId = "+821012345678",
            threads = CHIPS, isGroup = false)
    }
}

@Preview(name = "④ 메시지 — 스레드 없음", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewMessagesEmpty() = PreviewFrame {
    Box(Modifier.fillMaxSize().padding(8.dp)) {
        MessagesContent(thread = emptyList(), title = "채널 없음", follow = true,
            groupId = null, threads = emptyList())
    }
}

private fun act(i: Int, name: String, text: String, kind: ActivityKind, emg: Boolean = false) =
    ActivityRow(atMs = T0 + i * 30_000L, groupId = "g$i", groupName = name, text = text,
        kind = kind, emergency = emg)

private val EVENTS = listOf(
    act(9, "순찰1", "발언 김관제", ActivityKind.TALK),
    act(8, "순찰1", "입장 박현장", ActivityKind.JOIN),
    act(7, "상황실", "긴급 모드 시작", ActivityKind.EMERGENCY, emg = true),
    act(6, "순찰1", "SDS 수신 — 이당직", ActivityKind.SDS),
    act(5, "교통1", "퇴장 최순찰", ActivityKind.LEAVE),
    act(4, "순찰1", "발언 거부 — 우선순위 낮음", ActivityKind.ERROR),
    act(3, "야간순찰", "발언 이당직", ActivityKind.TALK),
    act(2, "정비반", "입장 김정비", ActivityKind.JOIN),
    act(1, "순찰1", "발언 박현장", ActivityKind.TALK),
)

@Preview(name = "⑤ 이벤트 — 전체 화면", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewActivity() = PreviewFrame {
    Box(Modifier.fillMaxSize().padding(8.dp)) {
        ActivityContent(rows = EVENTS)
    }
}

@Preview(name = "⑤ 이벤트 — 긴급·임박 고정 행", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewActivityPinned() = PreviewFrame {
    val now = System.currentTimeMillis()
    Box(Modifier.fillMaxSize().padding(8.dp)) {
        ActivityContent(rows = EVENTS.filter { it.kind == ActivityKind.TALK },
            pinned = listOf(
                com.cims.ue.dispatch.ui.AlertBannerUi("g-sit", com.cims.ue.dispatch.session.AlertKind.EMERGENCY,
                    "상황실", "1003 이순경", now - 95_000),
                com.cims.ue.dispatch.ui.AlertBannerUi("g-p2", com.cims.ue.dispatch.session.AlertKind.IMMINENT_PERIL,
                    "순찰2", "", now - 12_000)))
    }
}

@Preview(name = "⑤ 이벤트 — 없음", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewActivityEmpty() = PreviewFrame {
    Box(Modifier.fillMaxSize().padding(8.dp)) {
        ActivityContent(rows = emptyList())
    }
}
