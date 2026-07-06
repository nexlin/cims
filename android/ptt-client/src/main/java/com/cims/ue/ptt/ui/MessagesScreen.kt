package com.cims.ue.ptt.ui

import android.text.format.DateFormat
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
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
import com.cims.ue.core.message.MessageThread
import com.cims.ue.core.message.MsgDirection
import com.cims.ue.ptt.PttService
import com.cims.ue.ptt.R
import java.util.Date

private fun fmtTime(t: Long): String = DateFormat.format("HH:mm", Date(t)).toString()
private fun fmtDay(t: Long): String = DateFormat.format("M월 d일", Date(t)).toString()

/** 메시지 탭 — 대화(스레드) 목록. */
@Composable
fun MessagesScreen(st: PttUiState, svc: PttService?, onOpenThread: (String) -> Unit) {
    val tick = svc?.messageTick?.collectAsState()?.value ?: 0
    val threads: List<MessageThread> = remember(tick, svc) { svc?.messages?.threads() ?: emptyList() }

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 10.dp)) {
        ScreenHeader(label = null, title = "메시지")
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
                        .clickable { onOpenThread(th.peer) }
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
                            Text(th.last.text, color = Ct.TextDim, fontSize = 12.sp,
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

/** 대화 스레드(시안 `메시지화면.png`) — 말풍선 + 입력바. [peer]=그룹ID 또는 상대 번호. */
@Composable
fun MessageThreadScreen(st: PttUiState, svc: PttService?, peer: String, onBack: () -> Unit) {
    val tick = svc?.messageTick?.collectAsState()?.value ?: 0
    val entries = remember(tick, svc, peer) { svc?.messages?.thread(peer) ?: emptyList() }
    var input by remember { mutableStateOf("") }
    val listState = rememberLazyListState()
    val joined = st.session(peer) != null

    LaunchedEffect(peer) { svc?.markThreadRead(peer) }
    LaunchedEffect(entries.size) {
        if (entries.isNotEmpty()) listState.scrollToItem(entries.size - 1)
    }

    Column(Modifier.fillMaxSize().statusBarsPadding().imePadding()) {
        // 상단바
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Box(
                Modifier.size(36.dp).clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                    .clickable(onClick = onBack),
                contentAlignment = Alignment.Center,
            ) {
                Icon(painterResource(R.drawable.ic_back), contentDescription = "뒤로",
                    tint = Ct.Text, modifier = Modifier.size(18.dp))
            }
            Column(Modifier.weight(1f)) {
                Text(st.groupName(peer), color = Ct.Text, fontSize = 16.sp, fontWeight = FontWeight.Bold)
                Text("메시지", color = Ct.TextFaint, fontSize = 11.sp)
            }
            if (joined) PillBadge("접속", Ct.Mint)
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
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = if (mine) Arrangement.End else Arrangement.Start,
                        verticalAlignment = Alignment.Bottom,
                    ) {
                        if (mine) {
                            Text(fmtTime(e.time), color = Ct.TextFaint, fontSize = 10.sp,
                                modifier = Modifier.padding(end = 6.dp, bottom = 2.dp))
                        }
                        Text(
                            e.text,
                            color = if (mine) Ct.OnMint else Ct.Text,
                            fontSize = 14.sp,
                            modifier = Modifier
                                .widthIn(max = 264.dp)
                                .clip(RoundedCornerShape(
                                    topStart = 14.dp, topEnd = 14.dp,
                                    bottomStart = if (mine) 14.dp else 4.dp,
                                    bottomEnd = if (mine) 4.dp else 14.dp))
                                .background(if (mine) Ct.Mint else Ct.SurfaceHi)
                                .padding(horizontal = 12.dp, vertical = 8.dp),
                        )
                        if (!mine) {
                            Text(fmtTime(e.time), color = Ct.TextFaint, fontSize = 10.sp,
                                modifier = Modifier.padding(start = 6.dp, bottom = 2.dp))
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
            AttachButton(42)
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
