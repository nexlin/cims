// [채널] 화면 Preview — 머리의 조작과 로스터 (android_dispatch_tablet.md §6.3a)
//
// 메시지·이벤트는 [무전] 메뉴의 면이 되어 여기서 뺐다 — 두 곳에 두면 «어느 쪽이 지금 채널 것인가» 가
// 흐려진다. 이 화면이 답하는 질문은 «이 채널에 지금 누가 있고 내가 뭘 할 수 있나» 하나다.
package com.cims.ue.dispatch.ui.ptt

import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
import com.cims.ue.dispatch.ui.PreviewBody
import com.cims.ue.dispatch.ui.PreviewFrame
import com.cims.ue.sdk.RosterEntry

private fun roster(vararg n: String) = n.map { RosterEntry("sip:$it@cims", "connected") }

private val MINE_HEAD = ChannelHeadUi(
    id = "g1", title = "순찰1", badge = "멤버",
    subtitle = "참가 7 · 발언 김관제 00:14 · 12:31",
    joined = true, isMemberGroup = true, canTarget = true, targeted = true, unread = 3,
    groupId = "g1")

@Preview(name = "채널 — 내 채널(참여 중)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewChannelMine() = PreviewFrame {
    ChannelScreenContent(
        head = MINE_HEAD,
        roster = roster("1001", "1002", "1003", "1004", "1005", "1006", "1007"),
        speaker = "1001", me = "1002",
        nameOf = { if (it == "1001") "김관제" else "" })
}

@Preview(name = "채널 — 미참여(멤버 그룹)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewChannelNotJoined() = PreviewFrame {
    ChannelScreenContent(
        head = MINE_HEAD.copy(joined = false, canTarget = false, targeted = false,
            subtitle = "멤버 12 · 대기", unread = 0))
}

@Preview(name = "채널 — 긴급", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewChannelEmergency() = PreviewFrame {
    ChannelScreenContent(
        head = MINE_HEAD.copy(emergency = true, subtitle = "참가 9 · 발언 박현장 00:03"),
        roster = roster("1001", "1002", "1003"), speaker = "1003", me = "1002")
}

@Preview(name = "채널 — 임박 위험", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewChannelPeril() = PreviewFrame(dark = true) {
    ChannelScreenContent(
        head = MINE_HEAD.copy(imminentPeril = true, subtitle = "참가 4 · 발언 없음 · 01:35"),
        roster = roster("1001", "1002", "1021"), me = "1002")
}

/** 전이중 개별 통화 — 발언 대상 칩 대신 음소거. 편성이 없어 [편성 전원]·로스터가 없다(카드 그대로). */
@Preview(name = "채널 — 전이중 개별 통화(음소거 중)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewChannelFullDuplex() = PreviewFrame {
    ChannelScreenContent(
        head = ChannelHeadUi(id = "p3", title = "최주임", badge = "개별",
            subtitle = "발언 없음 · 00:21", joined = true, muted = true))
}

@Preview(name = "채널 — 범위(청취 중)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewChannelScoped() = PreviewFrame {
    ChannelScreenContent(
        head = ChannelHeadUi(id = "s1", title = "야간순찰", badge = "범위",
            subtitle = "참가 5 · 청취 중 · 발언 이당직", groupId = "s1", listening = true),
        roster = roster("2001", "2002", "2003", "2004", "2005"), speaker = "2001", me = "1002")
}

/** 로스터가 많을 때 — 전부 펴는 면이라 잘리지 않아야 한다(§6.3a). */
@Preview(name = "채널 — 로스터 24명", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewChannelBigRoster() = PreviewFrame {
    ChannelScreenContent(
        head = MINE_HEAD.copy(subtitle = "참가 24 · 발언 김관제 00:14"),
        roster = roster(*(1..24).map { "10%02d".format(it) }.toTypedArray()),
        speaker = "1001", me = "1002")
}

@Preview(name = "채널 — 사라짐", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewChannelGone() = PreviewFrame {
    ChannelScreenContent(head = ChannelHeadUi(id = "g9", title = "g9", gone = true))
}
