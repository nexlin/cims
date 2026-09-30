// 긴급·임박 배너 Preview — 전역 표면의 색·밀도 (android_dispatch_tablet.md §6.2a-1)
//
// 판정 대상: 긴급(빨강)과 임박(주황)이 한눈에 갈리는가, 착신 배너(옅은 면)와 헷갈리지 않는가, 여럿이 서도
// 채널 이름이 먼저 읽히는가. 앱은 어두운 테마로 뜨므로 어두운 쪽을 먼저 본다.
package com.cims.ue.dispatch.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
import com.cims.ue.dispatch.session.AlertKind

private val NOW = System.currentTimeMillis()

private val STACK = listOf(
    AlertBannerUi("g2", AlertKind.EMERGENCY, "상황실", initiator = "1003 이순경", sinceMs = NOW - 14_000),
    AlertBannerUi("g5", AlertKind.IMMINENT_PERIL, "교통1", initiator = "1021 박현장", sinceMs = NOW - 95_000),
)

@Preview(name = "긴급 배너 — 긴급·임박 스택(어두운 테마)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewAlertStackDark() = PreviewFrame(dark = true) { EmergencyBannerContent(STACK) }

@Preview(name = "긴급 배너 — 긴급·임박 스택", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewAlertStack() = PreviewFrame { EmergencyBannerContent(STACK) }

/** 넷이 서면 두 장만 펴고 «2건 더» 로 접는다 — 본문·착신 배너 자리가 남아야 한다. */
@Preview(name = "긴급 배너 — 넷(접힘)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewAlertFolded() = PreviewFrame(dark = true) {
    EmergencyBannerContent(STACK + listOf(
        AlertBannerUi("g7", AlertKind.EMERGENCY, "외곽경비", initiator = "1040 최반장", sinceMs = NOW - 300_000),
        AlertBannerUi("g8", AlertKind.IMMINENT_PERIL, "정비반", sinceMs = NOW - 420_000)))
}

/** 내가 건 긴급 — 코어가 개시자를 싣지 않아 «개시» 가 빠진다. */
@Preview(name = "긴급 배너 — 개시자 없음", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewAlertNoInitiator() = PreviewFrame(dark = true) {
    EmergencyBannerContent(listOf(AlertBannerUi("g1", AlertKind.EMERGENCY, "순찰1", sinceMs = NOW - 3_000)))
}
