// 채널 추가 — 오른쪽 사이드 패널 (android_dispatch_tablet.md §6.2e·§6.3a·§6.12)
//
// «채널» 면의 [채널 추가하기] 타일이 연다. PTT 주소록의 사람을 골라 **세 가지**를 한다 — 셋 다 내 채널에 카드 한 장을 더하는 일이다:
//   · **개별 통화** — 1명(또는 입력한 번호)과 1:1(TS 24.379 private call — 반이중이 기본, 전이중은 고를 때만).
//   · **애드혹 통화** — 고른 사람들과 지금 한 번(TS 24.379 ad hoc group call, 서버에 편성되지 않는다). [일제 통화] 는 고른
//     사람들에게 누르는 동안 나만 말하는 애드혹 일제 통화다(§17.2.2.1.1 9)) — 놓으면 끝난다.
//   · **그룹 추가** — 두고 쓰는 편성(GMS 그룹 생성 — TS 24.481). 패널 안에서 한 겹 들어간 폼([NewGroupPanel])이 받는다.
// 한 자리에 두는 이유 — 사람을 고르는 자리에서 곧바로 거는 것이 관제사의 순서다. **고르기가 먼저**고, 무엇을 할지는 아래 줄에서
// 고른다(모드를 먼저 고르게 하면 모드를 바꿀 때마다 고른 사람이 사라진다).
//
// 상태 칸은 **아는 것만** 적는다 — 로스터에 접속으로 잡힌 사람만 «접속». 주소록에는 등록 여부가 없어 «오프라인» 을
// 단정하지 않는다(비워 둔다).
package com.cims.ue.dispatch.ui.ptt

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.ui.FilterPill
import com.cims.ue.dispatch.ui.PersonAction
import com.cims.ue.dispatch.ui.PersonEntry
import com.cims.ue.dispatch.ui.PersonMenu
import com.cims.ue.dispatch.ui.Pill
import com.cims.ue.dispatch.ui.PillButton
import com.cims.ue.dispatch.ui.SidePanelFrame
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type

/** 사용자 한 줄. */
data class UserRowUi(
    val number: String,
    val name: String,
    /** 조직(잎 이름) — 거르기 칩의 값. */
    val org: String,
    val meta: String,
    /** «접속» — 로스터가 아는 경우만. 모르면 빈 값. */
    val status: String = "",
)

/**
 * 주소록 → 사용자 줄(순수 함수, 시험 대상). 나는 뺀다 — 나를 골라 통화를 걸거나 그룹에 두 번 넣을 일이 없다.
 * 상태는 [present](로스터에 접속으로 잡힌 번호, 정규형)가 준다.
 */
internal fun userRows(book: DirectoryBook, me: String, present: Set<String>,
                      statusOf: (String) -> String = { "" }): List<UserRowUi> {
    val meKey = DirectoryBook.normalize(me)
    return book.entries.asSequence()
        .filter { !it.external && it.msisdn.isNotBlank() && DirectoryBook.normalize(it.msisdn) != meKey }
        .distinctBy { DirectoryBook.normalize(it.msisdn) }
        .map { e ->
            val org = if (e.org.isBlank()) "" else book.orgPath(e.org).substringAfterLast(" › ").ifBlank { e.org }
            UserRowUi(e.msisdn, e.name.ifBlank { e.msisdn }, org,
                listOf("PTT ${com.cims.ue.dispatch.session.localNumber(e.msisdn)}", org).filter { it.isNotBlank() }.joinToString(" · "),
                // 어느 채널에서 말하는지·참여 중인지(«순찰1 발언»·«순찰1 참여») — 모르고 부르지 않게. 그것을 모르면 «접속».
                statusOf(e.msisdn).ifEmpty { if (DirectoryBook.normalize(e.msisdn) in present) "접속" else "" })
        }
        .sortedWith(compareBy({ it.status.isEmpty() }, { it.name }))
        .toList()
}

/** 거르기 칩 — «전체»·«접속» 다음에 사람이 많은 조직 셋. */
internal fun userFilters(rows: List<UserRowUi>): List<String> =
    listOf(FILTER_ALL, FILTER_ONLINE) +
        rows.groupingBy { it.org }.eachCount().filterKeys { it.isNotBlank() }
            .entries.sortedByDescending { it.value }.take(3).map { it.key }

internal const val FILTER_ALL = "전체"
internal const val FILTER_ONLINE = "접속"

internal fun List<UserRowUi>.filterBy(filter: String, query: String): List<UserRowUi> {
    val q = query.trim()
    return filter { r ->
        (filter == FILTER_ALL || (filter == FILTER_ONLINE && r.status.isNotEmpty()) || r.org == filter) &&
            (q.isEmpty() || r.name.contains(q, true) || r.number.contains(q) || r.org.contains(q, true))
    }
}

