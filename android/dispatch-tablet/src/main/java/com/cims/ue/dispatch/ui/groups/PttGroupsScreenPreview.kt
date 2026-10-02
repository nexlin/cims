// [PTT 그룹] 화면 Preview — 좌 목록 : 우 상세(정보 칸·능력 칩·멤버 2열) / 인라인 편집 폼 / 새 그룹 패널
// (android_dispatch_tablet.md §6.12)
package com.cims.ue.dispatch.ui.groups

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.session.ManagedGroup
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.ui.PreviewBody
import com.cims.ue.dispatch.ui.PreviewFrame
import com.cims.ue.sdk.GroupDoc
import com.cims.ue.sdk.GroupMember
import com.cims.ue.sdk.McVideoGroupAttrs

private fun grp(id: String, name: String, n: Int, owner: Boolean = false,
                member: Boolean = false, scope: Boolean = false, manage: Boolean = true, org: String = "OPS") =
    ManagedGroup(id = id, uri = "tel:$id", name = name, memberCount = n, isOwner = owner,
        orgCode = org, sessionType = "prearranged", canManage = manage,
        inListenScope = scope, isMember = member)

private val ROWS = listOf(
    grp("g001", "순찰1", 12, owner = true, member = true),
    grp("g002", "상황실", 8, member = true),
    grp("g003", "교통1", 15, scope = true, manage = false, org = "FLD"),
    grp("g004", "야간순찰", 9, scope = true, manage = false, org = "FLD"),
    grp("g005", "정비반", 6, owner = true, org = "MNT"),
    grp("g006", "외곽경비", 21, manage = true, org = "FLD"),
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

private val BOOK = DirectoryBook(
    orgs = listOf(OrgNode("HQ", "본부"), OrgNode("OPS", "관제과", "HQ"), OrgNode("FLD", "현장과", "HQ"),
        OrgNode("MNT", "정비과", "HQ")),
    entries = listOf(
        DirectoryEntry("OPS", "김관제", "1001"), DirectoryEntry("OPS", "이당직", "1002"),
        DirectoryEntry("FLD", "박현장", "1003"), DirectoryEntry("FLD", "최순찰", "1004"),
        DirectoryEntry("MNT", "정정비", "1005"), DirectoryEntry("OPS", "한지원", "1006"),
        DirectoryEntry("FLD", "오경비", "1007"), DirectoryEntry("FLD", "남교통", "1008"),
        DirectoryEntry("OPS", "서상황", "1009"), DirectoryEntry("OPS", "윤야간", "1010")))

/** 능력·한도를 전부 채운 문서 — 정보 칸·능력 칩·편집 폼의 모든 칸이 값으로 선다. */
private val DOC = GroupDoc(
    uri = "tel:g001", displayName = "순찰1", etag = "W/\"7\"",
    members = listOf(
        GroupMember("tel:1001", "김관제", role = "chair", priority = 12, required = true),
        GroupMember("tel:1002", "이당직", priority = 9, required = true),
        GroupMember("tel:1003", "박현장"), GroupMember("tel:1004", "최순찰"),
        GroupMember("tel:1005", "정정비"), GroupMember("tel:1006", "한지원")),
    sessionType = "prearranged", encryption = true, emergencyCall = true, emergencyAlert = true,
    allowSds = true, allowFd = true, requireAffiliation = true, priority = 9, maxParticipants = 40,
    orgCode = "OPS", authorizedUser = "tel:1001",
    hangTimerSec = 20, maxDurationSec = 7200, allowConferenceState = true, maxSdsSize = 20000, maxAutoRecv = 2097152,
    minNumberToStart = 2, ackTimeoutSec = 10, ackAction = "proceed",
    mcvideo = McVideoGroupAttrs(inviteMembers = false, maxDurationSec = 1800, protectMedia = false,
        protectTransmissionControl = false, audioEncodings = listOf("AMR-WB"), videoEncodings = listOf("H264"),
        maxTransmitters = 2, minNumberToStart = 1, groupPriority = 120, receptionHangTimerSec = 45,
        allowConferenceState = true))

private val DETAIL = GroupsUi(rows = ROWS, selected = ROWS[0], detail = MEMBERS, book = BOOK, doc = DOC,
    hasSession = true, videoIds = setOf("g001", "g003"), canCreate = true, listenHidden = true,
    myPttId = "tel:1001", myName = "김관제", hint = "관리 가능 4개 · 나머지는 청취 범위·멤버 그룹(보기만)")

@Preview(name = "PTT 그룹 — 상세(능력·한도 전부)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroups() = PreviewFrame { PttGroupsScreenContent(DETAIL) }

@Preview(name = "PTT 그룹 — 상세(어둡게)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroupsDark() = PreviewFrame(dark = true) { PttGroupsScreenContent(DETAIL) }

/** 보기 전용 행(청취 범위) — [편집]·[삭제] 가 없고 문서를 아직 못 받았다(정보 칸 «…»). */
@Preview(name = "PTT 그룹 — 보기 전용 · 문서 조회 중", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroupsReadOnly() = PreviewFrame {
    PttGroupsScreenContent(DETAIL.copy(selected = ROWS[2], doc = null, detail = emptyList(), detailBusy = true,
        hasSession = false))
}

/** 멤버가 많은 그룹 — 2열 표가 몇 줄 보이는지. */
@Preview(name = "PTT 그룹 — 멤버 24", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewGroupsBig() = PreviewFrame {
    PttGroupsScreenContent(DETAIL.copy(selected = ROWS[2], hasSession = false,
        doc = DOC.copy(mcvideo = null, allowFd = false, encryption = false),
        detail = (1..24).map { mem("대원$it", "20%02d".format(it),
            if (it <= 7) DetailMember.JOINED else DetailMember.ABSENT) }))
}

@Preview(name = "PTT 그룹 — 고르기 전", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroupsNone() = PreviewFrame {
    PttGroupsScreenContent(GroupsUi(rows = ROWS, book = BOOK, canCreate = true))
}

@Preview(name = "PTT 그룹 — 조회 실패", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroupsError() = PreviewFrame {
    PttGroupsScreenContent(GroupsUi(rows = emptyList(), error = "그룹 목록을 받지 못했습니다 (503)"))
}

// ── 편집 폼 ──

/** 편집 폼 표본 — 능력·한도·서비스(MCVideo 켜져 잠김)·필수 멤버까지 전부 채웠다. */
private val EDIT = editFormOf(DOC, ROWS[0], myPttNumber = "1001")

private val CANDIDATES = BOOK.entries.filter { e -> EDIT.members.none { it.number == e.msisdn } }

@Composable
private fun EditScreen(form: EditForm, dark: Boolean = false) = PreviewFrame(dark = dark) {
    PttGroupsScreenContent(DETAIL.copy(locked = true, editing = true),
        editPane = { GroupEditPane(form, CANDIDATES, BOOK) })
}

@Preview(name = "PTT 그룹 — 편집 폼(능력·한도 전부)", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewGroupEdit() = EditScreen(EDIT)

/** 서비스 절까지 내려 본 높이 — 속성 칸은 길어 스크롤한다. 넉넉한 높이로 절 넷을 한 번에 본다. */
@Preview(name = "PTT 그룹 — 편집 폼(절 넷 전부)", widthDp = 1200, heightDp = 1500, showBackground = true)
@Composable
private fun PreviewGroupEditTall() = EditScreen(EDIT)

@Preview(name = "PTT 그룹 — 편집 폼(어둡게)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroupEditDark() = EditScreen(EDIT, dark = true)

/** 새 그룹 — 나만 의장으로 든 빈 폼. MCVideo 는 꺼져 있고 켤 수 있다. */
@Preview(name = "PTT 그룹 — 새 그룹 폼", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewGroupNew() = EditScreen(EditForm(isNew = true, uri = "tel:g-1a2b3c4d", groupId = "g-1a2b3c4d",
    orgCode = "OPS", members = listOf(MemberRow("tel:1001", "김관제", "1001", isChair = true, isMe = true)), loaded = true))

@Preview(name = "PTT 그룹 — 편집 폼 저장 실패(412)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroupEditError() = EditScreen(EDIT.copy(
    error = "다른 곳에서 먼저 바뀌었습니다 — 최신 문서로 다시 열었습니다. 고칠 값을 확인하고 다시 저장하세요 (412)"))

@Preview(name = "PTT 그룹 — 편집 폼 문서 조회 중", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewGroupEditLoading() = EditScreen(EditForm(isNew = false, uri = "tel:g001", groupId = "g001",
    name = "순찰1", busy = true))

// ── 새 PTT 그룹 패널 ──

private val PANEL_FORM = EditForm(isNew = true, uri = "tel:g-1a2b3c4d", groupId = "g-1a2b3c4d", name = "3번 게이트 대응",
    members = listOf(MemberRow("tel:1001", "김관제", "1001", isChair = true, isMe = true),
        MemberRow("tel:1002", "이당직", "1002"), MemberRow("tel:1003", "박현장", "1003"),
        MemberRow("tel:1004", "최순찰", "1004")), loaded = true)

@Composable
private fun PanelFrame(body: @Composable () -> Unit) = PreviewFrame {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.TopEnd) { body() }
}

@Preview(name = "새 PTT 그룹 패널 — 줄인 폼", widthDp = 400, heightDp = 608, showBackground = true)
@Composable
private fun PreviewNewGroupPanel() = PanelFrame { NewGroupPanelContent(form = PANEL_FORM) }

/** [▸ 고급 설정] 을 편 채 — [PTT 그룹] 화면 폼과 같은 절(그룹 호 · 허용·한도 · 서비스)이 패널 폭에 선다. */
@Preview(name = "새 PTT 그룹 패널 — 고급 설정", widthDp = 400, heightDp = 1500, showBackground = true)
@Composable
internal fun PreviewNewGroupPanelAdvanced() = PanelFrame {
    NewGroupPanelContent(form = PANEL_FORM.copy(mcVideo = true), advancedOpen = true)
}
