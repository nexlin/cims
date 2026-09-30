@file:OptIn(ExperimentalFoundationApi::class)
// [관제] › [무전] 의 «메시지»·«이벤트» 면 + 이벤트 상세 패널 (docs/design/features/android_dispatch_tablet.md §6.3·§6.9a)
//
// 데스크톱의 ④ PTT 메시지 · ⑤ PTT 이벤트 본문이다. «메시지» = 왼쪽 대화 목록(340) : 오른쪽 대화, «이벤트» = 왼쪽 거르기(220) :
// 가운데 표 : 오른쪽 이벤트 상세(사이드 패널). 행을 누르면 보던 면을 떠나지 않고 옆에서 자세히 본다.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.session.ActivityRow
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.ui.Type
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.ActivityKind
import com.cims.ue.dispatch.session.AlertKind
import com.cims.ue.dispatch.session.SendState
import com.cims.ue.dispatch.ui.AlertBannerUi
import com.cims.ue.dispatch.ui.CountPill
import com.cims.ue.dispatch.ui.FilterPill
import com.cims.ue.dispatch.ui.HDivider
import com.cims.ue.dispatch.ui.Initial
import com.cims.ue.dispatch.ui.Label
import com.cims.ue.dispatch.ui.LabelStyle
import com.cims.ue.dispatch.ui.PillButton
import com.cims.ue.dispatch.ui.RecipientPicker
import com.cims.ue.dispatch.ui.SectionHead
import com.cims.ue.dispatch.ui.SidePanelFrame
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.VDivider
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale

private val hhmmss = SimpleDateFormat("HH:mm:ss", Locale.KOREA)
private val hhmm = SimpleDateFormat("HH:mm", Locale.KOREA)
private val md = SimpleDateFormat("M월 d일", Locale.KOREA)

/** 빠른 답 — 한 번 눌러 곧바로 보낸다(관제사가 가장 자주 치는 말). */
internal val QUICK_REPLIES = listOf("확인했습니다", "이동 중", "도착했습니다", "대기 바랍니다")

// ── 메시지 ──────────────────────────────────────────────────────────────────

/** «메시지» 면 — **VM 을 붙이는 껍데기**. [onChannelInfo] = 머리 [채널 정보 ›](보던 대화 옆에 채널 상세). */
@Composable
internal fun Messages(vm: PttMessagesViewModel, onChannelInfo: (String) -> Unit = {}) {
    val thread by vm.thread.collectAsStateWithLifecycle()
    val title by vm.title.collectAsStateWithLifecycle()
    val follow by vm.follow.collectAsStateWithLifecycle()
    val groupId by vm.groupId.collectAsStateWithLifecycle()
    val threads by vm.threads.collectAsStateWithLifecycle()
    val isGroup by vm.isGroup.collectAsStateWithLifecycle()
    val candidates by vm.candidates.collectAsStateWithLifecycle()
    val info by vm.groupInfo.collectAsStateWithLifecycle()

    var picking by remember { mutableStateOf(false) }
    if (picking) RecipientPicker(
        title = "새 무전 메시지 — 받는 곳",
        options = candidates,
        onPick = { k -> vm.openTo(k); picking = false },
        onDismiss = { picking = false })

    MessagesContent(
        thread = thread, title = title, follow = follow, groupId = groupId, threads = threads,
        isGroup = isGroup, members = info?.memberCount ?: 0, online = info?.connectedCount ?: 0,
        onToggleFollow = vm::toggleFollow, onPickThread = vm::pickThread,
        onSend = vm::send, onResend = vm::resend, onNew = { picking = true },
        onChannelInfo = onChannelInfo)
}

/** 대화 목록 거르기. */
private enum class ThreadFilter(val label: String) { ALL("전체"), GROUP("그룹"), DIRECT("1:1"), UNREAD("안 읽음") }

