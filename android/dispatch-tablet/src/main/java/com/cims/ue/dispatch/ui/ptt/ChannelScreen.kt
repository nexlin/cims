// [채널] 화면 — 한 채널의 전부 (android_dispatch_tablet.md §6.3a)
//
// [무전] 목록에서 채널을 누르면 열린다. 데스크톱이 ①②③④⑤ 를 한 화면에 펴 놓고 «포커스» 로 묶던 것을,
// 태블릿에서는 **화면 하나가 곧 포커스**가 되게 한다 — 보고 있는 채널이 화면이므로 «지금 뭘 보고 있나» 를
// 헷갈릴 자리가 없다.
//
// 조작(참여·긴급·나가기·발언 대상·음소거·청취)은 **머리에 모은다.** 목록의 행이 얇을 수 있는 이유가 이것이다.
// 아래 세 면은 좌우 스와이프로 넘긴다 — 셋을 동시에 펴면 각각 6줄이 되어 아무것도 못 읽는다.
@file:OptIn(ExperimentalFoundationApi::class)

package com.cims.ue.dispatch.ui.ptt

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.ui.PerilAmber
import com.cims.ue.dispatch.ui.Tag
import com.cims.ue.dispatch.ui.PersonAction
import com.cims.ue.dispatch.ui.PersonEntry
import com.cims.ue.dispatch.ui.PersonMenu
import com.cims.ue.dispatch.ui.Type

/**
 * @param id  [무전] 목록에서 연 채널 id — 내 채널(그룹·개별 통화·애드혹 그룹 통화) 또는 범위 채널.
 * @param onShowRoster 편성 전원 보기 — [PTT 그룹] 화면 상세로(§6.12).
 */
@Composable
fun ChannelScreen(
    id: String,
    channels: PttChannelsViewModel,
    scoped: ScopedChannelsViewModel,
    onBack: () -> Unit,
    onShowRoster: (String) -> Unit,
    /** 로스터 행의 사람 메뉴가 고른 행동(§6.2f) — 셸이 화면을 잇는다. */
    onPerson: (PersonAction, String) -> Unit = { _, _ -> },
    /** 이 그룹을 고칠 수 있는가 — 내 소유이거나 서버가 관리 범위로 준 그룹(셸이 판정한다). */
    canEdit: Boolean = false,
    /** 머리 [메시지] — 그룹 SDS 스레드로. */
    onMessages: (String) -> Unit = {},
    /** 머리 ⋮ [편집] — [PTT 그룹] 편집 폼으로. */
    onEdit: (String) -> Unit = {},
    /** 머리 ⋮ [삭제] — 확인은 여기서 받는다. */
    onDelete: (String) -> Unit = {},
    modifier: Modifier = Modifier,
) {
    val mineCards by channels.cards.collectAsStateWithLifecycle()
    // 범위 채널은 필터·검색 전 전부에서 찾는다 — 목록 검색이 가린 채널도 배너·검색에서 열린다.
    val scopedCards by scoped.allCards.collectAsStateWithLifecycle()
    val targets by channels.targetIds.collectAsStateWithLifecycle()
    val bcHeld by channels.broadcastHeld.collectAsStateWithLifecycle()
    val mine = mineCards.firstOrNull { it.id == id }
    val range = scopedCards.firstOrNull { it.id == id }
    val levels by scoped.rxLevels.collectAsStateWithLifecycle()

    ChannelScreenContent(
        head = channelHead(id, mine, range, targeted = mine != null && mine.id in targets,
                           broadcastHeld = mine != null && bcHeld == mine.id)
            .let { if (it.groupId != null) it.copy(canEdit = canEdit) else it },
        onBack = onBack,
        onShowRoster = onShowRoster,
        onMessages = onMessages,
        onEdit = onEdit,
        onDelete = onDelete,
        listenLevel = range?.listenSession?.callId?.let { levels[it] } ?: 1f,
        onListenLevel = { v -> range?.listenSession?.callId?.let { scoped.setRxLevel(it, v) } },
        onJoin = { mine?.let(channels::join) },
        onJoinEmergency = { mine?.let(channels::joinEmergency) },
        onLeave = { mine?.let(channels::leave) },
        onToggleTarget = { mine?.let { channels.toggleTarget(it.id) } },
        onToggleMute = { mine?.let { channels.toggleMute(it.id) } },
        onToggleListen = { range?.let(scoped::toggleListen) },
        onBroadcastDown = { mine?.let(channels::broadcastGroupDown) },
        onBroadcastUp = channels::broadcastUp,
        roster = (mine?.group ?: range?.group)?.roster.orEmpty(),
        speaker = mine?.speaker ?: range?.speaker.orEmpty(),
        me = channels.myPttNumber,
        nameOf = channels::nameOf,
        personAt = channels::personAt,
        onPerson = onPerson,
        modifier = modifier)
}

