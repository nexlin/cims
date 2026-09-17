// [PTT 그룹] 화면 상태 (docs/design/features/android_dispatch_tablet.md §6.6, dispatch_desktop_ui.md §4.7)
//
// 목록 원천은 관리 API(`/provisioning/directory/groups`) — **관리 범위 ∪ 내 소유 ∪ 청취 범위 ∪ 내 멤버**라
// 보기 전용 행이 섞인다. [편집]·[삭제]는 `canManage` 인 행에만 붙고, 실제 판정은 서버 GMS 게이트가 한다.
// 생성·편집·삭제는 XCAP PUT/DELETE(TS 24.481).
package com.cims.ue.dispatch.ui.groups

import com.cims.ue.dispatch.session.userPart
import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.ManagedGroup
import com.cims.ue.dispatch.session.ResponseText
import com.cims.ue.dispatch.session.TextArea
import com.cims.ue.dispatch.session.deleteGroup
import com.cims.ue.dispatch.session.getGroupDoc
import com.cims.ue.dispatch.session.joinGroup
import com.cims.ue.dispatch.session.newGroupUri
import com.cims.ue.dispatch.session.refreshGroups
import com.cims.ue.dispatch.session.saveGroup
import com.cims.ue.dispatch.session.telUri
import com.cims.ue.sdk.GroupDoc
import com.cims.ue.sdk.GroupMember
import com.cims.ue.sdk.RosterEntry
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/** 목록 필터 칩. */
enum class GroupFilter(val label: String) { ALL("전체"), MEMBER("멤버"), MINE("내 소유") }

/** 편집 폼의 멤버 한 줄. */
data class MemberRow(val uri: String, val name: String, val number: String,
                     val isChair: Boolean = false, val isMe: Boolean = false) {
    val label: String get() = name.ifBlank { number }
}

/**
 * 상세 «멤버» 한 줄 — **구성**(GMS 문서)에 **지금 상태**(로스터·발언자)를 겹친 것.
 * 데스크톱 `GroupDetailMember`(`GroupAdminViewModel.RefreshDetailMembers`) 대응.
 */
data class DetailMember(
    val name: String,
    val number: String,
    val status: String,
    val isMe: Boolean = false,
    val isChair: Boolean = false,
) {
    val absent: Boolean get() = status == ABSENT

    companion object {
        const val SPEAKING = "발언 중"
        const val JOINED = "참여"
        const val ABSENT = "미참가"
    }
}

/**
 * 구성 × 지금 상태 (순수 함수, 시험 대상). 데스크톱 `RefreshDetailMembers` 와 같은 규칙이다.
 *
 * - **명단은 문서가, 상태는 로스터가** 준다. 로스터에만 있고 문서에 없는 사람(은닉 아닌 청취자)은
 *   멤버가 아니므로 이 표에 나오지 않는다 — 이 표의 질문은 «편성된 사람이 지금 있나» 다.
 * - `connected` 와 `listener` 를 **둘 다 «참여»** 로 본다(그 자리에 있다는 뜻은 같다).
 * - **미참가는 뒤로** 보낸다. 같은 등급끼리는 문서 순서를 지킨다 — 갱신마다 줄이 뒤섞이면 읽을 수 없다.
 * - 번호 비교는 **정규형**으로. 로스터가 `tel:+8210…`, 문서가 `sip:010…` 이라 그대로 비교하면 안 붙는다.
 *
 * @param speaker 발언자 **표시명**(`SessionItem.speaker`) 또는 번호. 둘 다로 맞춰 본다.
 */
internal fun detailMembers(
    members: List<GroupMember>,
    roster: List<RosterEntry>,
    speaker: String,
    myPttId: String,
    nameOf: (String) -> String = { "" },
): List<DetailMember> {
    val present = roster
        .filter { it.status == "connected" || it.status == "listener" }
        .map { DirectoryBook.normalize(userPart(it.uri)) }
        .toHashSet()
    val speakerKey = DirectoryBook.normalize(userPart(speaker))
    val meKey = DirectoryBook.normalize(userPart(myPttId))
    val rows = members.map { m ->
        val number = userPart(m.uri)
        val key = DirectoryBook.normalize(number)
        val name = m.name.ifBlank { nameOf(number) }.ifBlank { number }
        val speaking = (speakerKey.isNotEmpty() && key == speakerKey) ||
            (speaker.isNotBlank() && speaker == name)
        DetailMember(
            name = name,
            number = number,
            status = when {
                speaking -> DetailMember.SPEAKING
                key in present -> DetailMember.JOINED
                else -> DetailMember.ABSENT
            },
            isMe = meKey.isNotEmpty() && key == meKey,
            isChair = m.role == "chair")
    }
    return rows.sortedBy { if (it.absent) 1 else 0 }  // 안정 정렬 — 같은 등급은 문서 순서 유지
}

