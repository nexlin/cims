// 서버 인증서 만료 배너 — 전 화면 공통 (android_dispatch_tablet.md §6.2a-3, sip_tls_signaling.md §8.6.2)
//
// 자동 갱신이 살아 있으면 뜨지 않는다 — 떴다면 서버의 자동 갱신이 죽은 것이다. 관제사는 고칠 수 없고 운영자에게
// 알려야 하므로 **닫기가 없고**(서버가 갱신되면 스스로 내린다), 운영자에게 그대로 전할 사실(서버·인증서·만료일·볼 알람)을
// 적는다. 데스크톱 배너 레이어의 `ServerCert` 배너와 같다 — 경고(≤30일) 옅은 빨강, 위험(≤7일·만료) 진한 빨강.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.CertLevel
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.ServerCert

/** 배너 한 장 — 세션 없이 Preview 가 서게 한다. */
data class CertBannerUi(val level: CertLevel, val title: String, val subtitle: String)

@Composable
fun ServerCertBanner(session: DispatchSession, modifier: Modifier = Modifier) {
    val e by session.serverCert.collectAsStateWithLifecycle()
    val cert = e ?: return
    val now = System.currentTimeMillis() / 1000L
    val level = ServerCert.level(cert, now)
    if (level == CertLevel.OK) return
    ServerCertBannerContent(CertBannerUi(level, ServerCert.title(cert, now), ServerCert.subtitle(cert)), modifier)
}

/** 배너 — **순수 컴포저블**. */
@Composable
fun ServerCertBannerContent(b: CertBannerUi, modifier: Modifier = Modifier) {
    // 위험 = 진한 빨강 면, 경고 = 옅은 빨강 면 + 빨강 글자 — 두 테마 모두. 진한 쪽 토큰이 테마마다 다르다(어둡게는
    //   errorContainer, 밝게는 error 가 진하다). 긴급 배너와는 [채널로 이동] 이 없고 한 줄 제목이라는 모양으로 갈린다.
    val cs = MaterialTheme.colorScheme
    val dark = cs.background.luminance() < 0.5f
    val critical = b.level == CertLevel.CRITICAL
    val bg = when {
        critical -> if (dark) cs.errorContainer else cs.error
        else -> if (dark) cs.errorContainer.copy(alpha = 0.35f) else cs.errorContainer
    }
    val fg = when {
        critical -> if (dark) cs.onErrorContainer else cs.onError
        else -> cs.error
    }
    Surface(color = bg, contentColor = fg, modifier = modifier.fillMaxWidth()) {
        Row(Modifier.padding(horizontal = 14.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Icon(Icons.Filled.Warning, contentDescription = null, modifier = Modifier.size(22.dp))
            Column(Modifier.weight(1f)) {
                Text(b.title, fontSize = Type.body, fontWeight = FontWeight.Bold, maxLines = 1)
                Text(b.subtitle, fontSize = Type.meta, maxLines = 2)
            }
        }
    }
}
