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
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
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
    // 잔여 일수는 **시간이 가며** 줄어든다 — 관측은 그대로라(같은 값이면 흐름이 다시 내지 않는다) 화면이 1분마다 스스로 다시
    //   판정한다(데스크톱의 60초 틱). 아니면 며칠 켜 둔 관제석에서 배너가 서지 않고, 경고가 위험으로 넘어가지 않는다.
    val now by androidx.compose.runtime.produceState(System.currentTimeMillis() / 1000L) {
        while (true) { kotlinx.coroutines.delay(60_000L); value = System.currentTimeMillis() / 1000L }
    }
    val level = ServerCert.level(cert, now)
    if (level == CertLevel.OK) return
    ServerCertBannerContent(CertBannerUi(level, ServerCert.title(cert, now), ServerCert.subtitle(cert)), modifier)
}

/** 배너 — **순수 컴포저블**. */
@Composable
fun ServerCertBannerContent(b: CertBannerUi, modifier: Modifier = Modifier) {
    WarnLine(b.title, b.subtitle, critical = b.level == CertLevel.CRITICAL, modifier = modifier)
}

/**
 * 경고 한 줄 — 서버 인증서 만료·자격 갱신 실패(데스크톱 배너 층의 «경고 한 줄»). 닫기가 없다. 경고 = 연한 빨강 면 + 빨강 글자,
 * 위험([critical]) = 진한 빨강 채움 + 흰 글자. 긴급 배너와는 버튼·경과가 없는 한 줄이라는 모양으로 갈린다.
 */
@Composable
fun WarnLine(title: String, subtitle: String = "", critical: Boolean = false, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    val bg = if (critical) p.emgFill else p.emgSoft
    val fg = if (critical) p.onAccent else p.emg
    Surface(color = bg, contentColor = fg, modifier = modifier.fillMaxWidth()
        .drawBehind { drawLine(p.emgEdge, Offset(0f, size.height - 0.5f), Offset(size.width, size.height - 0.5f), 1.dp.toPx()) }) {
        Row(Modifier.padding(start = 20.dp, end = 14.dp, top = 10.dp, bottom = 10.dp), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Icon(Icons.Filled.Warning, contentDescription = null, modifier = Modifier.size(18.dp))
            Text(androidx.compose.ui.text.buildAnnotatedString {
                pushStyle(androidx.compose.ui.text.SpanStyle(fontWeight = FontWeight.Bold)); append(title); pop()
                if (subtitle.isNotEmpty()) append(" · $subtitle")
            }, fontSize = Type.strong, maxLines = 2, overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis)
        }
    }
}