/**
 * 편집 폼 상태. 화면은 이 값만 그리고 판정은 [PttGroupsViewModel] 이 한다.
 *
 * `ifMatch` 는 폼을 **열 때**의 ETag 다 — 저장 사이에 다른 곳에서 바뀌면 412 로 걸린다.
 */
data class EditForm(
    val isNew: Boolean = true,
    val uri: String = "",
    val groupId: String = "",
    val name: String = "",
    val sessionType: String = "prearranged",
    val videoEnabled: Boolean = false,
    val allowSds: Boolean = true,
    val allowFd: Boolean = false,
    val emergencyCall: Boolean = true,
    val emergencyAlert: Boolean = true,
    val requireAffiliation: Boolean = true,
    val encryption: Boolean = false,
    val priority: Int = 5,
    val maxParticipants: Int = 0,
    val orgCode: String = "",
    val members: List<MemberRow> = emptyList(),
    val ifMatch: String = "",
    val search: String = "",
    val loaded: Boolean = false,
    val busy: Boolean = false,
    val error: String = "",
) {
    val title: String get() = if (isNew) "새 PTT 그룹" else "그룹 편집 — $name"
    val canSave: Boolean get() = loaded && !busy && name.isNotBlank() && members.isNotEmpty() &&
        (!isNew || groupId.isNotBlank())
}

class PttGroupsViewModel(private val s: DispatchSession) : ScreenViewModel() {

    private val _groups = MutableStateFlow<List<ManagedGroup>>(emptyList())

    private val _rows = MutableStateFlow<List<ManagedGroup>>(emptyList())
    val rows: StateFlow<List<ManagedGroup>> = _rows.asStateFlow()

    private val _filter = MutableStateFlow(GroupFilter.ALL)
    val filter: StateFlow<GroupFilter> = _filter.asStateFlow()

    private val _query = MutableStateFlow("")
    val query: StateFlow<String> = _query.asStateFlow()

    private val _selected = MutableStateFlow<ManagedGroup?>(null)
    val selected: StateFlow<ManagedGroup?> = _selected.asStateFlow()

    /** null 이면 상세, 아니면 같은 자리에 인라인 편집 폼(별창 없음, §4.7). */
    private val _form = MutableStateFlow<EditForm?>(null)
    val form: StateFlow<EditForm?> = _form.asStateFlow()

    private val _loading = MutableStateFlow(false)
    val loading: StateFlow<Boolean> = _loading.asStateFlow()

    private val _error = MutableStateFlow("")
    val error: StateFlow<String> = _error.asStateFlow()

    private val _book = MutableStateFlow(DirectoryBook())
    val book: StateFlow<DirectoryBook> = _book.asStateFlow()

    // ── 상세 «멤버»(로스터 전체) ──────────────────────────────────────────────
    private val _detail = MutableStateFlow<List<DetailMember>>(emptyList())
    val detail: StateFlow<List<DetailMember>> = _detail.asStateFlow()

    private val _detailBusy = MutableStateFlow(false)
    val detailBusy: StateFlow<Boolean> = _detailBusy.asStateFlow()

    /** 선택된 그룹의 GMS 문서(멤버 구성). 라이브 상태가 바뀔 때마다 이것과 겹쳐 다시 그린다. */
    private var detailDoc: GroupDoc? = null
    private var detailFor: String = ""
    private var detailJob: Job? = null

    /** 목록이 오기 전에 들어온 선택 요청(① 3줄 [로스터 전체]). */
    private var pendingSelect: String = ""

    private var loadJob: Job? = null
    private var formJob: Job? = null

