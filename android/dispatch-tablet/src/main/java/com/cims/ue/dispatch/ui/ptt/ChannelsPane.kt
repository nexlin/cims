// [관제] › [무전] › «채널» 면 — 내 채널 카드 + 타 채널 (android_dispatch_tablet.md §6.3a)
//
// **왼쪽 = 내 채널**(폭 470 고정, 카드 2열). 카드 모양은 시안 E1 그대로다 — 1줄 «핀 번호. 이름» · 2줄 발언자·사유 ·
// 3줄 접속자 · 4줄 참가·경과 + 오른쪽 아래 둥근 ✓. 카드는 «고르지 않고도 하는» 일만 품는다 — 발언 대상 ✓(전이중 개별
// 통화는 그 자리가 음소거), 참여 전이면 [참여]·[긴급](✓ 도 참여 — 참여는 곧 단일 발언 대상이다). 카드를 누르면
// **오른쪽 사이드 패널에 채널 상세**가 열린다(같은 카드를 다시 누르면 닫힌다). 마지막 칸 [채널 추가하기] 는 **채널 추가
// 패널**을 연다 — 개별 통화·애드혹 통화·그룹 추가 셋이 모두 내 채널에 카드 한 장을 더하는 일이라 한 자리다.
//
// **오른쪽 = 타 채널**(청취 범위 — 나머지 폭). 패널이 열리면 이 칸만 좁아져 2열 → 1열이 된다. 내 채널 칸은 움직이지 않는다
// — 말하려던 카드가 패널 때문에 자리를 옮기면 안 된다.
@file:OptIn(ExperimentalFoundationApi::class)

package com.cims.ue.dispatch.ui.ptt

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
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
    /**
     * 1줄 — «핀 번호. 이름». 핀 번호는 내 채널 안의 **고정 순서**(데스크톱 ① 핀 번호, dispatch_desktop_ui.md §4.1 — 진행 중이라고
     * 위로 올리지 않는다). 개별 통화는 이름 앞에 종류를 붙인다(«4. 개별 · 김반장», 애드혹은 이름이 이미 «애드혹 3인»).
     */
    val title: String,
    /** 채널 이름만 — 접근성 라벨이 쓴다(«순찰1 발언 대상»). */
    val name: String = title,
    /** 2줄 — 발언자·사유(«발언 김관제 00:14»·«발언 없음»·«대기 · 멤버 12»). 긴급·임박·일제는 앞에 그 낱말. */
    val sub: String = "",
    /** 3줄 — 접속자 미리보기(«김관제 · 이당직 · 박현장 +4»·«미참여»). */
    val roster: String = "",
    /** 4줄 왼쪽 — «참가 7 · 12:31»(참여 전이면 비고 [참여]·[긴급] 이 선다). */
    val meta: String = "",
    val unread: Int = 0,
    val emergency: Boolean = false,
    val peril: Boolean = false,
    val speaking: Boolean = false,
    val active: Boolean = false,
    val control: CardControl = CardControl.NONE,
    /** 발언 대상 ✓ 켜짐 / 음소거 켜짐. */
    val on: Boolean = false,
    /** 오른쪽 패널에 열려 있다. */
    val selected: Boolean = false,
)

/**
 * 도메인 카드 → 표시용 카드(순수 함수, 시험 대상). 줄마다 시안 E1 의 값이다.
 *
 * @param pin 내 채널 안의 차례(1부터). 0 이면 번호를 달지 않는다.
 */
internal fun ChannelCard.toCardUi(
    pin: Int,
    targeted: Boolean,
    selected: Boolean,
    me: String,
    nameOf: (String) -> String,
): MineCardUi {
    // 참여하지 않은 멤버 그룹 — 시안 «3. 교통1 / 대기 · 멤버 12 / 미참여 / [참여] [긴급]».
    val waiting = kind == CardKind.MEMBER && !joined
    val named = if (kind == CardKind.MEMBER || title.startsWith(badge)) title else "$badge · $title"
    val base = when {
        waiting -> listOf(if (group?.hasSession == true) "진행 중" else "대기", line2).filter { it.isNotEmpty() }.joinToString(" · ")
        else -> line2.ifEmpty { if (joined) "발언 없음" else "" }
    }
    val state = listOfNotNull("긴급".takeIf { emergency }, "임박".takeIf { imminentPeril && !emergency })
    val roster = when {
        waiting -> "미참여"
        kind == CardKind.PRIVATE -> title                          // 상대 — 시안 «김반장»
        group != null && group.roster.any { it.status.equals("connected", true) } -> {
            val pv = rosterPreview(group.roster, speaker, me, nameOf, max = 3)
            pv.chips.joinToString(" · ") { if (it.isMe) "${it.label}(나)" else it.label } +
                (if (pv.more > 0) " +${pv.more}" else "")
        }
        session?.adhocMembers?.isNotEmpty() == true ->
            session.adhocMembers.joinToString(" · ") { nameOf(com.cims.ue.dispatch.session.userPart(it)).ifBlank {
                com.cims.ue.dispatch.session.userPart(it) } }
        else -> ""
    }
    val control = when {
        canMute -> CardControl.MUTE
        canCheck -> CardControl.TARGET
        waiting -> CardControl.JOIN
        else -> CardControl.NONE
    }
    val meta = when {
        !joined -> ""
        kind == CardKind.PRIVATE -> stateText                      // 시안 «02:14» — 1:1 이라 참가 수가 없다
        else -> listOfNotNull(participants.takeIf { it > 0 }?.let { "참가 $it" }, stateText).joinToString(" · ")
    }
    return MineCardUi(
        id = id, title = if (pin > 0) "$pin. $named" else named, name = title,
        sub = (state + base).filter { it.isNotEmpty() }.joinToString(" · "),
        roster = roster, meta = meta,
        unread = unread, emergency = emergency, peril = imminentPeril && !emergency,
        speaking = speaking || speaker.isNotEmpty(), active = hasSession,
        control = control, on = if (control == CardControl.MUTE) muted else targeted, selected = selected)
}

