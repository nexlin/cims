// 채널 상세 — 오른쪽 사이드 패널 (android_dispatch_tablet.md §6.3a)
//
// 채널을 «고른 뒤» 하는 일을 모은다: 참여·나가기·긴급·발언 대상(또는 음소거)·일제 통화·메시지·편집/삭제, 그리고 **누가
// 있나**(접속 / 편성). 보던 면을 떠나지 않는다 — 카드 목록 옆에 밀어 열리고, 같은 카드를 다시 누르면 닫힌다. 청취 범위
// 채널이면 조작은 청취·음량뿐이다(청취는 recvonly 합류라 발언 요청이 서지 않는다, dispatch_center.md §5.6).
package com.cims.ue.dispatch.ui.ptt

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.ui.ForwardPttKeys
import com.cims.ue.dispatch.ui.Initial
import com.cims.ue.dispatch.ui.Label
import com.cims.ue.dispatch.ui.LabelStyle
import com.cims.ue.dispatch.ui.PersonAction
import com.cims.ue.dispatch.ui.PersonEntry
import com.cims.ue.dispatch.ui.PersonMenu
import com.cims.ue.dispatch.ui.PillButton
import com.cims.ue.dispatch.ui.Segmented
import com.cims.ue.dispatch.ui.SidePanelFrame
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type
import com.cims.ue.dispatch.ui.groups.DetailMember
import com.cims.ue.dispatch.ui.groups.PttGroupsViewModel

/** 사람 한 줄 — 접속 목록·편성 목록 공용. */
data class PersonRowUi(
    val number: String,
    val name: String,
    /** «PTT 5002 · 관제과 1팀». */
    val meta: String,
    val isMe: Boolean = false,
    val speaking: Boolean = false,
    val chair: Boolean = false,
    /** 편성됐지만 지금 없다(편성 목록만). */
    val absent: Boolean = false,
)

/** 번호 → «PTT 5002 · 관제과 1팀». 조직은 PTT 주소록의 소속(없으면 번호만). */
internal fun personMeta(number: String, book: DirectoryBook): String {
    val key = DirectoryBook.normalize(number)
    val org = book.entries.firstOrNull { DirectoryBook.normalize(it.msisdn) == key }?.org.orEmpty()
    val orgName = if (org.isBlank()) "" else book.orgPath(org).substringAfterLast(" › ").ifBlank { org }
    return listOf("PTT $number", orgName).filter { it.isNotBlank() }.joinToString(" · ")
}

/**
 * 채널 상세 패널 — **VM 을 붙이는 껍데기**.
 *
 * @param canEdit 편성 그룹을 고칠 수 있다(내 소유·관리 범위) — ⋮ [편집]·[삭제].
 */
