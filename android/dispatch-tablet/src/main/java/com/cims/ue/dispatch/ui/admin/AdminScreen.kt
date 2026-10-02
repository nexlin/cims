// [관리] 화면 (docs/design/features/android_dispatch_tablet.md §6.13, dispatch_desktop_ui.md §4.5)
//
// 세 카드 — 조직(«전체» + 트리, 선택 = 하위 포함 필터) | 구성원 표 | 편집 폼(구성원 = 기본 + 회선 카드 셋, 조직 = 한 절).
// [PTT 그룹] 화면과 같은 시각 언어·같은 폼 조각이다(groups/FormParts.kt). 서버가 관리 범위로 걸러 준 것만 보인다.
// 행 한 번 누르기 = 오른쪽 폼에 바로 편집. 편집 폼 카드는 **폼이 열렸을 때만** 선다 — 닫혀 있으면 구성원 표가 그 폭을 받아
// 번호 열 셋(VoLTE·VoIP·PTT)과 자격 열을 편다. 폼이 열려 표가 좁아지면 번호·자격을 이름 아래로 접는다.
package com.cims.ue.dispatch.ui.admin

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Groups
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.AdminView
import com.cims.ue.dispatch.session.LineKind
import com.cims.ue.dispatch.session.MemberInfo
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.session.SIP_TRANSPORTS
import com.cims.ue.dispatch.session.flattenOrgs
import com.cims.ue.dispatch.ui.ForwardPttKeys
import com.cims.ue.dispatch.ui.HDivider
import com.cims.ue.dispatch.ui.Initial
import com.cims.ue.dispatch.ui.Label
import com.cims.ue.dispatch.ui.LabelStyle
import com.cims.ue.dispatch.ui.Pill
import com.cims.ue.dispatch.ui.PillButton
import com.cims.ue.dispatch.ui.Segmented
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type
import com.cims.ue.dispatch.ui.avatarHueOf
import com.cims.ue.dispatch.ui.groups.CardFrame
import com.cims.ue.dispatch.ui.groups.CountTitle
import com.cims.ue.dispatch.ui.groups.EmptyNote
import com.cims.ue.dispatch.ui.groups.FieldRow
import com.cims.ue.dispatch.ui.groups.FormCard
import com.cims.ue.dispatch.ui.groups.FormDropdown
import com.cims.ue.dispatch.ui.groups.FormField
import com.cims.ue.dispatch.ui.groups.FormFooter
import com.cims.ue.dispatch.ui.groups.FormHint
import com.cims.ue.dispatch.ui.groups.FormSwitch
import com.cims.ue.dispatch.ui.groups.IconSquare
import com.cims.ue.dispatch.ui.groups.Labeled
import com.cims.ue.dispatch.ui.groups.NumberField
import com.cims.ue.dispatch.ui.groups.SearchBox
import com.cims.ue.dispatch.ui.groups.SoftBand

/** 구성원 표가 번호 열을 펴는 폭 — 이보다 좁으면 번호·자격을 이름 아래로 접는다(데스크톱 `IsNarrow` 와 같은 규칙). */
private val WideTable = 780.dp
// 번호 열은 E.164 한 줄(«+821310001001» — 13자리)이 옆 열에 붙지 않는 폭이다.
private val ColVolte = 136.dp
private val ColVoip = 136.dp
private val ColPtt = 136.dp
private val ColFlags = 140.dp

