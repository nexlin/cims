// 사람 메뉴 — 주소록의 한 사람에게 걸 수 있는 행동을 한 곳에(android_dispatch_tablet.md §6.2f).
//
// 데스크톱 `PersonActionsViewModel` 의 이식이다. 노리는 것 하나 — **서버 전화번호부는 사람이 아니라
// 회선을 준다.** 같은 사람이 PTT 번호와 내선을 따로 갖고 두 축(`phoneBook`/`pttBook`)에 나뉘어 들어오므로,
// 칩 하나를 눌렀을 때 «이 사람에게 사설콜도 통화도 걸 수 있다» 를 보이려면 먼저 사람 단위로 묶어야 한다.
//
// 데스크톱과 다른 것 하나 — **문자(SMS)가 없다.** 외부망 게이트웨이가 서버 과제라 태블릿에도 두지 않는다
// (§11). 없는 기능을 버튼으로 만들지 않는다.
package com.cims.ue.dispatch.ui

import com.cims.ue.dispatch.session.userPart
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry

/**
 * 사람 하나 — 이름·소속과, 그 사람이 가진 회선 둘.
 *
 * 회선이 하나만 있는 사람도 많다(PTT 만 쓰는 현장 인원, 내선만 있는 사무실). 그래서 행동 버튼은
 * **가진 회선에 따라** 나온다 — 없는 회선의 버튼을 비활성으로 두지 않고 아예 그리지 않는다.
 */
data class PersonEntry(
    val name: String,
    val pttNumber: String = "",
    val extension: String = "",
    val orgPath: String = "",
) {
    val hasPtt: Boolean get() = pttNumber.isNotEmpty()
    val hasLine: Boolean get() = extension.isNotEmpty()

    /** 메뉴 머리 — 이름 · PTT · 내선(가진 것만). */
    val head: String
        get() = listOf(name,
                       if (hasPtt) "PTT $pttNumber" else "",
                       if (hasLine) "내선 $extension" else "")
            .filter { it.isNotEmpty() }.joinToString(" · ")
}

/**
 * 두 주소록을 사람 단위로 묶는다 — 순수 함수(시험 대상).
 *
 * 묶는 키는 데스크톱과 같다: **이름 + 조직**. 이름이 없으면(서버가 번호만 준 행) 번호가 키라 따로 선다 —
 * 이름 없는 행끼리 합치면 남남이 한 사람이 된다.
 *
 * 이미 회선 둘을 가진 항목에 같은 키가 또 오면 **번호를 키로 따로 세운다**(데스크톱과 같은 처리). 동명이인이
 * 같은 조직에 있거나 한 사람이 회선을 셋 이상 가진 경우인데, 둘을 구별할 근거가 이름·조직뿐이라 합치면
 * 엉뚱한 사람에게 걸게 된다. 나누면 목록에 두 줄이 보일 뿐이다.
 *
 * @param exclude 정규형 번호 집합 — 내 회선. 나에게 사설콜·통화를 거는 항목이 목록에 있으면 안 된다.
 */
internal fun mergePeople(
    phone: DirectoryBook,
    ptt: DirectoryBook,
    exclude: Set<String> = emptySet(),
): List<PersonEntry> {
    val byKey = LinkedHashMap<String, PersonEntry>()

    fun add(e: DirectoryEntry, isPtt: Boolean, book: DirectoryBook) {
        val num = e.msisdn.trim()
        if (num.isEmpty()) return
        if (DirectoryBook.normalize(num) in exclude) return
        val name = e.name.trim()
        val key = if (name.isNotEmpty()) "$name|${e.org}" else num
        val cur = byKey[key]
        val entry = PersonEntry(
            name = name.ifEmpty { num },
            pttNumber = if (isPtt) num else "",
            extension = if (!isPtt) num else "",
            orgPath = book.orgPath(e.org),
        )
        if (cur == null) { byKey[key] = entry; return }
        if (cur.hasPtt && cur.hasLine) { byKey[num] = entry; return }   // 회선이 이미 둘 — 따로 세운다
        byKey[key] = cur.copy(
            pttNumber = cur.pttNumber.ifEmpty { entry.pttNumber },
            extension = cur.extension.ifEmpty { entry.extension },
            orgPath = cur.orgPath.ifEmpty { entry.orgPath },
        )
    }

    ptt.entries.forEach { add(it, isPtt = true, book = ptt) }
    phone.entries.forEach { add(it, isPtt = false, book = phone) }
    return byKey.values.sortedBy { it.name }
}

