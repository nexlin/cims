// Preview 공용 틀 — 모든 화면 Preview 가 같은 조건에서 뜬다 (android_dispatch_tablet.md §6.3)
//
// **같은 자로 재야 비교가 된다.** 화면마다 크기·테마를 다르게 잡으면 «이건 좁다» 가 배치 탓인지 Preview
// 설정 탓인지 알 수 없다. 그래서 크기 하나·틀 하나로 고정한다.
//
// 크기는 관제 태블릿 실물(가로 1280×800dp)에서 상단 바 56 + 발언 바 80 + 하단 내비 56 을 뺀 **608dp** 다
// (§6.3). 이 높이여야 «한 화면에 몇 줄 보이는가» 가 실제와 같다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier

/** 본문 크기 — 하단 내비·발언 바를 뺀 실제 값(§6.3). `@Preview(device = PreviewBody)`. */
const val PreviewBody = "spec:width=1280dp,height=608dp,dpi=240"

/** 전체 화면 크기 — 내비·발언 바까지 포함해 보고 싶을 때. */
const val PreviewFull = "spec:width=1280dp,height=800dp,dpi=240"

/** 테마·배경을 입힌다. Preview 는 앱 테마 밖에서 뜨므로 이걸 씌우지 않으면 색이 기본값으로 나온다. */
@Composable
fun PreviewFrame(dark: Boolean = false, body: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) { body() }
    }
}
