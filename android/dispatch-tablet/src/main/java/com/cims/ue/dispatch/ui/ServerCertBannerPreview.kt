// 서버 인증서 만료 배너 Preview — 경고·위험 두 단계의 색 (android_dispatch_tablet.md §6.2a-3)
//
// 판정 대상: 경고(연한 빨강 + 빨강 글자)와 위험(진한 빨강)이 한눈에 갈리는가, 긴급 배너(꽉 찬 빨강 면 + [채널로 이동])와
// 헷갈리지 않는가, 운영자에게 전할 사실(서버·인증서·만료일)이 두 줄 안에 읽히는가.
package com.cims.ue.dispatch.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
import com.cims.ue.dispatch.session.CertLevel

private const val SUB = "10.20.1.5:4430 · CN=csc.site1.cims.example.kr, O=CIMS Site1 · 만료 2026-10-12 · " +
    "자동 갱신 실패 신호 — 운영자에게 알리세요 (콘솔 알람 A-PRC-009 cert/…/renew)"

@Preview(name = "인증서 배너 — 경고(≤30일)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewCertWarn() = PreviewFrame(dark = true) {
    ServerCertBannerContent(CertBannerUi(CertLevel.WARN, "서버 인증서 12일 후 만료", SUB))
}

@Preview(name = "인증서 배너 — 위험(≤7일)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewCertCritical() = PreviewFrame(dark = true) {
    ServerCertBannerContent(CertBannerUi(CertLevel.CRITICAL, "서버 인증서 3일 후 만료", SUB))
}

@Preview(name = "인증서 배너 — 밝은 테마 경고·위험", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewCertLight() = PreviewFrame {
    androidx.compose.foundation.layout.Column {
        ServerCertBannerContent(CertBannerUi(CertLevel.WARN, "서버 인증서 12일 후 만료", SUB))
        ServerCertBannerContent(CertBannerUi(CertLevel.CRITICAL, "서버 인증서 3일 후 만료", SUB))
    }
}
