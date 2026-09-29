// [무전] 목록 Preview — IDE 에서 배치·밀도를 즉시 본다 (android_dispatch_tablet.md §6.3)
//
// 기기·에뮬레이터·네이티브 .so 없이 선다. 화면이 표시용 값([ChannelRowUi])만 받기 때문이다.
// **판정 대상은 배치·밀도·읽힘**이다 — 실제 동작(발언·세션)은 실기기에서 본다.
//
// 기기 지정은 관제 태블릿 실물과 같은 가로 1280×800 이다. 본문은 상단 바 56 + 발언 바 80 + 하단 내비 56 을
// 뺀 608dp 이므로, Preview 높이도 그만큼으로 잡아 «한 화면에 몇 줄 보이는가» 가 실제와 같게 한다.
package com.cims.ue.dispatch.ui.ptt

import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
import com.cims.ue.dispatch.ui.PreviewBody
import com.cims.ue.dispatch.ui.PreviewFrame

/** 흔한 상태 — 내 채널 다섯(하나 발언 중·하나 미읽음), 범위 채널 넷(하나 청취 중). */
private val MINE = listOf(
    ChannelRowUi("g1", "1. 순찰1", subtitle = "발언 김관제 00:14", state = "12:31",
        participants = 7, unread = 3, active = true, speaking = true, canTarget = true, targeted = true),
    ChannelRowUi("g2", "2. 상황실", subtitle = "발언 없음", state = "05:02",
        participants = 3, active = true, canTarget = true),
    ChannelRowUi("g3", "3. 교통1", subtitle = "멤버 12", state = "대기"),
    ChannelRowUi("p1", "4. 김반장", subtitle = "발언 없음", state = "02:14",
        active = true, canTarget = true),
    ChannelRowUi("a1", "5. 임시 3인", subtitle = "발언 없음", state = "00:48",
        participants = 3, active = true, canTarget = true),
)

private val SCOPED = listOf(
    ChannelRowUi("s1", "야간순찰", subtitle = "청취 중 · 발언 박현장", state = "03:20",
        participants = 5, active = true, speaking = true, listening = true),
    ChannelRowUi("s2", "정비반", subtitle = "세션 진행 중 · 참가 2", state = "진행 중",
        participants = 2, active = true, listening = false),
    ChannelRowUi("s3", "외곽경비", subtitle = "마지막 세션", state = "대기", listening = false),
    ChannelRowUi("s4", "타인 개인 통화 · 이당직", subtitle = "세션 진행 중 · 참가 2", state = "진행 중",
        participants = 2, active = true, listening = false),
)

@Preview(name = "무전 — 평시", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewPtt() = PreviewFrame {
    PttScreenContent(mine = MINE, scoped = SCOPED, filter = ScopeFilter.ALL, query = "",
        listenText = "동시 청취 1/4", listenFull = false)
}

@Preview(name = "무전 — 어두운 테마", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewPttDark() = PreviewFrame(dark = true) {
    PttScreenContent(mine = MINE, scoped = SCOPED, filter = ScopeFilter.ALL, query = "",
        listenText = "동시 청취 1/4", listenFull = false)
}

/** 긴급 — 행 배경이 바뀌어도 줄이 밀리지 않아야 한다. */
@Preview(name = "무전 — 긴급", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewPttEmergency() = PreviewFrame {
    PttScreenContent(
        mine = listOf(MINE[0].copy(emergency = true, subtitle = "긴급 · 발언 김관제 00:03")) + MINE.drop(1),
        scoped = SCOPED, filter = ScopeFilter.EMERGENCY, query = "",
        listenText = "동시 청취 4/4", listenFull = true)
}

/** 채널이 많을 때 — 스크롤 없이 몇 장이 보이는지 확인하는 자리(§6.3 «10~11장» 주장의 근거). */
@Preview(name = "무전 — 많음(16장)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewPttMany() = PreviewFrame {
    PttScreenContent(
        mine = (1..8).map { i ->
            ChannelRowUi("m$i", "$i. 채널$i", subtitle = "발언 없음", state = "대기",
                participants = i, canTarget = true)
        },
        scoped = (1..8).map { i -> ChannelRowUi("x$i", "범위채널$i", subtitle = "대기", state = "대기", listening = false) },
        filter = ScopeFilter.ALL, query = "", listenText = "동시 청취 0/4", listenFull = false)
}

/** 빈 상태 — 로그인 직후·범위 없음. 문구가 자리를 잡는지. */
@Preview(name = "무전 — 비어 있음", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewPttEmpty() = PreviewFrame {
    PttScreenContent(mine = emptyList(), scoped = emptyList(), filter = ScopeFilter.ALL, query = "",
        listenText = "동시 청취 0/4", listenFull = false)
}
