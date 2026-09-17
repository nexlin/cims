// [PTT] 탭 화면 — 발언 바 + ①②④⑤ (docs/design/features/android_dispatch_tablet.md §6.3)
//
// 배치: 발언 바(탭 바깥, MainActivity) / 위 ① 내 채널 · ② 범위 채널 / 아래 ④ 메시지 · ⑤ 이벤트.
// 카드 모드는 **타일**이고, 필터·검색은 ② 에만 있다.
package com.cims.ue.dispatch.ui.ptt

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

/** [PTT] 탭 본문 — 위 ①② / 아래 ④⑤. */
@Composable
fun PttTab(
    channels: PttChannelsViewModel,
    scoped: ScopedChannelsViewModel,
    messages: PttMessagesViewModel,
    activity: PttActivityViewModel,
    modifier: Modifier = Modifier,
) {
    val focused by channels.focused.collectAsStateWithLifecycle()

    // ④⑤ 는 포커스를 따라간다 — ① 이 포커스를 바꾸면 알린다(§6.3 포커스의 뜻).
    LaunchedEffect(focused?.id) {
        messages.onFocusChanged(focused?.id)
        activity.onFocusChanged(focused?.id)
    }

    Column(modifier.fillMaxSize()) {
        Row(Modifier.weight(0.55f).fillMaxWidth()) {
            Panel("① 내 채널", Modifier.weight(1f)) { MyChannels(channels) }
            VerticalDivider()
            Panel("② 범위 채널", Modifier.weight(1f)) { ScopedChannels(scoped) }
        }
        HorizontalDivider()
        Row(Modifier.weight(0.45f).fillMaxWidth()) {
            Panel("④ PTT 메시지", Modifier.weight(1f)) { Messages(messages) }
            VerticalDivider()
            Panel("⑤ PTT 이벤트", Modifier.weight(1f)) { Activity(activity) }
        }
    }
}

@Composable
private fun Panel(title: String, modifier: Modifier = Modifier, body: @Composable () -> Unit) {
    Column(modifier.fillMaxHeight().padding(8.dp)) {
        Text(title, fontWeight = FontWeight.Bold, fontSize = 13.sp,
             modifier = Modifier.padding(bottom = 6.dp))
        body()
    }
}

// ── ① 내 채널 ────────────────────────────────────────────────────────────────
@Composable
private fun MyChannels(vm: PttChannelsViewModel) {
    val cards by vm.cards.collectAsStateWithLifecycle()
    val selected by vm.selectedId.collectAsStateWithLifecycle()
    val targets by vm.targetIds.collectAsStateWithLifecycle()
    var sheet by remember { mutableStateOf(false) }

    // 빠른 발신 줄(§4.1) — 멤버 그룹은 서버가 편성하지만 사설콜·애드혹은 **관제사가 연다**.
    // 이 줄이 없으면 ① 은 «서버가 준 것만 보는» 패널이 된다.
    Row(Modifier.fillMaxWidth().padding(bottom = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Text("내 채널 ${cards.size}", fontSize = 12.sp, fontWeight = FontWeight.Bold,
             modifier = Modifier.weight(1f))
        OutlinedButton(onClick = { sheet = true },
            contentPadding = PaddingValues(horizontal = 10.dp)) { Text("사설콜·애드혹", fontSize = 12.sp) }
    }
    // 사람 메뉴의 «애드혹에 추가» 가 심어 둔 씨앗이 있으면 시트를 연다(§6.2f). 씨앗은 시트가 소비한다.
    val seed by vm.adhocSeed.collectAsStateWithLifecycle()
    LaunchedEffect(seed) { if (seed.isNotBlank()) sheet = true }
    if (sheet) OriginateSheet(vm) { sheet = false }

    if (cards.isEmpty()) { Empty("참여할 채널이 없습니다"); return }

    LazyColumn(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        items(cards, key = { it.id }) { c ->
            val isFocused = c.id == selected
            Card(
                colors = CardDefaults.cardColors(
                    containerColor = when {
                        c.emergency -> MaterialTheme.colorScheme.errorContainer
                        isFocused -> MaterialTheme.colorScheme.secondaryContainer
                        else -> MaterialTheme.colorScheme.surfaceVariant
                    }),
                modifier = Modifier.fillMaxWidth().clickable { vm.focus(c.id) }
            ) {
                Column(Modifier.padding(10.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        StateDot(active = c.hasSession, speaking = c.speaking, emergency = c.emergency)
                        Spacer(Modifier.width(6.dp))
                        Text("${c.index}. ${c.title}", fontWeight = FontWeight.Bold, fontSize = 14.sp,
                             modifier = Modifier.weight(1f))
                        if (c.unread > 0) Badge { Text("${c.unread}") }
                        Spacer(Modifier.width(6.dp))
                        Text(c.stateText, fontSize = 12.sp)
                    }
                    Text(c.line2, fontSize = 12.sp, modifier = Modifier.padding(start = 16.dp))

                    Row(
                        Modifier.padding(top = 6.dp).fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        // 발언 대상 — 체크박스는 «무엇을 고르는 것인지» 가 드러나지 않아 라벨 있는 칩으로 둔다.
                        // 카드 탭(포커스)과는 여전히 다른 조작이다(§6.3 포커스 ≠ 발언 대상).
                        if (c.canCheck) FilterChip(
                            selected = c.id in targets,
                            onClick = { vm.toggleTarget(c.id) },
                            label = { Text(if (c.id in targets) "발언 대상 ✓" else "발언 대상", fontSize = 12.sp) })

                        if (c.joined) {
                            TextButton(onClick = { vm.leave(c) }) { Text("나가기") }
                        } else if (c.kind == CardKind.MEMBER) {
                            Button(onClick = { vm.join(c) }) { Text("참여") }
                            TextButton(onClick = { vm.joinEmergency(c) }) { Text("긴급") }
                        }
                        Spacer(Modifier.weight(1f))
                        if (c.participants > 0) Text("참가 ${c.participants}", fontSize = 12.sp)
                    }
                }
            }
        }
    }
}

// ── ② 범위 채널 ──────────────────────────────────────────────────────────────
@Composable
private fun ScopedChannels(vm: ScopedChannelsViewModel) {
    val cards by vm.cards.collectAsStateWithLifecycle()
    val filter by vm.filter.collectAsStateWithLifecycle()
    val query by vm.query.collectAsStateWithLifecycle()

    Column {
        // 필터·검색은 이 패널에만 있다(§6.3)
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp),
            modifier = Modifier.padding(bottom = 6.dp)) {
            ScopeFilter.entries.forEach { f ->
                FilterChip(selected = filter == f, onClick = { vm.setFilter(f) },
                           label = { Text(f.label, fontSize = 12.sp) })
            }
        }
        OutlinedTextField(
            value = query, onValueChange = vm::setQuery,
            placeholder = { Text("검색", fontSize = 12.sp) },
            singleLine = true, modifier = Modifier.fillMaxWidth().padding(bottom = 6.dp),
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search))

        if (cards.isEmpty()) { Empty("청취 범위 채널이 없습니다"); return }

        LazyColumn(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            items(cards, key = { it.id }) { c ->
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = if (c.emergency) MaterialTheme.colorScheme.errorContainer
                                         else MaterialTheme.colorScheme.surfaceVariant),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Column(Modifier.padding(10.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            StateDot(active = c.hasSession, speaking = c.speaker.isNotEmpty(),
                                     emergency = c.emergency)
                            Spacer(Modifier.width(6.dp))
                            Text(c.title, fontWeight = FontWeight.Bold, fontSize = 14.sp,
                                 modifier = Modifier.weight(1f))
                            Text(c.stateText, fontSize = 12.sp)
                        }
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(c.line2, fontSize = 12.sp, modifier = Modifier.weight(1f))
                            TextButton(onClick = { vm.toggleListen(c) }) {
                                Text(if (c.listening) "청취 끄기" else "청취")
                            }
                        }
                    }
                }
            }
        }
    }
}

