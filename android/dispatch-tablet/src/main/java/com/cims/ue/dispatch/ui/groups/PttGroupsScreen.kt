// [PTT 그룹] 화면 (docs/design/features/android_dispatch_tablet.md §6.6, dispatch_desktop_ui.md §4.7)
//
// 카드 둘 — 왼쪽 좁은 목록(1) : 오른쪽 상세/편집(3). 편집은 **같은 자리의 인라인 폼**이다(별창 없음).
// 편집 중에는 목록이 잠긴다 — 저장·취소로만 나온다.
package com.cims.ue.dispatch.ui.groups

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
import com.cims.ue.dispatch.session.ManagedGroup

@Composable
fun PttGroupsScreen(vm: PttGroupsViewModel, onGoDispatch: () -> Unit, modifier: Modifier = Modifier) {
    val rows by vm.rows.collectAsStateWithLifecycle()
    val sel by vm.selected.collectAsStateWithLifecycle()
    val form by vm.form.collectAsStateWithLifecycle()
    val loading by vm.loading.collectAsStateWithLifecycle()
    val error by vm.error.collectAsStateWithLifecycle()

    LaunchedEffect(Unit) { if (rows.isEmpty()) vm.load() }

    Column(modifier.fillMaxSize()) {
        if (error.isNotBlank()) Text(error, Modifier.fillMaxWidth().padding(12.dp, 6.dp),
            color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
        if (loading) LinearProgressIndicator(Modifier.fillMaxWidth())
        Row(Modifier.weight(1f)) {
            GroupList(vm, rows, sel, Modifier.weight(1f))
            VerticalDivider()
            Box(Modifier.weight(3f).fillMaxHeight()) {
                when {
                    form != null -> EditPane(vm, form!!)
                    sel != null -> DetailPane(vm, sel!!, onGoDispatch)
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
private fun GroupList(vm: PttGroupsViewModel, rows: List<ManagedGroup>,
                      sel: ManagedGroup?, modifier: Modifier) {
    val filter by vm.filter.collectAsStateWithLifecycle()
    val query by vm.query.collectAsStateWithLifecycle()
    val locked = vm.locked

    Column(modifier.fillMaxHeight()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Text("그룹 ${rows.size}개", fontSize = 13.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { vm.load() }, enabled = !locked,
                contentPadding = PaddingValues(horizontal = 8.dp)) { Text("↻") }
            TextButton(onClick = { vm.newGroup() }, enabled = !locked,
                contentPadding = PaddingValues(horizontal = 8.dp)) { Text("+ 새 그룹") }
        }
        Row(Modifier.padding(horizontal = 10.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            GroupFilter.entries.forEach { f ->
                FilterChip(selected = filter == f, onClick = { vm.setFilter(f) },
                    enabled = !locked, label = { Text(f.label, fontSize = 11.sp) })
            }
        }
        OutlinedTextField(
            value = query, onValueChange = vm::search, enabled = !locked,
            placeholder = { Text("그룹명·id", fontSize = 12.sp) }, singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 6.dp),
            textStyle = MaterialTheme.typography.bodySmall)
        HorizontalDivider()
        LazyColumn(Modifier.weight(1f)) {
            items(rows, key = { it.id }) { g -> GroupRow(g, g.id == sel?.id, locked) { vm.select(g) } }
        }
    }
}

@Composable
private fun GroupRow(g: ManagedGroup, selected: Boolean, locked: Boolean, onClick: () -> Unit) {
    val bg = if (selected) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent
    Column(Modifier.fillMaxWidth().background(bg).clickable(enabled = !locked, onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Chip(g.relation)
            Spacer(Modifier.width(6.dp))
            Text(g.name.ifBlank { g.id }, fontSize = 14.sp, fontWeight = FontWeight.Bold, maxLines = 1)
        }
        Text("멤버 ${g.memberCount} · ${g.id}", fontSize = 11.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
    }
    HorizontalDivider()
}

@Composable
private fun Chip(text: String) {
    Surface(color = MaterialTheme.colorScheme.primary.copy(alpha = 0.18f), shape = RoundedCornerShape(4.dp)) {
        Text(text, Modifier.padding(horizontal = 5.dp, vertical = 1.dp), fontSize = 10.sp,
            color = MaterialTheme.colorScheme.primary)
    }
}

// ── 상세 ────────────────────────────────────────────────────────────────────

@Composable
private fun DetailPane(vm: PttGroupsViewModel, g: ManagedGroup, onGoDispatch: () -> Unit) {
    val book by vm.book.collectAsStateWithLifecycle()
    var confirmDelete by remember(g.id) { mutableStateOf(false) }

    Column(Modifier.fillMaxSize().padding(14.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(g.name.ifBlank { g.id }, fontSize = 18.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.width(8.dp))
            Text(g.id, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.width(8.dp))
            if (g.isMember) Chip("멤버")
            Spacer(Modifier.weight(1f))
            // 관리 판정은 서버가 한다 — 앱은 서버가 내려 준 canManage 로 버튼만 접는다(§4.7).
            if (g.canManage) TextButton(onClick = { vm.edit(g) }) { Text("편집") }
        }
        Spacer(Modifier.height(10.dp))
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
            Field("소속", if (g.orgCode.isBlank()) "미지정" else g.orgCode)
            Field("세션 종류", g.sessionType.ifBlank { "prearranged" })
            Field("멤버", "${g.memberCount}명")
            Field("관계", g.relation + if (g.isOwner) " · 내 소유" else "")
            if (!g.canManage) Text("보기 전용 — 관리 범위 밖이거나 내 소유가 아닙니다",
                fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp))
            if (book.entries.isEmpty()) Text("전화번호부를 아직 받지 못했습니다 — 멤버 이름은 번호로 보입니다",
                fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp))
        }
        HorizontalDivider()
        Row(Modifier.padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (g.canManage) TextButton(onClick = { confirmDelete = true }) {
                Text("삭제", color = MaterialTheme.colorScheme.error)
            }
            Spacer(Modifier.weight(1f))
            Button(onClick = { vm.openChannel(g, onGoDispatch) }) {
                Text(if (g.isMember) "채널로 (합류)" else "채널로")
            }
        }
    }

    if (confirmDelete) AlertDialog(
        onDismissRequest = { confirmDelete = false },
        title = { Text("그룹 삭제") },
        text = { Text("«${g.name.ifBlank { g.id }}» 을(를) 지웁니다. 되돌릴 수 없습니다.") },
        confirmButton = {
            TextButton(onClick = { confirmDelete = false; vm.delete(g) }) {
                Text("삭제", color = MaterialTheme.colorScheme.error)
            }
        },
        dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("취소") } })
}

@Composable
private fun Field(label: String, value: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
        Text(label, Modifier.width(110.dp), fontSize = 12.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, fontSize = 13.sp)
    }
}