@Composable
fun AdminScreen(vm: AdminViewModel, modifier: Modifier = Modifier) {
    if (!vm.available) return AdminNoScope(modifier)

    val view by vm.view.collectAsStateWithLifecycle()
    val members by vm.members.collectAsStateWithLifecycle()
    val form by vm.form.collectAsStateWithLifecycle()
    val orgForm by vm.orgForm.collectAsStateWithLifecycle()
    val orgIsNew by vm.orgFormIsNew.collectAsStateWithLifecycle()
    val loading by vm.loading.collectAsStateWithLifecycle()
    val error by vm.error.collectAsStateWithLifecycle()
    val org by vm.org.collectAsStateWithLifecycle()
    val query by vm.query.collectAsStateWithLifecycle()

    LaunchedEffect(Unit) { if (view.members.isEmpty()) vm.load() }

    val orgNode = orgForm
    val memberForm = form
    AdminScreenContent(
        ui = AdminUi(view = view, members = members, org = org, query = query,
            loading = loading, error = error, dirty = vm.dirty, editing = memberForm != null || orgNode != null,
            // 조직 폼이 위에 서 있는 동안은 «편집 중인 구성원» 이 없다 — 그 줄을 누르면 구성원 폼으로 돌아간다.
            editingUserId = if (orgNode == null) memberForm?.orig?.userId ?: 0 else 0),
        act = AdminActions(
            reload = { vm.load(force = true) }, selectOrg = vm::selectOrg, newOrg = vm::newOrg,
            newMember = vm::newMember, search = vm::search, open = vm::open,
            editOrg = vm::editOrg, deleteOrg = vm::deleteOrg),
        // 폼은 조직 폼이 먼저다 — 구성원 폼 위에 서고, 닫히면 고치던 구성원 폼이 돌아온다.
        formPane = {
            when {
                orgNode != null -> OrgFormPane(orgNode, orgIsNew, view, error,
                    OrgFormActions(change = vm::updateOrgForm, cancel = vm::closeOrgForm, save = vm::saveOrg))
                memberForm != null -> MemberFormPane(memberForm, view, error,
                    MemberFormActions(change = vm::update, changeLine = vm::updateLine, cancel = vm::closeForm,
                        save = vm::save, delete = vm::deleteMember))
            }
        },
        modifier = modifier)
}

@Composable
private fun AdminNoScope(modifier: Modifier) =
    Box(modifier.fillMaxSize().background(Tokens.palette.bar), contentAlignment = Alignment.Center) {
        Text("관리 범위 미배정 — 콘솔 «관리 > 역할» 에서 관리 범위(directory_write)를 받아야 합니다",
            color = Tokens.palette.muted, fontSize = Type.strong)
    }

/** [관리] 화면이 그리는 데 필요한 값 전부. */
data class AdminUi(
    val view: AdminView,
    val members: List<MemberInfo> = emptyList(),
    val org: String = "",
    val query: String = "",
    val loading: Boolean = false,
    val error: String = "",
    /** 저장하지 않은 폼이 있는가 — 구성원 머리 라벨·«변경 버림» 확인(§4.5). */
    val dirty: Boolean = false,
    /** 편집 폼(구성원 또는 조직)이 열려 있다 — 오른쪽에 폼 카드가 서고, 오류는 폼 바닥에 뜬다. */
    val editing: Boolean = false,
    /** 편집 중인 구성원 — 표에서 그 줄을 강조한다. 0 = 없음. */
    val editingUserId: Long = 0,
)

data class AdminActions(
    val reload: () -> Unit = {},
    val selectOrg: (String) -> Unit = {},
    val newOrg: () -> Unit = {},
    val newMember: () -> Unit = {},
    val search: (String) -> Unit = {},
    val open: (MemberInfo) -> Unit = {},
    val editOrg: (OrgNode) -> Unit = {},
    val deleteOrg: (String) -> Unit = {},
)

/** 구성원 폼의 조작. */
data class MemberFormActions(
    val change: ((MemberForm) -> MemberForm) -> Unit = {},
    val changeLine: (String, (LineForm) -> LineForm) -> Unit = { _, _ -> },
    val cancel: () -> Unit = {},
    val save: () -> Unit = {},
    val delete: (MemberInfo) -> Unit = {},
)

/** 조직 폼의 조작. */
data class OrgFormActions(
    val change: ((OrgNode) -> OrgNode) -> Unit = {},
    val cancel: () -> Unit = {},
    val save: () -> Unit = {},
)

