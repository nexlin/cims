// [관제] › [무전] › «채널» 면 — 내 채널 카드 + 타 채널 (android_dispatch_tablet.md §6.3a)
//
// **왼쪽 = 내 채널**(폭 470 고정, 카드 2열). 카드는 «고르지 않고도 하는» 일만 품는다 — 발언 대상 ✓(전이중 개별 통화는 그
// 자리가 음소거), 참여 전이면 [참여]·[긴급]. 카드를 누르면 **오른쪽 사이드 패널에 채널 상세**가 열린다(같은 카드를 다시
// 누르면 닫힌다). **오른쪽 = 타 채널**(청취 범위 — 나머지 폭). 패널이 열리면 이 칸만 좁아져 2열 → 1열이 된다. 내 채널
// 칸은 움직이지 않는다 — 말하려던 카드가 패널 때문에 자리를 옮기면 안 된다.
@file:OptIn(ExperimentalFoundationApi::class)

package com.cims.ue.dispatch.ui.ptt

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.ui.CountPill
import com.cims.ue.dispatch.ui.FilterPill
import com.cims.ue.dispatch.ui.Label
import com.cims.ue.dispatch.ui.LabelStyle
import com.cims.ue.dispatch.ui.PillButton
import com.cims.ue.dispatch.ui.SectionHead
import com.cims.ue.dispatch.ui.StatusDot
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.TwoLines
import com.cims.ue.dispatch.ui.Type
import com.cims.ue.dispatch.ui.VDivider
import com.cims.ue.dispatch.ui.dotColor

/** 내 채널 칸 폭 — 카드 2열(시안 값). 패널이 열려도 이 폭은 그대로다. */
val MineColumnWidth = 470.dp

/** 카드 오른쪽 아래의 조작 — 한 카드에 하나만 선다(서로 모순이라 같이 두지 않는다). */
enum class CardControl { TARGET, MUTE, JOIN, NONE }

/** 내 채널 카드 한 장 — 도메인 카드에서 잘라낸 표시용 값(Preview·시험이 이것만으로 그린다). */
data class MineCardUi(
    val id: String,
    val title: String,
    /** 1줄 오른쪽의 종류 — «개별»·«애드혹»(멤버 그룹은 비운다). */
    val kind: String = "",
    /** 2줄 — 발언자·사유(«발언 김관제 00:14»·«발언 없음»·«대기 · 멤버 12»). */
    val sub: String = "",
    /** 3줄 — 접속자 미리보기(«김관제 · 이당직 · 박현장 +4»·«미참여»). */
    val roster: String = "",
    /** 4줄 왼쪽 — «참가 7 · 12:31». */
    val meta: String = "",
    val unread: Int = 0,
    val emergency: Boolean = false,
    val peril: Boolean = false,
    val speaking: Boolean = false,
    val active: Boolean = false,
    val broadcast: Boolean = false,
    val control: CardControl = CardControl.NONE,
    /** 발언 대상 ✓ 켜짐 / 음소거 켜짐. */
    val on: Boolean = false,
    /** 오른쪽 패널에 열려 있다. */
    val selected: Boolean = false,
)

/** 도메인 카드 → 표시용 카드. 접속자 줄은 발언자·나를 먼저 세우고 셋까지 편다(`rosterPreview`). */
internal fun ChannelCard.toCardUi(
    targeted: Boolean,
    selected: Boolean,
    me: String,
    nameOf: (String) -> String,
): MineCardUi {
    val roster = when {
        group != null && group.roster.any { it.status.equals("connected", true) } -> {
            val pv = rosterPreview(group.roster, speaker, me, nameOf, max = 3)
            pv.chips.joinToString(" · ") { if (it.isMe) "${it.label}(나)" else it.label } +
                (if (pv.more > 0) " +${pv.more}" else "")
        }
        session?.adhocMembers?.isNotEmpty() == true ->
            session.adhocMembers.joinToString(" · ") { nameOf(com.cims.ue.dispatch.session.userPart(it)).ifBlank {
                com.cims.ue.dispatch.session.userPart(it) } }
        kind == CardKind.MEMBER && !joined -> "미참여"
        else -> ""
    }
    val control = when {
        canMute -> CardControl.MUTE
        canCheck -> CardControl.TARGET
        kind == CardKind.MEMBER && !joined -> CardControl.JOIN
        else -> CardControl.NONE
    }
    return MineCardUi(
        id = id, title = title,
        kind = if (kind == CardKind.MEMBER) "" else badge,
        sub = line2.ifEmpty { if (joined) "발언 없음" else "" },
        roster = roster,
        meta = listOfNotNull(participants.takeIf { it > 0 && joined }?.let { "참가 $it" },
            stateText.takeIf { joined || group?.hasSession == true }).joinToString(" · "),
        unread = unread, emergency = emergency, peril = imminentPeril && !emergency,
        speaking = speaking || speaker.isNotEmpty(), active = hasSession, broadcast = isBroadcast,
        control = control, on = if (control == CardControl.MUTE) muted else targeted, selected = selected)
}

