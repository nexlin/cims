// 관리 화면의 저장 판정 (docs/design/features/dispatch_desktop_ui.md §4.5)
//
// 앱이 내리는 유일한 판단은 «무엇이 바뀌었나» 다. 이걸 틀리면 저장할 때마다 서버가 H(A1) 재결박을
// 요구해 400 이 나거나(안 바뀐 회선을 보냄), IMSI 를 비워 보내 "IMSI 변경" 으로 오판된다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.LineKind
import com.cims.ue.dispatch.session.MemberInfo
import com.cims.ue.dispatch.session.NumberInfo
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.ui.admin.AdminViewModel.Companion.lineAction
import com.cims.ue.dispatch.ui.admin.AdminViewModel.Companion.needsPassword
import com.cims.ue.dispatch.ui.admin.LineAction
import com.cims.ue.dispatch.ui.admin.LineForm
import com.cims.ue.dispatch.ui.admin.MemberForm
import com.cims.ue.dispatch.ui.admin.flatten
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AdminFormTest {

    private val saved = NumberInfo(msisdn = "+821011112222", imsi = "450051234567890",
                                   serviceRef = "volte-a", sipTransport = "TLS")

    @Test fun `바뀐 것이 없으면 보내지 않는다`() {
        assertEquals(LineAction.None, lineAction(saved, LineForm.of(saved)))
    }

    @Test fun `번호를 비우면 회선 삭제`() {
        assertEquals(LineAction.Delete, lineAction(saved, LineForm()))
    }

    @Test fun `없던 회선을 비운 채 두면 할 일 없음`() {
        assertEquals(LineAction.None, lineAction(null, LineForm()))
        assertEquals(LineAction.None, lineAction(NumberInfo(), LineForm()))
    }

    @Test fun `같은 번호를 다시 실으면 저장된 IMSI 를 그대로 싣는다`() {
        val act = lineAction(saved, LineForm.of(saved).copy(sipTransport = "ANY")) as LineAction.Put
        assertEquals("+821011112222", act.number.msisdn)
        assertEquals("450051234567890", act.number.imsi)      // 비우면 서버가 번호 숫자로 채워 오판한다
        assertEquals("ANY", act.number.sipTransport)
        assertEquals("", act.password)                        // transport 만 바뀌면 비밀번호 불필요
    }

    @Test fun `번호가 바뀌면 새 회선이라 IMSI 를 비운다`() {
        val act = lineAction(saved, LineForm.of(saved).copy(msisdn = "+821033334444", password = "pw")) as LineAction.Put
        assertEquals("", act.number.imsi)
        assertEquals("pw", act.password)
    }

    @Test fun `transport 만 바뀌어도 PUT 한다 — ANY 는 되돌릴 수 있는 명시값`() {
        assertTrue(lineAction(saved, LineForm.of(saved).copy(sipTransport = "ANY")) is LineAction.Put)
        val back = lineAction(saved.copy(sipTransport = "ANY"), LineForm.of(saved).copy(sipTransport = "TLS"))
        assertTrue(back is LineAction.Put)
    }

    @Test fun `비밀번호만 넣어도 PUT 한다 — 재결박 요청이다`() {
        assertTrue(lineAction(saved, LineForm.of(saved).copy(password = "pw")) is LineAction.Put)
    }

    @Test fun `비밀번호가 필요한 경우 — 새 회선·번호 변경·서비스 변경`() {
        assertTrue(needsPassword(null, LineForm(msisdn = "1001")))
        assertTrue(needsPassword(saved, LineForm.of(saved).copy(msisdn = "+821033334444")))
        assertTrue(needsPassword(saved, LineForm.of(saved).copy(serviceRef = "volte-b")))
        assertFalse(needsPassword(saved, LineForm.of(saved).copy(sipTransport = "UDP")))
        assertFalse(needsPassword(saved, LineForm()))         // 삭제에는 비밀번호가 없다
    }

    @Test fun `열림은 변경이 아니다 — 클릭만으로 폼이 열린다`() {
        val m = MemberInfo(1L, "김순경", "kim", "T1", "경장", volte = saved)
        val opened = MemberForm(orig = m, name = m.name, title = m.title, org = m.org, loginId = m.loginId,
            lines = LineKind.all.associateWith { LineForm.of(m.line(it)) })
        assertFalse(opened.dirty)
        assertTrue(opened.copy(title = "경사").dirty)
        assertTrue(opened.copy(password = "x").dirty)
        assertTrue(opened.copy(lines = opened.lines + (LineKind.PTT to LineForm(msisdn = "1001"))).dirty)
    }

    @Test fun `새 구성원은 이름이나 번호를 넣어야 변경으로 본다`() {
        assertFalse(MemberForm().dirty)
        assertTrue(MemberForm(name = "박").dirty)
        assertFalse(MemberForm().canSave)                     // 이름 없이는 저장 불가
        assertTrue(MemberForm(name = "박").canSave)
    }

    @Test fun `조직 트리 평탄화 — 깊이 순서와 고아 보존`() {
        val flat = flatten(listOf(
            OrgNode("T", "팀01", "H", 1),
            OrgNode("H", "본부", "C", 1),
            OrgNode("C", "CIMS", "", 1),
            OrgNode("X", "고아", "없음", 9)))
        assertEquals(listOf("C" to 0, "H" to 1, "T" to 2, "X" to 0), flat.map { it.first.code to it.second })
    }

    @Test fun `조직 트리 평탄화 — 상위 고리가 있어도 잃지 않는다`() {
        val flat = flatten(listOf(OrgNode("A", "a", "B"), OrgNode("B", "b", "A")))
        assertEquals(setOf("A", "B"), flat.map { it.first.code }.toSet())
    }
}
