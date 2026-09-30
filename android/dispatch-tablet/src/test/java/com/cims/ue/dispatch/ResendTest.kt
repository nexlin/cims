// 실패한 말풍선 재전송(`Resend`) — JVM, 기기 불필요 (android_dispatch_tablet.md §6.2e)
//
// 노리는 것은 둘이다. 재전송이 **새 말풍선을 세우면** 같은 말이 두 번 보여 두 번 보낸 줄 안다. 화면이 넘긴 사본으로
// 판정하면 [재전송] 을 빠르게 두 번 눌렀을 때 **두 번 나간다** — 사본은 첫 누름 뒤에도 실패로 남아 있다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.session.Resend
import com.cims.ue.dispatch.session.SendState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ResendTest {

    private fun msg(id: String, state: SendState, outgoing: Boolean = true, group: String = "g1") =
        Message(id = id, groupId = group, fromUri = "", fromName = "", text = "t", atMs = 0,
                outgoing = outgoing, state = state)

    @Test fun `실패한 발신만 다시 보낸다`() {
        val failed = msg("a", SendState.FAILED)
        assertTrue(Resend.allowed(mapOf("g1" to listOf(failed)), failed))
        val sent = msg("b", SendState.SENT)
        assertFalse(Resend.allowed(mapOf("g1" to listOf(sent)), sent))
        val incoming = msg("c", SendState.FAILED, outgoing = false)
        assertFalse(Resend.allowed(mapOf("g1" to listOf(incoming)), incoming))
    }

    @Test fun `판정은 사본이 아니라 지금의 말풍선으로 — 두 번 눌러도 한 번만 나간다`() {
        val copy = msg("a", SendState.FAILED)                                   // 화면이 쥔 사본
        var threads = mapOf("g1" to listOf(copy))
        assertTrue(Resend.allowed(threads, copy))
        threads = Resend.patch(threads, "a") { it.copy(state = SendState.PENDING) }   // 첫 누름
        assertFalse("사본은 아직 실패지만 지금은 보내는 중", Resend.allowed(threads, copy))
    }

    @Test fun `사라진 말풍선은 다시 보내지 않는다`() {
        val gone = msg("a", SendState.FAILED)
        assertFalse(Resend.allowed(emptyMap(), gone))                           // 로그아웃·보관 기간 정리 뒤
    }

    @Test fun `같은 말풍선만 바뀐다 — 새 말풍선을 세우지 않는다`() {
        val a = msg("a", SendState.FAILED)
        val b = msg("b", SendState.FAILED)
        val other = msg("x", SendState.FAILED, group = "g2")
        val out = Resend.patch(mapOf("g1" to listOf(a, b), "g2" to listOf(other)), "a") {
            it.copy(msgId = "new", token = 9, state = SendState.PENDING)
        }
        assertEquals(listOf("a", "b"), out.getValue("g1").map { it.id })
        assertEquals(SendState.PENDING, out.getValue("g1")[0].state)
        assertEquals("new", out.getValue("g1")[0].msgId)
        assertEquals(SendState.FAILED, out.getValue("g1")[1].state)
        assertEquals(other, out.getValue("g2").single())
    }
}
