// 받는 사람 고르기 — «새 대화» 의 공통 화면 (android_dispatch_tablet.md §6.9a)
//
// 메시지 면 둘([무전]›«메시지» SDS · [통화]›«메시지» SMS)이 같이 쓴다. 두 벌로 만들지 않는 이유는 하는
// 일이 같기 때문이다 — 후보를 훑고 하나를 골라 **빈 대화를 연다**. 다른 것은 후보의 출처뿐이다
// (SDS = 편성 그룹 + PTT 주소록 / SMS = 전화 주소록).
//
// **그룹과 사람을 한 목록에 섞지 않는다.** 섹션을 나누고 그룹에는 «그룹» 표를 단다 — 무전 메시지는
// 그룹으로 보내면 편성 전원이 받고 사람으로 보내면 그 사람만 받아, 잘못 고르면 되돌릴 수 없다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog

/**
 * 후보 하나 — 그룹이거나 사람이다.
 *
 * @param key 스레드 키가 될 값 — 그룹 id 또는 번호. 이 값이 그대로 발신 대상이 된다.
 */
data class RecipientOption(
    val key: String,
    val title: String,
    val subtitle: String = "",
    val group: Boolean = false,
)

/**
 * «새 대화» — 후보를 골라 빈 대화를 연다.
 *
 * 별창이 아니라 [Dialog] 인 이유: 고르고 나면 곧바로 뒤의 대화 면으로 돌아가야 하는 **한 걸음짜리**
 * 조작이다. 면을 따로 두면 «쓰다 말고 상대를 바꾸는» 흔한 일이 두 번의 화면 이동이 된다.
 *
 * @param manualHint 비우면 직접 입력 칸을 내지 않는다. 주소록에 없는 번호에 보낼 수 있어야 하는
 *   전화 축에서만 쓴다(무전은 편성·주소록 밖으로 보낼 자리가 없다).
 */
@Composable
fun RecipientPicker(
    title: String,
    options: List<RecipientOption>,
    onPick: (String) -> Unit,
    onDismiss: () -> Unit,
    manualHint: String = "",
) {
    var q by remember { mutableStateOf("") }
    var manual by remember { mutableStateOf("") }
    val hits = remember(options, q) { filterRecipients(options, q) }
    val groups = hits.filter { it.group }
    val people = hits.filterNot { it.group }

    Dialog(onDismissRequest = onDismiss) {
        ForwardPttKeys()                    // 대화상자가 떠 있어도 측면 키는 발언이다(§7)
        Surface(shape = MaterialTheme.shapes.large, tonalElevation = 4.dp,
            modifier = Modifier.width(520.dp).heightIn(max = 560.dp)) {
            Column(Modifier.padding(16.dp)) {
                Text(title, fontSize = Type.title, fontWeight = FontWeight.Bold)
                OutlinedTextField(value = q, onValueChange = { q = it },
                    placeholder = { Text("이름·번호", fontSize = Type.body) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp))

                if (manualHint.isNotEmpty()) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        OutlinedTextField(value = manual, onValueChange = { manual = it },
                            placeholder = { Text(manualHint, fontSize = Type.body) }, singleLine = true,
                            keyboardOptions = KeyboardOptions(
                                keyboardType = KeyboardType.Phone, imeAction = ImeAction.Done),
                            modifier = Modifier.weight(1f))
                        TextButton(onClick = { onPick(manual.trim()) },
                            enabled = manual.isNotBlank()) { Text("열기") }
                    }
                    HorizontalDivider(Modifier.padding(vertical = 6.dp))
                }

                if (hits.isEmpty()) {
                    Box(Modifier.fillMaxWidth().padding(24.dp), contentAlignment = Alignment.Center) {
                        Text(if (options.isEmpty()) "받을 수 있는 상대가 없습니다"
                             else "일치하는 상대가 없습니다",
                            fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
                LazyColumn(Modifier.weight(1f, fill = false)) {
                    if (groups.isNotEmpty()) item(key = "h-g") { SectionLine("그룹 ${groups.size}") }
                    items(groups, key = { "g-" + it.key }) { RecipientRow(it, onPick) }
                    if (people.isNotEmpty()) item(key = "h-p") { SectionLine("사람 ${people.size}") }
                    items(people, key = { "p-" + it.key }) { RecipientRow(it, onPick) }
                }

                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    TextButton(onClick = onDismiss) { Text("닫기") }
                }
            }
        }
    }
}

@Composable
private fun SectionLine(text: String) =
    Text(text, Modifier.padding(top = 8.dp, bottom = 2.dp), fontSize = Type.meta,
        fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.onSurfaceVariant)

@Composable
private fun RecipientRow(o: RecipientOption, onPick: (String) -> Unit) {
    Row(Modifier.fillMaxWidth().clickable { onPick(o.key) }.padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(o.title, fontSize = Type.strong, fontWeight = FontWeight.Bold, maxLines = 1)
            if (o.subtitle.isNotEmpty()) Text(o.subtitle, fontSize = Type.meta, maxLines = 1,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if (o.group) Tag("그룹", MaterialTheme.colorScheme.tertiary, leading = 0)
    }
    HorizontalDivider()
}

/** 이름·번호(부분 일치, 대소문자 무시) — 순수 함수(시험 대상). 순서는 그룹 먼저, 그 안에서 이름순. */
internal fun filterRecipients(options: List<RecipientOption>, q: String): List<RecipientOption> {
    val needle = q.trim().lowercase()
    return options
        .filter {
            needle.isEmpty() || it.title.lowercase().contains(needle) ||
                it.key.lowercase().contains(needle) || it.subtitle.lowercase().contains(needle)
        }
        .sortedWith(compareByDescending<RecipientOption> { it.group }.thenBy { it.title })
}

// ── 미리보기 ────────────────────────────────────────────────────────────────

@androidx.compose.ui.tooling.preview.Preview(name = "새 대화 — 무전(그룹+사람)", showBackground = true)
@Composable
private fun PreviewPickerPtt() = PreviewFrame {
    RecipientPicker(title = "새 무전 메시지 — 받는 곳", options = PREVIEW_PTT,
        onPick = {}, onDismiss = {})
}

/** 전화 축은 **직접 입력 칸**이 하나 더 있다 — 주소록에 없는 내선에도 보낼 수 있어야 한다. */
@androidx.compose.ui.tooling.preview.Preview(name = "새 대화 — 문자(직접 입력)", showBackground = true)
@Composable
private fun PreviewPickerSms() = PreviewFrame {
    RecipientPicker(title = "새 문자 — 받는 사람", options = PREVIEW_SMS,
        manualHint = "번호 직접 입력", onPick = {}, onDismiss = {})
}

private val PREVIEW_PTT = listOf(
    RecipientOption("g001", "순찰1", "편성 5명", group = true),
    RecipientOption("g002", "상황실", "편성 8명", group = true),
    RecipientOption("g003", "교통1", "편성 3명", group = true),
    RecipientOption("1002", "이당직", "1002 · CIMS › 본부 › 당직"),
    RecipientOption("1003", "박현장", "1003 · CIMS › 본부 › 현장"),
    RecipientOption("1004", "최순찰", "1004 · CIMS › 본부 › 순찰"))

private val PREVIEW_SMS = listOf(
    RecipientOption("1002", "이당직", "1002 · CIMS › 본부 › 당직"),
    RecipientOption("1003", "박현장", "1003 · CIMS › 본부 › 현장"),
    RecipientOption("01012345678", "김지원", "01012345678 · CIMS › 지원"))
