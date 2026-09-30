// 주소록 검색·표시 규칙 (docs/design/features/android_dispatch_tablet.md §6.2)
//
// 핵심은 **표시와 동작을 가르는 것**이다(identifier_model.md) — 이름으로 찾되 걸 때는 번호로 건다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.ui.call.filter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ContactsTest {

    private val book = DirectoryBook(
        orgs = listOf(
            OrgNode("C", "CIMS"),
            OrgNode("H", "본부", "C"),
            OrgNode("T1", "팀01", "H"),
            OrgNode("T2", "팀02", "H")),
        entries = listOf(
            DirectoryEntry("T1", "김순경", "01011112222"),
            DirectoryEntry("T2", "이순경", "+821033334444"),
            DirectoryEntry("H", "박경위", "1003"),
            DirectoryEntry("", "무소속", "01055556666")))

    // ── 번호를 치는 동안의 제안(데스크톱 ③ 번호 필드) ──
    @Test fun `치는 번호가 로컬 표기든 E164 든 같은 사람에 걸린다`() {
        assertEquals(listOf("이순경"), book.suggest("010333").map { it.name })     // 저장은 +8210…
        assertEquals(listOf("김순경"), book.suggest("+8210111").map { it.name })   // 저장은 010…
        assertEquals(listOf("박경위"), book.suggest("100").map { it.name })        // 내선은 그대로
    }

    @Test fun `이름으로도 제안한다`() {
        assertEquals(listOf("김순경", "이순경"), book.suggest("순경").map { it.name })
    }

    @Test fun `URI 를 치는 중이거나 비었으면 제안하지 않는다`() {
        assertTrue(book.suggest("sip:1003@d").isEmpty())
        assertTrue(book.suggest("  ").isEmpty())
    }

    @Test fun `제안은 여덟까지`() {
        val many = DirectoryBook(entries = (1..20).map { DirectoryEntry("", "사람$it", "0101000%04d".format(it)) })
        assertEquals(8, many.suggest("010").size)
    }

    @Test fun `이름으로 찾는다`() {
        assertEquals(listOf("김순경"), filter(book, "김순", "").map { it.name })
    }

    @Test fun `번호는 정규형으로 비교한다 — 로컬 표기와 E164 가 같은 사람`() {
        // 서버가 +8210333… 으로 준 사람을 010333… 으로 검색해도 걸려야 한다
        assertEquals(listOf("이순경"), filter(book, "01033334444", "").map { it.name })
        assertEquals(listOf("이순경"), filter(book, "+821033334444", "").map { it.name })
    }

    @Test fun `내선처럼 짧은 번호는 국가코드를 붙이지 않는다`() {
        assertEquals("1003", DirectoryBook.normalize("1003"))
        assertEquals(listOf("박경위"), filter(book, "1003", "").map { it.name })
    }

    @Test fun `조직 필터는 하위를 포함한다`() {
        assertEquals(setOf("김순경", "이순경", "박경위"), filter(book, "", "H").map { it.name }.toSet())
        assertEquals(listOf("김순경"), filter(book, "", "T1").map { it.name })
        // 최상위를 고르면 소속이 있는 사람만 — 무소속은 조직 트리에 없다
        assertTrue("무소속" !in filter(book, "", "C").map { it.name })
    }

    @Test fun `빈 질의는 전부 이름순`() {
        assertEquals(listOf("김순경", "무소속", "박경위", "이순경"), filter(book, "", "").map { it.name })
    }

    @Test fun `조직 경로는 루트부터`() {
        assertEquals("CIMS › 본부 › 팀01", book.orgPath("T1"))
        assertEquals("", book.orgPath(""))
    }

    @Test fun `이름 조회는 표기 차이를 넘는다`() {
        assertEquals("이순경", book.nameOf("01033334444"))
        assertEquals("김순경", book.nameOf("+821011112222"))
        assertEquals("", book.nameOf("01099999999"))
    }
}

/**
 * 주소록 축 — **전화 가족(volte∪voip)과 PTT 는 갈라 든다**.
 *
 * 서버(`csc handle_provisioning_directory`)는 `service=volte` 에 이동·유선을 합산해 주고 `service=voip` 는
 * 유선만 준다. 관제 회선이 유선이라고 `voip` 로 물으면 전화번호부가 거의 비고, PTT 를 섞으면 전화로
 * 걸리지 않는 번호가 발신 목록에 뜬다.
 */
class DirectoryAxisTest {

