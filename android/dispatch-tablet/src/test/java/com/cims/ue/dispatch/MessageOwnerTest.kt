// 메시지 보관의 주인 규칙(`OwnerRule`) — JVM, 기기 불필요 (android_dispatch_tablet.md §6.2e «메시지 보관»)
//
// 보관의 격리 단위는 로그인 ID 다(관제석은 자리별 ID — 교대해도 같다). 노리는 것: 한 기기에 다른 자리 ID 로 로그인하면
// 앞 ID 의 대화가 보이지 않는다 · 로그인 전에는 아무것도 보이지 않는다 · `owner` 열 이전 판의 행은 그 기기의 첫 로그인 ID 가
// **한 번만** 이어받는다(둘째 로그인 ID 가 가로채지 않는다). `MessageStore` 의 질의가 이 규칙을 SQL 로 옮긴 것이다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.OwnerRule
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MessageOwnerTest {

    @Test fun `주인은 프로파일의 로그인 ID — 비면 로그인에 쓴 값`() {
        assertEquals("seat-01", OwnerRule.pick("seat-01", "typed"))
        assertEquals("typed", OwnerRule.pick("", "typed"))
        assertEquals("", OwnerRule.pick("", ""))
    }

    @Test fun `내 로그인 ID 의 행만 보인다`() {
        assertTrue(OwnerRule.visible(rowOwner = "seat-01", owner = "seat-01"))
        assertFalse("다른 자리의 대화", OwnerRule.visible(rowOwner = "seat-01", owner = "seat-02"))
        assertFalse("대소문자가 다르면 다른 ID", OwnerRule.visible(rowOwner = "Seat-01", owner = "seat-01"))
    }

    @Test fun `로그인 전에는 아무것도 보이지 않는다 — 주인 없는 옛 행도`() {
        assertFalse(OwnerRule.visible(rowOwner = "seat-01", owner = ""))
        assertFalse(OwnerRule.visible(rowOwner = "", owner = ""))
    }

    @Test fun `주인 없는 옛 행은 첫 로그인 ID 가 한 번 이어받는다`() {
        var rows = listOf("", "", "")                                              // owner 열 이전 판의 행
        rows = rows.map { OwnerRule.adopt(it, "seat-01") }                         // 첫 로그인
        assertEquals(listOf("seat-01", "seat-01", "seat-01"), rows)
        rows = rows.map { OwnerRule.adopt(it, "seat-02") }                         // 다른 자리 ID 의 로그인
        assertEquals("이미 주인이 있는 행은 넘어가지 않는다", listOf("seat-01", "seat-01", "seat-01"), rows)
        assertTrue(rows.none { OwnerRule.visible(it, "seat-02") })
        assertTrue(rows.all { OwnerRule.visible(it, "seat-01") })                  // 다시 seat-01 로 — 그대로 이어진다
    }

    @Test fun `로그인 ID 를 모르면 옛 행을 귀속하지 않는다`() {
        assertEquals("", OwnerRule.adopt("", ""))
        assertEquals("seat-01", OwnerRule.adopt("seat-01", ""))
    }
}
