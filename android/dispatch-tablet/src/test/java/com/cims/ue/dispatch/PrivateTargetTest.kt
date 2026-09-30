// 개별 통화 대상 풀이(`privateTargetOf`) — JVM, 기기 불필요 (android_dispatch_tablet.md §6.2e)
//
// 노리는 것: 주소록에 없는 번호를 **걸 수 없는** 것(막 개설한 회선·주소록을 못 받은 때), 그리고 모르는 이름을 번호처럼
// 보내 서버 404 를 받는 것.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.ui.ptt.privateTargetOf
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PrivateTargetTest {

    private val book = DirectoryBook(entries = listOf(
        DirectoryEntry("", "김반장", "1003"),
        DirectoryEntry("", "이순경", "1004"),
    ))

    @Test fun `고른 사람이 먼저다`() {
        assertEquals("1004", privateTargetOf(book, "김반장", listOf(DirectoryEntry("", "이순경", "1004"))))
    }

    @Test fun `이름이 정확히 맞으면 그 번호로 푼다`() {
        assertEquals("1003", privateTargetOf(book, " 김반장 ", emptyList()))
    }

    @Test fun `주소록에 없어도 번호 모양이면 그 번호로 건다`() {
        assertEquals("1099", privateTargetOf(book, "1099", emptyList()))
        assertEquals("01012345678", privateTargetOf(book, "010-1234-5678", emptyList()))
    }

    @Test fun `모르는 이름과 빈 입력은 대상이 아니다`() {
        assertNull(privateTargetOf(book, "박주임", emptyList()))
        assertNull(privateTargetOf(book, "  ", emptyList()))
    }
}