/**
 * «메시지» 본문 — **대화 목록 : 대화** 두 열(§6.9a). 초안·거르기만 자기 상태로 든다.
 *
 * @param members·online 그룹 대화의 머리 «그룹 전원 · 편성 12 · 접속 7» — 보내기 전에 몇 명에게 가는지 보이게.
 */
@Composable
fun MessagesContent(
    thread: List<Message>,
    title: String,
    follow: Boolean,
    groupId: String?,
    threads: List<ThreadChip>,
    isGroup: Boolean = true,
    members: Int = 0,
    online: Int = 0,
    onToggleFollow: () -> Unit = {},
    onPickThread: (String) -> Unit = {},
    onSend: (String) -> Unit = {},
    /** 실패한 발신 말풍선의 [재전송]. */
    onResend: (Message) -> Unit = {},
    onNew: () -> Unit = {},
    onChannelInfo: (String) -> Unit = {},
) {
    val p = Tokens.palette
    var tf by remember { mutableStateOf(ThreadFilter.ALL) }
    val shown = remember(threads, tf) {
        threads.filter { t -> when (tf) {
            ThreadFilter.ALL -> true
            ThreadFilter.GROUP -> t.group
            ThreadFilter.DIRECT -> !t.group
            ThreadFilter.UNREAD -> t.unread > 0
        } }
    }
    Row(Modifier.fillMaxSize()) {
        // ── 왼쪽: 대화 목록 ──
        Column(Modifier.width(340.dp).fillMaxHeight()) {
            SectionHead("대화 ${threads.size}") {
                Spacer(Modifier.weight(1f))
                // 따라가기 = 포커스 채널로 자동 전환. 끄면 고른 대화에 머문다(§6.9a).
                PillButton(if (follow) "따라가기 ✓" else "따라가기", onToggleFollow, height = 32.dp)
                PillButton("＋ 새 대화", onNew, height = 32.dp, strongBorder = true)
            }
            val unreadTotal = threads.count { it.unread > 0 }
            Row(Modifier.padding(start = 16.dp, end = 16.dp, bottom = 8.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                ThreadFilter.entries.forEach { f ->
                    FilterPill(if (f == ThreadFilter.UNREAD && unreadTotal > 0) "${f.label} $unreadTotal" else f.label,
                        f == tf, onClick = { tf = f })
                }
            }
            if (threads.isEmpty()) Text("주고받은 대화가 없습니다 — [＋ 새 대화] 로 그룹이나 사람을 고릅니다",
                Modifier.padding(16.dp), fontSize = Type.meta, color = p.muted)
            LazyColumn(Modifier.weight(1f)) {
                items(shown, key = { it.key }) { t -> ThreadRow(t, selected = t.key == groupId) { onPickThread(t.key) } }
            }
        }
        VDivider()

        // ── 오른쪽: 고른 대화 ──
        Column(Modifier.weight(1f).fillMaxHeight()) {
            if (groupId == null) {
                Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally) {
                    Text("대화를 고르세요", fontSize = Type.body, color = p.muted)
                    Spacer(Modifier.height(8.dp))
                    PillButton("＋ 새 대화", onNew, strongBorder = true)
                }
                return@Column
            }
            // **그룹인지 사람인지 머리에 적는다** — 같은 입력칸에서 «편성 전원에게» 와 «이 사람에게» 가 갈리므로,
            //   보내기 전에 어디로 가는지 보여야 한다.
            Row(
                Modifier.fillMaxWidth().height(56.dp)
                    .drawBehind { drawLine(p.divider, Offset(0f, size.height - 0.5f), Offset(size.width, size.height - 0.5f), 1.dp.toPx()) }
                    .padding(start = 20.dp, end = 12.dp),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Text(title, fontSize = Type.head, fontWeight = FontWeight.Bold, maxLines = 1,
                    overflow = TextOverflow.Ellipsis, modifier = Modifier.widthIn(max = 360.dp))
                if (isGroup) {
                    Label(if (members > 0) "그룹 전원 · 편성 $members" else "그룹 전원", LabelStyle.STRONG, round = true)
                    if (online > 0) Text("접속 $online", fontSize = Type.meta, color = p.muted)
                } else Label("1:1", LabelStyle.OUTLINE, round = true)
                Spacer(Modifier.weight(1f))
                if (isGroup) PillButton("채널 정보 ›", { onChannelInfo(groupId) })
            }
            val list = rememberLazyListState()
            // 새 글이 오면 맨 아래로 — 대화는 아래가 최신이다.
            LaunchedEffect(thread.size) {
                if (thread.isNotEmpty()) list.animateScrollToItem((thread.size + dayBreaks(thread) - 1).coerceAtLeast(0))
            }
            LazyColumn(Modifier.weight(1f).padding(horizontal = 20.dp), state = list,
                verticalArrangement = Arrangement.spacedBy(8.dp), contentPadding = PaddingValues(vertical = 14.dp)) {
                var lastDay = ""
                thread.forEach { m ->
                    val day = dayLabel(m.atMs)
                    if (day != lastDay) {
                        lastDay = day
                        item(key = "day-$day-${m.id}") {
                            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) { Label(day, LabelStyle.OUTLINE, round = true) }
                        }
                    }
                    item(key = m.id) { Bubble(m, showName = isGroup, onResend = { onResend(m) }) }
                }
            }
            var draft by remember(groupId) { mutableStateOf("") }
            // 빠른 답 — 한 번 눌러 곧바로 보낸다.
            Row(Modifier.fillMaxWidth()
                    .drawBehind { drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
                    .horizontalScroll(rememberScrollState()).padding(start = 20.dp, end = 20.dp, top = 8.dp),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("빠른 답", fontSize = Type.meta, color = p.muted)
                QUICK_REPLIES.forEach { q -> PillButton(q, { onSend(q) }, height = 32.dp) }
            }
            Row(Modifier.fillMaxWidth().height(68.dp).padding(horizontal = 20.dp),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                val send = { if (draft.isNotBlank()) { onSend(draft); draft = "" } }
                BasicTextField(
                    value = draft, onValueChange = { draft = it }, singleLine = true,
                    textStyle = TextStyle(fontSize = Type.strong, color = p.ink), cursorBrush = SolidColor(p.ink),
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                    keyboardActions = KeyboardActions(onSend = { send() }),
                    modifier = Modifier.weight(1f).height(44.dp).clip(RoundedCornerShape(22.dp))
                        .border(1.5.dp, p.ink, RoundedCornerShape(22.dp)).background(p.paper),
                    decorationBox = { inner ->
                        Box(Modifier.fillMaxSize().padding(horizontal = 16.dp), contentAlignment = Alignment.CenterStart) {
                            if (draft.isEmpty()) Text(
                                if (isGroup) "그룹 전원에게 ($title${if (members > 0) " · ${members}명" else ""})" else "이 사람에게 ($title)",
                                fontSize = Type.strong, color = p.faint, maxLines = 1)
                            inner()
                        }
                    })
                PillButton("보내기", send, height = 44.dp, filled = true, enabled = draft.isNotBlank())
            }
        }
    }
}