/**
 * 개별 통화 대상 — 고른 사람이 있으면 그 사람, 없으면 **입력한 이름·번호**(데스크톱 `PttOriginateViewModel.Start` 와 같은
 * 풀이). 이름은 주소록에 정확히 있을 때만 그 번호로 푼다. 주소록에 없어도 **번호 모양이면 그 번호**로 건다 — 주소록을 못
 * 받았거나 막 개설한 회선도 걸 수 있어야 한다. 모르는 이름은 null(서버 404 로 돌아올 요청을 보내지 않는다).
 * 순수 함수(시험 대상).
 */
internal fun privateTargetOf(book: DirectoryBook, query: String, picked: List<DirectoryEntry>): String? {
    picked.firstOrNull()?.let { return it.msisdn }
    val q = query.trim()
    if (q.isEmpty()) return null
    book.entries.firstOrNull { it.name.isNotBlank() && it.name.equals(q, ignoreCase = true) }?.let { return it.msisdn }
    val qn = DirectoryBook.normalize(q)
    book.entries.firstOrNull { DirectoryBook.normalize(it.msisdn) == qn }?.let { return it.msisdn }
    val numberLike = q.any { it.isDigit() } && q.all { it.isDigit() || it in "+-() " }
    return if (numberLike) q.filter { it.isDigit() || it == '+' } else null
}

/** 아래 줄 조작의 자격 — 고름·입력에서 정한다(순수 함수, 시험 대상). */
data class AddChannelActions(
    /** 개별 통화 대상 — 고른 한 명, 아무도 안 골랐으면 입력한 이름·번호. 둘 이상 골랐으면 null(1:1 이 아니다). */
    val privateTarget: String?,
    val canPrivate: Boolean,
    val canAdhoc: Boolean,
    /** [일제 통화] — 누르는 동안은 고름이 비어도 켜 둔다(제스처가 끊기면 즉시 끝난다). */
    val canBroadcast: Boolean,
    val canGroup: Boolean,
)

/**
 * @param picked 고른 PTT 번호(고른 순서).
 * @param canCreate 그룹 생성 자격 — 최종 판정은 서버 GMS 다.
 * @param holding 애드혹 일제 통화를 누르고 있다 — 그동안 다른 조작은 잠근다.
 */
internal fun addChannelActions(book: DirectoryBook, query: String, picked: List<String>,
                               canCreate: Boolean, holding: Boolean): AddChannelActions {
    val entries = picked.map { n ->
        val key = DirectoryBook.normalize(n)
        book.entries.firstOrNull { DirectoryBook.normalize(it.msisdn) == key } ?: DirectoryEntry("", "", n)
    }
    val target = if (entries.size <= 1) privateTargetOf(book, query, entries) else null
    return AddChannelActions(
        privateTarget = target,
        canPrivate = !holding && target != null,
        canAdhoc = !holding && picked.isNotEmpty(),
        canBroadcast = holding || picked.isNotEmpty(),
        canGroup = !holding && picked.isNotEmpty() && canCreate)
}

/**
 * 채널 추가 패널 — **VM 을 붙이는 껍데기**.
 *
 * @param present 로스터에 접속으로 잡힌 번호(정규형) — 상태 칸의 근거.
 * @param picked 고른 번호(목록 순서가 아니라 고른 순서) — 패널을 닫아도 남는다(MainViewModel).
 * @param canCreate 그룹 생성 자격 — 없으면 [그룹 추가] 를 끈다(최종 판정은 서버 GMS).
 * @param onGroup [그룹 추가 ›] — 고른 사람으로 새 그룹 폼(패널 안 한 겹).
 */
