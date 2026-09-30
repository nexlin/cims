// [PTT 그룹] 화면 (docs/design/features/android_dispatch_tablet.md §6.6, dispatch_desktop_ui.md §4.7)
//
// 카드 둘 — 왼쪽 좁은 목록(1) : 오른쪽 상세/편집(3). 편집은 **같은 자리의 인라인 폼**이다(별창 없음).
// 편집 중에는 목록이 잠긴다 — 저장·취소로만 나온다.
package com.cims.ue.dispatch.ui.groups

import com.cims.ue.dispatch.ui.CimsFilterChip

import com.cims.ue.dispatch.ui.Tag
import com.cims.ue.dispatch.ui.Type
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.ManagedGroup

@Composable
fun PttGroupsScreen(vm: PttGroupsViewModel, onOpenChannel: (String) -> Unit, modifier: Modifier = Modifier) {
    val rows by vm.rows.collectAsStateWithLifecycle()
    val sel by vm.selected.collectAsStateWithLifecycle()
    val form by vm.form.collectAsStateWithLifecycle()
    val loading by vm.loading.collectAsStateWithLifecycle()
    val error by vm.error.collectAsStateWithLifecycle()
    val filter by vm.filter.collectAsStateWithLifecycle()
    val query by vm.query.collectAsStateWithLifecycle()
    val book by vm.book.collectAsStateWithLifecycle()
    val detail by vm.detail.collectAsStateWithLifecycle()
    val detailBusy by vm.detailBusy.collectAsStateWithLifecycle()

    LaunchedEffect(Unit) { if (rows.isEmpty()) vm.load() }

    PttGroupsScreenContent(
        ui = GroupsUi(rows = rows, selected = sel, detail = detail, detailBusy = detailBusy,
            book = book, filter = filter, query = query, loading = loading, error = error,
            locked = vm.locked, editing = form != null),
        act = GroupsActions(
            reload = vm::load, newGroup = vm::newGroup, setFilter = vm::setFilter, search = vm::search,
            select = vm::select, edit = vm::edit, delete = vm::delete,
            openChannel = { g -> vm.openChannel(g, onOpenChannel) }),
        // 편집 폼은 입력 상태가 VM 에 있어 통째로 넘긴다 — 판정 대상이 밀도가 아니라 폼 동작이다.
        editPane = { form?.let { EditPane(vm, it) } },
        modifier = modifier)
}

/** [PTT 그룹] 화면이 그리는 데 필요한 값 전부. */
data class GroupsUi(
    val rows: List<ManagedGroup> = emptyList(),
    val selected: ManagedGroup? = null,
    val detail: List<DetailMember> = emptyList(),
    val detailBusy: Boolean = false,
    val book: DirectoryBook = DirectoryBook(),
    val filter: GroupFilter = GroupFilter.ALL,
    val query: String = "",
    val loading: Boolean = false,
    val error: String = "",
    /** 편집 중 — 목록·[↻]이 잠긴다(§4.7). */
    val locked: Boolean = false,
    val editing: Boolean = false,
)

data class GroupsActions(
    val reload: () -> Unit = {},
    val newGroup: () -> Unit = {},
    val setFilter: (GroupFilter) -> Unit = {},
    val search: (String) -> Unit = {},
    val select: (ManagedGroup) -> Unit = {},
    val edit: (ManagedGroup) -> Unit = {},
    val delete: (ManagedGroup) -> Unit = {},
    val openChannel: (ManagedGroup) -> Unit = {},
)