/** 대화 목록 한 줄(68) — 머리글자(그룹 = 둥근 네모, 1:1 = 원) · 제목·종류 · 마지막 한 통 · 시각·미읽음. */
@Composable
internal fun ThreadRow(t: ThreadChip, selected: Boolean, showKind: Boolean = true, onClick: () -> Unit) {
    val p = Tokens.palette
    Row(
        Modifier.fillMaxWidth().height(68.dp).background(if (selected) p.fill else p.paper)
            .drawBehind {
                drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx())
                if (selected) drawRect(p.ink, size = androidx.compose.ui.geometry.Size(3.dp.toPx(), size.height))
            }
            .clickable(onClick = onClick).padding(start = 16.dp, end = 12.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Initial(t.title, size = 36.dp, square = t.group)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(t.title, fontSize = Type.strong, fontWeight = if (t.unread > 0) FontWeight.Bold else FontWeight.Medium,
                    maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                if (showKind) Label(if (t.group) "그룹" else "1:1", LabelStyle.OUTLINE)
            }
            if (t.last.isNotEmpty()) Text(t.last, fontSize = Type.meta, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(4.dp)) {
            if (t.lastAtMs > 0) Text(hhmm.format(Date(t.lastAtMs)), fontSize = Type.micro, color = p.muted)
            CountPill(t.unread)
        }
    }
}

