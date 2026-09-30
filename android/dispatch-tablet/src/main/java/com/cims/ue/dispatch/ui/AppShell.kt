// 앱 껍데기 — 왼쪽 레일 · 상단 바 · 관제 탭 줄 · 본문(면 + 사이드 패널) · 발언 바 (android_dispatch_tablet.md §6.3)
//
// `MainActivity.Shell` 에서 **그리는 부분만** 떼어 낸 것이다. 세션·VM 을 모르므로 Preview 가 선다 — «실제 태블릿에서
// 보이는 한 장» 을 Preview 로 봐야 본문의 줄 수를 제대로 읽는다. 본문·발언 바·패널은 호출자가 넣는다.
//
// 세로 예산(가로 1280×800): 상단 바 64 + 탭 줄 48 + 본문 + 발언 바 80. 메뉴는 아래가 아니라 **왼쪽 레일(폭 80)** 이라
// 그만큼이 본문 높이로 간다. 시스템 막대(상태·제스처)가 보이면 그 몫은 본문에서 빠진다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.History
import androidx.compose.material.icons.filled.MoreHoriz
import androidx.compose.material.icons.filled.PushPin
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.SupportAgent
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.abs

/** 상단 바가 쓰는 값 — 내가 누구이고 어디에 붙어 있는가. */
data class TopBarUi(
    val displayName: String = "관제",
    /** "관제1과 · 대표 7000" — 전화 그룹이 없으면 빈 값. */
    val deskLine: String = "",
    /** 등록 점등 — 계정 수만큼 ●(등록) / ○(미등록). 권위는 코어 스냅샷이다. */
    val registrations: List<Boolean> = emptyList(),
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
    /** [더보기] 점 — [관리]에 저장하지 않은 폼. */
    val adminDirty: Boolean = false,
)

/** 레일 폭·패널 폭 — 시안 값(§6.3). */
val RailWidth = 80.dp
val PanelWidth = 400.dp

/**
 * 껍데기 한 장.
 *
 * @param tabs 관제 탭 줄 — [관제] 에서만 부른다(이력·더보기는 한 면이라 탭이 없다).
 * @param banners 긴급·착신·자격 배너 — 상단 바 바로 아래, 화면과 무관하게 뜬다(§6.2a).
 * @param notices 토스트 자리 — 본문 **위에 겹쳐** 우하단(§6.2a-2). 넘겨받은 Modifier 가 자리다.
 */
@Composable
fun AppShellContent(
    screen: AppScreen,
    top: TopBarUi = TopBarUi(),
    badges: NavBadges = NavBadges(),
    onSelect: (AppScreen) -> Unit = {},
    onSearch: () -> Unit = {},
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
            Rail(screen, badges, onSelect)
            Box(Modifier.weight(1f).fillMaxHeight()) {
                Column(Modifier.fillMaxSize()) {
                    TopBar(top, onSearch, menu)
                    banners()
                    if (screen == AppScreen.DISPATCH) tabs()
                    Box(Modifier.weight(1f).fillMaxWidth()) { body() }
                    // **발언 바는 상시로 둔다**(§6.3). 관제사는 전화를 받으면서도, 이력을 보면서도 무전한다.
                    talkBar()
                }
                notices(Modifier.align(Alignment.BottomEnd).padding(end = 12.dp, bottom = 92.dp))
            }
        }
    }
}