@Composable
fun ChannelPanel(
    id: String,
    channels: PttChannelsViewModel,
    scoped: ScopedChannelsViewModel,
    groups: PttGroupsViewModel?,
    canEdit: Boolean,
    pinned: Boolean,
    onPin: () -> Unit,
    onClose: () -> Unit,
    onMessages: (String) -> Unit,
    onEdit: (String) -> Unit,
    onDelete: (String) -> Unit,
    onPerson: (PersonAction, String) -> Unit,
) {
    val mineCards by channels.cards.collectAsStateWithLifecycle()
    val targets by channels.targetIds.collectAsStateWithLifecycle()
    val bcHeld by channels.broadcastHeld.collectAsStateWithLifecycle()
    // 필터·검색 전 전부에서 찾는다 — 검색어가 가린 채널이 «사라졌습니다» 로 보이지 않게.
    val scopedCards by scoped.allCards.collectAsStateWithLifecycle()
    val levels by scoped.rxLevels.collectAsStateWithLifecycle()
    val book by channels.pttBook.collectAsStateWithLifecycle()
    val detail = groups?.detail?.collectAsStateWithLifecycle()?.value.orEmpty()
    val mine = mineCards.firstOrNull { it.id == id }
    val range = scopedCards.firstOrNull { it.id == id }
    val head = channelHead(id, mine, range, targeted = mine != null && mine.id in targets,
        broadcastHeld = mine != null && bcHeld == mine.id)
        .let { if (it.groupId != null) it.copy(canEdit = canEdit) else it }

    // 편성 목록 — 그룹 문서가 준다. 열 때 한 번 받는다(보기 전용 — 폼을 열지 않는다).
    val gid = head.groupId
    LaunchedEffect(gid) { if (gid != null) groups?.selectById(gid) }

    val me = channels.myPttNumber
    val roster = (mine?.group ?: range?.group)?.roster.orEmpty()
    val speaker = mine?.speaker ?: range?.speaker.orEmpty()
    val connected = rosterPreview(roster, speaker, me, channels::nameOf, max = Int.MAX_VALUE).chips.map {
        PersonRowUi(it.number, it.label, personMeta(it.number, book), isMe = it.isMe, speaking = it.speaking)
    }
    val members = detail.map { d: DetailMember ->
        PersonRowUi(d.number, d.name, personMeta(d.number, book), isMe = d.isMe,
            speaking = d.status == DetailMember.SPEAKING, chair = d.isChair, absent = d.absent)
    }

    // 한 줄 요약 — 종류 · 참가 · 경과(또는 상태) · 편성 · 발언(시안 순서).
    val info = when {
        mine != null -> listOfNotNull(
            if (mine.kind == CardKind.MEMBER) "멤버 그룹" else mine.badge,
            mine.participants.takeIf { it > 0 && mine.joined }?.let { "참가 $it" },
            mine.stateText,
            mine.memberCount.takeIf { it > 0 }?.let { "편성 $it" },
            mine.speaker.takeIf { it.isNotEmpty() }?.let { "발언 $it" })
        range != null -> listOfNotNull("청취 범위",
            range.participants.takeIf { it > 0 }?.let { "참가 $it" },
            range.group.memberCount.takeIf { it > 0 }?.let { "편성 $it" },
            range.speaker.takeIf { it.isNotEmpty() }?.let { "발언 $it" })
        else -> emptyList()
    }.joinToString(" · ")

    ChannelPanelContent(
        head = head,
        info = info,
        memberCount = mine?.memberCount ?: range?.group?.memberCount ?: 0,
        emergencyMine = mine?.emergencyMine == true,
        connected = connected, members = members,
        pinned = pinned, onPin = onPin, onClose = onClose,
        onJoin = { mine?.let(channels::join) },
        onJoinEmergency = { mine?.let(channels::joinEmergency) },
        onLeave = { mine?.let(channels::leave) },
        onEmergency = { on -> mine?.let { channels.setEmergency(it, on) } },
        onToggleTarget = { mine?.let { channels.toggleTarget(it.id) } },
        onToggleMute = { mine?.let { channels.toggleMute(it.id) } },
        onToggleListen = { range?.let(scoped::toggleListen) },
        onBroadcastDown = { mine?.let(channels::broadcastGroupDown) },
        onBroadcastUp = channels::broadcastUp,
        listenLevel = range?.listenSession?.callId?.let { levels[it] } ?: 1f,
        onListenLevel = { v -> range?.listenSession?.callId?.let { scoped.setRxLevel(it, v) } },
        onMessages = { gid?.let(onMessages) },
        onEdit = { gid?.let(onEdit) },
        onDelete = { gid?.let(onDelete) },
        personAt = channels::personAt,
        onPerson = onPerson)
}

