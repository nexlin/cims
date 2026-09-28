// [PTT 그룹] 화면 Preview — 좌 목록 : 우 상세(멤버 표) (android_dispatch_tablet.md §6.12)
package com.cims.ue.dispatch.ui.groups

import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
import com.cims.ue.dispatch.session.ManagedGroup
import com.cims.ue.dispatch.ui.PreviewBody
import com.cims.ue.dispatch.ui.PreviewFrame

private fun grp(id: String, name: String, n: Int, owner: Boolean = false,
                member: Boolean = false, scope: Boolean = false, manage: Boolean = true) =
    ManagedGroup(id = id, uri = "tel:$id", name = name, memberCount = n, isOwner = owner,
        orgCode = "OPS", sessionType = "prearranged", canManage = manage,
        inListenScope = scope, isMember = member)

private val ROWS = listOf(
    grp("g001", "순찰1", 12, owner = true, member = true),
    grp("g002", "상황실", 8, member = true),
    grp("g003", "교통1", 15, scope = true, manage = false),
    grp("g004", "야간순찰", 9, scope = true, manage = false),
    grp("g005", "정비반", 6, owner = true),
    grp("g006", "외곽경비", 21, scope = true, manage = false),
    grp("g007", "지원2조", 4, member = true),
)

private fun mem(name: String, num: String, st: String, me: Boolean = false, chair: Boolean = false) =
    DetailMember(name = name, number = num, status = st, isMe = me, isChair = chair)

private val MEMBERS = listOf(
    mem("김관제", "1001", DetailMember.SPEAKING, me = true, chair = true),
    mem("이당직", "1002", DetailMember.JOINED),
    mem("박현장", "1003", DetailMember.JOINED),
    mem("최순찰", "1004", DetailMember.JOINED),
    mem("정정비", "1005", DetailMember.ABSENT),
    mem("한지원", "1006", DetailMember.ABSENT),
    mem("오경비", "1007", DetailMember.ABSENT),
    mem("남교통", "1008", DetailMember.ABSENT),
)

@Preview(name = "PTT 그룹 — 상세", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroups() = PreviewFrame {
    PttGroupsScreenContent(GroupsUi(rows = ROWS, selected = ROWS[0], detail = MEMBERS))
}

/** 멤버가 많은 그룹 — 표가 몇 줄 보이는지. */
@Preview(name = "PTT 그룹 — 멤버 24", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroupsBig() = PreviewFrame {
    PttGroupsScreenContent(GroupsUi(rows = ROWS, selected = ROWS[2],
        detail = (1..24).map { mem("대원$it", "20%02d".format(it),
            if (it <= 7) DetailMember.JOINED else DetailMember.ABSENT) }))
}

@Preview(name = "PTT 그룹 — 고르기 전", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroupsNone() = PreviewFrame {
    PttGroupsScreenContent(GroupsUi(rows = ROWS))
}

@Preview(name = "PTT 그룹 — 조회 실패", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroupsError() = PreviewFrame {
    PttGroupsScreenContent(GroupsUi(rows = emptyList(), error = "그룹 목록을 받지 못했습니다 (503)"))
}
