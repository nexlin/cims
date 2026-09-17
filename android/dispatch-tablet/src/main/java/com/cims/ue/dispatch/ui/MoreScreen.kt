// [더보기] 화면 — 자주 열지 않는 화면들의 입구 (android_dispatch_tablet.md §6.3)
//
// 데스크톱에서 최상위 메뉴였던 [이력][PTT 그룹][관리](§3.4 F2~F4)를 여기 모은다. 빼는 근거는 «자주
// 쓰는가» 하나다 — 조회·편성·관리는 상황이 생겼을 때 여는 화면이고, 무전·통화·메시지·감청은 상시다.
// 화면 «내용» 은 그대로이고 들어가는 문만 한 겹 깊어진다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

/**
 * @param dirty [관리]에 저장하지 않은 폼이 있는가 — 점 배지(§4.5). 전환을 막지는 않는다.
 */
@Composable
fun MoreScreen(
    onOpen: (MoreItem) -> Unit,
    onSettings: () -> Unit,
    dirty: Boolean,
    modifier: Modifier = Modifier,
) {
    Column(modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
        MoreItem.entries.forEach { item ->
            MoreRow(item.label, item.hint, badge = dirty && item == MoreItem.ADMIN) { onOpen(item) }
        }
        HorizontalDivider(Modifier.padding(vertical = 8.dp))
        MoreRow("설정", "오디오·발언·동시 청취 상한·서버 인증서") { onSettings() }
    }
}

@Composable
private fun MoreRow(title: String, hint: String, badge: Boolean = false, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(title, fontSize = Type.title, fontWeight = FontWeight.Bold)
                if (badge) { Spacer(Modifier.width(6.dp)); Badge() }
            }
            Text(hint, fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, contentDescription = null)
    }
    HorizontalDivider()
}