/**
 * 채널 화면 머리가 그리는 데 필요한 전부 — 도메인 카드에서 잘라낸 표시용 값.
 *
 * 내 채널과 범위 채널은 **조작이 다르다**(참여/긴급/나가기·발언 대상 vs 청취). 그 차이를 화면이 아니라
 * 이 값이 들고 있게 해서, 화면은 «무엇을 그릴지» 만 보게 한다.
 */
data class ChannelHeadUi(
    val id: String,
    val title: String,
    val badge: String = "",
    val subtitle: String = "",
    val emergency: Boolean = false,
    /** 임박 위험 — 긴급이 아닐 때만(서열 긴급 › 임박). */
    val imminentPeril: Boolean = false,
    val groupId: String? = null,
    /** 내 채널 — 참여 중인가(나가기 vs 참여·긴급). null = 범위 채널. */
    val joined: Boolean? = null,
    val isMemberGroup: Boolean = false,
    val canTarget: Boolean = false,
    val targeted: Boolean = false,
    /** [일제 통화] — 멤버 편성 그룹에 진행 중 통화가 없을 때만([ChannelCard.canBroadcast]). */
    val canBroadcast: Boolean = false,
    /** [일제 통화] 를 누르고 있다(잠금 발언이면 켜 두었다) — 개시되면 [joined] 가 되지만 버튼은 뗄 때까지 남는다. */
    val broadcastHeld: Boolean = false,
    /** 일제 통화 세션 — 머리 배지 «일제». */
    val broadcast: Boolean = false,
    /** 음소거 — 참여 중인 전이중 개별 통화만(발언 대상 대신). null = 없음. */
    val muted: Boolean? = null,
    val unread: Int = 0,
    /** 범위 채널 — 청취 중인가. null = 내 채널. */
    val listening: Boolean? = null,
    /** 편성 그룹을 고칠 수 있다(내 소유·관리 범위) — 머리 ⋮ [편집]·[삭제]. 개별 통화·애드혹은 편성이 없어 늘 false. */
    val canEdit: Boolean = false,
    /** 청취 leg — 청취 중일 때만. 머리의 음량 막대가 이 호의 수신 음량을 바꾼다. */
    val listenCallId: Int? = null,
    /** 둘 다 없다 = 채널이 사라졌다(세션 종료·편성 제외). */
    val gone: Boolean = false,
)

internal fun channelHead(
    id: String,
    mine: ChannelCard?,
    range: ScopedCard?,
    targeted: Boolean,
    broadcastHeld: Boolean = false,
): ChannelHeadUi = when {
    mine != null -> ChannelHeadUi(
        id = id, title = mine.title, badge = mine.badge,
        subtitle = listOfNotNull(
            mine.participants.takeIf { it > 0 }?.let { "참가 $it" },
            mine.line2.takeIf { it.isNotEmpty() },
            mine.stateText).joinToString(" · "),
        emergency = mine.emergency, imminentPeril = mine.imminentPeril && !mine.emergency,
        groupId = mine.group?.id,
        joined = mine.joined, isMemberGroup = mine.kind == CardKind.MEMBER,
        canTarget = mine.canCheck, targeted = targeted,
        canBroadcast = mine.canBroadcast, broadcastHeld = broadcastHeld, broadcast = mine.isBroadcast,
        muted = mine.muted.takeIf { mine.canMute }, unread = mine.unread)
    range != null -> ChannelHeadUi(
        id = id, title = range.title, badge = "범위",
        subtitle = listOfNotNull(
            range.participants.takeIf { it > 0 }?.let { "참가 $it" },
            range.line2.takeIf { it.isNotEmpty() }).joinToString(" · "),
        emergency = range.emergency, imminentPeril = range.imminentPeril && !range.emergency,
        groupId = range.group.id, listening = range.listening,
        listenCallId = range.listenSession?.takeIf { range.listening }?.callId)
    else -> ChannelHeadUi(id = id, title = id, gone = true)
}

