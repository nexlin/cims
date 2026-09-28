// [더보기] 화면 Preview (android_dispatch_tablet.md §6.3)
package com.cims.ue.dispatch.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview

@Preview(name = "더보기", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewMore() = PreviewFrame {
    MoreScreen(onOpen = {}, onSettings = {}, dirty = false)
}

/** [관리]에 저장하지 않은 폼이 있을 때 — 점 배지. 전환을 막지는 않는다(§4.5). */
@Preview(name = "더보기 — 관리 미저장", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewMoreDirty() = PreviewFrame {
    MoreScreen(onOpen = {}, onSettings = {}, dirty = true)
}