/** 말풍선 — 받은 것 = 옅은 면(그룹이면 보낸 사람 이름 위), 보낸 것 = 검정 면 오른쪽. 시각·전달 상태는 풍선 바깥. */
@Composable
internal fun Bubble(m: Message, showName: Boolean, onResend: () -> Unit) {
    val p = Tokens.palette
    Column(Modifier.fillMaxWidth(), horizontalAlignment = if (m.outgoing) Alignment.End else Alignment.Start,
        verticalArrangement = Arrangement.spacedBy(2.dp)) {
        // 1:1 에서는 상대가 하나뿐이라 이름을 매 말풍선에 적을 이유가 없다.
        if (!m.outgoing && showName && m.fromName.isNotBlank())
            Text(m.fromName, fontSize = Type.meta, fontWeight = FontWeight.Bold, color = p.ink2)
        Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            if (m.outgoing) Meta(m, onResend)
            Box(Modifier.widthIn(max = 520.dp).clip(RoundedCornerShape(10.dp))
                    .background(if (m.outgoing) p.ink else p.fill).padding(horizontal = 12.dp, vertical = 9.dp)) {
                Text(m.text, fontSize = Type.strong, color = if (m.outgoing) p.onInk else p.ink)
            }
            if (!m.outgoing) Meta(m, onResend)
        }
    }
}

@Composable
private fun Meta(m: Message, onResend: () -> Unit) {
    val p = Tokens.palette
    Column(horizontalAlignment = if (m.outgoing) Alignment.End else Alignment.Start) {
        Text(hhmm.format(Date(m.atMs)) + (if (m.outgoing) sendMark(m.state) else ""), fontSize = Type.micro,
            color = if (m.state == SendState.FAILED) p.emergency else p.muted, maxLines = 1)
        // 실패는 누르면 다시 보낸다 — 같은 말풍선이 갱신된다(데스크톱 ⚠ 링크).
        if (m.outgoing && m.state == SendState.FAILED) ResendButton(onResend)
    }
}

/** 실패한 발신 말풍선의 [재전송] — 무전 메시지와 문자 면이 같이 쓴다. */
@Composable
internal fun ResendButton(onClick: () -> Unit) {
    TextButton(onClick = onClick, contentPadding = PaddingValues(horizontal = 4.dp, vertical = 0.dp),
        modifier = Modifier.heightIn(min = 28.dp)) {
        Text("재전송", fontSize = Type.meta, color = Tokens.palette.emergency)
    }
}

private fun sendMark(s: SendState): String = when (s) {
    SendState.PENDING -> " · 보내는 중"
    SendState.SENT -> " ✓"
    SendState.DELIVERED -> " ✓✓"
    SendState.READ -> " ✓✓"
    SendState.FAILED -> " · 실패"
    else -> ""
}

/** 날짜 칸 이름 — 오늘·어제·«M월 d일». */
internal fun dayLabel(atMs: Long, nowMs: Long = System.currentTimeMillis()): String {
    fun key(ms: Long) = Calendar.getInstance().apply { timeInMillis = ms }.let { it.get(Calendar.YEAR) * 1000 + it.get(Calendar.DAY_OF_YEAR) }
    val d = key(atMs); val today = key(nowMs); val yesterday = key(nowMs - 86_400_000L)
    return when (d) { today -> "오늘"; yesterday -> "어제"; else -> md.format(Date(atMs)) }
}