/** [채널] 본문 — **순수 컴포저블**. 아래 세 면은 호출자가 넘긴다(메시지·이벤트는 제 VM 을 쓴다). */
@Composable
fun ChannelScreenContent(
    head: ChannelHeadUi,
    onBack: () -> Unit = {},
    onShowRoster: (String) -> Unit = {},
    onMessages: (String) -> Unit = {},
    onEdit: (String) -> Unit = {},
    onDelete: (String) -> Unit = {},
    /** 청취 leg 의 수신 음량(0~2) — 청취 중일 때 머리에 막대가 선다. */
    listenLevel: Float = 1f,
    onListenLevel: (Float) -> Unit = {},
    onJoin: () -> Unit = {},
    onJoinEmergency: () -> Unit = {},
    onLeave: () -> Unit = {},
    onToggleTarget: () -> Unit = {},
    onToggleMute: () -> Unit = {},
    onToggleListen: () -> Unit = {},
    onBroadcastDown: () -> Unit = {},
    onBroadcastUp: () -> Unit = {},
    roster: List<com.cims.ue.sdk.RosterEntry> = emptyList(),
    speaker: String = "",
    me: String = "",
    nameOf: (String) -> String = { "" },
    personAt: (String) -> PersonEntry? = { null },
    onPerson: (PersonAction, String) -> Unit = { _, _ -> },
    modifier: Modifier = Modifier,
) {
    Column(modifier.fillMaxSize()) {
        // ── 머리 — 조작을 여기 모은다. 목록의 행이 얇을 수 있는 이유가 이것이다(§6.3a) ──
        Surface(color = when {
            head.emergency -> MaterialTheme.colorScheme.errorContainer
            head.imminentPeril -> PerilAmber.copy(alpha = 0.22f)
            else -> MaterialTheme.colorScheme.surfaceVariant
        }) {
            Column(Modifier.fillMaxWidth().padding(start = 4.dp, end = 8.dp, top = 4.dp, bottom = 6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "뒤로")
                    }
                    Text(head.title, fontSize = Type.title,
                        fontWeight = FontWeight.Bold, maxLines = 1, modifier = Modifier.weight(1f))
                    if (head.broadcast) Tag("일제")
                    if (head.badge.isNotEmpty()) Tag(head.badge)
                    // 전역 배너(§6.2a-1)와 같은 낱말 — 배너에서 [채널로 이동] 한 뒤 «왜 여기 왔나» 가 머리에 남는다.
                    if (head.emergency) Tag("긴급", MaterialTheme.colorScheme.error)
                    else if (head.imminentPeril) Tag("임박 위험", PerilAmber)
                }
                if (head.subtitle.isNotEmpty()) Text(head.subtitle, fontSize = Type.meta,
                    modifier = Modifier.padding(start = 12.dp))

                Row(Modifier.padding(start = 8.dp, top = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    when {
                        head.joined == true && !head.broadcastHeld -> TextButton(onClick = onLeave) { Text("나가기") }
                        head.joined == false && head.isMemberGroup -> {
                            Button(onClick = onJoin) { Text("참여") }
                            TextButton(onClick = onJoinEmergency) { Text("긴급") }
                        }
                    }
                    // [일제 통화] — 누르는 동안 개시+발언, 놓으면 끝. 개시되면 joined 가 되지만 뗄 때까지 같은 자리에 남긴다
                    //   (버튼이 사라지면 제스처가 취소돼 즉시 끝난다).
                    if (head.isMemberGroup && (head.joined == false || head.broadcastHeld))
                        BroadcastHoldButton(enabled = head.canBroadcast || head.broadcastHeld, held = head.broadcastHeld,
                                            onDown = onBroadcastDown, onUp = onBroadcastUp)
                    if (head.canTarget) FilterChip(
                        selected = head.targeted,
                        onClick = onToggleTarget,
                        label = { Text(if (head.targeted) "발언 대상 ✓" else "발언 대상", fontSize = Type.meta) })
                    // 켜져 있으면 경고색 — 목록 행의 음소거 토글과 같은 색이다. 긴급 머리(errorContainer) 위에서도 읽힌다.
                    head.muted?.let { m ->
                        FilterChip(
                            selected = m,
                            onClick = onToggleMute,
                            leadingIcon = {
                                Icon(if (m) Icons.Filled.MicOff else Icons.Filled.Mic, contentDescription = null,
                                    modifier = Modifier.size(FilterChipDefaults.IconSize))
                            },
                            label = { Text(if (m) "음소거 중" else "음소거", fontSize = Type.meta) },
                            colors = FilterChipDefaults.filterChipColors(
                                selectedContainerColor = MaterialTheme.colorScheme.error,
                                selectedLabelColor = MaterialTheme.colorScheme.onError,
                                selectedLeadingIconColor = MaterialTheme.colorScheme.onError))
                    }
                    if (head.listening != null) TextButton(onClick = onToggleListen) {
                        Text(if (head.listening) "청취 중지" else "청취")
                    }
                    // 청취는 `listenOnly` 합류라 서버가 Floor Taken 의 `permissionToRequest=0` 을 준다 —
                    //   발언 버튼이 안 먹는 이유를 **누르기 전에** 적는다(dispatch_center.md §5.6).
                    if (head.listening == true) Tag("청취 전용 — 발언 요청 불가",
                        MaterialTheme.colorScheme.onSurfaceVariant, leading = 0)
                    Spacer(Modifier.weight(1f))
                    // 이 채널의 글 — 멤버 그룹만(그룹 SDS 는 멤버에게만 열린다, TS 24.282). 미읽음이 있으면 수를 단다.
                    val gid = head.groupId
                    if (gid != null && head.isMemberGroup) TextButton(onClick = { onMessages(gid) }) {
                        Text(if (head.unread > 0) "메시지 ${head.unread}" else "메시지", fontSize = Type.meta)
                    }
                    if (gid != null) {
                        TextButton(onClick = { onShowRoster(gid) }) { Text("편성 전원", fontSize = Type.meta) }
                        if (head.canEdit) ManageMenu(onEdit = { onEdit(gid) }, onDelete = { onDelete(gid) },
                            title = head.title)
                    }
                }
                // 청취 음량 — 여러 채널을 같이 들을 때 이 채널만 줄이거나 키운다(데스크톱 청취 창 [음량]).
                if (head.listenCallId != null) Box(Modifier.padding(start = 12.dp, end = 4.dp)) {
                    com.cims.ue.dispatch.ui.call.RxLevelRow(listenLevel, onListenLevel)
                }
            }
        }

        if (head.gone) {
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Text("채널이 사라졌습니다 — 세션이 끝났거나 편성에서 빠졌습니다",
                    fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center)
            }
            return@Column
        }

        // 로스터 한 장 — 메시지·이벤트는 [무전] 메뉴의 면이 되어 여기서 뺐다(§6.3).
        //   두 곳에 두면 «어느 쪽이 지금 채널 것인가» 가 흐려진다.
        RosterPage(roster = roster, speaker = speaker, me = me, nameOf = nameOf,
            personAt = personAt, onPerson = onPerson, modifier = Modifier.weight(1f))
    }
}

