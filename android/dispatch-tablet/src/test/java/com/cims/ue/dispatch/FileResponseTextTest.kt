// MCData 파일(FD) 응답 문구 — JVM, 기기 불필요 (android_dispatch_tablet.md §10, dispatch_desktop_ui.md §9 `Area.File`)
//
// FD 콘텐츠 서버는 403 하나로 여러 사유를 낸다(그룹의 파일 전송 꺼짐·비멤버·scope 부족) — 본문 `error` 로 갈라야 관제사가
// 무엇을 고쳐야 하는지 안다. SDK 는 실패 사유에 호출 이름을 앞에 붙여 준다(`uploadFd 403: {…}`) — 본문만 떼어 읽는다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.ResponseText
import com.cims.ue.dispatch.session.TextArea
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class FileResponseTextTest {

    @Test fun `상태코드 문구는 데스크톱 사전과 같다`() {
        assertEquals("로그인이 만료됐습니다 — 다시 로그인하세요", ResponseText.forStatus(TextArea.FILE, 401))
        assertEquals("파일 전송 권한이 없습니다", ResponseText.forStatus(TextArea.FILE, 403))
        assertEquals("그룹 또는 파일이 서버에 없습니다", ResponseText.forStatus(TextArea.FILE, 404))
        assertEquals("파일이 너무 큽니다 (서버 한도)", ResponseText.forStatus(TextArea.FILE, 413))
        assertEquals("서버 파일 저장소가 설정되지 않았습니다 (운영자)", ResponseText.forStatus(TextArea.FILE, 503))
        assertEquals("받을 수 없는 파일 주소입니다", ResponseText.forStatus(TextArea.FILE, -2))
        assertNull(ResponseText.forStatus(TextArea.FILE, 500))
    }

    @Test fun `본문 error 가 상태코드보다 먼저다 — 403 을 사유로 가른다`() {
        assertEquals("이 그룹은 파일 전송이 꺼져 있습니다 (그룹 설정 — 파일 전송 허용)",
            ResponseText.of(TextArea.FILE, 403, """{"error":"file distribution disabled for group g001"}"""))
        assertEquals("그룹 멤버가 아니라 파일을 보낼 수 없습니다",
            ResponseText.of(TextArea.FILE, 403, """{"error":"not a member of group g001"}"""))
        assertEquals("토큰 권한이 부족합니다 — 다시 로그인하세요",
            ResponseText.of(TextArea.FILE, 403, """{"error":"insufficient_scope"}"""))
        assertEquals("서버에 없는 그룹입니다", ResponseText.of(TextArea.FILE, 404, """{"error":"unknown group g9"}"""))
        assertEquals("파일이 서버에 없습니다 (보관 기간이 지났을 수 있습니다)",
            ResponseText.of(TextArea.FILE, 404, """{"error":"file not found"}"""))
        assertEquals("파일이 서버에 없습니다 (보관 기간이 지났을 수 있습니다)",
            ResponseText.of(TextArea.FILE, 404, """{"error":"file content missing"}"""))
        assertEquals("파일이 너무 큽니다 (서버 한도)", ResponseText.of(TextArea.FILE, 413, """{"error":"file too large (max 52428800)"}"""))
    }

    @Test fun `SDK 가 붙인 호출 이름을 떼고 본문을 읽는다`() {
        assertEquals("""{"error":"x"}""", ResponseText.jsonPart("""uploadFd 403: {"error":"x"}"""))
        assertEquals("request: timeout", ResponseText.jsonPart("request: timeout"))
        assertEquals("이 그룹은 파일 전송이 꺼져 있습니다 (그룹 설정 — 파일 전송 허용)",
            ResponseText.of(TextArea.FILE, 403, """uploadFd 403: {"error":"file distribution disabled"}"""))
    }

    @Test fun `모르는 본문은 상태코드 문구로, 그것도 없으면 원문으로`() {
        assertEquals("파일 전송 권한이 없습니다", ResponseText.of(TextArea.FILE, 403, """{"error":"something else"}"""))
        assertEquals("받을 수 없는 파일 주소입니다", ResponseText.of(TextArea.FILE, -2, "downloadFd: not a FD url"))
        assertEquals("서버에 닿지 않습니다 (-1)", ResponseText.of(TextArea.FILE, -1, "request: connect failed"))
        assertEquals("oops (500)", ResponseText.of(TextArea.FILE, 500, """{"error":"oops"}"""))
    }
}
