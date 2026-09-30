// 앱 껍데기 — 상단 바 · 발언 바 · 하단 내비 (android_dispatch_tablet.md §6.3)
//
// `MainActivity.Shell` 에서 **그리는 부분만** 떼어 낸 것이다. 떼어 낸 이유는 하나다 —
// **«실제 태블릿에서 보이는 한 장»** 을 Preview 로 봐야 하기 때문이다. 본문만 Preview 하면 상단 바 56 +
// 발언 바 80 + 하단 내비 56 = 192dp 가 빠진 그림이라, 본문이 실제보다 넉넉해 보이고 «한 화면에 몇 줄» 도
// 틀리게 읽힌다.
//
// 세션·VM 을 모르므로 Preview 가 선다. 본문·발언 바는 호출자가 넣는다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.foundation.LocalOverscrollFactory
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.PagerDefaults
import androidx.compose.foundation.pager.PagerState
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
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
 * 하단 내비의 배지 — 어디에 뭐가 쌓였나(요약 띠를 대신한다, §6.10).
 *
 * 감청·청취 수는 **메뉴 배지로 세지 않는다** — 그 둘은 [통화]·[무전] 안의 면이라 그쪽 탭 배지가 말한다.
 * 메뉴 배지는 «이 메뉴에 안 본 것이 있다» 만 말한다.
 */
data class NavBadges(
    /** [무전] — 미읽음 SDS. */
    val unread: Int = 0,
    /** [더보기] 점 — [관리]에 저장하지 않은 폼. */
    val adminDirty: Boolean = false,
)

/**
 * 껍데기 한 장. 본문은 [content], 발언 바는 [talkBar] 로 받는다.
 *
 * @param banners 착신·자격 배너 — 상단 바 바로 아래, 화면과 무관하게 뜬다(§6.2a).
 */
