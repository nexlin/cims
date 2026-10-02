// 관리 화면의 저장 판정 (docs/design/features/dispatch_desktop_ui.md §4.5)
//
// 앱이 내리는 유일한 판단은 «무엇이 바뀌었나» 다. 이걸 틀리면 저장할 때마다 서버가 H(A1) 재결박을
// 요구해 400 이 나거나(안 바뀐 회선을 보냄), IMSI 를 비워 보내 "IMSI 변경" 으로 오판된다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.AdminScope
import com.cims.ue.dispatch.session.AdminView
import com.cims.ue.dispatch.session.LineKind
import com.cims.ue.dispatch.session.MemberInfo
import com.cims.ue.dispatch.session.NumberInfo
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.session.ServiceRef
import com.cims.ue.dispatch.ui.admin.AdminViewModel.Companion.lineAction
import com.cims.ue.dispatch.ui.admin.AdminViewModel.Companion.linesToDelete
import com.cims.ue.dispatch.ui.admin.AdminViewModel.Companion.memberMatches
import com.cims.ue.dispatch.ui.admin.AdminViewModel.Companion.needsPassword
import com.cims.ue.dispatch.ui.admin.AdminViewModel.Companion.openLine
import com.cims.ue.dispatch.ui.admin.AdminViewModel.Companion.saveBlocker
import com.cims.ue.dispatch.ui.admin.AdminViewModel.Companion.scopeText
import com.cims.ue.dispatch.ui.admin.orgChoices
import com.cims.ue.dispatch.ui.admin.LineAction
import com.cims.ue.dispatch.ui.admin.LineForm
import com.cims.ue.dispatch.ui.admin.MemberForm
import com.cims.ue.dispatch.session.flattenOrgs
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
        assertFalse(MemberForm(name = "박").canSave)          // 소속 조직도 필수 — 데스크톱과 같은 선검사
        assertEquals("이름과 소속 조직은 필수입니다", MemberForm(name = "박").missing)
        assertTrue(MemberForm(name = "박", org = "HQ").canSave)
        assertEquals("", MemberForm(name = "박", org = "HQ").missing)
    }

    // ── 폼을 열 때의 회선 한 장 ──
    //
    // 기존 회선은 저장된 값 그대로다. 없는 회선은 새로 개설할 카드라 첫 후보·TLS 를 미리 골라 두되,
    // 그 기본값이 «변경» 으로 읽히면 안 된다(행을 누르기만 해도 «저장하지 않은 변경» 이 붙는다).
    private val view = AdminView(
        scope = AdminScope(groupId = "g", directoryWrite = "own", orgCode = "t1"),
        services = listOf(ServiceRef("volte", "volte-a"), ServiceRef("voip", "voip-a", "ims.example.org"),
            ServiceRef("voip", "voip-b")),
        orgs = listOf(OrgNode("hq", "본부"), OrgNode("t1", "팀01", "hq")))

    @Test fun `기존 회선은 저장된 값 그대로 연다`() {
        assertEquals(LineForm.of(saved), openLine(view, LineKind.VOLTE, saved))
        // 저장된 서비스가 후보에 없어도 바꾸지 않는다 — 바꾸면 저장마다 «서비스 변경» 이 된다
        val odd = saved.copy(serviceRef = "옛서비스", sipTransport = "ANY")
        assertEquals("옛서비스", openLine(view, LineKind.VOLTE, odd).serviceRef)
        assertEquals("ANY", openLine(view, LineKind.VOLTE, odd).sipTransport)
    }

    @Test fun `없는 회선은 첫 후보와 TLS 를 미리 고른다`() {
        val voip = openLine(view, LineKind.VOIP, null)
        assertEquals("voip-a", voip.serviceRef)
        assertEquals("TLS", voip.sipTransport)
        assertEquals("", voip.msisdn)
        assertEquals("", openLine(view, LineKind.PTT, null).serviceRef)     // 후보가 없으면 비운다
        assertEquals("voip-a", openLine(view, LineKind.VOIP, NumberInfo()).serviceRef)   // 빈 회선도 없는 회선이다
    }

    @Test fun `새 회선 카드의 기본값은 변경이 아니다`() {
        val m = MemberInfo(1L, "김순경", "kim", "t1", "경장", volte = saved)
        val opened = MemberForm(orig = m, name = m.name, title = m.title, org = m.org, loginId = m.loginId,
            lines = LineKind.all.associateWith { openLine(view, it, m.line(it)) })
        assertFalse(opened.dirty)
        assertEquals(LineAction.None, lineAction(m.voip, opened.lines.getValue(LineKind.VOIP)))   // 번호가 없으면 보낼 것도 없다
        assertTrue(opened.copy(lines = opened.lines + (LineKind.VOIP to
            opened.lines.getValue(LineKind.VOIP).copy(msisdn = "7001"))).dirty)
        assertFalse(MemberForm(lines = LineKind.all.associateWith { openLine(view, it, null) }).dirty)
    }

    // ── 저장 선검사 — 서버가 400 을 낼 것이 확실한 입력은 보내지 않는다 ──

    private fun formOf(m: MemberInfo?) = MemberForm(orig = m, name = m?.name ?: "새사람",
        lines = LineKind.all.associateWith { openLine(view, it, m?.line(it)) })

    private fun MemberForm.line(kind: String, f: (LineForm) -> LineForm) =
        copy(lines = lines + (kind to f(lines.getValue(kind))))

    @Test fun `바뀐 회선이 없으면 막지 않는다`() {
        assertEquals("", saveBlocker(formOf(MemberInfo(1L, "김순경", volte = saved))))
        assertEquals("", saveBlocker(formOf(null)))
    }

    @Test fun `새 회선에는 SIP 비밀번호가 필요하다`() {
        val f = formOf(null).line(LineKind.VOIP) { it.copy(msisdn = "7001") }
        assertTrue(saveBlocker(f).startsWith("VoIP 새 회선에는 SIP 비밀번호"))
        assertEquals("", saveBlocker(f.line(LineKind.VOIP) { it.copy(password = "pw") }))
    }

    @Test fun `새 회선인데 접속서비스 후보가 없으면 막는다`() {
        val f = formOf(null).line(LineKind.PTT) { it.copy(msisdn = "5001", password = "pw") }
        assertTrue(saveBlocker(f).startsWith("PTT 회선을 개설하려면 접속서비스"))
    }

    @Test fun `번호나 접속서비스를 바꾸면 비밀번호가 필요하고 transport 만 바꾸면 아니다`() {
        val f = formOf(MemberInfo(1L, "김순경", volte = saved))
        assertTrue(saveBlocker(f.line(LineKind.VOLTE) { it.copy(msisdn = "+821099998888") })
            .startsWith("VoLTE 번호를 바꾸려면"))
        assertTrue(saveBlocker(f.line(LineKind.VOLTE) { it.copy(serviceRef = "volte-b") })
            .startsWith("VoLTE 접속서비스를 바꾸려면"))
        assertEquals("", saveBlocker(f.line(LineKind.VOLTE) { it.copy(sipTransport = "ANY") }))
        assertEquals("", saveBlocker(f.line(LineKind.VOLTE) { it.copy(msisdn = "") }))      // 삭제에는 비밀번호가 없다
    }

    @Test fun `지울 회선 — 저장된 번호를 비운 것만`() {
        val m = MemberInfo(1L, "김순경", volte = saved, ptt = NumberInfo(msisdn = "5001"))
        val f = formOf(m)
        assertTrue(linesToDelete(f).isEmpty())
        assertEquals(listOf(LineKind.PTT to "5001"), linesToDelete(f.line(LineKind.PTT) { it.copy(msisdn = " ") }))
        assertEquals(listOf(LineKind.VOLTE to "+821011112222", LineKind.PTT to "5001"),
            linesToDelete(f.line(LineKind.PTT) { it.copy(msisdn = "") }.line(LineKind.VOLTE) { it.copy(msisdn = "") }))
        assertTrue(linesToDelete(formOf(null)).isEmpty())                                  // 없던 회선은 지울 것이 없다
    }

    // ── 검색·머리 글자 ──

    @Test fun `구성원 검색 — 이름·아이디·번호 어느 표기로든`() {
        val m = MemberInfo(1L, "김순경", "kim", "t1", volte = saved, ptt = NumberInfo(msisdn = "5001"))
        assertTrue(memberMatches(m, ""))
        assertTrue(memberMatches(m, " 순경 "))
        assertTrue(memberMatches(m, "KIM"))
        assertTrue(memberMatches(m, "5001"))
        assertTrue(memberMatches(m, "010-1111"))            // 로컬 표기로 쳐도 +8210… 회선이 걸린다
        assertTrue(memberMatches(m, "+82101111"))
        assertFalse(memberMatches(m, "9999"))
        assertFalse(memberMatches(m, "박"))
        assertFalse(memberMatches(m, "순경1"))              // 숫자가 섞인 이름 검색이 번호에 걸리지 않는다
    }

    @Test fun `관리 범위 문구 — 서버가 준 범위를 그대로 읽어 준다`() {
        assertEquals("관리 범위: 본부 › 팀01 하위", scopeText(view))
        assertEquals("관리 범위: 전체 조직", scopeText(view.copy(scope = AdminScope(directoryWrite = "all"))))
        assertEquals("", scopeText(view.copy(scope = AdminScope(directoryWrite = "none"))))
        assertEquals("관리 범위: x9 하위", scopeText(view.copy(scope = AdminScope(directoryWrite = "own", orgCode = "x9"))))
    }

    @Test fun `폼 머리의 이름은 입력을 따라간다`() {
        assertEquals("새 구성원", MemberForm().formName)
        assertEquals("새 구성원 등록", MemberForm().formSub)
        assertEquals("박", MemberForm(name = " 박 ").formName)
        val m = MemberInfo(7L, "김순경")
        assertEquals("#7", MemberForm(orig = m, name = "").formName)
        assertEquals("구성원 편집", MemberForm(orig = m, name = m.name).formSub)
    }

    @Test fun `조직 트리 평탄화 — 깊이 순서와 고아 보존`() {
        val flat = flattenOrgs(listOf(
            OrgNode("T", "팀01", "H", 1),
            OrgNode("H", "본부", "C", 1),
            OrgNode("C", "CIMS", "", 1),
            OrgNode("X", "고아", "없음", 9)))
        assertEquals(listOf("C" to 0, "H" to 1, "T" to 2, "X" to 0), flat.map { it.first.code to it.second })
    }

    @Test fun `조직 트리 평탄화 — 상위 고리가 있어도 잃지 않는다`() {
        val flat = flattenOrgs(listOf(OrgNode("A", "a", "B"), OrgNode("B", "b", "A")))
        assertEquals(setOf("A", "B"), flat.map { it.first.code }.toSet())
    }

    @Test fun `조직 트리 평탄화 — 부모가 목록에 없으면 그 노드가 뿌리다(범위로 잘린 트리)`() {
        // 주소록은 볼 수 있는 조직만 온다 — «CIMS» 없이 «본부» 부터 올 수 있다. 끝으로 밀려나지 않고 부모가 자식 앞에 선다.
        val flat = flattenOrgs(listOf(
            OrgNode("T2", "팀02", "H", 2),
            OrgNode("T1", "팀01", "H", 1),
            OrgNode("H", "본부", "C", 1),
            OrgNode("K", "관제그룹", "C", 0)))
        assertEquals(listOf("K" to 0, "H" to 0, "T1" to 1, "T2" to 1), flat.map { it.first.code to it.second })
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
