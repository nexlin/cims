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
import androidx.compose.foundation.border
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
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

/**
 * 본문 — **순수 컴포저블**. 넓으면 **왼쪽 대화 목록 : 오른쪽 대화**, 좁으면(주소록 패널이 열려 면이 줄었을 때) 한 번에 하나 —
 * 목록에서 고르면 대화, 대화 머리 [‹] 로 목록.
 */
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
    BoxWithConstraints(modifier.fillMaxSize()) {
        val compact = maxWidth < 600.dp
        var showList by remember { mutableStateOf(peer == null) }
        LaunchedEffect(peer) { if (peer != null) showList = false }
        if (compact) {
            if (showList || peer == null) ThreadList(threads, peer, onPick = { k -> onPick(k); showList = false }, onNew,
                Modifier.fillMaxSize())
            else Conversation(thread, peer, title, available, external, onSend, onResend, onNew,
                onBack = { showList = true }, modifier = Modifier.fillMaxSize())
        } else Row(Modifier.fillMaxSize()) {
            ThreadList(threads, peer, onPick, onNew, Modifier.width(340.dp).fillMaxHeight())
            com.cims.ue.dispatch.ui.VDivider()
            Conversation(thread, peer, title, available, external, onSend, onResend, onNew, onBack = null,
                modifier = Modifier.weight(1f).fillMaxHeight())
        }
    }
}

/** 대화 목록 — 무전 «메시지» 와 같은 모양(문자는 모두 1:1 이라 종류 라벨이 없다). */
@Composable
private fun ThreadList(threads: List<ThreadChip>, peer: String?, onPick: (String) -> Unit, onNew: () -> Unit,
                       modifier: Modifier) {
    val p = com.cims.ue.dispatch.ui.Tokens.palette
    Column(modifier) {
        com.cims.ue.dispatch.ui.SectionHead("문자 ${threads.size}") {
            Spacer(Modifier.weight(1f))
            com.cims.ue.dispatch.ui.PillButton("＋ 새 대화", onNew, height = 32.dp, strongBorder = true)
        }
        if (threads.isEmpty()) Text(
            "주고받은 문자가 없습니다 — [＋ 새 대화] 로 시작합니다",
            Modifier.padding(16.dp), fontSize = Type.meta, color = p.muted)
        LazyColumn(Modifier.weight(1f)) {
            items(threads, key = { it.key }) { t ->
                com.cims.ue.dispatch.ui.ptt.ThreadRow(t, selected = t.key == peer, showKind = false) { onPick(t.key) }
            }
        }
    }
}

/** 고른 대화. [onBack] 이 있으면(좁을 때) 머리 왼쪽에 [‹] — 목록으로. */
@Composable
private fun Conversation(
    thread: List<Message>,
    peer: String?,
    title: String,
    available: Boolean,
    external: Boolean,
    onSend: (String) -> Unit,
    onResend: (Message) -> Unit,
    onNew: () -> Unit,
    onBack: (() -> Unit)?,
    modifier: Modifier,
) {
    val p = com.cims.ue.dispatch.ui.Tokens.palette
    Column(modifier) {
        if (peer == null) {
            Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center,
                horizontalAlignment = Alignment.CenterHorizontally) {
                Text("대화를 고르세요", fontSize = Type.body, color = p.muted)
                Spacer(Modifier.height(8.dp))
                com.cims.ue.dispatch.ui.PillButton("＋ 새 대화", onNew, strongBorder = true)
            }
            return@Column
        }
        Row(Modifier.fillMaxWidth().height(56.dp).padding(start = if (onBack != null) 4.dp else 20.dp, end = 12.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            if (onBack != null) IconButton(onClick = onBack, modifier = Modifier.size(44.dp)) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "대화 목록",
                    modifier = Modifier.size(20.dp))
            }
            Text(title, fontSize = Type.head, fontWeight = FontWeight.Bold, maxLines = 1,
                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
            com.cims.ue.dispatch.ui.Label("문자", com.cims.ue.dispatch.ui.LabelStyle.OUTLINE, round = true)
            if (external) com.cims.ue.dispatch.ui.Label("외부망", com.cims.ue.dispatch.ui.LabelStyle.OUTLINE,
                round = true, color = p.emergency)
        }
        com.cims.ue.dispatch.ui.HDivider()
        LazyColumn(Modifier.weight(1f).padding(horizontal = 20.dp), verticalArrangement = Arrangement.spacedBy(8.dp),
            contentPadding = PaddingValues(vertical = 14.dp)) {
            items(thread, key = { it.id }) { m ->
                com.cims.ue.dispatch.ui.ptt.Bubble(m, showName = false, onResend = { onResend(m) })
            }
        }
        SmsInput(available = available, external = external, onSend = onSend, peer = peer)
    }
}

/** 보내기 줄 — 글자 수와 **못 보내는 이유**를 같이 든다(70자를 넘으면 LMS 로 나간다 — 누르기 전에 알아야 한다). */
@Composable
private fun SmsInput(available: Boolean, external: Boolean, peer: String, onSend: (String) -> Unit) {
    val p = com.cims.ue.dispatch.ui.Tokens.palette
    var draft by remember(peer) { mutableStateOf("") }
    val blocked = !available || external
    Column(Modifier.fillMaxWidth()
            .drawBehind { drawLine(p.hair, androidx.compose.ui.geometry.Offset(0f, 0f), androidx.compose.ui.geometry.Offset(size.width, 0f), 1.dp.toPx()) }) {
        if (blocked) Text(
            if (!available) "전화 계정이 없어 문자를 보낼 수 없습니다"
            else "외부망 번호입니다 — 게이트웨이가 없어 보낼 수 없습니다(받는 것은 됩니다)",
            Modifier.fillMaxWidth().padding(top = 8.dp),
            fontSize = Type.meta, color = p.emergency, textAlign = TextAlign.Center)
        Row(Modifier.fillMaxWidth().height(68.dp).padding(horizontal = 20.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            val send = { if (!blocked && draft.isNotBlank()) { onSend(draft); draft = "" } }
            androidx.compose.foundation.text.BasicTextField(
                value = draft, onValueChange = { draft = it }, singleLine = true, enabled = !blocked,
                textStyle = androidx.compose.ui.text.TextStyle(fontSize = Type.strong, color = p.ink),
                cursorBrush = androidx.compose.ui.graphics.SolidColor(p.ink),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                keyboardActions = androidx.compose.foundation.text.KeyboardActions(onSend = { send() }),
                modifier = Modifier.weight(1f).height(44.dp).clip(RoundedCornerShape(22.dp))
                    .border(1.5.dp, if (blocked) p.line else p.ink, RoundedCornerShape(22.dp))
                    .background(if (blocked) p.bar else p.paper),
                decorationBox = { inner ->
                    Box(Modifier.fillMaxSize().padding(horizontal = 16.dp), contentAlignment = Alignment.CenterStart) {
                        if (draft.isEmpty()) Text("문자", fontSize = Type.strong, color = p.faint)
                        inner()
                    }
                })
            Text(smsCountText(draft), fontSize = Type.micro, color = p.muted)
            com.cims.ue.dispatch.ui.PillButton("보내기", send, height = 44.dp, filled = true,
                enabled = !blocked && draft.isNotBlank())
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
