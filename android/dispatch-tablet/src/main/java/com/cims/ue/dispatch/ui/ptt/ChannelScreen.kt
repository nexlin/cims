// [채널] 화면 — 한 채널의 전부 (android_dispatch_tablet.md §6.3a)
//
// [무전] 목록에서 채널을 누르면 열린다. 데스크톱이 ①②③④⑤ 를 한 화면에 펴 놓고 «포커스» 로 묶던 것을,
// 태블릿에서는 **화면 하나가 곧 포커스**가 되게 한다 — 보고 있는 채널이 화면이므로 «지금 뭘 보고 있나» 를
// 헷갈릴 자리가 없다.
//
// 조작(참여·긴급·나가기·발언 대상·청취)은 **머리에 모은다.** 목록의 행이 얇을 수 있는 이유가 이것이다.
// 아래 세 면은 좌우 스와이프로 넘긴다 — 셋을 동시에 펴면 각각 6줄이 되어 아무것도 못 읽는다.
@file:OptIn(ExperimentalFoundationApi::class)

package com.cims.ue.dispatch.ui.ptt

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.ui.Tag
import com.cims.ue.dispatch.ui.Type

/**
 * @param id  [무전] 목록에서 연 채널 id — 내 채널(그룹·개인 통화·임시 그룹 통화) 또는 범위 채널.
 * @param onShowRoster 편성 전원 보기 — [PTT 그룹] 화면 상세로(§6.12).
 */
@Composable
fun ChannelScreen(
    id: String,
    channels: PttChannelsViewModel,
    scoped: ScopedChannelsViewModel,
    onBack: () -> Unit,
    onShowRoster: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val mineCards by channels.cards.collectAsStateWithLifecycle()
    val scopedCards by scoped.cards.collectAsStateWithLifecycle()
    val targets by channels.targetIds.collectAsStateWithLifecycle()
    val bcHeld by channels.broadcastHeld.collectAsStateWithLifecycle()
    val mine = mineCards.firstOrNull { it.id == id }
    val range = scopedCards.firstOrNull { it.id == id }

    ChannelScreenContent(
        head = channelHead(id, mine, range, targeted = mine != null && mine.id in targets,
                           broadcastHeld = mine != null && bcHeld == mine.id),
        onBack = onBack,
        onShowRoster = onShowRoster,
        onJoin = { mine?.let(channels::join) },
        onJoinEmergency = { mine?.let(channels::joinEmergency) },
        onLeave = { mine?.let(channels::leave) },
        onToggleTarget = { mine?.let { channels.toggleTarget(it.id) } },
        onToggleListen = { range?.let(scoped::toggleListen) },
        onBroadcastDown = { mine?.let(channels::broadcastGroupDown) },
        onBroadcastUp = channels::broadcastUp,
        roster = (mine?.group ?: range?.group)?.roster.orEmpty(),
        speaker = mine?.speaker ?: range?.speaker.orEmpty(),
        me = channels.myPttNumber,
        nameOf = channels::nameOf,
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
    val unread: Int = 0,
    /** 범위 채널 — 청취 중인가. null = 내 채널. */
    val listening: Boolean? = null,
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
        emergency = mine.emergency, groupId = mine.group?.id,
        joined = mine.joined, isMemberGroup = mine.kind == CardKind.MEMBER,
        canTarget = mine.canCheck, targeted = targeted,
        canBroadcast = mine.canBroadcast, broadcastHeld = broadcastHeld, broadcast = mine.isBroadcast,
        unread = mine.unread)
    range != null -> ChannelHeadUi(
        id = id, title = range.title, badge = "범위",
        subtitle = listOfNotNull(
            range.participants.takeIf { it > 0 }?.let { "참가 $it" },
            range.line2.takeIf { it.isNotEmpty() }).joinToString(" · "),
        groupId = range.group.id, listening = range.listening)
    else -> ChannelHeadUi(id = id, title = id, gone = true)
}

/** [채널] 본문 — **순수 컴포저블**. 아래 세 면은 호출자가 넘긴다(메시지·이벤트는 제 VM 을 쓴다). */
@Composable
fun ChannelScreenContent(
    head: ChannelHeadUi,
    onBack: () -> Unit = {},
    onShowRoster: (String) -> Unit = {},
    onJoin: () -> Unit = {},
    onJoinEmergency: () -> Unit = {},
    onLeave: () -> Unit = {},
    onToggleTarget: () -> Unit = {},
    onToggleListen: () -> Unit = {},
    onBroadcastDown: () -> Unit = {},
    onBroadcastUp: () -> Unit = {},
    roster: List<com.cims.ue.sdk.RosterEntry> = emptyList(),
    speaker: String = "",
    me: String = "",
    nameOf: (String) -> String = { "" },
    modifier: Modifier = Modifier,
) {
    Column(modifier.fillMaxSize()) {
        // ── 머리 — 조작을 여기 모은다. 목록의 행이 얇을 수 있는 이유가 이것이다(§6.3a) ──
        Surface(color = if (head.emergency) MaterialTheme.colorScheme.errorContainer
                        else MaterialTheme.colorScheme.surfaceVariant) {
            Column(Modifier.fillMaxWidth().padding(start = 4.dp, end = 8.dp, top = 4.dp, bottom = 6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "뒤로")
                    }
                    Text(head.title, fontSize = Type.title,
                        fontWeight = FontWeight.Bold, maxLines = 1, modifier = Modifier.weight(1f))
                    if (head.broadcast) Tag("일제")
                    if (head.badge.isNotEmpty()) Tag(head.badge)
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
                    if (head.listening != null) TextButton(onClick = onToggleListen) {
                        Text(if (head.listening) "청취 중지" else "청취")
                    }
                    // 청취는 `listenOnly` 합류라 서버가 Floor Taken 의 `permissionToRequest=0` 을 준다 —
                    //   발언 버튼이 안 먹는 이유를 **누르기 전에** 적는다(dispatch_center.md §5.5).
                    if (head.listening == true) Tag("청취 전용 — 발언 요청 불가",
                        MaterialTheme.colorScheme.onSurfaceVariant, leading = 0)
                    Spacer(Modifier.weight(1f))
                    head.groupId?.let { gid ->
                        TextButton(onClick = { onShowRoster(gid) }) { Text("편성 전원", fontSize = Type.meta) }
                    }
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
            modifier = Modifier.weight(1f))
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
    modifier: Modifier = Modifier,
) {
    val chips = rosterPreview(roster, speaker, me, nameOf, max = Int.MAX_VALUE).chips
    if (chips.isEmpty()) {
        Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text("접속한 사람이 없습니다", fontSize = Type.body,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        return
    }
    LazyColumn(modifier.fillMaxSize()) {
        items(chips, key = { it.number }) { c ->
            Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Text(c.label + if (c.isMe) " (나)" else "", fontSize = Type.body,
                    fontWeight = if (c.speaking) FontWeight.Bold else FontWeight.Normal,
                    modifier = Modifier.weight(1f), maxLines = 1)
                Text(c.number, fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (c.speaking) Tag("발언 중")
            }
            HorizontalDivider()
        }
    }
}
