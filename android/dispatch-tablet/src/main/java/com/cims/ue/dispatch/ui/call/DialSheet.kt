@file:OptIn(ExperimentalFoundationApi::class)
// 발신 시트 — 다이얼패드 · 주소록 · 최근 (docs/design/features/android_dispatch_tablet.md §6.2,
// dispatch_desktop_ui.md §4.3 [▦▾] 팝오버 · §6.6 팝오버의 번역)
//
// 데스크톱의 [▦▾] 팝오버 셋을 태블릿에서는 **전면 시트 하나에 탭 셋**으로 접는다. 셋 다 하는 일이 같기
// 때문이다 — 번호를 하나 골라 건다. 고르면 시트가 닫히고 바로 발신한다(한 번 더 누르게 하지 않는다).
package com.cims.ue.dispatch.ui.call

import com.cims.ue.dispatch.ui.Type
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import com.cims.ue.dispatch.ui.PersonAction
import com.cims.ue.dispatch.ui.PersonMenu
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry

private enum class DialTab(val label: String) { PAD("다이얼패드"), BOOK("주소록"), RECENT("최근") }

private val hhmm = java.text.SimpleDateFormat("MM-dd HH:mm", java.util.Locale.KOREA)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DialSheet(
    vm: CallDeskViewModel,
    onPerson: (PersonAction, String) -> Unit = { _, _ -> },
    onDismiss: () -> Unit,
) {
    var tab by remember { mutableStateOf(DialTab.BOOK) }

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.fillMaxWidth().heightIn(min = 320.dp, max = 560.dp)) {
            TabRow(selectedTabIndex = tab.ordinal) {
                DialTab.entries.forEach { t ->
                    Tab(selected = tab == t, onClick = { tab = t }, text = { Text(t.label) })
                }
            }
            val place: (String) -> Unit = { n -> vm.dialTo(n); onDismiss() }
            when (tab) {
                DialTab.PAD -> Dialpad(vm, place)
                DialTab.BOOK -> Contacts(vm, onPerson, place)
                DialTab.RECENT -> Recent(vm, place)
            }
        }
    }
}

// ── 다이얼패드 ──────────────────────────────────────────────────────────────

@Composable
private fun Dialpad(vm: CallDeskViewModel, onDial: (String) -> Unit) {
    var n by remember { mutableStateOf("") }
    val book by vm.book.collectAsStateWithLifecycle()
    Column(Modifier.fillMaxWidth().padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally) {
        Text(n.ifEmpty { "번호를 누르세요" }, fontSize = Type.huge, fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp))
        // 이름이 잡히면 바로 보여 준다 — 잘못 누른 번호를 걸기 전에 안다.
        val who = if (n.isBlank()) "" else book.nameOf(n)
        Text(who.ifBlank { " " }, fontSize = Type.strong, color = MaterialTheme.colorScheme.primary)
        Spacer(Modifier.height(8.dp))
        listOf("123", "456", "789", "*0#").forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                row.forEach { d ->
                    OutlinedButton(onClick = { n += d },
                        modifier = Modifier.size(78.dp, 52.dp)) {
                        Text(d.toString(), fontSize = Type.head, fontWeight = FontWeight.Bold)
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp),
            verticalAlignment = Alignment.CenterVertically) {
            OutlinedButton(onClick = { n = n.dropLast(1) }, enabled = n.isNotEmpty()) { Text("←") }
            Button(onClick = { onDial(n) }, enabled = n.isNotBlank(),
                modifier = Modifier.height(48.dp).widthIn(min = 140.dp)) {
                Text("발신", fontSize = Type.title, fontWeight = FontWeight.Bold)
            }
        }
    }
}

// ── 주소록 ──────────────────────────────────────────────────────────────────

