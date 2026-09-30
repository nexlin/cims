// 채널 머리 모델 · 공용 조작 (android_dispatch_tablet.md §6.3a)
//
// 채널 상세 패널([ChannelPanel])과 긴급·임박 표시 시험이 같은 값을 본다 — 도메인 카드(내 채널·범위 채널)에서 **조작 자격**을
// 잘라 낸 [ChannelHeadUi]. 그 곁에 두 조작 부품: 누르는 동안의 [일제 통화]([BroadcastHoldButton])와 편성 그룹 ⋮([ManageMenu]).
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.CimsFilterChip

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
 * 채널 패널의 머리·조작 줄이 그리는 데 필요한 전부 — 도메인 카드에서 잘라낸 표시용 값.
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

/**
 * [일제 통화] 한 버튼 — 누르고 있는 동안 개시하고 말하며, 놓으면 끝(잠금 발언이면 VM 이 뗌을 무시한다).
 *
 * 발언 바 [PttButton] 과 같은 제스처다 — 취소(화면 이탈·제스처 무효화)에도 `finally` 가 반드시 뗌을 보낸다.
 * 진행 중 통화가 있으면 비활성(진행 중 호는 일제 통화로 바꿀 수 없다 — TS 24.379 §10.1.1.3.1.1 15)).
 */
@Composable
internal fun BroadcastHoldButton(
    enabled: Boolean,
    held: Boolean,
    onDown: () -> Unit,
    onUp: () -> Unit,
    modifier: Modifier = Modifier,
    height: androidx.compose.ui.unit.Dp = 36.dp,
) {
    val p = com.cims.ue.dispatch.ui.Tokens.palette
    // 제스처(pointerInput)는 한 번 걸리므로 재구성 뒤의 최신 콜백을 읽는다 — 옛 카드로 개시하지 않게.
    val down by rememberUpdatedState(onDown)
    val up by rememberUpdatedState(onUp)
    Surface(
        color = when { !enabled -> p.bar; held -> p.ink; else -> p.paper },
        contentColor = when { !enabled -> p.faint; held -> p.onInk; else -> p.ink },
        shape = RoundedCornerShape(height / 2),
        border = if (held) null else BorderStroke(1.5.dp, if (enabled) p.ink else p.line),
        modifier = modifier.height(height).then(
            if (!enabled) Modifier
            else Modifier.pointerInput(Unit) {
                detectTapGestures(onPress = {
                    down()
                    try { tryAwaitRelease() } finally { up() }
                })
            }),
    ) {
        Box(Modifier.padding(horizontal = 12.dp), contentAlignment = Alignment.Center) {
            Text(if (held) "일제 통화 중" else "일제 통화", fontSize = Type.body, fontWeight = FontWeight.Bold)
        }
    }
}

/**
 * 패널 ⋮ — 편성 그룹의 [편집]·[삭제](데스크톱 채널 카드·범위 채널 카드의 같은 버튼, §4.2). 머리를 조작으로 채우지 않게
 * 접어 둔다 — 자주 하는 일(참여·발언 대상·청취)이 앞에 선다. 삭제는 되돌릴 수 없어 여기서 한 번 더 묻는다.
 */
@Composable
internal fun ManageMenu(onEdit: () -> Unit, onDelete: () -> Unit, title: String) {
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