/** 왼쪽 레일 — 메뉴 셋. 고른 것은 알약 면 + 굵은 글자. */
@Composable
private fun Rail(screen: AppScreen, badges: NavBadges, onSelect: (AppScreen) -> Unit) {
    val p = Tokens.palette
    Column(
        Modifier.width(RailWidth).fillMaxHeight().background(p.fill)
            .drawBehind { drawLine(p.divider, Offset(size.width - 0.5f, 0f), Offset(size.width - 0.5f, size.height), 1.dp.toPx()) }
            .padding(top = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        // 앱 표시 — 로고가 정해지면 이 자리에 둔다.
        Box(Modifier.size(48.dp, 40.dp).clip(RoundedCornerShape(8.dp)).background(p.ink),
            contentAlignment = Alignment.Center) {
            Text("CIMS", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = p.onInk, letterSpacing = 0.5.sp)
        }
        Spacer(Modifier.height(8.dp))
        AppScreen.entries.forEach { s ->
            val on = s == screen
            val n = when (s) { AppScreen.DISPATCH -> badges.unread + badges.callWaiting + badges.smsUnread; else -> 0 }
            val dot = s == AppScreen.MORE && badges.adminDirty
            Column(
                Modifier.width(RailWidth).clickable { onSelect(s) }.padding(vertical = 4.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                Box(Modifier.size(56.dp, 32.dp).clip(RoundedCornerShape(16.dp))
                        .background(if (on) p.line else androidx.compose.ui.graphics.Color.Transparent),
                    contentAlignment = Alignment.Center) {
                    Icon(railIconOf(s), contentDescription = null, modifier = Modifier.size(22.dp), tint = p.ink)
                    if (n > 0) CountPill(n, Modifier.align(Alignment.TopEnd).padding(top = 0.dp))
                    if (dot) Box(Modifier.align(Alignment.TopEnd).padding(4.dp).size(8.dp).clip(RoundedCornerShape(4.dp))
                            .background(p.emergency))
                }
                Text(s.label, fontSize = Type.meta, fontWeight = if (on) FontWeight.Bold else FontWeight.Normal,
                    color = p.ink)
            }
        }
    }
}

internal fun railIconOf(s: AppScreen): ImageVector = when (s) {
    AppScreen.DISPATCH -> Icons.Filled.SupportAgent
    AppScreen.HISTORY -> Icons.Filled.History
    AppScreen.MORE -> Icons.Filled.MoreHoriz
}

/** 상단 바(64) — 이름 · 소속·대표번호 · 등록 점 · 검색 · 세션 메뉴. */
@Composable
private fun TopBar(top: TopBarUi, onSearch: () -> Unit, menu: @Composable () -> Unit) {
    val p = Tokens.palette
    Row(
        Modifier.fillMaxWidth().height(64.dp)
            .drawBehind { drawLine(p.divider, Offset(0f, size.height - 0.5f), Offset(size.width, size.height - 0.5f), 1.dp.toPx()) }
            .padding(start = 20.dp, end = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(top.displayName.ifBlank { "관제" }, fontSize = 20.sp, fontWeight = FontWeight.Bold, maxLines = 1)
        if (top.deskLine.isNotBlank()) Text(top.deskLine, fontSize = Type.body, color = p.muted, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
        // 등록 점등 — 계정마다 하나(●등록·○미등록).
        if (top.registrations.isNotEmpty()) Text(top.registrations.joinToString("") { if (it) "●" else "○" },
            fontSize = Type.body, letterSpacing = 2.sp)
        Spacer(Modifier.weight(1f))
        // 통합 검색 — 데스크톱 `Ctrl+K` 자리. 태블릿엔 그 입력이 없어 상단 바가 입구다(§6.2f).
        IconButton(onClick = onSearch, modifier = Modifier.size(44.dp)) {
            Icon(Icons.Filled.Search, contentDescription = "검색", modifier = Modifier.size(22.dp))
        }
        menu()
    }
}

/**
 * 관제 탭 줄(48) — [무전|통화] 세그먼트 · 그 모드의 하위 탭 · 뒤에 붙는 동작([사용자]).
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
        Segmented(
            options = DispatchMode.entries.map { it.label },
            selected = page.mode.ordinal,
            onSelect = { onMode(DispatchMode.entries[it]) },
            itemWidth = 104.dp,
            badges = listOf(0, badges.callWaiting))
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
                if (selected) drawRect(p.ink, topLeft = Offset(0f, size.height - 3.dp.toPx()),
                    size = androidx.compose.ui.geometry.Size(size.width, 3.dp.toPx()))
            },
        contentAlignment = Alignment.Center,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, fontSize = Type.strong, fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
                color = if (selected) p.ink else p.muted)
            if (badge > 0) { Spacer(Modifier.width(6.dp)); CountPill(badge) }
        }
    }
}

/**
 * [관제] 본문 — 면 pager + 오른쪽 사이드 패널(밀어내기).
 *
 * 패널은 **덮지 않고 민다** — 면의 폭이 그만큼 줄고 면이 스스로 다시 배치한다(타 채널 2열 → 1열). 내 채널처럼 폭이 정해진
 * 칸은 움직이지 않는다. 가장자리 스와이프로 열지 않는다 — 면 넘기기와 겹친다.
 *
 * @param panel 열린 패널 — null 이면 닫힘. 폭은 [PanelWidth] 로 이 함수가 준다.
 */
@Composable
fun DispatchBody(
    page: DispatchPage,
    onPage: (DispatchPage) -> Unit,
    panel: (@Composable () -> Unit)?,
    content: @Composable (DispatchPage) -> Unit,
) {
    Row(Modifier.fillMaxSize()) {
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
        HorizontalPager(state = pager, modifier = Modifier.weight(1f).fillMaxHeight()) { i ->
            Box(Modifier.fillMaxSize()) { content(DISPATCH_PAGES[i]) }
        }
        if (panel != null) panel()
    }
}

/**
 * 오른쪽 사이드 패널 틀(폭 400) — 머리(56: 종류 라벨 또는 ←, 제목, 고정, 닫기) + 내용.
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
    Surface(color = p.paper, contentColor = p.ink, shadowElevation = 10.dp,
        modifier = modifier.width(PanelWidth).fillMaxHeight()) {
        Column(Modifier.fillMaxSize()
            .drawBehind { drawLine(p.ink, Offset(0.75.dp.toPx(), 0f), Offset(0.75.dp.toPx(), size.height), 1.5.dp.toPx()) }) {
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
                        modifier = Modifier.size(18.dp), tint = if (pinned) p.ink else p.muted)
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
