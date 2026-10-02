package com.cims.ue.ptt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** 호 수락·거절 사유 판정(CallRules) — 기기 없이 도는 것. */
class CallRulesTest {
    // TS 24.281 §6.2.3.2.2 2) — 원치 않는 초대는 받기 전에 거절한다(받고 나서 BYE 하지 않는다)
    @Test fun `영상 초대는 지금 영상 채널의 것만 받는다`() {
        assertTrue(CallRules.acceptVideoInvitation("g103", videoChannel = "g103", alreadyInCall = false))
        assertFalse("다른 채널", CallRules.acceptVideoInvitation("g103", videoChannel = "g001", alreadyInCall = false))
        assertFalse("영상 채널 없음", CallRules.acceptVideoInvitation("g103", videoChannel = null, alreadyInCall = false))
        assertFalse("이미 그 그룹 영상 호에 있다", CallRules.acceptVideoInvitation("g103", videoChannel = "g103", alreadyInCall = true))
        assertFalse(CallRules.acceptVideoInvitation("", videoChannel = "", alreadyInCall = false))
    }

    // TS 24.281 §9.2.1.2.4.1 — 망 끊김으로 잃은 prearranged 영상 호는 세션 식별자로 재합류한다
    @Test fun `재합류는 망 문제로 잃은 prearranged 호만`() {
        val uri = "sip:g103@csp;gr=1790-1"
        assertTrue(CallRules.rejoinVideoSession(prearranged = true, wasActive = true, lastCode = 408, sessionUri = uri))
        assertTrue(CallRules.rejoinVideoSession(true, true, 503, uri))
        assertFalse("서버의 정상 해제(BYE)", CallRules.rejoinVideoSession(true, true, 200, uri))
        assertFalse("성립한 적 없는 호", CallRules.rejoinVideoSession(true, false, 408, uri))
        assertFalse("chat 은 그냥 다시 합류한다", CallRules.rejoinVideoSession(false, true, 408, uri))
        assertFalse("세션 식별자를 모른다", CallRules.rejoinVideoSession(true, true, 408, ""))
        assertFalse("거절", CallRules.rejoinVideoSession(true, true, 403, uri))
    }

    // TS 24.379 §4.4.2 — 서버 거절 사유(486 103 · 486 122 · 403 115 …)
    @Test fun `거절 사유는 Warning 번호로 가른다`() {
        assertEquals("동시에 참여할 수 있는 그룹 통화 수를 넘었습니다", CallRules.rejectionText(486, 103))
        assertEquals("그룹 통화 정원이 찼습니다", CallRules.rejectionText(486, 122))
        assertEquals("사용이 중지된 그룹입니다", CallRules.rejectionText(403, 115))
        assertEquals("상대가 통화를 받지 않았습니다", CallRules.rejectionText(480, 110))
        assertNull("모르는 번호", CallRules.rejectionText(403, 999))
        assertNull("Warning 없음", CallRules.rejectionText(486, 0))
    }

    @Test fun `성립한 호의 Warning 은 실패 사유가 아니다`() {
        // 200 OK 의 Warning 122 = «정원 때문에 일부만 불렀다» — 호는 성립했다
        assertNull(CallRules.rejectionText(200, 122))
        assertNull(CallRules.rejectionText(0, 122))
    }
}
