@file:OptIn(ExperimentalFoundationApi::class)
// [통화] › [주소록] — 전화번호부 오른쪽 사이드 패널 (docs/design/features/android_dispatch_tablet.md §6.2b,
// dispatch_desktop_ui.md §4.3 [▦▾] 팝오버 · §6.6 팝오버의 번역)
//
// **면이 아니라 패널이다** — 탭 줄 [주소록] 이 어느 통화 면에서든 연다. 다이얼패드를 보며, 문자를 쓰며, 내역을 보며 옆에 펴 두고
// 곧바로 건다(고정하면 면을 옮겨도 남는다). 행 오른쪽 [발신]·[문자] 가 한 번에 하는 두 가지, **행을 누르면 사람 메뉴**다 —
// 한 사람에게 할 수 있는 일이 다섯(통화·문자·개별 통화·무전 메시지·기록)이라 행 탭 하나를 발신에 고정하면 나머지는 숨고,
// 목록을 훑다 잘못 눌러 걸리는 사고도 난다.
package com.cims.ue.dispatch.ui.call

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.Sms
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
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
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.ui.FilterPill
import com.cims.ue.dispatch.ui.PersonAction
import com.cims.ue.dispatch.ui.PersonEntry
import com.cims.ue.dispatch.ui.PersonMenu
import com.cims.ue.dispatch.ui.SidePanelFrame
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type
import com.cims.ue.dispatch.ui.ptt.SearchField

/**
 * 주소록 패널 — **VM 을 붙이는 껍데기**.
 *
 * [발신] 은 곧바로 건다(`dialTo`) — 패널은 그대로라 이어서 다른 사람을 볼 수 있고, 건 통화는 왼쪽 고정 칸 «내 통화» 에 선다.
 * [문자] 와 사람 메뉴는 [onPerson] 으로 올린다 — 문자는 «메시지» 면으로, 개별 통화는 [무전] 으로 옮겨 가야 하므로 둘을 다 아는
 * 곳(`MainViewModel`)이 잇는다.
 */
@Composable
fun BookPanel(
    vm: CallDeskViewModel,
    pinned: Boolean,
    onPin: () -> Unit,
    onClose: () -> Unit,
    onPerson: (PersonAction, String) -> Unit,
) {
    val book by vm.book.collectAsStateWithLifecycle()
    BookPanelContent(book = book, pinned = pinned, onPin = onPin, onClose = onClose,
        onDial = vm::dialTo, onSms = { n -> onPerson(PersonAction.SMS, n) },
        personAt = vm::personAt, onPerson = onPerson)
}