/** [PTT 그룹] 본문 — **순수 컴포저블**. 편집 폼만 호출자가 넘긴다. */
@Composable
fun PttGroupsScreenContent(
    ui: GroupsUi,
    act: GroupsActions = GroupsActions(),
    editPane: @Composable () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    Column(modifier.fillMaxSize()) {
        if (ui.error.isNotBlank()) Text(ui.error, Modifier.fillMaxWidth().padding(12.dp, 6.dp),
            color = MaterialTheme.colorScheme.error, fontSize = Type.strong)
        if (ui.loading) LinearProgressIndicator(Modifier.fillMaxWidth())
        Row(Modifier.weight(1f)) {
            GroupList(ui, act, Modifier.weight(1f))
            VerticalDivider()
            Box(Modifier.weight(3f).fillMaxHeight()) {
                when {
                    ui.editing -> editPane()
                    ui.selected != null -> DetailPane(ui, act, ui.selected)
                    else -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        Text("그룹을 고르세요", color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
        }
    }
}

// ── 목록 ────────────────────────────────────────────────────────────────────

@Composable
private fun GroupList(ui: GroupsUi, act: GroupsActions, modifier: Modifier = Modifier) {
    val rows = ui.rows
    val sel = ui.selected
    val filter = ui.filter
    val query = ui.query
    val locked = ui.locked

    Column(modifier.fillMaxHeight()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Text("그룹 ${rows.size}개", fontSize = Type.strong, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { act.reload() }, enabled = !locked,
                contentPadding = PaddingValues(horizontal = 8.dp)) { Text("↻") }
            // 새 그룹은 여기서 만들지 않는다 — [관제] › [무전] «채널» 의 [채널 추가하기] 에서 사람을 골라 [그룹 추가](§6.12).
        }
        Row(Modifier.padding(horizontal = 10.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            GroupFilter.entries.forEach { f ->
                CimsFilterChip(selected = filter == f, onClick = { act.setFilter(f) },
                    enabled = !locked, label = { Text(f.label, fontSize = Type.meta) })
            }
        }
        OutlinedTextField(
            value = query, onValueChange = act.search, enabled = !locked,
            placeholder = { Text("그룹명·id", fontSize = Type.body) }, singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 6.dp),
            textStyle = MaterialTheme.typography.bodySmall)
        HorizontalDivider()
        LazyColumn(Modifier.weight(1f)) {
            items(rows, key = { it.id }) { g -> GroupRow(g, g.id == sel?.id, locked) { act.select(g) } }
        }
    }
}

@Composable
private fun GroupRow(g: ManagedGroup, selected: Boolean, locked: Boolean, onClick: () -> Unit) {
    val bg = if (selected) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent
    Column(Modifier.fillMaxWidth().background(bg).clickable(enabled = !locked, onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Tag(g.relation)
            Spacer(Modifier.width(6.dp))
            Text(g.name.ifBlank { g.id }, fontSize = Type.title, fontWeight = FontWeight.Bold, maxLines = 1)
        }
        Text("멤버 ${g.memberCount} · ${g.id}", fontSize = Type.meta,
            color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
    }
    HorizontalDivider()
}


/** 상세 멤버 한 줄 — 이름·번호와 지금 상태. 발언 중은 색으로, 미참가는 흐리게. */
@Composable
private fun MemberLine(m: DetailMember) {
    val speaking = m.status == DetailMember.SPEAKING
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(m.name + if (m.isMe) " (나)" else "", fontSize = Type.strong,
            fontWeight = if (speaking) FontWeight.Bold else FontWeight.Normal,
            color = if (m.absent) MaterialTheme.colorScheme.onSurfaceVariant
                    else MaterialTheme.colorScheme.onSurface,
            maxLines = 1)
        if (m.isChair) { Spacer(Modifier.width(4.dp)); Tag("의장") }
        Spacer(Modifier.width(8.dp))
        Text(m.number, fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
        Spacer(Modifier.weight(1f))
        Text(m.status, fontSize = Type.meta,
            fontWeight = if (speaking) FontWeight.Bold else FontWeight.Normal,
            color = when {
                speaking -> MaterialTheme.colorScheme.primary
                m.absent -> MaterialTheme.colorScheme.onSurfaceVariant
                else -> MaterialTheme.colorScheme.onSurface
            })
    }
}

// ── 상세 ────────────────────────────────────────────────────────────────────

@Composable
private fun DetailPane(ui: GroupsUi, act: GroupsActions, g: ManagedGroup) {
    val book = ui.book
    val members = ui.detail
    val membersBusy = ui.detailBusy
    var confirmDelete by remember(g.id) { mutableStateOf(false) }

    Column(Modifier.fillMaxSize().padding(14.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(g.name.ifBlank { g.id }, fontSize = Type.head, fontWeight = FontWeight.Bold)
            Spacer(Modifier.width(8.dp))
            Text(g.id, fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.width(8.dp))
            if (g.isMember) Tag("멤버")
            Spacer(Modifier.weight(1f))
            // 관리 판정은 서버가 한다 — 앱은 서버가 내려 준 canManage 로 버튼만 접는다(§4.7).
            if (g.canManage) TextButton(onClick = { act.edit(g) }) { Text("편집") }
        }
        Spacer(Modifier.height(10.dp))
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
            Field("소속", if (g.orgCode.isBlank()) "미지정" else g.orgCode)
            Field("세션 종류", g.sessionType.ifBlank { "prearranged" })
            Field("관계", g.relation + if (g.isOwner) " · 내 소유" else "")
            if (!g.canManage) Text("보기 전용 — 관리 범위 밖이거나 내 소유가 아닙니다",
                fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp))
            if (book.entries.isEmpty()) Text("전화번호부를 아직 받지 못했습니다 — 멤버 이름은 번호로 보입니다",
                fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp))

            // 멤버 전원 — ① 카드 3줄이 «+n» 으로 접은 로스터의 **전체**가 여기다(§6.3a).
            //   카드를 늘리지 않는 이유는 3×2 격자의 카드 높이가 고정이어야 하기 때문이다.
            Spacer(Modifier.height(12.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("멤버 ${if (members.isEmpty()) g.memberCount else members.size}명",
                    fontSize = Type.strong, fontWeight = FontWeight.Bold)
                val here = members.count { !it.absent }
                if (members.isNotEmpty()) {
                    Spacer(Modifier.width(6.dp))
                    Text("· 참여 $here", fontSize = Type.body,
                        color = if (here > 0) MaterialTheme.colorScheme.primary
                                else MaterialTheme.colorScheme.onSurfaceVariant)
                }
                if (membersBusy) {
                    Spacer(Modifier.width(8.dp))
                    CircularProgressIndicator(Modifier.size(12.dp), strokeWidth = 2.dp)
                }
            }
            Spacer(Modifier.height(4.dp))
            if (members.isEmpty() && !membersBusy)
                Text("멤버 목록을 받지 못했습니다", fontSize = Type.meta,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            members.forEach { m -> MemberLine(m) }
        }
        HorizontalDivider()
        Row(Modifier.padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (g.canManage) TextButton(onClick = { confirmDelete = true }) {
                Text("삭제", color = MaterialTheme.colorScheme.error)
            }
            Spacer(Modifier.weight(1f))
            // 채널이 있는 그룹만 — 멤버(① 카드)·청취 범위(② 행). 관리 범위만 있으면 태블릿에 갈 채널이 없다.
            if (g.hasChannel) Button(onClick = { act.openChannel(g) }) {
                Text(if (g.isMember) "채널로 (합류)" else "채널로")
            }
        }
    }

    if (confirmDelete) AlertDialog(
        onDismissRequest = { confirmDelete = false },
        title = { com.cims.ue.dispatch.ui.ForwardPttKeys(); Text("그룹 삭제") },
        text = { Text("«${g.name.ifBlank { g.id }}» 을(를) 지웁니다. 되돌릴 수 없습니다.") },
        confirmButton = {
            TextButton(onClick = { confirmDelete = false; act.delete(g) }) {
                Text("삭제", color = MaterialTheme.colorScheme.error)
            }
        },
        dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("취소") } })
}