@OptIn(ExperimentalMaterial3Api::class)
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
    /** 지금 보고 있는 [무전] 면 — 스와이프가 이 값을 바꾼다. */
    pttPane: PttPane = PttPane.CHANNELS,
    /** 지금 보고 있는 [통화] 면. */
    callPane: CallPane = CallPane.CALLS,
    /** 스와이프가 다른 장으로 넘어갔다 — 메뉴·면을 함께 알린다. */
    onPage: (AppPage) -> Unit = {},
    /**
     * 그 메뉴의 탭줄 — **면 pager 위에 고정으로** 놓인다(면이 하나인 메뉴는 아무것도 그리지 않는다).
     * 면을 밀 때 이 줄이 같이 미끄러지면 메뉴가 통째로 바뀐 것처럼 보인다(§6.3).
     */
    tabs: @Composable (AppPage) -> Unit = {},
    /**
     * 토스트 자리 — 본문 **위에 겹쳐** 우하단에 선다(§6.2a-2, 데스크톱 §3.2). 본문을 밀지 않는다 — 실패를 알릴 때마다
     * 목록이 들썩이면 보던 자리를 잃는다. 넘겨받은 Modifier 가 자리(정렬·여백)다.
     */
    notices: @Composable (Modifier) -> Unit = {},
    /** 한 장의 본문. 스와이프로 미리 그려 두므로 **선택된 것만이 아니라 요청받은 장**을 그린다. */
    content: @Composable ColumnScope.(AppPage) -> Unit,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Row(verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        Text(top.displayName.ifBlank { "관제" }, fontWeight = FontWeight.Bold)
                        if (top.deskLine.isNotBlank()) Text(top.deskLine, fontSize = Type.strong)
                        if (top.registrations.isNotEmpty()) Text(
                            top.registrations.joinToString(" ") { if (it) "●" else "○" },
                            fontSize = Type.strong)
                    }
                },
                actions = {
                    // 감청 칩은 없다 — 감청은 «진행 중» 행의 상태이고 켜고 끄는 자리가 거기다(§6.5).
                    // 통합 검색 — 데스크톱의 `Ctrl+K` 자리. 태블릿엔 그 입력이 없어 상단 바가 입구다(§6.2f).
                    IconButton(onClick = onSearch) {
                        Icon(Icons.Filled.Search, contentDescription = "검색")
                    }
                    menu()
                })
        },
        bottomBar = {
            Column {
                // **발언 바는 내비 위에 상시로 둔다**(§6.3). 관제사는 전화를 받으면서도, 이력을 보면서도
                //   무전한다 — 발언만은 «어느 화면을 보고 있는가» 와 무관한 조작이다.
                talkBar()
                NavigationBar {
                    AppScreen.entries.forEach { s ->
                        NavigationBarItem(
                            selected = screen == s,
                            onClick = { onSelect(s) },
                            icon = {
                                val dot = s == AppScreen.MORE && badges.adminDirty
                                val n = if (s == AppScreen.PTT) badges.unread else 0
                                when {
                                    dot -> BadgedBox(badge = { Badge() }) {
                                        Icon(navIconOf(s), contentDescription = s.label)
                                    }
                                    n > 0 -> BadgedBox(badge = { Badge { Text("$n") } }) {
                                        Icon(navIconOf(s), contentDescription = s.label)
                                    }
                                    else -> Icon(navIconOf(s), contentDescription = s.label)
                                }
                            },
                            label = { Text(s.label) })
                    }
                }
            }
        }
    ) { pad ->
      Box(Modifier.fillMaxSize().padding(pad)) {
        Column(Modifier.fillMaxSize()) {
            banners()
            // ── 이동은 **겹친 pager 둘** ────────────────────────────────────────────────
            //
            // 바깥 = 메뉴, 안쪽 = 그 메뉴의 면. 겹치는 이유는 **탭줄이 어디에 붙느냐**가 둘 사이에서
            //   다르기 때문이다:
            //     · 면을 밀 때  — 탭줄은 **제자리**, 아래 본문만 미끄러진다(안쪽 pager 만 움직인다).
            //     · 메뉴가 바뀔 때 — 탭줄과 본문이 **한 덩어리로** 옆으로 나간다(바깥 pager 가 움직인다).
            //   한 줄짜리 pager 로 펴면 면을 밀 때도 탭줄이 함께 미끄러져 «메뉴가 바뀐 줄» 알게 된다.
            //
            // 끝 면에서 계속 밀면 안쪽이 더 갈 곳이 없어 **남은 끌기가 바깥으로 넘어간다**(중첩 스크롤) —
            //   그래서 통화›«통화내역» → [더보기] 가 한 동작으로 이어진다.
            val outer = rememberPagerState(initialPage = screen.ordinal) { AppScreen.entries.size }
            LaunchedEffect(screen) { outer.goTo(screen.ordinal) }
            HorizontalPager(
                state = outer,
                // **손가락을 같은 거리만 움직여도 넘어가게** 한다. 안쪽에서 넘어온 끌기에는 속도가 거의
                //   붙지 않아(중첩 스크롤로 넘길 때 fling 은 안쪽이 먼저 받는다) 기본값 0.5 를 그대로 두면
                //   화면 절반을 끌어야 메뉴가 바뀐다 — 면을 넘길 때(가볍게 튕기면 넘어간다)와 너무 다르다.
                flingBehavior = PagerDefaults.flingBehavior(
                    state = outer, snapPositionalThreshold = MENU_SNAP_THRESHOLD),
                modifier = Modifier.weight(1f)) { mi ->
                val menu = AppScreen.entries[mi]
                MenuPage(
                    // **목적지까지 같아야 «정착»** 이다. `currentPage` 만 보면 건너뛰는 도중 지나가는
                    //   장이 절반을 넘는 순간 자기를 정착으로 알리고, 그 알림이 목적지를 덮어써 이동이
                    //   중간에 선다(내비로 두 칸 이상 건너뛸 때의 그 증상).
                    menu = menu,
                    settled = outer.currentPage == mi && outer.targetPage == mi,
                    screen = screen,
                    wantPane = paneIndexOf(menu, pttPane, callPane),
                    onPage = onPage, tabs = tabs, content = content)
            }
        }
        notices(Modifier.align(Alignment.BottomEnd).padding(12.dp))
      }
    }
}

