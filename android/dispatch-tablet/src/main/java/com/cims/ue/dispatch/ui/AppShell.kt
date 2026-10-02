// 앱 껍데기 — 왼쪽 레일 · 상단 바 · 관제 탭 줄 · 본문(면 + 사이드 패널) · 발언 바 (android_dispatch_tablet.md §6.3)
//
// `MainActivity.Shell` 에서 **그리는 부분만** 떼어 낸 것이다. 세션·VM 을 모르므로 Preview 가 선다 — «실제 태블릿에서
// 보이는 한 장» 을 Preview 로 봐야 본문의 줄 수를 제대로 읽는다. 본문·발언 바·패널은 호출자가 넣는다.
//
// 세로 예산(가로 1280×800): 상단 바 64 + 탭 줄 48 + 본문 + 발언 바 80. 메뉴는 아래가 아니라 **왼쪽 레일(폭 80)** 이라
// 그만큼이 본문 높이로 간다. 시스템 막대(상태·제스처)가 보이면 그 몫은 본문에서 빠진다.
package com.cims.ue.dispatch.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.FastOutLinearInEasing
import androidx.compose.animation.core.LinearOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.PagerState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.outlined.AccountTree
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.outlined.Groups
import androidx.compose.material.icons.outlined.History
import androidx.compose.material.icons.filled.PushPin
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.SupportAgent
import androidx.compose.material.icons.outlined.PushPin
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.abs

/** 등록 점등 한 개 — 계정의 등록 상태(Windows `RegDot`: 회색 미등록 · 주황 등록 중 · 녹색 등록 · 빨강 실패). */
enum class RegDot { OFF, PENDING, ON, FAILED }

/** 상단 바가 쓰는 값 — 내가 누구이고 어디에 붙어 있는가. */
data class TopBarUi(
    val displayName: String = "관제",
    /** "관제1과 · 대표 7000" — 전화 그룹이 없으면 빈 값. */
    val deskLine: String = "",
    /** 등록 점등 — 계정마다 하나(PTT · 전화). 권위는 코어 스냅샷이다. */
    val registrations: List<RegDot> = emptyList(),
    /** 진행 중인 감청·청취 수 — 0 이면 칩을 그리지 않는다(데스크톱 «감청 중 N»). */
    val monitors: Int = 0,
)

/**
 * 레일·탭의 배지 — 어디에 뭐가 쌓였나(요약 띠를 대신한다, §6.10). **안 본 것이 있다** 만 말한다.
 */
data class NavBadges(
    /** [관제] › [무전] › «메시지» — 미읽음 SDS. */
    val unread: Int = 0,
    /** [관제] › [통화] — 응답을 기다리는 것(울리는 착신 + 대표번호 대기열). */
    val callWaiting: Int = 0,
    /** [관제] › [통화] › «메시지» — 미읽음 문자. */
    val smsUnread: Int = 0,
    /** 레일 [관리] 점 — 저장하지 않은 폼. */
    val adminDirty: Boolean = false,
)

/** 레일 폭(§6.3). */
val RailWidth = 80.dp

/**
 * 오른쪽 패널 폭 — 기본 400. 패널 왼쪽 가장자리를 **끌어서** 바꾼다(태블릿은 화면이 좁아 겹친 패널이 가리는 만큼을 사람이
 * 정한다): [PanelMinWidth] ~ 본문 폭 − [PanelBodyKeep]. 끈 값은 설정에 남는다(`Settings.panelWidthDp`).
 */
val PanelWidth = 400.dp
val PanelMinWidth = 320.dp
/** 패널을 아무리 넓혀도 본문에 남기는 폭 — 내 채널 칸·통화 고정 칸의 절반은 늘 보인다. */
val PanelBodyKeep = 240.dp

/** 지금의 패널 폭 — 패널 틀([SidePanelFrame])이 읽는다. Preview 처럼 본문 밖에서 그리면 기본값이다. */
val LocalPanelWidth = androidx.compose.runtime.compositionLocalOf { PanelWidth }

/** 끈 뒤의 패널 폭 — 순수 함수(시험 대상). [dragDp] 는 왼쪽으로 끈 만큼이 양수다. */
internal fun panelWidthAfterDrag(width: Dp, dragDp: Dp, bodyWidth: Dp): Dp {
    val max = (bodyWidth - PanelBodyKeep).coerceAtLeast(PanelMinWidth)
    return (width + dragDp).coerceIn(PanelMinWidth, max)
}

