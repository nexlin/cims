// 사용자 목록 — 오른쪽 사이드 패널 (android_dispatch_tablet.md §6.3a·§6.12)
//
// 탭 줄 [사용자] 가 연다. PTT 주소록의 사람을 **여럿 골라** 두 가지를 한다: 지금 한 번 이야기하는 **애드혹**(편성 없음 —
// TS 24.379 ad hoc group call), 두고 쓰는 **그룹으로 저장**(GMS 그룹 생성 — TS 24.481, 패널 안에서 한 겹 들어간다).
// 그룹 만들기가 [더보기] 가 아니라 여기인 이유 — 사람을 고르는 자리에서 곧바로 묶는 것이 관제사의 순서다.
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
import com.cims.ue.dispatch.ui.FilterPill
import com.cims.ue.dispatch.ui.PersonAction
import com.cims.ue.dispatch.ui.PersonEntry
import com.cims.ue.dispatch.ui.PersonMenu
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
 * 주소록 → 사용자 줄(순수 함수, 시험 대상). 나는 뺀다 — 나를 골라 애드혹을 열거나 그룹에 두 번 넣을 일이 없다.
 * 상태는 [present](로스터에 접속으로 잡힌 번호, 정규형)가 준다.
 */
internal fun userRows(book: DirectoryBook, me: String, present: Set<String>): List<UserRowUi> {
    val meKey = DirectoryBook.normalize(me)
    return book.entries.asSequence()
        .filter { !it.external && it.msisdn.isNotBlank() && DirectoryBook.normalize(it.msisdn) != meKey }
        .distinctBy { DirectoryBook.normalize(it.msisdn) }
        .map { e ->
            val org = if (e.org.isBlank()) "" else book.orgPath(e.org).substringAfterLast(" › ").ifBlank { e.org }
            UserRowUi(e.msisdn, e.name.ifBlank { e.msisdn }, org,
                listOf("PTT ${e.msisdn}", org).filter { it.isNotBlank() }.joinToString(" · "),
                if (DirectoryBook.normalize(e.msisdn) in present) "접속" else "")
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
 * 사용자 목록 패널 — **VM 을 붙이는 껍데기**.
 *
 * @param present 로스터에 접속으로 잡힌 번호(정규형) — 상태 칸의 근거.
 * @param picked 고른 번호(목록 순서가 아니라 고른 순서) — 패널을 닫아도 남는다(MainViewModel).
 * @param canCreate 그룹 생성 자격 — 없으면 [그룹으로 저장] 을 끈다(최종 판정은 서버 GMS).
 */
@Composable
fun UsersPanel(
    channels: PttChannelsViewModel,
    present: Set<String>,
    picked: List<String>,
    onToggle: (String) -> Unit,
    onClear: () -> Unit,
    canCreate: Boolean,
    pinned: Boolean,
    onPin: () -> Unit,
    onClose: () -> Unit,
    onSaveGroup: () -> Unit,
    onPerson: (PersonAction, String) -> Unit,
) {
    val book by channels.pttBook.collectAsStateWithLifecycle()
    val error by channels.originError.collectAsStateWithLifecycle()
    val rows = remember(book, present) { userRows(book, channels.myPttNumber, present) }
    UsersPanelContent(
        rows = rows, picked = picked, onToggle = onToggle, onClear = onClear,
        canCreate = canCreate, error = error,
        pinned = pinned, onPin = onPin, onClose = onClose,
        onAdhoc = { channels.startAdhoc(picked, emergency = false) { onClear(); onClose() } },
        onSaveGroup = onSaveGroup,
        personAt = channels::personAt, onPerson = onPerson)
}

/** 사용자 목록 본문 — **순수 컴포저블**(검색어·거르기만 제 상태). */
@Composable
fun UsersPanelContent(
    rows: List<UserRowUi>,
    picked: List<String> = emptyList(),
    onToggle: (String) -> Unit = {},
    onClear: () -> Unit = {},
    canCreate: Boolean = true,
    error: String? = null,
    pinned: Boolean = false,
    onPin: () -> Unit = {},
    onClose: () -> Unit = {},
    onAdhoc: () -> Unit = {},
    onSaveGroup: () -> Unit = {},
    personAt: (String) -> PersonEntry? = { null },
    onPerson: (PersonAction, String) -> Unit = { _, _ -> },
) {
    val p = Tokens.palette
    var query by remember { mutableStateOf("") }
    var filter by remember { mutableStateOf(FILTER_ALL) }
    val filters = remember(rows) { userFilters(rows) }
    val shown = remember(rows, filter, query) { rows.filterBy(filter, query) }
    val pickedKeys = remember(picked) { picked.map(DirectoryBook::normalize).toSet() }

    SidePanelFrame(title = "사용자 ${rows.size}", tag = "사용자", pinned = pinned, onPin = onPin, onClose = onClose) {
        SearchField(query, { query = it }, "이름 · PTT 번호 · 조직",
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
                Text(if (rows.isEmpty()) "PTT 주소록을 받지 못했습니다 — 서버 연결을 확인하세요" else "일치하는 사람이 없습니다",
                    fontSize = Type.body, color = p.muted, modifier = Modifier.padding(24.dp))
            }
        }
        // 고른 사람으로 하는 일 — 지금 한 번(애드혹) / 두고 쓰기(그룹).
        Column(
            Modifier.fillMaxWidth()
                .drawBehind { drawLine(p.ink, Offset(0f, 0f), Offset(size.width, 0f), 1.5.dp.toPx()) }
                .padding(start = 16.dp, end = 16.dp, top = 10.dp, bottom = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(if (picked.isEmpty()) "사람을 고르세요" else "${picked.size}명 선택", fontSize = Type.body,
                    fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
                if (picked.isNotEmpty()) TextButton(onClick = onClear) { Text("선택 해제", fontSize = Type.meta) }
            }
            error?.let { Text(it, fontSize = Type.meta, color = p.emergency) }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PillButton("애드혹 열기", onAdhoc, height = 44.dp, strongBorder = true, trailingNote = "지금 한 번",
                    enabled = picked.isNotEmpty(), modifier = Modifier.weight(1f))
                PillButton("그룹으로 저장 ›", onSaveGroup, height = 44.dp, filled = true,
                    enabled = picked.isNotEmpty() && canCreate, modifier = Modifier.weight(1f))
            }
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
        Modifier.fillMaxWidth().height(48.dp).background(if (selected) p.fill else p.paper)
            .drawBehind { drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
            .clickable(onClick = onToggle)
            .padding(start = 16.dp, end = 4.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(Modifier.size(22.dp).clip(RoundedCornerShape(4.dp)).background(if (selected) p.ink else p.paper)
                .border(2.dp, p.ink, RoundedCornerShape(4.dp)), contentAlignment = Alignment.Center) {
            if (selected) Icon(Icons.Filled.Check, contentDescription = null, tint = p.onInk, modifier = Modifier.size(14.dp))
        }
        Column(Modifier.weight(1f)) {
            Text(r.name, fontSize = Type.strong, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(r.meta, fontSize = Type.meta, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (r.status.isNotEmpty()) Text(r.status, fontSize = Type.meta, color = p.ink)
        Box {
            IconButton(onClick = { menu = true }, modifier = Modifier.size(36.dp)) {
                Icon(Icons.Filled.MoreVert, contentDescription = "${r.name} 사람 메뉴", tint = p.muted, modifier = Modifier.size(18.dp))
            }
            if (menu) PersonMenu(person = personAt(r.number), expanded = true, onDismiss = { menu = false },
                onPick = { a, n -> menu = false; onPerson(a, n) })
        }
    }
}