/** [관리] 본문 — **순수 컴포저블**. 편집 폼만 호출자가 넘긴다([MemberFormPane]·[OrgFormPane]). */
@Composable
fun AdminScreenContent(
    ui: AdminUi,
    act: AdminActions = AdminActions(),
    formPane: @Composable () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    val p = Tokens.palette
    var discard by remember { mutableStateOf<MemberInfo?>(null) }
    var confirmOrg by remember { mutableStateOf<OrgNode?>(null) }

    Row(modifier.fillMaxSize().background(p.bg).padding(horizontal = 16.dp, vertical = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(14.dp)) {
        OrgCard(ui, act, onDelete = { confirmOrg = it }, modifier = Modifier.width(232.dp).fillMaxHeight())
        MemberCard(ui, act, Modifier.weight(1f).fillMaxHeight()) { m ->
            when {
                // 편집 중인 그 구성원 — 다시 눌러도 입력을 지우지 않는다
                ui.editingUserId != 0L && m.userId == ui.editingUserId -> Unit
                ui.dirty -> discard = m
                else -> act.open(m)
            }
        }
        if (ui.editing) CardFrame(Modifier.width(404.dp).fillMaxHeight()) { formPane() }
    }

    // 변경이 있는 채로 다른 구성원을 누르면 확인 — 아니오면 선택이 되돌아간다(§4.5).
    discard?.let { target ->
        AlertDialog(
            onDismissRequest = { discard = null },
            title = { ForwardPttKeys(); Text("변경 버림") },
            text = { Text("저장하지 않은 변경이 있습니다. 버리고 «${target.name}» 을(를) 열까요?") },
            confirmButton = { TextButton(onClick = { discard = null; act.open(target) }) { Text("버리고 열기") } },
            dismissButton = { TextButton(onClick = { discard = null }) { Text("취소") } })
    }

    confirmOrg?.let { o ->
        AlertDialog(
            onDismissRequest = { confirmOrg = null },
            title = { ForwardPttKeys(); Text("조직 삭제") },
            text = { Text("조직 «${o.name.ifBlank { o.code }}» (${o.code}) 을(를) 삭제할까요?\n하위 조직·구성원이 남아 있으면 지울 수 없습니다.") },
            confirmButton = {
                TextButton(onClick = { confirmOrg = null; act.deleteOrg(o.code) }) { Text("삭제", color = p.emg) }
            },
            dismissButton = { TextButton(onClick = { confirmOrg = null }) { Text("취소") } })
    }
}

// ── 조직 — «전체» + 트리, 바닥 = 고른 조직 [편집][삭제] ─────────────────────────

@Composable
private fun OrgCard(ui: AdminUi, act: AdminActions, onDelete: (OrgNode) -> Unit, modifier: Modifier) {
    val p = Tokens.palette
    val view = ui.view
    val flat = remember(view.orgs) { flattenOrgs(view.orgs) }
    val counts = remember(view.members) { view.members.groupingBy { it.org }.eachCount() }
    val cur = view.orgs.firstOrNull { it.code == ui.org }
    CardFrame(modifier) {
        Row(Modifier.padding(start = 16.dp, end = 12.dp, top = 10.dp, bottom = 10.dp),
            verticalAlignment = Alignment.CenterVertically) {
            CountTitle("조직", view.orgs.size, Modifier.weight(1f))
            // 고른 조직 아래에 만든다(오른쪽에 폼).
            PillButton("새 조직", act.newOrg, kind = Pill.LINE, height = 32.dp, leading = Icons.Filled.Add)
        }
        HDivider(hair = true)
        LazyColumn(Modifier.weight(1f)) {
            // «전체» — 조직 필터 풀기(범위 안 구성원 전부)
            item {
                OrgRow(Icons.Filled.Groups, "전체", view.members.size, depth = 0, selected = ui.org.isBlank(), bold = true) {
                    act.selectOrg("")
                }
            }
            items(flat, key = { it.first.code }) { (o, depth) ->
                OrgRow(Icons.Filled.Folder, o.name.ifBlank { o.code }, counts[o.code] ?: 0, depth, selected = ui.org == o.code) {
                    act.selectOrg(o.code)
                }
            }
        }
        HDivider(hair = true)
        Column(Modifier.padding(start = 16.dp, end = 12.dp, top = 10.dp, bottom = 10.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(cur?.name?.ifBlank { cur.code } ?: "조직을 고르세요", fontSize = Type.body, fontWeight = FontWeight.SemiBold,
                    color = if (cur != null) p.ink else p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f))
                PillButton("편집", { cur?.let(act.editOrg) }, height = 30.dp, enabled = cur != null)
                // 하위 조직·구성원이 남아 있으면 서버가 거절한다 — 확인을 받고 보낸다.
                PillButton("삭제", { cur?.let(onDelete) }, kind = Pill.RED, height = 30.dp, enabled = cur != null)
            }
            FormHint(listOf(AdminViewModel.scopeText(view), "전화 그룹 편성(멤버 · 대표번호)과 역할은 운영 콘솔에서 합니다.")
                .filter { it.isNotEmpty() }.joinToString(" · "))
        }
    }
}

