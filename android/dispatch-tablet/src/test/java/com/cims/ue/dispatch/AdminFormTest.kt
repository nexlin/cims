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
import com.cims.ue.dispatch.ui.admin.orgChoices
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

    // ── 조직 선택 목록 (상위 조직 콤보) ──
    //
    // 자기 자신·자손을 상위로 고르면 고리가 된다. 서버가 막더라도 **고를 수 있게 두면 안 된다** —
    // 고르고 저장해서 400 을 보는 UI 는 그 자체가 결함이다.
    private val tree = listOf(
        OrgNode("hq", "본부"),
        OrgNode("t1", "팀01", "hq"),
        OrgNode("t1a", "반01", "t1"),
        OrgNode("t2", "팀02", "hq"))

    @Test fun `뺄 것이 없으면 전부 준다`() {
        assertEquals(4, orgChoices(tree).size)
    }

    @Test fun `자기와 자손을 뺀다`() {
        val codes = orgChoices(tree, excludeSubtreeOf = "t1").map { it.first.code }
        assertTrue(codes.contains("hq"))
        assertTrue(codes.contains("t2"))
        assertFalse(codes.contains("t1"))
        assertFalse(codes.contains("t1a"))
    }

    @Test fun `루트를 빼면 전부 사라진다`() {
        assertTrue(orgChoices(tree, excludeSubtreeOf = "hq").isEmpty())
    }

    @Test fun `빈 코드는 아무것도 빼지 않는다`() {
        assertEquals(4, orgChoices(tree, excludeSubtreeOf = "").size)
        assertEquals(4, orgChoices(tree, excludeSubtreeOf = null).size)
    }

    // 고리가 이미 있는 자료(서버 오류·경합)에서도 멈추지 않아야 한다.
    @Test fun `고리가 있어도 끝난다`() {
        val cyc = listOf(OrgNode("a", "A", "b"), OrgNode("b", "B", "a"))
        assertEquals(2, orgChoices(cyc).size)
        assertEquals(0, orgChoices(cyc, excludeSubtreeOf = "a").size)
    }

    @Test fun `깊이가 들여쓰기의 근거다`() {
        val d = orgChoices(tree).associate { it.first.code to it.second }
        assertEquals(0, d["hq"])
        assertEquals(1, d["t1"])
        assertEquals(2, d["t1a"])
    }
}
