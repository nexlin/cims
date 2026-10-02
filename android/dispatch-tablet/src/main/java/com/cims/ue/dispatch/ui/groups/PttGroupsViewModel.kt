// [PTT 그룹] 화면 상태 (docs/design/features/android_dispatch_tablet.md §6.12, dispatch_desktop_ui.md §4.7)
//
// 목록 원천은 관리 API(`/provisioning/directory/groups`) — **관리 범위 ∪ 내 소유 ∪ 청취 범위 ∪ 내 멤버**라
// 보기 전용 행이 섞인다. [편집]·[삭제]는 `canManage` 인 행에만 붙고, 실제 판정은 서버 GMS 게이트가 한다.
// 생성·편집·삭제는 XCAP PUT/DELETE(TS 24.481). 폼의 값과 문서로 되돌리는 규칙은 GroupForm.kt 에 있다.
package com.cims.ue.dispatch.ui.groups

import com.cims.ue.dispatch.session.userPart
import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.ManagedGroup
import com.cims.ue.dispatch.session.NoticeLevel
import com.cims.ue.dispatch.session.ResponseText
import com.cims.ue.dispatch.session.TextArea
import com.cims.ue.dispatch.session.deleteGroup
import com.cims.ue.dispatch.session.getGroupDoc
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

class PttGroupsViewModel(private val s: DispatchSession) : ScreenViewModel() {

    private val _groups = MutableStateFlow<List<ManagedGroup>>(emptyList())
    /**
     * 관리 목록 전부(필터 전) — 서버가 행마다 `canManage` 를 준다. [채널] 화면 머리의 [편집]·[삭제] 자격이 이 값을 본다
     * (데스크톱 범위 채널 `CanEdit = IsManageScope || IsOwner` — 소유만 보면 관리 범위의 그룹을 못 고친다).
     */
    val groups: StateFlow<List<ManagedGroup>> = _groups.asStateFlow()

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

    private val _hint = MutableStateFlow("")
    /** 목록 범위 안내 — 상세 카드 바닥의 한 줄([groupListHint]). */
    val hint: StateFlow<String> = _hint.asStateFlow()

    // ── 상세(선택 그룹) — 문서 GET(소유자·우선순위·긴급·능력·멤버) + 세션 로스터(참여·발언) ──
    private val _detail = MutableStateFlow<List<DetailMember>>(emptyList())
    val detail: StateFlow<List<DetailMember>> = _detail.asStateFlow()
    /** [detail] 이 어느 그룹의 것인가 — 채널 상세가 제 그룹의 명단인지 확인한다(폼이 잠겨 있으면 선택이 바뀌지 않는다). */
    val detailGroupId: String get() = detailFor

    private val _detailBusy = MutableStateFlow(false)
    val detailBusy: StateFlow<Boolean> = _detailBusy.asStateFlow()

    /**
     * 선택된 그룹의 GMS 문서 — 정보 칸·능력 칩·멤버 구성의 원천. 라이브 상태가 바뀔 때마다 이것과 겹쳐 다시 그린다.
     * 받기 전·못 받았으면 null(정보 칸은 «…»).
     */
    private val _detailDoc = MutableStateFlow<GroupDoc?>(null)
    val detailDoc: StateFlow<GroupDoc?> = _detailDoc.asStateFlow()

    private val _detailError = MutableStateFlow("")
    val detailError: StateFlow<String> = _detailError.asStateFlow()

    private val _detailLive = MutableStateFlow(false)
    /** 선택된 그룹에 세션이 돌고 있다(로스터로 안다 — 참여하지 않아도) — 머리 라벨 «세션 진행 중»·삭제 확인 문구. */
    val detailLive: StateFlow<Boolean> = _detailLive.asStateFlow()