/** 조직 트리의 한 줄(40) — 깊이만큼 들여 쓴다. 고른 줄 = 연한 남색 면 + 왼쪽 남색 띠. 수는 그 조직에 **직접** 속한 인원이다. */
@Composable
private fun OrgRow(icon: ImageVector, name: String, count: Int, depth: Int, selected: Boolean, bold: Boolean = false,
                   onClick: () -> Unit) {
    val p = Tokens.palette
    Row(Modifier.fillMaxWidth().height(40.dp).background(if (selected) p.primarySoft else Color.Transparent)
            .drawBehind { if (selected) drawRect(p.primaryLine, size = Size(3.dp.toPx(), size.height)) }
            .clickable(onClick = onClick)
            .padding(start = (13 + depth * 14).dp, end = 14.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Icon(icon, contentDescription = null, tint = if (selected) p.primaryInk else p.wire, modifier = Modifier.size(15.dp))
        Text(name, fontSize = Type.body, fontWeight = if (bold || selected) FontWeight.SemiBold else FontWeight.Normal,
            color = if (selected) p.primaryInk else p.ink, maxLines = 1, overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f).padding(start = 8.dp))
        if (count > 0) Text("$count", fontSize = Type.meta, color = p.muted)
    }
}

/**
 * 조직 고르기 목록 — 평탄화에서 **고를 수 없는 것을 뺀 것**(순수 함수, 시험 대상).
 *
 * @param excludeSubtreeOf 이 코드와 그 하위 전체를 뺀다. 조직을 편집할 때 자기 자신·자손을 상위로 고르면
 *   고리가 생긴다 — 서버가 막더라도 **고를 수 있게 두면 안 된다**(고르고 저장 눌러서 실패하는 UI 는
 *   그 자체가 결함이다). 구성원 «소속» 처럼 뺄 것이 없으면 null.
 */
internal fun orgChoices(orgs: List<OrgNode>, excludeSubtreeOf: String? = null): List<Pair<OrgNode, Int>> {
    val all = flattenOrgs(orgs)
    if (excludeSubtreeOf.isNullOrBlank()) return all
    val byParent = orgs.groupBy { it.parent }
    val banned = HashSet<String>()
    fun mark(code: String) {
        if (!banned.add(code)) return
        byParent[code].orEmpty().forEach { mark(it.code) }
    }
    mark(excludeSubtreeOf)
    return all.filter { it.first.code !in banned }
}

/**
 * 조직 선택 콤보 — 데스크톱 `DirectoryAdminViewModel.OrgParent`(평탄화 목록 + 들여쓰기)의 이식.
 *
 * 조직은 **고르는 것이지 치는 것이 아니다.** 조직 코드는 사람이 외우는 값이 아니라 오타가 나면 조용히 다른 조직에
 * 붙거나 저장이 400 으로 떨어진다. 트리는 이미 화면에 있으므로 그 목록을 그대로 고르게 한다.
 *
 * @param noneLabel 빈 값(없음·최상위)을 고를 수 있으면 그 글자, 아니면 null.
 */
@Composable
private fun OrgPicker(
    value: String,
    choices: List<Pair<OrgNode, Int>>,
    noneLabel: String?,
    modifier: Modifier = Modifier,
    onPick: (String) -> Unit,
) {
    val picked = choices.firstOrNull { it.first.code == value }?.first
    // 고른 값이 목록에 없으면 코드를 그대로 보인다 — 범위 밖 조직에 붙어 있는 구성원을 «없음» 으로 보이면
    //   저장할 때 소속이 조용히 바뀐다.
    val shown = when {
        value.isBlank() -> noneLabel.orEmpty()
        picked != null -> picked.name.ifBlank { picked.code } + " (" + picked.code + ")"
        else -> "$value (범위 밖)"
    }
    val options = listOfNotNull(noneLabel?.let { "" to it }) +
        choices.map { (o, depth) -> o.code to ("   ".repeat(depth) + o.name.ifBlank { o.code } + "  (" + o.code + ")") }
    FormDropdown(shown, options, text = { it.second }, onPick = { onPick(it.first) }, modifier = modifier,
        placeholder = value.isBlank())
}

// ── 구성원 표 ───────────────────────────────────────────────────────────────