/**
 * «채널» 면 — **VM 을 붙이는 껍데기**. 그리는 일은 [ChannelsPaneContent] 가 한다.
 *
 * @param selectedId 오른쪽 패널에 열린 채널 — 그 카드·행을 강조한다.
 * @param onOpen 카드·행을 눌렀다 — 패널 열기/닫기(같은 대상이면 닫힌다).
 */
@Composable
fun ChannelsPane(
    channels: PttChannelsViewModel,
    scoped: ScopedChannelsViewModel,
    selectedId: String?,
    onOpen: (String) -> Unit,
    /** 발신 시트 사람 행의 [문자] — 1:1 SDS 스레드로. */
    onMessage: (String) -> Unit = {},
    modifier: Modifier = Modifier,
) {
    val mine by channels.cards.collectAsStateWithLifecycle()
    val targets by channels.targetIds.collectAsStateWithLifecycle()
    val scopedCards by scoped.cards.collectAsStateWithLifecycle()
    val filter by scoped.filter.collectAsStateWithLifecycle()
    val query by scoped.query.collectAsStateWithLifecycle()
    val listenText by scoped.listenText.collectAsStateWithLifecycle()

    var sheet by remember { mutableStateOf(false) }
    // 사람 메뉴의 «애드혹에 추가» 가 심어 둔 씨앗이 있으면 시트를 연다(§6.2f). 씨앗은 시트가 소비한다.
    val seed by channels.adhocSeed.collectAsStateWithLifecycle()
    LaunchedEffect(seed) { if (seed.isNotBlank()) sheet = true }
    if (sheet) OriginateSheet(channels, onDismiss = { sheet = false }, onMessage = onMessage)

    ChannelsPaneContent(
        mine = mine.map { it.toCardUi(it.id in targets, it.id == selectedId, channels.myPttNumber, channels::nameOf) },
        other = scopedCards.map { it.toRowUi() },
        selectedId = selectedId,
        filter = filter, query = query, listenText = listenText,
        onOpen = onOpen,
        onControl = { id ->
            val c = mine.firstOrNull { it.id == id } ?: return@ChannelsPaneContent
            when {
                c.canMute -> channels.toggleMute(id)
                c.canCheck -> channels.toggleTarget(id)
            }
        },
        onJoin = { id -> mine.firstOrNull { it.id == id }?.let(channels::join) },
        onJoinEmergency = { id -> mine.firstOrNull { it.id == id }?.let(channels::joinEmergency) },
        onToggleListen = { id -> scopedCards.firstOrNull { it.id == id }?.let(scoped::toggleListen) },
        onFilter = scoped::setFilter, onQuery = scoped::setQuery,
        onOriginate = { sheet = true },
        modifier = modifier)
}

/** «채널» 면 본문 — **순수 컴포저블**(검색창 펼침만 제 상태). */
@Composable
fun ChannelsPaneContent(
    mine: List<MineCardUi>,
    other: List<ChannelRowUi>,
    selectedId: String? = null,
    filter: ScopeFilter = ScopeFilter.ALL,
    query: String = "",
    listenText: String = "",
    onOpen: (String) -> Unit = {},
    onControl: (String) -> Unit = {},
    onJoin: (String) -> Unit = {},
    onJoinEmergency: (String) -> Unit = {},
    onToggleListen: (String) -> Unit = {},
    onFilter: (ScopeFilter) -> Unit = {},
    onQuery: (String) -> Unit = {},
    onOriginate: () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    Row(modifier.fillMaxSize()) {
        MineColumn(mine, onOpen, onControl, onJoin, onJoinEmergency, onOriginate, Modifier.width(MineColumnWidth))
        VDivider()
        OtherColumn(other, selectedId, filter, query, listenText, onOpen, onToggleListen, onFilter, onQuery,
            Modifier.weight(1f))
    }
}