private fun dayBreaks(thread: List<Message>): Int = thread.map { dayLabel(it.atMs) }.distinct().size

// ── 이벤트 ───────────────────────────────────────────────────────────────────

/** 거르기의 종류 — 입장·퇴장은 한 칸으로 묶는다(시안 «입퇴장»). */
enum class EventKind(val label: String, val kinds: Set<ActivityKind>) {
    TALK("발언", setOf(ActivityKind.TALK)),
    MOVE("입퇴장", setOf(ActivityKind.JOIN, ActivityKind.LEAVE)),
    EMERGENCY("긴급", setOf(ActivityKind.EMERGENCY)),
    SDS("SDS", setOf(ActivityKind.SDS)),
    ERROR("오류", setOf(ActivityKind.ERROR)),
}

/** 표의 종류 낱말. */
internal fun kindLabel(k: ActivityKind): String = when (k) {
    ActivityKind.TALK -> "발언"; ActivityKind.JOIN -> "입장"; ActivityKind.LEAVE -> "퇴장"
    ActivityKind.EMERGENCY -> "긴급"; ActivityKind.SDS -> "SDS"; ActivityKind.ERROR -> "오류"
}

/**
 * «이벤트» 면 — **VM 을 붙이는 껍데기**.
 *
 * @param selectedId 오른쪽 패널에 열린 이벤트(행 강조).
 * @param onSelect 행을 눌렀다 — 이벤트 상세 패널 열기/닫기.
 */
@Composable
internal fun Activity(
    vm: PttActivityViewModel,
    selectedId: Long? = null,
    onSelect: (ActivityRow) -> Unit = {},
    onOpenChannel: (String) -> Unit = {},
    /** 머리 [이력에서 보기] — 끝난 세션의 날짜별 조회는 [이력]. */
    onHistory: () -> Unit = {},
) {
    val rows by vm.allRows.collectAsStateWithLifecycle()
    val pinned by vm.pinned.collectAsStateWithLifecycle()
    val export = com.cims.ue.dispatch.ui.rememberCsvExport()
    ActivityContent(rows, pinned = pinned, selectedId = selectedId, onSelect = onSelect, onOpenChannel = onOpenChannel,
        onHistory = onHistory,
        onExport = { export("ptt-activity-" + SimpleDateFormat("yyyyMMdd-HHmm", Locale.ROOT).format(Date()) + ".csv", vm::csv) })
}