@Composable
private fun MemberCard(ui: AdminUi, act: AdminActions, modifier: Modifier, onOpen: (MemberInfo) -> Unit) {
    val p = Tokens.palette
    val view = ui.view
    val orgName = view.orgs.firstOrNull { it.code == ui.org }?.let { it.name.ifBlank { it.code } }
    CardFrame(modifier) {
        Column(Modifier.padding(start = 16.dp, end = 12.dp, top = 10.dp, bottom = 10.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                CountTitle("구성원", ui.members.size)
                Text(if (orgName == null) "범위 전체" else "$orgName 하위 포함", fontSize = Type.meta, color = p.muted,
                    maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f).padding(start = 6.dp))
                // 열림 ≠ 변경 — 열 때와 달라진 동안에만 붙는다.
                if (ui.dirty) Label("저장하지 않은 변경", LabelStyle.PILOT)
                IconSquare(Icons.Filled.Refresh, "새로고침", act.reload)
                PillButton("새 구성원", act.newMember, kind = Pill.INK, height = 32.dp, leading = Icons.Filled.Add)
            }
            SearchBox(ui.query, act.search, "이름 · 아이디 · 번호 검색", Modifier.widthIn(max = 340.dp))
            // 오류 — 폼이 열려 있으면 폼 바닥([저장] 옆), 아니면 여기.
            if (ui.error.isNotBlank() && !ui.editing) SoftBand(ui.error)
        }
        if (ui.loading) LinearProgressIndicator(Modifier.fillMaxWidth().height(2.dp))
        BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
            val wide = maxWidth >= WideTable
            Column(Modifier.fillMaxSize()) {
                // 열 머리
                Row(Modifier.fillMaxWidth().background(p.canvas)
                        .drawBehind {
                            drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx())
                            drawLine(p.hair, Offset(0f, size.height), Offset(size.width, size.height), 1.dp.toPx())
                        }
                        .padding(start = 19.dp, end = 14.dp, top = 6.dp, bottom = 6.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    ColHead(if (wide) "이름 · 직함 · 소속" else "이름 · 직함 · 소속 · 번호", Modifier.weight(1f))
                    if (wide) {
                        ColHead("VoLTE", Modifier.width(ColVolte))
                        ColHead("VoIP", Modifier.width(ColVoip))
                        ColHead("PTT", Modifier.width(ColPtt))
                        ColHead("자격", Modifier.width(ColFlags))
                    }
                }
                if (ui.members.isEmpty() && !ui.loading) EmptyNote("조건에 맞는 구성원이 없습니다")
                LazyColumn(Modifier.weight(1f)) {
                    items(ui.members, key = { it.userId }) { m ->
                        MemberRow(m, view.orgPath(m.org), wide, selected = ui.editingUserId == m.userId) { onOpen(m) }
                    }
                }
            }
        }
    }
}

@Composable
private fun ColHead(text: String, modifier: Modifier = Modifier) {
    Text(text, modifier, fontSize = Type.micro, color = Tokens.palette.muted, maxLines = 1)
}

/** 번호 칸 — 회선이 없으면 흐린 «–». */
@Composable
private fun NumCell(number: String?, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    val has = !number.isNullOrBlank()
    Text(if (has) com.cims.ue.dispatch.session.localNumber(number) else "–", modifier, fontSize = Type.meta, fontFamily = FontFamily.Monospace,
        color = if (has) p.ink else p.wire, maxLines = 1, overflow = TextOverflow.Ellipsis)
}

/** 자격 라벨 — «그룹 생성»(남색 — allow-create-group) · «원격 청취»(청록 — allow-ambient-listening, 역할 배정의 결과). */
@Composable
private fun FlagLabels(m: MemberInfo, modifier: Modifier = Modifier) {
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
        if (m.allowCreateGroup) Label("그룹 생성", LabelStyle.INK)
        if (m.allowAmbientListening) Label("원격 청취", LabelStyle.TEAL)
    }
}

/**
 * 구성원 한 줄 — 아바타 · 이름 · 직함 · 소속 경로 | VoLTE | VoIP | PTT | 자격. 좁으면 번호·자격 열을 접어 이름 줄에 라벨을,
 * 소속 아래에 "VoLTE … · VoIP … · PTT …" 한 줄을 둔다. 편집 중인 줄 = 연한 남색 면 + 왼쪽 남색 띠.
 */