/** 채널 상세 패널 본문 — **순수 컴포저블**(세그먼트 선택·확인창만 제 상태). */
@Composable
fun ChannelPanelContent(
    head: ChannelHeadUi,
    /** 한 줄 요약 — «멤버 그룹 · 참가 7 · 12:31 · 편성 12 · 발언 김관제». */
    info: String = "",
    memberCount: Int = 0,
    emergencyMine: Boolean = false,
    connected: List<PersonRowUi> = emptyList(),
    members: List<PersonRowUi> = emptyList(),
    pinned: Boolean = false,
    onPin: () -> Unit = {},
    onClose: () -> Unit = {},
    onJoin: () -> Unit = {},
    onJoinEmergency: () -> Unit = {},
    onLeave: () -> Unit = {},
    onEmergency: (Boolean) -> Unit = {},
    onToggleTarget: () -> Unit = {},
    onToggleMute: () -> Unit = {},
    onToggleListen: () -> Unit = {},
    onBroadcastDown: () -> Unit = {},
    onBroadcastUp: () -> Unit = {},
    listenLevel: Float = 1f,
    onListenLevel: (Float) -> Unit = {},
    onMessages: () -> Unit = {},
    onEdit: () -> Unit = {},
    onDelete: () -> Unit = {},
    personAt: (String) -> PersonEntry? = { null },
    onPerson: (PersonAction, String) -> Unit = { _, _ -> },
) {
    val p = Tokens.palette
    var confirmEmergency by remember { mutableStateOf(false) }
    SidePanelFrame(title = head.title, tag = "채널 상세", pinned = pinned, onPin = onPin, onClose = onClose) {
        if (head.gone) {
            Box(Modifier.fillMaxSize().padding(24.dp), contentAlignment = Alignment.Center) {
                Text("채널이 사라졌습니다 — 세션이 끝났거나 편성에서 빠졌습니다", fontSize = Type.body, color = p.muted)
            }
            return@SidePanelFrame
        }
        if (info.isNotEmpty()) Text(info, fontSize = Type.meta, color = p.muted, maxLines = 2,
            overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 10.dp))
        if (head.emergency || head.imminentPeril || head.broadcast) Row(
            Modifier.padding(start = 16.dp, top = 6.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            if (head.emergency) Label("긴급", LabelStyle.STRONG, color = p.emergency)
            else if (head.imminentPeril) Label("임박 위험", LabelStyle.STRONG, color = p.peril)
            if (head.broadcast) Label("일제 통화", LabelStyle.STRONG)
        }

        // 조작 — 그 채널이 지금 받아 줄 수 있는 것만 선다.
        @OptIn(ExperimentalLayoutApi::class)
        FlowRow(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            when {
                head.listening != null -> PillButton(if (head.listening) "청취 중지" else "청취", onToggleListen,
                    filled = !head.listening, strongBorder = head.listening)
                head.joined == true && !head.broadcastHeld -> {
                    PillButton("나가기", onLeave)
                    // 진행 중 긴급 — 올리기는 확인을 받는다(그룹 전원에게 긴급이 선다). 해제는 내가 올린 것만 세운다.
                    if (!head.emergency) PillButton("긴급", { confirmEmergency = true }, color = p.emergency)
                    else if (emergencyMine) PillButton("긴급 해제", { onEmergency(false) }, color = p.emergency)
                }
                head.joined == false && head.isMemberGroup -> {
                    PillButton("참여", onJoin, filled = true)
                    PillButton("긴급 참여", onJoinEmergency, color = p.emergency)
                }
            }
            // [일제 통화] — 누르는 동안 개시+발언, 놓으면 끝. 개시되면 joined 가 되지만 뗄 때까지 같은 자리에 남긴다.
            if (head.isMemberGroup && (head.joined == false || head.broadcastHeld))
                BroadcastHoldButton(enabled = head.canBroadcast || head.broadcastHeld, held = head.broadcastHeld,
                    onDown = onBroadcastDown, onUp = onBroadcastUp)
            if (head.canTarget) PillButton(if (head.targeted) "✓ 발언 대상" else "발언 대상", onToggleTarget,
                filled = head.targeted)
            head.muted?.let { m ->
                PillButton(if (m) "음소거 중" else "음소거", onToggleMute, filled = m, color = if (m) p.emergency else null)
            }
            // 이 채널의 글 — 멤버 그룹만(그룹 SDS 는 멤버에게만 열린다, TS 24.282). 미읽음이 있으면 수를 단다.
            if (head.groupId != null && head.isMemberGroup)
                PillButton(if (head.unread > 0) "메시지 ${head.unread} ›" else "메시지 ›", onMessages)
            if (head.canEdit) ManageMenu(onEdit = onEdit, onDelete = onDelete, title = head.title)
        }
        if (head.listening == true) {
            Text("청취 전용 — 발언 요청 불가", fontSize = Type.meta, color = p.muted,
                modifier = Modifier.padding(horizontal = 16.dp))
            Box(Modifier.padding(start = 16.dp, end = 12.dp)) { com.cims.ue.dispatch.ui.call.RxLevelRow(listenLevel, onListenLevel) }
        }

        // 누가 있나 — 접속(지금) / 편성(구성 × 지금 상태). 편성이 없는 채널(개별·애드혹)은 접속만.
        var tab by remember(head.id) { mutableIntStateOf(0) }
        val hasRoster = head.groupId != null
        if (hasRoster) Segmented(
            options = listOf("접속 ${connected.size}", "편성 ${if (members.isNotEmpty()) members.size else memberCount}"),
            selected = tab, onSelect = { tab = it }, height = 34.dp,
            modifier = Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 10.dp))
        val rows = if (hasRoster && tab == 1) members else connected
        if (rows.isEmpty()) Box(Modifier.fillMaxWidth().padding(24.dp), contentAlignment = Alignment.Center) {
            Text(if (hasRoster && tab == 1) "편성 명단을 받는 중입니다" else "접속한 사람이 없습니다",
                fontSize = Type.body, color = p.muted)
        } else LazyColumn(Modifier.weight(1f)) {
            items(rows, key = { it.number }) { r -> PersonRow(r, personAt, onPerson) }
        }
    }

    if (confirmEmergency) AlertDialog(
        onDismissRequest = { confirmEmergency = false },
        title = { ForwardPttKeys(); Text("긴급으로 올립니다") },
        text = { Text("«${head.title}» 의 진행 중 통화를 긴급으로 올립니다. 참여자 전원에게 긴급이 표시됩니다.") },
        confirmButton = {
            TextButton(onClick = { confirmEmergency = false; onEmergency(true) }) { Text("긴급", color = p.emergency) }
        },
        dismissButton = { TextButton(onClick = { confirmEmergency = false }) { Text("취소") } })
}