    private val _videoIds = MutableStateFlow<Set<String>>(emptySet())
    /**
     * MCVideo 그룹으로 **알려진** 그룹 id — 서비스 칩 «영상». 관리 목록에는 서비스가 없어 내 영상 채널(세션의
     * `GroupInfo.mcVideo` — MCVideo user profile)과 열어 본 그룹 문서로만 안다(dispatch_desktop_ui.md §10.6).
     */
    val videoIds: StateFlow<Set<String>> = _videoIds.asStateFlow()

    /** 문서로 확인한 MCVideo 그룹 — 목록을 다시 받아도 «영상» 칩을 잇는다. */
    private val videoKnown = HashSet<String>()

    private var detailFor: String = ""
    private var detailJob: Job? = null

    /** 목록이 오기 전에 들어온 선택 요청(① 3줄 [로스터 전체]). */
    private var pendingSelect: String = ""
    /** 목록이 오기 전에 들어온 편집 요청([채널] 머리 [편집]) — 선택이 서면 그 그룹의 폼을 연다. */
    private var pendingEdit: String = ""

    private var loadJob: Job? = null
    private var formJob: Job? = null

    init {
        // 구성은 문서가 주지만 **상태는 로스터가** 준다 — 로스터·발언자가 바뀌면 같은 문서로 다시 겹친다.
        //   문서를 다시 받지 않는다(구성은 XCAP 변경 통지로만 바뀐다).
        scope.launch {
            s.groups.collect {
                // 관리 범위가 없으면 목록의 원천이 세션(GMS 목록)이다 — 그룹이 생기고 사라지는 대로 따라간다.
                if (!s.dispatch.canAdminDirectory) fromSession()
                projectDetail(); projectVideo()
            }
        }
        scope.launch { s.sessions.collect { projectDetail() } }
        // 멤버 후보·이름·조직 경로 — 세션이 받아 둔 PTT 주소록을 따라간다(늦게 도착해도 후보가 선다).
        scope.launch { s.pttBook.collect { if (it.entries.isNotEmpty()) _book.value = it } }
    }

    /** 편집 중에는 목록·[↻]·[+ 새 그룹]이 잠긴다 — 편집 대상이 바뀌지 않게(§4.7). */
    val locked: Boolean get() = _form.value != null
    /** 열려 있는 폼이 **새 그룹**의 것인가 — [채널 추가] › [그룹 추가 ›] 가 이어 쓰고, 그 흐름을 벗어나면 닫는다. */
    val editingNew: Boolean get() = _form.value?.isNew == true
    /** 저장 요청이 나가 있다 — 그동안의 [취소] 는 받지 않는다(요청은 이미 서버에 닿았을 수 있다). */
    private var saving = false

    /**
     * [+ 새 그룹] 을 둘 것인가 — 그룹 생성 자격(`ptt.allowCreateGroup`)이나 관리 범위가 있고 PTT 회선이 섰을 때
     * (데스크톱 `CanCreateGroups`). 최종 판정은 서버(GMS)다 — 누를 수 없는 버튼을 세우지 않을 뿐이다.
     */
    val canCreate: Boolean get() =
        (s.profile.value?.allowGroupCreation == true || s.dispatch.canAdminDirectory) && s.pttAccount != null

    /** 이 관제석의 청취가 그룹 로스터에 보이지 않는가(관제 역할의 `listen_visibility`) — 정보 칸 «청취 노출». */
    val listenHidden: Boolean get() = s.listenHidden

