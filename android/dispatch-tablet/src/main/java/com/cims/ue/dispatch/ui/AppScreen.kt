// 최상위 화면 — 하단 내비 다섯 (docs/design/features/android_dispatch_tablet.md §6.3)
//
// **모바일 앱으로 짠다.** 데스크톱은 1920×1080 한 장에 6패널을 동시에 편다(dispatch_desktop_ui.md §3.1).
// 태블릿 본문(608dp)은 그 38% 밖에 안 되므로 같은 격자를 줄여 넣으면 어느 칸도 제 몫을 못 한다 — 실제로
// 채널 카드가 2.7장, 메시지가 6줄만 보였다. 그래서 **한 화면은 한 가지 일만** 하고, 나머지는
// 하단 내비와 좌우 스와이프로 **이동해서** 본다. 화면을 쪼개지 않는다.
// 예외는 발언 바 하나 — 어느 화면에서나 무전할 수 있어야 하므로 내비 위에 상시로 둔다.
//
// 데스크톱 최상위 메뉴 넷(§3.4 관제·이력·PTT 그룹·관리)은 «관리 축» 으로 묶여 있는데, 태블릿에서는
// 관제사가 **하는 일** 로 묶는 편이 손이 덜 간다. 이력·PTT 그룹·관리는 하루에 몇 번 여는 화면이라
// [더보기] 뒤로 보내고, 늘 쓰는 무전·통화·메시지·감청을 앞으로 낸다. 화면 «내용» 은 그대로다.
package com.cims.ue.dispatch.ui

/** 최상위 화면. 순서가 곧 하단 내비의 배열이다. */
enum class AppScreen(val label: String, val hotkey: String) {
    PTT("무전", "F1"),
    CALLS("통화", "F2"),
    MESSAGES("메시지", "F3"),
    MONITOR("감청", "F4"),
    MORE("더보기", "F5");

    companion object {
        /** 하드 키보드가 있으면 F1~F5 도 받는다(§7). */
        fun ofFunctionKey(keyCode: Int): AppScreen? = when (keyCode) {
            android.view.KeyEvent.KEYCODE_F1 -> PTT
            android.view.KeyEvent.KEYCODE_F2 -> CALLS
            android.view.KeyEvent.KEYCODE_F3 -> MESSAGES
            android.view.KeyEvent.KEYCODE_F4 -> MONITOR
            android.view.KeyEvent.KEYCODE_F5 -> MORE
            else -> null
        }
    }
}

/**
 * [더보기] 안에서 여는 화면 — 하단 내비 항목이 **아니라** 그 안의 이동이다(뒤로가기로 목록에 돌아온다).
 *
 * 데스크톱에서는 최상위 메뉴 F2~F4 인 것들이다(§3.4). 태블릿에서 뒤로 보내는 근거는 «자주 쓰는가» 하나다 —
 * 조회·편성·관리는 상황이 생겼을 때 여는 화면이고, 무전·통화는 상시다.
 */
enum class MoreItem(val label: String, val hint: String) {
    HISTORY("이력", "끝난 통화·PTT 세션 조회와 녹취 재생"),
    PTT_GROUPS("PTT 그룹", "범위 안 그룹 보기·생성·편집"),
    ADMIN("관리", "조직·구성원·번호"),
}