@Composable
private fun Field(label: String, value: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
        Text(label, Modifier.width(110.dp), fontSize = Type.body,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, fontSize = Type.strong)
    }
}

// ── 편집 폼 ─────────────────────────────────────────────────────────────────

@Composable
private fun EditPane(vm: PttGroupsViewModel, f: EditForm) {
    Column(Modifier.fillMaxSize().padding(14.dp)) {
        Text(f.title, fontSize = Type.title, fontWeight = FontWeight.Bold)
        if (f.error.isNotBlank()) Text(f.error, Modifier.padding(top = 4.dp),
            color = MaterialTheme.colorScheme.error, fontSize = Type.body)
        if (f.busy) LinearProgressIndicator(Modifier.fillMaxWidth().padding(top = 4.dp))
        Spacer(Modifier.height(8.dp))

        Row(Modifier.weight(1f)) {
            // 왼쪽 — 속성
            Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(end = 10.dp)) {
                OutlinedTextField(value = f.name, onValueChange = { v -> vm.update { it.copy(name = v) } },
                    label = { Text("그룹 이름") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                Spacer(Modifier.height(6.dp))
                OutlinedTextField(value = f.groupId,
                    onValueChange = { v -> vm.update { it.copy(groupId = v) } },
                    label = { Text("그룹 id") }, singleLine = true, enabled = f.isNew,
                    supportingText = { if (!f.isNew) Text("만든 뒤에는 바꿀 수 없습니다", fontSize = Type.meta) },
                    modifier = Modifier.fillMaxWidth())
                Spacer(Modifier.height(6.dp))
                Text("세션 종류", fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    PttGroupsViewModel.SESSION_TYPES.forEach { t ->
                        CimsFilterChip(selected = f.sessionType == t,
                            onClick = { vm.update { it.copy(sessionType = t) } },
                            label = { Text(t, fontSize = Type.meta) })
                    }
                }
                Spacer(Modifier.height(6.dp))
                Toggle("SDS 허용", f.allowSds) { v -> vm.update { it.copy(allowSds = v) } }
                Toggle("파일 전송(FD)", f.allowFd) { v -> vm.update { it.copy(allowFd = v) } }
                Toggle("영상", f.videoEnabled) { v -> vm.update { it.copy(videoEnabled = v) } }
                Toggle("암호화", f.encryption) { v -> vm.update { it.copy(encryption = v) } }
                Toggle("긴급 호", f.emergencyCall) { v -> vm.update { it.copy(emergencyCall = v) } }
                Toggle("긴급 알림", f.emergencyAlert) { v -> vm.update { it.copy(emergencyAlert = v) } }
                Toggle("참가(affiliation) 필요", f.requireAffiliation) { v ->
                    vm.update { it.copy(requireAffiliation = v) }
                }
                Spacer(Modifier.height(6.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(value = f.priority.toString(),
                        onValueChange = { v -> vm.update { it.copy(priority = v.toIntOrNull() ?: it.priority) } },
                        label = { Text("우선순위 0~15") }, singleLine = true, modifier = Modifier.weight(1f))
                    OutlinedTextField(value = f.maxParticipants.toString(),
                        onValueChange = { v -> vm.update { it.copy(maxParticipants = v.toIntOrNull() ?: it.maxParticipants) } },
                        label = { Text("최대 참가(0=무제한)") }, singleLine = true, modifier = Modifier.weight(1f))
                }
            }
            VerticalDivider()
            // 오른쪽 — 멤버
            Column(Modifier.weight(1f).padding(start = 10.dp)) {
                Text("멤버 ${f.members.size}명", fontSize = Type.strong, fontWeight = FontWeight.Bold)
                Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
                    f.members.forEach { m ->
                        Row(Modifier.fillMaxWidth().padding(vertical = 2.dp),
                            verticalAlignment = Alignment.CenterVertically) {
                            Text(m.label + if (m.isMe) " (나)" else "", Modifier.weight(1f), fontSize = Type.body)
                            TextButton(onClick = { vm.toggleChair(m.uri) },
                                contentPadding = PaddingValues(horizontal = 6.dp)) {
                                Text(if (m.isChair) "의장" else "참가자", fontSize = Type.meta)
                            }
                            TextButton(onClick = { vm.removeMember(m.uri) },
                                contentPadding = PaddingValues(horizontal = 6.dp)) {
                                Text("−", fontSize = Type.title)
                            }
                        }
                    }
                }
                HorizontalDivider()
                val cands by vm.candidates.collectAsStateWithLifecycle()
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(value = f.search, onValueChange = { v -> vm.update { it.copy(search = v) } },
                        placeholder = { Text("PTT 주소록 검색", fontSize = Type.body) }, singleLine = true,
                        modifier = Modifier.weight(1f).padding(vertical = 4.dp),
                        textStyle = MaterialTheme.typography.bodySmall)
                    // 검색으로 좁힌 뒤 한 번에 — 보이는 후보만 넣는다(데스크톱 [표시된 전원 추가]).
                    TextButton(onClick = vm::addAllShown, enabled = cands.isNotEmpty()) {
                        Text("표시된 ${cands.size}명 추가", fontSize = Type.meta)
                    }
                }
                LazyColumn(Modifier.weight(1f)) {
                    items(cands, key = { it.msisdn }) { c ->
                        Row(Modifier.fillMaxWidth().clickable { vm.addMember(c.msisdn, c.name) }
                                .padding(vertical = 3.dp),
                            verticalAlignment = Alignment.CenterVertically) {
                            Text(c.name.ifBlank { c.msisdn }, Modifier.weight(1f), fontSize = Type.body)
                            Text(c.msisdn, fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            Text(" +", fontSize = Type.title, color = MaterialTheme.colorScheme.primary)
                        }
                    }
                }
            }
        }

        HorizontalDivider()
        Row(Modifier.padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { vm.cancelEdit() }) { Text("취소") }
            Button(onClick = { vm.save() }, enabled = f.canSave) {
                Text(if (f.isNew) "그룹 만들기" else "저장")
            }
        }
    }
}

@Composable
private fun Toggle(label: String, value: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 1.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.weight(1f), fontSize = Type.body)
        Switch(checked = value, onCheckedChange = onChange)
    }
}