/** 사람 한 줄(50) — 머리글자 · 이름·소속 · [개별]·[SDS]. 줄을 누르면 사람 메뉴(통화·문자·기록…). */
@Composable
private fun PersonRow(r: PersonRowUi, personAt: (String) -> PersonEntry?, onPerson: (PersonAction, String) -> Unit) {
    val p = Tokens.palette
    var menu by remember { mutableStateOf(false) }
    Box {
        Row(
            Modifier.fillMaxWidth().height(50.dp)
                .drawBehind { drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
                .clickable(enabled = !r.isMe) { menu = true }
                .padding(start = 16.dp, end = 12.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Initial(r.name, strong = r.isMe)
            Column(Modifier.weight(1f)) {
                Text(r.name + (if (r.isMe) " (나)" else "") + (if (r.speaking) " · 발언 중" else ""),
                    fontSize = Type.strong, maxLines = 1, overflow = TextOverflow.Ellipsis,
                    fontWeight = if (r.isMe || r.speaking) FontWeight.Bold else FontWeight.Medium,
                    color = if (r.absent) p.faint else p.ink)
                Text(r.meta + (if (r.absent) " · 미참가" else ""), fontSize = Type.meta, color = p.muted,
                    maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            if (r.chair) Label("의장")
            if (!r.isMe) {
                PillButton("개별", { onPerson(PersonAction.PRIVATE_CALL, r.number) }, height = 32.dp)
                PillButton("SDS", { onPerson(PersonAction.SDS, r.number) }, height = 32.dp)
            }
        }
        if (menu) PersonMenu(person = personAt(r.number), expanded = true, onDismiss = { menu = false },
            onPick = { a, n -> menu = false; onPerson(a, n) })
    }
}
