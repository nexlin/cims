// 사설콜·애드혹 개설 시트 (docs/design/features/android_dispatch_tablet.md §6.3,
// dispatch_desktop_ui.md §4.1 빠른 발신 줄 + 팝오버)
//
// 데스크톱은 [사설콜 ▾]·[애드혹 ▾] 두 팝오버지만, 태블릿은 팝오버를 시트로 옮긴다(§6.6).
// 둘이 하는 일이 «PTT 주소록에서 대상을 골라 세션을 연다» 로 같아 **한 시트에 탭 둘**로 접는다 —
// 다른 것은 «몇 명인가» 뿐이다(사설콜 1명, 애드혹 N명).
//
// 애드혹 구성 중에는 바깥을 눌러도 닫히지 않는다(§4.1) — 고른 대상이 말없이 사라지면 안 된다.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.Type
import androidx.compose.runtime.LaunchedEffect
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
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry

private enum class OriginTab(val label: String) { PRIVATE("사설콜"), ADHOC("애드혹") }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun OriginateSheet(vm: PttChannelsViewModel, onDismiss: () -> Unit) {
    var tab by remember { mutableStateOf(OriginTab.PRIVATE) }
    var picked by remember { mutableStateOf<List<DirectoryEntry>>(emptyList()) }
    var emergency by remember { mutableStateOf(false) }
    var fullDuplex by remember { mutableStateOf(false) }
    var query by remember { mutableStateOf("") }
    val book by vm.pttBook.collectAsStateWithLifecycle()
    val error by vm.originError.collectAsStateWithLifecycle()

    // 사람 메뉴가 심어 둔 씨앗 — 애드혹 탭으로 열고 그 사람을 미리 골라 둔다(§6.2f).
    //   주소록에 없으면 이름 없이 번호만으로 항목을 만든다 — 고른 것이 안 보이면 «추가가 안 됐다» 로 읽힌다.
    LaunchedEffect(book) {
        val seed = vm.consumeAdhocSeed()
        if (seed.isBlank()) return@LaunchedEffect
        val key = DirectoryBook.normalize(seed)
        val e = book.entries.firstOrNull { DirectoryBook.normalize(it.msisdn) == key }
            ?: DirectoryEntry("", "", seed)
        tab = OriginTab.ADHOC
        if (picked.none { DirectoryBook.normalize(it.msisdn) == key }) picked = picked + e
    }

    // 애드혹을 구성 중이면 바깥 탭으로 닫지 않는다 — 고른 대상이 말없이 사라지면 안 된다(§4.1).
    val composing = tab == OriginTab.ADHOC && picked.isNotEmpty()
    val close = { vm.clearOriginError(); onDismiss() }

    ModalBottomSheet(
        onDismissRequest = { if (!composing) close() },
        properties = ModalBottomSheetDefaults.properties(shouldDismissOnBackPress = !composing),
    ) {
        Column(Modifier.fillMaxWidth().heightIn(min = 360.dp, max = 560.dp)
            .padding(horizontal = 16.dp)) {

            TabRow(selectedTabIndex = tab.ordinal) {
                OriginTab.entries.forEach { t ->
                    Tab(selected = tab == t,
                        onClick = { tab = t; picked = emptyList(); vm.clearOriginError() },
                        text = { Text(t.label) })
                }
            }

            Text(
                if (tab == OriginTab.PRIVATE) "PTT 사용자 한 명과 1:1 세션을 엽니다"
                else "고른 사람들로 임시 세션을 엽니다 — 서버에 편성되지 않습니다",
                fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 6.dp))

            // 고른 대상 — 애드혹은 칩으로 쌓인다.
            if (picked.isNotEmpty()) Row(Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                picked.forEach { e ->
                    InputChip(selected = true, onClick = { picked = picked - e },
                        label = { Text(e.name.ifBlank { e.msisdn }, fontSize = Type.meta) })
                }
            }

            Row(verticalAlignment = Alignment.CenterVertically) {
                FilterChip(selected = emergency, onClick = { emergency = !emergency },
                    label = { Text("긴급", fontSize = Type.meta) })
                if (tab == OriginTab.PRIVATE) {
                    Spacer(Modifier.width(6.dp))
                    // 전이중은 마이크가 늘 열려 있어 발언 대상이 되지 못한다 — 카드의 [음소거]로 다룬다.
                    FilterChip(selected = fullDuplex, onClick = { fullDuplex = !fullDuplex },
                        label = { Text("전이중(마이크 상시)", fontSize = Type.meta) })
                }
            }

            OutlinedTextField(value = query, onValueChange = { query = it },
                placeholder = { Text("이름·PTT 번호", fontSize = Type.strong) }, singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp))

