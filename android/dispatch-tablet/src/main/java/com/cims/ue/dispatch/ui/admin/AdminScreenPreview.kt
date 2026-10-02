// [관리] 화면 Preview — 조직 카드 : 구성원 표 : 편집 폼(구성원·조직) (android_dispatch_tablet.md §6.13)
package com.cims.ue.dispatch.ui.admin

import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
import com.cims.ue.dispatch.session.AdminScope
import com.cims.ue.dispatch.session.AdminView
import com.cims.ue.dispatch.session.LineKind
import com.cims.ue.dispatch.session.MemberInfo
import com.cims.ue.dispatch.session.NumberInfo
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.session.ServiceRef
import com.cims.ue.dispatch.ui.PreviewBody
import com.cims.ue.dispatch.ui.PreviewFrame

private val ORGS = listOf(
    OrgNode("HQ", "본부"),
    OrgNode("OPS", "관제과", parent = "HQ"),
    OrgNode("OPS1", "1팀", parent = "OPS"),
    OrgNode("OPS2", "2팀", parent = "OPS"),
    OrgNode("FLD", "현장과", parent = "HQ"),
    OrgNode("MNT", "정비과", parent = "HQ"),
)

private val SERVICES = listOf(
    ServiceRef(LineKind.VOLTE, "volte", "ims.example.org"),
    ServiceRef(LineKind.VOIP, "voip-desk", "ims.example.org"),
    ServiceRef(LineKind.PTT, "ptt", "mcptt.example.org"))

private fun num(msisdn: String, service: String, create: Boolean = false, listen: Boolean = false, pickup: String = "") =
    NumberInfo(msisdn = msisdn, imsi = msisdn, serviceRef = service, sipTransport = "TLS", authScheme = "digest",
        profile = if (service == "ptt") mapOf("allowCreateGroup" to create, "allowAmbientListening" to listen) else emptyMap(),
        pickupGroup = pickup)

private fun person(id: Long, name: String, org: String, title: String, volte: String = "", voip: String = "",
                   ptt: String = "", create: Boolean = false, listen: Boolean = false) =
    MemberInfo(userId = id, name = name, loginId = "u$id", org = org, title = title,
        volte = volte.takeIf { it.isNotBlank() }?.let { num(it, "volte") },
        voip = voip.takeIf { it.isNotBlank() }?.let { num(it, "voip-desk", pickup = "pg-ops") },
        ptt = ptt.takeIf { it.isNotBlank() }?.let { num(it, "ptt", create, listen) })

private val MEMBERS = listOf(
    person(1, "김관제", "OPS1", "관제사", voip = "7001", ptt = "01310001001", create = true, listen = true),
    person(2, "이당직", "OPS1", "당직", volte = "01022223333", ptt = "01310001002", create = true),
    person(3, "박현장", "FLD", "반장", volte = "01033334444", ptt = "01310002001"),
    person(4, "최순찰", "FLD", "대원", ptt = "01310002002"),
    person(5, "정정비", "MNT", "기사", volte = "01055556666"),
    person(6, "한지원", "OPS2", "관제사", volte = "01066667777", voip = "7006", ptt = "01310001006"),
    person(7, "오경비", "FLD", "대원", ptt = "01310002007"),
    person(8, "남교통", "FLD", "대원", ptt = "01310002008"),
    person(9, "서상황", "OPS2", "관제사", voip = "7009", ptt = "01310001009", listen = true),
    person(10, "윤야간", "OPS1", "관제사", volte = "01010101010", ptt = "01310001010"),
)

private val VIEW = AdminView(
    scope = AdminScope(groupId = "pg-ops", directoryWrite = "all", orgCode = "HQ"),
    services = SERVICES, orgs = ORGS, members = MEMBERS)

/** 폼을 연 그대로의 구성원 폼 — VM 의 `open` 과 같은 구성. */
private fun formOf(m: MemberInfo, view: AdminView = VIEW) = MemberForm(
    orig = m, name = m.name, title = m.title, org = m.org, loginId = m.loginId,
    lines = LineKind.all.associateWith { AdminViewModel.openLine(view, it, m.line(it)) },
    allowCreateGroup = m.allowCreateGroup)