/**
 * «채널» 면 — **VM 을 붙이는 껍데기**. 그리는 일은 [ChannelsPaneContent] 가 한다.
 *
 * @param selectedId 오른쪽 패널에 열린 채널 — 그 카드·행을 강조한다.
 * @param onOpen 카드·행을 눌렀다 — 패널 열기/닫기(같은 대상이면 닫힌다).
 * @param addOpen [채널 추가] 패널이 열려 있다 — 타일을 강조한다.
 * @param onAdd [채널 추가하기] 타일 — 채널 추가 패널을 연다/닫는다.
 */
@Composable
fun ChannelsPane(
    channels: PttChannelsViewModel,
    scoped: ScopedChannelsViewModel,
    selectedId: String?,
    onOpen: (String) -> Unit,
    addOpen: Boolean = false,
    onAdd: () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    val mine by channels.cards.collectAsStateWithLifecycle()
    val targets by channels.targetIds.collectAsStateWithLifecycle()
    val scopedCards by scoped.cards.collectAsStateWithLifecycle()
    val filter by scoped.filter.collectAsStateWithLifecycle()
    val query by scoped.query.collectAsStateWithLifecycle()
    val listenText by scoped.listenText.collectAsStateWithLifecycle()

    ChannelsPaneContent(
        mine = mine.mapIndexed { i, c ->
            c.toCardUi(pin = i + 1, targeted = c.id in targets, selected = c.id == selectedId,
                me = channels.myPttNumber, nameOf = channels::nameOf)
        },
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
        addOpen = addOpen, onAdd = onAdd,
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
    addOpen: Boolean = false,
    onAdd: () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    Row(modifier.fillMaxSize()) {
        MineColumn(mine, onOpen, onControl, onJoin, onJoinEmergency, addOpen, onAdd, Modifier.width(MineColumnWidth))
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
    addOpen: Boolean,
    onAdd: () -> Unit,
    modifier: Modifier,
) {
    val p = Tokens.palette
    Column(modifier.fillMaxHeight()) {
        SectionHead("내 채널 ${mine.size}", end = 16.dp) {
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
            // 채널을 더하는 자리 — 개별 통화(1명)·애드혹 통화(N명)·그룹 추가(편성)를 한 패널에서 한다(§6.2e·§6.12).
            item(key = "#add") { AddChannelTile(addOpen, onAdd) }
            if (mine.isEmpty()) item(key = "#empty", span = { GridItemSpan(2) }) {
                Text("멤버 그룹이 없습니다 — 편성은 관리자에게 요청하거나 [채널 추가하기] 에서 그룹을 만드세요",
                    fontSize = Type.body, color = p.muted, modifier = Modifier.padding(8.dp))
            }
        }
    }
}

/** 카드 글자 — 크기마다 줄 높이를 시안에 맞춰 고정한다(120 안에 네 줄이 든다, §6.3c). */
private val CardTitle = TextStyle(fontSize = Type.title, lineHeight = Type.title * 1.3f, fontWeight = FontWeight.Bold)
private val CardSub = TextStyle(fontSize = Type.body, lineHeight = Type.body * 1.3f)
private val CardMeta = TextStyle(fontSize = Type.meta, lineHeight = Type.meta * 1.3f)

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
    // 테두리 — 기본 1 옅은 선, 열림 2.5 검정 + 회색 면(시안 E2), 긴급·임박 2 그 색 + 옅은 면(데스크톱 §4.1 «카드 테두리 빨강/주황»).
    val (bw, line, bg) = when {
        c.emergency -> Triple(2.dp, p.emergency, p.emergencyFill)
        c.peril -> Triple(2.dp, p.peril, p.peril.copy(alpha = 0.10f))
        c.selected -> Triple(2.5.dp, p.ink, p.bar)
        else -> Triple(1.dp, p.line, p.paper)
    }
    // 시안은 border-box — 안쪽 여백이 **테두리 안에서** 잰 값이다(위 10 · 오른쪽 10 · 아래 8 · 왼쪽 12).
    Column(
        Modifier.height(120.dp).clip(shape).background(bg).border(BorderStroke(bw, line), shape).clickable(onClick = onOpen)
            .padding(start = 12.dp + bw, end = 10.dp + bw, top = 10.dp + bw, bottom = 8.dp + bw),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            StatusDot(dotColor(c.emergency, c.peril, c.speaking, c.active))
            Text(c.title, style = CardTitle, color = p.ink, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f))
            CountPill(c.unread)
        }
        val indent = Modifier.padding(start = 17.dp)
        if (c.sub.isNotEmpty()) {
            Spacer(Modifier.height(4.dp))
            Text(c.sub, indent, style = CardSub, color = p.ink2, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (c.roster.isNotEmpty()) {
            Spacer(Modifier.height(4.dp))
            Text(c.roster, indent, style = CardMeta, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        Spacer(Modifier.weight(1f))
        Row(indent, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            if (c.control == CardControl.JOIN) {
                // 참여 전 — 시안이 4줄 왼쪽에 [참여]·[긴급] 을 둔다. 오른쪽 ✓ 도 참여다(참여는 곧 단일 발언 대상 — `join`).
                PillButton("참여", onJoin, height = 30.dp, filled = true)
                PillButton("긴급", onJoinEmergency, height = 30.dp, color = p.emergency)
                Spacer(Modifier.weight(1f))
            } else Text(c.meta, style = CardMeta, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f))
            when (c.control) {
                CardControl.TARGET -> RoundToggle(on = c.on, onClick = onControl,
                    label = if (c.on) "${c.name} 발언 대상 해제" else "${c.name} 발언 대상") {
                    CheckMark(if (c.on) p.onInk else p.faint)
                }
                CardControl.JOIN -> RoundToggle(on = false, onClick = onJoin, label = "${c.name} 참여하고 발언 대상") {
                    CheckMark(p.faint)
                }
                // 음소거 — 켜지면 경고색으로 채운다(«말하고 있다고 믿는데 안 나가는» 상태를 놓치지 않게).
                CardControl.MUTE -> RoundToggle(on = c.on, onClick = onControl, onColor = p.emergency,
                    label = if (c.on) "${c.name} 음소거 풀기" else "${c.name} 음소거") {
                    Icon(if (c.on) Icons.Filled.MicOff else Icons.Filled.Mic, contentDescription = null,
                        modifier = Modifier.size(18.dp), tint = if (c.on) p.onInk else p.ink)
                }
                CardControl.NONE -> Unit
            }
        }
    }
}

/** 둥근 토글(36) — 발언 대상 ✓·음소거. 켜지면 면을 채운다(시안 테두리 1.5 · 꺼짐 옅은 선). */
@Composable
private fun RoundToggle(
    on: Boolean,
    onClick: () -> Unit,
    label: String,
    onColor: Color = Tokens.palette.ink,
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

/** ✓ — 시안의 선 그림(24 격자 M5 12 L10 17 L20 7, 굵기 2.5, 둥근 끝)을 18 에 그대로 옮긴다. */
@Composable
private fun CheckMark(color: Color) {
    Canvas(Modifier.size(18.dp)) {
        val u = size.minDimension / 24f
        val path = Path().apply { moveTo(5f * u, 12f * u); lineTo(10f * u, 17f * u); lineTo(20f * u, 7f * u) }
        drawPath(path, color, style = Stroke(width = 2.5f * u, cap = StrokeCap.Round, join = StrokeJoin.Round))
    }
}

/** «+ 채널 추가하기» 타일 — 카드와 같은 크기의 점선 자리(시안: 점선 1.5 · «+» 20 굵게 · 글자 14). 패널이 열리면 채운다. */
@Composable
private fun AddChannelTile(open: Boolean, onClick: () -> Unit) {
    val p = Tokens.palette
    Column(
        Modifier.height(120.dp).fillMaxWidth().clip(RoundedCornerShape(10.dp))
            .background(if (open) p.bar else Color.Transparent)
            .clickable(onClickLabel = if (open) "채널 추가 닫기" else "채널 추가", onClick = onClick)
            .drawBehind {
                val w = (if (open) 2.5.dp else 1.5.dp).toPx()
                drawRoundRect(if (open) p.ink else p.faint, topLeft = Offset(w / 2, w / 2),
                    size = androidx.compose.ui.geometry.Size(size.width - w, size.height - w),
                    cornerRadius = CornerRadius(10.dp.toPx()),
                    style = Stroke(width = w, pathEffect = PathEffect.dashPathEffect(floatArrayOf(4.5.dp.toPx(), 4.5.dp.toPx()))))
            },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterVertically),
    ) {
        Text("+", fontSize = Type.display, fontWeight = FontWeight.Bold, color = p.ink2)
        Text("채널 추가하기", fontSize = Type.strong, color = p.ink2, fontWeight = if (open) FontWeight.Bold else FontWeight.Normal)
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
