// [PTT 그룹] 상세 «멤버» 투영 — 구성(GMS 문서) × 지금 상태(로스터·발언자).
// 데스크톱 `GroupAdminViewModel.RefreshDetailMembers` 와 같은 규칙인지 본다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.ui.groups.DetailMember
import com.cims.ue.dispatch.ui.groups.detailMembers
import com.cims.ue.sdk.GroupMember
import com.cims.ue.sdk.RosterEntry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

private fun member(uri: String, name: String = "", role: String = "") =
    GroupMember(uri = uri, name = name, role = role)

private fun entry(uri: String, status: String = "connected") = RosterEntry(uri, status)

class DetailMembersTest {

    private val doc = listOf(
        member("sip:1001@cims", "김관제"),
        member("sip:1002@cims", "", "chair"),
        member("tel:+821012345678", "박현장"),
    )

    @Test
    fun `로스터에 있으면 참여 없으면 미참가`() {
        val rows = detailMembers(doc, listOf(entry("sip:1001@cims")), "", "sip:9000@cims")
        assertEquals(DetailMember.JOINED, rows.first { it.number == "1001" }.status)
        assertEquals(DetailMember.ABSENT, rows.first { it.number == "1002" }.status)
    }

    @Test
    fun `청취자도 그 자리에 있다 — 참여로 센다`() {
        val rows = detailMembers(doc, listOf(entry("sip:1002@cims", "listener")), "", "")
        assertEquals(DetailMember.JOINED, rows.first { it.number == "1002" }.status)
    }

    @Test
    fun `번호는 정규형으로 맞춘다 — tel 과 0 으로 시작하는 번호가 같은 사람이다`() {
        val rows = detailMembers(doc, listOf(entry("tel:+821012345678")), "", "")
        assertEquals(DetailMember.JOINED, rows.first { it.name == "박현장" }.status)
    }

    @Test
    fun `발언자는 표시명으로도 번호로도 붙는다`() {
        val byName = detailMembers(doc, listOf(entry("sip:1001@cims")), "김관제", "")
        assertEquals(DetailMember.SPEAKING, byName.first { it.number == "1001" }.status)
        val byNumber = detailMembers(doc, listOf(entry("sip:1001@cims")), "sip:1001@cims", "")
        assertEquals(DetailMember.SPEAKING, byNumber.first { it.number == "1001" }.status)
    }

    @Test
    fun `미참가는 뒤로 가고 같은 등급은 문서 순서를 지킨다`() {
        val rows = detailMembers(doc, listOf(entry("tel:+821012345678")), "", "")
        assertEquals(listOf("박현장", "김관제", "1002"), rows.map { it.name })
        assertTrue(rows.last().absent)
    }

    @Test
    fun `이름이 없으면 전화번호부 그것도 없으면 번호`() {
        val rows = detailMembers(doc, emptyList(), "", "", nameOf = { if (it == "1002") "이당직" else "" })
        assertEquals("이당직", rows.first { it.number == "1002" }.name)
        assertEquals("김관제", rows.first { it.number == "1001" }.name)
    }

    @Test
    fun `나와 의장 표시`() {
        val rows = detailMembers(doc, emptyList(), "", "sip:1001@cims")
        assertTrue(rows.first { it.number == "1001" }.isMe)
        assertFalse(rows.first { it.number == "1002" }.isMe)
        assertTrue(rows.first { it.number == "1002" }.isChair)
    }

    @Test
    fun `내 번호가 비면 아무도 나로 표시하지 않는다`() {
        val rows = detailMembers(doc, emptyList(), "", "")
        assertTrue(rows.none { it.isMe })
    }
}
