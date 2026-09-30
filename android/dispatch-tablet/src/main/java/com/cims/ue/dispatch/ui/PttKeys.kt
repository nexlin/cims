// 측면 PTT 키를 시트·대화상자에서도 받는다 (docs/design/features/android_dispatch_tablet.md §7)
package com.cims.ue.dispatch.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.platform.LocalView
import androidx.core.view.ViewCompat
import com.cims.ue.dispatch.session.DispatchService

/**
 * 이 창(시트·대화상자)도 측면 PTT 키를 넘긴다 — **모달이 발언을 막으면 안 된다**(데스크톱 전역 핫키는 대화상자가
 * 떠 있어도 먹는다). 시트·대화상자는 Activity 와 다른 창이라 [MainActivity.dispatchKeyEvent] 로 키가 오지 않는다.
 * 앱의 모든 시트·대화상자 내용에서 한 번 부른다.
 *
 * 창의 뷰가 쓰지 않은 키만 받는다(unhandled key listener) — 누름을 받았으면 그 뗌도 같은 곳으로 온다.
 */
@Composable
fun ForwardPttKeys() {
    val view = LocalView.current
    DisposableEffect(view) {
        val root = view.rootView
        val listener = ViewCompat.OnUnhandledKeyEventListenerCompat { _, e ->
            DispatchService.session?.hwPtt?.onKeyEvent(e) == true
        }
        ViewCompat.addOnUnhandledKeyEventListener(root, listener)
        onDispose { ViewCompat.removeOnUnhandledKeyEventListener(root, listener) }
    }
}