            error?.let {
                Text(it, color = MaterialTheme.colorScheme.error, fontSize = Type.body,
                    modifier = Modifier.padding(bottom = 4.dp))
            }

            val rows = remember(book, query, picked) { pttCandidates(book, query, picked) }
            Box(Modifier.weight(1f)) {
                if (rows.isEmpty()) Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        if (book.entries.isEmpty()) "PTT 주소록을 받지 못했습니다 — 서버 연결을 확인하세요"
                        else "일치하는 사람이 없습니다",
                        fontSize = Type.strong, color = MaterialTheme.colorScheme.onSurfaceVariant)
                } else LazyColumn {
                    items(rows, key = { it.msisdn }) { e ->
                        Row(Modifier.fillMaxWidth()
                                .clickable {
                                    picked = if (tab == OriginTab.PRIVATE) listOf(e) else picked + e
                                    vm.clearOriginError()
                                }
                                .padding(vertical = 9.dp),
                            verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text(e.name.ifBlank { e.msisdn }, fontSize = Type.title, fontWeight = FontWeight.Bold)
                                Text(e.msisdn, fontSize = Type.meta,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                            Text(if (tab == OriginTab.PRIVATE) "걸기" else "추가",
                                fontSize = Type.strong, color = MaterialTheme.colorScheme.primary)
                        }
                        HorizontalDivider()
                    }
                }
            }

            HorizontalDivider()
            Row(Modifier.fillMaxWidth().padding(vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Text(
                    if (tab == OriginTab.PRIVATE) picked.firstOrNull()?.let { "대상 ${it.name.ifBlank { it.msisdn }}" } ?: "대상을 고르세요"
                    else "대상 ${picked.size}명",
                    Modifier.weight(1f), fontSize = Type.body)
                TextButton(onClick = close) { Text("취소") }
                Button(
                    onClick = {
                        if (tab == OriginTab.PRIVATE)
                            vm.startPrivate(picked.first().msisdn, fullDuplex, emergency, close)
                        else vm.startAdhoc(picked.map { it.msisdn }, emergency, close)
                    },
                    enabled = picked.isNotEmpty(),
                ) { Text(if (tab == OriginTab.PRIVATE) "사설콜" else "애드혹 열기") }
            }
            Spacer(Modifier.height(8.dp))
        }
    }
}

/** 후보 — 이미 고른 사람은 뺀다. 이름·번호 검색(정규형). 순수 함수(시험 대상). */
internal fun pttCandidates(book: DirectoryBook, query: String,
                           picked: List<DirectoryEntry>): List<DirectoryEntry> {
    val taken = picked.map { DirectoryBook.normalize(it.msisdn) }.toHashSet()
    val q = query.trim().lowercase()
    val qn = DirectoryBook.normalize(query.trim())
    return book.entries.asSequence()
        .filter { DirectoryBook.normalize(it.msisdn) !in taken }
        .filter {
            q.isEmpty() || it.name.lowercase().contains(q) ||
                (qn.isNotEmpty() && DirectoryBook.normalize(it.msisdn).contains(qn.trimStart('+')))
        }
        .sortedBy { it.name.ifBlank { it.msisdn } }
        .take(200).toList()
}
