// 긴급·임박 배너 — 전 화면 공통 (android_dispatch_tablet.md §6.2a-1, dispatch_desktop_ui.md §3.2)
//
// 채널 행의 빨강은 [무전] 목록을 보고 있을 때만 보인다. 관제사가 [통화]·[이력]·[더보기] 에 있는 동안 선 긴급
// 그룹콜은 어디에도 안 보인다 — 착신 배너와 같은 이유로 **화면과 무관한 표면**이 따로 있어야 한다.
//
// 착신 배너와 다른 점: 받을 것이 아니라 **알아야 할 상태**다. 긴급·임박은 닫기가 없고(조건이 풀리거나 세션이 끝나면 스스로
// 빠진다), 버튼은 [채널로 이동] 과 자격이 있을 때의 [긴급 해제]·[임박 해제] 다. **긴급 경보**(TS 24.379 §12.1 — 세션 없이도 오는
// 별개 신호)는 보라 배너로 같은 스택에 서고 [경보 해제](자격이 있을 때)·[닫기](이 화면의 표시만 내린다)를 갖는다.
package com.cims.ue.dispatch.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.AlertKind
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.EmergencyAlertBanner
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.session.canCancelCondition
import com.cims.ue.dispatch.session.cancelAlert
import com.cims.ue.dispatch.session.cancelCondition
import com.cims.ue.dispatch.session.dismissAlert
import kotlinx.coroutines.launch

/** 배너 한 장이 그리는 데 필요한 전부 — 세션 없이 Preview·단위시험이 서게 한다. */
data class AlertBannerUi(
    /** [채널로 이동] 이 여는 채널 — ① 카드·② 행과 같은 id. */
    val channelId: String,
    val kind: AlertKind,
    /** 채널 이름 — 카드 제목과 같다. */
    val title: String,
    /** 개시자 표시(번호 이름 병기). 모르면 빈 값 — 내가 건 긴급·진행 중에 걸린 조건은 개시자를 모른다([SessionItem.alertInitiator]). */
    val initiator: String = "",
    val sinceMs: Long = System.currentTimeMillis(),
    /** [긴급 해제]·[임박 해제]·[경보 해제] 를 낼 자격이 있나 — 세션 조건·user profile 이 바뀌면 따라온다. */
    val canCancel: Boolean = false,
    /** 조건 배너면 그 세션의 호 — [해제] 가 조건 하향을 보낼 대상. 경보 배너는 null. */
    val callId: Int? = null,
    /** 긴급 경보 배너면 그 경보([kind] 는 쓰이지 않는다) — [경보 해제]·[닫기] 의 대상. */
    val alert: EmergencyAlertBanner? = null,
) {
    val isAlert: Boolean get() = alert != null
    /** 스택 안에서 이 배너를 가리키는 열쇠 — 조건 배너는 채널마다 하나, 경보는 그룹·발신자마다 하나. */
    val key: String get() = alert?.let { "alert|" + it.key } ?: "cond|$channelId"
}

/** 긴급 경보 → 배너. 제목은 그룹 이름, 머리는 «긴급 경보 · 개시 누구». */
internal fun EmergencyAlertBanner.toAlertBannerUi(canCancel: Boolean): AlertBannerUi = AlertBannerUi(
    channelId = groupId, kind = AlertKind.EMERGENCY, title = groupName, initiator = userLabel,
    sinceMs = sinceMs, canCancel = canCancel, alert = this)

/** 세션 → 배너. 개시자는 mcptt-info `<mcptt-calling-user-id>` 다([SessionItem.alertInitiator]). */
internal fun SessionItem.toAlertBannerUi(canCancel: Boolean = false, label: (String) -> String): AlertBannerUi? {
    val kind = alertKind ?: return null
    val caller = alertInitiator
    return AlertBannerUi(
        channelId = channelId, kind = kind, title = title.ifEmpty { info.groupId },
        initiator = if (caller.isBlank()) "" else label(caller),
        sinceMs = alertSinceMs ?: startedAtMs, canCancel = canCancel, callId = callId)
}

/** 조건 배너와 경보 배너를 한 스택으로 — 최신이 위(어느 것부터 볼지는 «언제부터» 가 정한다). */
internal fun alertBannerStack(conditions: List<AlertBannerUi>, alerts: List<AlertBannerUi>): List<AlertBannerUi> =
    (conditions + alerts).sortedByDescending { it.sinceMs }

