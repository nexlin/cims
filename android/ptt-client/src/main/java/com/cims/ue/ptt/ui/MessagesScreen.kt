package com.cims.ue.ptt.ui

import android.text.format.DateFormat
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.layout.fillMaxHeight
import com.cims.ue.core.message.MessageThread
import com.cims.ue.core.message.MsgDirection
import com.cims.ue.core.message.SendState
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.PttService
import com.cims.ue.ptt.R
import java.util.Date

private fun fmtTime(t: Long): String = DateFormat.format("HH:mm", Date(t)).toString()
private fun fmtDay(t: Long): String = DateFormat.format("M월 d일", Date(t)).toString()
private fun fmtSize(n: Long): String = when {
    n >= 1048576 -> "%.1fMB".format(n / 1048576.0)
    n >= 1024 -> "%.1fKB".format(n / 1024.0)
    n > 0 -> "${n}B"
    else -> ""
}

/** 삭제 확인 다이얼로그 공통. */
@Composable
private fun DeleteConfirmDialog(text: String, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("메시지 삭제") },
        text = { Text(text) },
        confirmButton = {
            TextButton(onClick = { onConfirm(); onDismiss() }) { Text("삭제", color = Ct.Red) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("취소") } },
    )
}

/** 메시지 탭 — 대화(스레드) 목록. 길게 누름=대화 삭제, 헤더 휴지통=전체 삭제. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun MessagesScreen(st: PttUiState, svc: PttService?, onOpenThread: (String) -> Unit) {
    val tick = svc?.messageTick?.collectAsState()?.value ?: 0
    val threads: List<MessageThread> = remember(tick, svc) { svc?.messages?.threads() ?: emptyList() }
    var confirmPeer by remember { mutableStateOf<String?>(null) }
    var confirmAll by remember { mutableStateOf(false) }

    confirmPeer?.let { peer ->
        DeleteConfirmDialog(
            "'${st.groupName(peer)}' 대화를 삭제할까요?",
            onConfirm = { svc?.deleteThread(peer) },
            onDismiss = { confirmPeer = null },
        )
    }
    if (confirmAll) {
        DeleteConfirmDialog(
            "모든 대화를 삭제할까요?",
            onConfirm = { svc?.deleteAllMessages() },
            onDismiss = { confirmAll = false },
        )
    }

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 10.dp)) {
        ScreenHeader(label = null, title = "메시지", trailing = if (threads.isEmpty()) null else {
            {
                Box(
                    Modifier.size(36.dp).clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                        .clickable { confirmAll = true },
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(painterResource(R.drawable.ic_delete), contentDescription = "전체 삭제",
                        tint = Ct.TextDim, modifier = Modifier.size(17.dp))
                }
            }
        })
        Spacer(Modifier.height(10.dp))

        if (threads.isEmpty()) {
            Box(Modifier.fillMaxWidth().padding(vertical = 60.dp), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Icon(painterResource(R.drawable.ic_message), contentDescription = null,
                        tint = Ct.TextFaint, modifier = Modifier.size(36.dp))
                    Text("메시지가 없습니다", color = Ct.TextFaint, fontSize = 13.sp)
                    Text("채널 화면의 말풍선 버튼으로 대화를 시작하세요", color = Ct.TextFaint, fontSize = 11.sp)
                }
            }
        }

        LazyColumn(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            items(threads, key = { it.peer }) { th ->
                Row(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Ct.Surface)
                        .combinedClickable(
                            onClick = { onOpenThread(th.peer) },
                            onLongClick = { confirmPeer = th.peer },
                        )
                        .padding(horizontal = 12.dp, vertical = 11.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    InitialAvatar(st.groupName(th.peer), active = th.unread > 0)
                    Column(Modifier.weight(1f)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(st.groupName(th.peer), color = Ct.Text, fontSize = 14.sp,
                                fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
                            Text(fmtTime(th.last.time), color = Ct.TextFaint, fontSize = 11.sp)
                        }
                        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 2.dp)) {
                            Text(th.last.text.ifBlank { if (th.last.attName.isNotBlank()) "📎 ${th.last.attName}" else "" },
                                color = Ct.TextDim, fontSize = 12.sp,
                                maxLines = 1, modifier = Modifier.weight(1f))
                            if (th.unread > 0) {
                                Box(
                                    Modifier.size(17.dp).clip(CircleShape).background(Ct.Mint),
                                    contentAlignment = Alignment.Center,
                                ) {
                                    Text(if (th.unread > 9) "9+" else "${th.unread}",
                                        color = Ct.OnMint, fontSize = 9.sp, fontWeight = FontWeight.Bold)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/** 대화 스레드(시안 `메시지화면.png`) — 말풍선 + 입력바. [peer]=그룹ID 또는 상대 번호.
 *  말풍선 길게 누름=선택 모드(1건/다건 삭제), 상단바에서 전체선택·삭제. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun MessageThreadScreen(st: PttUiState, svc: PttService?, peer: String, onBack: () -> Unit) {
    val tick = svc?.messageTick?.collectAsState()?.value ?: 0
    val entries = remember(tick, svc, peer) { svc?.messages?.thread(peer) ?: emptyList() }
    // MSRP 발신 진행률(msgId → 0f~1f) — 전송 중 말풍선 진행 바
    val progress = svc?.sendProgress?.collectAsState()?.value ?: emptyMap()
    var input by remember { mutableStateOf("") }
    val listState = rememberLazyListState()
    val joined = st.session(peer) != null
    // 선택 삭제 모드 — 선택된 MessageEntry.key 집합(비면 일반 모드)
    val selected = remember(peer) { mutableStateListOf<String>() }
    val selecting = selected.isNotEmpty()
    fun toggle(k: String) { if (!selected.remove(k)) selected.add(k) }
    var confirmSel by remember { mutableStateOf(false) }

    LaunchedEffect(peer) { svc?.markThreadRead(peer) }
    LaunchedEffect(entries.size) {
        if (entries.isNotEmpty()) listState.scrollToItem(entries.size - 1)
    }
    androidx.activity.compose.BackHandler(enabled = selecting) { selected.clear() }

    if (confirmSel) {
        DeleteConfirmDialog(
            "선택한 ${selected.size}건을 삭제할까요?",
            onConfirm = {
                svc?.deleteMessages(entries.filter { it.key in selected })
                selected.clear()
            },
            onDismiss = { confirmSel = false },
        )
    }

    Column(Modifier.fillMaxSize().statusBarsPadding().imePadding()) {
        // 상단바 — 선택 모드에서는 선택 수 + 전체선택 + 삭제로 전환
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Box(
                Modifier.size(36.dp).clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                    .clickable(onClick = { if (selecting) selected.clear() else onBack() }),
                contentAlignment = Alignment.Center,
            ) {
                Icon(painterResource(if (selecting) R.drawable.ic_close else R.drawable.ic_back),
                    contentDescription = if (selecting) "선택 취소" else "뒤로",
                    tint = Ct.Text, modifier = Modifier.size(18.dp))
            }
            Column(Modifier.weight(1f)) {
                Text(if (selecting) "${selected.size}개 선택" else st.groupName(peer),
                    color = Ct.Text, fontSize = 16.sp, fontWeight = FontWeight.Bold)
                Text(if (selecting) "삭제할 메시지 선택" else "메시지",
                    color = Ct.TextFaint, fontSize = 11.sp)
            }
            if (selecting) {
                Text("전체선택", color = Ct.Mint, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.clip(RoundedCornerShape(50)).background(Ct.Mint.copy(alpha = 0.14f))
                        .clickable {
                            selected.clear()
                            selected.addAll(entries.map { it.key })
                        }
                        .padding(horizontal = 10.dp, vertical = 5.dp))
                Box(
                    Modifier.size(36.dp).clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                        .clickable { confirmSel = true },
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(painterResource(R.drawable.ic_delete), contentDescription = "삭제",
                        tint = Ct.Red, modifier = Modifier.size(17.dp))
                }
            } else {
                // 1:1 통화 진입 — 상대가 그룹이 아닌 개인일 때만 (private call, TS 24.379 §11.1)
                val isGroupPeer = st.groups.any { PttController.bareId(it.uri) == peer }
                val ps = st.session(peer)
                when {
                    isGroupPeer -> if (joined) PillBadge("접속", Ct.Mint)
                    ps == null -> {
                        Text("무전", color = Ct.Mint, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
                            modifier = Modifier.clip(RoundedCornerShape(50))
                                .background(Ct.Mint.copy(alpha = 0.14f))
                                .clickable { st.ctl?.startPrivateCall(peer, fullDuplex = false) }
                                .padding(horizontal = 10.dp, vertical = 5.dp))
                        Text("통화", color = Ct.Amber, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
                            modifier = Modifier.clip(RoundedCornerShape(50))
                                .background(Ct.Amber.copy(alpha = 0.14f))
                                .clickable { st.ctl?.startPrivateCall(peer, fullDuplex = true) }
                                .padding(horizontal = 10.dp, vertical = 5.dp))
                    }
                    else -> {
                        PillBadge(if (ps.fullDuplex) "통화 중" else "무전 중", Ct.Mint)
                        Text("종료", color = Ct.Red, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
                            modifier = Modifier.clip(RoundedCornerShape(50))
                                .background(Ct.Red.copy(alpha = 0.14f))
                                .clickable { st.ctl?.leaveGroup(peer) }
                                .padding(horizontal = 10.dp, vertical = 5.dp))
                    }
                }
            }
        }

        // 말풍선 목록 — 날짜 구분선 + 발신(민트, 우측)/수신(다크, 좌측)
        LazyColumn(
            state = listState,
            modifier = Modifier.weight(1f).fillMaxWidth().padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            var lastDay = ""
            entries.forEachIndexed { i, e ->
                val day = fmtDay(e.time)
                if (day != lastDay) {
                    lastDay = day
                    items(listOf("day-$day-$i")) {
                        Box(Modifier.fillMaxWidth().padding(vertical = 8.dp), contentAlignment = Alignment.Center) {
                            Text(day, color = Ct.TextFaint, fontSize = 11.sp,
                                modifier = Modifier.clip(RoundedCornerShape(50))
                                    .background(Ct.SurfaceHi).padding(horizontal = 10.dp, vertical = 3.dp))
                        }
                    }
                }
                items(listOf("msg-$i")) {
                    val mine = e.direction == MsgDirection.OUT
                    val isSel = e.key in selected
                    Column(
                        Modifier.fillMaxWidth()
                            .clip(RoundedCornerShape(10.dp))
                            .background(if (isSel) Ct.Mint.copy(alpha = 0.10f)
                                else androidx.compose.ui.graphics.Color.Transparent)
                            .combinedClickable(
                                onClick = { if (selecting) toggle(e.key) },
                                onLongClick = { toggle(e.key) },
                            )
                            .padding(2.dp),
                    ) {
                        // 그룹 수신 문자 — 발신자 라벨 (MCData mcdata-info 그룹 귀속으로 스레드=그룹)
                        if (!mine && e.sender.isNotBlank() && e.sender != peer) {
                            Text(e.sender, color = Ct.TextFaint, fontSize = 10.sp,
                                modifier = Modifier.padding(start = 4.dp, bottom = 1.dp))
                        }
                        Row(
                            Modifier.fillMaxWidth(),
                            horizontalArrangement = if (mine) Arrangement.End else Arrangement.Start,
                            verticalAlignment = Alignment.Bottom,
                        ) {
                            if (mine) {
                                // 전송 상태 — 🕓 전송중(%)/⚠ 실패(탭=재전송)/✓ 전송됨/✓✓ 전달확인
                                val (label, color) = when {
                                    e.sendState == SendState.PENDING -> {
                                        val pct = progress[e.msgId]
                                            ?.let { " ${(it * 100).toInt()}%" }.orEmpty()
                                        "🕓$pct" to Ct.TextFaint
                                    }
                                    e.sendState == SendState.FAILED -> "⚠ 실패 · 재전송" to Ct.Red
                                    e.delivered -> "✓✓ ${fmtTime(e.time)}" to Ct.Mint
                                    else -> "✓ ${fmtTime(e.time)}" to Ct.TextFaint
                                }
                                Text(label, color = color, fontSize = 10.sp,
                                    modifier = Modifier.padding(end = 6.dp, bottom = 2.dp).let { m ->
                                        if (e.sendState == SendState.FAILED && !selecting)
                                            m.clickable { svc?.resendMessage(e) } else m
                                    })
                            }
                            // 첨부(MCData FD) — 탭: 미다운로드 → 다운로드, 완료 → 열기
                            val isAtt = e.attName.isNotBlank()
                            val bubble = Modifier
                                .widthIn(max = 264.dp)
                                .clip(RoundedCornerShape(
                                    topStart = 14.dp, topEnd = 14.dp,
                                    bottomStart = if (mine) 14.dp else 4.dp,
                                    bottomEnd = if (mine) 4.dp else 14.dp))
                                .background(if (mine) Ct.Mint else Ct.SurfaceHi)
                                .let { m ->
                                    // 첨부 탭=받기/열기 — 선택 모드에서는 선택 토글로 동작(길게 누름=선택 진입)
                                    if (isAtt) m.combinedClickable(
                                        onClick = {
                                            if (selecting) toggle(e.key)
                                            else if (e.attPath.isBlank()) svc?.downloadAttachment(e)
                                            else svc?.openAttachment(e)
                                        },
                                        onLongClick = { toggle(e.key) },
                                    ) else m
                                }
                                .padding(horizontal = 12.dp, vertical = 8.dp)
                            Text(
                                if (isAtt) {
                                    val st = if (e.attPath.isBlank()) "받기" else "열기"
                                    "📎 ${e.attName}\n${fmtSize(e.attSize)} · $st"
                                } else e.text,
                                color = if (mine) Ct.OnMint else Ct.Text,
                                fontSize = 14.sp,
                                modifier = bubble,
                            )
                            if (!mine) {
                                Text(fmtTime(e.time), color = Ct.TextFaint, fontSize = 10.sp,
                                    modifier = Modifier.padding(start = 6.dp, bottom = 2.dp))
                            }
                        }
                        // MSRP 전송 진행 바 — 청크 진행률(작은 문자는 순간 완료라 안 보임)
                        if (mine && e.sendState == SendState.PENDING) {
                            progress[e.msgId]?.let { f ->
                                Box(
                                    Modifier.align(Alignment.End).padding(top = 3.dp, end = 2.dp)
                                        .width(140.dp).height(3.dp)
                                        .clip(RoundedCornerShape(2.dp)).background(Ct.SurfaceHi),
                                ) {
                                    Box(Modifier.fillMaxHeight().fillMaxWidth(f.coerceIn(0f, 1f))
                                        .background(Ct.Mint))
                                }
                            }
                        }
                    }
                }
            }
        }

        // 입력바(시안) — 첨부(사진/동영상, 전송은 서버 경로 연동 후) + 입력 + 전송
        Row(
            Modifier.fillMaxWidth().background(Ct.Surface)
                .navigationBarsPadding().padding(horizontal = 12.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            AttachButton(42) { uri -> svc?.sendGroupAttachment(peer, uri) }
            Box(
                Modifier.weight(1f).height(42.dp)
                    .clip(RoundedCornerShape(21.dp)).background(Ct.SurfaceHi)
                    .padding(horizontal = 14.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                if (input.isEmpty()) Text("메시지 입력", color = Ct.TextFaint, fontSize = 13.sp)
                BasicTextField(
                    value = input, onValueChange = { input = it },
                    textStyle = TextStyle(color = Ct.Text, fontSize = 14.sp),
                    cursorBrush = SolidColor(Ct.Mint),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
            val canSend = input.isNotBlank()
            Box(
                Modifier.size(42.dp).clip(CircleShape)
                    .background(if (canSend) Ct.Mint else Ct.SurfaceHi)
                    .clickable(enabled = canSend) {
                        svc?.sendGroupMessage(peer, input.trim())
                        input = ""
                    },
                contentAlignment = Alignment.Center,
            ) {
                Icon(painterResource(R.drawable.ic_send), contentDescription = "전송",
                    tint = if (canSend) Ct.OnMint else Ct.TextFaint, modifier = Modifier.size(18.dp))
            }
        }
    }
}