/**
 * 번호·URI 하나로 사람을 찾는다 — PTT 번호든 내선이든 같은 사람에 닿아야 한다.
 *
 * 못 찾으면 **그 번호만 가진 항목을 만들어 돌려준다**. 주소록에 없는 상대(외부 번호·방금 바뀐 회선)에도
 * 메뉴가 떠야 하기 때문이다 — 빈 메뉴보다 «통화» 하나라도 있는 편이 낫다. 어느 회선으로 볼지는
 * **전화번호부에 있으면 내선, 아니면 PTT** 로 가른다(데스크톱 `Resolve` 와 같은 판정).
 */
internal fun resolvePerson(
    people: List<PersonEntry>,
    numberOrUri: String,
    phone: DirectoryBook,
    fallbackName: String = "",
): PersonEntry? {
    val n = DirectoryBook.normalize(userPart(numberOrUri))
    if (n.isEmpty()) return null
    people.firstOrNull {
        (it.hasPtt && DirectoryBook.normalize(it.pttNumber) == n) ||
            (it.hasLine && DirectoryBook.normalize(it.extension) == n)
    }?.let { return it }
    val bare = userPart(numberOrUri)
    val inPhone = phone.entries.any { DirectoryBook.normalize(it.msisdn) == n }
    return PersonEntry(
        name = fallbackName.ifEmpty { bare },
        pttNumber = if (inPhone) "" else bare,
        extension = if (inPhone) bare else "",
    )
}

/** `sip:1001@d` · `tel:+8210…` · `1001` → 번호 부분. */

/** 사람 메뉴가 낼 수 있는 행동 — 화면이 이어 붙인다. */
enum class PersonAction { PRIVATE_CALL, ADHOC_ADD, SDS, CALL }

/**
 * 드롭다운 본체. 가진 회선에 있는 행동만 그린다.
 *
 * `onPick` 은 행동과 **그 행동이 쓸 번호**를 함께 준다 — 사설콜·SDS 는 PTT 번호로, 통화는 내선으로 가야
 * 하는데 호출부가 다시 고르게 하면 같은 판정이 두 곳에 생긴다.
 */
@Composable
fun PersonMenu(
    person: PersonEntry?,
    expanded: Boolean,
    onDismiss: () -> Unit,
    onPick: (PersonAction, String) -> Unit,
) {
    if (person == null) return
    DropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        Column(Modifier.padding(horizontal = 12.dp, vertical = 4.dp)) {
            Text(person.head, fontWeight = FontWeight.Bold, fontSize = Type.strong)
            if (person.orgPath.isNotEmpty())
                Text(person.orgPath, fontSize = Type.meta,
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        HorizontalDivider()
        if (person.hasPtt) {
            DropdownMenuItem(text = { Text("사설콜") },
                onClick = { onPick(PersonAction.PRIVATE_CALL, person.pttNumber); onDismiss() })
            DropdownMenuItem(text = { Text("애드혹에 추가") },
                onClick = { onPick(PersonAction.ADHOC_ADD, person.pttNumber); onDismiss() })
            DropdownMenuItem(text = { Text("문자(SDS)") },
                onClick = { onPick(PersonAction.SDS, person.pttNumber); onDismiss() })
        }
        if (person.hasLine) {
            DropdownMenuItem(text = { Text("통화") },
                onClick = { onPick(PersonAction.CALL, person.extension); onDismiss() })
        }
        if (!person.hasPtt && !person.hasLine) {
            Row(Modifier.padding(horizontal = 12.dp, vertical = 6.dp)) {
                Text("걸 수 있는 회선이 없습니다", fontSize = Type.body,
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}
