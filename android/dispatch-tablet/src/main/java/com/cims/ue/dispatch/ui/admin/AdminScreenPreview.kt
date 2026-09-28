// [관리] 화면 Preview — 조직 트리 : 구성원 표 : 편집 폼 세 칸 (android_dispatch_tablet.md §6.13)
package com.cims.ue.dispatch.ui.admin

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.session.AdminScope
import com.cims.ue.dispatch.session.AdminView
import com.cims.ue.dispatch.session.MemberInfo
import com.cims.ue.dispatch.session.NumberInfo
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.ui.PreviewBody
import com.cims.ue.dispatch.ui.PreviewFrame
import com.cims.ue.dispatch.ui.Type

private val ORGS = listOf(
    OrgNode("HQ", "본부"),
    OrgNode("OPS", "관제과", parent = "HQ"),
    OrgNode("OPS1", "1팀", parent = "OPS"),
    OrgNode("OPS2", "2팀", parent = "OPS"),
    OrgNode("FLD", "현장과", parent = "HQ"),
    OrgNode("MNT", "정비과", parent = "HQ"),
)

private fun num(msisdn: String) = NumberInfo(msisdn = msisdn, serviceRef = "voip-default")

private fun person(id: Long, name: String, org: String, title: String, volte: String = "", ptt: String = "") =
    MemberInfo(userId = id, name = name, loginId = "u$id", org = org, title = title,
        volte = volte.takeIf { it.isNotBlank() }?.let(::num),
        ptt = ptt.takeIf { it.isNotBlank() }?.let(::num))

private val MEMBERS = listOf(
    person(1, "김관제", "OPS1", "관제사", volte = "1001", ptt = "5001"),
    person(2, "이당직", "OPS1", "당직", volte = "1002", ptt = "5002"),
    person(3, "박현장", "FLD", "반장", ptt = "5003"),
    person(4, "최순찰", "FLD", "대원", ptt = "5004"),
    person(5, "정정비", "MNT", "기사", volte = "1005"),
    person(6, "한지원", "OPS2", "관제사", volte = "1006", ptt = "5006"),
    person(7, "오경비", "FLD", "대원", ptt = "5007"),
    person(8, "남교통", "FLD", "대원", ptt = "5008"),
    person(9, "서상황", "OPS2", "관제사", volte = "1009", ptt = "5009"),
    person(10, "윤야간", "OPS1", "관제사", volte = "1010", ptt = "5010"),
)

private val VIEW = AdminView(
    scope = AdminScope(groupId = "pg-ops", directoryWrite = "all", orgCode = "HQ"),
    orgs = ORGS, members = MEMBERS)

@Composable
private fun FormStub(text: String) =
    Box(Modifier.fillMaxSize().padding(16.dp), contentAlignment = Alignment.Center) {
        Text(text, fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }

@Preview(name = "관리 — 목록", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewAdmin() = PreviewFrame {
    AdminScreenContent(AdminUi(view = VIEW, members = MEMBERS, org = "OPS"))
}

@Preview(name = "관리 — 편집 중(미저장)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewAdminEditing() = PreviewFrame {
    AdminScreenContent(
        AdminUi(view = VIEW, members = MEMBERS, org = "FLD", dirty = true, editing = true,
            editingUserId = 3),
        formPane = { FormStub("구성원 편집 폼 — 따로 Preview") })
}

@Preview(name = "관리 — 검색 결과 없음", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewAdminNoHit() = PreviewFrame {
    AdminScreenContent(AdminUi(view = VIEW, members = emptyList(), query = "없는이름"))
}

@Preview(name = "관리 — 조회 실패", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewAdminError() = PreviewFrame {
    AdminScreenContent(AdminUi(view = AdminView(), error = "구성원 목록을 받지 못했습니다 (403)"))
}