@Composable
private fun MineColumn(
    mine: List<MineCardUi>,
    onOpen: (String) -> Unit,
    onControl: (String) -> Unit,
    onJoin: (String) -> Unit,
    onJoinEmergency: (String) -> Unit,
    onOriginate: () -> Unit,
    modifier: Modifier,
) {
    val p = Tokens.palette
    Column(modifier.fillMaxHeight()) {
        SectionHead("내 채널 ${mine.size}") {
            Spacer(Modifier.weight(1f))
            Text("카드 = 상세 열기 · ✓ = 말하기", fontSize = Type.meta, color = p.muted)
        }
        LazyVerticalGrid(
            columns = GridCells.Fixed(2),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
            modifier = Modifier.weight(1f),
        ) {
            items(mine, key = { it.id }) { c ->
                MineCard(c, onOpen = { onOpen(c.id) }, onControl = { onControl(c.id) },
                    onJoin = { onJoin(c.id) }, onJoinEmergency = { onJoinEmergency(c.id) })
            }
            // 새로 여는 자리 — 개별 통화(1명)·애드혹 그룹 통화(N명)는 편성이 없어 여기서 연다(발신 시트, §6.2e).
            item(key = "#originate") { OriginateTile(onOriginate) }
            if (mine.isEmpty()) item(key = "#empty", span = { GridItemSpan(2) }) {
                Text("멤버 그룹이 없습니다 — 편성은 관리자에게 요청하거나 [사용자] 에서 그룹을 만드세요",
                    fontSize = Type.body, color = p.muted, modifier = Modifier.padding(8.dp))
            }
        }
    }
}

@Composable
private fun MineCard(
    c: MineCardUi,
    onOpen: () -> Unit,
    onControl: () -> Unit,
    onJoin: () -> Unit,
    onJoinEmergency: () -> Unit,
) {
    val p = Tokens.palette
    val shape = RoundedCornerShape(10.dp)
    val (border, bg) = when {
        c.emergency -> BorderStroke(2.dp, p.emergency) to p.emergencyFill
        c.peril -> BorderStroke(2.dp, p.peril) to p.peril.copy(alpha = 0.10f)
        c.selected -> BorderStroke(2.5.dp, p.ink) to p.bar
        else -> BorderStroke(1.dp, p.line) to p.paper
    }
    Column(
        Modifier.height(120.dp).clip(shape).background(bg).border(border, shape).clickable(onClick = onOpen)
            .padding(start = 12.dp, end = 10.dp, top = 10.dp, bottom = 8.dp),
        verticalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            StatusDot(dotColor(c.emergency, c.peril, c.speaking, c.active))
            // 제목은 라벨에 밀릴 때만 줄어든다 — 남는 폭을 빈칸과 나눠 갖지 않게 한 줄 안에 묶는다.
            Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(c.title, fontSize = Type.title, fontWeight = FontWeight.Bold, maxLines = 1,
                    overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                // 종류 라벨 — 제목이 이미 그 낱말로 시작하면 되풀이하지 않는다(«애드혹 3인»).
                if (c.kind.isNotEmpty() && !c.title.startsWith(c.kind)) Label(c.kind, LabelStyle.OUTLINE)
                if (c.broadcast) Label("일제", LabelStyle.STRONG)
                if (c.emergency) Label("긴급", LabelStyle.STRONG, color = p.emergency)
                else if (c.peril) Label("임박", LabelStyle.STRONG, color = p.peril)
            }
            CountPill(c.unread)
        }
        val indent = Modifier.padding(start = 17.dp)
        if (c.sub.isNotEmpty()) Text(c.sub, indent, fontSize = Type.body, color = p.ink2, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
        if (c.roster.isNotEmpty()) Text(c.roster, indent, fontSize = Type.meta, color = p.muted, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.weight(1f))
        Row(indent, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(c.meta, fontSize = Type.meta, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f))
            when (c.control) {
                CardControl.TARGET -> RoundToggle(on = c.on, onClick = onControl,
                    label = if (c.on) "${c.title} 발언 대상 해제" else "${c.title} 발언 대상") {
                    Icon(Icons.Filled.Check, contentDescription = null, modifier = Modifier.size(18.dp),
                        tint = if (c.on) p.onInk else p.faint)
                }
                // 음소거 — 켜지면 경고색으로 채운다(«말하고 있다고 믿는데 안 나가는» 상태를 놓치지 않게).
                CardControl.MUTE -> RoundToggle(on = c.on, onClick = onControl, onColor = p.emergency,
                    label = if (c.on) "${c.title} 음소거 풀기" else "${c.title} 음소거") {
                    Icon(if (c.on) Icons.Filled.MicOff else Icons.Filled.Mic, contentDescription = null,
                        modifier = Modifier.size(18.dp), tint = if (c.on) p.onInk else p.ink)
                }
                CardControl.JOIN -> {
                    PillButton("참여", onJoin, height = 32.dp, filled = true)
                    PillButton("긴급", onJoinEmergency, height = 32.dp, color = p.emergency)
                }
                CardControl.NONE -> Unit
            }
        }
    }
}

