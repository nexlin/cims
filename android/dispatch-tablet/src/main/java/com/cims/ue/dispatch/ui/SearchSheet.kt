// 통합 검색 — 사람·채널을 한 상자에서(android_dispatch_tablet.md §6.2f).
//
// 데스크톱 `PersonActionsViewModel` 의 검색 절반을 이식한 것이다. 데스크톱은 `Ctrl+K` 로 열지만 태블릿에는
// 그 입력이 없어 **상단 바의 돋보기**가 입구다(§11 에 적어 둔 대로).
//
// 결과 행은 데스크톱과 같게 **행동 버튼을 바로 단다** — 한 번 더 눌러 메뉴를 여는 것보다 빠르고, 검색은
// «찾아서 곧바로 건다» 는 동작이기 때문이다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.GroupInfo

/** 검색 결과 한 줄 — 사람이거나 채널이다. */
sealed interface SearchHit {
    data class Person(val entry: PersonEntry) : SearchHit
    data class Channel(val group: GroupInfo) : SearchHit
}

/** 결과 상한 — 데스크톱과 같은 값(사람 12 · 채널 6). 목록이 길면 고르는 것이 아니라 훑는 것이 된다. */
private const val MAX_PEOPLE = 12
private const val MAX_GROUPS = 6

/**
 * 검색 판정 — 순수 함수(시험 대상).
 *
 * 데스크톱 `PersonActionsViewModel.Filter` 와 같은 규칙이다.
 * - 사람: 이름에 질의가 들어 있거나, **정규형 번호**(내선·PTT)에 정규형 질의가 들어 있다.
 *   번호를 정규형으로 비교하는 이유는 `010…` 으로 친 질의가 `+8210…` 저장값에 걸려야 하기 때문이다.
 * - 채널: 이름 또는 id.
 * - 빈 질의는 **전부**(상한까지) — 열자마자 목록이 보여야 무엇을 찾을 수 있는지 안다.
 */
internal fun searchDirectory(
    people: List<PersonEntry>,
    groups: List<GroupInfo>,
    query: String,
): List<SearchHit> {
    val q = query.trim()
    val qn = DirectoryBook.normalize(q)
    val hitPeople = if (q.isEmpty()) people else people.filter { p ->
        p.name.contains(q, ignoreCase = true) ||
            (qn.isNotEmpty() &&
                (DirectoryBook.normalize(p.extension).contains(qn) ||
                    DirectoryBook.normalize(p.pttNumber).contains(qn)))
    }
    val hitGroups = if (q.isEmpty()) groups else groups.filter {
        it.name.contains(q, ignoreCase = true) || it.id.contains(q, ignoreCase = true)
    }
    return hitPeople.take(MAX_PEOPLE).map { SearchHit.Person(it) } +
        hitGroups.take(MAX_GROUPS).map { SearchHit.Channel(it) }
}

/** 채널 행의 부제 — 데스크톱 `GroupEntry.Meta` 와 같은 구성. */
internal fun channelMeta(g: GroupInfo): String = buildString {
    append("멤버 ${g.memberCount}")
    append(if (g.isMember) " · 멤버" else " · 청취 범위")
    if (g.hasSession) append(" · 진행 중 ${g.connectedCount}")
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SearchSheet(
    people: List<PersonEntry>,
    groups: List<GroupInfo>,
    onPerson: (PersonAction, String) -> Unit,
    onChannel: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var query by remember { mutableStateOf("") }
    val hits = searchDirectory(people, groups, query)

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.fillMaxWidth().heightIn(min = 360.dp, max = 560.dp)
            .padding(horizontal = 16.dp)) {
            OutlinedTextField(
                value = query, onValueChange = { query = it },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                label = { Text("이름 · 내선 · PTT 번호 · 채널") })

            if (hits.isEmpty()) {
                Text("찾는 결과가 없습니다", Modifier.padding(vertical = 16.dp), fontSize = Type.body,
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                return@Column
            }

            LazyColumn(Modifier.padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                items(hits, key = {
                    when (it) {
                        is SearchHit.Person -> "p:" + it.entry.pttNumber + "/" + it.entry.extension
                        is SearchHit.Channel -> "g:" + it.group.id
                    }
                }) { hit ->
                    when (hit) {
                        is SearchHit.Person -> PersonRow(hit.entry) { a, n -> onPerson(a, n); onDismiss() }
                        is SearchHit.Channel -> ChannelRow(hit.group) { onChannel(it); onDismiss() }
                    }
                    HorizontalDivider()
                }
            }
        }
    }
}

@Composable
private fun PersonRow(p: PersonEntry, onPick: (PersonAction, String) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(p.head, fontSize = Type.strong, fontWeight = FontWeight.Medium, maxLines = 1)
            if (p.orgPath.isNotEmpty())
                Text(p.orgPath, fontSize = Type.meta, maxLines = 1,
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        // 가진 회선에 있는 행동만 — 비활성 버튼을 늘어놓지 않는다(사람 메뉴와 같은 규칙).
        if (p.hasPtt) {
            Small("사설콜") { onPick(PersonAction.PRIVATE_CALL, p.pttNumber) }
            Small("애드혹") { onPick(PersonAction.ADHOC_ADD, p.pttNumber) }
            Small("SDS") { onPick(PersonAction.SDS, p.pttNumber) }
        }
        if (p.hasLine) Small("통화") { onPick(PersonAction.CALL, p.extension) }
    }
}

@Composable
private fun ChannelRow(g: GroupInfo, onChannel: (String) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(g.name, fontSize = Type.strong, fontWeight = FontWeight.Medium, maxLines = 1)
            Text(channelMeta(g), fontSize = Type.meta,
                 color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Small("채널로") { onChannel(g.id) }
    }
}

@Composable
private fun Small(text: String, onClick: () -> Unit) {
    TextButton(onClick = onClick, contentPadding = PaddingValues(horizontal = 8.dp)) {
        Text(text, fontSize = Type.meta)
    }
}
