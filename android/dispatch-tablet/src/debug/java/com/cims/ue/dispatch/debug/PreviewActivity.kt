// 기기 미리보기 — **debug 빌드에만** 있는 진입점 (android_dispatch_tablet.md §9)
//
// 데스크톱의 `--ui-preview-canvas` 에 해당한다. 로그인·서버·엔진 없이 표본 자료로 화면을 그려, 실제 기기의 밀도·색·손짓(면 넘기기·
// 패널 밀기·패널 폭 끌기)을 본다. release APK 에는 이 Activity 가 없다(debug 소스 셋).
//
//   adb shell am start -n com.cims.ue.dispatch/.debug.PreviewActivity                         움직이는 미리보기(레일·탭·패널이 동작)
//   … --es name channel-panel                                                                 고정 한 장(ui/AppPreview.kt `DEVICE_PREVIEWS` 의 이름)
//   … --ez dark true                                                                          어두운 테마(움직이는 미리보기 — 레일 [설정] 으로도 바꾼다)
//   … --es banners alerts|incoming|cert|toasts                                                배너 층·토스트 표본
//   … --es screen history|groups|admin · --es mode call · --es panel channel|add|book|event   처음 자리
package com.cims.ue.dispatch.debug

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.core.view.WindowCompat
import com.cims.ue.dispatch.ui.AppScreen
import com.cims.ue.dispatch.ui.DEVICE_PREVIEWS
import com.cims.ue.dispatch.ui.DispatchMode
import com.cims.ue.dispatch.ui.NavState
import com.cims.ue.dispatch.ui.PreviewApp
import com.cims.ue.dispatch.ui.SidePanel

class PreviewActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val name = intent.getStringExtra("name").orEmpty()
        val dark = intent.getBooleanExtra("dark", false)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            isAppearanceLightStatusBars = !dark
            isAppearanceLightNavigationBars = !dark
        }
        val start = NavState(
            screen = when (intent.getStringExtra("screen")) {
                "history" -> AppScreen.HISTORY
                "groups" -> AppScreen.PTT_GROUPS
                "admin" -> AppScreen.ADMIN
                else -> AppScreen.DISPATCH
            },
            mode = if (intent.getStringExtra("mode") == "call") DispatchMode.CALL else DispatchMode.PTT,
            panel = when (intent.getStringExtra("panel")) {
                "channel" -> SidePanel.Channel("g1")
                "add" -> SidePanel.AddChannel
                "book" -> SidePanel.Book
                "event" -> SidePanel.Event(10)
                else -> null
            })
        val fixed = DEVICE_PREVIEWS[name]
        setContent {
            if (fixed != null) fixed() else PreviewApp(dark = dark, start = start, banners = intent.getStringExtra("banners").orEmpty())
        }
    }
}