/**
 * 껍데기 한 장.
 *
 * @param tabs 관제 탭 줄 — [관제] 에서만 부른다(이력·PTT 그룹·관리는 한 면이라 탭이 없다).
 * @param banners 긴급·착신·자격 배너 — 상단 바 바로 아래, 화면과 무관하게 뜬다(§6.2a).
 * @param notices 토스트 자리 — 본문 **위에 겹쳐** 우하단(§6.2a-2). 넘겨받은 Modifier 가 자리다.
 * @param canAdmin 관리 범위가 있는가 — 없으면 레일 [관리] 가 흐리고, 누르면 [onAdminDenied].
 * @param panelOpen 오른쪽 패널이 열려 있다 — 토스트가 패널 왼쪽에 선다([panelWidth] 만큼).
 */
@Composable
fun AppShellContent(
    screen: AppScreen,
    top: TopBarUi = TopBarUi(),
    badges: NavBadges = NavBadges(),
    onSelect: (AppScreen) -> Unit = {},
    onSearch: () -> Unit = {},
    onSettings: () -> Unit = {},
    canAdmin: Boolean = true,
    onAdminDenied: () -> Unit = {},
    onMonitors: () -> Unit = {},
    panelOpen: Boolean = false,
    panelWidth: Dp = PanelWidth,
    menu: @Composable () -> Unit = {},
    talkBar: @Composable () -> Unit = {},
    banners: @Composable ColumnScope.() -> Unit = {},
    tabs: @Composable () -> Unit = {},
    notices: @Composable (Modifier) -> Unit = {},
    body: @Composable () -> Unit,
) {
    val p = Tokens.palette
    Surface(color = p.paper, contentColor = p.ink, modifier = Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing)) {
            Rail(screen, badges, onSelect, onSettings, canAdmin, onAdminDenied)
            Box(Modifier.weight(1f).fillMaxHeight()) {
                Column(Modifier.fillMaxSize()) {
                    TopBar(top, onSearch, onMonitors, menu)
                    banners()
                    if (screen == AppScreen.DISPATCH) tabs()
                    Box(Modifier.weight(1f).fillMaxWidth()) { body() }
                    // **발언 바는 상시로 둔다**(§6.3). 관제사는 전화를 받으면서도, 이력을 보면서도 무전한다.
                    talkBar()
                }
                // 패널이 열린 동안은 패널 왼쪽에 선다 — 패널이 토스트를 가리지 않게.
                notices(Modifier.align(Alignment.BottomEnd)
                    .padding(end = if (panelOpen && screen == AppScreen.DISPATCH) panelWidth + 12.dp else 12.dp, bottom = 92.dp))
            }
        }
    }
}

/**
 * 왼쪽 레일 — [관제][이력][PTT 그룹][관리] + 바닥 [설정]. 전부 한 번에 누른다. 고른 것 = 연한 남색 알약 + 남색 아이콘·굵은 글자
 * (Windows `RailItem`).
 */
