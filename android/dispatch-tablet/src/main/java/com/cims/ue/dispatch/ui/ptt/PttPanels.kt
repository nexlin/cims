@file:OptIn(ExperimentalFoundationApi::class)
// PTT 공용 조각 — 메시지·이벤트·상태 점 (docs/design/features/android_dispatch_tablet.md §6.3)
//
// 데스크톱의 ④ PTT 메시지 · ⑤ PTT 이벤트 본문이다. 태블릿에서는 **두 곳**이 이것을 쓴다 —
// 채널 화면의 «메시지»·«이벤트» 면(한 채널)과 [메시지] 화면(전 채널). 같은 것을 두 벌로 만들지 않는다.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.Type
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import com.cims.ue.dispatch.ui.PersonAction
import com.cims.ue.dispatch.ui.PersonMenu
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.ActivityKind
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private val hhmmss = SimpleDateFormat("HH:mm:ss", Locale.KOREA)
private val hhmm = SimpleDateFormat("HH:mm", Locale.KOREA)

/** ④ PTT 메시지 — 스레드 칩 + 대화 + 입력. 채널 화면의 «메시지» 면과 [메시지] 화면이 같이 쓴다. */
@Composable
internal fun Messages(vm: PttMessagesViewModel) {
    val thread by vm.thread.collectAsStateWithLifecycle()
    val title by vm.title.collectAsStateWithLifecycle()
    val follow by vm.follow.collectAsStateWithLifecycle()
    val groupId by vm.groupId.collectAsStateWithLifecycle()
    var draft by remember { mutableStateOf("") }

    Column {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("● $title", fontSize = Type.body, modifier = Modifier.weight(1f))
            TextButton(onClick = vm::toggleFollow) {
                Text(if (follow) "따라가기 켬" else "고정", fontSize = Type.body)
            }
        }

        // 스레드 칩 — 그룹과 1:1 이 한 줄에 선다. **이게 없으면 1:1 을 열고 돌아갈 수 없다**(§6.9a).
        val threads by vm.threads.collectAsStateWithLifecycle()
        if (threads.isNotEmpty()) Row(
            Modifier.fillMaxWidth().padding(vertical = 4.dp).horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            threads.forEach { t ->
                FilterChip(
                    selected = t.key == groupId,
                    onClick = { vm.pickThread(t.key) },
                    label = {
                        Text(t.title + if (t.unread > 0) "  ${t.unread}" else "", fontSize = Type.meta)
                    })
            }
        }
        LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            items(thread, key = { it.id }) { m ->
                Row(Modifier.fillMaxWidth(),
                    horizontalArrangement = if (m.outgoing) Arrangement.End else Arrangement.Start) {
                    Surface(
                        color = if (m.outgoing) MaterialTheme.colorScheme.primaryContainer
                                else MaterialTheme.colorScheme.surfaceVariant,
                        shape = RoundedCornerShape(8.dp)
                    ) {
                        Column(Modifier.padding(8.dp).widthIn(max = 260.dp)) {
                            if (!m.outgoing) Text(m.fromName, fontSize = Type.meta, fontWeight = FontWeight.Bold)
                            Text(m.text, fontSize = Type.strong)
                            Text(hhmm.format(Date(m.atMs)) + sendMark(m.state), fontSize = Type.micro)
                        }
                    }
                }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = draft, onValueChange = { draft = it },
                placeholder = { Text("메시지", fontSize = Type.body) },
                singleLine = true, enabled = groupId != null,
                modifier = Modifier.weight(1f),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send))
            TextButton(
                onClick = { vm.send(draft); draft = "" },
                enabled = groupId != null && draft.isNotBlank()
            ) { Text("보내기") }
        }
    }
}

private fun sendMark(s: com.cims.ue.dispatch.session.SendState): String = when (s) {
    com.cims.ue.dispatch.session.SendState.PENDING -> " ⏳"
    com.cims.ue.dispatch.session.SendState.SENT -> " ✓"
    com.cims.ue.dispatch.session.SendState.DELIVERED -> " ✓✓"
    com.cims.ue.dispatch.session.SendState.READ -> " ✓✓"
    com.cims.ue.dispatch.session.SendState.FAILED -> " ✕"
    else -> ""
}

// ── ⑤ 이벤트 ─────────────────────────────────────────────────────────────────
@Composable
internal fun Activity(vm: PttActivityViewModel) {
    val rows by vm.rows.collectAsStateWithLifecycle()
    val filter by vm.filter.collectAsStateWithLifecycle()
    val follow by vm.followFocus.collectAsStateWithLifecycle()

    Column {
        Row(verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            ActivityFilter.entries.forEach { f ->
                FilterChip(selected = filter == f, onClick = { vm.setFilter(f) },
                           label = { Text(f.label, fontSize = Type.body) })
            }
            Spacer(Modifier.weight(1f))
            TextButton(onClick = vm::toggleFollowFocus) {
                Text(if (follow) "포커스만" else "전체", fontSize = Type.body)
            }
        }
        if (rows.isEmpty()) { Empty("이벤트 없음"); return }
        LazyColumn(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            items(rows, key = { it.atMs.toString() + it.text }) { r ->
                Row(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                    Text(hhmmss.format(Date(r.atMs)), fontSize = Type.meta,
                         modifier = Modifier.width(64.dp))
                    Text(r.groupName, fontSize = Type.meta, fontWeight = FontWeight.Bold,
                         modifier = Modifier.width(80.dp))
                    Text(r.text, fontSize = Type.meta,
                         color = if (r.emergency || r.kind == ActivityKind.ERROR)
                                     MaterialTheme.colorScheme.error else Color.Unspecified)
                }
            }
        }
    }
}

// ── 공용 ─────────────────────────────────────────────────────────────────────
@Composable
internal fun StateDot(active: Boolean, speaking: Boolean, emergency: Boolean) {
    val c = when {
        emergency -> MaterialTheme.colorScheme.error
        speaking -> MaterialTheme.colorScheme.primary
        active -> MaterialTheme.colorScheme.tertiary
        else -> MaterialTheme.colorScheme.outline
    }
    Box(Modifier.size(10.dp).clip(RoundedCornerShape(5.dp)).background(c))
}

@Composable
private fun Empty(text: String) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(text, fontSize = Type.body, color = MaterialTheme.colorScheme.outline)
    }
}