/**
 * 메뉴 한 장 — 고정 탭줄 + 면 pager.
 *
 * **들어올 때 어느 면에 서는가** 가 이 함수의 핵심이다. `initialPage` 는 이 장이 처음 그려질 때 한 번만
 * 읽히는데, 그 시점이 «어떻게 들어왔는가» 를 그대로 말해 준다:
 *
 * | 들어온 경로 | 그릴 때의 [screen] | 서는 면 |
 * |---|---|---|
 * | 하단 내비 탭 | **이미 이 메뉴**(VM 이 먼저 바뀐다) | 기억한 면 |
 * | 앞으로 밀어서 | 아직 앞 메뉴 | **첫 면** |
 * | 뒤로 밀어서 | 아직 뒤 메뉴 | **끝 면** |
 *
 * 미는 경우에 가장자리 면에 세우는 이유는 **되돌릴 수 있어야** 하기 때문이다. 기억한 면에 세우면
 * 통화›«통화» 에서 뒤로 밀어 무전에 갔다가 다시 앞으로 밀었을 때 원래 자리로 돌아오지 못한다.
 */
@Composable
private fun MenuPage(
    menu: AppScreen,
    settled: Boolean,
    screen: AppScreen,
    wantPane: Int,
    onPage: (AppPage) -> Unit,
    tabs: @Composable (AppPage) -> Unit,
    content: @Composable ColumnScope.(AppPage) -> Unit,
) {
    val initial = if (menu == screen) wantPane else entryPane(menu, from = screen)
    // **저장·복원하지 않는다**(`rememberPagerState` 가 아니라 `remember`). 이 장은 화면 밖으로 나가면 버려지는데,
    //   pager 의 저장 상태가 되살아나면 위 진입 규칙(`initial`)이 무시되고 **떠날 때의 면**에 선다. 그러면 면을 정해서
    //   부른 이동([채널로 이동] — «채널» 면)이, 정착하자마자 아래 알림이 그 옛 면을 VM 에 되돌려 써 덮인다.
    //   면의 기억은 VM(`pttPane`·`callPane`)이 갖는다 — 여기가 한 벌 더 가질 이유가 없다.
    val inner = remember { PagerState(currentPage = initial) { menu.paneCount } }

    // 탭·내비·사람 메뉴·[채널로 이동] 이 면을 **바꿨을 때만** 따라간다 — 규칙은 [PaneRequest] 가 갖는다.
    //   정착을 기다려 따라가고, 닿든·손가락이 끊든·새 요청이 대신하든 **그 요청을 내려놓는다**(`finally`) — 끊긴 채
    //   남으면 아래 알림이 영영 막혀 면을 넘겨도 VM 좌표가 멈춘다.
    val request = remember { PaneRequest() }
    LaunchedEffect(wantPane) { request.onWant(wantPane) }
    LaunchedEffect(request.pending, settled) {
        val want = request.pending ?: return@LaunchedEffect
        if (!settled) return@LaunchedEffect
        try { inner.goTo(want) } finally { request.finish(want) }
    }

    // **자리가 잡힌 장의, 자리가 잡힌 면만** 좌표를 알린다 — 요청을 든 동안은 알리지 않는다(위).
    //
    //   · 바깥이 아직 가는 중이면(`settled` 거짓) 미리 그려 둔 옆 장이 보지도 않은 면으로 상태를 끈다.
    //   · 안쪽이 가는 중이면 지나가는 면이 곧 `wantPane` 이 되어 위 효과가 다시 시작되고, 가던
    //     애니메이션이 취소돼 중간에 선다 — 탭을 두 칸 이상 건너뛸 때의 그 증상이다.
    val innerSettled = inner.currentPage == inner.targetPage
    LaunchedEffect(settled, innerSettled, inner.currentPage, request.pending) {
        if (settled && innerSettled && request.canReport) onPage(AppPage(menu, inner.currentPage))
    }

    Column(Modifier.fillMaxSize()) {
        // 강조는 **정착할 면**(`targetPage`)을 가리킨다 — `currentPage` 는 절반을 넘겨야 바뀌어서
        //   미는 내내 옛 탭이 켜져 있다가 툭 튄다.
        if (menu.paneCount > 1) tabs(AppPage(menu, inner.targetPage))

        // **끝 면에서 늘어나지 않게 한다.** 가장자리의 overscroll(늘어나는 효과)이 끌기를 먹어 버리면
        //   바깥 pager 로 넘어가는 몫이 줄어 «메뉴 넘기기만 유난히 뻑뻑한» 느낌이 된다. 여기서는 늘어나는
        //   대신 그대로 옆 메뉴로 넘겨준다 — 양 끝(이력·더보기)의 늘어남은 바깥 pager 가 여전히 보여 준다.
        //
        //   끄는 범위는 **pager 자신뿐**이다. 본문 안쪽(세로 목록 등)에는 원래 효과를 도로 넣어 준다 —
        //   통째로 끄면 목록을 끝까지 내렸을 때의 반응이 같이 사라진다.
        val overscroll = LocalOverscrollFactory.current
        CompositionLocalProvider(LocalOverscrollFactory provides null) {
            HorizontalPager(state = inner, modifier = Modifier.weight(1f)) { pi ->
                CompositionLocalProvider(LocalOverscrollFactory provides overscroll) {
                    Column(Modifier.fillMaxSize()) { content(AppPage(menu, pi)) }
                }
            }
        }
    }
}