/** «이벤트» 본문 — **순수 컴포저블**(종류·채널 거르기와 따라가기만 제 상태). */
@Composable
fun ActivityContent(
    rows: List<ActivityRow>,
    /** 목록 위 고정 행 — 진행 중인 긴급·임박(거르기와 무관). */
    pinned: List<AlertBannerUi> = emptyList(),
    selectedId: Long? = null,
    onSelect: (ActivityRow) -> Unit = {},
    /** 고정 행 [채널로] — 그 채널 상세로. */
    onOpenChannel: (String) -> Unit = {},
    onHistory: () -> Unit = {},
    onExport: () -> Unit = {},
) {
    val p = Tokens.palette
    var kinds by remember { mutableStateOf(EventKind.entries.toSet()) }
    var hidden by remember { mutableStateOf(emptySet<String>()) }
    var follow by remember { mutableStateOf(true) }
    val channels = remember(rows) { rows.map { it.groupId to it.groupName }.distinctBy { it.first } }
    val shown = remember(rows, kinds, hidden) {
        val allowed = kinds.flatMap { it.kinds }.toSet()
        rows.filter { it.kind in allowed && it.groupId !in hidden }
    }
    Row(Modifier.fillMaxSize()) {
        // ── 거르기 ──
        Column(Modifier.width(220.dp).fillMaxHeight().background(p.canvas)
                .verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 14.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text("종류", fontSize = Type.meta, fontWeight = FontWeight.Bold, color = p.ink2, modifier = Modifier.padding(bottom = 4.dp))
            EventKind.entries.forEach { k ->
                val n = rows.count { it.kind in k.kinds }
                CheckLine(k.label, k in kinds, count = n) { kinds = if (k in kinds) kinds - k else kinds + k }
            }
            HDivider(Modifier.padding(vertical = 8.dp))
            Text("채널", fontSize = Type.meta, fontWeight = FontWeight.Bold, color = p.ink2, modifier = Modifier.padding(bottom = 4.dp))
            if (channels.isEmpty()) Text("아직 없음", fontSize = Type.meta, color = p.faint)
            channels.forEach { (id, name) ->
                CheckLine(name.ifBlank { id }, id !in hidden) { hidden = if (id in hidden) hidden - id else hidden + id }
            }
        }
        VDivider()
        // ── 표 ──
        Column(Modifier.weight(1f).fillMaxHeight()) {
            SectionHead("이벤트 ${shown.size}") {
                Spacer(Modifier.weight(1f))
                PillButton(if (follow) "새 이벤트 따라가기 ✓" else "새 이벤트 따라가기", { follow = !follow }, height = 32.dp)
                // 끝난 세션의 날짜별 조회·녹취는 [이력] 이다 — 여기는 관제사의 작업 메모리(데스크톱 ⑤ 머리와 같다).
                PillButton("이력에서 보기", onHistory, height = 32.dp)
                PillButton("CSV", onExport, height = 32.dp)
            }
            pinned.forEach { PinnedAlertRow(it) { onOpenChannel(it.channelId) } }
            Row(Modifier.fillMaxWidth().height(32.dp)
                    .drawBehind { drawLine(p.ink, Offset(0f, size.height - 0.5f), Offset(size.width, size.height - 0.5f), 1.dp.toPx()) }
                    .padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                listOf("시각" to 76.dp, "채널" to 96.dp, "종류" to 70.dp).forEach { (h, w) ->
                    Text(h, fontSize = Type.micro, color = p.muted, modifier = Modifier.width(w))
                }
                Text("내용", fontSize = Type.micro, color = p.muted)
            }
            if (shown.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(if (rows.isEmpty()) "이벤트 없음 — 발언·입퇴장·긴급·메시지가 여기 쌓입니다" else "거른 조건에 맞는 이벤트가 없습니다",
                        fontSize = Type.body, color = p.muted)
                }
                return@Column
            }
            val list = rememberLazyListState()
            LaunchedEffect(shown.firstOrNull()?.id) { if (follow) list.animateScrollToItem(0) }
            LazyColumn(Modifier.weight(1f), state = list) {
                items(shown, key = { if (it.id != 0L) it.id else it.atMs * 31 + it.text.hashCode() }) { r ->
                    EventRow(r, selected = r.id != 0L && r.id == selectedId) { onSelect(r) }
                }
            }
        }
    }
}

/** 표 한 줄(46) — 시각(고정폭) · 채널 · 종류 라벨 · 내용. */
@Composable
private fun EventRow(r: ActivityRow, selected: Boolean, onClick: () -> Unit) {
    val p = Tokens.palette
    Row(
        Modifier.fillMaxWidth().height(46.dp).background(if (selected) p.fill else p.paper)
            .drawBehind { drawLine(p.hair, Offset(0f, size.height - 0.5f), Offset(size.width, size.height - 0.5f), 1.dp.toPx()) }
            .clickable(onClick = onClick).padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(hhmmss.format(Date(r.atMs)), fontSize = Type.meta, fontFamily = FontFamily.Monospace, modifier = Modifier.width(76.dp))
        Text(r.groupName, fontSize = Type.body, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis,
            modifier = Modifier.width(96.dp).padding(end = 8.dp))
        Box(Modifier.width(70.dp)) { KindLabel(r) }
        Text(r.text, fontSize = Type.body, maxLines = 1, overflow = TextOverflow.Ellipsis,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
            color = if (r.kind == ActivityKind.ERROR) p.emergency else p.ink, modifier = Modifier.weight(1f))
    }
}

