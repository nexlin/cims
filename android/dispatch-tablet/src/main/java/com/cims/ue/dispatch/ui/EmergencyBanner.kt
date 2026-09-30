// 긴급·임박 배너 — 전 화면 공통 (android_dispatch_tablet.md §6.2a-1, dispatch_desktop_ui.md §3.2)
//
// 채널 행의 빨강은 [무전] 목록을 보고 있을 때만 보인다. 관제사가 [통화]·[이력]·[더보기] 에 있는 동안 선 긴급
// 그룹콜은 어디에도 안 보인다 — 착신 배너와 같은 이유로 **화면과 무관한 표면**이 따로 있어야 한다.
//
// 착신 배너와 다른 점: 받을 것이 아니라 **알아야 할 상태**다. 닫기가 없고(조건이 풀리거나 세션이 끝나면 스스로
// 빠진다), 버튼은 [채널로 이동] 하나다. 착신 배너(옅은 면)와 헷갈리지 않게 **꽉 찬 면**으로 그린다.
package com.cims.ue.dispatch.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.AlertKind
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.ui.ptt.fmtElapsed

/** 임박 위험의 색 — 배너 면·행·머리 태그가 같은 값을 쓴다(데스크톱 `Brush.Ring`). */
internal val PerilAmber = Color(0xFFF59E0B)

/** 배너 한 장이 그리는 데 필요한 전부 — 세션 없이 Preview·단위시험이 서게 한다. */
data class AlertBannerUi(
    /** [채널로 이동] 이 여는 채널 — ① 카드·② 행과 같은 id. */
    val channelId: String,
    val kind: AlertKind,
    /** 채널 이름 — 카드 제목과 같다. */
    val title: String,
    /** 개시자 표시(번호 이름 병기). 모르면 빈 값 — 내가 건 긴급은 코어가 개시자를 싣지 않는다. */
    val initiator: String = "",
    val sinceMs: Long = System.currentTimeMillis(),
)

/** 세션 → 배너. 개시자는 mcptt-info `<mcptt-calling-user-id>` 다. */
internal fun SessionItem.toAlertBannerUi(label: (String) -> String): AlertBannerUi? {
    val kind = alertKind ?: return null
    val caller = info.mcptt.callingUserId
    return AlertBannerUi(
        channelId = channelId, kind = kind, title = title.ifEmpty { info.groupId },
        initiator = if (caller.isBlank()) "" else label(caller),
        sinceMs = alertSinceMs ?: startedAtMs)
}

@Composable
fun EmergencyBanners(
    session: DispatchSession,
    onOpen: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val alerts by session.alerts.collectAsStateWithLifecycle()
    EmergencyBannerContent(alerts.mapNotNull { it.toAlertBannerUi(session::displayLabel) }, onOpen, modifier)
}

/**
 * 한 번에 펴 두는 배너 수. 넘치면 «n건 더» 줄로 접는다 — 닫기가 없는 배너가 쌓여 **본문과 착신 배너의 자리를
 * 다 먹으면** [채널로 이동] 을 눌러도 대응할 화면이 없다. 숨기는 것이 아니라 접는 것이다(건수는 늘 보인다).
 */
internal const val ALERT_VISIBLE = 2

/** 펼쳤을 때의 높이 상한 — 넘치면 스택 안에서 스크롤한다. 본문(608dp)의 절반 아래로 둔다. */
private val ALERT_EXPANDED_MAX = 280.dp

/** 펴 둘 배너 수 — 접혀 있으면 [ALERT_VISIBLE] 까지, 펼치면 전부. */
internal fun alertFold(total: Int, expanded: Boolean): Int =
    if (expanded) total else minOf(total, ALERT_VISIBLE)

/** 배너 스택 — **순수 컴포저블**. 최신이 위(어느 채널로 갈지는 관제사가 고른다). */
@Composable
fun EmergencyBannerContent(
    items: List<AlertBannerUi>,
    onOpen: (String) -> Unit = {},
    modifier: Modifier = Modifier,
) {
    var expanded by remember { mutableStateOf(false) }
    AnimatedVisibility(
        visible = items.isNotEmpty(),
        enter = expandVertically(),
        exit = shrinkVertically(),
        modifier = modifier,
    ) {
        Column(Modifier.fillMaxWidth()) {
            val shown = alertFold(items.size, expanded)
            Column(Modifier.heightIn(max = ALERT_EXPANDED_MAX).verticalScroll(rememberScrollState())) {
                items.take(shown).forEach { AlertBanner(it) { onOpen(it.channelId) } }
            }
            if (items.size > ALERT_VISIBLE) MoreAlerts(hidden = items.size - shown, expanded) { expanded = !expanded }
        }
    }
}

/** 접힌 배너 줄 — «긴급·임박 n건 더» / «접기». 긴급 색의 옅은 면이라 스택의 일부로 읽힌다. */
@Composable
private fun MoreAlerts(hidden: Int, expanded: Boolean, onToggle: () -> Unit) {
    Surface(color = MaterialTheme.colorScheme.errorContainer.copy(alpha = 0.6f),
        contentColor = MaterialTheme.colorScheme.onErrorContainer,
        modifier = Modifier.fillMaxWidth().clickable(onClick = onToggle)) {
        Text(if (expanded) "접기" else "긴급·임박 ${hidden}건 더 — 눌러서 펼치기",
            Modifier.padding(horizontal = 14.dp, vertical = 8.dp),
            fontSize = Type.body, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun AlertBanner(b: AlertBannerUi, onOpen: () -> Unit) {
    // 긴급 = 앱 전체의 긴급 색(행·머리와 같은 errorContainer), 임박 = 주황. 둘 다 꽉 찬 면이다.
    val (bg, fg) = when (b.kind) {
        AlertKind.EMERGENCY -> MaterialTheme.colorScheme.errorContainer to MaterialTheme.colorScheme.onErrorContainer
        AlertKind.IMMINENT_PERIL -> PerilAmber to Color.Black
    }
    // 경과는 1초마다 다시 그린다 — 얼마나 됐는지가 대응 순서의 판단 재료다.
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(b.channelId, b.sinceMs) {
        while (true) { kotlinx.coroutines.delay(1000); tick++ }
    }

    Surface(color = bg, contentColor = fg, modifier = Modifier.fillMaxWidth()) {
        Row(
            Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Box(Modifier.size(10.dp).background(fg, RoundedCornerShape(5.dp)))
            Column(Modifier.weight(1f)) {
                Text(
                    b.kind.bannerTitle + if (b.initiator.isNotEmpty()) " · 개시 ${b.initiator}" else "",
                    fontSize = Type.body, fontWeight = FontWeight.Bold, maxLines = 1)
                Text(b.title, fontSize = Type.head, fontWeight = FontWeight.Bold, maxLines = 1)
            }
            @Suppress("UNUSED_EXPRESSION") tick     // 1초 틱을 이 조합에 묶는다
            Text(fmtElapsed((System.currentTimeMillis() - b.sinceMs).coerceAtLeast(0)), fontSize = Type.title)
            Button(onClick = onOpen, modifier = Modifier.height(48.dp),
                colors = ButtonDefaults.buttonColors(containerColor = fg, contentColor = bg)) {
                Text("채널로 이동", fontWeight = FontWeight.Bold)
            }
        }
    }
}
