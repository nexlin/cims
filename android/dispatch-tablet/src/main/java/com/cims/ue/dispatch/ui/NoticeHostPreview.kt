// 토스트 Preview — 등급별 색과 ▸상세 (android_dispatch_tablet.md §6.2a-2)
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.session.Notice
import com.cims.ue.dispatch.session.NoticeLevel

private val SAMPLE = listOf(
    Notice(3, NoticeLevel.ERROR, "당겨받을 호가 없습니다", "404 Not Found"),
    Notice(2, NoticeLevel.WARN, "동시 청취 상한 4"),
    Notice(1, NoticeLevel.ERROR, "PTT 등록 실패 — 서버 응답 없음 — 재시도 중", "408 Request Timeout"),
)

@Preview(name = "토스트 — 우하단 스택(어두운 테마)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewNoticesDark() = PreviewFrame(dark = true) {
    Box(Modifier.fillMaxSize()) { NoticeStack(SAMPLE, modifier = Modifier.align(Alignment.BottomEnd).padding(12.dp)) }
}
