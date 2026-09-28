// [무전] 메뉴 — 면 셋 (android_dispatch_tablet.md §6.3)
//
// 채널 · 메시지 · 이벤트. 셋이 한 축에 있는 이유는 전부 **무전의 일**이기 때문이다 — SDS 는 무전
// 채널의 대화이고 청취는 무전 leg 이다. 따로 최상위 메뉴로 세우면 «내가 듣던 게 어디 있더라» 를
// 두 군데서 찾게 된다.
//
// **탭은 지름길이고 이동은 스와이프다.** 면은 최상위 메뉴와 함께 한 줄로 꿰여 있어(`APP_PAGES`)
// 옆으로 밀면 옆 면으로, 끝 면에서 더 밀면 옆 메뉴로 넘어간다. 여기 탭줄은 그 줄의 한 지점으로
// 건너뛰는 수단이다 — 탭줄 자체는 **각 장이 자기 것을 그린다**(밀 때 옆 메뉴의 탭줄이 따라 들어온다).
//
// «메시지»·«이벤트» 는 **포커스를 따라간다** — 채널 면에서 고른 채널의 것을 보여 준다. 데스크톱이
// ④⑤ 를 포커스에 묶은 것과 같은 불변이다(dispatch_desktop_ui.md §4.1).
package com.cims.ue.dispatch.ui.ptt

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Badge
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.ui.PttPane
import com.cims.ue.dispatch.ui.Type

/**
 * [무전] 탭줄 **하나**. 껍데기가 면 pager 위에 고정으로 놓는다(§6.3) — 면을 밀 때 이 줄은 제자리에
 * 남고 아래 본문만 미끄러진다. 줄까지 같이 미끄러지면 «메뉴가 통째로 바뀌었나» 로 읽힌다.
 *
 * @param unread 미읽음 SDS — «메시지» 탭 배지.
 */
@Composable
fun PttTabRow(pane: PttPane, onPane: (PttPane) -> Unit, unread: Int = 0) {
    TabRow(selectedTabIndex = pane.ordinal) {
        PttPane.entries.forEach { p ->
            val n = if (p == PttPane.MESSAGES) unread else 0
            Tab(selected = pane == p, onClick = { onPane(p) }, text = {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(p.label, fontSize = Type.body)
                    if (n > 0) { Spacer(Modifier.width(4.dp)); Badge { Text("$n") } }
                }
            })
        }
    }
}

/** 탭줄 + 본문 한 벌 — **미리보기·단독 사용**용. 앱에서는 껍데기가 둘을 따로 놓는다. */
@Composable
fun PttTabs(
    pane: PttPane,
    onPane: (PttPane) -> Unit,
    unread: Int = 0,
    modifier: Modifier = Modifier,
    body: @Composable (PttPane) -> Unit,
) {
    Column(modifier.fillMaxSize()) {
        PttTabRow(pane, onPane, unread)
        // **weight 여야 한다.** `fillMaxSize` 는 들어온 최대 높이를 그대로 요구해서 TabRow 높이만큼
        //   넘치고, 안에 LazyColumn 이 있으면 Preview 렌더가 깨진다.
        Box(Modifier.weight(1f)) { body(pane) }
    }
}

/** 면 하나가 비었을 때의 문구 자리 — 면마다 다른 말을 해야 하므로 호출자가 준다. */
@Composable
internal fun PaneEmpty(text: String) =
    Box(Modifier.fillMaxSize().padding(24.dp), contentAlignment = Alignment.Center) {
        Text(text, fontSize = Type.body,
            color = androidx.compose.material3.MaterialTheme.colorScheme.onSurfaceVariant)
    }