@Composable
private fun MemberRow(m: MemberInfo, orgPath: String, wide: Boolean, selected: Boolean, onClick: () -> Unit) {
    val p = Tokens.palette
    val name = m.name.ifBlank { "#${m.userId}" }
    Row(Modifier.fillMaxWidth().background(if (selected) p.primarySoft else Color.Transparent)
            .drawBehind {
                drawLine(p.hair, Offset(0f, size.height), Offset(size.width, size.height), 1.dp.toPx())
                if (selected) drawRect(p.primaryLine, size = Size(3.dp.toPx(), size.height))
            }
            .clickable(onClick = onClick)
            .padding(start = 13.dp, end = 14.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Initial(name, size = 34.dp)
        Column(Modifier.weight(1f).padding(start = 11.dp, end = 10.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(name, fontSize = Type.strong, fontWeight = FontWeight.SemiBold, maxLines = 1,
                    overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                if (m.title.isNotBlank()) Text(m.title, fontSize = Type.meta, color = p.ink2, maxLines = 1,
                    modifier = Modifier.padding(start = 7.dp))
                if (!wide) FlagLabels(m, Modifier.padding(start = 8.dp))
            }
            if (orgPath.isNotBlank()) Text(orgPath, fontSize = Type.micro, color = p.muted, maxLines = 1,
                overflow = TextOverflow.Ellipsis)
            if (!wide) {
                val numbers = listOf("VoLTE" to m.volte, "VoIP" to m.voip, "PTT" to m.ptt)
                    .mapNotNull { (k, n) -> n?.msisdn?.takeIf { it.isNotBlank() }?.let { "$k $it" } }
                    .joinToString(" · ")
                if (numbers.isNotEmpty()) Text(numbers, fontSize = Type.micro, fontFamily = FontFamily.Monospace, maxLines = 1,
                    overflow = TextOverflow.Ellipsis)
            }
        }
        if (wide) {
            NumCell(m.volte?.msisdn, Modifier.width(ColVolte))
            NumCell(m.voip?.msisdn, Modifier.width(ColVoip))
            NumCell(m.ptt?.msisdn, Modifier.width(ColPtt))
            FlagLabels(m, Modifier.width(ColFlags))
        }
    }
}

// ── 편집 폼 — 구성원 ─────────────────────────────────────────────────────────

/**
 * 구성원 폼 — **순수 컴포저블**. 머리(아바타 · 이름 · 설명 · [삭제]) + 절 카드(«기본» + 회선 카드 셋) + 바닥 고정 [취소][저장].
 *
 * @param error 화면의 오류(조회·삭제 실패) — 폼이 열려 있는 동안은 폼 바닥에 뜬다. 폼 자신의 오류(`f.error`)가 먼저다.
 */
@Composable
internal fun MemberFormPane(f: MemberForm, view: AdminView, error: String = "", act: MemberFormActions = MemberFormActions()) {
    val p = Tokens.palette
    var confirmDelete by remember(f.orig?.userId) { mutableStateOf(false) }
    var confirmLines by remember(f.orig?.userId) { mutableStateOf<List<Pair<String, String>>>(emptyList()) }

    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(start = 18.dp, end = 14.dp, top = 12.dp, bottom = 12.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Initial(f.formName, size = 42.dp)
            Column(Modifier.weight(1f).padding(horizontal = 12.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(f.formName, fontSize = Type.head, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(f.formSub, fontSize = Type.meta, color = p.muted)
            }
            // 구성원과 VoLTE/VoIP/PTT 회선을 함께 지운다.
            if (!f.isNew) PillButton("삭제", { confirmDelete = true }, kind = Pill.RED, height = 32.dp)
        }
        HDivider()
        if (f.busy) LinearProgressIndicator(Modifier.fillMaxWidth().height(2.dp))
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())
                .padding(start = 14.dp, end = 14.dp, top = 12.dp, bottom = 8.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)) {
            FormCard("기본") {
                FieldRow {
                    Labeled("이름", Modifier.weight(1f)) { FormField(f.name, { v -> act.change { it.copy(name = v) } }) }
                    Labeled("직함", Modifier.weight(1f)) { FormField(f.title, { v -> act.change { it.copy(title = v) } }) }
                }
                Labeled("소속 조직") {
                    OrgPicker(f.org, orgChoices(view.orgs), noneLabel = "(없음)") { v -> act.change { it.copy(org = v) } }
                }
                FieldRow {
                    // 단말·관제 앱 로그인에 쓰는 아이디
                    Labeled("로그인 아이디", Modifier.weight(1f)) {
                        FormField(f.loginId, { v -> act.change { it.copy(loginId = v) } }, keyboard = KeyboardType.Ascii)
                    }
                    Labeled("로그인 비밀번호(바꿀 때만)", Modifier.weight(1f)) {
                        FormField(f.password, { v -> act.change { it.copy(password = v) } }, password = true)
                    }
                }
            }
            LineKind.all.forEach { kind -> LineCard(f, view, kind, act) }
            FormHint("새 회선과 번호 · 접속서비스 변경에는 SIP 비밀번호가 필요합니다(서버가 H(A1) 로만 보관). " +
                "다른 화면에 다녀와도 이 폼은 유지됩니다.")
        }
        FormFooter(error = f.error.ifBlank { error }, note = if (f.busy) "저장 중…" else f.missing) {
            PillButton("취소", act.cancel, kind = Pill.LINE, height = 44.dp)
            PillButton("저장", {
                // 회선을 지우는 저장은 먼저 확인을 받는다 — 그 단말의 등록이 끊긴다.
                val deletes = AdminViewModel.linesToDelete(f)
                if (deletes.isEmpty()) act.save() else confirmLines = deletes
            }, kind = Pill.INK, height = 44.dp, enabled = f.canSave)
        }
    }

    if (confirmDelete) f.orig?.let { m ->
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { ForwardPttKeys(); Text("구성원 삭제") },
            text = { Text("«${m.name}» 을(를) 삭제할까요?\nVoLTE/VoIP/PTT 회선도 함께 삭제되고 단말 등록이 끊깁니다.") },
            confirmButton = {
                TextButton(onClick = { confirmDelete = false; act.delete(m) }) { Text("삭제", color = p.emg) }
            },
            dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("취소") } })
    }

    if (confirmLines.isNotEmpty()) AlertDialog(
        onDismissRequest = { confirmLines = emptyList() },
        title = { ForwardPttKeys(); Text("회선 삭제") },
        text = {
            Text(confirmLines.joinToString("\n") { (kind, number) -> "${AdminViewModel.lineName(kind)} 번호 $number" } +
                "\n\n위 회선을 삭제하고 저장할까요? 단말 등록이 끊깁니다.")
        },
        confirmButton = {
            TextButton(onClick = { confirmLines = emptyList(); act.save() }) { Text("삭제하고 저장", color = p.emg) }
        },
        dismissButton = { TextButton(onClick = { confirmLines = emptyList() }) { Text("취소") } })
}

