// [무전] 화면 — 채널 목록 한 열 (android_dispatch_tablet.md §6.3)
//
// 데스크톱의 ①②(내 채널·범위 채널)를 **한 목록**으로 합친다. 두 패널로 나눠 놓으면 각각 반쪽 폭·반쪽
// 높이라 3장씩밖에 못 본다 — 관제사가 채널을 «훑는» 일을 못 한다. 합치면 전체 폭·전체 높이를 써서
// 열 몇 장이 보이고, 둘의 구분은 섹션 머리로 충분하다(소속이 다를 뿐 같은 종류의 것이다).
//
// **행은 얇다.** 조작 버튼([참여][긴급][나가기])을 행에서 뺐기 때문이다 — 그것들은 채널을 «고른 뒤» 하는
// 일이라 채널 화면 머리에 있다. 행에 남는 조작은 «고르지 않고도 하는» 둘뿐이다: 발언 대상 지정(무전),
// 청취 토글(범위). 행을 누르면 채널 화면으로 간다.
@file:OptIn(ExperimentalFoundationApi::class)

package com.cims.ue.dispatch.ui.ptt

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.ui.Tag
import com.cims.ue.dispatch.ui.Type

/**
 * [무전] — 채널 목록. **VM 을 붙이는 껍데기**다.
 *
 * 그리는 일은 전부 [PttScreenContent] 가 한다 — 그쪽은 표시용 값([ChannelRowUi])만 받아 Preview 가 선다.
 * 여기서 하는 일은 셋: 흐름 구독, 도메인 → 표시용 변환, 콜백 연결.
 *
 * @param onOpen 행 탭 — 채널 화면으로.
 */
@Composable
fun PttScreen(
    channels: PttChannelsViewModel,
    scoped: ScopedChannelsViewModel,
    onOpen: (String) -> Unit,
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
    if (sheet) OriginateSheet(channels) { sheet = false }

    PttScreenContent(
        mine = mine.map { it.toRowUi(targeted = it.id in targets) },
        scoped = scopedCards.map { it.toRowUi() },
        filter = filter,
        query = query,
        listenText = listenText,
        listenFull = scoped.listenFull,
        onOpen = onOpen,
        onToggleTarget = channels::toggleTarget,
        onToggleListen = { id -> scopedCards.firstOrNull { it.id == id }?.let(scoped::toggleListen) },
        onFilter = scoped::setFilter,
        onQuery = scoped::setQuery,
        onOriginate = { sheet = true },
        modifier = modifier)
}

/**
 * [무전] 목록 본문 — **순수 컴포저블**. 상태를 받기만 하고 갖지 않는다(검색창 펼침만 예외).
 *
 * 세션도 VM 도 모르므로 Preview 와 단위시험에서 그대로 쓸 수 있다.
 */
@Composable
fun PttScreenContent(
    mine: List<ChannelRowUi>,
    scoped: List<ChannelRowUi>,
    filter: ScopeFilter,
    query: String,
    listenText: String,
    listenFull: Boolean,
    onOpen: (String) -> Unit = {},
    onToggleTarget: (String) -> Unit = {},
    onToggleListen: (String) -> Unit = {},
    onFilter: (ScopeFilter) -> Unit = {},
    onQuery: (String) -> Unit = {},
    onOriginate: () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    var searching by remember { mutableStateOf(false) }

    LazyColumn(modifier.fillMaxSize()) {
        item(key = "h-mine") {
            SectionHeader("내 채널 ${mine.size}") {
                TextButton(onClick = onOriginate) { Text("사설콜·애드혹", fontSize = Type.meta) }
            }
        }
        if (mine.isEmpty()) item(key = "e-mine") { EmptyLine("참여할 채널이 없습니다") }
        items(mine, key = { "m-" + it.id }) { r ->
            ChannelRow(r, onClick = { onOpen(r.id) }) {
                // 발언 대상은 **채널을 열지 않고** 지정한다 — 여러 채널을 잡아 두고 말하는 조작이라
                //   목록에 있어야 한다(포커스 ≠ 발언 대상, §6.3).
                if (r.canTarget) TargetToggle(on = r.targeted) { onToggleTarget(r.id) }
            }
        }

        item(key = "h-scoped") {
            SectionHeader("범위 채널 ${scoped.size}") {
                Text(listenText, fontSize = Type.meta,
                    color = if (listenFull) MaterialTheme.colorScheme.error
                            else MaterialTheme.colorScheme.onSurfaceVariant)
                IconButton(onClick = { searching = !searching }) {
                    Icon(Icons.Filled.Search, contentDescription = "검색")
                }
            }
        }
        item(key = "f-scoped") {
            Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(4.dp),
                verticalAlignment = Alignment.CenterVertically) {
                ScopeFilter.entries.forEach { f ->
                    FilterChip(selected = filter == f, onClick = { onFilter(f) },
                        label = { Text(f.label, fontSize = Type.meta) })
                }
            }
        }
        if (searching) item(key = "q-scoped") {
            OutlinedTextField(
                value = query, onValueChange = onQuery,
                placeholder = { Text("채널 검색", fontSize = Type.body) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search))
        }
        if (scoped.isEmpty()) item(key = "e-scoped") { EmptyLine("청취 범위 채널이 없습니다") }
        // **듣고 있는 것이 먼저다.** 청취는 «켜 두고 잊는» 조작이라, 목록 아래에 묻히면 몇 개를 듣고 있는지
        //   모른 채 상한에 걸린다. 따로 면을 두지 않는 대신 목록에서 위로 올린다(§6.3).
        items(scoped.sortedBy { if (it.listening == true) 0 else 1 }, key = { "s-" + it.id }) { r ->
            ChannelRow(r, onClick = { onOpen(r.id) }) {
                TextButton(onClick = { onToggleListen(r.id) },
                    contentPadding = PaddingValues(horizontal = 8.dp)) {
                    Text(if (r.listening == true) "청취 중" else "청취", fontSize = Type.meta)
                }
            }
        }
    }
}

