// 최상위 화면 — 하단 내비 넷 (docs/design/features/android_dispatch_tablet.md §6.2, dispatch_desktop_ui.md §3.4)
//
// 데스크톱의 상단 바 메뉴 4개(F1~F4)를 태블릿에서는 하단 내비로 낸다. 별창은 없고 같은 창의
// 레이어 전환이며, 화면 상태는 앱 수명 동안 유지한다(폼 입력이 탭 전환으로 날아가지 않게).
package com.cims.ue.dispatch.ui

/** 최상위 화면. 순서가 곧 하단 내비의 배열이다. */
enum class AppScreen(val label: String, val hotkey: String) {
    DISPATCH("관제", "F1"),
    HISTORY("이력", "F2"),
    PTT_GROUPS("PTT 그룹", "F3"),
    ADMIN("관리", "F4");

    /** 관제 밖 화면에는 상단에 관제 요약 띠가 붙는다(§6.2). */
    val showsSummaryStrip: Boolean get() = this != DISPATCH

    companion object {
        /** 하드 키보드가 있으면 F1~F4 도 받는다(§7). */
        fun ofFunctionKey(keyCode: Int): AppScreen? = when (keyCode) {
            android.view.KeyEvent.KEYCODE_F1 -> DISPATCH
            android.view.KeyEvent.KEYCODE_F2 -> HISTORY
            android.view.KeyEvent.KEYCODE_F3 -> PTT_GROUPS
            android.view.KeyEvent.KEYCODE_F4 -> ADMIN
            else -> null
        }
    }
}

/** 관제 캔버스의 탭 둘 — 6패널을 접은 결과(§6.3). */
enum class DispatchTab(val label: String) {
    PTT("PTT"),
    CALLS("일반통화"),
}