/**
 * 면 이동 요청의 수명 — VM 이 바꾼 면(요청)을 **닿거나, 손가락이 끊거나, 새 요청이 대신할 때까지** 든다.
 *
 *   · 이 장이 선 뒤의 **첫 값은 요청이 아니다** — 그때의 면은 진입 규칙이 정했다. 요청으로 보면 밀어서 막 들어온 장이
 *     옛 기억값으로 튕겨 나간다(밀어서 바뀐 것은 좌표 알림이 정본이다).
 *   · 요청을 든 동안은 좌표를 알리지 않는다([canReport]) — 바깥이 가는 중에 온 요청이, 정착하는 순간 옛 면의 알림에
 *     덮이지 않게.
 *   · 이동이 끝나면(닿음·손가락이 끊음) 그 요청을 내려놓는다([finish]). 끊긴 뒤에는 **손가락이 멈춘 면**을 따른다.
 *     새 요청이 이미 대신했으면 옛 끝맺음은 새 요청을 건드리지 않는다.
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
 * 한 칸이면 미끄러지듯, 멀면 곧바로.
 *
 * 멀리 갈 때 `animateScrollToPage` 로 쓸고 가면 두 가지가 나빠진다 — 지나치는 장이 한 번씩 그려졌다
 * 사라져 느리고, 무엇보다 **그 장들이 자기를 «지금 화면» 으로 알린다.** 내비에서 [이력] → [더보기] 를
 * 누르면 [무전]·[통화] 를 거치는데, 거치는 순간 상태가 그쪽으로 바뀌어 이동이 거기서 멈춘다.
 * 내비·사람 메뉴처럼 **한 번에 닿아야 하는** 이동은 중간을 거치지 않는다.
 */
private suspend fun PagerState.goTo(page: Int) {
    val to = page.coerceIn(0, (pageCount - 1).coerceAtLeast(0))
    if (to == currentPage) return
    if (abs(to - currentPage) == 1) animateScrollToPage(to) else scrollToPage(to)
}

/**
 * 메뉴가 바뀌는 문턱 — 한 장의 이 비율만큼 끌면 넘어간다(기본값은 0.5).
 *
 * 면을 넘길 때는 속도만으로도 넘어가는데 메뉴는 거리로만 판정되므로, 손끝의 느낌을 맞추려면 이쪽을
 * 낮춰야 한다. 면 끝에서 더 미는 동작은 «옆 메뉴로 가겠다» 말고 다른 뜻이 없어 낮춰도 오조작이 아니다.
 */
private const val MENU_SNAP_THRESHOLD = 0.15f