@Composable
fun AddChannelPanel(
    channels: PttChannelsViewModel,
    present: Set<String>,
    picked: List<String>,
    onToggle: (String) -> Unit,
    onClear: () -> Unit,
    canCreate: Boolean,
    pinned: Boolean,
    onPin: () -> Unit,
    onClose: () -> Unit,
    onGroup: () -> Unit,
    onPerson: (PersonAction, String) -> Unit,
    /** 걸었다 — «채널» 면으로(다른 면에서 패널을 열어 걸면 새 카드가 보이지 않는다). */
    onStarted: () -> Unit = {},
) {
    val book by channels.pttBook.collectAsStateWithLifecycle()
    val error by channels.originError.collectAsStateWithLifecycle()
    val bcHeld by channels.broadcastHeld.collectAsStateWithLifecycle()
    val holding = bcHeld == PttChannelsViewModel.ADHOC_BROADCAST
    val cards by channels.cards.collectAsStateWithLifecycle()      // 발언자·세션이 바뀌면 줄의 상태도 다시 낸다
    val rows = remember(book, present, cards) {
        val status = channels.pttStatusMap()                  // 한 번 만들어 줄마다 찾는다
        userRows(book, channels.myPttNumber, present) { n -> status[DirectoryBook.normalize(n)].orEmpty() }
    }
    var query by remember { mutableStateOf("") }
    var emergency by remember { mutableStateOf(false) }
    var fullDuplex by remember { mutableStateOf(false) }
    val actions = addChannelActions(book, query, picked, canCreate, holding)

    // 걸었다 — 고름·입력을 비우고 패널을 닫는다(새 카드가 내 채널에 선다). 실패면 남겨 두고 사유를 적는다.
    //   고정(핀)한 패널은 닫지 않는다 — 연달아 부르려고 고정해 둔 것이다(데스크톱과 같다).
    val done = { onClear(); query = ""; emergency = false; fullDuplex = false; if (!pinned) onClose(); onStarted() }
    // 애드혹 일제 통화를 놓았다(끝) — 누르는 동안은 닫지 않았다. 개시 실패면 고름·패널을 남긴다.
    var wasHolding by remember { mutableStateOf(false) }
    LaunchedEffect(holding) {
        if (wasHolding && !holding && error == null) done()
        wasHolding = holding
    }
    DisposableEffect(Unit) { onDispose { channels.clearOriginError() } }

    AddChannelPanelContent(
        rows = rows, picked = picked, onToggle = { if (!holding) { channels.clearOriginError(); onToggle(it) } },
        onClear = onClear, query = query, onQuery = { query = it; channels.clearOriginError() },
        actions = actions, targetLabel = actions.privateTarget?.let { book.nameOf(it).ifBlank { it } }.orEmpty(),
        emergency = emergency, onEmergency = { emergency = !emergency },
        fullDuplex = fullDuplex, onFullDuplex = { fullDuplex = !fullDuplex },
        holding = holding, error = error,
        pinned = pinned, onPin = onPin, onClose = onClose,
        onPrivate = { actions.privateTarget?.let { channels.startPrivate(it, fullDuplex, emergency, done) } },
        onAdhoc = { channels.startAdhoc(picked, emergency, done) },
        onBroadcastDown = { channels.broadcastAdhocDown(picked) }, onBroadcastUp = channels::broadcastUp,
        onGroup = onGroup,
        personAt = channels::personAt, onPerson = onPerson)
}