    init {
        // 구성은 문서가 주지만 **상태는 로스터가** 준다 — 로스터·발언자가 바뀌면 같은 문서로 다시 겹친다.
        //   문서를 다시 받지 않는다(구성은 XCAP 변경 통지로만 바뀐다).
        scope.launch { s.groups.collect { projectDetail() } }
        scope.launch { s.sessions.collect { projectDetail() } }
    }

    /** 편집 중에는 목록·[↻]·[+ 새 그룹]이 잠긴다 — 편집 대상이 바뀌지 않게(§4.7). */
    val locked: Boolean get() = _form.value != null

    fun setFilter(f: GroupFilter) { _filter.value = f; project() }
    fun search(q: String) { _query.value = q; project() }

    fun load() {
        loadJob?.cancel()
        loadJob = scope.launch { reload() }
    }

    /**
     * 목록을 다시 받는다. **저장·삭제 뒤에는 이걸 기다린 다음** 선택을 옮겨야 한다 —
     * `load()` 를 쏘고 바로 `_groups.value` 를 읽으면 옛 목록이라 방금 만든 그룹을 못 찾는다.
     */
    private suspend fun reload() {
        val m = s.management() ?: return run { _error.value = "로그인 전" }
        _loading.value = true
        _error.value = ""
        val r = m.listGroups()
        _loading.value = false
        if (!r.ok) { _error.value = r.reason; return }
        _groups.value = r.value.orEmpty()
        project()
        // 멤버 후보용 PTT 주소록 — 세션이 로그인 때 이미 받아 뒀다(같은 것을 두 번 받지 않는다).
        // 비어 있으면 그때 한 번 더 시도한다.
        _book.value = s.pttBook.value
        if (_book.value.entries.isEmpty())
            m.directory("ptt").let { d -> if (d.ok) d.value?.let { _book.value = it } }
    }

    private fun project() {
        val q = _query.value.trim().lowercase()
        _rows.value = _groups.value.filter { g ->
            when (_filter.value) {
                GroupFilter.MEMBER -> g.isMember
                GroupFilter.MINE -> g.isOwner
                GroupFilter.ALL -> true
            } && (q.isEmpty() || g.name.lowercase().contains(q) || g.id.lowercase().contains(q))
        }
        _selected.value?.let { sel -> if (_rows.value.none { it.id == sel.id }) _selected.value = null }
        if (pendingSelect.isNotBlank())
            _groups.value.firstOrNull { it.id == pendingSelect }?.let { pendingSelect = ""; select(it) }
    }

    fun select(g: ManagedGroup) {
        if (locked) return                    // 편집 중에는 선택이 바뀌지 않는다
        _selected.value = g
        loadDetail(g)
    }

    /**
     * ① 3줄 [로스터 전체] — 화면 밖에서 그룹 id 로 연다. 목록이 아직 없으면 도착한 뒤에 적용한다.
     *
     * 필터·검색을 **되돌린다.** 밖에서 지목한 그룹이 지금 필터에 걸리면 `project` 의 «행에 없으면 선택 해제»
     * 가 곧바로 선택을 지워, 눌렀는데 아무 일도 안 일어난 것처럼 보인다.
     */
    fun selectById(groupId: String) {
        if (groupId.isBlank() || locked) return
        if (_filter.value != GroupFilter.ALL || _query.value.isNotEmpty()) {
            _filter.value = GroupFilter.ALL
            _query.value = ""
            project()
        }
        val hit = _groups.value.firstOrNull { it.id == groupId }
        if (hit != null) { select(hit); return }
        pendingSelect = groupId
        load()
    }

    /** 상세 멤버 — GMS 문서를 받아 둔다. [edit] 과 달리 **폼을 열지 않는다**(보기 전용 행도 본다). */
    private fun loadDetail(g: ManagedGroup) {
        detailJob?.cancel()
        if (detailFor != g.id) { detailDoc = null; _detail.value = emptyList() }
        detailFor = g.id
        _detailBusy.value = true
        detailJob = scope.launch {
            val r = s.getGroupDoc(g.uri)
            if (detailFor != g.id) return@launch     // 그 사이 선택이 바뀌었다 — 남의 문서를 걸지 않는다
            _detailBusy.value = false
            detailDoc = if (r.ok) r.value else null
            projectDetail()
        }
    }

