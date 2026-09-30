// 최상위 화면 — 하단 내비 넷 (docs/design/features/android_dispatch_tablet.md §6.3)
//
// **모바일 앱으로 짠다.** 데스크톱은 1920×1080 한 장에 6패널을 동시에 편다(dispatch_desktop_ui.md §3.1).
// 태블릿 본문(608dp)은 그 38% 밖에 안 되므로 같은 격자를 줄여 넣으면 어느 칸도 제 몫을 못 한다 — 실제로
// 채널 카드가 2.7장, 메시지가 6줄만 보였다. 그래서 **한 화면은 한 가지 일만** 하고, 나머지는
// 하단 내비와 좌우 스와이프로 **이동해서** 본다. 화면을 쪼개지 않는다.
// 예외는 발언 바 하나 — 어느 화면에서나 무전할 수 있어야 하므로 내비 위에 상시로 둔다.
//
// **축은 넷뿐이다.** 관제사가 하는 일로 묶되, 한 일이 두 군데 나오지 않게 한다 —
// 감청(통화 leg)은 [통화] 안에, 청취(무전 leg)와 SDS 는 [무전] 안에 둔다. 따로 축을 세우면
// «내가 듣고 있던 게 어디 있더라» 를 두 곳에서 찾게 된다.
//
// **가로 스와이프는 면 전체를 한 줄로 꿴다**([APP_PAGES]). 메뉴와 면을 두 축으로 두면 한 번 쓸었을 때
// 어느 축이 움직일지 예측할 수 없는데, **평평하게 한 줄로 펴면 축이 하나뿐이라** 그 모호함이 사라진다 —
// 끝 면에서 계속 밀면 옆 메뉴의 첫 면으로 넘어간다(통화›«통화내역» → [더보기]).
// 탭과 하단 내비는 그 줄의 **한 지점으로 건너뛰는** 지름길이다.
package com.cims.ue.dispatch.ui

/**
 * 최상위 화면. 순서가 곧 하단 내비의 배열이고 **좌우 스와이프 순서**다.
 *
 * **[이력]이 첫 화면**인 이유 — 관제에서 «무슨 일이 있었나» 를 보는 일이 가장 잦다. 무전·통화는 상황이
 * 생겼을 때 들어가는 곳이고, 그때는 발언 바가 상시로 있어 어디서든 무전할 수 있다.
 *
 * **감청·메시지는 최상위에 두지 않는다** — 둘 다 «무전의 일» 이거나 «통화의 일» 이지 그 자체가 축이 아니다.
 * 감청은 통화 leg, 청취는 무전 leg 이고, SDS 는 무전 채널의 대화다. 축을 따로 세우면 같은 것을 두 군데서
 * 찾게 된다(§6.3).
 */
enum class AppScreen(val label: String) {
    HISTORY("이력"),
    PTT("무전"),
    CALLS("통화"),
    MORE("더보기");

    /** 이 메뉴가 가진 면의 수 — 평평한 차례([APP_PAGES])를 세우는 데 쓴다. */
    val paneCount: Int
        get() = when (this) {
            HISTORY -> 1
            PTT -> PttPane.entries.size
            CALLS -> CallPane.entries.size
            MORE -> 1
        }

}

/**
 * [더보기] 안에서 여는 화면 — 하단 내비 항목이 **아니라** 그 안의 이동이다(뒤로가기로 목록에 돌아온다).
 *
 * 편성·관리는 상황이 생겼을 때 여는 화면이라 한 겹 뒤에 둔다. 이력은 자주 보므로 최상위로 올렸다.
 */
enum class MoreItem(val label: String, val hint: String) {
    PTT_GROUPS("PTT 그룹", "범위 안 그룹 보기·생성·편집"),
    ADMIN("관리", "조직·구성원·번호"),
}

/**
 * [무전] 메뉴 안의 면 — 탭으로 건너뛰거나 좌우로 민다([APP_PAGES], §6.3).
 *
 * **«청취» 면은 두지 않는다.** 청취 중인 채널은 «채널» 면의 범위 채널 목록에 이미 그렇게 적혀 있고
 * 거기서 끄고 켠다 — 면을 따로 두면 같은 채널을 두 군데서 보게 되고, 어느 쪽이 최신인지 헷갈린다.
 */
