// [통화] > «메시지» 면 — 전화 축 문자(SMS/LMS)
//   정본: android_dispatch_tablet.md §6.2e, volte_supplementary_services.md §4.3
//
// 휴대폰 문자 앱과 같은 모양 — **왼쪽 대화 목록 : 오른쪽 대화**. [무전] > «메시지» 와 같은 배치를 쓰되
// 망이 다르다(이쪽은 SIP MESSAGE text/plain, 저쪽은 MCData SDS).
//
// **외부망 번호는 보내기가 막힌다** — 게이트웨이가 없다. 막는 것을 말하지 않으면 «왜 안 가지» 가 된다.
//
// 목록 머리의 **[＋ 새 대화]** 가 «아직 주고받은 적 없는 사람에게 처음 보내는» 길이다. 이것이 없으면
// 상대에게서 먼저 오기를 기다리거나 사람 메뉴를 거치는 수밖에 없어, 문자 면만 열어서는 아무것도 못 쓴다.
package com.cims.ue.dispatch.ui.call

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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.ui.RecipientPicker
import com.cims.ue.dispatch.ui.Type
import com.cims.ue.dispatch.ui.ptt.ThreadChip
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private val hhmm = SimpleDateFormat("HH:mm", Locale.KOREA)

/** VM 을 붙이는 껍데기. */
@Composable
fun SmsPane(vm: SmsMessagesViewModel, modifier: Modifier = Modifier) {
    val threads by vm.threads.collectAsStateWithLifecycle()
    val thread by vm.thread.collectAsStateWithLifecycle()
    val peer by vm.peer.collectAsStateWithLifecycle()
    val title by vm.title.collectAsStateWithLifecycle()
    val external by vm.selectedIsExternal.collectAsStateWithLifecycle()
    val candidates by vm.candidates.collectAsStateWithLifecycle()

    var picking by remember { mutableStateOf(false) }
    if (picking) RecipientPicker(
        title = "새 문자 — 받는 사람",
        options = candidates,
        // 주소록에 없는 내선에도 보낼 수 있어야 한다 — 방금 개통된 회선은 주소록이 늦게 따라온다.
        manualHint = "번호 직접 입력",
        onPick = { n -> vm.openTo(n); picking = false },
        onDismiss = { picking = false })

    SmsPaneContent(
        threads = threads, thread = thread, peer = peer, title = title,
        available = vm.available, external = external,
        onPick = vm::pick, onSend = vm::send, onResend = vm::resend, onNew = { picking = true }, modifier = modifier)
}

/** 본문 — **순수 컴포저블**. */
@Composable
fun SmsPaneContent(
    threads: List<ThreadChip>,
    thread: List<Message>,
    peer: String?,
    title: String,
    available: Boolean = true,
    external: Boolean = false,
    onPick: (String) -> Unit = {},
    onSend: (String) -> Unit = {},
    /** 실패한 발신 말풍선의 [재전송]. */
    onResend: (Message) -> Unit = {},
    onNew: () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    Row(modifier.fillMaxSize()) {
        // ── 왼쪽: 대화 목록 ──
        Column(Modifier.width(280.dp).fillMaxHeight()) {
            Row(Modifier.fillMaxWidth().padding(start = 10.dp, end = 4.dp, top = 4.dp, bottom = 4.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Text("문자 ${threads.size}", Modifier.weight(1f),
                    fontSize = Type.strong, fontWeight = FontWeight.Bold)
                TextButton(onClick = onNew, contentPadding = PaddingValues(horizontal = 8.dp)) {
                    Text("＋ 새 대화", fontSize = Type.meta)
                }
            }
            HorizontalDivider()
            if (threads.isEmpty()) Text(
                "주고받은 문자가 없습니다 — [＋ 새 대화] 로 시작합니다",
                Modifier.padding(12.dp), fontSize = Type.meta,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            LazyColumn(Modifier.weight(1f)) {
                items(threads, key = { it.key }) { t ->
                    val on = t.key == peer
                    Row(Modifier.fillMaxWidth()
                        .background(if (on) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent)
                        .clickable { onPick(t.key) }
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
            if (peer == null) {
                Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally) {
                    Text("대화를 고르세요", fontSize = Type.body,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    TextButton(onClick = onNew) { Text("＋ 새 대화") }
                }
                return@Column
            }
            Text(title, fontSize = Type.title, fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(bottom = 4.dp))
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
                                Text(m.text, fontSize = Type.strong)
                                Text(hhmm.format(Date(m.atMs)) + sendMark(m.state),
                                    fontSize = Type.micro)
                                if (m.outgoing && m.state == com.cims.ue.dispatch.session.SendState.FAILED)
                                    com.cims.ue.dispatch.ui.ptt.ResendButton { onResend(m) }
                            }
                        }
                    }
                }
            }
            SmsInput(available = available, external = external, onSend = onSend, peer = peer)
        }
    }
}

/** 보내기 줄 — 글자 수와 **못 보내는 이유**를 같이 든다. */
@Composable
private fun SmsInput(available: Boolean, external: Boolean, peer: String, onSend: (String) -> Unit) {
    var draft by remember(peer) { mutableStateOf("") }
    val blocked = !available || external
    Column {
        if (blocked) Text(
            if (!available) "전화 계정이 없어 문자를 보낼 수 없습니다"
            else "외부망 번호입니다 — 게이트웨이가 없어 보낼 수 없습니다(받는 것은 됩니다)",
            Modifier.fillMaxWidth().padding(vertical = 4.dp),
            fontSize = Type.meta, color = MaterialTheme.colorScheme.error, textAlign = TextAlign.Center)
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = draft, onValueChange = { draft = it },
                placeholder = { Text("문자", fontSize = Type.body) },
                enabled = !blocked, singleLine = true, modifier = Modifier.weight(1f),
                // 70자를 넘으면 LMS 로 나간다 — 누르기 전에 알아야 한다.
                supportingText = { Text(smsCountText(draft), fontSize = Type.micro) },
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send))
            TextButton(onClick = { onSend(draft); draft = "" },
                enabled = !blocked && draft.isNotBlank()) { Text("보내기") }
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