    private fun projectDetail() {
        val doc = detailDoc ?: return run { _detail.value = emptyList() }
        val live = s.groups.value.firstOrNull { it.id == detailFor }
        val speaker = s.sessions.value.firstOrNull { it.info.groupId == detailFor }?.speaker.orEmpty()
        _detail.value = detailMembers(
            members = doc.members, roster = live?.roster.orEmpty(), speaker = speaker,
            myPttId = s.myPttId, nameOf = { _book.value.nameOf(it) })
    }

    /** 상세 [채널로] — 관제 캔버스로 돌아가고, 멤버 그룹이면 합류한다(§4.7). */
    fun openChannel(g: ManagedGroup, onGoDispatch: () -> Unit) {
        if (g.isMember) scope.launch { s.joinGroup(g.id) }
        onGoDispatch()
    }

    // ── 편집 ──────────────────────────────────────────────────────────────────

    /** [+ 새 그룹] — 나를 의장으로 넣고 시작한다. */
    fun newGroup() {
        val uri = newGroupUri()
        val me = s.myPttId
        _form.value = EditForm(
            isNew = true, uri = uri, groupId = userPart(uri),
            // 관리 범위가 있으면 데스크 소속 조직에 귀속 — 같은 범위의 다른 관제사에게도 보인다(§4.5).
            orgCode = s.dispatch.orgCode,
            members = listOf(MemberRow(me, s.profileName(), userPart(me), isChair = true, isMe = true)),
            loaded = true)
    }

    /** [편집] — 문서를 받아 폼을 채운다. ETag 는 저장 때 If-Match 로 쓴다. */
    fun edit(g: ManagedGroup) = edit(g, cancelPrevious = true)

    /**
     * `cancelPrevious=false` 는 **저장 코루틴 안에서** 다시 열 때 쓴다(412 충돌).
     * 거기서 취소하면 자기 자신을 끊게 되고, 뒤따르는 문구 갱신이 사라진다.
     */
    private fun edit(g: ManagedGroup, cancelPrevious: Boolean) {
        if (cancelPrevious) formJob?.cancel()
        _form.value = EditForm(isNew = false, uri = g.uri, groupId = g.id, name = g.name, busy = true)
        formJob = scope.launch {
            val r = s.getGroupDoc(g.uri)
            if (!r.ok) {
                _form.value = _form.value?.copy(busy = false,
                    error = ResponseText.of(TextArea.GROUP, r.code, r.reason))
                return@launch
            }
            _form.value = formOf(r.value!!, g)
        }
    }

    private fun formOf(d: GroupDoc, g: ManagedGroup): EditForm = EditForm(
        isNew = false,
        uri = d.uri.ifBlank { g.uri },
        groupId = userPart(d.uri.ifBlank { g.uri }),
        name = d.displayName,
        sessionType = if (d.sessionType in SESSION_TYPES) d.sessionType else "prearranged",
        videoEnabled = d.videoEnabled, allowSds = d.allowSds, allowFd = d.allowFd,
        emergencyCall = d.emergencyCall, emergencyAlert = d.emergencyAlert,
        requireAffiliation = d.requireAffiliation, encryption = d.encryption,
        priority = d.priority, maxParticipants = d.maxParticipants, orgCode = d.orgCode,
        members = d.members.map { m ->
            val num = userPart(m.uri)
            MemberRow(m.uri, m.name.ifBlank { _book.value.nameOf(num) }, num,
                isChair = m.role == "chair", isMe = num == userPart(s.myPttId))
        },
        ifMatch = d.etag.ifBlank { g.etag },
        loaded = true)

    fun update(f: (EditForm) -> EditForm) { _form.value = _form.value?.let(f) }

    fun cancelEdit() { formJob?.cancel(); _form.value = null }

    /** 후보 → 멤버. 같은 번호가 이미 있으면 무시한다(정규형 비교). */
    fun addMember(number: String, name: String) {
        val f = _form.value ?: return
        val key = DirectoryBook.normalize(number)
        if (f.members.any { DirectoryBook.normalize(it.number) == key }) return
        val uri = telUri(number)
        _form.value = f.copy(members = f.members + MemberRow(uri, name.ifBlank { _book.value.nameOf(number) },
            userPart(uri), isMe = DirectoryBook.normalize(userPart(s.myPttId)) == key))
    }

    fun removeMember(uri: String) {
        _form.value = _form.value?.let { f -> f.copy(members = f.members.filterNot { it.uri == uri }) }
    }

