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
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.ui.Tag
import com.cims.ue.dispatch.ui.Type

private val PAGES = listOf("로스터", "메시지", "이벤트")

/**
 * @param id  [무전] 목록에서 연 채널 id — 내 채널(그룹·사설콜·애드혹) 또는 범위 채널.
 * @param onShowRoster 편성 전원 보기 — [PTT 그룹] 화면 상세로(§6.12).
 */
@Composable
fun ChannelScreen(
    id: String,
    channels: PttChannelsViewModel,
    scoped: ScopedChannelsViewModel,
    messages: PttMessagesViewModel,
    activity: PttActivityViewModel,
    page: Int,
    onPageChange: (Int) -> Unit,
    onBack: () -> Unit,
    onShowRoster: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val mineCards by channels.cards.collectAsStateWithLifecycle()
    val scopedCards by scoped.cards.collectAsStateWithLifecycle()
    val targets by channels.targetIds.collectAsStateWithLifecycle()
    val mine = mineCards.firstOrNull { it.id == id }
    val range = scopedCards.firstOrNull { it.id == id }

    // ④⑤ 는 포커스를 따라간다 — 배치가 바뀌어도 그대로인 불변(§6.3).
    LaunchedEffect(id) {
        messages.onFocusChanged(id)
        activity.onFocusChanged(id)
    }

    val pager = rememberPagerState(initialPage = page.coerceIn(0, PAGES.lastIndex)) { PAGES.size }
    LaunchedEffect(pager.currentPage) { onPageChange(pager.currentPage) }

    Column(modifier.fillMaxSize()) {
        // ── 머리 ──
        Surface(color = if (mine?.emergency == true) MaterialTheme.colorScheme.errorContainer
                        else MaterialTheme.colorScheme.surfaceVariant) {
            Column(Modifier.fillMaxWidth().padding(start = 4.dp, end = 8.dp, top = 4.dp, bottom = 6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "뒤로")
                    }
                    Text(mine?.title ?: range?.title ?: id, fontSize = Type.title,
                        fontWeight = FontWeight.Bold, maxLines = 1, modifier = Modifier.weight(1f))
                    mine?.let { Tag(it.badge) }
                    if (range != null) Tag("범위")
                }
                val sub = mine?.let { c ->
                    listOfNotNull(
                        c.participants.takeIf { it > 0 }?.let { "참가 $it" },
                        c.line2.takeIf { it.isNotEmpty() },
                        c.stateText).joinToString(" · ")
                } ?: range?.let { c ->
                    listOfNotNull(
                        c.participants.takeIf { it > 0 }?.let { "참가 $it" },
                        c.line2.takeIf { it.isNotEmpty() }).joinToString(" · ")
                }
                if (!sub.isNullOrEmpty()) Text(sub, fontSize = Type.meta,
                    modifier = Modifier.padding(start = 12.dp))

                // ── 조작 — 목록의 행이 얇을 수 있는 이유가 여기다 ──
                Row(Modifier.padding(start = 8.dp, top = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    if (mine != null) {
                        if (mine.joined) TextButton(onClick = { channels.leave(mine) }) { Text("나가기") }
                        else if (mine.kind == CardKind.MEMBER) {
                            Button(onClick = { channels.join(mine) }) { Text("참여") }
                            TextButton(onClick = { channels.joinEmergency(mine) }) { Text("긴급") }
                        }
                        if (mine.canCheck) FilterChip(
                            selected = mine.id in targets,
                            onClick = { channels.toggleTarget(mine.id) },
                            label = { Text(if (mine.id in targets) "발언 대상 ✓" else "발언 대상",
                                fontSize = Type.meta) })
                    }
                    if (range != null) TextButton(onClick = { scoped.toggleListen(range) }) {
                        Text(if (range.listening) "청취 중지" else "청취")
                    }
                    Spacer(Modifier.weight(1f))
                    val gid = mine?.group?.id ?: range?.group?.id
                    if (gid != null) TextButton(onClick = { onShowRoster(gid) }) {
                        Text("편성 전원", fontSize = Type.meta)
                    }
                }
            }
        }

        if (mine == null && range == null) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("채널이 사라졌습니다 — 세션이 끝났거나 편성에서 빠졌습니다",
                    fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center)
            }
            return@Column
        }

        // ── 세 면 — 좌우 스와이프 ──
        TabRow(selectedTabIndex = pager.currentPage) {
            PAGES.forEachIndexed { i, label ->
                val unread = if (i == 1) mine?.unread ?: 0 else 0
                Tab(selected = pager.currentPage == i,
                    onClick = { onPageChange(i) },
                    text = {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(label, fontSize = Type.body)
                            if (unread > 0) { Spacer(Modifier.width(4.dp)); Badge { Text("$unread") } }
                        }
                    })
            }
        }
        // 탭을 누른 것도 스와이프와 같은 자리로 모은다 — 두 경로가 상태를 따로 들면 어긋난다.
        LaunchedEffect(page) { if (page != pager.currentPage) pager.animateScrollToPage(page) }

        HorizontalPager(state = pager, modifier = Modifier.weight(1f)) { i ->
            when (i) {
                0 -> RosterPage(
                    roster = (mine?.group ?: range?.group)?.roster.orEmpty(),
                    speaker = mine?.speaker ?: range?.speaker.orEmpty(),
                    me = channels.myPttNumber,
                    nameOf = channels::nameOf)
                1 -> Box(Modifier.fillMaxSize().padding(8.dp)) { Messages(messages) }
                else -> Box(Modifier.fillMaxSize().padding(8.dp)) { Activity(activity) }
            }
        }
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
) {
    val chips = rosterPreview(roster, speaker, me, nameOf, max = Int.MAX_VALUE).chips
    if (chips.isEmpty()) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text("접속한 사람이 없습니다", fontSize = Type.body,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        return
    }
    LazyColumn(Modifier.fillMaxSize()) {
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
