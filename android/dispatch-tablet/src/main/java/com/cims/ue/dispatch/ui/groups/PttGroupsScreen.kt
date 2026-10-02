// [PTT 그룹] 화면 (docs/design/features/android_dispatch_tablet.md §6.12, dispatch_desktop_ui.md §4.7)
//
// 카드 둘 — 왼쪽 그룹 목록(300) : 오른쪽 선택 그룹 카드. 관제 화면과 같은 시각 언어다(알약 버튼 · 각진 필터 칩 · 19px 라벨 ·
// 아바타 · 흰 카드). [편집]·[+ 새 그룹] 은 **같은 화면의 인라인 폼**이다(별창 없음) — 폼은 본문 폭 전체를 쓴다: 편집 중에는
// 목록이 잠기므로(저장·취소로만 나온다) 그 자리를 속성 절·PTT 주소록·멤버 세 칸에 내준다.
package com.cims.ue.dispatch.ui.groups

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Edit
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
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.session.ManagedGroup
import com.cims.ue.dispatch.ui.CountPill
import com.cims.ue.dispatch.ui.FilterPill
import com.cims.ue.dispatch.ui.ForwardPttKeys
import com.cims.ue.dispatch.ui.HDivider
import com.cims.ue.dispatch.ui.Initial
import com.cims.ue.dispatch.ui.Label
import com.cims.ue.dispatch.ui.LabelStyle
import com.cims.ue.dispatch.ui.Pill
import com.cims.ue.dispatch.ui.PillButton
import com.cims.ue.dispatch.ui.Rect
import com.cims.ue.dispatch.ui.RectButton
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type
import com.cims.ue.sdk.GroupDoc

