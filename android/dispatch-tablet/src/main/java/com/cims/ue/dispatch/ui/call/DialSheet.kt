@file:OptIn(ExperimentalFoundationApi::class)
// [통화] > «주소록» 면 — 전화번호부 (docs/design/features/android_dispatch_tablet.md §6.2b,
// dispatch_desktop_ui.md §4.3 [▦▾] 팝오버 · §6.6 팝오버의 번역)
//
// **행을 누르면 사람 메뉴가 뜬다** — 휴대폰 연락처와 같다. 전에는 탭이 곧 발신이었는데, 한 사람에게 할 수
// 있는 일이 다섯(통화·문자·개별 통화·무전 메시지·기록)이라 탭 하나를 발신에 고정하면 나머지는 롱프레스를
// 아는 사람만 쓰게 되고, 목록을 훑다 잘못 눌러 걸리는 사고도 난다. 바로 걸고 싶으면 행 오른쪽 [📞] 이다.
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

private val hhmm = java.text.SimpleDateFormat("MM-dd HH:mm", java.util.Locale.KOREA)

@OptIn(ExperimentalMaterial3Api::class)
/**
 * [통화] > «주소록» 면 — 전화번호부에서 골라 건다.
 *
 * 전에는 시트 안의 탭 하나였다. 시트는 «잠깐 열고 닫는» 표면인데 주소록은 **거는 일의 주 경로**라
 * 면으로 올렸다. 다이얼패드는 «통화» 면의 키패드가, 최근은 «통화내역» 이 대신한다 — 셋을 한 시트에
 * 모아 둘 이유가 없어졌다.
 */
@Composable
fun ContactsPane(
    vm: CallDeskViewModel,
    onPerson: (PersonAction, String) -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(modifier.fillMaxSize()) {
        Contacts(vm, onPerson, onDial = { n -> vm.dialTo(n) })
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
        // **탭 = 사람 메뉴**(§6.2f) · 오른쪽 [📞] = 바로 발신. 롱프레스도 메뉴를 연다 — 전에 그랬으므로
        //   손에 익은 사람이 헤매지 않게 남겨 둔다(둘 다 같은 결과라 헷갈릴 일이 없다).
        var menuFor by remember { mutableStateOf<String?>(null) }
        LazyColumn(Modifier.fillMaxWidth()) {
            items(rows, key = { it.msisdn }) { e ->
                if (menuFor == e.msisdn) PersonMenu(
                    person = vm.personAt(e.msisdn), expanded = true,
                    onDismiss = { menuFor = null },
                    onPick = { a, n -> onPerson(a, n); menuFor = null })
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)
                        .combinedClickable(onClick = { menuFor = e.msisdn },
                                           onLongClick = { menuFor = e.msisdn })
                        .padding(vertical = 9.dp)) {
                        Text(e.name.ifBlank { e.msisdn }, fontSize = Type.title, fontWeight = FontWeight.Bold)
                        val path = book.orgPath(e.org)
                        Text(listOfNotNull(e.msisdn, path.takeIf { it.isNotBlank() }).joinToString(" · "),
                            fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    IconButton(onClick = { onDial(e.msisdn) }) {
                        Text("📞", fontSize = Type.title)
                    }
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
