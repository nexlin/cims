// 최상위 화면 — 왼쪽 레일 + 관제의 면 + 오른쪽 사이드 패널 (docs/design/features/android_dispatch_tablet.md §6.3)
//
// **모바일 앱으로 짠다.** 데스크톱은 1920×1080 에 모드마다 한 화면을 편다(dispatch_desktop_ui.md §3.1). 태블릿 본문은
// 그 절반도 안 되므로 같은 격자를 줄여 넣으면 어느 칸도 제 몫을 못 한다. 그래서 **한 면은 한 가지 일만** 하고 나머지는
// 탭과 좌우 스와이프로 **이동해서** 본다. 예외는 둘 — 어디서나 무전할 수 있어야 하는 발언 바(아래 상시)와, 보던 면을
// 떠나지 않고 한 대상을 자세히 보는 **오른쪽 사이드 패널**(본문 위에 겹친다, 400dp)이다.
//
// **레일은 데스크톱과 같다 — 관제 · 이력 · PTT 그룹 · 관리 + 바닥 [설정].** 전부 한 번에 누른다(메뉴를 거치지 않는다).
// 무전과 통화는 둘 다 «관제사가 지금 거는 일» 이라 [관제] 하나로 묶고, 그 안을 [무전|통화] 세그먼트와 하위 탭 한 줄로 나눈다.
// 탭을 두 줄로 쌓으면 세로(가로 전용 화면에서 가장 모자란 자원)를 한 줄 더 쓰므로, 메뉴는 아래가 아니라 **왼쪽 세로
// 레일**(폭 80)에 둔다 — 그 80dp 가 본문 높이로 간다. [설정] 은 화면이 아니라 시트를 연다(켜짐 표시 없음).
//
// **가로 스와이프는 관제의 면 여섯 장을 한 줄로 꿴다**([DISPATCH_PAGES]) — 무전(채널·메시지·이벤트) 다음에 통화(통화·
// 메시지·통화내역). 끝 면에서 더 밀면 다른 모드로 넘어간다. 메뉴(레일)는 밀어서 바꾸지 않는다 — 세로 레일을
// 가로 손짓으로 넘기는 것은 예측할 수 없는 동작이다. 패널은 가장자리 스와이프로 열지 않는다(면 넘기기와 겹친다).
//
// [통화] 는 왼쪽에 **고정 칸**(대표번호 대기열·진행 중·내 통화·관제 그룹원)을 둔다 — 면이 아니다. 통화 면을 오가도 제자리에
// 남아 지금 벌어지는 통화를 놓치지 않는다. 주소록도 면이 아니라 탭 줄 [주소록] 이 여는 오른쪽 패널이다.
package com.cims.ue.dispatch.ui

/**
 * 왼쪽 레일의 화면. 순서가 곧 레일의 배열이다(데스크톱 F1~F4 — dispatch_desktop_ui.md §3.4). 첫 화면은 [관제] — 관제사가
 * 가장 오래 머무는 곳이다. [설정] 은 여기 없다 — 레일 바닥의 버튼이 시트를 연다.
 */
enum class AppScreen(val label: String) {
    DISPATCH("관제"),
    HISTORY("이력"),
    /** 범위 안 PTT 그룹 보기·편집·삭제(§6.12). 새 그룹은 [무전] «채널» 의 [채널 추가하기] 패널에서 만든다. */
    PTT_GROUPS("PTT 그룹"),
    /** 조직·구성원·번호(§6.13) — 관리 범위(`dispatch.directoryWrite`)가 없으면 레일에서 흐리다. */
    ADMIN("관리"),
}

/** [관제] 의 두 모드 — 탭 줄 왼쪽의 [무전|통화] 세그먼트. */
enum class DispatchMode(val label: String) { PTT("무전"), CALL("통화") }

/**
 * [무전] 의 면. **«청취» 면은 두지 않는다** — 청취 중인 채널은 «채널» 면의 타 채널 목록에 그렇게 적혀 있고 거기서 끄고
 * 켠다. 면을 따로 두면 같은 채널을 두 군데서 보게 된다.
 */
enum class PttPane(val label: String) {
    CHANNELS("채널"), MESSAGES("메시지"), EVENTS("이벤트"),
}

/**
 * [통화] 의 면 — «통화»(다이얼패드)·«메시지»(문자)·«통화내역». 대표번호 대기열·진행 중·내 통화·관제 그룹원은 면이 아니라 모든
 * 통화 면의 **왼쪽 고정 칸**이고, 주소록은 탭 줄 [주소록] 이 여는 오른쪽 패널이다([SidePanel.Book]). «감청» 자리는 없다 —
 * 감청은 통화에 붙는 leg 이므로 고정 칸의 **그 통화 행에서** 켜고 끈다. «통화내역» 은 끝난 것만 담는다(날짜로 보는 과거
 * 조회·녹취는 최상위 [이력] 이다).
 */
