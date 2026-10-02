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
        // CSP 응답 코드 표(mcptt_standard_conformance.md C4e) — 없는 그룹 404 113 · 재합류 미인가 403 121 · 애드혹 403 185·186·189 · 운용 시간 밖 403 100
        assertEquals("없는 그룹입니다", CallRules.rejectionText(404, 113))
        assertEquals("이 그룹 통화에 참여할 권한이 없습니다", CallRules.rejectionText(403, 121))
        assertEquals("애드혹 통화 권한이 없습니다", CallRules.rejectionText(403, 185))
        assertEquals("애드혹 통화를 지원하지 않는 시스템입니다", CallRules.rejectionText(403, 186))
        assertEquals("애드혹 통화에 부를 수 있는 인원을 넘었습니다", CallRules.rejectionText(403, 189))
        assertEquals("지금은 이 그룹으로 통화할 수 없습니다", CallRules.rejectionText(403, 100))
        assertNull("모르는 번호", CallRules.rejectionText(403, 999))
        assertNull("Warning 없음", CallRules.rejectionText(486, 0))
    }

    // TS 24.379 §10.1.1.2.1.1 · TS 24.281 §9.2.1.2.1.1 — <preconfigured-group-use-only> true 면 호·경보를 열지 않는다
    @Test fun `사전 구성 전용 그룹으로는 호를 열지 않는다`() {
        assertFalse(CallRules.groupUsable(true))
        assertTrue(CallRules.groupUsable(false))
        assertTrue("문서를 아직 받지 못했다 — 서버가 판정한다", CallRules.groupUsable(null))
        assertEquals("이 그룹으로는 통화할 수 없습니다", CallRules.rejectionText(403, 167))
        assertEquals("이 그룹에는 경보를 보낼 수 없습니다", CallRules.rejectionText(403, 168))
    }

    @Test fun `성립한 호의 Warning 은 실패 사유가 아니다`() {
        // 200 OK 의 Warning 122 = «정원 때문에 일부만 불렀다» — 호는 성립했다
        assertNull(CallRules.rejectionText(200, 122))
        assertNull(CallRules.rejectionText(0, 122))
    }
}