/** 섹션 머리 — 목록 위에 붙어 따라온다(어느 섹션을 보고 있는지 놓치지 않게). */
@Composable
private fun SectionHeader(title: String, actions: @Composable RowScope.() -> Unit) {
    Surface(color = MaterialTheme.colorScheme.surfaceVariant, tonalElevation = 2.dp) {
        Row(Modifier.fillMaxWidth().padding(start = 12.dp, end = 4.dp, top = 4.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Text(title, fontSize = Type.strong, fontWeight = FontWeight.Bold,
                modifier = Modifier.weight(1f))
            actions()
        }
    }
}

/**
 * 채널 한 줄 — 두 줄 56dp.
 *
 * 1줄 = 상태 점 · 이름 · (오른쪽) 미읽음·참가·상태 · 조작 하나
 * 2줄 = 발언자·사유 등 «지금 무슨 일이 있는가»
 */
@Composable
private fun ChannelRow(
    r: ChannelRowUi,
    onClick: () -> Unit,
    trailing: @Composable () -> Unit,
) {
    Column(Modifier.fillMaxWidth()
        .background(if (r.emergency) MaterialTheme.colorScheme.errorContainer else Color.Transparent)
        .clickable(onClick = onClick)
        .padding(start = 12.dp, end = 4.dp, top = 6.dp, bottom = 6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            StateDot(active = r.active, speaking = r.speaking, emergency = r.emergency)
            Spacer(Modifier.width(8.dp))
            Text(r.title, fontSize = Type.strong, fontWeight = FontWeight.Bold,
                maxLines = 1, modifier = Modifier.weight(1f))
            if (r.unread > 0) { Badge { Text("${r.unread}") }; Spacer(Modifier.width(6.dp)) }
            if (r.participants > 0) {
                Text("참가 ${r.participants}", fontSize = Type.meta,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.width(6.dp))
            }
            Text(r.state, fontSize = Type.meta)
            trailing()
        }
        if (r.subtitle.isNotEmpty()) Text(r.subtitle, fontSize = Type.meta,
            color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1,
            modifier = Modifier.padding(start = 20.dp))
    }
    HorizontalDivider()
}

/** 발언 대상 토글 — 체크 하나. 라벨을 쓰면 줄이 길어져 이름이 밀린다. */
@Composable
private fun TargetToggle(on: Boolean, onToggle: () -> Unit) {
    FilledIconToggleButton(checked = on, onCheckedChange = { onToggle() },
        modifier = Modifier.size(36.dp)) {
        Icon(Icons.Filled.Check, contentDescription = if (on) "발언 대상 해제" else "발언 대상")
    }
}

@Composable
private fun EmptyLine(text: String) {
    Text(text, Modifier.fillMaxWidth().padding(24.dp), fontSize = Type.body,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        textAlign = androidx.compose.ui.text.style.TextAlign.Center)
}
