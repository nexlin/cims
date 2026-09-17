// [관리] 화면 (docs/design/features/android_dispatch_tablet.md §6.7, dispatch_desktop_ui.md §4.5)
//
// 세 칸 — 조직 트리(좁게) | 구성원 표 | 편집 폼. 태블릿에는 서브내비를 두지 않고(항목이 하나뿐)
// 화면 머리에 범위 안내를 붙인다. 행 한 번 클릭이 곧 편집이다.
package com.cims.ue.dispatch.ui.admin

import com.cims.ue.dispatch.ui.Tag
import com.cims.ue.dispatch.ui.Type
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.AdminView
import com.cims.ue.dispatch.session.LineKind
import com.cims.ue.dispatch.session.MemberInfo
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.session.SIP_TRANSPORTS

@Composable
fun AdminScreen(vm: AdminViewModel, modifier: Modifier = Modifier) {
    if (!vm.available) return Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text("관리 범위 미배정 — 콘솔 «관리 > 역할» 에서 관리 범위(directory_write)를 받아야 합니다",
            color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = Type.strong)
    }

    val view by vm.view.collectAsStateWithLifecycle()
    val members by vm.members.collectAsStateWithLifecycle()
    val form by vm.form.collectAsStateWithLifecycle()
    val loading by vm.loading.collectAsStateWithLifecycle()
    val error by vm.error.collectAsStateWithLifecycle()
    var discard by remember { mutableStateOf<MemberInfo?>(null) }

    LaunchedEffect(Unit) { if (view.members.isEmpty()) vm.load() }

    Column(modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Text("조직 · 구성원 · 번호", fontSize = Type.title, fontWeight = FontWeight.Bold)
            Spacer(Modifier.width(8.dp))
            Text("관리 범위 ${view.scope.directoryWrite.ifBlank { "—" }}", fontSize = Type.meta,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (vm.dirty) {
                Spacer(Modifier.width(8.dp))
                Surface(color = MaterialTheme.colorScheme.tertiary.copy(alpha = 0.2f),
                    shape = RoundedCornerShape(4.dp)) {
                    Text("저장하지 않은 변경", Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                        fontSize = Type.meta, color = MaterialTheme.colorScheme.tertiary)
                }
            }
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { vm.load(force = true) }) { Text("새로고침") }
        }
        if (error.isNotBlank()) Text(error, Modifier.fillMaxWidth().padding(horizontal = 12.dp),
            color = MaterialTheme.colorScheme.error, fontSize = Type.body)
        if (loading) LinearProgressIndicator(Modifier.fillMaxWidth())

        Row(Modifier.weight(1f)) {
            OrgTree(vm, view, Modifier.width(210.dp))
            VerticalDivider()
            MemberTable(vm, view, members, Modifier.weight(1f)) { m ->
                if (vm.dirty) discard = m else vm.open(m)
            }
            VerticalDivider()
            Box(Modifier.weight(1.3f).fillMaxHeight()) {
                form?.let { MemberForm(vm, view, it) }
                    ?: Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        Text("구성원을 고르세요", color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
            }
        }
    }

    // 변경이 있는 채로 다른 구성원을 누르면 확인 — 아니오면 선택이 되돌아간다(§4.5).
    discard?.let { target ->
        AlertDialog(
            onDismissRequest = { discard = null },
            title = { Text("변경 버림") },
            text = { Text("저장하지 않은 변경이 있습니다. 버리고 «${target.name}» 을(를) 열까요?") },
            confirmButton = { TextButton(onClick = { discard = null; vm.open(target) }) { Text("버리고 열기") } },
            dismissButton = { TextButton(onClick = { discard = null }) { Text("취소") } })
    }

    OrgDialog(vm, view)
}

// ── 조직 트리 ───────────────────────────────────────────────────────────────