/** 채널 추가 본문 — **순수 컴포저블**(거르기만 제 상태). */
@Composable
fun AddChannelPanelContent(
    rows: List<UserRowUi>,
    picked: List<String> = emptyList(),
    onToggle: (String) -> Unit = {},
    onClear: () -> Unit = {},
    query: String = "",
    onQuery: (String) -> Unit = {},
    actions: AddChannelActions = AddChannelActions(null, false, picked.isNotEmpty(), picked.isNotEmpty(), picked.isNotEmpty()),
    /** 개별 통화 대상의 표시 이름(아무도 안 골랐을 때 입력한 번호를 알린다). */
    targetLabel: String = "",
    emergency: Boolean = false,
    onEmergency: () -> Unit = {},
    fullDuplex: Boolean = false,
    onFullDuplex: () -> Unit = {},
    holding: Boolean = false,
    error: String? = null,
    pinned: Boolean = false,
    onPin: () -> Unit = {},
    onClose: () -> Unit = {},
    onPrivate: () -> Unit = {},
    onAdhoc: () -> Unit = {},
    onBroadcastDown: () -> Unit = {},
    onBroadcastUp: () -> Unit = {},
    onGroup: () -> Unit = {},
    personAt: (String) -> PersonEntry? = { null },
    onPerson: (PersonAction, String) -> Unit = { _, _ -> },
) {
    val p = Tokens.palette
    var filter by remember { mutableStateOf(FILTER_ALL) }
    val filters = remember(rows) { userFilters(rows) }
    val shown = remember(rows, filter, query) { rows.filterBy(filter, query) }
    val pickedKeys = remember(picked) { picked.map(DirectoryBook::normalize).toSet() }

    SidePanelFrame(title = "사용자 ${rows.size}", tag = "채널 추가", pinned = pinned, onPin = onPin, onClose = onClose) {
        // 검색 = 거르기 + 개별 통화 번호 입력 — 주소록에 없는 번호도 여기 치고 [개별 통화] 로 건다.
        SearchField(query, onQuery, "이름 · PTT 번호 · 조직",
            Modifier.padding(start = 16.dp, end = 16.dp, top = 10.dp, bottom = 6.dp))
        Row(Modifier.horizontalScroll(rememberScrollState()).padding(start = 16.dp, end = 16.dp, top = 2.dp, bottom = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            filters.forEach { f -> FilterPill(f, f == filter, onClick = { filter = f }) }
        }
        LazyColumn(Modifier.weight(1f)) {
            items(shown, key = { it.number }) { r ->
                UserRow(r, DirectoryBook.normalize(r.number) in pickedKeys, onToggle = { onToggle(r.number) },
                    personAt = personAt, onPerson = onPerson)
            }
            if (shown.isEmpty()) item {
                Text(when {
                        rows.isEmpty() -> "PTT 주소록을 받지 못했습니다 — 서버 연결을 확인하세요"
                        targetLabel.isNotEmpty() -> "주소록에 없는 번호입니다 — [개별 통화] 로 겁니다"
                        else -> "일치하는 사람이 없습니다"
                    },
                    fontSize = Type.body, color = p.muted, modifier = Modifier.padding(24.dp))
            }
        }
        // 고른 사람으로 하는 일 — 지금 걸기(개별·애드혹·일제) / 두고 쓰기(그룹 추가).
        Column(
            Modifier.fillMaxWidth()
                .drawBehind { drawLine(p.edge, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
                .padding(start = 16.dp, end = 16.dp, top = 10.dp, bottom = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    when {
                        picked.isNotEmpty() -> "${picked.size}명 선택"
                        targetLabel.isNotEmpty() -> "개별 통화 · $targetLabel"
                        else -> "사람을 고르세요"
                    },
                    fontSize = Type.body, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f))
                // 거는 방식 — 긴급(개별·애드혹 둘 다, TS 24.379 emergency) · 전이중(개별 통화만 — 마이크가 늘 열려 발언 대상이 아니다).
                FilterPill("긴급", emergency, onClick = onEmergency)
                FilterPill("전이중(개별)", fullDuplex, onClick = onFullDuplex)
                if (picked.isNotEmpty()) TextButton(onClick = onClear, enabled = !holding,
                    contentPadding = PaddingValues(horizontal = 6.dp)) { Text("선택 해제", fontSize = Type.meta) }
            }
            error?.let { Text(it, fontSize = Type.meta, color = p.emg) }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PillButton("개별 통화", onPrivate, height = 44.dp, kind = Pill.LINE,
                    enabled = actions.canPrivate, modifier = Modifier.weight(1f))
                PillButton("애드혹 통화", onAdhoc, height = 44.dp, kind = Pill.LINE,
                    enabled = actions.canAdhoc, modifier = Modifier.weight(1f))
                // 누르는 동안 개시+발언, 놓으면 끝 — 채널 상세의 [일제 통화] 와 같은 한 버튼. 누르는 동안 버튼이 사라지면 끝난다.
                BroadcastHoldButton(enabled = actions.canBroadcast, held = holding,
                    onDown = onBroadcastDown, onUp = onBroadcastUp, height = 44.dp, modifier = Modifier.weight(1f))
            }
            PillButton("그룹 추가 ›", onGroup, height = 44.dp, kind = Pill.INK,
                enabled = actions.canGroup, modifier = Modifier.fillMaxWidth())
        }
    }
}

@Composable
private fun UserRow(
    r: UserRowUi,
    selected: Boolean,
    onToggle: () -> Unit,
    personAt: (String) -> PersonEntry?,
    onPerson: (PersonAction, String) -> Unit,
) {
    val p = Tokens.palette
    var menu by remember { mutableStateOf(false) }
    Row(
        Modifier.fillMaxWidth().height(48.dp).background(if (selected) p.primarySoft else p.paper)
            .drawBehind { drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
            .clickable(onClick = onToggle)
            .padding(start = 16.dp, end = 4.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(Modifier.size(22.dp).clip(RoundedCornerShape(5.dp)).background(if (selected) p.primary else p.paper)
                .border(1.5.dp, if (selected) p.primary else p.edge, RoundedCornerShape(5.dp)), contentAlignment = Alignment.Center) {
            if (selected) Icon(Icons.Filled.Check, contentDescription = null, tint = p.onPrimary, modifier = Modifier.size(14.dp))
        }
        Column(Modifier.weight(1f)) {
            Text(r.name, fontSize = Type.strong, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(r.meta, fontSize = Type.meta, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (r.status.isNotEmpty()) Text(r.status, fontSize = Type.meta, color = p.talkInk)
        Box {
            IconButton(onClick = { menu = true }, modifier = Modifier.size(36.dp)) {
                Icon(Icons.Filled.MoreVert, contentDescription = "${r.name} 사람 메뉴", tint = p.muted, modifier = Modifier.size(18.dp))
            }
            if (menu) PersonMenu(person = personAt(r.number), expanded = true, onDismiss = { menu = false },
                onPick = { a, n -> menu = false; onPerson(a, n) })
        }
    }
}
