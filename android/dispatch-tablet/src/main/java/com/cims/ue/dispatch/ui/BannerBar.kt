// 배너 한 줄(64) — 긴급·임박·경보·착신·새 영상이 같은 모양을 쓴다 (android_dispatch_tablet.md §6.2a, dispatch_desktop_ui.md §3.2)
//
// 배너 층은 상단 바 아래, 화면과 무관하다. 종류는 **색**으로 갈린다(Windows 배너 층과 같은 토큰):
// 긴급 = 연한 빨강 면 + 진한 빨강 글자 · 임박 = 주황 채움 + 먹 글자 · 경보 = 연한 보라 · 대표번호 착신 = 옅은 주황 ·
// 직접 착신 = 옅은 파랑 · PTT 개별 통화·새 영상 = 옅은 청록. 두 줄(머리 13 굵게 / 제목 20 굵게) · 경과 · 오른쪽에 버튼들.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.ui.ptt.fmtElapsed

/** 배너의 종류 — 색만 다르다. */
enum class BannerTone { EMERGENCY, PERIL, ALERT, PILOT, DIRECT, PTT, VIDEO }

/** 배너 한 벌의 색 — 면·아래 선·제목 글자·머리 글자·점. */
internal data class BannerLook(val bg: Color, val edge: Color, val fg: Color, val head: Color, val dot: Color)

@Composable
internal fun BannerTone.look(): BannerLook {
    val p = Tokens.palette
    return when (this) {
        BannerTone.EMERGENCY -> BannerLook(p.emgSoft, p.emgEdge, p.emgInk, p.emgInk, p.emg)
        BannerTone.PERIL -> BannerLook(p.peril, p.perilEdge, p.onPeril, p.onPeril, p.onPeril)
        BannerTone.ALERT -> BannerLook(p.monSoft, p.monEdge, p.monInk, p.monInk, p.mon)
        BannerTone.PILOT -> BannerLook(p.ringBanner, p.ringEdge, p.ink, p.ringInk, p.peril)
        BannerTone.DIRECT -> BannerLook(p.heldSoft, p.divider, p.ink, p.held, p.held)
        BannerTone.PTT -> BannerLook(p.listenSoft, p.divider, p.ink, p.listenInk, p.listen)
        BannerTone.VIDEO -> BannerLook(p.listenSoft, p.divider, p.ink, p.listenInk, p.listen)
    }
}

/**
 * 두 줄 배너.
 *
 * @param sinceMs 경과의 기준 시각 — null 이면 경과를 적지 않는다. 1초마다 다시 그린다.
 * @param actions 오른쪽 버튼들(44 높이 알약).
 */
@Composable
fun BannerBar(
    tone: BannerTone,
    line1: String,
    line2: String,
    modifier: Modifier = Modifier,
    sinceMs: Long? = null,
    actions: @Composable RowScope.() -> Unit = {},
) {
    val look = tone.look()
    var tick by remember { mutableIntStateOf(0) }
    if (sinceMs != null) LaunchedEffect(sinceMs) {
        while (true) { kotlinx.coroutines.delay(1000); tick++ }
    }
    Row(
        modifier.fillMaxWidth().heightIn(min = 64.dp).background(look.bg)
            .drawBehind { drawLine(look.edge, Offset(0f, size.height - 0.5f), Offset(size.width, size.height - 0.5f), 1.dp.toPx()) }
            .padding(start = 20.dp, end = 14.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(Modifier.padding(end = 4.dp).size(10.dp).clip(CircleShape).background(look.dot))
        Column(Modifier.weight(1f)) {
            Text(line1, fontSize = Type.body, fontWeight = FontWeight.Bold, color = look.head, maxLines = 1,
                overflow = TextOverflow.Ellipsis)
            Text(line2, fontSize = Type.display, fontWeight = FontWeight.Bold, color = look.fg, maxLines = 1,
                overflow = TextOverflow.Ellipsis)
        }
        if (sinceMs != null) {
            @Suppress("UNUSED_EXPRESSION") tick     // 1초 틱을 이 조합에 묶는다
            Text(fmtElapsed((System.currentTimeMillis() - sinceMs).coerceAtLeast(0)), fontSize = Type.title,
                fontWeight = FontWeight.SemiBold, fontFamily = FontFamily.Monospace, color = look.fg,
                modifier = Modifier.padding(end = 4.dp))
        }
        actions()
    }
}