/** 주소록 본문 — **순수 컴포저블**(검색어·조직만 제 상태). */
@Composable
fun BookPanelContent(
    book: DirectoryBook,
    pinned: Boolean = false,
    onPin: () -> Unit = {},
    onClose: () -> Unit = {},
    onDial: (String) -> Unit = {},
    onSms: (String) -> Unit = {},
    personAt: (String) -> PersonEntry? = { null },
    onPerson: (PersonAction, String) -> Unit = { _, _ -> },
) {
    val p = Tokens.palette
    var q by remember { mutableStateOf("") }
    var org by remember { mutableStateOf("") }
    val rows = remember(book, q, org) { filter(book, q, org) }
    // 거르기 칩 — «전체» 가 늘 처음, 그다음은 **트리 순서**(부모 → 자식, 형제는 `sort`·이름 — 관리 화면의 조직 트리와 같다).
    //   고르면 그 조직과 하위 전부다(`filter`).
    val orgs = remember(book) { com.cims.ue.dispatch.session.flattenOrgs(book.orgs).map { it.first } }

    SidePanelFrame(title = "주소록 ${book.entries.size}", tag = "주소록", pinned = pinned, onPin = onPin, onClose = onClose) {
        SearchField(q, { q = it }, "이름 · 번호", Modifier.padding(start = 16.dp, end = 16.dp, top = 10.dp, bottom = 6.dp))
        if (book.orgs.isNotEmpty()) Row(
            Modifier.horizontalScroll(rememberScrollState()).padding(start = 16.dp, end = 16.dp, top = 2.dp, bottom = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            FilterPill("전체", org.isBlank(), onClick = { org = "" })
            orgs.forEach { o ->
                FilterPill(o.name.ifBlank { o.code }, org == o.code, onClick = { org = if (org == o.code) "" else o.code })
            }
        }
        LazyColumn(Modifier.weight(1f)) {
            items(rows, key = { it.msisdn }) { e ->
                BookRow(e, book.orgPath(e.org), onDial = { onDial(e.msisdn) }, onSms = { onSms(e.msisdn) },
                    personAt = personAt, onPerson = onPerson)
            }
            if (rows.isEmpty()) item {
                Text(if (book.entries.isEmpty()) "전화번호부를 받지 못했습니다 — 서버 연결을 확인하세요" else "일치하는 사람이 없습니다",
                    fontSize = Type.body, color = p.muted, modifier = Modifier.padding(24.dp))
            }
        }
    }
}

@Composable
private fun BookRow(
    e: DirectoryEntry,
    orgPath: String,
    onDial: () -> Unit,
    onSms: () -> Unit,
    personAt: (String) -> PersonEntry?,
    onPerson: (PersonAction, String) -> Unit,
) {
    val p = Tokens.palette
    var menu by remember { mutableStateOf(false) }
    Row(
        Modifier.fillMaxWidth().height(56.dp).background(p.paper)
            .drawBehind { drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
            .combinedClickable(onClick = { menu = true }, onLongClick = { menu = true })
            .padding(start = 16.dp, end = 4.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Column(Modifier.weight(1f)) {
            Text(e.name.ifBlank { e.msisdn }, fontSize = Type.strong, fontWeight = FontWeight.SemiBold, maxLines = 1,
                overflow = TextOverflow.Ellipsis)
            Text(listOfNotNull(e.msisdn, orgPath.takeIf { it.isNotBlank() }).joinToString(" · "),
                fontSize = Type.meta, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        IconButton(onClick = onDial, modifier = Modifier.size(44.dp)) {
            Icon(Icons.Filled.Call, contentDescription = "${e.name.ifBlank { e.msisdn }} 발신", tint = p.ink,
                modifier = Modifier.size(20.dp))
        }
        IconButton(onClick = onSms, modifier = Modifier.size(44.dp)) {
            Icon(Icons.Filled.Sms, contentDescription = "${e.name.ifBlank { e.msisdn }} 문자", tint = p.ink,
                modifier = Modifier.size(20.dp))
        }
        if (menu) PersonMenu(person = personAt(e.msisdn), expanded = true, onDismiss = { menu = false },
            onPick = { a, n -> menu = false; onPerson(a, n) })
    }
}

/** 이름·번호 검색 + 조직(하위 포함) 필터. 정규형으로 비교해 `010…` 과 `+8210…` 이 같게 걸린다. */
internal fun filter(book: DirectoryBook, q: String, org: String): List<DirectoryEntry> {
    val needle = q.trim().lowercase()
    val digits = DirectoryBook.normalize(q.trim())
    val subtree = if (org.isBlank()) null else subtreeOf(book, org)
    return book.entries.asSequence()
        .filter { subtree == null || it.org in subtree }
        .filter {
            needle.isEmpty() || it.name.lowercase().contains(needle) ||
                (digits.isNotEmpty() && DirectoryBook.normalize(it.msisdn).contains(digits.trimStart('+')))
        }
        .sortedBy { it.name.ifBlank { it.msisdn } }
        .toList()
}

private fun subtreeOf(book: DirectoryBook, code: String): Set<String> {
    val out = linkedSetOf(code)
    var added = true
    while (added) {
        added = false
        book.orgs.forEach { if (it.parent in out && out.add(it.code)) added = true }
    }
    return out
}
