@file:OptIn(ExperimentalFoundationApi::class)
// PTT 공용 조각 — 메시지·이벤트·상태 점 (docs/design/features/android_dispatch_tablet.md §6.3)
//
// 데스크톱의 ④ PTT 메시지 · ⑤ PTT 이벤트 본문이다. 태블릿에서는 **두 곳**이 이것을 쓴다 —
// 채널 화면의 «메시지»·«이벤트» 면(한 채널)과 [메시지] 화면(전 채널). 같은 것을 두 벌로 만들지 않는다.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.session.ActivityRow
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.ui.Type
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import com.cims.ue.dispatch.ui.PersonAction
import com.cims.ue.dispatch.ui.PersonMenu
import com.cims.ue.dispatch.ui.RecipientPicker
import com.cims.ue.dispatch.ui.Tag
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.material3.Badge
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.VerticalDivider
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.fillMaxHeight
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

/** ④ PTT 메시지 — **VM 을 붙이는 껍데기**. [무전] › «메시지» 면이 쓴다. */
@Composable
internal fun Messages(vm: PttMessagesViewModel) {
    val thread by vm.thread.collectAsStateWithLifecycle()
    val title by vm.title.collectAsStateWithLifecycle()
    val follow by vm.follow.collectAsStateWithLifecycle()
    val groupId by vm.groupId.collectAsStateWithLifecycle()
    val threads by vm.threads.collectAsStateWithLifecycle()
    val isGroup by vm.isGroup.collectAsStateWithLifecycle()
    val candidates by vm.candidates.collectAsStateWithLifecycle()

    var picking by remember { mutableStateOf(false) }
    if (picking) RecipientPicker(
        title = "새 무전 메시지 — 받는 곳",
        options = candidates,
        onPick = { k -> vm.openTo(k); picking = false },
        onDismiss = { picking = false })

    MessagesContent(
        thread = thread, title = title, follow = follow, groupId = groupId, threads = threads,
        isGroup = isGroup, onToggleFollow = vm::toggleFollow, onPickThread = vm::pickThread,
        onSend = vm::send, onNew = { picking = true })
}

/**
 * ④ 본문 — **스레드 목록 : 대화** 두 열(android_dispatch_tablet.md §6.9a).
 *
 * 전에는 칩 한 줄 + 대화였다. 칩은 **몇 개 넘어가면 가로로 밀려** 어느 스레드가 있는지 한눈에 안 보이고,
 * 미읽음이 어디 쌓였는지도 모른다. 메시지 앱이 흔히 그러듯 **왼쪽에 스레드 목록, 오른쪽에 대화**로 둔다 —
 * 가로 1280dp 라 둘 다 놓을 수 있고, 목록에서 고르면 오른쪽이 바뀐다.
 *
 * 초안만 자기 상태로 든다(보내면 비운다).
 */