@Composable
fun PttGroupsScreen(vm: PttGroupsViewModel, onOpenChannel: (String) -> Unit, modifier: Modifier = Modifier) {
    val rows by vm.rows.collectAsStateWithLifecycle()
    val all by vm.groups.collectAsStateWithLifecycle()
    val sel by vm.selected.collectAsStateWithLifecycle()
    val form by vm.form.collectAsStateWithLifecycle()
    val loading by vm.loading.collectAsStateWithLifecycle()
    val error by vm.error.collectAsStateWithLifecycle()
    val filter by vm.filter.collectAsStateWithLifecycle()
    val query by vm.query.collectAsStateWithLifecycle()
    val book by vm.book.collectAsStateWithLifecycle()
    val detail by vm.detail.collectAsStateWithLifecycle()
    val detailBusy by vm.detailBusy.collectAsStateWithLifecycle()
    val doc by vm.detailDoc.collectAsStateWithLifecycle()
    val detailError by vm.detailError.collectAsStateWithLifecycle()
    val live by vm.detailLive.collectAsStateWithLifecycle()
    val videoIds by vm.videoIds.collectAsStateWithLifecycle()
    val hint by vm.hint.collectAsStateWithLifecycle()
    val candidates by vm.candidates.collectAsStateWithLifecycle()

    LaunchedEffect(Unit) { if (rows.isEmpty()) vm.load() }

    PttGroupsScreenContent(
        ui = GroupsUi(rows = rows, selected = sel, detail = detail, detailBusy = detailBusy,
            book = book, filter = filter, query = query, loading = loading, error = error,
            locked = form != null, editing = form != null, total = all.size,
            doc = doc, detailError = detailError, hasSession = live, videoIds = videoIds,
            canCreate = vm.canCreate, listenHidden = vm.listenHidden,
            myPttId = vm.myPttId, myName = vm.myName, hint = hint),
        act = GroupsActions(
            reload = vm::load, newGroup = vm::newGroup, setFilter = vm::setFilter, search = vm::search,
            select = vm::select, edit = vm::edit, delete = vm::delete,
            openChannel = { g -> vm.openChannel(g, onOpenChannel) }),
        editPane = {
            form?.let { f ->
                GroupEditPane(f, candidates, book, GroupFormActions(
                    change = vm::update, add = { e -> vm.addMember(e.msisdn, e.name) }, addAllShown = vm::addAllShown,
                    remove = vm::removeMember, toggleChair = vm::toggleChair, toggleRequired = vm::toggleRequired,
                    cancel = vm::cancelEdit, save = vm::save))
            }
        },
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
    /** 편집 중 — 목록·[↻]·[+ 새 그룹]이 잠긴다(§4.7). */
    val locked: Boolean = false,
    val editing: Boolean = false,
    /** «그룹 N» — 필터 전의 전체 수. */
    val total: Int = rows.size,
    /** 선택 그룹의 GMS 문서 — 정보 칸·능력 칩. 받기 전이면 null(«…»). */
    val doc: GroupDoc? = null,
    val detailError: String = "",
    /** 선택 그룹에 세션이 돌고 있다 — 머리 라벨 «세션 진행 중»·삭제 확인의 경고. */
    val hasSession: Boolean = false,
    /** MCVideo 그룹으로 알려진 id — 서비스 칩 «영상». */
    val videoIds: Set<String> = emptySet(),
    /** 그룹 생성 자격 — 없으면 [+ 새 그룹] 을 세우지 않는다(최종 판정은 GMS). */
    val canCreate: Boolean = false,
    /** 이 관제석의 청취가 로스터에 보이지 않는다 — 정보 칸 «청취 노출». */
    val listenHidden: Boolean = true,
    val myPttId: String = "",
    val myName: String = "",
    /** 목록 범위 안내 — 상세 카드 바닥. */
    val hint: String = "",
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

/** 편집 폼의 조작 — 값은 [EditForm] 이, 판정은 [PttGroupsViewModel] 이 갖는다. */
data class GroupFormActions(
    val change: FormChange = {},
    val add: (DirectoryEntry) -> Unit = {},
    val addAllShown: () -> Unit = {},
    val remove: (String) -> Unit = {},
    val toggleChair: (String) -> Unit = {},
    val toggleRequired: (String) -> Unit = {},
    val cancel: () -> Unit = {},
    val save: () -> Unit = {},
)

/** [PTT 그룹] 본문 — **순수 컴포저블**. 편집 폼만 호출자가 넘긴다. */
@Composable
fun PttGroupsScreenContent(
    ui: GroupsUi,
    act: GroupsActions = GroupsActions(),
    editPane: @Composable () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    val p = Tokens.palette
    Row(modifier.fillMaxSize().background(p.bg).padding(horizontal = 16.dp, vertical = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(14.dp)) {
        if (ui.editing) {
            CardFrame(Modifier.weight(1f).fillMaxHeight()) { editPane() }
        } else {
            GroupList(ui, act, Modifier.width(300.dp).fillMaxHeight())
            CardFrame(Modifier.weight(1f).fillMaxHeight()) {
                val g = ui.selected
                if (g != null) DetailPane(ui, act, g)
                else Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center) {
                    Icon(Icons.Filled.Groups, contentDescription = null, tint = p.muted,
                        modifier = Modifier.size(36.dp).alpha(0.35f))
                    Gap(10.dp)
                    Text("그룹을 고르면 여기에 상세가 보입니다", fontSize = Type.body, color = p.muted)
                }
            }
        }
    }
}

// ── 목록 ────────────────────────────────────────────────────────────────────

@Composable
private fun GroupList(ui: GroupsUi, act: GroupsActions, modifier: Modifier = Modifier) {
    val locked = ui.locked
    CardFrame(modifier) {
        Column(Modifier.padding(start = 16.dp, end = 12.dp, top = 10.dp, bottom = 10.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                CountTitle("그룹", ui.total)
                Spacer(Modifier.weight(1f))
                IconSquare(Icons.Filled.Refresh, "목록 새로고침", act.reload, enabled = !locked)
                // 사람을 먼저 고르는 길은 [채널 추가] 패널의 [그룹 추가 ›] 다. 여기서는 빈 폼(나 = 의장)에서 시작한다 — 같은 폼이다.
                if (ui.canCreate) {
                    GapW(6.dp)
                    PillButton("새 그룹", act.newGroup, kind = Pill.INK, height = 32.dp, enabled = !locked,
                        leading = Icons.Filled.Add)
                }
            }
            SearchBox(ui.query, act.search, "그룹 이름 · id 검색", enabled = !locked)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                GroupFilter.entries.forEach { f ->
                    FilterPill(f.label, ui.filter == f, onClick = { if (!locked) act.setFilter(f) })
                }
            }
            if (ui.error.isNotBlank()) SoftBand(ui.error)
        }
        if (ui.loading) LinearProgressIndicator(Modifier.fillMaxWidth().height(2.dp)) else HDivider(hair = true)
        if (ui.rows.isEmpty() && !ui.loading) EmptyNote("조건에 맞는 그룹이 없습니다")
        LazyColumn(Modifier.weight(1f)) {
            items(ui.rows, key = { it.id }) { g ->
                GroupRow(g, orgPath = orgPathOf(ui.book, g.orgCode), video = g.id in ui.videoIds,
                    selected = g.id == ui.selected?.id, locked = locked) { act.select(g) }
            }
        }
    }
}

/** 조직 경로 — 주소록의 조직 트리로 푼다. 트리에 없는 코드(범위 밖)는 코드 그대로 보인다. */
private fun orgPathOf(book: DirectoryBook, code: String): String =
    if (code.isBlank()) "" else book.orgPath(code).ifBlank { code }

/** 관계 라벨의 색조 — 멤버 = 연한 남색 › 청취 범위 = 청록 › 소유 · 범위 = 무채. */
private fun relationStyle(g: ManagedGroup): LabelStyle = when {
    g.isMember -> LabelStyle.INK
    g.inListenScope -> LabelStyle.TEAL
    else -> LabelStyle.FILL
}

/** 목록 행(62) — 그룹 아바타 · 이름 + 관계 라벨 · 인원 / id · 소속 · 서비스 칩. */
@Composable
private fun GroupRow(g: ManagedGroup, orgPath: String, video: Boolean, selected: Boolean, locked: Boolean,
                     onClick: () -> Unit) {
    val p = Tokens.palette
    val name = g.name.ifBlank { g.id }
    Row(Modifier.fillMaxWidth().height(62.dp).background(if (selected) p.primarySoft else Color.Transparent)
            .drawBehind {
                drawLine(p.hair, Offset(0f, size.height), Offset(size.width, size.height), 1.dp.toPx())
                if (selected) drawRect(p.primaryLine, size = Size(3.dp.toPx(), size.height))
            }
            .clickable(enabled = !locked, onClick = onClick)
            .padding(start = 13.dp, end = 14.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Initial(name, size = 36.dp, square = true)
        Column(Modifier.weight(1f).padding(start = 11.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(name, fontSize = Type.strong, fontWeight = FontWeight.SemiBold, maxLines = 1,
                    overflow = TextOverflow.Ellipsis, color = if (selected) p.primaryInk else p.ink,
                    modifier = Modifier.weight(1f, fill = false))
                Label(g.relation, relationStyle(g), Modifier.padding(start = 7.dp))
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(listOf(g.id, orgPath).filter { it.isNotEmpty() }.joinToString(" · "), fontSize = Type.micro,
                    color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                // 서비스 칩(§10.6) — 음성(MCPTT, 늘) · 영상(MCVideo 로 알려진 그룹)
                Label("음성", LabelStyle.FILL, Modifier.padding(start = 6.dp), bold = false)
                if (video) Label("영상", LabelStyle.TEAL, Modifier.padding(start = 4.dp))
            }
        }
        Row(Modifier.padding(start = 8.dp), verticalAlignment = Alignment.Bottom) {
            Text("${g.memberCount}", fontSize = Type.body, fontWeight = FontWeight.Bold)
            Text("명", fontSize = Type.meta, color = p.ink2)
        }
    }
}

// ── 상세 ────────────────────────────────────────────────────────────────────

@Composable
private fun DetailPane(ui: GroupsUi, act: GroupsActions, g: ManagedGroup) {
    val p = Tokens.palette
    val doc = ui.doc
    val members = ui.detail
    val name = g.name.ifBlank { g.id }
    val orgPath = orgPathOf(ui.book, g.orgCode)
    val video = g.id in ui.videoIds || doc?.mcvideo != null
    var confirmDelete by remember(g.id) { mutableStateOf(false) }

    Column(Modifier.fillMaxSize()) {
        // 머리: 아바타 · 이름 · 라벨 · id — [채널로][편집][삭제]
        Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 16.dp, top = 14.dp, bottom = 14.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Initial(name, size = 46.dp, square = true)
            Column(Modifier.weight(1f).padding(start = 14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(name, fontSize = Type.display, fontWeight = FontWeight.Bold, maxLines = 1,
                        overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false).padding(end = 4.dp))
                    Label(g.relation, relationStyle(g))
                    if (video) Label("영상", LabelStyle.TEAL)
                    if (ui.hasSession) Label("세션 진행 중", LabelStyle.TALK)
                    // 관리 판정은 서버가 한다 — 앱은 서버가 내려 준 canManage 로 버튼만 접는다(§4.7).
                    if (!g.canManage) Label("보기 전용", LabelStyle.OUTLINE, bold = false)
                }
                Text(listOf(g.id, orgPath).filter { it.isNotEmpty() }.joinToString(" · "), fontSize = Type.meta,
                    color = p.muted, fontFamily = FontFamily.Monospace, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                // 채널이 있는 그룹만 — 멤버(내 채널 카드)·청취 범위(타 채널 행). 관리 범위만 있으면 태블릿에 갈 채널이 없다.
                if (g.hasChannel) PillButton("채널로", { act.openChannel(g) }, kind = Pill.LINE)
                if (g.canManage) {
                    PillButton("편집", { act.edit(g) }, kind = Pill.INK, leading = Icons.Filled.Edit)
                    PillButton("삭제", { confirmDelete = true }, kind = Pill.RED)
                }
            }
        }
        HDivider()

        LazyColumn(Modifier.weight(1f).padding(start = 20.dp, end = 16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)) {
            item { Gap(6.dp) }
            if (ui.detailError.isNotBlank()) item { SoftBand(ui.detailError) }
            if (ui.detailBusy && doc == null) item { FormHint("그룹 문서 조회 중…") }
            // 정보 칸 여섯 — 문서를 받기 전에는 «…»
            item {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    InfoTile("소유자", doc?.let { ownerLabel(it.authorizedUser, ui.myPttId, ui.myName, ui.book::nameOf) } ?: "…",
                        Modifier.weight(1f))
                    InfoTile("소속", orgPath.ifEmpty { "—" }, Modifier.weight(1f))
                    InfoTile("세션 종류", doc?.let { sessionTypeText(it.sessionType) } ?: "…", Modifier.weight(1f))
                }
            }
            item {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    InfoTile("그룹 우선순위", doc?.priority?.toString() ?: "…", Modifier.weight(1f))
                    InfoTile("긴급", doc?.let(::emergencyText) ?: "…", Modifier.weight(1f))
                    // 이 관제석의 청취가 그룹 로스터에 보이는가(관제 역할의 listen_visibility)
                    InfoTile("청취 노출", if (ui.listenHidden) "은닉" else "투명", Modifier.weight(1f))
                }
            }
            // 능력 칩 — 음성(늘) · SDS · FD · MCVideo · 암호화 · affiliation 필요
            if (doc != null) item {
                @OptIn(ExperimentalLayoutApi::class)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    capabilityChips(doc).forEach { CapabilityChip(it) }
                }
            }
            // 멤버 — 2열, 참여 › 미참가 순(`detailMembers`). 채널 카드 3줄이 «+n» 으로 접은 로스터의 **전체**가 여기다(§6.3b).
            item {
                Row(Modifier.fillMaxWidth().padding(top = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("멤버 ${if (members.isEmpty()) g.memberCount else members.size}", fontSize = Type.strong,
                        fontWeight = FontWeight.Bold)
                    Spacer(Modifier.weight(1f))
                    if (members.isNotEmpty()) Text("affiliation ${members.count { !it.absent }}", fontSize = Type.meta,
                        color = p.muted)
                }
            }
            if (ui.book.entries.isEmpty() && members.isNotEmpty())
                item { FormHint("전화번호부를 아직 받지 못했습니다 — 멤버 이름은 번호로 보입니다") }
            if (members.isEmpty() && !ui.detailBusy) item { FormHint("멤버 목록을 받지 못했습니다") }
            items(members.chunked(2)) { pair ->
                Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    MemberCell(pair[0], Modifier.weight(1f))
                    if (pair.size > 1) MemberCell(pair[1], Modifier.weight(1f)) else Spacer(Modifier.weight(1f))
                }
            }
            item { Gap(6.dp) }
        }
        // 바닥 안내 — 목록 범위
        if (ui.hint.isNotBlank()) {
            HDivider(hair = true)
            Text(ui.hint, fontSize = Type.meta, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp))
        }
    }

    if (confirmDelete) AlertDialog(
        onDismissRequest = { confirmDelete = false },
        title = { ForwardPttKeys(); Text("그룹 삭제") },
        text = {
            Text("그룹 «$name» (${g.id}) 을(를) 삭제할까요?\n멤버 ${g.memberCount}명의 단말에서도 사라집니다." +
                if (ui.hasSession) "\n진행 중인 세션이 있습니다 — 삭제하면 서버가 세션을 정리합니다." else "")
        },
        confirmButton = {
            TextButton(onClick = { confirmDelete = false; act.delete(g) }) { Text("삭제", color = p.emg) }
        },
        dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("취소") } })
}