    fun toggleChair(uri: String) {
        _form.value = _form.value?.let { f ->
            f.copy(members = f.members.map { if (it.uri == uri) it.copy(isChair = !it.isChair) else it })
        }
    }

    /**
     * 후보 목록 — 이미 멤버인 번호는 뺀다. 200건에서 끊는다(§4.7).
     *
     * **관측 가능해야 한다.** 함수로 두면 Compose 가 주소록·폼 변화를 추적하지 못해 검색어를 쳐도
     * 목록이 그대로거나, 주소록이 늦게 도착해도 비어 보인다.
     */
    val candidates: StateFlow<List<com.cims.ue.dispatch.session.DirectoryEntry>> =
        combine(_book, _form) { book, f -> candidatesOf(book, f) }
            .stateIn(scope, SharingStarted.Eagerly, emptyList())

    /** 저장 — 신규는 uri 충돌을 세션이 한 번 재시도한다. 412 면 최신 문서로 다시 연다. */
    fun save() {
        val f = _form.value ?: return
        if (!f.canSave) return
        _form.value = f.copy(busy = true, error = "")
        formJob = scope.launch {
            val doc = docOf(f)
            val r = s.saveGroup(doc, if (f.isNew) "" else f.ifMatch)
            if (!r.ok) {
                _form.value = _form.value?.copy(busy = false,
                    error = ResponseText.of(TextArea.GROUP, r.code, r.reason))
                if (r.code == 412 && !f.isNew) _selected.value?.let { edit(it, cancelPrevious = false) }
                return@launch
            }
            val savedUri = r.value!!.uri.ifBlank { f.uri }     // 재시도로 바뀔 수 있다 — 응답이 정본
            _form.value = null
            reload()                                            // 새 목록을 받은 **뒤에** 고른다
            s.refreshGroups()                                   // ① 카드도 새 그룹을 본다
            _selected.value = _groups.value.firstOrNull { it.uri == savedUri || it.id == userPart(savedUri) }
        }
    }

    internal fun docOf(f: EditForm): GroupDoc = GroupDoc(
        uri = if (f.isNew) telUri(f.groupId.trim()) else f.uri,
        displayName = f.name.trim(),
        members = f.members.map {
            GroupMember(it.uri, it.name, if (it.isChair) "chair" else "participant", if (it.isChair) 7 else 5)
        },
        sessionType = f.sessionType,
        videoEnabled = f.videoEnabled, encryption = f.encryption,
        emergencyCall = f.emergencyCall, emergencyAlert = f.emergencyAlert,
        allowSds = f.allowSds, allowFd = f.allowFd,
        requireAffiliation = f.requireAffiliation,
        priority = f.priority.coerceIn(0, 15),
        maxParticipants = f.maxParticipants.coerceAtLeast(0),
        orgCode = f.orgCode)

    /** 삭제 — 확인은 화면이 받는다. */
    fun delete(g: ManagedGroup) {
        scope.launch {
            val r = s.deleteGroup(g.uri)
            if (!r.ok) { _error.value = ResponseText.of(TextArea.GROUP, r.code, r.reason); return@launch }
            _selected.value = null
            reload()
            s.refreshGroups()
        }
    }

    companion object {
        val SESSION_TYPES = listOf("prearranged", "chat", "broadcast")

        /** 후보 계산 — 순수 함수(시험 대상). */
        internal fun candidatesOf(book: DirectoryBook, f: EditForm?):
            List<com.cims.ue.dispatch.session.DirectoryEntry> {
            if (f == null) return emptyList()
            val taken = f.members.map { DirectoryBook.normalize(it.number) }.toHashSet()
            val q = f.search.trim().lowercase()
            val qn = DirectoryBook.normalize(f.search.trim())
            return book.entries.asSequence()
                .filter { DirectoryBook.normalize(it.msisdn) !in taken }
                .filter {
                    q.isEmpty() || it.name.lowercase().contains(q) ||
                        (qn.isNotEmpty() && DirectoryBook.normalize(it.msisdn).contains(qn))
                }
                .take(200).toList()
        }    }
}

/** 표시 이름 — 프로파일에 없으면 PTT 번호. */
private fun DispatchSession.profileName(): String =
    profile.value?.displayName?.takeIf { it.isNotBlank() }
        ?: userPart(myPttId)
