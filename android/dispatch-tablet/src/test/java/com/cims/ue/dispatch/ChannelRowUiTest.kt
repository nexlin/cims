// 채널 목록 행·채널 머리의 **표시 규약** — 도메인 카드 → 화면이 받는 값(ChannelUi.kt·ChannelScreen.kt).
//
// 화면을 순수 컴포저블로 가른 덕에 «무엇을 어떻게 보여 주는가» 를 렌더 없이 고정할 수 있다.
// 여기서 잡는 것은 눈으로는 놓치기 쉬운 것들이다 — 핀 번호 표기, 범위 채널은 발언 대상이 될 수 없음,
// 청취 토글이 내 채널에는 없음, 전이중 개별 통화는 발언 대상 대신 음소거.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.AccountKind
import com.cims.ue.dispatch.session.Operation
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.ui.ptt.CardKind
import com.cims.ue.dispatch.ui.ptt.ChannelCard
import com.cims.ue.dispatch.ui.ptt.channelHead
import com.cims.ue.dispatch.ui.ptt.toRowUi
import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.McpttInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ChannelRowUiTest {

    private fun card(id: String = "g1", title: String = "순찰1") =
        ChannelCard(id = id, kind = CardKind.MEMBER, title = title)

    @Test fun `행 제목은 채널 이름 그대로다`() {
        assertEquals("교통1", card(title = "교통1").toRowUi(targeted = false).title)
    }

    @Test fun `내 채널에는 청취 토글이 없다`() {
        // listening = null 이 «이 행에 청취 버튼을 그리지 않는다» 는 뜻이다.
        assertNull(card().toRowUi(targeted = false).listening)
    }

    @Test fun `발언 대상 표시는 밖에서 받은 값을 그대로 쓴다`() {
        assertTrue(card().toRowUi(targeted = true).targeted)
        assertFalse(card().toRowUi(targeted = false).targeted)
    }

    @Test fun `세션이 없으면 대기이고 참가 수는 0 이다`() {
        val r = card().toRowUi(targeted = false)
        assertEquals("대기", r.state)
        assertEquals(0, r.participants)
        assertFalse(r.active)
        assertFalse(r.speaking)
        assertFalse(r.emergency)
    }

    @Test fun `참여하지 않은 멤버 그룹은 멤버 수를 2줄에 적는다`() {
        // 카드가 세션 없이도 서는 유일한 종류다(§6.3) — 그때 2줄이 비면 왜 있는 채널인지 알 수 없다.
        assertEquals("멤버 0", card().toRowUi(targeted = false).subtitle)
    }

    // ── 전이중 개별 통화 음소거 ──
    /** 참여 중인 개별 통화 — 전이중이면 floor 가 없다(`mc_no_floor_ctrl`). */
    private fun privateCall(fullDuplex: Boolean, muted: Boolean = false): ChannelCard {
        val info = CallInfo(
            callId = 7, accountId = 0, dir = CallDir.OUTGOING, state = CallState.ACTIVE,
            remoteUri = "sip:1003@d", calledParty = "", video = false, mediaActive = true,
            muted = muted, listen = true, playbackRoute = 0, lastCode = 0, lastReason = "",
            sources = emptyList(), isMcptt = true, groupId = "1003",
            mcptt = McpttInfo(true, "", "", "", "", false, false, true, fullDuplex),
            halfDuplex = !fullDuplex, listenOnly = false, joinedDialog = "")
        return ChannelCard(id = "1003", kind = CardKind.PRIVATE, title = "김반장",
            session = SessionItem(7, AccountKind.PTT, Operation.PTT_PRIVATE, info))
    }

    @Test fun `전이중 개별 통화는 발언 대상 대신 음소거 토글을 단다`() {
        val r = privateCall(fullDuplex = true).toRowUi(targeted = false)
        assertFalse(r.canTarget)
        assertEquals(false, r.muted)
    }

    @Test fun `음소거 표시는 코어 스냅샷을 그대로 따른다`() {
        assertEquals(true, privateCall(fullDuplex = true, muted = true).toRowUi(targeted = false).muted)
    }

    @Test fun `반이중·미참여 카드에는 음소거 토글이 없다`() {
        assertNull(privateCall(fullDuplex = false).toRowUi(targeted = false).muted)
        assertNull(card().toRowUi(targeted = false).muted)
    }

    @Test fun `채널 머리도 같은 규칙으로 음소거를 단다`() {
        val head = channelHead("1003", privateCall(fullDuplex = true, muted = true), range = null, targeted = false)
        assertEquals(true, head.muted)
        assertFalse(head.canTarget)
        assertNull(channelHead("g1", card(), range = null, targeted = false).muted)
    }
}
