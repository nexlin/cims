// 문자(SMS) 판정 — 순수 부분만 (android_dispatch_tablet.md §6.2e)
//
// 여기서 시험하는 셋은 **앱이 혼자 내리는 판정**이다. 서버가 도와주지 않으므로(외부망 여부를 알려 주는
// 통로가 이 경로엔 없다) 틀리면 관제사가 «보냈는데 안 갔다» 를 이유 없이 겪는다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.session.MessageKind
import com.cims.ue.dispatch.session.CallLogKind
import com.cims.ue.dispatch.session.CallLogRow
import com.cims.ue.dispatch.session.isExternalNumber
import com.cims.ue.dispatch.ui.call.keepForPerson
import com.cims.ue.dispatch.ui.call.SMS_LIMIT
import com.cims.ue.dispatch.ui.call.smsCountText
import com.cims.ue.dispatch.ui.call.smsThreads
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SmsTest {

    private val book = DirectoryBook(entries = listOf(
        DirectoryEntry(org = "t1", name = "이당직", msisdn = "1002"),
        DirectoryEntry(org = "t1", name = "박현장", msisdn = "01012345678")))

    private fun m(peer: String, at: Long, out: Boolean = false, read: Boolean = true) = Message(
        id = "$peer-$at", groupId = peer, fromUri = if (out) "" else "sip:$peer@d",
        fromName = if (out) "나" else peer, text = "t", atMs = at, outgoing = out,
        read = read, kind = MessageKind.SMS)

    // ── 외부망 판정 ──

    /** 주소록에 있으면 사이트 안이다 — 휴대폰 번호라도. */
    @Test fun 주소록에_있으면_내부() {
        assertFalse(isExternalNumber(book, "1002"))
        assertFalse(isExternalNumber(book, "01012345678"))
    }

    /** E.164 와 국내 로컬 표기는 같은 번호다(`normalize`). */
    @Test fun 정규형이_같으면_내부() {
        assertFalse(isExternalNumber(book, "+821012345678"))
        assertFalse(isExternalNumber(book, "sip:1002@cims.local"))
    }

    /** 주소록에 없는 긴 번호는 외부망 — 막고 이유를 말한다. */
    @Test fun 모르는_긴번호는_외부() {
        assertTrue(isExternalNumber(book, "01055551111"))
        assertTrue(isExternalNumber(book, "+821099998888"))
    }

    /** 주소록을 아직 못 받았어도 내선(6자리 이하)은 막지 않는다 — 폴백. */
    @Test fun 주소록_전이라도_내선은_통과() {
        val empty = DirectoryBook()
        assertFalse(isExternalNumber(empty, "1002"))
        assertFalse(isExternalNumber(empty, "700123"))
        assertTrue(isExternalNumber(empty, "0212345678"))
    }

    @Test fun 빈_번호는_외부로_본다() {
        assertTrue(isExternalNumber(book, ""))
        assertTrue(isExternalNumber(book, "   "))
    }

    // ── 대화 목록 ──

    /** 최근 순 — 방금 온 것이 앞. */
    @Test fun 최근순으로_선다() {
        val map = mapOf(
            "1002" to listOf(m("1002", 1_000), m("1002", 5_000)),
            "1003" to listOf(m("1003", 9_000)))
        assertEquals(listOf("1003", "1002"), smsThreads(map, book).map { it.key })
        assertEquals(9_000L, smsThreads(map, book).first().lastAtMs)
    }

    /** 이름은 주소록이 준다 — 없으면 번호 그대로. */
    @Test fun 이름은_주소록에서() {
        val map = mapOf("1002" to listOf(m("1002", 1)), "01055551111" to listOf(m("01055551111", 2)))
        val t = smsThreads(map, book).associate { it.key to it.title }
        assertEquals("이당직", t["1002"])
        assertEquals("01055551111", t["01055551111"])
    }

    /** 미읽음은 **받은 것만** 센다 — 내가 보낸 말풍선은 셀 이유가 없다. */
    @Test fun 미읽음은_수신만_센다() {
        val map = mapOf("1002" to listOf(
            m("1002", 1, read = false), m("1002", 2, out = true, read = false),
            m("1002", 3, read = true)))
        assertEquals(1, smsThreads(map, book).single().unread)
    }

    /** 빈 스레드는 목록에 내지 않는다(markRead 뒤 남은 껍데기). */
    @Test fun 빈_스레드는_숨긴다() {
        assertTrue(smsThreads(mapOf("1002" to emptyList()), book).isEmpty())
    }

    // ── 글자 수 ──

    @Test fun 칠십자까지_SMS() {
        assertEquals("0/$SMS_LIMIT SMS", smsCountText(""))
        assertEquals("70/$SMS_LIMIT SMS", smsCountText("가".repeat(70)))
    }

    @Test fun 넘으면_LMS() {
        assertEquals("71자 LMS", smsCountText("가".repeat(71)))
    }

    // ── 사람 축 통화 기록 필터(사람 메뉴 «통화 기록») ──

    private fun log(number: String) = CallLogRow(
        atMs = 1_790_000_000_000L, peer = number, text = "착신 응답",
        kind = CallLogKind.ANSWERED, number = number)

    @Test fun 빈_필터는_거르지_않는다() {
        assertTrue(keepForPerson(log("1002"), ""))
        assertTrue(keepForPerson(log(""), "   "))
    }

    /** 국내 로컬 표기와 E.164 는 같은 사람이다. */
    @Test fun 정규형으로_비교한다() {
        assertTrue(keepForPerson(log("+821012345678"), "01012345678"))
        assertTrue(keepForPerson(log("1002"), "sip:1002@cims"))
        assertFalse(keepForPerson(log("1003"), "1002"))
    }

    /** 번호가 없는 행(발신자 표시 제한)은 특정 사람의 기록이라 말할 수 없다. */
    @Test fun 번호없는_행은_사람필터에_안_걸린다() {
        assertFalse(keepForPerson(log(""), "1002"))
    }
}
