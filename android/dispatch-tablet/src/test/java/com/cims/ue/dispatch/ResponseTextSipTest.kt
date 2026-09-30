// 응답 문구 사전 — SIP 영역 (android_dispatch_tablet.md §10, dispatch_desktop_ui.md §9)
//
// 노리는 것은 문장이 **데스크톱과 갈라지는** 것과, 사전에 없는 응답을 **조용히 삼키는** 것이다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.Notice
import com.cims.ue.dispatch.session.NoticeBoard
import com.cims.ue.dispatch.session.NoticeLevel
import com.cims.ue.dispatch.session.Operation
import com.cims.ue.dispatch.session.ResponseText
import com.cims.ue.dispatch.session.TextArea
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ResponseTextSipTest {

    @Test fun `같은 코드도 영역마다 다르게 읽힌다`() {
        assertEquals("당겨받을 호가 없습니다", ResponseText.sip(TextArea.PICKUP, 404, "Not Found"))
        assertEquals("없는 번호입니다", ResponseText.sip(TextArea.CALL, 404, "Not Found"))
        assertEquals("상대를 찾을 수 없음", ResponseText.sip(TextArea.PTT_PRIVATE, 404, "Not Found"))
        assertEquals("전송 실패 — 재전송", ResponseText.sip(TextArea.SDS, 404, "Not Found"))
    }

    @Test fun `전달은 403 만 따로, 나머지 4xx 는 원 통화 유지`() {
        assertEquals("이 서비스는 호 전달이 허용되지 않습니다", ResponseText.sip(TextArea.TRANSFER, 403, ""))
        assertEquals("전달 대상이 응답하지 않아 원 통화를 유지합니다", ResponseText.sip(TextArea.TRANSFER, 480, ""))
    }

    @Test fun `사전에 없으면 원문을 괄호로 붙인다`() {
        assertEquals("실패 (500 Server Error)", ResponseText.sip(TextArea.CALL, 500, "Server Error"))
        assertEquals("실패 (500)", ResponseText.sip(TextArea.CALL, 500, ""))
    }

    @Test fun `코드가 없는 실패는 사유를 그대로 쓴다`() {
        assertEquals("동시 청취 상한 4", ResponseText.sip(TextArea.JOIN, -1, "동시 청취 상한 4"))
        assertEquals("실패", ResponseText.sip(TextArea.JOIN, -1, ""))
    }

    @Test fun `등록 실패 문장`() {
        assertEquals("인증 실패 — 다시 로그인", ResponseText.sip(TextArea.REGISTER, 403, ""))
        assertEquals("서버 응답 없음 — 재시도 중", ResponseText.sip(TextArea.REGISTER, 408, ""))
    }

    @Test fun `동작에서 영역을 고른다`() {
        assertEquals(TextArea.PICKUP, ResponseText.areaOf(Operation.PICKUP))
        assertEquals(TextArea.JOIN, ResponseText.areaOf(Operation.JOIN))
        assertEquals(TextArea.EMERGENCY, ResponseText.areaOf(Operation.EMERGENCY))
        assertEquals(TextArea.CALL, ResponseText.areaOf(Operation.DIAL))
    }

    // ── 토스트 규칙 ──
    private fun n(id: Long, level: NoticeLevel = NoticeLevel.ERROR) = Notice(id, level, "t$id")

    @Test fun `토스트는 최신 위, 여섯 장까지`() {
        var list = emptyList<Notice>()
        (1L..8L).forEach { list = NoticeBoard.push(list, n(it)) }
        assertEquals((8L downTo 3L).toList(), list.map { it.id })
        assertEquals(listOf(8L, 6L, 5L, 4L, 3L), NoticeBoard.dismiss(list, 7).map { it.id })
    }

    @Test fun `오류는 손으로 닫는다`() {
        assertFalse(n(1, NoticeLevel.ERROR).autoClose)
        assertTrue(n(1, NoticeLevel.WARN).autoClose)
        assertTrue(n(1, NoticeLevel.INFO).autoClose)
    }
}