    private val phone = DirectoryBook(entries = listOf(
        DirectoryEntry("T1", "김순경", "01011112222"),      // 이동
        DirectoryEntry("T1", "이순경", "1002")))            // 유선 내선 — volte 축에 합산돼 온다
    private val ptt = DirectoryBook(entries = listOf(
        DirectoryEntry("T1", "김순경", "5001"),
        DirectoryEntry("T2", "무전만", "5002")))

    @Test fun `발신 목록에는 전화 번호만 — PTT 번호는 걸리지 않는다`() {
        val dialable = filter(phone, "", "").map { it.msisdn }
        assertTrue("5002" !in dialable)
        assertEquals(setOf("01011112222", "1002"), dialable.toSet())
    }

    @Test fun `이름 표시는 둘을 합친다 — 같은 사람이다`() {
        // 표시용 색인은 전화 + PTT 합산이라 PTT 번호로 온 이벤트에도 이름이 붙는다
        val idx = (phone.entries + ptt.entries)
            .filter { it.name.isNotBlank() }
            .associate { DirectoryBook.normalize(it.msisdn) to it.name }
        assertEquals("김순경", idx[DirectoryBook.normalize("01011112222")])
        assertEquals("무전만", idx["5002"])
    }
}

/**
 * 화면이 읽는 파생 값은 **순수 함수**여야 한다 — VM 의 현재 값을 읽는 형태면 Compose 가 그 읽기를
 * 추적하지 못해, 늦게 도착한 주소록·새로고침한 접속서비스가 화면에 실리지 않는다.
 */
class DerivedPureTest {

    @Test fun `접속서비스 후보 — 저장값이 후보에 없어도 보인다`() {
        val view = com.cims.ue.dispatch.session.AdminView(
            services = listOf(
                com.cims.ue.dispatch.session.ServiceRef("volte", "volte-a"),
                com.cims.ue.dispatch.session.ServiceRef("voip", "voip-a")))
        val pick = com.cims.ue.dispatch.ui.admin.AdminViewModel.serviceChoicesOf(view, "volte", "옛서비스")
        assertEquals(listOf("volte-a", "옛서비스"), pick.map { it.name })
        // 저장값이 후보에 있으면 더 붙이지 않는다
        assertEquals(listOf("volte-a"),
            com.cims.ue.dispatch.ui.admin.AdminViewModel.serviceChoicesOf(view, "volte", "volte-a").map { it.name })
        // 빈 값이면 그대로
        assertEquals(listOf("voip-a"),
            com.cims.ue.dispatch.ui.admin.AdminViewModel.serviceChoicesOf(view, "voip", "").map { it.name })
    }

    @Test fun `그룹 멤버 후보 — 이미 멤버인 번호는 빠진다`() {
        val book = DirectoryBook(entries = listOf(
            DirectoryEntry("T", "김순경", "1001"),
            DirectoryEntry("T", "이순경", "1002")))
        val form = com.cims.ue.dispatch.ui.groups.EditForm(
            members = listOf(com.cims.ue.dispatch.ui.groups.MemberRow("tel:1001", "김순경", "1001")))
        val c = com.cims.ue.dispatch.ui.groups.PttGroupsViewModel.candidatesOf(book, form)
        assertEquals(listOf("1002"), c.map { it.msisdn })
    }

    @Test fun `표시된 전원 추가 — 이미 멤버·같은 묶음의 중복은 건너뛰고 참가자로 든다`() {
        val form = com.cims.ue.dispatch.ui.groups.EditForm(
            members = listOf(com.cims.ue.dispatch.ui.groups.MemberRow("tel:1001", "김순경", "1001", isChair = true)))
        val shown = listOf(
            DirectoryEntry("T", "김순경", "1001"),        // 이미 멤버
            DirectoryEntry("T", "", "01022223333"),        // 이름 없음 — 주소록 이름으로
            DirectoryEntry("T", "중복", "+821022223333"),  // 위와 같은 사람(정규형)
            DirectoryEntry("T", "나", "1009"))
        val out = com.cims.ue.dispatch.ui.groups.PttGroupsViewModel.withEntries(form, shown, "1009") { "이순경" }
        assertEquals(listOf("1001", "01022223333", "1009"), out.members.map { it.number })
        assertEquals("이순경", out.members[1].name)
        assertTrue("나는 나로 표시", out.members[2].isMe)
        assertTrue("더한 사람은 참가자", out.members.drop(1).none { it.isChair })
    }

    @Test fun `그룹 멤버 후보 — 폼이 없으면 빈 목록`() {
        assertTrue(com.cims.ue.dispatch.ui.groups.PttGroupsViewModel.candidatesOf(
            DirectoryBook(entries = listOf(DirectoryEntry("T", "김", "1001"))), null).isEmpty())
    }
}
