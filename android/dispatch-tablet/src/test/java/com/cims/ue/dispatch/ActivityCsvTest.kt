// ⑤ 이벤트 CSV — 열·시간순·이스케이프 — JVM, 기기 불필요 (android_dispatch_tablet.md §6.7)
//
// 노리는 것은 **내보낸 파일이 표로 안 읽히는** 결함이다 — 쉼표·따옴표가 든 채널 이름·내용이 열을 밀어내는 것,
// 최신순으로 나가 시간 흐름을 거꾸로 읽게 되는 것.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.ActivityKind
import com.cims.ue.dispatch.session.ActivityRow
import com.cims.ue.dispatch.ui.ptt.activityCsv
import org.junit.Assert.assertEquals
import org.junit.Test

class ActivityCsvTest {

    @Test fun `시간순·열·이스케이프`() {
        val utc = java.util.TimeZone.getTimeZone("UTC")
        val rows = listOf(      // 세션은 최신이 앞이다
            ActivityRow(61_000, "g2", "교통, 1반", "긴급 개시 · \"1003\"", ActivityKind.EMERGENCY, emergency = true),
            ActivityRow(1_000, "g1", "순찰1", "발언 김관제", ActivityKind.TALK))
        val lines = activityCsv(rows, utc).trimEnd().split("\r\n")
        assertEquals("time,channel,kind,detail,emergency", lines[0])
        assertEquals("1970-01-01 00:00:01,\"순찰1\",\"발언\",\"발언 김관제\",0", lines[1])
        assertEquals("1970-01-01 00:01:01,\"교통, 1반\",\"긴급\",\"긴급 개시 · \"\"1003\"\"\",1", lines[2])
    }
}