    val myPttId: String get() = s.myPttId
    val myName: String get() = s.profileName()

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
        _error.value = ""
        if (!s.dispatch.canAdminDirectory) {
            fromSession()
        } else {
            _loading.value = true
            val r = m.listGroups()
            _loading.value = false
            if (!r.ok) { _error.value = r.reason; return }
            _groups.value = r.value.orEmpty()
            s.noteGroupTypes(_groups.value.associate { it.id to it.sessionType })   // 채널 카드의 [일제 통화] 판정이 쓴다
            _hint.value = groupListHint(_groups.value)
            project()
        }
        // 멤버 후보용 PTT 주소록 — 세션이 로그인 때 이미 받아 뒀다(같은 것을 두 번 받지 않는다).
        // 비어 있으면 그때 한 번 더 시도한다.
        if (s.pttBook.value.entries.isNotEmpty()) _book.value = s.pttBook.value
        if (_book.value.entries.isEmpty())
            m.directory("ptt").let { d -> if (d.ok) d.value?.let { _book.value = it } }
    }

    /**
     * 관리 범위가 없는 관제사의 목록 — 관리 목록 API 는 403 `no_directory_admin` 이라 **GMS 목록의 내 멤버 그룹**을 보인다
     * (데스크톱 `GroupAdminViewModel.FromSession`). 편집·삭제는 내 소유만이다 — 그룹 생성 자격만 받은 사람도 제가 만든
     * 그룹은 여기서 고친다.
     */
    private fun fromSession() {
        _groups.value = managedOf(s.groups.value)
        _hint.value = if (_groups.value.isEmpty()) "" else "관리 범위가 없어 내 멤버 그룹만 보입니다 — 편집·삭제는 내 소유만"
        project()
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
        if (pendingEdit.isNotBlank())
            _groups.value.firstOrNull { it.id == pendingEdit }?.let { pendingEdit = ""; if (it.canManage) edit(it) }
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
        // 그 그룹이 지금 목록에 **안 보일 때만** 되돌린다 — 채널 상세를 열 때마다([채널] 화면이 편성 명단을 여기서 빌린다)
        //   [PTT 그룹] 화면의 필터·검색을 지우지 않게.
        if (_rows.value.none { it.id == groupId } && (_filter.value != GroupFilter.ALL || _query.value.isNotEmpty())) {
            _filter.value = GroupFilter.ALL
            _query.value = ""
            project()
        }
        val hit = _groups.value.firstOrNull { it.id == groupId }
        if (hit != null) { select(hit); return }
        pendingSelect = groupId
        load()
    }

    /**
     * 관리 목록이 아직 없으면 받는다 — [채널] 화면이 [편집] 자격을 물을 때(이 화면을 열기 전이면 목록이 비어 있다).
     * 이미 받는 중이면 겹치지 않는다.
     */
    fun ensureLoaded() {
        if (_groups.value.isEmpty() && loadJob?.isActive != true) load()
    }

    /**
     * 화면 밖에서 그룹 id 로 **편집 폼**을 연다([채널] 머리 [편집] — 데스크톱 `OpenDrawerEdit`). 선택을 옮기고, 관리할 수
     * 있는 행이면 폼을 연다. 편집 중인 폼이 있으면 그 폼을 지킨다 — 말없이 덮어쓰면 고치던 것이 사라진다.
     */
    fun editById(groupId: String) {
        if (groupId.isBlank() || locked) return
        selectById(groupId)
        val hit = _groups.value.firstOrNull { it.id == groupId }
        if (hit != null) { if (hit.canManage) edit(hit) } else pendingEdit = groupId
    }

    /** 화면 밖에서 그룹 id 로 삭제한다(확인은 부른 화면이 받았다). 목록에 없거나 관리할 수 없으면 하지 않는다. */
    fun deleteById(groupId: String) {
        _groups.value.firstOrNull { it.id == groupId && it.canManage }?.let { return delete(it) }
        // 관리 목록에 없다(목록을 못 받았거나 아직 안 받았다) — 채널 상세의 ⋮ 는 세션의 소유 판정으로도 서므로 그 그룹으로 지운다.
        //   아무 일도 하지 않으면 «삭제» 를 눌렀는데 패널만 닫힌다.
        val g = s.groups.value.firstOrNull { it.id == groupId }
        if (g == null) s.notify(NoticeLevel.WARN, "그룹을 찾을 수 없습니다", groupId)
        else deleteGroup(g.id, g.uri, g.name)
    }

    /** 상세 — GMS 문서를 받아 둔다. [edit] 과 달리 **폼을 열지 않는다**(보기 전용 행도 본다). */
    private fun loadDetail(g: ManagedGroup) {
        detailJob?.cancel()
        if (detailFor != g.id) { _detailDoc.value = null; _detail.value = emptyList() }
        detailFor = g.id
        _detailError.value = ""
        _detailBusy.value = true
        projectDetail()                               // 세션 진행 여부는 문서 없이도 안다
        detailJob = scope.launch {
            val r = s.getGroupDoc(g.uri)
            if (detailFor != g.id) return@launch     // 그 사이 선택이 바뀌었다 — 남의 문서를 걸지 않는다
            _detailBusy.value = false
            _detailDoc.value = if (r.ok) r.value else null
            if (r.ok) noteVideo(g.id, r.value?.mcvideo != null)
            else _detailError.value = ResponseText.of(TextArea.GROUP, r.code, r.reason)
            projectDetail()
        }
    }

    private fun projectDetail() {
        val live = s.groups.value.firstOrNull { it.id == detailFor }
        _detailLive.value = detailFor.isNotEmpty() && live?.hasSession == true
        val doc = _detailDoc.value ?: return run { _detail.value = emptyList() }
        val speaker = s.sessions.value.firstOrNull { it.info.groupId == detailFor }?.speaker.orEmpty()
        _detail.value = detailMembers(
            members = doc.members, roster = live?.roster.orEmpty(), speaker = speaker,
            myPttId = s.myPttId, nameOf = { _book.value.nameOf(it) })
    }

    /** 문서가 알려 준 서비스 — MCVideo 몫이 있으면 «영상» 으로 기억하고, 없으면(콘솔에서 껐다) 잊는다. */
    private fun noteVideo(groupId: String, video: Boolean) {
        if (video) videoKnown.add(groupId) else videoKnown.remove(groupId)
        projectVideo()
    }

    private fun projectVideo() {
        _videoIds.value = s.groups.value.filter { it.mcVideo }.mapTo(HashSet()) { it.id } + videoKnown
    }

    /**
     * 상세 [채널로] — [관제] › [무전] 으로 돌아가 그 그룹의 **채널 패널**을 연다(데스크톱 §4.7 `GoToChannel`).
     * 멤버 그룹은 내 채널 카드, 청취 범위 그룹은 타 채널 행이다. **합류하지 않는다** — 참여는 그 채널의 [참여] 가 한다.
     *
     * 여는 길은 목록 행·긴급 배너와 같은 [onOpen](`MainViewModel.openChannel` — «채널» 면으로)이다. 채널이 있는 그룹만
     * 부른다(`hasChannel`) — 관리 범위만 있는 그룹은 태블릿에 채널이 없어 열면 «사라졌습니다» 가 된다.
     */
    fun openChannel(g: ManagedGroup, onOpen: (String) -> Unit) {
        if (g.hasChannel) onOpen(g.id)
    }

    // ── 편집 ──────────────────────────────────────────────────────────────────

    /** [+ 새 그룹] — 나를 의장으로 넣고 시작한다. 편집 중인 폼이 있으면 그 폼을 지킨다. */
    fun newGroup() {
        if (locked) return
        val uri = newGroupUri()
        val me = s.myPttId
        _form.value = EditForm(
            isNew = true, uri = uri, groupId = userPart(uri),
            // 관리 범위가 있으면 데스크 소속 조직에 귀속 — 같은 범위의 다른 관제사에게도 보인다(§4.5).
            orgCode = if (s.dispatch.canAdminDirectory) s.dispatch.orgCode else "",
            members = listOf(MemberRow(me, s.profileName(), userPart(me), isChair = true, isMe = true)),
            loaded = true)
    }

    /** [편집] — 문서를 받아 폼을 채운다. ETag 는 저장 때 If-Match 로 쓴다. */
    fun edit(g: ManagedGroup) = edit(g, cancelPrevious = true)

    /**
     * `cancelPrevious=false` 는 **저장 코루틴 안에서** 다시 열 때 쓴다(412 충돌).
     * 거기서 취소하면 자기 자신을 끊게 되고, 뒤따르는 문구 갱신이 사라진다.
     */
    private fun edit(g: ManagedGroup, cancelPrevious: Boolean, note: String = "") {
        if (cancelPrevious) formJob?.cancel()
        _form.value = EditForm(isNew = false, uri = g.uri, groupId = g.id, name = g.name, busy = true, error = note)
        // 새 조회가 폼의 일이 된다 — [취소] 가 이것을 끊어야 늦게 온 문서가 닫은 폼을 다시 열지 않는다.
        formJob = scope.launch {
            val r = s.getGroupDoc(g.uri)
            if (!r.ok) {
                _form.value = _form.value?.copy(busy = false,
                    error = ResponseText.of(TextArea.GROUP, r.code, r.reason))
                return@launch
            }
            _form.value = editFormOf(r.value!!, g, userPart(s.myPttId), _book.value::nameOf).copy(error = note)
        }
    }

    fun update(f: (EditForm) -> EditForm) { _form.value = _form.value?.let(f) }

    /**
     * 폼을 닫는다. **저장 중에는 닫지 않는다** — 코루틴만 끊기고 PUT 은 서버에서 끝나, 그룹은 만들어지거나 바뀌었는데 알림도
     * 목록 갱신도 없이 폼만 사라진다. 저장이 끝나면 폼이 스스로 닫힌다(실패면 사유와 함께 남는다).
     */
    fun cancelEdit() {
        // 저장 중 — 끝나면 닫는다(성공이면 어차피 닫히고, 실패면 사유를 토스트로 알리고 닫는다. 보이지 않는 곳에 오류와 함께
        //   남으면 잠금을 쥔 채 아무도 닫지 못한다).
        if (saving) { cancelAfterSave = true; return }
        formJob?.cancel(); _form.value = null
    }
    private var cancelAfterSave = false

    /** 후보 → 멤버. 같은 번호가 이미 있으면 무시한다(정규형 비교). */
    fun addMember(number: String, name: String) {
        val f = _form.value ?: return
        _form.value = withEntries(f, listOf(DirectoryEntry("", name, number)), userPart(s.myPttId), _book.value::nameOf)
    }

    /**
     * [표시된 전원 추가] — 지금 보이는 후보 전부를 한 번에(데스크톱 `AddAllShown`). 검색으로 좁힌 다음 누르는 조작이라
     * **보이는 것만** 넣는다(후보 상한 200 안) — 주소록 전부가 아니다.
     */
    fun addAllShown() {
        val f = _form.value ?: return
        _form.value = withEntries(f, candidates.value, userPart(s.myPttId), _book.value::nameOf)
    }

    fun removeMember(uri: String) {
        _form.value = _form.value?.let { f -> f.copy(members = f.members.filterNot { it.uri == uri }) }
    }

    /** 의장 ↔ 참가자. 역할을 바꾼 멤버는 저장 때 역할 기본 우선순위를 싣는다([MemberRow.priority]). */
    fun toggleChair(uri: String) {
        _form.value = _form.value?.let { f ->
            f.copy(members = f.members.map { if (it.uri == uri) it.copy(isChair = !it.isChair) else it })
        }
    }

    /** 필수 ↔ 선택(`<on-network-required>`). */
    fun toggleRequired(uri: String) {
        _form.value = _form.value?.let { f ->
            f.copy(members = f.members.map { if (it.uri == uri) it.copy(required = !it.required) else it })
        }
    }

    /**
     * 후보 목록 — 이미 멤버인 번호는 뺀다. 200건에서 끊는다(§4.7).
     *
     * **관측 가능해야 한다.** 함수로 두면 Compose 가 주소록·폼 변화를 추적하지 못해 검색어를 쳐도
     * 목록이 그대로거나, 주소록이 늦게 도착해도 비어 보인다.
     */
    val candidates: StateFlow<List<DirectoryEntry>> =
        combine(_book, _form) { book, f -> candidatesOf(book, f) }
            .stateIn(scope, SharingStarted.Eagerly, emptyList())

    /** 저장 — 신규는 uri 충돌을 세션이 한 번 재시도한다. 412 면 최신 문서로 다시 연다. */
    fun save() {
        val f = _form.value ?: return
        if (!f.canSave) return
        _form.value = f.copy(busy = true, error = "")
        saving = true
        cancelAfterSave = false
        formJob = scope.launch {
            val r = try { s.saveGroup(groupDocOf(f), if (f.isNew) "" else f.ifMatch) } finally { saving = false }
            val leave = cancelAfterSave
            cancelAfterSave = false
            if (!r.ok) {
                val why = ResponseText.of(TextArea.GROUP, r.code, r.reason)
                if (leave) {
                    _form.value = null
                    s.notify(NoticeLevel.ERROR, "PTT 그룹 «${f.name.trim()}» 저장 실패 — $why", "${r.code} ${r.reason}".trim())
                    return@launch
                }
                _form.value = _form.value?.copy(busy = false, error = why)
                // 다른 곳에서 먼저 바뀌었다 — 최신 문서로 다시 연다. 사유는 새 폼에도 남긴다(말없이 값이 바뀌면 안 된다).
                if (r.code == 412 && !f.isNew)
                    _groups.value.firstOrNull { it.uri == f.uri || it.id == f.groupId }
                        ?.let { edit(it, cancelPrevious = false, note = why) }
                return@launch
            }
            val savedUri = r.value!!.uri.ifBlank { f.uri }     // 재시도로 바뀔 수 있다 — 응답이 정본
            _form.value = null
            // 폼이 닫히는 것이 곧 성공이지만, 패널에서 만든 그룹은 닫힌 뒤 어디에 섰는지 한 줄로 알린다.
            if (f.isNew) s.notify(NoticeLevel.INFO, "PTT 그룹 «${f.name.trim()}» 을(를) 만들었습니다")
            else s.notify(NoticeLevel.INFO, "그룹 편집 완료", f.name.trim())
            s.refreshGroups()                                   // ① 카드도 새 그룹을 본다(관리 범위가 없으면 목록의 원천이기도 하다)
            reload()                                            // 새 목록을 받은 **뒤에** 고른다
            // 그 그룹의 상세로 돌아온다 — 문서를 다시 받아 방금 고친 값이 정보 칸·능력 칩에 선다.
            _groups.value.firstOrNull { it.uri == savedUri || it.id == userPart(savedUri) }?.let { hit ->
                if (_rows.value.any { it.id == hit.id }) { _selected.value = hit; loadDetail(hit) }
            }
        }
    }

    /** 삭제 — 확인은 화면이 받는다. */
    fun delete(g: ManagedGroup) = deleteGroup(g.id, g.uri, g.name)

    /**
     * 결과는 토스트로도 알린다 — 채널 상세의 ⋮ 에서 지울 때는 이 화면의 목록 머리(오류 띠)가 보이지 않는다(데스크톱
     * `DeleteGroup` 도 실패·«그룹 삭제 완료» 를 토스트로 낸다). 실패해도 목록은 다시 받는다 — 이미 지워진 그룹(404)이면
     * 그 줄이 남아 있지 않게.
     */
    private fun deleteGroup(id: String, uri: String, name: String) {
        // 세션 수명에서 돈다 — 채널 상세에서 지우면 패널이 곧바로 닫히고, 화면이 닫혀도 결과를 알려야 한다
        s.scopeLaunch {
            val r = s.deleteGroup(uri)
            if (!r.ok) {
                val why = ResponseText.of(TextArea.GROUP, r.code, r.reason)
                _error.value = why
                s.notify(NoticeLevel.ERROR, "${name.ifBlank { id }} — 그룹 삭제 실패", why)
                reload()
                return@scopeLaunch
            }
            if (_selected.value?.id == id) _selected.value = null
            videoKnown.remove(id)
            s.notify(NoticeLevel.INFO, "그룹 삭제 완료", name.ifBlank { id })
            s.refreshGroups()
            reload()
        }
    }

    companion object {
        /** 그룹 종류(TS 24.481 `<on-network-invite-members>` — true = 편성, false = 채팅). 일제 통화는 그룹 종류가 아니라
         *  호 속성이라(TS 24.379 §4.12 — 편성 그룹에서 통화마다 broadcast-ind) 없다. 일제 통화용 그룹은 그룹 이름으로 알린다
         *  (mcptt_broadcast_group_call.md §5). */
        val SESSION_TYPES = listOf("prearranged", "chat")

        /**
         * 폼에 멤버를 더한다 — 이미 있는 번호·같은 묶음 안의 중복은 건너뛴다(정규형 비교). 의장이 아니라 참가자로 든다.
         * 순수 함수(시험 대상).
         */
        internal fun withEntries(f: EditForm, entries: List<DirectoryEntry>,
                                 myPttNumber: String, nameOf: (String) -> String): EditForm {
            val taken = f.members.map { DirectoryBook.normalize(it.number) }.toHashSet()
            val me = DirectoryBook.normalize(myPttNumber)
            val add = entries.filter { taken.add(DirectoryBook.normalize(it.msisdn)) }.map { e ->
                val uri = telUri(e.msisdn)
                MemberRow(uri, e.name.ifBlank { nameOf(e.msisdn) }, userPart(uri),
                    isMe = DirectoryBook.normalize(e.msisdn) == me)
            }
            return if (add.isEmpty()) f else f.copy(members = f.members + add)
        }

        /**
         * 세션의 그룹(GMS 목록) → 관리 목록 행. 멤버 그룹만 든다 — 청취 범위 그룹은 관리 범위가 있을 때만 서버가 준다.
         * 고칠 수 있는 것은 내 소유뿐이다(GMS 게이트와 같은 판정). 순수 함수(시험 대상).
         */
        internal fun managedOf(groups: List<com.cims.ue.dispatch.session.GroupInfo>): List<ManagedGroup> =
            groups.filter { it.isMember }.map { g ->
                ManagedGroup(id = g.id, uri = g.uri, name = g.name, memberCount = g.memberCount, isOwner = g.isOwner,
                    sessionType = g.sessionType, etag = g.etag, canManage = g.isOwner, isMember = true)
            }

        /** 후보 계산 — 순수 함수(시험 대상). */
        internal fun candidatesOf(book: DirectoryBook, f: EditForm?): List<DirectoryEntry> {
            if (f == null) return emptyList()
            val taken = f.members.map { DirectoryBook.normalize(it.number) }.toHashSet()
            val q = f.search.trim().lowercase()
            // 번호는 표기 셋(저장된 그대로 · E.164 · 국내 표기)으로 맞춘다 — 정규형끼리만 비교하면 짧은 국내 표기(`010`, `0103`)가
            //   `+8210…` 에 걸리지 않는다.
            val digits = f.search.trim().filter { it.isDigit() || it == '+' }
            return book.entries.asSequence()
                .filter { DirectoryBook.normalize(it.msisdn) !in taken }
                .filter {
                    q.isEmpty() || it.name.lowercase().contains(q) ||
                        (digits.isNotEmpty() && DirectoryBook.numberForms(it.msisdn).any { form -> form.contains(digits) })
                }
                .take(200).toList()
        }
    }
}

/** 표시 이름 — 프로파일에 없으면 PTT 번호. */
private fun DispatchSession.profileName(): String =
    profile.value?.displayName?.takeIf { it.isNotBlank() }
        ?: userPart(myPttId)