enum class PttPane(val label: String) {
    CHANNELS("채널"), MESSAGES("메시지"), EVENTS("이벤트"),
}

/**
 * [통화] 메뉴 안의 면 — 탭으로 건너뛰거나 좌우로 민다([APP_PAGES], §6.3).
 *
 * **«감청» 을 위한 자리는 없다.** 감청은 통화에 붙는 leg 이므로 **그 통화 행에서** 켜고 끄고, 켜져 있으면
 * 그 행이 «청취 중» 이라고 말한다. 따로 구역을 두면 같은 통화가 두 군데 나오고 어느 쪽이 최신인지 흐려진다.
 *
 * **«진행 중» 은 «통화» 면에 있다** — 지금 벌어지는 일이기 때문이다. «통화내역» 은 끝난 것만 담는다.
 * (최상위 [이력] 은 **다른 것**이다 — 날짜를 골라 보는 과거 조회이고 녹취 재생이 거기 있다.)
 *
 * **«그룹원» 은 면이 아니다.** 그룹원은 «거는 상대» 이자 «상태를 곁눈질하는 대상» 이라 «통화» 면의 띠로
 * 족하다(데스크톱 ③ 의 그룹원 띠, dispatch_desktop_ui.md §4.4). 거는 일은 «주소록» 면이 받는다.
 */
enum class CallPane(val label: String) {
    CALLS("통화"), BOOK("주소록"), MESSAGES("메시지"), LOG("통화내역"),
}

/**
 * 좌우 스와이프의 한 장 — **메뉴와 그 안의 면을 합친 좌표**다.
 *
 * 메뉴 축과 면 축을 따로 두면 스와이프가 어느 축을 움직이는지 정할 수 없다. 둘을 한 줄로 펴면
 * 축이 하나라 «옆으로 밀면 옆 면» 하나로 끝난다 — 면의 끝에서 밀면 옆 메뉴로 넘어가는 것도 같은 규칙이다.
 *
 * @param pane 메뉴 안의 면 번호(면이 하나뿐인 메뉴는 0).
 */
data class AppPage(val screen: AppScreen, val pane: Int = 0) {
    val pttPane: PttPane? get() = if (screen == AppScreen.PTT) PttPane.entries[pane] else null
    val callPane: CallPane? get() = if (screen == AppScreen.CALLS) CallPane.entries[pane] else null
}

/**
 * **닿는 차례** — 이력 · 무전(채널·메시지·이벤트) · 통화(통화·주소록·메시지·통화내역) · 더보기.
 *
 * 화면이 이 목록을 pager 로 펴 놓는 것은 **아니다**(펴면 면을 밀 때 탭줄까지 미끄러진다, §6.3).
 * 실제로는 pager 둘이 겹쳐 있고, 안쪽이 끝에 닿으면 바깥으로 넘어가면서 **결과적으로** 이 차례가 된다.
 * 그 «결과» 를 한 곳에 적어 두는 것이 이 값이고, [entryPane] 이 이음매를 만든다.
 *
 * 순서는 [AppScreen] 과 각 메뉴의 면 순서를 그대로 이은 것이다 — 따로 적지 않는다(두 곳에 적으면
 * 면을 하나 늘렸을 때 한쪽만 고치게 된다).
 */
val APP_PAGES: List<AppPage> =
    AppScreen.entries.flatMap { s -> List(s.paneCount) { AppPage(s, it) } }

/** 지금 보고 있는 면의 번호 — 면이 하나인 메뉴는 0. */
fun paneIndexOf(menu: AppScreen, pttPane: PttPane, callPane: CallPane): Int = when (menu) {
    AppScreen.PTT -> pttPane.ordinal
    AppScreen.CALLS -> callPane.ordinal
    else -> 0
}

/**
 * 밀어서 [menu] 로 들어올 때 **서는 면** — 앞으로 들어오면 첫 면, 뒤로 들어오면 끝 면.
 *
 * 이래야 [APP_PAGES] 의 차례가 이어지고, 무엇보다 **되돌릴 수 있다**: 통화›«통화» 에서 뒤로 밀면
 * 무전›«이벤트» 에 서고 거기서 앞으로 밀면 통화›«통화» 로 돌아온다. 기억한 면에 세우면 왕복이 깨진다.
 *
 * @param from 밀기 전에 보고 있던 메뉴.
 */