enum class CallPane(val label: String) {
    CALLS("통화"), MESSAGES("메시지"), LOG("통화내역"),
}

/**
 * 관제의 면 한 장 — **모드와 그 안의 면을 합친 좌표**다. 두 축으로 두면 스와이프가 어느 축을 움직이는지 정할 수 없다 —
 * 한 줄로 펴면 «옆으로 밀면 옆 면» 하나로 끝나고, 모드의 끝에서 밀면 다른 모드로 넘어가는 것도 같은 규칙이다.
 */
data class DispatchPage(val mode: DispatchMode, val pane: Int = 0) {
    val pttPane: PttPane? get() = if (mode == DispatchMode.PTT) PttPane.entries[pane] else null
    val callPane: CallPane? get() = if (mode == DispatchMode.CALL) CallPane.entries[pane] else null
    /** [DISPATCH_PAGES] 안의 차례. */
    val index: Int get() = DISPATCH_PAGES.indexOf(this)
}

/** **닿는 차례** — 무전(채널·메시지·이벤트) · 통화(통화·메시지·통화내역). 순서는 두 enum 의 순서를 그대로 잇는다. */
val DISPATCH_PAGES: List<DispatchPage> =
    PttPane.entries.map { DispatchPage(DispatchMode.PTT, it.ordinal) } +
        CallPane.entries.map { DispatchPage(DispatchMode.CALL, it.ordinal) }

fun pageOf(p: PttPane) = DispatchPage(DispatchMode.PTT, p.ordinal)
fun pageOf(p: CallPane) = DispatchPage(DispatchMode.CALL, p.ordinal)

/**
 * 오른쪽 사이드 패널의 내용 — **한 번에 하나**, 쌓지 않는다. 같은 대상을 다시 누르면 닫고, 다른 대상이면 내용을 바꾼다.
 * 패널 **안에서** 들어간 것만 ← 로 돌아간다([parent]).
 */
sealed interface SidePanel {
    /** 채널 상세 — 채널 카드·타 채널 행·메시지 [채널 정보]·배너 [채널로 이동]·검색. id 는 채널 카드 id. */
    data class Channel(val id: String) : SidePanel
    /**
     * 채널 추가 — 탭 줄 [사용자]·«채널» 면의 [채널 추가하기] 타일. 사람을 골라 **개별 통화**(1명)·**애드혹 통화**(여럿)를 걸거나
     * **그룹을 추가**한다(그룹 추가는 한 겹 들어간 [NewGroup]). 셋 다 내 채널에 카드 한 장을 더하는 일이라 한 자리에서 한다.
     */
    data object AddChannel : SidePanel
    /** 새 PTT 그룹 — 채널 추가에서 한 겹 들어온 것(← 가 채널 추가로 돌아간다). */
    data object NewGroup : SidePanel
    /** 주소록 — [통화] 탭 줄의 [주소록]. 어느 통화 면에서든 열어 바로 걸거나 문자를 보낸다. */
    data object Book : SidePanel
    /** 이벤트 상세 — «이벤트» 면의 행. id 는 이벤트 행 id. */
    data class Event(val id: Long) : SidePanel

    /** 패널 안에서 한 겹 들어온 것이면 돌아갈 곳, 아니면 null. */
    val parent: SidePanel? get() = if (this == NewGroup) AddChannel else null
}

// ── 이동 규칙 ────────────────────────────────────────────────────────────────

/**
 * 화면 좌표 하나 — 이동 규칙이 다루는 전부.
 *
 * 규칙을 [MainViewModel] 안이 아니라 **값 위의 순수 함수**로 두는 이유: 뒤로가기는 «가로챌 것인가» 와 «무엇을 되돌릴
 * 것인가» 두 곳에서 같은 판정을 해야 하는데, 손으로 두 번 적으면 반드시 어긋난다. 어긋나면 뒤로가기를 먹고도 화면이
 * 그대로여서 관제사는 앱이 멈춘 줄 안다. 함수 하나를 양쪽이 쓴다.
 */