@Composable
private fun Contacts(
    vm: CallDeskViewModel,
    onPerson: (PersonAction, String) -> Unit,
    onDial: (String) -> Unit,
) {
    val book by vm.book.collectAsStateWithLifecycle()
    var q by remember { mutableStateOf("") }
    var org by remember { mutableStateOf("") }

    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
        OutlinedTextField(value = q, onValueChange = { q = it },
            placeholder = { Text("이름·번호", fontSize = Type.strong) }, singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp))
        if (book.orgs.isNotEmpty()) Row(Modifier.horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            FilterChip(selected = org.isBlank(), onClick = { org = "" },
                label = { Text("전체", fontSize = Type.meta) })
            book.orgs.forEach { o ->
                FilterChip(selected = org == o.code, onClick = { org = if (org == o.code) "" else o.code },
                    label = { Text(o.name.ifBlank { o.code }, fontSize = Type.meta) })
            }
        }
        val rows = remember(book, q, org) { filter(book, q, org) }
        if (rows.isEmpty()) {
            Box(Modifier.fillMaxWidth().padding(24.dp), contentAlignment = Alignment.Center) {
                Text(
                    if (book.entries.isEmpty()) "전화번호부를 받지 못했습니다 — 서버 연결을 확인하세요"
                    else "일치하는 사람이 없습니다",
                    fontSize = Type.strong, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            return@Column
        }
        // 탭 = 발신(종전대로), **롱프레스 = 사람 메뉴**(§6.2f). 주소록은 전화 축이라 탭이 통화인 것이 맞고,
        //   같은 사람의 PTT 행동(사설콜·애드혹·SDS)은 메뉴로 연다.
        var menuFor by remember { mutableStateOf<String?>(null) }
        LazyColumn(Modifier.fillMaxWidth()) {
            items(rows, key = { it.msisdn }) { e ->
                if (menuFor == e.msisdn) PersonMenu(
                    person = vm.personAt(e.msisdn), expanded = true,
                    onDismiss = { menuFor = null },
                    onPick = { a, n -> onPerson(a, n); menuFor = null })
                Row(Modifier.fillMaxWidth()
                        .combinedClickable(onClick = { onDial(e.msisdn) },
                                           onLongClick = { menuFor = e.msisdn })
                        .padding(vertical = 9.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(e.name.ifBlank { e.msisdn }, fontSize = Type.title, fontWeight = FontWeight.Bold)
                        val path = book.orgPath(e.org)
                        Text(listOfNotNull(e.msisdn, path.takeIf { it.isNotBlank() }).joinToString(" · "),
                            fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    Text("발신", fontSize = Type.strong, color = MaterialTheme.colorScheme.primary)
                }
                HorizontalDivider()
            }
        }
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

// ── 최근 ────────────────────────────────────────────────────────────────────

@Composable
private fun Recent(vm: CallDeskViewModel, onDial: (String) -> Unit) {
    val log by vm.callLog.collectAsStateWithLifecycle()
    // 내 통화만, 같은 상대는 최근 한 줄로 접는다.
    // 번호가 없는(구) 행은 다시 걸 수 없으므로 뺀다 — 눌러도 아무 일이 없는 줄을 두지 않는다.
    val rows = remember(log) {
        log.filterNot { it.others || it.number.isBlank() }.distinctBy { it.number }.take(50)
    }
    if (rows.isEmpty()) {
        Box(Modifier.fillMaxWidth().padding(24.dp), contentAlignment = Alignment.Center) {
            Text("최근 통화가 없습니다", fontSize = Type.strong, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        return
    }
    LazyColumn(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
        items(rows, key = { it.number }) { r ->
            Row(Modifier.fillMaxWidth().clickable { onDial(r.number) }.padding(vertical = 9.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(r.label, fontSize = Type.title, fontWeight = FontWeight.Bold)
                    Text(
                        listOfNotNull(
                            hhmm.format(java.util.Date(r.startedAtMs)),
                            r.text.takeIf { it.isNotBlank() },
                            if (r.answered) "통화 ${durText(r.durationSec)}" else null,
                        ).joinToString(" · "),
                        fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Text("발신", fontSize = Type.strong, color = MaterialTheme.colorScheme.primary)
            }
            HorizontalDivider()
        }
    }
}
