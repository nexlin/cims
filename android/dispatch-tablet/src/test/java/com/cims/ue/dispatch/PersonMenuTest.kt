// 사람 메뉴 계약 시험 — JVM, 기기 불필요 (android_dispatch_tablet.md §9)
//
// 노리는 것은 **엉뚱한 사람에게 걸게 되는** 자리다. 서버 전화번호부는 사람이 아니라 회선을 주므로,
// 묶는 규칙이 한 칸만 어긋나도 A 의 칩에서 B 에게 사설콜이 나간다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.session.GroupInfo
import com.cims.ue.dispatch.ui.SearchHit
import com.cims.ue.dispatch.ui.channelMeta
import com.cims.ue.dispatch.ui.mergePeople
import com.cims.ue.dispatch.ui.searchDirectory
import com.cims.ue.dispatch.ui.resolvePerson
import com.cims.ue.dispatch.ui.userPart
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PersonMenuTest {

    private fun PersonEntryOf(name: String, ext: String) =
        com.cims.ue.dispatch.ui.PersonEntry(name = name, extension = ext)


    private val orgs = listOf(OrgNode("t1", "팀01", "hq"), OrgNode("hq", "본부"))
    private fun book(vararg e: Triple<String, String, String>) =
        DirectoryBook(orgs = orgs, entries = e.map { DirectoryEntry(it.first, it.second, it.third) })

    // ── 묶기 ──
    @Test fun `같은 사람의 PTT 와 내선이 한 항목이 된다`() {
        val ptt = book(Triple("t1", "이순경", "+821011112222"))
        val phone = book(Triple("t1", "이순경", "1001"))
        val people = mergePeople(phone, ptt)
        assertEquals(1, people.size)
        assertEquals("+821011112222", people[0].pttNumber)
        assertEquals("1001", people[0].extension)
        assertEquals("CIMS".let { "본부 › 팀01" }, people[0].orgPath)
        assertTrue(people[0].head.contains("PTT"))
        assertTrue(people[0].head.contains("내선"))
    }

    @Test fun `소속이 다르면 동명이인이라 따로 선다`() {
        val ptt = book(Triple("t1", "이순경", "+821011112222"))
        val phone = book(Triple("hq", "이순경", "1001"))
        assertEquals(2, mergePeople(phone, ptt).size)
    }

    // 이름이 없는 행끼리 합치면 남남이 한 사람이 된다 — 번호가 키여야 한다.
    @Test fun `이름 없는 행은 번호로 구분된다`() {
        val ptt = book(Triple("t1", "", "+821011112222"))
        val phone = book(Triple("t1", "", "1001"))
        val people = mergePeople(phone, ptt)
        assertEquals(2, people.size)
        assertTrue(people.none { it.hasPtt && it.hasLine })
    }

    // 회선을 이미 둘 가진 항목에 같은 키가 또 오면 합치지 않는다 — 어느 쪽이 맞는지 알 방법이 없다.
    @Test fun `회선이 셋이면 세 번째는 따로 선다`() {
        val ptt = book(Triple("t1", "이순경", "+821011112222"))
        val phone = book(Triple("t1", "이순경", "1001"), Triple("t1", "이순경", "1002"))
        val people = mergePeople(phone, ptt)
        assertEquals(2, people.size)
        assertEquals(1, people.count { it.hasPtt && it.hasLine })
    }

    @Test fun `내 회선은 목록에서 빠진다`() {
        val ptt = book(Triple("t1", "나", "+821011112222"), Triple("t1", "남", "+821033334444"))
        val phone = book(Triple("t1", "나", "1001"))
        val me = setOf(DirectoryBook.normalize("+821011112222"), DirectoryBook.normalize("1001"))
        val people = mergePeople(phone, ptt, exclude = me)
        assertEquals(1, people.size)
        assertEquals("남", people[0].name)
    }

    // 전화번호부의 `01012345678` 과 서버의 `+821012345678` 은 한 번호다.
    @Test fun `내 회선 제외는 정규형으로 비교한다`() {
        val ptt = book(Triple("t1", "나", "01011112222"))
        val people = mergePeople(DirectoryBook(), ptt, exclude = setOf(DirectoryBook.normalize("+821011112222")))
        assertTrue(people.isEmpty())
    }

    // ── 찾기 ──
    @Test fun `PTT 번호로도 내선으로도 같은 사람에 닿는다`() {
        val ptt = book(Triple("t1", "이순경", "+821011112222"))
        val phone = book(Triple("t1", "이순경", "1001"))
        val people = mergePeople(phone, ptt)
        val byPtt = resolvePerson(people, "sip:+821011112222@d", phone)
        val byExt = resolvePerson(people, "sip:1001@d", phone)
        assertEquals("이순경", byPtt?.name)
        assertEquals("이순경", byExt?.name)
    }

    // 주소록에 없는 상대에도 메뉴가 떠야 한다 — 빈 메뉴보다 «통화» 하나라도 있는 편이 낫다.
    @Test fun `주소록에 없으면 그 번호만 가진 항목을 만든다`() {
        val phone = book(Triple("t1", "이순경", "1001"))
        val made = resolvePerson(emptyList(), "sip:1001@d", phone)
        assertTrue(made!!.hasLine)
        assertFalse(made.hasPtt)
        val unknown = resolvePerson(emptyList(), "sip:9999@d", phone)
        assertTrue(unknown!!.hasPtt)      // 전화번호부에 없으면 PTT 쪽으로 본다
        assertFalse(unknown.hasLine)
    }

    @Test fun `번호가 없으면 항목을 만들지 않는다`() {
        assertNull(resolvePerson(emptyList(), "", DirectoryBook()))
        assertNull(resolvePerson(emptyList(), "sip:@d", DirectoryBook()))
    }

    @Test fun `URI 에서 번호만 뽑는다`() {
        assertEquals("1001", userPart("sip:1001@domain"))
        assertEquals("+8210111", userPart("tel:+8210111"))
        assertEquals("1001", userPart("1001"))
    }

    // ── 통합 검색 (데스크톱 PersonActionsViewModel.Filter 와 같은 규칙) ──
    private fun grp(id: String, name: String, members: Int = 3, isMember: Boolean = true) =
        GroupInfo(id = id, uri = "sip:$id@d", name = name, memberCount = members, isMember = isMember)

    private val people = mergePeople(
        book(Triple("t1", "이순경", "1001"), Triple("t1", "박경장", "1002")),
        book(Triple("t1", "이순경", "+821011112222")))

    @Test fun `빈 질의는 사람과 채널을 모두 준다`() {
        val hits = searchDirectory(people, listOf(grp("g1", "경비")), "")
        assertEquals(3, hits.size)
        assertTrue(hits.last() is SearchHit.Channel)
    }

    @Test fun `이름으로 찾는다`() {
        val hits = searchDirectory(people, emptyList(), "이순")
        assertEquals(1, hits.size)
        assertEquals("이순경", (hits[0] as SearchHit.Person).entry.name)
    }

    // 전화번호부에 `010…` 으로 친 질의가 `+8210…` 저장값에 걸려야 한다.
    @Test fun `번호는 정규형으로 비교한다`() {
        val hits = searchDirectory(people, emptyList(), "01011112222")
        assertEquals(1, hits.size)
        assertEquals("이순경", (hits[0] as SearchHit.Person).entry.name)
    }

    @Test fun `내선 일부로도 걸린다`() {
        assertEquals(1, searchDirectory(people, emptyList(), "1002").size)
    }

    @Test fun `채널은 이름과 id 로 찾는다`() {
        val gs = listOf(grp("dg-01", "경비"), grp("dg-02", "순찰"))
        assertEquals(1, searchDirectory(emptyList(), gs, "경비").size)
        assertEquals(1, searchDirectory(emptyList(), gs, "dg-02").size)
        assertEquals(2, searchDirectory(emptyList(), gs, "dg-").size)
    }

    @Test fun `결과 상한은 사람 12 채널 6`() {
        val many = (1..30).map { PersonEntryOf("사람$it", "100$it") }
        val gs = (1..30).map { grp("g$it", "채널$it") }
        val hits = searchDirectory(many, gs, "")
        assertEquals(12, hits.count { it is SearchHit.Person })
        assertEquals(6, hits.count { it is SearchHit.Channel })
    }

    @Test fun `채널 부제는 멤버 수와 범위를 적는다`() {
        assertTrue(channelMeta(grp("g1", "경비", members = 5)).contains("멤버 5"))
        assertTrue(channelMeta(grp("g1", "경비", isMember = false)).contains("청취 범위"))
    }
}