/**
 * 회선 카드 하나 — 카드 머리 오른쪽에 저장된 회선 상태(«개설됨» 녹색 / «미개설»). 후보는 화면이 관측 중인 [view] 에서 바로
 * 계산한다(비관측 읽기 금지). PTT 카드에는 PTT 자격 스위치가 붙는다.
 */
@Composable
private fun LineCard(f: MemberForm, view: AdminView, kind: String, act: MemberFormActions) {
    val line = f.lines[kind] ?: LineForm()
    val orig = f.orig?.line(kind)
    val opened = orig != null && !orig.isEmpty
    val choices = AdminViewModel.serviceChoicesOf(view, kind, line.serviceRef)
    val needPw = AdminViewModel.needsPassword(orig, line)

    FormCard(LineKind.label(kind), note = LineKind.note(kind),
        trailing = { Label(if (opened) "개설됨" else "미개설", if (opened) LabelStyle.TALK else LabelStyle.FILL) }) {
        Labeled("번호 (E.164 — 비우면 회선 삭제)") {
            FormField(line.msisdn, { v -> act.changeLine(kind) { it.copy(msisdn = v) } }, mono = true,
                keyboard = KeyboardType.Phone)
        }
        if (opened && line.msisdn.isBlank())
            SoftBand("번호를 비웠습니다 — 저장하면 이 회선(${orig!!.msisdn})을 삭제합니다. 단말 등록이 끊깁니다.")
        FieldRow {
            Labeled("접속서비스", Modifier.weight(1f)) {
                // 저장된 값이 후보에 없어도 그대로 보인다 — 첫 후보로 바꿔 넣으면 저장마다 «서비스 변경» 이 된다.
                FormDropdown(
                    shown = choices.firstOrNull { it.name == line.serviceRef }?.label ?: line.serviceRef.ifBlank { "선택" },
                    options = choices, text = { it.label },
                    onPick = { sv -> act.changeLine(kind) { it.copy(serviceRef = sv.name) } },
                    enabled = choices.isNotEmpty(), placeholder = line.serviceRef.isBlank())
            }
            // ANY = 가입자 지정 없음(접속서비스 기본을 따른다) — 다른 값에서 되돌릴 수 있는 명시값이다.
            Labeled("SIP transport", Modifier.weight(1f)) {
                Segmented(SIP_TRANSPORTS, SIP_TRANSPORTS.indexOfFirst { it.equals(line.sipTransport, ignoreCase = true) },
                    onSelect = { i -> act.changeLine(kind) { it.copy(sipTransport = SIP_TRANSPORTS[i]) } },
                    modifier = Modifier.fillMaxWidth(), height = 40.dp, strong = false)
            }
        }
        // 후보가 없으면 개설을 막는다 — 서버가 400 을 낼 것이 확실하다(§4.5).
        if (choices.isEmpty()) SoftBand("접속서비스 후보가 없습니다 — 서버(CSC)가 kind=$kind 후보를 내려주지 않아 회선 개설이 실패합니다. " +
            "운영자에게 접속서비스 등록을 요청하세요.")
        orig?.pickupGroup?.takeIf { it.isNotBlank() }?.let { pickup ->
            Labeled("픽업 그룹 (콘솔 전화 그룹에서 편성 — 읽기 전용)") { FormField(pickup, {}, enabled = false, mono = true) }
        }
        Labeled(if (needPw) "SIP 비밀번호 (필수 — 개설 · 번호 · 접속서비스 변경)" else "SIP 비밀번호 (바꿀 때만)") {
            FormField(line.password, { v -> act.changeLine(kind) { it.copy(password = v) } }, password = true,
                enabled = line.msisdn.isNotBlank(), error = needPw && line.password.isBlank())
        }
        if (kind == LineKind.PTT) Column {
            // allow-create-group — 이 구성원이 PTT 그룹을 만들 수 있다
            FormSwitch("PTT 그룹 생성 자격", f.allowCreateGroup, { v -> act.change { it.copy(allowCreateGroup = v) } })
            // allow-ambient-listening — 역할 배정의 결과라 여기서 바꾸지 않는다(서버가 `not_editable` 로 거절한다)
            FormSwitch("원격 청취 자격", f.orig?.allowAmbientListening == true, {}, enabled = false,
                note = "원격 청취는 운영 콘솔에서 역할로 부여합니다 — 여기서는 표시만")
        }
    }
}