// ── 편집 폼 ─────────────────────────────────────────────────────────────────

@Composable
private fun EditPane(vm: PttGroupsViewModel, f: EditForm) {
    Column(Modifier.fillMaxSize().padding(14.dp)) {
        Text(f.title, fontSize = 16.sp, fontWeight = FontWeight.Bold)
        if (f.error.isNotBlank()) Text(f.error, Modifier.padding(top = 4.dp),
            color = MaterialTheme.colorScheme.error, fontSize = 12.sp)
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
                    supportingText = { if (!f.isNew) Text("만든 뒤에는 바꿀 수 없습니다", fontSize = 11.sp) },
                    modifier = Modifier.fillMaxWidth())
                Spacer(Modifier.height(6.dp))
                Text("세션 종류", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    PttGroupsViewModel.SESSION_TYPES.forEach { t ->
                        FilterChip(selected = f.sessionType == t,
                            onClick = { vm.update { it.copy(sessionType = t) } },
                            label = { Text(t, fontSize = 11.sp) })
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
                Text("멤버 ${f.members.size}명", fontSize = 13.sp, fontWeight = FontWeight.Bold)
                Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
                    f.members.forEach { m ->
                        Row(Modifier.fillMaxWidth().padding(vertical = 2.dp),
                            verticalAlignment = Alignment.CenterVertically) {
                            Text(m.label + if (m.isMe) " (나)" else "", Modifier.weight(1f), fontSize = 12.sp)
                            TextButton(onClick = { vm.toggleChair(m.uri) },
                                contentPadding = PaddingValues(horizontal = 6.dp)) {
                                Text(if (m.isChair) "의장" else "참가자", fontSize = 11.sp)
                            }
                            TextButton(onClick = { vm.removeMember(m.uri) },
                                contentPadding = PaddingValues(horizontal = 6.dp)) {
                                Text("−", fontSize = 14.sp)
                            }
                        }
                    }
                }
                HorizontalDivider()
                OutlinedTextField(value = f.search, onValueChange = { v -> vm.update { it.copy(search = v) } },
                    placeholder = { Text("PTT 주소록 검색", fontSize = 12.sp) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
                    textStyle = MaterialTheme.typography.bodySmall)
                val cands by vm.candidates.collectAsStateWithLifecycle()
                LazyColumn(Modifier.weight(1f)) {
                    items(cands, key = { it.msisdn }) { c ->
                        Row(Modifier.fillMaxWidth().clickable { vm.addMember(c.msisdn, c.name) }
                                .padding(vertical = 3.dp),
                            verticalAlignment = Alignment.CenterVertically) {
                            Text(c.name.ifBlank { c.msisdn }, Modifier.weight(1f), fontSize = 12.sp)
                            Text(c.msisdn, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            Text(" +", fontSize = 14.sp, color = MaterialTheme.colorScheme.primary)
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
        Text(label, Modifier.weight(1f), fontSize = 12.sp)
        Switch(checked = value, onCheckedChange = onChange)
    }
}