/** 정보 칸 — 연한 면 위에 작은 이름표 + 값. */
@Composable
private fun InfoTile(label: String, value: String, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    Column(modifier.clip(RoundedCornerShape(8.dp)).background(p.fill).padding(horizontal = 14.dp, vertical = 9.dp),
        verticalArrangement = Arrangement.spacedBy(3.dp)) {
        Text(label, fontSize = Type.micro, color = p.ink2, maxLines = 1)
        Text(value, fontSize = Type.strong, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/** 능력 칩(24) — 그 그룹이 할 수 있는 것 하나. */
@Composable
private fun CapabilityChip(text: String) {
    val p = Tokens.palette
    Box(Modifier.height(24.dp).clip(RoundedCornerShape(4.dp)).background(p.fill).padding(horizontal = 9.dp),
        contentAlignment = Alignment.Center) {
        Text(text, fontSize = Type.meta, color = p.ink2, maxLines = 1)
    }
}

/** 상세 멤버 한 칸 — 아바타 · 이름 · (나) · [의장] · 번호 / 지금 상태. 발언 중은 녹색 라벨, 미참가는 줄을 흐리게. */
@Composable
private fun MemberCell(m: DetailMember, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    Row(modifier.alpha(if (m.absent) 0.55f else 1f)
            .drawBehind { drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
            .padding(start = 2.dp, end = 6.dp, top = 7.dp, bottom = 7.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Initial(m.name, size = 30.dp, strong = m.isMe)
        Column(Modifier.weight(1f).padding(start = 10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(m.name, fontSize = Type.body, fontWeight = FontWeight.SemiBold, maxLines = 1,
                    overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                if (m.isMe) Text(" (나)", fontSize = Type.meta, color = p.ink2)
                if (m.isChair) Label("의장", LabelStyle.FILL, Modifier.padding(start = 6.dp), bold = false)
            }
            Text(com.cims.ue.dispatch.session.localNumber(m.number), fontSize = Type.micro, color = p.muted, fontFamily = FontFamily.Monospace, maxLines = 1)
        }
        when (m.status) {
            DetailMember.SPEAKING -> Label("발언 중", LabelStyle.TALK, Modifier.padding(start = 8.dp))
            else -> Text(m.status, fontSize = Type.meta, color = if (m.absent) p.ink2 else p.talkInk,
                modifier = Modifier.padding(start = 8.dp))
        }
    }
}

// ── 편집 폼 ─────────────────────────────────────────────────────────────────

/**
 * 그룹 생성·편집 폼 — **순수 컴포저블**. 왼쪽 = 속성 절 넷(기본 · 그룹 호 · 허용·한도 · 서비스) · 가운데 = PTT 주소록(고르는 곳) ·
 * 오른쪽 = 멤버(고른 것) · 바닥 = 오류 + [취소][저장]. 문서를 받는 동안은 본문을 비워 둔다 — 받기 전에 고친 값이
 * 도착한 문서에 덮이지 않게.
 *
 * @param candidates 멤버 후보(PTT 주소록에서 이미 멤버인 번호를 뺀 것, 검색 반영).
 */
@Composable
internal fun GroupEditPane(
    f: EditForm,
    candidates: List<DirectoryEntry>,
    book: DirectoryBook,
    act: GroupFormActions = GroupFormActions(),
) {
    val p = Tokens.palette
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 16.dp, top = 12.dp, bottom = 12.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Text(f.title, fontSize = Type.display, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f))
            Label("편집 중", LabelStyle.INK)
            Text("저장하거나 취소하면 목록으로 돌아갑니다", fontSize = Type.meta, color = p.muted,
                modifier = Modifier.padding(start = 8.dp))
        }
        HDivider()
        if (!f.loaded) {
            EmptyNote(if (f.busy) "그룹 문서 조회 중…" else "그룹 문서를 받지 못했습니다", Modifier.weight(1f))
        } else Row(Modifier.weight(1f)) {
            // ═══ 속성 ═══
            Column(Modifier.width(384.dp).fillMaxHeight().verticalScroll(rememberScrollState())
                    .padding(start = 16.dp, end = 6.dp, top = 12.dp, bottom = 8.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)) {
                BasicSection(f, act.change)
                GroupCallSection(f, act.change)
                LimitsSection(f, act.change)
                ServiceSection(f, act.change)
                FormHint("저장하면 GMS 그룹 문서로 서버에 올라가고(XCAP PUT), 멤버 단말에는 문서 변경 통지가 갑니다. " +
                    "편집·삭제는 내가 만든 그룹(또는 관리 범위 안 그룹)만 됩니다.")
            }
            // ═══ 멤버 — PTT 주소록(고르는 곳) → 멤버(고른 것) ═══
            Row(Modifier.weight(1f).fillMaxHeight().padding(start = 6.dp, end = 16.dp, top = 12.dp, bottom = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                CandidateCard(f, candidates, book, act, Modifier.weight(1f).fillMaxHeight())
                MemberCard(f, act, Modifier.weight(1f).fillMaxHeight())
            }
        }
        FormFooter(error = f.error) {
            PillButton("취소", act.cancel, kind = Pill.LINE, height = 44.dp)
            PillButton(f.saveLabel, act.save, kind = Pill.INK, height = 44.dp, enabled = f.canSave)
        }
    }
}

/** PTT 주소록 카드 — 검색으로 좁히고 한 명씩 [＋ 추가], 또는 보이는 후보 전부를 한 번에. */
@Composable
private fun CandidateCard(f: EditForm, candidates: List<DirectoryEntry>, book: DirectoryBook, act: GroupFormActions,
                          modifier: Modifier = Modifier) {
    val p = Tokens.palette
    CardFrame(modifier) {
        Column(Modifier.padding(start = 14.dp, end = 12.dp, top = 10.dp, bottom = 10.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("PTT 주소록", fontSize = Type.strong, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
                // 검색으로 좁힌 뒤 한 번에 — 보이는 후보만 넣는다(주소록 전부가 아니다).
                PillButton("표시된 ${candidates.size}명 추가", act.addAllShown, kind = Pill.LINE, height = 30.dp,
                    enabled = candidates.isNotEmpty())
            }
            SearchBox(f.search, { v -> act.change { it.copy(search = v) } }, "이름 · 번호 검색")
        }
        HDivider(hair = true)
        if (candidates.isEmpty()) EmptyNote(when {
            book.entries.isEmpty() -> "PTT 주소록을 받지 못했습니다"
            f.search.isNotBlank() -> "일치하는 사람이 없습니다"
            else -> "주소록의 모두가 이미 멤버입니다"
        })
        LazyColumn(Modifier.weight(1f)) {
            // 열쇠를 주지 않는다 — 주소록에 같은 번호가 두 줄 있으면(서버 줄 + 가져온 CSV 줄) 중복 열쇠로 목록이 죽는다.
            items(candidates) { c ->
                val name = c.name.ifBlank { c.msisdn }
                Row(Modifier.fillMaxWidth().clickable { act.add(c) }
                        .drawBehind { drawLine(p.hair, Offset(0f, size.height), Offset(size.width, size.height), 1.dp.toPx()) }
                        .padding(start = 14.dp, end = 12.dp, top = 7.dp, bottom = 7.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Initial(name, size = 30.dp)
                    Column(Modifier.weight(1f).padding(horizontal = 10.dp)) {
                        Text(name, fontSize = Type.body, fontWeight = FontWeight.SemiBold, maxLines = 1,
                            overflow = TextOverflow.Ellipsis)
                        Text(listOf(com.cims.ue.dispatch.session.localNumber(c.msisdn), orgPathOf(book, c.org)).filter { it.isNotEmpty() }.joinToString("  "),
                            fontSize = Type.micro, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    PillButton("추가", { act.add(c) }, height = 30.dp, leading = Icons.Filled.Add)
                }
            }
        }
    }
}

/** 멤버 카드 — 고른 사람들. 줄마다 [필수/선택]·[의장/참가자] 토글과 [×] 빼기. */
@Composable
private fun MemberCard(f: EditForm, act: GroupFormActions, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    CardFrame(modifier) {
        Column(Modifier.padding(start = 14.dp, end = 12.dp, top = 10.dp, bottom = 10.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(Modifier.height(30.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("멤버", fontSize = Type.strong, fontWeight = FontWeight.Bold)
                CountPill(f.members.size, Modifier.padding(start = 8.dp))
            }
            FormHint("의장은 발언 우선순위가 높습니다. 필수 멤버는 개시자에게 응답하기 전에 그 멤버의 응답을 기다립니다. 눌러서 바꿉니다.")
        }
        HDivider(hair = true)
        if (f.members.isEmpty()) EmptyNote("멤버가 없습니다 — 왼쪽 주소록에서 추가하세요")
        LazyColumn(Modifier.weight(1f)) {
            items(f.members) { m ->
                Row(Modifier.fillMaxWidth()
                        .drawBehind { drawLine(p.hair, Offset(0f, size.height), Offset(size.width, size.height), 1.dp.toPx()) }
                        .padding(start = 14.dp, end = 8.dp, top = 7.dp, bottom = 7.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Initial(m.label, size = 30.dp, strong = m.isMe)
                    Column(Modifier.weight(1f).padding(horizontal = 10.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(m.label, fontSize = Type.body, fontWeight = FontWeight.SemiBold, maxLines = 1,
                                overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                            if (m.isMe) Text(" (나)", fontSize = Type.meta, color = p.ink2)
                        }
                        Text(com.cims.ue.dispatch.session.localNumber(m.number), fontSize = Type.micro, color = p.muted, fontFamily = FontFamily.Monospace, maxLines = 1)
                    }
                    // 필수 멤버(on-network-required) · 의장 — 켜지면 채움
                    RectButton(if (m.required) "필수" else "선택", { act.toggleRequired(m.uri) },
                        Modifier.width(58.dp), kind = if (m.required) Rect.INK else Rect.SOFT, height = 30.dp)
                    GapW(4.dp)
                    RectButton(if (m.isChair) "의장" else "참가자", { act.toggleChair(m.uri) },
                        Modifier.width(64.dp), kind = if (m.isChair) Rect.ON else Rect.SOFT, height = 30.dp, bold = m.isChair)
                    IconSquare(Icons.Filled.Close, "멤버에서 빼기", { act.remove(m.uri) }, size = 32.dp)
                }
            }
        }
    }
}