@Composable
fun EmergencyBanners(
    session: DispatchSession,
    onOpen: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val alerts by session.alerts.collectAsStateWithLifecycle()
    val raised by session.alertBanners.collectAsStateWithLifecycle()
    val caps by session.capabilities.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val items = alertBannerStack(
        alerts.mapNotNull { it.toAlertBannerUi(canCancelCondition(it, caps), session::displayLabel) },
        raised.map { it.toAlertBannerUi(caps.cancelEmergencyAlert) })
    EmergencyBannerContent(items, onOpen, modifier,
        onCancel = { b ->
            scope.launch(com.cims.ue.dispatch.session.UnhandledGuard) {
                val a = b.alert
                if (a != null) session.cancelAlert(a) else b.callId?.let { session.cancelCondition(it) }
            }
        },
        onDismiss = { b -> b.alert?.let(session::dismissAlert) })
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
    onCancel: (AlertBannerUi) -> Unit = {},
    onDismiss: (AlertBannerUi) -> Unit = {},
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
                items.take(shown).forEach { b ->
                    key(b.key) { AlertBanner(b, onOpen = { onOpen(b.channelId) }, onCancel = { onCancel(b) }, onDismiss = { onDismiss(b) }) }
                }
            }
            if (items.size > ALERT_VISIBLE) MoreAlerts(hidden = items.size - shown, expanded) { expanded = !expanded }
        }
    }
}

/** 접힌 배너 줄 — «긴급·임박 n건 더» / «접기». 긴급 색의 옅은 면이라 스택의 일부로 읽힌다. */
@Composable
private fun MoreAlerts(hidden: Int, expanded: Boolean, onToggle: () -> Unit) {
    val p = Tokens.palette
    Surface(color = p.emgSoft, contentColor = p.emgInk,
        modifier = Modifier.fillMaxWidth().clickable(onClick = onToggle)) {
        Text(if (expanded) "접기" else "긴급·임박 ${hidden}건 더 — 눌러서 펼치기",
            Modifier.padding(horizontal = 20.dp, vertical = 8.dp),
            fontSize = Type.body, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun AlertBanner(b: AlertBannerUi, onOpen: () -> Unit, onCancel: () -> Unit, onDismiss: () -> Unit) {
    // 긴급 = 연한 빨강 면 + 진한 빨강 글자, 임박 = 주황 채움 + 먹 글자, 경보 = 연한 보라(데스크톱 배너 층과 같은 토큰).
    //   경과는 1초마다 다시 그린다 — 얼마나 됐는지가 대응 순서의 판단 재료다.
    val peril = !b.isAlert && b.kind == AlertKind.IMMINENT_PERIL
    val head = when {
        b.isAlert -> "긴급 경보"
        else -> b.kind.bannerTitle
    }
    BannerBar(
        tone = when { b.isAlert -> BannerTone.ALERT; peril -> BannerTone.PERIL; else -> BannerTone.EMERGENCY },
        line1 = head + if (b.initiator.isNotEmpty()) " · 개시 ${b.initiator}" else "",
        line2 = b.title,
        sinceMs = b.sinceMs,
    ) {
        // [해제] — 긴급·임박 = 조건 해제 요청, 경보 = 경보 취소(남의 경보면 제3자 취소). 자격이 있을 때만 선다.
        if (b.canCancel) PillButton(
            when { b.isAlert -> "경보 해제"; peril -> "임박 해제"; else -> "긴급 해제" }, onCancel,
            kind = when { b.isAlert -> Pill.MON_LINE; peril -> Pill.ON_PERIL_LINE; else -> Pill.EMG_LINE }, height = 44.dp)
        PillButton("채널로 이동", onOpen,
            kind = when { b.isAlert -> Pill.MON_FILL; peril -> Pill.ON_PERIL; else -> Pill.RED_FILL }, height = 44.dp)
        // [닫기] — 경보만. 이 화면의 표시만 내린다(취소 신호를 놓쳤을 때) — 서버의 경보는 그대로다.
        if (b.isAlert) PillButton("닫기", onDismiss, kind = Pill.MON_LINE, height = 44.dp)
    }
}