@Composable
private fun OrgTree(vm: AdminViewModel, view: AdminView, modifier: Modifier) {
    val sel by vm.org.collectAsStateWithLifecycle()
    Column(modifier.fillMaxHeight()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Text("조직", fontSize = Type.strong, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { vm.newOrg() },
                contentPadding = PaddingValues(horizontal = 6.dp)) { Text("+ 새 조직", fontSize = Type.meta) }
        }
        HorizontalDivider()
        LazyColumn(Modifier.weight(1f)) {
            items(flatten(view.orgs), key = { it.first.code }) { (o, depth) ->
                val on = sel == o.code
                Row(Modifier.fillMaxWidth()
                        .background(if (on) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent)
                        .clickable { vm.selectOrg(o.code) }
                        .padding(start = (8 + depth * 12).dp, end = 6.dp, top = 6.dp, bottom = 6.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Text(o.name.ifBlank { o.code }, Modifier.weight(1f), fontSize = Type.body, maxLines = 1)
                    Text("${view.members.count { it.org == o.code }}", fontSize = Type.meta,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        HorizontalDivider()
        Row(Modifier.padding(4.dp)) {
            val cur = view.orgs.firstOrNull { it.code == sel }
            TextButton(onClick = { cur?.let { vm.editOrg(it) } }, enabled = cur != null,
                contentPadding = PaddingValues(horizontal = 8.dp)) { Text("편집", fontSize = Type.meta) }
            TextButton(onClick = { cur?.let { vm.deleteOrg(it.code) } }, enabled = cur != null,
                contentPadding = PaddingValues(horizontal = 8.dp)) {
                Text("삭제", fontSize = Type.meta, color = MaterialTheme.colorScheme.error)
            }
        }
    }
}

/** 조직 트리를 깊이 순으로 평탄화 — 고리가 있으면 남은 것을 뿌리로 올려 잃지 않는다. */
internal fun flatten(orgs: List<OrgNode>): List<Pair<OrgNode, Int>> {
    val byParent = orgs.groupBy { it.parent }
    val out = ArrayList<Pair<OrgNode, Int>>()
    val seen = HashSet<String>()
    fun walk(parent: String, depth: Int) {
        byParent[parent].orEmpty().sortedWith(compareBy({ it.sort }, { it.name })).forEach {
            if (seen.add(it.code)) { out.add(it to depth); walk(it.code, depth + 1) }
        }
    }
    walk("", 0)
    orgs.forEach { if (seen.add(it.code)) out.add(it to 0) }
    return out
}

/**
 * 조직 고르기 목록 — 평탄화에서 **고를 수 없는 것을 뺀 것**(순수 함수, 시험 대상).
 *
 * @param excludeSubtreeOf 이 코드와 그 하위 전체를 뺀다. 조직을 편집할 때 자기 자신·자손을 상위로 고르면
 *   고리가 생긴다 — 서버가 막더라도 **고를 수 있게 두면 안 된다**(고르고 저장 눌러서 실패하는 UI 는
 *   그 자체가 결함이다). 구성원 «소속» 처럼 뺄 것이 없으면 null.
 */
internal fun orgChoices(orgs: List<OrgNode>, excludeSubtreeOf: String? = null): List<Pair<OrgNode, Int>> {
    val all = flatten(orgs)
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
 * 종전에는 **코드를 손으로 쳤다.** 조직 코드는 사람이 외우는 값이 아니라 오타가 나면 조용히 다른 조직에
 * 붙거나 저장이 400 으로 떨어진다. 트리는 이미 화면에 있으므로 그 목록을 그대로 고르게 한다.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun OrgPicker(
    label: String,
    value: String,
    choices: List<Pair<OrgNode, Int>>,
    allowNone: Boolean,
    noneLabel: String,
    modifier: Modifier = Modifier,
    onPick: (String) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    val picked = choices.firstOrNull { it.first.code == value }?.first
    // 고른 값이 목록에 없으면 코드를 그대로 보인다 — 범위 밖 조직에 붙어 있는 구성원을 «없음» 으로 보이면
    //   저장할 때 소속이 조용히 바뀐다.
    val shown = when {
        value.isBlank() -> noneLabel
        picked != null -> picked.name.ifBlank { picked.code } + " (" + picked.code + ")"
        else -> value + " (범위 밖)"
    }
    ExposedDropdownMenuBox(expanded = open, onExpandedChange = { open = it }, modifier = modifier) {
        OutlinedTextField(
            value = shown, onValueChange = {}, readOnly = true, singleLine = true,
            label = { Text(label) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = open) },
            modifier = Modifier.menuAnchor(MenuAnchorType.PrimaryNotEditable))
        ExposedDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            if (allowNone) DropdownMenuItem(
                text = { Text(noneLabel) },
                onClick = { onPick(""); open = false })
            choices.forEach { (o, depth) ->
                DropdownMenuItem(
                    text = {
                        Text("   ".repeat(depth) + o.name.ifBlank { o.code } + "  (" + o.code + ")",
                             fontSize = Type.strong)
                    },
                    onClick = { onPick(o.code); open = false })
            }
        }
    }
}

@Composable
private fun OrgDialog(vm: AdminViewModel, view: AdminView) {
    val o by vm.orgForm.collectAsStateWithLifecycle()
    val isNew by vm.orgFormIsNew.collectAsStateWithLifecycle()
    val node = o ?: return
    AlertDialog(
        onDismissRequest = { vm.closeOrgForm() },
        title = { Text(if (isNew) "새 조직" else "조직 편집") },
        text = {
            Column {
                OutlinedTextField(value = node.code, enabled = isNew,
                    onValueChange = { v -> vm.updateOrgForm { it.copy(code = v) } },
                    label = { Text("코드") }, singleLine = true)
                Spacer(Modifier.height(6.dp))
                OutlinedTextField(value = node.name,
                    onValueChange = { v -> vm.updateOrgForm { it.copy(name = v) } },
                    label = { Text("이름") }, singleLine = true)
                Spacer(Modifier.height(6.dp))
                // 편집 중인 조직 자신·자손은 고를 수 없다(고리 방지). 새 조직은 아직 자손이 없다.
                OrgPicker(
                    label = "상위 조직",
                    value = node.parent,
                    choices = orgChoices(view.orgs, excludeSubtreeOf = if (isNew) null else node.code),
                    allowNone = true, noneLabel = "(최상위)",
                    onPick = { v -> vm.updateOrgForm { it.copy(parent = v) } })
                Spacer(Modifier.height(6.dp))
                OutlinedTextField(value = node.sort.toString(),
                    onValueChange = { v -> vm.updateOrgForm { it.copy(sort = v.toIntOrNull() ?: it.sort) } },
                    label = { Text("정렬") }, singleLine = true)
            }
        },
        confirmButton = { TextButton(onClick = { vm.saveOrg() }) { Text("저장") } },
        dismissButton = { TextButton(onClick = { vm.closeOrgForm() }) { Text("취소") } })
}

// ── 구성원 표 ───────────────────────────────────────────────────────────────

@Composable
private fun MemberTable(vm: AdminViewModel, view: AdminView, members: List<MemberInfo>,
                        modifier: Modifier, onOpen: (MemberInfo) -> Unit) {
    val query by vm.query.collectAsStateWithLifecycle()
    val org by vm.org.collectAsStateWithLifecycle()
    val form by vm.form.collectAsStateWithLifecycle()

    Column(modifier.fillMaxHeight()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Text("구성원 ${members.size}명" +
                 (if (org.isNotBlank()) " · ${view.orgPath(org)} 하위 포함" else ""),
                fontSize = Type.body, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { vm.newMember() },
                contentPadding = PaddingValues(horizontal = 8.dp)) { Text("+ 새 구성원", fontSize = Type.meta) }
        }
        OutlinedTextField(value = query, onValueChange = vm::search,
            placeholder = { Text("이름·아이디·번호", fontSize = Type.body) }, singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 10.dp),
            textStyle = MaterialTheme.typography.bodySmall)
        HorizontalDivider(Modifier.padding(top = 6.dp))
        LazyColumn(Modifier.weight(1f)) {
            items(members, key = { it.userId }) { m ->
                val on = form?.orig?.userId == m.userId
                Column(Modifier.fillMaxWidth()
                        .background(if (on) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent)
                        .clickable { onOpen(m) }
                        .padding(horizontal = 10.dp, vertical = 6.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(m.name, fontSize = Type.strong, fontWeight = FontWeight.Bold)
                        if (m.title.isNotBlank()) {
                            Spacer(Modifier.width(6.dp))
                            Text(m.title, fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        Spacer(Modifier.weight(1f))
                        if (m.allowCreateGroup) Tag("그룹 생성")
                        if (m.allowAmbientListening) Tag("원격 청취")
                    }
                    Text(listOfNotNull(
                            view.orgPath(m.org).takeIf { it.isNotBlank() },
                            m.volte?.msisdn?.takeIf { it.isNotBlank() }?.let { "이동 $it" },
                            m.voip?.msisdn?.takeIf { it.isNotBlank() }?.let { "유선 $it" },
                            m.ptt?.msisdn?.takeIf { it.isNotBlank() }?.let { "PTT $it" },
                        ).joinToString(" · "),
                        fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
                }
                HorizontalDivider()
            }
        }
    }
}


// ── 편집 폼 ─────────────────────────────────────────────────────────────────

@Composable
private fun MemberForm(vm: AdminViewModel, view: AdminView, f: MemberForm) {
    var confirmDelete by remember(f.orig?.userId) { mutableStateOf(false) }

    Column(Modifier.fillMaxSize().padding(12.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(f.heading, fontSize = Type.title, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            if (!f.isNew) TextButton(onClick = { confirmDelete = true }) {
                Text("삭제", color = MaterialTheme.colorScheme.error)
            }
        }
        if (f.error.isNotBlank()) Text(f.error, Modifier.padding(top = 4.dp),
            color = MaterialTheme.colorScheme.error, fontSize = Type.body)
        if (f.busy) LinearProgressIndicator(Modifier.fillMaxWidth().padding(top = 4.dp))

        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
            OutlinedTextField(value = f.name, onValueChange = { v -> vm.update { it.copy(name = v) } },
                label = { Text("이름") }, singleLine = true, modifier = Modifier.fillMaxWidth())
            Spacer(Modifier.height(6.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                OutlinedTextField(value = f.title, onValueChange = { v -> vm.update { it.copy(title = v) } },
                    label = { Text("직함") }, singleLine = true, modifier = Modifier.weight(1f))
                OrgPicker(
                    label = "소속",
                    value = f.org,
                    choices = orgChoices(view.orgs),
                    allowNone = true, noneLabel = "(없음)",
                    modifier = Modifier.weight(1f),
                    onPick = { v -> vm.update { it.copy(org = v) } })
            }
            Spacer(Modifier.height(6.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                OutlinedTextField(value = f.loginId, onValueChange = { v -> vm.update { it.copy(loginId = v) } },
                    label = { Text("로그인 아이디") }, singleLine = true, modifier = Modifier.weight(1f))
                OutlinedTextField(value = f.password, onValueChange = { v -> vm.update { it.copy(password = v) } },
                    label = { Text("비밀번호(바꿀 때만)") }, singleLine = true,
                    visualTransformation = PasswordVisualTransformation(), modifier = Modifier.weight(1f))
            }

            Spacer(Modifier.height(10.dp))
            LineKind.all.forEach { kind -> LineCard(vm, view, f, kind) }

            Spacer(Modifier.height(8.dp))
            Text("PTT 자격", fontSize = Type.strong, fontWeight = FontWeight.Bold)
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text("그룹 생성", Modifier.weight(1f), fontSize = Type.body)
                Switch(checked = f.allowCreateGroup,
                    onCheckedChange = { v -> vm.update { it.copy(allowCreateGroup = v) } })
            }
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text("원격 청취 (표시만 — 콘솔 역할에서 부여)", Modifier.weight(1f), fontSize = Type.body,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Switch(checked = f.orig?.allowAmbientListening == true, onCheckedChange = null, enabled = false)
            }
        }

        HorizontalDivider()
        Row(Modifier.padding(top = 8.dp)) {
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { vm.closeForm() }) { Text("닫기") }
            Button(onClick = { vm.save() }, enabled = f.canSave) { Text("저장") }
        }
    }

    if (confirmDelete) f.orig?.let { m ->
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { Text("구성원 삭제") },
            text = { Text("«${m.name}» 과(와) 그 회선을 지웁니다. 되돌릴 수 없습니다.") },
            confirmButton = {
                TextButton(onClick = { confirmDelete = false; vm.deleteMember(m) }) {
                    Text("삭제", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("취소") } })
    }
}

/** 회선 카드 하나. 후보는 화면이 관측 중인 [view] 에서 바로 계산한다(비관측 읽기 금지). */
@Composable
private fun LineCard(vm: AdminViewModel, view: AdminView, f: MemberForm, kind: String) {
    val line = f.lines[kind] ?: LineForm()
    val orig = f.orig?.line(kind)
    val choices = AdminViewModel.serviceChoicesOf(view, kind, line.serviceRef)
    val needPw = AdminViewModel.needsPassword(orig, line)

    OutlinedCard(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        Column(Modifier.padding(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(LineKind.label(kind), fontSize = Type.strong, fontWeight = FontWeight.Bold)
                Spacer(Modifier.weight(1f))
                if (orig != null && !orig.isEmpty && line.msisdn.isBlank())
                    Text("비우면 회선 삭제", fontSize = Type.meta, color = MaterialTheme.colorScheme.error)
            }
            OutlinedTextField(value = line.msisdn,
                onValueChange = { v -> vm.updateLine(kind) { it.copy(msisdn = v) } },
                label = { Text("번호") }, singleLine = true, modifier = Modifier.fillMaxWidth())

            Spacer(Modifier.height(4.dp))
            Text("접속서비스", fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (choices.isEmpty()) {
                // 후보가 없으면 개설을 막는다 — 서버가 400 을 낼 것이 확실하다(§4.5).
                Text("후보가 없습니다 — 서버 접속서비스(kind=$kind) 등록이 필요합니다(운영자)",
                    fontSize = Type.meta, color = MaterialTheme.colorScheme.error)
            } else {
                Row(Modifier.horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    choices.forEach { sv ->
                        FilterChip(selected = line.serviceRef == sv.name,
                            onClick = { vm.updateLine(kind) { it.copy(serviceRef = sv.name) } },
                            label = { Text(sv.name, fontSize = Type.meta) })
                    }
                }
            }

            Spacer(Modifier.height(4.dp))
            Text("SIP transport", fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                SIP_TRANSPORTS.forEach { t ->
                    FilterChip(selected = line.sipTransport.equals(t, ignoreCase = true),
                        onClick = { vm.updateLine(kind) { it.copy(sipTransport = t) } },
                        label = { Text(t, fontSize = Type.meta) })
                }
            }

            if (line.msisdn.isNotBlank()) {
                Spacer(Modifier.height(4.dp))
                OutlinedTextField(value = line.password,
                    onValueChange = { v -> vm.updateLine(kind) { it.copy(password = v) } },
                    label = { Text(if (needPw) "SIP 비밀번호 (필수)" else "SIP 비밀번호 (바꿀 때만)") },
                    singleLine = true, isError = needPw && line.password.isBlank(),
                    visualTransformation = PasswordVisualTransformation(),
                    modifier = Modifier.fillMaxWidth())
            }
            orig?.pickupGroup?.takeIf { it.isNotBlank() }?.let {
                Text("픽업 그룹 $it (읽기 전용 — 콘솔 전화 그룹에서 파생)", fontSize = Type.meta,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}