@Composable
private fun KindLabel(r: ActivityRow) {
    val p = Tokens.palette
    when {
        r.kind == ActivityKind.EMERGENCY -> Label(kindLabel(r.kind), LabelStyle.STRONG, color = p.emergency)
        r.kind == ActivityKind.ERROR -> Label(kindLabel(r.kind), LabelStyle.OUTLINE, color = p.emergency)
        else -> Label(kindLabel(r.kind), LabelStyle.OUTLINE)
    }
}

/** 체크 한 줄(36) — 거르기 칸. */
@Composable
private fun CheckLine(label: String, on: Boolean, count: Int? = null, onClick: () -> Unit) {
    val p = Tokens.palette
    Row(Modifier.fillMaxWidth().height(36.dp).clickable(onClick = onClick), verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Box(Modifier.size(18.dp).clip(RoundedCornerShape(3.dp)).background(if (on) p.ink else p.paper)
                .border(2.dp, p.ink, RoundedCornerShape(3.dp)))
        Text(label, fontSize = Type.body, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
        if (count != null) Text("$count", fontSize = Type.meta, color = p.muted)
    }
}

/**
 * 고정 행 한 줄 — 경과 · 종류 · «<채널> 진행 중» · [채널로]. 채널 행과 같은 면·낱말(긴급 = 빨강, 임박 = 주황 — §6.2a-1)이라 목록의
 * 그 채널과 한눈에 이어진다. 경과는 배너와 같은 값(조건이 선 때부터)을 1초마다 다시 그린다.
 */
@Composable
private fun PinnedAlertRow(b: AlertBannerUi, onOpen: () -> Unit) {
    val p = Tokens.palette
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(b.channelId, b.sinceMs) {
        while (true) { kotlinx.coroutines.delay(1000); tick++ }
    }
    val emergency = b.kind == AlertKind.EMERGENCY
    val c = if (emergency) p.emergency else p.peril
    Row(Modifier.fillMaxWidth().height(44.dp).background(if (emergency) p.emergencyFill else p.peril.copy(alpha = 0.14f))
            .padding(start = 16.dp, end = 8.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        @Suppress("UNUSED_EXPRESSION") tick     // 1초 틱을 이 조합에 묶는다
        Text(fmtElapsed((System.currentTimeMillis() - b.sinceMs).coerceAtLeast(0)), fontSize = Type.meta,
            fontFamily = FontFamily.Monospace, modifier = Modifier.width(66.dp))
        Label(if (emergency) "긴급" else "임박", LabelStyle.STRONG, color = c)
        Text("${b.title} 진행 중", fontSize = Type.body, fontWeight = FontWeight.Bold, maxLines = 1,
            color = if (emergency) p.emergency else p.ink, modifier = Modifier.weight(1f))
        PillButton("채널로", onOpen, height = 32.dp, color = c)
    }
}

// ── 이벤트 상세 패널 ─────────────────────────────────────────────────────────

/**
 * 이벤트 상세 — 오른쪽 사이드 패널. 그 이벤트의 사실(시각·채널·종류·내용)과 **앞뒤 이벤트**(같은 채널, 앞 둘·뒤 둘)를
 * 보이고, 그 채널로·그 세션의 이력으로 곧바로 간다. 메시지면 [답장] 이 선다.
 */
@Composable
fun EventPanel(
    row: ActivityRow?,
    all: List<ActivityRow>,
    onClose: () -> Unit,
    onOpenChannel: (String) -> Unit,
    onHistory: () -> Unit,
    onReply: (String) -> Unit,
    /** 열 수 있는 채널인가 — 1:1 SDS 의 키는 사람 번호라 채널이 아니다. */
    canOpen: Boolean = true,
) {
    val p = Tokens.palette
    if (row == null) {
        SidePanelFrame(title = "이벤트", tag = "이벤트 상세", onClose = onClose) {
            Box(Modifier.fillMaxSize().padding(24.dp), contentAlignment = Alignment.Center) {
                Text("이벤트가 목록에서 사라졌습니다", fontSize = Type.body, color = p.muted)
            }
        }
        return
    }
    val around = remember(row, all) { aroundOf(row, all) }
    SidePanelFrame(title = "${kindLabel(row.kind)} · ${row.groupName}", tag = "이벤트 상세", onClose = onClose) {
        Column(Modifier.weight(1f).padding(16.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Kv("시각", SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.KOREA).format(Date(row.atMs)), mono = true)
                Kv("채널", row.groupName)
                Kv("종류", kindLabel(row.kind) + if (row.emergency && row.kind != ActivityKind.EMERGENCY) " · 긴급 중" else "")
                Kv("내용", row.text)
            }
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("앞뒤 이벤트", fontSize = Type.meta, fontWeight = FontWeight.Bold, color = p.ink2)
                if (around.size <= 1) Text("같은 채널의 다른 이벤트가 없습니다", fontSize = Type.meta, color = p.faint)
                else around.forEach { a ->
                    val me = a.id == row.id
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        Text(hhmmss.format(Date(a.atMs)), fontSize = Type.meta, fontFamily = FontFamily.Monospace,
                            color = if (me) p.ink else p.muted, modifier = Modifier.width(64.dp))
                        Text(a.text + if (me) " ← 지금 보는 것" else "", fontSize = Type.meta,
                            fontWeight = if (me) FontWeight.Bold else FontWeight.Normal, color = if (me) p.ink else p.muted,
                            maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                }
            }
        }
        Row(Modifier.fillMaxWidth()
                .drawBehind { drawLine(p.divider, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
                .padding(horizontal = 16.dp, vertical = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (row.kind == ActivityKind.SDS && row.groupId.isNotBlank())
                PillButton("답장", { onReply(row.groupId) }, height = 44.dp, filled = true, modifier = Modifier.weight(1f))
            PillButton("채널 열기", { onOpenChannel(row.groupId) }, height = 44.dp, strongBorder = true, enabled = canOpen,
                modifier = Modifier.weight(1f))
            PillButton("이력에서 세션 보기 ›", onHistory, height = 44.dp, modifier = Modifier.weight(1f))
        }
    }
}

/** 같은 채널의 앞 둘 · 그 이벤트 · 뒤 둘 — 시간 순(순수 함수, 시험 대상). */
internal fun aroundOf(row: ActivityRow, all: List<ActivityRow>): List<ActivityRow> {
    val same = all.filter { it.groupId == row.groupId }.sortedBy { it.atMs }
    val i = same.indexOfFirst { it.id == row.id && it.atMs == row.atMs }.takeIf { it >= 0 } ?: return listOf(row)
    return same.subList((i - 2).coerceAtLeast(0), (i + 3).coerceAtMost(same.size))
}

@Composable
private fun Kv(k: String, v: String, mono: Boolean = false) {
    val p = Tokens.palette
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(k, fontSize = Type.body, color = p.muted, modifier = Modifier.width(72.dp))
        Text(v, fontSize = Type.body, fontFamily = if (mono) FontFamily.Monospace else null)
    }
}

// ── 공용 ─────────────────────────────────────────────────────────────────────
@Composable
internal fun StateDot(active: Boolean, speaking: Boolean, emergency: Boolean) {
    com.cims.ue.dispatch.ui.StatusDot(com.cims.ue.dispatch.ui.dotColor(emergency, false, speaking, active))
}