/** 둥근 토글(36) — 발언 대상 ✓·음소거. 켜지면 면을 채운다. */
@Composable
private fun RoundToggle(
    on: Boolean,
    onClick: () -> Unit,
    label: String,
    onColor: androidx.compose.ui.graphics.Color = Tokens.palette.ink,
    icon: @Composable () -> Unit,
) {
    val p = Tokens.palette
    Box(
        Modifier.size(36.dp).clip(CircleShape).background(if (on) onColor else p.paper)
            .border(1.5.dp, if (on) onColor else p.line, CircleShape)
            .clickable(onClickLabel = label, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { icon() }
}

/** «+ 개별 · 애드혹 열기» 타일 — 카드와 같은 크기의 점선 자리. */
@Composable
private fun OriginateTile(onClick: () -> Unit) {
    val p = Tokens.palette
    Column(
        Modifier.height(120.dp).fillMaxWidth().clip(RoundedCornerShape(10.dp)).clickable(onClick = onClick)
            .drawBehind {
                val w = 1.5.dp.toPx()
                drawRoundRect(p.faint, topLeft = Offset(w / 2, w / 2),
                    size = androidx.compose.ui.geometry.Size(size.width - w, size.height - w),
                    cornerRadius = CornerRadius(10.dp.toPx()),
                    style = Stroke(width = w, pathEffect = PathEffect.dashPathEffect(floatArrayOf(10f, 8f))))
            },
        horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center,
    ) {
        Text("+", fontSize = Type.huge, fontWeight = FontWeight.Bold, color = p.ink2)
        Text("개별 · 애드혹 열기", fontSize = Type.strong, color = p.ink2)
    }
}

@Composable
private fun OtherColumn(
    other: List<ChannelRowUi>,
    selectedId: String?,
    filter: ScopeFilter,
    query: String,
    listenText: String,
    onOpen: (String) -> Unit,
    onToggleListen: (String) -> Unit,
    onFilter: (ScopeFilter) -> Unit,
    onQuery: (String) -> Unit,
    modifier: Modifier,
) {
    val p = Tokens.palette
    var searching by remember { mutableStateOf(query.isNotEmpty()) }
    BoxWithConstraints(modifier.fillMaxHeight()) {
        // 넓으면 2열 카드, 패널이 열려 좁아지면 1열 행(시안 E1·E2).
        val twoCols = maxWidth >= 560.dp
        Column(Modifier.fillMaxSize()) {
            SectionHead("타 채널") {
                Label("청취 가능", LabelStyle.OUTLINE, round = true)
                Text("${other.size}", fontSize = Type.title, fontWeight = FontWeight.Bold)
                Spacer(Modifier.weight(1f))
                if (listenText.isNotEmpty() && twoCols) Text(listenText, fontSize = Type.meta, color = p.muted)
                IconButton(onClick = { searching = !searching; if (!searching) onQuery("") },
                    modifier = Modifier.size(40.dp)) {
                    Icon(Icons.Filled.Search, contentDescription = "타 채널 검색", modifier = Modifier.size(20.dp))
                }
            }
            if (searching) SearchField(query, onQuery, "채널 이름", Modifier.padding(start = 16.dp, end = 16.dp, bottom = 8.dp))
            Row(Modifier.padding(start = 16.dp, end = 16.dp, bottom = 10.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                ScopeFilter.entries.forEach { f -> FilterPill(f.label, f == filter, onClick = { onFilter(f) }) }
            }
            LazyVerticalGrid(
                columns = GridCells.Fixed(if (twoCols) 2 else 1),
                contentPadding = PaddingValues(start = if (twoCols) 16.dp else 0.dp, end = if (twoCols) 16.dp else 0.dp, bottom = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(if (twoCols) 8.dp else 0.dp),
                modifier = Modifier.weight(1f),
            ) {
                items(other, key = { it.id }) { o ->
                    OtherRow(o, selected = o.id == selectedId, card = twoCols,
                        onOpen = { onOpen(o.id) }, onToggleListen = { onToggleListen(o.id) })
                }
                if (other.isEmpty()) item(span = { GridItemSpan(maxLineSpan) }) {
                    Text(if (query.isNotEmpty() || filter != ScopeFilter.ALL) "조건에 맞는 채널이 없습니다"
                         else "청취 범위의 채널이 없습니다",
                        fontSize = Type.body, color = p.muted, modifier = Modifier.padding(16.dp))
                }
            }
            // 발언하려면 참여해야 한다는 것을 **누르기 전에** 적는다 — 청취는 recvonly 합류다(dispatch_center.md §5.6).
            Box(Modifier.fillMaxWidth().drawBehind {
                drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx())
            }.padding(horizontal = 16.dp, vertical = 10.dp)) {
                Text("청취는 듣기만 합니다 — 발언하려면 그 채널에 참여해야 합니다. 행을 누르면 오른쪽에 채널 상세가 열립니다.",
                    fontSize = Type.meta, color = p.muted, maxLines = 2)
            }
        }
    }
}

@Composable
private fun OtherRow(
    o: ChannelRowUi,
    selected: Boolean,
    card: Boolean,
    onOpen: () -> Unit,
    onToggleListen: () -> Unit,
) {
    val p = Tokens.palette
    val shape = RoundedCornerShape(8.dp)
    val bg = when {
        o.emergency -> p.emergencyFill
        o.imminentPeril -> p.peril.copy(alpha = 0.10f)
        selected -> p.fill
        else -> p.paper
    }
    val base = Modifier.fillMaxWidth().height(64.dp)
    val framed = if (card) base.clip(shape).background(bg)
            .border(if (selected) 2.dp else 1.dp, if (selected) p.ink else p.divider, shape)
        else base.background(bg).drawBehind { drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
    Row(
        framed.clickable(onClick = onOpen).padding(start = if (card) 14.dp else 16.dp, end = if (card) 10.dp else 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        StatusDot(dotColor(o.emergency, o.imminentPeril, o.speaking, o.active || o.listening == true))
        TwoLines(o.title, o.subtitle, Modifier.weight(1f))
        if (o.emergency) Label("긴급", LabelStyle.STRONG, color = p.emergency)
        else if (o.imminentPeril) Label("임박", LabelStyle.STRONG, color = p.peril)
        val on = o.listening == true
        PillButton(if (on) "청취 중" else "청취", onToggleListen, height = 34.dp, filled = on, strongBorder = !on)
    }
}

/** 한 줄 검색 입력(40) — 칸 머리 아래에 펴진다. */
@Composable
internal fun SearchField(value: String, onValue: (String) -> Unit, hint: String, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    BasicTextField(
        value = value, onValueChange = onValue, singleLine = true,
        textStyle = TextStyle(fontSize = Type.body, color = p.ink),
        cursorBrush = SolidColor(p.ink),
        modifier = modifier.fillMaxWidth().height(40.dp).clip(RoundedCornerShape(6.dp))
            .border(1.dp, p.line, RoundedCornerShape(6.dp)).background(p.paper),
        decorationBox = { inner ->
            Box(Modifier.fillMaxSize().padding(horizontal = 12.dp), contentAlignment = Alignment.CenterStart) {
                if (value.isEmpty()) Text(hint, fontSize = Type.body, color = p.faint)
                inner()
            }
        })
}
