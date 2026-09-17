// 응답 문구 사전 — 데스크톱 `Services/ResponseText.cs` 와 같은 문장 (dispatch_desktop_ui.md §9)
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.ResponseText
import com.cims.ue.dispatch.session.TextArea
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ResponseTextTest {

    @Test fun `오류 본문의 error 가 상태코드보다 세분이다`() {
        val t = ResponseText.of(TextArea.MANAGEMENT, 403, """{"error":"no_directory_admin"}""")
        assertTrue(t.contains("관제 그룹의 관리 범위"))
    }

    @Test fun `number_exists 는 where 로 문구가 갈린다`() {
        assertTrue(ResponseText.of(TextArea.MANAGEMENT, 409,
            """{"error":"number_exists","where":"phone_groups"}""").contains("대표번호"))
        assertTrue(ResponseText.of(TextArea.MANAGEMENT, 409,
            """{"error":"number_exists","where":"voip_subscriptions"}""").contains("VoIP"))
        assertTrue(ResponseText.of(TextArea.MANAGEMENT, 409,
            """{"error":"number_exists"}""").contains("다른 구성원"))
    }

    @Test fun `schema_not_migrated 는 detail 로 유선 테이블을 가른다`() {
        assertTrue(ResponseText.forManagementError("schema_not_migrated", "voip_subscriptions 없음")!!
            .contains("migrate_voip_subscriptions.sql"))
        assertTrue(ResponseText.forManagementError("schema_not_migrated", "")!!
            .contains("DB 마이그레이션"))
    }

    @Test fun `서버의 문장형 오류는 접두 매칭으로 번역한다`() {
        assertTrue(ResponseText.forManagementError("passwd required when imsi or service_ref changes")!!
            .contains("H(A1) 재결박"))
        assertTrue(ResponseText.forManagementError("service_ref required for voip line")!!
            .contains("유선(voip) 접속서비스"))
        assertNull(ResponseText.forManagementError("완전히 모르는 오류"))
    }

    @Test fun `GMS 그룹 오류`() {
        assertTrue(ResponseText.of(TextArea.GROUP, 409, """{"error":"uri_taken"}""").contains("다른 사용자"))
        assertTrue(ResponseText.of(TextArea.GROUP, 412, """{"error":"etag_mismatch"}""").contains("다시 읽어"))
        assertTrue(ResponseText.of(TextArea.GROUP, 400,
            """{"error":"unknown_member","detail":["1001","1002"]}""").contains("1001"))
    }

    @Test fun `사전에 없으면 서버 문장을 그대로 보인다 — 조용히 삼키지 않는다`() {
        val t = ResponseText.of(TextArea.MANAGEMENT, 500, "boom")
        assertTrue(t.contains("boom"))
    }

    @Test fun `연결 실패는 음수 코드로 구분한다`() {
        assertTrue(ResponseText.of(TextArea.MANAGEMENT, -1, "").contains("서버에 닿지"))
    }

    @Test fun `JSON 이 아닌 본문은 통째로 error 로 본다`() {
        val (e, d, w) = ResponseText.parse("plain text")
        assertEquals("plain text", e)
        assertEquals("", d)
        assertEquals("", w)
    }
}

/**
 * 자격 만료 판정 — **되살릴 수 없는 것만** 앱 전체 로그아웃이다.
 *
 * 관제석은 상시 켜 두는 자리라 네트워크 흔들림으로 로그인 화면에 튕기면 만료로 튕기는 것보다 나쁘다.
 * CSC 는 폐기·만료·회전 실패를 전부 `400 {"error":"invalid_grant"}` 로 낸다(csc `mcptt.py` refresh 핸들러).
 */
class SessionEndedTest {

    // DispatchSession 의 판정과 같은 규칙(그쪽은 인스턴스 메서드라 여기서 규칙만 고정한다).
    private fun ended(code: Int, reason: String): Boolean =
        code == 401 || (code == 400 && reason.contains("invalid_grant"))

    @Test fun `폐기·만료된 자격은 세션 종료다`() {
        assertTrue(ended(400, """refresh 400: {"error":"invalid_grant"}"""))
        assertTrue(ended(401, "unauthorized"))
    }

    @Test fun `네트워크·서버 장애는 세션 종료가 아니다`() {
        assertFalse(ended(-1, "refresh: connection refused"))   // 코어의 전송 실패는 음수 코드
        assertFalse(ended(503, "service unavailable"))
        assertFalse(ended(500, "internal error"))
        assertFalse(ended(0, ""))
    }

    @Test fun `400 이어도 다른 사유면 종료가 아니다`() {
        assertFalse(ended(400, """refresh 400: {"error":"invalid_request"}"""))
    }
}