/**
 * [일제 통화] 한 버튼 — 누르고 있는 동안 개시하고 말하며, 놓으면 끝(잠금 발언이면 VM 이 뗌을 무시한다).
 *
 * 발언 바 [PttButton] 과 같은 제스처다 — 취소(화면 이탈·제스처 무효화)에도 `finally` 가 반드시 뗌을 보낸다.
 * 진행 중 통화가 있으면 비활성(진행 중 호는 일제 통화로 바꿀 수 없다 — TS 24.379 §10.1.1.3.1.1 15)).
 */
@Composable
internal fun BroadcastHoldButton(enabled: Boolean, held: Boolean, onDown: () -> Unit, onUp: () -> Unit) {
    val scheme = MaterialTheme.colorScheme
    // 제스처(pointerInput)는 한 번 걸리므로 재구성 뒤의 최신 콜백을 읽는다 — 옛 카드로 개시하지 않게.
    val down by rememberUpdatedState(onDown)
    val up by rememberUpdatedState(onUp)
    Surface(
        color = when { !enabled -> scheme.surfaceVariant; held -> scheme.primary; else -> scheme.secondaryContainer },
        contentColor = when { !enabled -> scheme.onSurfaceVariant; held -> scheme.onPrimary; else -> scheme.onSecondaryContainer },
        shape = RoundedCornerShape(20.dp),
        border = if (enabled) null else BorderStroke(1.dp, scheme.outline),
        modifier = Modifier.heightIn(min = 40.dp).then(
            if (!enabled) Modifier
            else Modifier.pointerInput(Unit) {
                detectTapGestures(onPress = {
                    down()
                    try { tryAwaitRelease() } finally { up() }
                })
            }),
    ) {
        Text(if (held) "일제 통화 중" else "일제 통화", fontSize = Type.strong, fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp))
    }
}