@Composable
fun MessagesContent(
    thread: List<Message>,
    title: String,
    follow: Boolean,
    groupId: String?,
    threads: List<ThreadChip>,
    isGroup: Boolean = true,
    onToggleFollow: () -> Unit = {},
    onPickThread: (String) -> Unit = {},
    onSend: (String) -> Unit = {},
    onNew: () -> Unit = {},
) {
    Row(Modifier.fillMaxSize()) {
        // ── 왼쪽: 스레드 목록 ──
        Column(Modifier.width(280.dp).fillMaxHeight()) {
            Row(Modifier.fillMaxWidth().padding(start = 10.dp, end = 4.dp, top = 4.dp, bottom = 4.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Text("대화 ${threads.size}", fontSize = Type.strong, fontWeight = FontWeight.Bold,
                    modifier = Modifier.weight(1f))
                // 따라가기 = 포커스 채널로 자동 전환. 끄면 고른 대화에 머문다(§6.9a).
                TextButton(onClick = onToggleFollow,
                    contentPadding = PaddingValues(horizontal = 6.dp)) {
                    Text(if (follow) "따라가기" else "고정", fontSize = Type.meta)
                }
                TextButton(onClick = onNew, contentPadding = PaddingValues(horizontal = 6.dp)) {
                    Text("＋ 새 대화", fontSize = Type.meta)
                }
            }
            HorizontalDivider()
            if (threads.isEmpty()) {
                Text("주고받은 대화가 없습니다 — [＋ 새 대화] 로 그룹이나 사람을 고릅니다",
                    Modifier.padding(16.dp), fontSize = Type.meta,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            LazyColumn(Modifier.weight(1f)) {
                items(threads, key = { it.key }) { t ->
                    val on = t.key == groupId
                    Row(Modifier.fillMaxWidth()
                        .background(if (on) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent)
                        .clickable { onPickThread(t.key) }
                        .padding(horizontal = 10.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically) {
                        Text(t.title, fontSize = Type.body, maxLines = 1,
                            fontWeight = if (t.unread > 0) FontWeight.Bold else FontWeight.Normal,
                            modifier = Modifier.weight(1f))
                        if (t.lastAtMs > 0) Text(hhmm.format(Date(t.lastAtMs)), fontSize = Type.micro,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        if (t.unread > 0) { Spacer(Modifier.width(4.dp)); Badge { Text("${t.unread}") } }
                    }
                    HorizontalDivider()
                }
            }
        }
        VerticalDivider()

        // ── 오른쪽: 고른 대화 ──
        Column(Modifier.weight(1f).fillMaxHeight().padding(8.dp)) {
            if (groupId == null) {
                Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally) {
                    Text("대화를 고르세요", fontSize = Type.body,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    TextButton(onClick = onNew) { Text("＋ 새 대화") }
                }
                return@Column
            }
            // **그룹인지 사람인지 머리에 적는다** — 같은 입력칸에서 «편성 전원에게» 와 «이 사람에게» 가
            //   갈리므로, 보내기 전에 어디로 가는지 보여야 한다.
            Row(Modifier.padding(bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(title, fontSize = Type.title, fontWeight = FontWeight.Bold, maxLines = 1,
                    modifier = Modifier.weight(1f, fill = false))
                Spacer(Modifier.width(6.dp))
                if (isGroup) Tag("그룹 전원", MaterialTheme.colorScheme.tertiary, leading = 0)
                else Tag("1:1", MaterialTheme.colorScheme.primary, leading = 0)
            }
            HorizontalDivider()
            LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                items(thread, key = { it.id }) { m ->
                    Row(Modifier.fillMaxWidth(),
                        horizontalArrangement = if (m.outgoing) Arrangement.End else Arrangement.Start) {
                        Surface(
                            color = if (m.outgoing) MaterialTheme.colorScheme.primaryContainer
                                    else MaterialTheme.colorScheme.surfaceVariant,
                            shape = RoundedCornerShape(8.dp)
                        ) {
                            Column(Modifier.padding(8.dp).widthIn(max = 420.dp)) {
                                // 1:1 에서는 상대가 하나뿐이라 이름을 매 말풍선에 적을 이유가 없다.
                                if (!m.outgoing && isGroup) Text(m.fromName, fontSize = Type.meta,
                                    fontWeight = FontWeight.Bold)
                                Text(m.text, fontSize = Type.strong)
                                Text(hhmm.format(Date(m.atMs)) + sendMark(m.state), fontSize = Type.micro)
                            }
                        }
                    }
                }
            }
            var draft by remember(groupId) { mutableStateOf("") }
            Row(verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = draft, onValueChange = { draft = it },
                    placeholder = {
                        Text(if (isGroup) "그룹 전원에게" else "이 사람에게", fontSize = Type.body)
                    },
                    singleLine = true, modifier = Modifier.weight(1f),
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send))
                TextButton(onClick = { onSend(draft); draft = "" }, enabled = draft.isNotBlank()) {
                    Text("보내기")
                }
            }
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
    ActivityContent(rows, filter, follow, vm::setFilter, vm::toggleFollowFocus)
}

/** ⑤ 본문 — **순수 컴포저블**. */
@Composable
fun ActivityContent(
    rows: List<ActivityRow>,
    filter: ActivityFilter,
    follow: Boolean,
    onFilter: (ActivityFilter) -> Unit = {},
    onToggleFollow: () -> Unit = {},
) {
    Column {
        Row(verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            ActivityFilter.entries.forEach { f ->
                FilterChip(selected = filter == f, onClick = { onFilter(f) },
                           label = { Text(f.label, fontSize = Type.body) })
            }
            Spacer(Modifier.weight(1f))
            TextButton(onClick = onToggleFollow) {
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
