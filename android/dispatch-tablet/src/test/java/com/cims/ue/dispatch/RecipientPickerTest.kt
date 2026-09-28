// «새 대화» 후보 거르기·세우기 (android_dispatch_tablet.md §6.9a)
package com.cims.ue.dispatch

import com.cims.ue.dispatch.ui.RecipientOption
import com.cims.ue.dispatch.ui.filterRecipients
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RecipientPickerTest {

    private val all = listOf(
        RecipientOption("g002", "상황실", "편성 8명", group = true),
        RecipientOption("1002", "이당직", "1002 · 본부 › 당직"),
        RecipientOption("g001", "순찰1", "편성 5명", group = true),
        RecipientOption("1003", "박현장", "1003 · 본부 › 현장"))

    /** 그룹이 먼저, 그 안에서 이름순 — 잘못 고르면 편성 전원이 받으므로 섞지 않는다. */
    @Test fun 그룹이_먼저고_이름순이다() {
        assertEquals(listOf("상황실", "순찰1", "박현장", "이당직"),
            filterRecipients(all, "").map { it.title })
    }

    @Test fun 이름으로_찾는다() {
        assertEquals(listOf("박현장"), filterRecipients(all, "현장").map { it.title })
    }

    /** 번호(키)로도 찾는다 — 이름을 모르고 번호만 아는 경우가 흔하다. */
    @Test fun 번호로도_찾는다() {
        assertEquals(listOf("이당직"), filterRecipients(all, "1002").map { it.title })
        assertEquals(listOf("순찰1"), filterRecipients(all, "g001").map { it.title })
    }

    /** 부제(조직 경로)도 걸린다 — «본부» 로 훑는 조작. */
    @Test fun 조직으로도_걸린다() {
        assertEquals(2, filterRecipients(all, "본부").size)
    }

    @Test fun 대소문자를_가리지_않는다() {
        assertEquals(filterRecipients(all, "G001"), filterRecipients(all, "g001"))
    }

    @Test fun 없으면_빈_목록() {
        assertTrue(filterRecipients(all, "없는이름").isEmpty())
    }
}
