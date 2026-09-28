// [통화] 화면 Preview — 세 면의 밀도를 각각 본다 (android_dispatch_tablet.md §6.3)
package com.cims.ue.dispatch.ui.call

import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
import com.cims.ue.dispatch.session.CallLogKind
import com.cims.ue.dispatch.session.CallLogRow
import com.cims.ue.dispatch.session.DeskTally
import com.cims.ue.dispatch.session.DialogRow
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.session.MessageKind
import com.cims.ue.dispatch.session.SendState
import com.cims.ue.dispatch.ui.ptt.ThreadChip
import com.cims.ue.dispatch.ui.CallPane
import com.cims.ue.dispatch.ui.PreviewBody
import com.cims.ue.dispatch.ui.PreviewFrame
import com.cims.ue.dispatch.ui.previewCallInfo
import com.cims.ue.dispatch.ui.previewSession
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.DialogInfo

private val T0 = 1_790_000_000_000L

private fun dlg(id: String, state: String, remote: String, confirmed: Boolean = false, incoming: Boolean = true) =
    DialogRow(
        watched = "1001", id = id,
        info = DialogInfo(accountId = 0, watched = "1001", id = id, callId = "c-$id",
            localTag = "l", remoteTag = "r",
            direction = if (incoming) "recipient" else "initiator",
            state = state, remoteIdentity = "sip:$remote@cims", full = true),
        startedAtMs = T0 - 120_000, confirmedAtMs = if (confirmed) T0 - 90_000 else null,
        wasConfirmed = confirmed)

private val MEMBERS = listOf(
    MemberChip(aor = "1001", number = "1001", name = "김관제", isMe = true),
    MemberChip(aor = "1002", number = "1002", name = "이당직", isMe = false,
        dialog = dlg("d2", "confirmed", "7001", confirmed = true)),
    MemberChip(aor = "1003", number = "1003", name = "박현장", isMe = false, dialog = dlg("d3", "early", "7002")),
    MemberChip(aor = "1004", number = "1004", name = "최순찰", isMe = false),
    MemberChip(aor = "1005", number = "1005", name = "정정비", isMe = false,
        dialog = dlg("d5", "confirmed", "7003", confirmed = true), monitoring = true),
)

private val QUEUE = listOf(
    QueueItem(dialog = dlg("q1", "early", "01055551111"), caller = "01055551111",
        ringingAt = listOf("이당직", "박현장")),
    QueueItem(dialog = dlg("q2", "confirmed", "01055552222", confirmed = true), caller = "01055552222",
        answeredBy = "최순찰"),
)

private val CALLS = listOf(
    CallCard(previewSession(callId = 31, title = "민원인 A",
        info = previewCallInfo(callId = 31, remoteUri = "sip:01055551111@cims", state = CallState.ACTIVE),
        elapsedSec = 134)),
    CallCard(previewSession(callId = 32, title = "상담2",
        info = previewCallInfo(callId = 32, remoteUri = "sip:1009@cims", state = CallState.INCOMING),
        elapsedSec = 6)),
    CallCard(previewSession(callId = 33, title = "본사",
        info = previewCallInfo(callId = 33, remoteUri = "sip:02111222@cims", state = CallState.HELD),
        elapsedSec = 420), dtmfOpen = true, dtmfSent = "1*"),
)

private fun logRow(i: Int, peer: String, kind: CallLogKind, answered: Boolean = true, pilot: Boolean = false) =
    CallLogRow(atMs = T0 - i * 600_000L, peer = peer, text = "", kind = kind, number = "0105555$i",
        startedAtMs = T0 - i * 600_000L - 60_000,
        answeredAtMs = if (answered) T0 - i * 600_000L - 40_000 else null, viaPilot = pilot)

private val LOG = listOf(
    logRow(1, "김민원", CallLogKind.ANSWERED),
    logRow(2, "이시민", CallLogKind.MISSED, answered = false, pilot = true),
    logRow(3, "본사 총무", CallLogKind.OUTGOING),
    logRow(4, "박민원", CallLogKind.PICKUP, pilot = true),
    logRow(5, "최시민", CallLogKind.TRANSFER),
    logRow(6, "정민원", CallLogKind.MONITOR),
    logRow(7, "한시민", CallLogKind.ANSWERED),
    logRow(8, "오민원", CallLogKind.MISSED, answered = false),
    logRow(9, "서시민", CallLogKind.ANSWERED, pilot = true),
    logRow(10, "남민원", CallLogKind.OUTGOING),
)

private val UI = CallsUi(
    dialNumber = "1002", members = MEMBERS, queue = QUEUE,
    tally = DeskTally(answered = 12, missed = 3, outgoing = 8, transfer = 2, monitor = 1),
    calls = CALLS, log = LOG, liveHint = "감시 대상 5회선 · 구독 성립 5")

@Preview(name = "통화 — 1면 통화(키패드·진행 중)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewCalls() = PreviewFrame { CallsScreenContent(ui = UI, pane = CallPane.CALLS) }

@Preview(name = "통화 — 2면 주소록", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewCallsBook() = PreviewFrame {
    CallsScreenContent(ui = UI, pane = CallPane.BOOK,
        bookPane = { })   // 주소록 본문은 VM 을 쓰므로 Preview 에서는 비운다
}

@Preview(name = "통화 — 3면 메시지(준비 중)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewCallsSms() = PreviewFrame {
    CallsScreenContent(ui = UI, pane = CallPane.MESSAGES,
        smsPane = { SmsPaneContent(threads = SMS_THREADS, thread = SMS_THREAD, peer = "1002", title = "이당직") })
}

@Preview(name = "통화 — 4면 통화내역", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewCallsLog() = PreviewFrame { CallsScreenContent(ui = UI, pane = CallPane.LOG) }

@Preview(name = "통화 — 어두운 테마", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewCallsDark() = PreviewFrame(dark = true) { CallsScreenContent(ui = UI, pane = CallPane.CALLS) }

@Preview(name = "통화 — 조용할 때", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewCallsIdle() = PreviewFrame {
    CallsScreenContent(ui = CallsUi(members = MEMBERS.take(2)), pane = CallPane.CALLS)
}

private val SMS_THREADS = listOf(
    ThreadChip("1002", "이당직", 0, T0),
    ThreadChip("1003", "박현장", 2, T0 - 600_000),
    ThreadChip("01055551111", "김민원", 0, T0 - 3_600_000))

private val SMS_THREAD = listOf(
    Message(id = "s1", groupId = "1002", fromUri = "sip:1002@cims", fromName = "이당직",
        text = "3번 게이트 확인 부탁드립니다", atMs = T0 - 300_000, outgoing = false,
        kind = MessageKind.SMS),
    Message(id = "s2", groupId = "1002", fromUri = "", fromName = "나",
        text = "확인했습니다. 이상 없습니다", atMs = T0 - 240_000, outgoing = true,
        state = SendState.SENT, kind = MessageKind.SMS))
