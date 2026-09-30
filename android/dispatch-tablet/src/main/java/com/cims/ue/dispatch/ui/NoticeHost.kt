// 토스트 — 명령 실패의 사유, 우하단 (android_dispatch_tablet.md §6.2a-2, dispatch_desktop_ui.md §3.2)
//
// 배너(긴급·착신)가 «지금 벌어지는 일» 이라면 토스트는 «방금 누른 것이 왜 안 됐나» 다. 그래서 자리도 다르다 — 배너는
// 위에서 본문을 밀고, 토스트는 본문 위에 겹쳐 오른쪽 아래에 선다(누른 손 근처, 보던 목록은 그대로).
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.Notice
import com.cims.ue.dispatch.session.NoticeLevel

@Composable
fun Notices(session: DispatchSession, modifier: Modifier = Modifier) {
    val list by session.notices.collectAsStateWithLifecycle()
    NoticeStack(list, onDismiss = session::dismissNotice, modifier = modifier)
}

/** 토스트 스택 — **순수 컴포저블**. 최신이 위, 최대 6장(세션이 자른다). */
@Composable
fun NoticeStack(items: List<Notice>, onDismiss: (Long) -> Unit = {}, modifier: Modifier = Modifier) {
    if (items.isEmpty()) return
    Column(modifier.widthIn(max = 480.dp), verticalArrangement = Arrangement.spacedBy(6.dp),
        horizontalAlignment = Alignment.End) {
        items.forEach { NoticeCard(it, onDismiss) }
    }
}

@Composable
private fun NoticeCard(n: Notice, onDismiss: (Long) -> Unit) {
    var open by remember(n.id) { mutableStateOf(false) }
    val (bg, fg) = when (n.level) {
        NoticeLevel.ERROR -> MaterialTheme.colorScheme.errorContainer to MaterialTheme.colorScheme.onErrorContainer
        NoticeLevel.WARN -> MaterialTheme.colorScheme.tertiaryContainer to MaterialTheme.colorScheme.onTertiaryContainer
        NoticeLevel.INFO -> MaterialTheme.colorScheme.inverseSurface to MaterialTheme.colorScheme.inverseOnSurface
    }
    Surface(color = bg, contentColor = fg, shape = RoundedCornerShape(8.dp), shadowElevation = 4.dp) {
        Column(Modifier.padding(start = 12.dp, end = 4.dp, top = 4.dp, bottom = 6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(n.text, fontSize = Type.body, fontWeight = FontWeight.Bold,
                    modifier = Modifier.weight(1f, fill = false))
                // 원문 코드는 접어 둔다 — 문장이 먼저 읽혀야 하고, 코드는 운영자에게 전할 때 편다.
                if (n.detail.isNotEmpty()) TextButton(onClick = { open = !open }) {
                    Text(if (open) "▾상세" else "▸상세", fontSize = Type.meta)
                }
                IconButton(onClick = { onDismiss(n.id) }) { Icon(Icons.Filled.Close, contentDescription = "닫기") }
            }
            if (open) Text(n.detail, fontSize = Type.meta, modifier = Modifier.padding(end = 8.dp))
        }
    }
}
