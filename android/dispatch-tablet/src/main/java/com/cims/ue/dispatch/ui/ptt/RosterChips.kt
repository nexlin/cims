// ① 채널 카드 3줄 — 로스터 미리보기 (android_dispatch_tablet.md §6.3a).
//
// 데스크톱은 «포커스 카드만 3줄, 로스터 칩(발언 중 녹색 · 나 점선 · +n)» 이다(dispatch_desktop_ui.md §4.1
// «공통 — 채널 카드»). 태블릿도 같다 — 카드 전부에 로스터를 펴면 한 화면에 카드 7장이 들어가지 않는다.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.session.userPart
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.sdk.RosterEntry

/** 로스터 칩 하나. */
data class RosterChip(
    /** 다시 걸 때 쓰는 번호(사람 메뉴가 받는 값). */
    val number: String,
    /** 표시 — 이름이 잡히면 이름, 아니면 번호. */
    val label: String,
    val speaking: Boolean = false,
    val isMe: Boolean = false,
)

/** 칩과, 잘려 나간 수. */
data class RosterPreview(val chips: List<RosterChip>, val more: Int)

/**
 * 로스터 → 칩 미리보기 (순수 함수, 시험 대상).
 *
 * **발언자는 절대 잘리지 않는다.** 카드 한 줄에 다 넣을 수 없어 `+n` 으로 접는데, 하필 지금 말하는 사람이
 * 접힌 쪽에 있으면 카드가 «누가 말하는지» 를 못 보여 준다 — 그게 이 줄의 존재 이유다. 그래서 발언자를
 * 먼저 세우고 나머지를 서버 순서대로 채운다(나도 앞으로 — 내가 그 채널에 있는지가 다음으로 중요하다).
 *
 * 로스터에는 **접속한 사람만** 담긴다(`status == "connected"`). 은닉 청취자는 서버가 로스터에서 빼므로
 * (dispatch_center.md §5.6 `listen_visibility`) 앱이 따로 거를 것이 없다.
 *
 * @param speaker 지금 발언자의 번호·URI(없으면 빈 문자열).
 * @param me 내 PTT 번호.
 * @param max 칩 최대 개수.
 */
internal fun rosterPreview(
    roster: List<RosterEntry>,
    speaker: String,
    me: String,
    nameOf: (String) -> String = { "" },
    max: Int = 6,
): RosterPreview {
    val speakerKey = DirectoryBook.normalize(userPart(speaker))
    val meKey = DirectoryBook.normalize(userPart(me))
    val connected = roster.filter { it.status.equals("connected", ignoreCase = true) }
    val all = connected.map { e ->
        val num = userPart(e.uri)
        val key = DirectoryBook.normalize(num)
        RosterChip(
            number = num,
            label = nameOf(num).ifBlank { num },
            speaking = speakerKey.isNotEmpty() && key == speakerKey,
            isMe = meKey.isNotEmpty() && key == meKey)
    }
    // 발언자 → 나 → 나머지(서버 순서). 안정 정렬이라 같은 등급끼리는 순서가 유지된다.
    val ordered = all.sortedBy { if (it.speaking) 0 else if (it.isMe) 1 else 2 }
    val shown = ordered.take(max.coerceAtLeast(1))
    return RosterPreview(shown, (ordered.size - shown.size).coerceAtLeast(0))
}