fun entryPane(menu: AppScreen, from: AppScreen): Int =
    if (menu.ordinal > from.ordinal) 0 else menu.paneCount - 1

// ── 이동 규칙 ────────────────────────────────────────────────────────────────

/**
 * 화면 좌표 하나 — 이동 규칙이 다루는 전부.
 *
 * 규칙을 [MainViewModel] 안이 아니라 **값 위의 순수 함수**로 두는 이유: 뒤로가기는 «가로챌 것인가»
 * 와 «무엇을 되돌릴 것인가» 두 곳에서 같은 판정을 해야 하는데, 손으로 두 번 적으면 반드시 어긋난다.
 * 어긋나면 뒤로가기를 먹고도 화면이 그대로여서 관제사는 앱이 멈춘 줄 안다. 함수 하나를 양쪽이 쓴다.
 */
data class NavState(
    val screen: AppScreen,
    /** [무전] 이 열어 둔 채널 상세 — 메뉴를 옮겨도 기억된다. */
    val channel: String? = null,
    /** [더보기] 가 열어 둔 안쪽 화면. */
    val more: MoreItem? = null,
    val pttPane: PttPane = PttPane.CHANNELS,
    val callPane: CallPane = CallPane.CALLS,
)

/**
 * 하단 내비를 눌렀다 — **두 칸 이상 떨어져 있어도 한 번에** 그 메뉴로 간다.
 *
 * 같은 항목을 다시 누르면 **그 메뉴의** 안쪽 화면만 닫는다(모바일 관례). 다른 메뉴가 기억해 둔
 * 안쪽 화면은 건드리지 않는다 — 그쪽으로 돌아가면 보던 자리가 있어야 한다.
 */
fun NavState.onNav(target: AppScreen): NavState = when {
    screen != target -> copy(screen = target)
    target == AppScreen.PTT -> copy(channel = null)
    target == AppScreen.MORE -> copy(more = null)
    else -> this
}

/**
 * 채널을 연다 — 어디서 불렀든(목록 행·긴급 배너·검색·[PTT 그룹]) **[무전] 의 «채널» 면**에 그 채널 화면이 선다.
 *
 * 채널 화면은 «채널» 면에만 그려진다(§6.3a). 면을 같이 옮기지 않으면 «메시지»·«이벤트» 를 보고 있었거나 마지막으로
 * 그 면을 봤을 때 연 채널이 **보이지 않는다** — 누른 것이 아무 일도 안 한 것처럼 된다.
 */
fun NavState.openChannel(id: String): NavState =
    copy(screen = AppScreen.PTT, channel = id, pttPane = PttPane.CHANNELS)

/**
 * 뒤로가기 한 겹 — 되돌릴 것이 없으면 null(그때는 가로채지 않는다).
 *
 * 순서는 연 순서의 역순이다: ① **보고 있는 메뉴의** 안쪽 화면 → ② 그 메뉴의 첫 면 → ③ 첫 화면.
 *
 * ①이 «보고 있는 메뉴» 로 한정되는 것이 요점이다. 안쪽 화면은 메뉴마다 기억되므로, 다른 메뉴에서
 * 그걸 닫으면 **화면은 그대로인데 뒤로가기만 한 번 먹힌다.**
 *
 * 돌려주는 값은 **반드시 지금과 다르다** — 같으면 «되돌릴 것이 없다» 는 뜻이므로 null 이다.
 */
fun NavState.onBack(): NavState? = when {
    screen == AppScreen.PTT && channel != null -> copy(channel = null)
    screen == AppScreen.MORE && more != null -> copy(more = null)
    screen == AppScreen.PTT && pttPane != PttPane.CHANNELS -> copy(pttPane = PttPane.CHANNELS)
    screen == AppScreen.CALLS && callPane != CallPane.CALLS -> copy(callPane = CallPane.CALLS)
    screen != AppScreen.HISTORY -> copy(screen = AppScreen.HISTORY)
    else -> null
}