// ── 편집 폼 — 조직 ───────────────────────────────────────────────────────────

/** 조직 폼 — **순수 컴포저블**. 머리(폴더 아바타 · 이름 · 설명) + 절 카드 «조직» + 바닥 고정 [취소][저장]. */
@Composable
internal fun OrgFormPane(node: OrgNode, isNew: Boolean, view: AdminView, error: String = "",
                         act: OrgFormActions = OrgFormActions()) {
    val p = Tokens.palette
    val title = node.name.trim().ifEmpty { if (isNew) "새 조직" else node.code }
    val hue = p.avatars[avatarHueOf(title)]
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(start = 18.dp, end = 14.dp, top = 12.dp, bottom = 12.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(42.dp).clip(RoundedCornerShape(10.dp)).background(hue.bg), contentAlignment = Alignment.Center) {
                Icon(Icons.Filled.Folder, contentDescription = null, tint = hue.fg, modifier = Modifier.size(20.dp))
            }
            Column(Modifier.weight(1f).padding(horizontal = 12.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(title, fontSize = Type.head, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(if (isNew) "새 조직 등록" else "조직 편집 · ${node.code}", fontSize = Type.meta, color = p.muted,
                    maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        HDivider()
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())
                .padding(start = 14.dp, end = 14.dp, top = 12.dp, bottom = 8.dp)) {
            FormCard("조직") {
                Labeled("코드 — 불변 키(만든 뒤 바꿀 수 없음)") {
                    FormField(node.code, { v -> act.change { it.copy(code = v) } }, enabled = isNew, mono = true,
                        keyboard = KeyboardType.Ascii)
                }
                Labeled("이름") { FormField(node.name, { v -> act.change { it.copy(name = v) } }) }
                // 편집 중인 조직 자신·자손은 고를 수 없다(고리 방지). 새 조직은 아직 자손이 없다.
                Labeled("상위 조직") {
                    OrgPicker(node.parent, orgChoices(view.orgs, excludeSubtreeOf = if (isNew) null else node.code),
                        noneLabel = "(최상위)") { v -> act.change { it.copy(parent = v) } }
                }
                Labeled("정렬 순서") {
                    NumberField(node.sort, { v -> act.change { it.copy(sort = v) } }, Modifier.width(96.dp))
                }
            }
        }
        FormFooter(error = error) {
            PillButton("취소", act.cancel, kind = Pill.LINE, height = 44.dp)
            PillButton("저장", act.save, kind = Pill.INK, height = 44.dp)
        }
    }
}