/** 폼이 닫힌 목록 — 구성원 표가 넓어 번호 열 셋과 자격 열이 선다. */
@Preview(name = "관리 — 목록(번호 열)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewAdmin() = PreviewFrame {
    AdminScreenContent(AdminUi(view = VIEW, members = MEMBERS, org = "OPS"))
}

/** 구성원 폼 — 회선 셋을 다 가진 사람(개설됨 셋 · 픽업 그룹 · PTT 자격). 표는 좁아져 번호를 이름 아래로 접는다. */
@Preview(name = "관리 — 구성원 편집 폼", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewAdminEditing() = PreviewFrame {
    AdminScreenContent(
        AdminUi(view = VIEW, members = MEMBERS, editing = true, editingUserId = 6),
        formPane = { MemberFormPane(formOf(MEMBERS[5]), VIEW) })
}

/** 폼 전체 — 기본 + 회선 카드 셋을 스크롤 없이 한 번에 본다. */
@Preview(name = "관리 — 구성원 편집 폼(전체 높이)", widthDp = 1200, heightDp = 1500, showBackground = true)
@Composable
private fun PreviewAdminEditingTall() = PreviewFrame {
    AdminScreenContent(
        AdminUi(view = VIEW, members = MEMBERS, dirty = true, editing = true, editingUserId = 6),
        formPane = { MemberFormPane(formOf(MEMBERS[5]).copy(title = "선임 관제사"), VIEW) })
}

/** 번호를 비운 회선(저장하면 삭제) · 새로 넣는 회선(비밀번호 필수) · 저장 실패 문구가 폼 바닥에. */
@Preview(name = "관리 — 회선 삭제·개설과 오류", widthDp = 1200, heightDp = 1500, showBackground = true)
@Composable
internal fun PreviewAdminLines() = PreviewFrame {
    val f = formOf(MEMBERS[1])
    AdminScreenContent(
        AdminUi(view = VIEW, members = MEMBERS, dirty = true, editing = true, editingUserId = 2),
        formPane = {
            MemberFormPane(f.copy(
                lines = f.lines + (LineKind.VOLTE to f.lines.getValue(LineKind.VOLTE).copy(msisdn = "")) +
                    (LineKind.VOIP to f.lines.getValue(LineKind.VOIP).copy(msisdn = "7002")),
                error = "VoIP 새 회선에는 SIP 비밀번호가 필요합니다(H(A1) 결박)"), VIEW)
        })
}

/** 새 구성원 — 접속서비스 후보가 없는 종류(PTT)는 경고 띠가 서고 개설이 막힌다. */
@Preview(name = "관리 — 새 구성원 · 후보 없음", widthDp = 1200, heightDp = 1500, showBackground = true)
@Composable
internal fun PreviewAdminNew() = PreviewFrame {
    val view = VIEW.copy(services = SERVICES.filter { it.kind != LineKind.PTT })
    AdminScreenContent(
        AdminUi(view = view, members = MEMBERS, org = "FLD", editing = true),
        formPane = {
            MemberFormPane(MemberForm(org = "FLD",
                lines = LineKind.all.associateWith { AdminViewModel.openLine(view, it, null) }), view)
        })
}

@Preview(name = "관리 — 조직 편집 폼", device = PreviewBody, showBackground = true)
@Composable
internal fun PreviewAdminOrg() = PreviewFrame {
    AdminScreenContent(
        AdminUi(view = VIEW, members = MEMBERS.filter { it.org.startsWith("OPS") }, org = "OPS", editing = true),
        formPane = { OrgFormPane(ORGS[1], isNew = false, view = VIEW) })
}

@Preview(name = "관리 — 구성원 편집 폼(어둡게)", device = PreviewBody, showBackground = true)
@Composable
private fun PreviewAdminDark() = PreviewFrame(dark = true) {
    AdminScreenContent(
        AdminUi(view = VIEW, members = MEMBERS, dirty = true, editing = true, editingUserId = 1),
        formPane = { MemberFormPane(formOf(MEMBERS[0]), VIEW) })
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
