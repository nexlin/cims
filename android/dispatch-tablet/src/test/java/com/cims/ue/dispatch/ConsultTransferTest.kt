// 상담 전달(attended) — 카드 조작 자격 (android_dispatch_tablet.md §6.6, dispatch_desktop_ui.md §4.3)
//
// 노리는 것은 **엉뚱한 호에 전달 조작이 붙는** 것이다 — 상담 호에 다시 [전달] 이 붙으면 상담 호를 또 전달하는 꼴이 되고,
// 연결 전의 상담 호에 [전달 완결] 이 붙으면 Replaces 가 성립하지 않는다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.AccountKind
import com.cims.ue.dispatch.session.Operation
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.ui.call.CallCard
import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.McpttInfo
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ConsultTransferTest {

    private fun card(state: CallState, consultFor: Int? = null): CallCard {
        val info = CallInfo(
            callId = 2, accountId = 0, dir = CallDir.OUTGOING, state = state,
            remoteUri = "sip:1003@d", calledParty = "", video = false, mediaActive = true,
            muted = false, listen = true, playbackRoute = 0, lastCode = 0, lastReason = "",
            sources = emptyList(), isMcptt = false, groupId = "",
            mcptt = McpttInfo(false, "", "", "", "", false, false, false, false),
            halfDuplex = false, listenOnly = false, joinedDialog = "")
        return CallCard(SessionItem(2, AccountKind.PHONE, Operation.TRANSFER, info, consultFor = consultFor))
    }

    @Test fun `통화·보류 중인 원 통화는 전달할 수 있다`() {
        assertTrue(card(CallState.ACTIVE).canTransfer)
        assertTrue(card(CallState.HELD).canTransfer)
        assertFalse("연결 전에는 넘길 통화가 없다", card(CallState.OUTGOING).canTransfer)
    }

    @Test fun `상담 호에는 전달이 없고 연결되면 완결만 있다`() {
        val ringing = card(CallState.OUTGOING, consultFor = 1)
        assertTrue(ringing.consult)
        assertFalse(ringing.canTransfer)
        assertFalse("상대가 받기 전에는 Replaces 가 성립하지 않는다", ringing.canComplete)
        assertTrue(card(CallState.ACTIVE, consultFor = 1).canComplete)
    }
}