@Composable
private fun Rail(screen: AppScreen, badges: NavBadges, onSelect: (AppScreen) -> Unit, onSettings: () -> Unit,
                 canAdmin: Boolean, onAdminDenied: () -> Unit) {
    val p = Tokens.palette
    Column(
        Modifier.width(RailWidth).fillMaxHeight().background(p.rail)
            .drawBehind { drawLine(p.divider, Offset(size.width - 0.5f, 0f), Offset(size.width - 0.5f, size.height), 1.dp.toPx()) }
            .padding(top = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(Modifier.size(48.dp, 40.dp).clip(RoundedCornerShape(8.dp)).background(p.primary),
            contentAlignment = Alignment.Center) {
            Text("CIMS", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = p.onPrimary, letterSpacing = 0.5.sp)
        }
        Spacer(Modifier.height(20.dp))
        AppScreen.entries.forEach { s ->
            val enabled = s != AppScreen.ADMIN || canAdmin
            RailItem(
                label = s.label, icon = railIconOf(s), selected = s == screen, enabled = enabled,
                count = if (s == AppScreen.DISPATCH) badges.unread + badges.callWaiting + badges.smsUnread else 0,
                dot = s == AppScreen.ADMIN && badges.adminDirty,
                onClick = { if (enabled) onSelect(s) else onAdminDenied() })
            Spacer(Modifier.height(12.dp))
        }
        Spacer(Modifier.weight(1f))
        // [설정] — 화면이 아니라 시트를 연다(켜짐 표시 없음).
        RailItem(label = "설정", icon = Icons.Outlined.Settings, selected = false, onClick = onSettings)
        Spacer(Modifier.height(12.dp))
    }
}

@Composable
private fun RailItem(label: String, icon: ImageVector, selected: Boolean, onClick: () -> Unit,
                     enabled: Boolean = true, count: Int = 0, dot: Boolean = false) {
    val p = Tokens.palette
    val fg = if (selected) p.primaryInk else p.ink2
    Column(
        Modifier.width(72.dp).alpha(if (enabled) 1f else 0.45f).clip(RoundedCornerShape(12.dp)).clickable(onClick = onClick)
            .padding(vertical = 2.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Box(Modifier.size(56.dp, 32.dp), contentAlignment = Alignment.Center) {
            Box(Modifier.fillMaxSize().clip(RoundedCornerShape(16.dp))
                    .background(if (selected) p.railActive else Color.Transparent), contentAlignment = Alignment.Center) {
                Icon(icon, contentDescription = null, modifier = Modifier.size(22.dp), tint = fg)
            }
            if (count > 0) CountPill(count, Modifier.align(Alignment.TopEnd))
            if (dot) Box(Modifier.align(Alignment.TopEnd).padding(4.dp).size(8.dp).clip(CircleShape).background(p.emg))
        }
        Text(label, fontSize = Type.meta, fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal, color = fg,
            maxLines = 1)
    }
}

internal fun railIconOf(s: AppScreen): ImageVector = when (s) {
    // 선 아이콘(Outlined) — 데스크톱 레일의 스트로크 아이콘과 같은 인상(채움 아이콘은 레일이 무겁다)
    AppScreen.DISPATCH -> Icons.Outlined.SupportAgent
    AppScreen.HISTORY -> Icons.Outlined.History
    AppScreen.PTT_GROUPS -> Icons.Outlined.Groups
    AppScreen.ADMIN -> Icons.Outlined.AccountTree
}

/** 상단 바(64) — 이름 · 소속·대표번호 · 등록 점 · «감청 중 N» · 검색 칸 · 세션 메뉴. */
@Composable
private fun TopBar(top: TopBarUi, onSearch: () -> Unit, onMonitors: () -> Unit, menu: @Composable () -> Unit) {
    val p = Tokens.palette
    Row(
        Modifier.fillMaxWidth().height(64.dp).background(p.paper)
            .drawBehind { drawLine(p.divider, Offset(0f, size.height - 0.5f), Offset(size.width, size.height - 0.5f), 1.dp.toPx()) }
            .padding(start = 20.dp, end = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(top.displayName.ifBlank { "관제" }, fontSize = Type.display, fontWeight = FontWeight.Bold, maxLines = 1)
        if (top.deskLine.isNotBlank()) Text(top.deskLine, fontSize = Type.body, color = p.muted, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
        // 등록 점등 — 계정마다 하나: 회색 미등록 · 주황 등록 중 · 녹색 등록 · 빨강 실패.
        if (top.registrations.isNotEmpty()) Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            top.registrations.forEach { r ->
                StatusDot(when (r) { RegDot.ON -> p.talk; RegDot.PENDING -> p.ring; RegDot.FAILED -> p.emg; RegDot.OFF -> p.wire })
            }
        }
        Spacer(Modifier.weight(1f))
        // 진행 중인 감청·청취 — 누르면 그 자리로(데스크톱 «감청 중 N» 칩, §6.5).
        if (top.monitors > 0) Row(
            Modifier.height(32.dp).clip(RoundedCornerShape(16.dp)).background(p.monSoft)
                .border(1.dp, p.mon, RoundedCornerShape(16.dp)).clickable(onClick = onMonitors).padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Box(Modifier.size(8.dp).clip(CircleShape).background(p.mon))
            Text("감청 중 ${top.monitors}", fontSize = Type.body, fontWeight = FontWeight.SemiBold, color = p.monInk)
        }
        // 통합 검색 — 데스크톱 `Ctrl+K` 자리. 태블릿엔 그 입력이 없어 상단 바가 입구다(§6.2f).
        Row(
            Modifier.width(280.dp).height(36.dp).clip(RoundedCornerShape(8.dp)).background(p.paper)
                .border(1.dp, p.line, RoundedCornerShape(8.dp)).clickable(onClickLabel = "검색", onClick = onSearch)
                .padding(horizontal = 10.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Icon(Icons.Filled.Search, contentDescription = null, modifier = Modifier.size(18.dp), tint = p.muted)
            Text("이름 · 내선 · PTT 번호 · 채널", fontSize = Type.body, color = p.muted, maxLines = 1)
        }
        menu()
    }
}

/**
 * 관제 탭 줄(48) — [무전|통화] 세그먼트 · 그 모드의 하위 탭 · 뒤에 붙는 목록 버튼(무전 [사용자] · 통화 [주소록]).
 *
 * 탭 줄은 **면 pager 위에 고정**으로 놓인다 — 면을 밀 때 줄은 제자리에 남고 본문만 미끄러진다. 강조는 **정착할 면**을
 * 가리킨다(밀기가 끝나기 전에도 도착할 탭이 켜진다).
 */
@Composable
fun DispatchTabs(
    page: DispatchPage,
    onMode: (DispatchMode) -> Unit,
    onPage: (DispatchPage) -> Unit,
    badges: NavBadges = NavBadges(),
    trailing: @Composable RowScope.() -> Unit = {},
) {
    val p = Tokens.palette
    Row(
        Modifier.fillMaxWidth().height(48.dp).background(p.bar)
            .drawBehind { drawLine(p.divider, Offset(0f, size.height - 0.5f), Offset(size.width, size.height - 0.5f), 1.dp.toPx()) }
            .padding(start = 16.dp, end = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        // 수 = 무전: 안 읽은 무전 메시지, 통화: 응답 대기 + 안 읽은 문자(데스크톱 탭 줄과 같다).
        Segmented(
            options = DispatchMode.entries.map { it.label },
            selected = page.mode.ordinal,
            onSelect = { onMode(DispatchMode.entries[it]) },
            itemWidth = 104.dp,
            badges = listOf(badges.unread, badges.callWaiting + badges.smsUnread))
        Box(Modifier.width(1.dp).height(24.dp).background(p.line))
        Row(Modifier.fillMaxHeight()) {
            val tabs = when (page.mode) {
                DispatchMode.PTT -> PttPane.entries.map { pageOf(it) to it.label }
                DispatchMode.CALL -> CallPane.entries.map { pageOf(it) to it.label }
            }
            tabs.forEach { (target, label) ->
                val n = when {
                    target.pttPane == PttPane.MESSAGES -> badges.unread
                    target.callPane == CallPane.MESSAGES -> badges.smsUnread
                    else -> 0
                }
                SubTab(label, target == page, n) { onPage(target) }
            }
        }
        Spacer(Modifier.weight(1f))
        trailing()
    }
}

@Composable
private fun SubTab(label: String, selected: Boolean, badge: Int, onClick: () -> Unit) {
    val p = Tokens.palette
    Box(
        Modifier.width(112.dp).fillMaxHeight().clickable(onClick = onClick)
            .drawBehind {
                if (selected) drawRect(p.primaryLine, topLeft = Offset(0f, size.height - 3.dp.toPx()),
                    size = androidx.compose.ui.geometry.Size(size.width, 3.dp.toPx()))
            },
        contentAlignment = Alignment.Center,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, fontSize = Type.strong, fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
                color = if (selected) p.primaryInk else p.muted)
            if (badge > 0) { Spacer(Modifier.width(6.dp)); CountPill(badge) }
        }
    }
}

/**
 * 탭 줄 오른쪽 끝의 목록 버튼([사용자]·[주소록]) — 열린 동안 연한 남색(Windows `ListToggle`).
 */
@Composable
fun ListToggle(text: String, open: Boolean, onClick: () -> Unit, leading: ImageVector? = null) {
    val p = Tokens.palette
    val shape = RoundedCornerShape(18.dp)
    Row(
        Modifier.height(36.dp).clip(shape).background(if (open) p.primarySoft else p.paper)
            .border(1.5.dp, if (open) p.primaryLine else p.edge, shape).clickable(onClick = onClick).padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        val fg = if (open) p.primaryInk else p.ink
        if (leading != null) Icon(leading, contentDescription = null, modifier = Modifier.size(16.dp), tint = fg)
        Text(text, fontSize = Type.strong, fontWeight = FontWeight.Bold, color = fg, maxLines = 1)
    }
}

/**
 * 한 모드의 면들 위에 **한 벌만** 얹는 왼쪽 고정 칸 — [통화] 의 대기열·진행 중·내 통화·그룹원(§6.3).
 *
 * 면마다 그리면 면을 밀 때 같이 미끄러지고, 펼친 감청·열린 전달 칸 같은 칸 안의 상태도 면마다 따로 논다. 그래서 pager 밖에
 * 한 벌을 두고 면은 그 폭만큼 왼쪽을 비운다. 그 모드의 면끼리 오갈 때는 제자리, 다른 모드로 넘어갈 때만 그 모드의 첫·끝 면과
 * 함께 미끄러져 들고 난다([fixedShift]).
 *
 * @param width 칸 폭(오른쪽 나눔선 포함) — 그 모드의 면은 이만큼 왼쪽을 비워 둔다.
 */
class FixedColumn(val mode: DispatchMode, val width: Dp, val content: @Composable () -> Unit)

/**
 * 고정 칸의 가로 밀림(면 폭 단위) — pager 위치 [pos](현재 면 + 밀린 비율)에서. 그 모드의 면([first]..[last]) 안이면 0(제자리),
 * 앞 모드에서 넘어오는 중이면 0~1(오른쪽에서 들어온다), 뒤로 나가는 중이면 0~-1. 한 면 넘게 떨어지면 ±1(화면 밖)에서 멈춘다.
 * 순수 함수(시험 대상).
 */
internal fun fixedShift(pos: Float, first: Int, last: Int): Float = when {
    pos < first -> (first - pos).coerceAtMost(1f)
    pos > last -> -(pos - last).coerceAtMost(1f)
    else -> 0f
}

/** 패널이 밀려 들어오고 나가는 시간(ms) — Windows `SlideOverPanel` 과 같다(들어옴 220 감속 · 나감 180 가속). */
internal const val PANEL_IN_MS = 220
internal const val PANEL_OUT_MS = 180

/**
 * [관제] 본문 — 면 pager + 오른쪽 사이드 패널.
 *
 * 패널은 **본문 위에 겹친다** — 오른쪽 끝에서 왼쪽으로 밀려 들어오고 닫으면 오른쪽으로 밀려 나간다(Windows 관제 앱과 같다,
 * dispatch_desktop_ui.md §3.6). 본문(면)은 폭·배치를 바꾸지 않고 오른쪽 400 이 패널 아래에 가려진다. 같은 패널 안에서 내용만
 * 바뀌면(다른 대상 = 교체) 움직이지 않는다. 가장자리 스와이프로 열지 않는다 — 면 넘기기와 겹친다.
 *
 * 패널 왼쪽 가장자리의 손잡이를 끌면 폭이 바뀐다([panelWidthAfterDrag]) — 놓을 때 [onPanelWidth] 로 알린다(설정에 저장).
 *
 * @param panel 열린 패널 — null 이면 닫힘.
 * @param panelWidth 패널 폭(저장된 값). 본문이 좁으면 그 안으로 죈다.
 * @param fixed 한 모드의 면들 위에 얹는 고정 칸([FixedColumn]) — 없으면 null.
 */
@Composable
fun DispatchBody(
    page: DispatchPage,
    onPage: (DispatchPage) -> Unit,
    panel: (@Composable () -> Unit)?,
    fixed: FixedColumn? = null,
    panelWidth: Dp = PanelWidth,
    onPanelWidth: (Dp) -> Unit = {},
    content: @Composable (DispatchPage) -> Unit,
) {
    BoxWithConstraints(Modifier.fillMaxSize().clipToBounds()) {
        // **저장·복원하지 않는다**(`rememberPagerState` 가 아니라 `remember`). 면의 기억은 VM(NavState)이 갖는다 —
        //   pager 가 한 벌 더 가지면 복원된 옛 면이 VM 의 면을 덮는다.
        val pager = remember { PagerState(currentPage = page.index.coerceAtLeast(0)) { DISPATCH_PAGES.size } }
        // 탭·배너·사람 메뉴가 면을 **바꿨을 때만** 따라간다 — 규칙은 [PaneRequest] 가 갖는다.
        val request = remember { PaneRequest() }
        LaunchedEffect(page.index) { request.onWant(page.index) }
        LaunchedEffect(request.pending) {
            val want = request.pending ?: return@LaunchedEffect
            try { pager.goTo(want) } finally { request.finish(want) }
        }
        // **자리가 잡힌 면만** 알린다 — 요청을 든 동안은 알리지 않는다(지나가는 면이 요청을 덮지 않게).
        val settled = pager.currentPage == pager.targetPage
        LaunchedEffect(settled, pager.currentPage, request.pending) {
            if (settled && request.canReport) {
                val now = DISPATCH_PAGES[pager.currentPage]
                if (now != page) onPage(now)
            }
        }
        HorizontalPager(state = pager, modifier = Modifier.fillMaxSize()) { i ->
            Box(Modifier.fillMaxSize()) { content(DISPATCH_PAGES[i]) }
        }
        if (fixed != null) {
            val first = DISPATCH_PAGES.indexOfFirst { it.mode == fixed.mode }
            val last = DISPATCH_PAGES.indexOfLast { it.mode == fixed.mode }
            val pageWidth = constraints.maxWidth.toFloat()
            // 위치는 그리기 단계에서만 읽는다 — 미는 동안 칸을 다시 구성하지 않는다.
            Box(Modifier.width(fixed.width).fillMaxHeight().graphicsLayer {
                translationX = fixedShift(pager.currentPage + pager.currentPageOffsetFraction, first, last) * pageWidth
            }) { fixed.content() }
        }
        // 닫는 순간 VM 은 패널을 곧바로 비운다 — 밀려 나가는 동안 그릴 내용은 마지막 것을 든다(빈 패널이 미끄러지지 않게).
        var last by remember { mutableStateOf(panel) }
        if (panel != null) last = panel
        // 끄는 동안의 폭은 여기서 든다(저장값은 놓을 때 한 번) — 끌 때마다 설정을 쓰지 않는다.
        val bodyWidth = maxWidth
        var width by remember(panelWidth, bodyWidth) { mutableStateOf(panelWidthAfterDrag(panelWidth, 0.dp, bodyWidth)) }
        val density = androidx.compose.ui.platform.LocalDensity.current
        AnimatedVisibility(
            visible = panel != null,
            modifier = Modifier.align(Alignment.CenterEnd),
            enter = slideInHorizontally(tween(PANEL_IN_MS, easing = LinearOutSlowInEasing)) { it },
            exit = slideOutHorizontally(tween(PANEL_OUT_MS, easing = FastOutLinearInEasing)) { it },
        ) {
            // 패널의 빈 자리를 민 손짓이 아래 면(pager)으로 새지 않게 여기서 받는다.
            Box(Modifier.width(width).fillMaxHeight().pointerInput(Unit) {}) {
                androidx.compose.runtime.CompositionLocalProvider(LocalPanelWidth provides width) { (panel ?: last)?.invoke() }
                PanelGrip(
                    onDrag = { px -> width = panelWidthAfterDrag(width, with(density) { (-px).toDp() }, bodyWidth) },
                    onEnd = { onPanelWidth(width) },
                    modifier = Modifier.align(Alignment.CenterStart))
            }
        }
    }
}

/**
 * 패널 폭 손잡이 — 패널 안 왼쪽 가장자리의 세로 띠(닿는 폭 16 — 패널 밖으로 내면 부모 경계 밖이라 손짓을 못 받는다). 가운데 짧은
 * 막대가 «끌 수 있다» 를 말한다. 끄는 동안 막대가 남색이 된다.
 */
@Composable
private fun PanelGrip(onDrag: (Float) -> Unit, onEnd: () -> Unit, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    var dragging by remember { mutableStateOf(false) }
    Box(
        modifier.width(16.dp).fillMaxHeight()
            .pointerInput(Unit) {
                detectHorizontalDragGestures(
                    onDragStart = { dragging = true },
                    onDragEnd = { dragging = false; onEnd() },
                    onDragCancel = { dragging = false; onEnd() },
                ) { change, amount -> change.consume(); onDrag(amount) }
            },
        contentAlignment = Alignment.Center,
    ) {
        Box(Modifier.size(5.dp, 44.dp).clip(RoundedCornerShape(3.dp))
            .background(if (dragging) p.primaryLine else p.edge))
    }
}

/**
 * 오른쪽 사이드 패널 틀(폭 = [LocalPanelWidth], 기본 400) — 머리(56: 종류 라벨 또는 ←, 제목, 고정, 닫기) + 내용.
 *
 * @param tag 종류 라벨(«채널 상세»·«사용자»·«이벤트 상세»). [onBack] 이 있으면(패널 안에서 한 겹 들어온 것) 라벨 대신 ← 이다.
 * @param pinned 고정 상태 — null 이면 고정 단추를 두지 않는다(한 겹 들어온 폼·이벤트 상세).
 */
@Composable
fun SidePanelFrame(
    title: String,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
    tag: String? = null,
    onBack: (() -> Unit)? = null,
    pinned: Boolean? = null,
    onPin: () -> Unit = {},
    content: @Composable ColumnScope.() -> Unit,
) {
    val p = Tokens.palette
    Surface(color = p.paper, contentColor = p.ink, shadowElevation = 12.dp,
        modifier = modifier.width(LocalPanelWidth.current).fillMaxHeight()) {
        Column(Modifier.fillMaxSize()
            .drawBehind { drawLine(p.edge, Offset(0.5.dp.toPx(), 0f), Offset(0.5.dp.toPx(), size.height), 1.dp.toPx()) }) {
            Row(
                Modifier.fillMaxWidth().height(56.dp)
                    .drawBehind { drawLine(p.divider, Offset(0f, size.height - 0.5f), Offset(size.width, size.height - 0.5f), 1.dp.toPx()) }
                    .padding(start = if (onBack != null) 4.dp else 16.dp, end = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                if (onBack != null) IconButton(onClick = onBack, modifier = Modifier.size(44.dp)) {
                    Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "뒤로", modifier = Modifier.size(20.dp))
                } else if (tag != null) Label(tag)
                Text(title, fontSize = Type.head, fontWeight = FontWeight.Bold, maxLines = 1,
                    overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                if (pinned != null) IconButton(onClick = onPin, modifier = Modifier.size(40.dp)) {
                    Icon(if (pinned) Icons.Filled.PushPin else Icons.Outlined.PushPin,
                        contentDescription = if (pinned) "패널 고정 풀기" else "패널 고정",
                        modifier = Modifier.size(18.dp), tint = if (pinned) p.primaryInk else p.muted)
                }
                IconButton(onClick = onClose, modifier = Modifier.size(44.dp)) {
                    Icon(Icons.Filled.Close, contentDescription = "패널 닫기", modifier = Modifier.size(20.dp))
                }
            }
            content()
        }
    }
}

/**
 * 면 이동 요청의 수명 — VM 이 바꾼 면(요청)을 **닿거나, 손가락이 끊거나, 새 요청이 대신할 때까지** 든다.
 *
 *   · pager 가 선 뒤의 **첫 값은 요청이 아니다** — 그때의 면은 pager 가 이미 그 자리에서 시작했다.
 *   · 요청을 든 동안은 좌표를 알리지 않는다([canReport]) — 지나가는 면이 정착하는 순간 옛 면의 알림에 덮이지 않게.
 *   · 이동이 끝나면(닿음·손가락이 끊음) 그 요청을 내려놓는다([finish]). 새 요청이 이미 대신했으면 옛 끝맺음은 새 요청을
 *     건드리지 않는다.
 *
 * Compose 효과 밖의 값으로 떼어 둔 것은 시험 때문이다 — 끊는 순서를 기기 없이 고정한다.
 */
internal class PaneRequest {
    /** 지금 가야 할 면. 없으면 null. Compose 가 읽는 상태다(효과의 열쇠). */
    var pending by mutableStateOf<Int?>(null)
        private set
    private var seenFirst = false

    /** VM 의 면이 바뀌었다. */
    fun onWant(pane: Int) {
        if (seenFirst) pending = pane else seenFirst = true
    }

    /** [pane] 으로 가던 이동이 끝났다 — 닿았든 끊겼든. 그 요청이 아직 지금의 요청일 때만 내려놓는다. */
    fun finish(pane: Int) {
        if (pending == pane) pending = null
    }

    /** 좌표를 알려도 되는가 — 요청을 들고 있지 않을 때만. */
    val canReport: Boolean get() = pending == null
}

/**
 * 한 칸이면 미끄러지듯, 멀면 곧바로. 멀리 갈 때 쓸고 가면 지나치는 면이 한 번씩 그려졌다 사라져 느리고, 그 면들이
 * 자기를 «지금 면» 으로 알려 이동이 거기서 멈춘다.
 */
private suspend fun PagerState.goTo(page: Int) {
    val to = page.coerceIn(0, (pageCount - 1).coerceAtLeast(0))
    if (to == currentPage) return
    if (abs(to - currentPage) == 1) animateScrollToPage(to) else scrollToPage(to)
}
