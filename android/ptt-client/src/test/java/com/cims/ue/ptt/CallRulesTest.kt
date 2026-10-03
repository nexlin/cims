package com.cims.ue.ptt

import com.cims.ue.sdk.CommencementMode
import com.cims.ue.sdk.SdsPayload
import com.cims.ue.sdk.SdsPayloadType
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

    // TS 24.379 §11.1.1.2.1.1 14) — 상대 응답 방식은 고른 값, 인가가 없는 방식은 싣지 않는다(서버 403 125·126·143 전에)
    @Test fun `개별 통화 상대 응답은 인가된 방식만`() {
        val all = CommencementMode.entries.toSet()
        val noForce = setOf(CommencementMode.UNSPECIFIED, CommencementMode.AUTO, CommencementMode.MANUAL)
        assertEquals(CommencementMode.MANUAL, CallRules.effectiveCommencement(CommencementMode.MANUAL, all))
        assertEquals(CommencementMode.UNSPECIFIED, CallRules.effectiveCommencement(CommencementMode.FORCE_AUTO, noForce))
        assertEquals("프로파일 미수신 — 고른 값 그대로", CommencementMode.FORCE_AUTO,
            CallRules.effectiveCommencement(CommencementMode.FORCE_AUTO, null))
        assertEquals("개별 통화 인가 없음", CommencementMode.UNSPECIFIED,
            CallRules.effectiveCommencement(CommencementMode.AUTO, emptySet()))
    }

    // TS 24.282 §9.2.1.2 6)d) — 글이 아닌 payload(위치·이진)도 말풍선에 한 줄씩
    @Test fun `문자 말풍선은 위치와 이진 payload 를 덧붙인다`() {
        fun p(type: Int, data: ByteArray = ByteArray(0), text: String = "", charset: Int = 0, loc: Boolean = false,
              lat: Double = 0.0, lon: Double = 0.0) = SdsPayload(type, data, text, charset, loc, lat, lon)
        val loc = p(SdsPayloadType.LOCATION, loc = true, lat = 37.5665, lon = 126.978)
        assertEquals("안녕\n위치 37.56650, 126.97800", CallRules.sdsDisplayText("안녕", listOf(p(SdsPayloadType.TEXT, text = "안녕"), loc)))
        assertEquals("위치 37.56650, 126.97800", CallRules.sdsDisplayText("", listOf(loc)))
        assertEquals("이진 데이터 3바이트", CallRules.sdsDisplayText("", listOf(p(SdsPayloadType.BINARY, ByteArray(3)))))
        assertEquals("읽을 수 없는 문자 집합(38)", CallRules.sdsDisplayText("", listOf(p(SdsPayloadType.CODED_TEXT, ByteArray(2), charset = 38))))
        assertEquals("", CallRules.sdsDisplayText("", listOf(p(SdsPayloadType.LOCATION_ALTITUDE, ByteArray(2)))))
    }

    // TS 24.379 §11.1.1.2.1.2 10) — 수동 응답 요청 착신은 받기 전까지 [받기]·[거절]
    @Test fun `수동 응답 착신만 수락을 기다린다`() {
        assertTrue(CallRules.awaitingAnswer(incoming = true, ringing = true, commencement = CommencementMode.MANUAL))
        assertFalse("코어가 자동으로 받는다", CallRules.awaitingAnswer(true, true, CommencementMode.AUTO))
        assertFalse("강제 자동", CallRules.awaitingAnswer(true, true, CommencementMode.FORCE_AUTO))
        assertFalse("이미 받았다", CallRules.awaitingAnswer(true, false, CommencementMode.MANUAL))
        assertFalse("발신", CallRules.awaitingAnswer(false, true, CommencementMode.MANUAL))
    }

    // TS 24.379 §10.1.1.2.4.1 · TS 24.281 §9.2.1.2.4.1 — 망 끊김으로 잃은 prearranged 호(무전·영상)는 세션 식별자로 재합류한다
    @Test fun `재합류는 망 문제로 잃은 prearranged 호만`() {
        val uri = "sip:g103@csp;gr=1790-1"
        assertTrue(CallRules.rejoinLostSession(prearranged = true, wasActive = true, lastCode = 408, sessionUri = uri))
        assertTrue(CallRules.rejoinLostSession(true, true, 503, uri))
        assertFalse("서버의 정상 해제(BYE)", CallRules.rejoinLostSession(true, true, 200, uri))
        assertFalse("성립한 적 없는 호", CallRules.rejoinLostSession(true, false, 408, uri))
        assertFalse("chat 은 그냥 다시 합류한다", CallRules.rejoinLostSession(false, true, 408, uri))
        assertFalse("세션 식별자를 모른다", CallRules.rejoinLostSession(true, true, 408, ""))
        assertFalse("거절", CallRules.rejoinLostSession(true, true, 403, uri))
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

    // TS 24.282 §4.9 — 그룹 SDS·FD 거절 사유(403 116·206·213·217)
    @Test fun `문자 전송 거절 사유는 Warning 번호로 가른다`() {
        assertEquals("이 그룹의 멤버가 아닙니다", CallRules.sendRejectionText(403, 116))
        assertEquals("이 그룹은 문자를 쓸 수 없습니다", CallRules.sendRejectionText(403, 206))
        assertEquals("이 그룹은 파일 전송을 쓸 수 없습니다", CallRules.sendRejectionText(403, 213))
        assertEquals("메시지가 너무 큽니다", CallRules.sendRejectionText(403, 217))
        assertNull(CallRules.sendRejectionText(403, 0))
        assertNull("성공 응답", CallRules.sendRejectionText(200, 217))
    }

    // TS 24.282 §9.2.1.1 1) → §11.1 — 보내기 전에 그룹 문서의 문자 허용·크기 상한을 본다
    @Test fun `그룹 문자는 보내기 전에 허용과 크기를 본다`() {
        assertNull(CallRules.sdsBlockReason(allowSds = true, maxSdsBytes = 100, payloadBytes = 100))
        assertEquals("메시지가 너무 큽니다(최대 100바이트)", CallRules.sdsBlockReason(true, 100, 101))
        assertEquals("이 그룹은 문자를 쓸 수 없습니다", CallRules.sdsBlockReason(false, null, 1))
        assertNull("상한 없음", CallRules.sdsBlockReason(true, null, 1_000_000))
        assertNull("0 = 상한 없음", CallRules.sdsBlockReason(true, 0, 1_000_000))
        assertNull("문서를 아직 받지 못했다 — 서버가 판정한다", CallRules.sdsBlockReason(null, null, 5))
    }

    @Test fun `성립한 호의 Warning 은 실패 사유가 아니다`() {
        // 200 OK 의 Warning 122 = «정원 때문에 일부만 불렀다» — 호는 성립했다
        assertNull(CallRules.rejectionText(200, 122))
        assertNull(CallRules.rejectionText(0, 122))
    }
}