/**
 * 머리 ⋮ — 편성 그룹의 [편집]·[삭제](데스크톱 채널 카드·범위 채널 카드의 같은 버튼, §4.2). 머리를 조작으로 채우지 않게
 * 접어 둔다 — 자주 하는 일(참여·발언 대상·청취)이 앞에 선다. 삭제는 되돌릴 수 없어 여기서 한 번 더 묻는다.
 */
@Composable
private fun ManageMenu(onEdit: () -> Unit, onDelete: () -> Unit, title: String) {
    var open by remember { mutableStateOf(false) }
    var confirm by remember { mutableStateOf(false) }
    Box {
        IconButton(onClick = { open = true }) { Icon(Icons.Filled.MoreVert, contentDescription = "그룹 관리") }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            DropdownMenuItem(text = { Text("편집") }, onClick = { open = false; onEdit() })
            DropdownMenuItem(text = { Text("삭제", color = MaterialTheme.colorScheme.error) },
                onClick = { open = false; confirm = true })
        }
    }
    if (confirm) AlertDialog(
        onDismissRequest = { confirm = false },
        title = { com.cims.ue.dispatch.ui.ForwardPttKeys(); Text("그룹 삭제") },
        text = { Text("«$title» 을(를) 지웁니다. 멤버 전원의 단말에서도 사라집니다. 되돌릴 수 없습니다.") },
        confirmButton = {
            TextButton(onClick = { confirm = false; onDelete() }) {
                Text("삭제", color = MaterialTheme.colorScheme.error)
            }
        },
        dismissButton = { TextButton(onClick = { confirm = false }) { Text("취소") } })
}

/**
 * 로스터 면 — **지금 접속한 사람 전부**(잘리지 않는다).
 *
 * 목록 행의 미리보기와 달리 여기는 높이가 넉넉하므로 접을 이유가 없다. 순서는 미리보기와 같은 규칙을
 * 쓴다(`rosterPreview` — 발언자 → 나 → 서버 순서). 같은 규칙을 두 번 적지 않는다.
 * «편성됐지만 지금 없는 사람» 은 여기 없다 — 그건 머리의 [편성 전원] 이 답한다(§6.12).
 */
@Composable
private fun RosterPage(
    roster: List<com.cims.ue.sdk.RosterEntry>,
    speaker: String,
    me: String,
    nameOf: (String) -> String,
    personAt: (String) -> PersonEntry?,
    onPerson: (PersonAction, String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val chips = rosterPreview(roster, speaker, me, nameOf, max = Int.MAX_VALUE).chips
    // 열린 메뉴는 한 번에 하나 — 번호로 든다(③ 그룹원 띠·⑥ 행과 같은 규칙).
    var menuFor by remember { mutableStateOf<String?>(null) }
    if (chips.isEmpty()) {
        Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text("접속한 사람이 없습니다", fontSize = Type.body,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        return
    }
    LazyColumn(modifier.fillMaxSize()) {
        items(chips, key = { it.number }) { c ->
            // 행 탭 = 사람 메뉴 — 그 사람과 개별 통화·SDS·통화·문자를 곧바로(데스크톱 로스터 칩의 사람 메뉴, §6.2f).
            Box {
                Row(Modifier.fillMaxWidth()
                        .clickable(enabled = !c.isMe) { menuFor = c.number }
                        .padding(horizontal = 12.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Text(c.label + if (c.isMe) " (나)" else "", fontSize = Type.body,
                        fontWeight = if (c.speaking) FontWeight.Bold else FontWeight.Normal,
                        modifier = Modifier.weight(1f), maxLines = 1)
                    Text(c.number, fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (c.speaking) Tag("발언 중")
                }
                if (menuFor == c.number) PersonMenu(
                    person = personAt(c.number), expanded = true,
                    onDismiss = { menuFor = null },
                    onPick = { a, n -> menuFor = null; onPerson(a, n) })
            }
            HorizontalDivider()
        }
    }
}