// ── ④ 메시지 ─────────────────────────────────────────────────────────────────
@Composable
private fun Messages(vm: PttMessagesViewModel) {
    val thread by vm.thread.collectAsStateWithLifecycle()
    val title by vm.title.collectAsStateWithLifecycle()
    val follow by vm.follow.collectAsStateWithLifecycle()
    val groupId by vm.groupId.collectAsStateWithLifecycle()
    var draft by remember { mutableStateOf("") }

    Column {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("● $title", fontSize = 12.sp, modifier = Modifier.weight(1f))
            TextButton(onClick = vm::toggleFollow) {
                Text(if (follow) "따라가기 켬" else "고정", fontSize = 12.sp)
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
                            if (!m.outgoing) Text(m.fromName, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                            Text(m.text, fontSize = 13.sp)
                            Text(hhmm.format(Date(m.atMs)) + sendMark(m.state), fontSize = 10.sp)
                        }
                    }
                }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = draft, onValueChange = { draft = it },
                placeholder = { Text("메시지", fontSize = 12.sp) },
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
private fun Activity(vm: PttActivityViewModel) {
    val rows by vm.rows.collectAsStateWithLifecycle()
    val filter by vm.filter.collectAsStateWithLifecycle()
    val follow by vm.followFocus.collectAsStateWithLifecycle()

    Column {
        Row(verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            ActivityFilter.entries.forEach { f ->
                FilterChip(selected = filter == f, onClick = { vm.setFilter(f) },
                           label = { Text(f.label, fontSize = 12.sp) })
            }
            Spacer(Modifier.weight(1f))
            TextButton(onClick = vm::toggleFollowFocus) {
                Text(if (follow) "포커스만" else "전체", fontSize = 12.sp)
            }
        }
        if (rows.isEmpty()) { Empty("이벤트 없음"); return }
        LazyColumn(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            items(rows, key = { it.atMs.toString() + it.text }) { r ->
                Row(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                    Text(hhmmss.format(Date(r.atMs)), fontSize = 11.sp,
                         modifier = Modifier.width(64.dp))
                    Text(r.groupName, fontSize = 11.sp, fontWeight = FontWeight.Bold,
                         modifier = Modifier.width(80.dp))
                    Text(r.text, fontSize = 11.sp,
                         color = if (r.emergency || r.kind == ActivityKind.ERROR)
                                     MaterialTheme.colorScheme.error else Color.Unspecified)
                }
            }
        }
    }
}

// ── 공용 ─────────────────────────────────────────────────────────────────────
@Composable
private fun StateDot(active: Boolean, speaking: Boolean, emergency: Boolean) {
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
        Text(text, fontSize = 12.sp, color = MaterialTheme.colorScheme.outline)
    }
}