data class NavState(
    val screen: AppScreen = AppScreen.DISPATCH,
    val mode: DispatchMode = DispatchMode.PTT,
    /** 모드마다 보던 면 — 모드를 오가도 남는다. */
    val pttPane: PttPane = PttPane.CHANNELS,
    val callPane: CallPane = CallPane.CALLS,
    /** 오른쪽 사이드 패널. null = 닫힘. [관제] 에서만 그린다. */
    val panel: SidePanel? = null,
    /** 패널 고정 — 고정하면 탭·모드를 옮겨도 남고, 아니면 옮길 때 닫힌다. 패널이 닫히면 풀린다. */
    val pinned: Boolean = false,
) {
    /** 지금 [관제] 가 보여 주는 면. */
    val page: DispatchPage get() = when (mode) {
        DispatchMode.PTT -> pageOf(pttPane)
        DispatchMode.CALL -> pageOf(callPane)
    }
}

/**
 * 레일을 눌렀다. [관제] 를 다시 누르면 패널을 닫는다(모바일 관례). 다른 화면으로 옮기면 고정하지 않은 패널은 닫힌다
 * (탭을 옮길 때와 같다).
 */
fun NavState.onNav(target: AppScreen): NavState = when {
    screen != target -> copy(screen = target).let { if (it.pinned) it else it.copy(panel = null) }
    target == AppScreen.DISPATCH && panel != null -> copy(panel = null, pinned = false)
    else -> this
}

/** 탭·스와이프로 면을 옮겼다 — 고정하지 않은 패널은 닫힌다. 같은 면이면 그대로다. */
fun NavState.toPage(page: DispatchPage): NavState {
    val moved = screen != AppScreen.DISPATCH || page != this.page
    val next = copy(screen = AppScreen.DISPATCH, mode = page.mode,
        pttPane = page.pttPane ?: pttPane, callPane = page.callPane ?: callPane)
    return if (moved && !pinned) next.copy(panel = null) else next
}

/** [무전|통화] 세그먼트 — 그 모드에서 보던 면으로 간다. */
fun NavState.toMode(mode: DispatchMode): NavState =
    toPage(if (mode == DispatchMode.PTT) pageOf(pttPane) else pageOf(callPane))

/** 패널을 연다 — 같은 대상이면 닫고(토글), 다른 대상이면 바꾼다. 면은 옮기지 않는다(보던 면 위에 선다). */
fun NavState.togglePanel(p: SidePanel): NavState =
    if (panel == p) copy(panel = null, pinned = false) else copy(screen = AppScreen.DISPATCH, panel = p)

/** 패널에 그 대상을 세운다 — 토글하지 않는다(메시지 [채널 정보]·패널 안의 이동). */
fun NavState.showPanel(p: SidePanel): NavState = copy(screen = AppScreen.DISPATCH, panel = p)

/**
 * 채널을 **찾아가서** 연다 — 긴급 배너 [채널로 이동]·검색·[PTT 그룹] «채널로». [무전] › «채널» 면에 그 채널 상세가 선다.
 * 면을 같이 옮기지 않으면 «메시지»·«이벤트» 를 보고 있었을 때 누른 것이 목록에서 보이지 않는다.
 */
fun NavState.openChannel(id: String): NavState =
    copy(screen = AppScreen.DISPATCH, mode = DispatchMode.PTT, pttPane = PttPane.CHANNELS, panel = SidePanel.Channel(id))

fun NavState.closePanel(): NavState = copy(panel = null, pinned = false)

fun NavState.togglePin(): NavState = if (panel == null) this else copy(pinned = !pinned)

/**
 * 뒤로가기 한 겹 — 되돌릴 것이 없으면 null(그때는 가로채지 않는다).
 *
 * 순서는 연 순서의 역순이다: ① 패널 안의 한 겹 → ② 패널 → ③ 그 모드의 첫 면 → ④ [무전] → ⑤ [관제].
 * **보고 있는 화면의 것만** 되돌린다 — 보이지 않는 패널을 닫으면 화면은 그대로인데 뒤로가기만 한 번 먹힌다.
 * 돌려주는 값은 **반드시 지금과 다르다** — 같으면 «되돌릴 것이 없다» 는 뜻이므로 null 이다.
 */
fun NavState.onBack(): NavState? {
    val p = panel
    return when {
        screen == AppScreen.DISPATCH && p?.parent != null -> copy(panel = p.parent)
        screen == AppScreen.DISPATCH && p != null -> closePanel()
        screen == AppScreen.DISPATCH && mode == DispatchMode.CALL && callPane != CallPane.CALLS -> copy(callPane = CallPane.CALLS)
        screen == AppScreen.DISPATCH && mode == DispatchMode.CALL -> copy(mode = DispatchMode.PTT)
        screen == AppScreen.DISPATCH && pttPane != PttPane.CHANNELS -> copy(pttPane = PttPane.CHANNELS)
        screen != AppScreen.DISPATCH -> copy(screen = AppScreen.DISPATCH)
        else -> null
    }
}
