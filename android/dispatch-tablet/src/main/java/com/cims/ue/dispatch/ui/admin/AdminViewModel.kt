// [관리] 화면 상태 (docs/design/features/android_dispatch_tablet.md §6.13, dispatch_desktop_ui.md §4.5)
//
// 조직·구성원·VoLTE/VoIP/PTT 번호. **앱은 범위 enum 을 해석하지 않는다** — 서버가 걸러 준 조직·구성원만
// 보이고 쓰기 판정도 서버가 한다. 앱이 하는 판단은 하나뿐이다: *무엇이 바뀌었나*. 바뀐 회선만 PUT 하고,
// 바뀐 것이 없으면 보내지 않는다(서버가 H(A1) 재결박을 요구해 저장마다 400 이 나기 때문).
package com.cims.ue.dispatch.ui.admin

import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.AdminView
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.LineKind
import com.cims.ue.dispatch.session.MemberInfo
import com.cims.ue.dispatch.session.NoticeLevel
import com.cims.ue.dispatch.session.NumberInfo
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.session.ServiceRef
import com.cims.ue.dispatch.session.flattenOrgs
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** 회선 폼 한 장 — 카드 하나에 대응. 비밀번호는 저장 뒤 버린다(서버가 H(A1) 로만 보관). */
data class LineForm(
    val msisdn: String = "",
    val serviceRef: String = "",
    val sipTransport: String = "",
    val password: String = "",
) {
    companion object {
        fun of(n: NumberInfo?): LineForm =
            if (n == null) LineForm()
            else LineForm(n.msisdn, n.serviceRef, n.sipTransport)
    }
}

/** 구성원 편집 폼. `orig` 가 null 이면 새 구성원. */
data class MemberForm(
    val orig: MemberInfo? = null,
    val name: String = "",
    val title: String = "",
    val org: String = "",
    val loginId: String = "",
    val password: String = "",
    val lines: Map<String, LineForm> = LineKind.all.associateWith { LineForm() },
    val allowCreateGroup: Boolean = false,
    val busy: Boolean = false,
    val error: String = "",
) {
    val isNew: Boolean get() = orig == null

    /** 폼 머리의 이름 — 입력을 따라간다. 비면 «새 구성원»/«#id». */
    val formName: String get() = name.trim().ifEmpty { if (isNew) "새 구성원" else "#${orig!!.userId}" }
    val formSub: String get() = if (isNew) "새 구성원 등록" else "구성원 편집"

    /**
     * 열 때와 달라졌는가 — 클릭만으로 폼이 열리므로 "열림 ≠ 변경" 이다(§4.5).
     *
     * 없는 회선은 **번호를 넣었을 때만** 변경이다. 새 회선 카드는 접속서비스 첫 후보·TLS 를 미리 골라 두는데
     * ([AdminViewModel.openLine]), 번호 없이는 아무것도 보내지 않으므로 그 기본값은 변경이 아니다.
     */
    val dirty: Boolean get() {
        if (isNew) return name.isNotBlank() || lines.values.any { it.msisdn.isNotBlank() }
        val o = orig!!
        if (name != o.name || title != o.title || org != o.org || loginId != o.loginId) return true
        if (password.isNotBlank()) return true
        if (allowCreateGroup != o.allowCreateGroup) return true
        return LineKind.all.any { k ->
            val line = lines[k] ?: LineForm()
            val saved = o.line(k)
            if (saved == null || saved.isEmpty) line.msisdn.isNotBlank() else line != LineForm.of(saved)
        }
    }

    /** 저장 선검사 — 이름과 소속 조직은 필수다(데스크톱 `DirectoryAdminViewModel` 과 같다. 범위가 조직으로 정해져 조직 없는 구성원은 관리 밖에 선다). */
    val canSave: Boolean get() = !busy && name.isNotBlank() && org.isNotBlank()
    /** 저장이 막힌 까닭 — 고친 것이 있는데 필수 칸이 비었을 때만. */
    val missing: String get() = if (dirty && (name.isBlank() || org.isBlank())) "이름과 소속 조직은 필수입니다" else ""
}

/** 회선 하나에 대해 저장이 할 일. */
sealed interface LineAction {
    data object None : LineAction
    data object Delete : LineAction
    data class Put(val number: NumberInfo, val password: String) : LineAction
}

class AdminViewModel(private val s: DispatchSession) : ScreenViewModel() {

    private val _view = MutableStateFlow(AdminView())
    val view: StateFlow<AdminView> = _view.asStateFlow()

    private val _org = MutableStateFlow("")            // 선택 조직(빈 값 = 전체)
    val org: StateFlow<String> = _org.asStateFlow()

    private val _query = MutableStateFlow("")
    val query: StateFlow<String> = _query.asStateFlow()

    private val _members = MutableStateFlow<List<MemberInfo>>(emptyList())
    val members: StateFlow<List<MemberInfo>> = _members.asStateFlow()

    private val _form = MutableStateFlow<MemberForm?>(null)
    val form: StateFlow<MemberForm?> = _form.asStateFlow()

    /**
     * 조직 편집 폼 — null 이면 닫혀 있다. 열려 있으면 편집 폼 칸에 구성원 폼 **대신** 선다. 구성원 폼은 버리지 않는다 —
     * 조직 폼이 닫히면 고치던 구성원 폼이 그대로 돌아온다.
     */
    private val _orgForm = MutableStateFlow<OrgNode?>(null)
    val orgForm: StateFlow<OrgNode?> = _orgForm.asStateFlow()
    private val _orgFormIsNew = MutableStateFlow(true)
    val orgFormIsNew: StateFlow<Boolean> = _orgFormIsNew.asStateFlow()
    /** 조직 폼을 열 때의 값 — 달라졌는지([dirty])의 기준. */
    private var orgFormBase: OrgNode? = null

    private val _loading = MutableStateFlow(false)
    val loading: StateFlow<Boolean> = _loading.asStateFlow()

    private val _error = MutableStateFlow("")
    val error: StateFlow<String> = _error.asStateFlow()

    private var etag = ""
    private var job: Job? = null

    /** 관리 범위가 없으면 화면이 잠긴다 — 메뉴는 보이되 비활성이다(§3.4). */
    val available: Boolean get() = s.dispatch.canAdminDirectory

    /** 저장하지 않은 변경 — [관리] 메뉴 점 배지와 구성원 머리 라벨의 근거. 구성원 폼·조직 폼 어느 쪽이든. */
    val dirty: Boolean get() =
        _form.value?.dirty == true || (_orgForm.value != null && _orgForm.value != orgFormBase)

    /**
     * [dirty] 의 관측 가능한 꼴 — 레일 [관리] 의 점 배지가 폼을 고치는 **그 순간** 바뀐다(게터만 두면 다른 화면으로 옮길 때에야
     * 맞춰진다). 폼이 바뀔 때마다 다시 판정한다.
     */
    val dirtyFlow: StateFlow<Boolean> =
        kotlinx.coroutines.flow.combine(_form, _orgForm) { _, _ -> dirty }
            .stateIn(scope, kotlinx.coroutines.flow.SharingStarted.Eagerly, false)

    // ── 적재 ──────────────────────────────────────────────────────────────────

    private var loadJob: Job? = null

    /**
     * 조회 — **쓰는 중에는 하지 않는다.** 저장·삭제는 여러 요청이 이어지는 한 묶음(구성원 → 회선들 → PTT 자격)이라 중간에
     * 끊으면 반쪽만 저장되고 폼이 «저장 중…» 에 멈춘다. 저장이 끝나면 그쪽이 스스로 다시 받는다.
     */
    fun load(force: Boolean = false) {
        if (job?.isActive == true) return
        loadJob?.cancel()
        loadJob = scope.launch { reload(force) }
    }

    /**
     * 한 벌 재조회. **저장·삭제 코루틴 안에서는 이걸 부른다** — `load()` 는 `job` 을 취소하므로
     * 자기 자신을 끊게 된다(지금은 뒤에 남은 일이 없어 드러나지 않지만, 한 줄만 늘어도 사라진다).
     */
    private suspend fun reload(force: Boolean) {
        val m = s.management() ?: return run { _error.value = "로그인 전" }
        _loading.value = true
        _error.value = ""
        val r = m.adminView(if (force) "" else etag)
        _loading.value = false
        if (!r.ok) { _error.value = r.reason; return }
        r.value?.let { v -> _view.value = v; etag = v.etag }   // null = 304, 내용 유지
        project()
    }

    /** 조직·구성원·번호가 바뀌었다 — 주소록(전화번호부)도 따라오게 다시 받는다. 화면의 재조회를 기다리게 하지 않는다. */
    private fun syncDirectory() { scope.launch { s.refreshDirectory() } }

    /** 조직 선택 = 하위 포함 필터. 같은 조직을 다시 누르거나 빈 값(«전체»)이면 필터를 푼다. */
    fun selectOrg(code: String) { _org.value = if (_org.value == code) "" else code; project() }
    fun search(q: String) { _query.value = q; project() }

    private fun project() {
        val v = _view.value
        val subtree = v.orgSubtree(_org.value)
        val q = _query.value
        _members.value = v.members.filter { m -> (subtree == null || m.org in subtree) && memberMatches(m, q) }
    }

    // ── 구성원 폼 ─────────────────────────────────────────────────────────────

    /** 행 클릭 = 바로 편집. 저장하지 않은 변경이 있으면 화면이 먼저 확인을 받는다(§4.5). */
    fun open(m: MemberInfo) {
        if (saving) return
        closeOrgForm()
        _error.value = ""
        _form.value = MemberForm(
            orig = m, name = m.name, title = m.title, org = m.org, loginId = m.loginId,
            lines = LineKind.all.associateWith { openLine(_view.value, it, m.line(it)) },
            allowCreateGroup = m.allowCreateGroup)
    }

    /** [+ 새 구성원] — 고른 조직(없으면 트리의 첫 조직)에 둔다. */
    fun newMember() {
        if (saving) return
        closeOrgForm()
        _error.value = ""
        val v = _view.value
        _form.value = MemberForm(
            org = _org.value.ifBlank { flattenOrgs(v.orgs).firstOrNull()?.first?.code.orEmpty() },
            lines = LineKind.all.associateWith { openLine(v, it, null) })
    }

    /**
     * 저장이 도는 중인가 — 그동안은 **폼을 바꾸지도 닫지도 다른 사람으로 갈지도 않는다.** 저장은 여러 요청이 이어지는 한 묶음이고
     * 단계마다 지금 폼의 기준(`orig`)을 읽는다 — 그사이 폼이 다른 사람의 것으로 바뀌면 남은 회선 요청이 그 사람의 회선을 기준으로
     * 계산돼 앞 사람에게 나가고, 닫히면 바뀌지 않은 회선이 새 회선으로 다시 나간다.
     */
    private val saving: Boolean get() = _form.value?.busy == true

    fun closeForm() { if (saving) return; _form.value = null; _error.value = "" }

    fun update(f: (MemberForm) -> MemberForm) { if (saving) return; _form.value = _form.value?.let(f) }

    fun updateLine(kind: String, f: (LineForm) -> LineForm) {
        if (saving) return
        _form.value = _form.value?.let { m ->
            m.copy(lines = m.lines + (kind to f(m.lines[kind] ?: LineForm())))
        }
    }

    /**
     * 접속서비스 후보 — 종류에 맞는 것 + **저장된 값**.
     *
     * 기존 회선의 저장값이 후보에 없어도 그대로 보여야 한다. 첫 후보로 바꿔 넣으면 저장할 때마다
     * "서비스 변경 → 재결박 비밀번호 필요(400)" 가 난다(§4.5).
     */
    fun serviceChoices(kind: String, current: String): List<ServiceRef> =
        serviceChoicesOf(_view.value, kind, current)

    fun save() {
        val f = _form.value ?: return
        if (!f.canSave) return
        val m = s.management() ?: return
        // 서버가 400 을 낼 것이 확실한 입력은 보내지 않는다 — 구성원만 생기고 회선은 실패하는 부분 저장을 줄인다.
        saveBlocker(f).takeIf { it.isNotEmpty() }?.let { why -> _form.value = f.copy(error = why); return }
        _form.value = f.copy(busy = true, error = "")
        _error.value = ""
        job = scope.launch {
            var userId = f.orig?.userId ?: 0L
            if (f.isNew) {
                val r = m.createMember(f.name.trim(), f.org, f.title.trim(), f.loginId.trim(), f.password)
                if (!r.ok) { _form.value = _form.value?.copy(busy = false, error = r.reason); return@launch }
                userId = r.value ?: 0L
                if (userId <= 0L) {
                    _form.value = _form.value?.copy(busy = false, error = "서버가 구성원 id 를 내지 않았습니다")
                    return@launch
                }
                // **생성은 이미 서버에서 끝났다.** 뒤따르는 회선 PUT 이 실패해도 이 폼은 더 이상
                // «새 구성원» 이 아니다 — 그대로 두면 [저장]을 다시 눌렀을 때 POST 가 또 나가
                // 같은 사람이 두 번 만들어진다. 확정된 id 를 폼에 박아 재시도가 갱신 경로를 타게 한다.
                _form.value = _form.value?.copy(orig = MemberInfo(userId = userId, name = f.name.trim(),
                    loginId = f.loginId.trim(), org = f.org, title = f.title.trim()))
            } else {
                val r = m.updateMember(userId, f.name.trim(), f.org, f.title.trim(), f.loginId.trim(), f.password)
                if (!r.ok) { _form.value = _form.value?.copy(busy = false, error = r.reason); return@launch }
            }

            // 회선 — 바뀐 것만. 바뀌지 않은 회선을 보내면 서버가 재결박을 요구한다.
            // **성공한 회선은 그때그때 폼 기준에 반영한다** — 뒤에서 실패해 재시도할 때 이미 적용된
            // 변경을 다시 보내면 «IMSI·서비스 변경» 으로 오판돼 400 이 난다.
            var pttLine: NumberInfo? = null
            for (kind in LineKind.all) {
                val form = f.lines[kind] ?: LineForm()
                when (val act = lineAction(_form.value?.orig?.line(kind), form)) {
                    LineAction.None -> if (kind == LineKind.PTT) pttLine = _form.value?.orig?.line(kind)
                    LineAction.Delete -> {
                        val r = m.deleteNumber(userId, kind)
                        if (!r.ok) { _form.value = _form.value?.copy(busy = false, error = r.reason); syncDirectory(); return@launch }
                        noteLineSaved(kind, null)
                    }
                    is LineAction.Put -> {
                        val r = m.putNumber(userId, kind, act.number, act.password)
                        if (!r.ok) { _form.value = _form.value?.copy(busy = false, error = r.reason); syncDirectory(); return@launch }
                        noteLineSaved(kind, act.number)
                        if (kind == LineKind.PTT) pttLine = act.number
                    }
                }
            }

            // PTT 자격 — **PTT 회선이 있을 때만** 보낸다. 가입이 없으면 서버가 404 `Subscription not found`
            // 로 거절하므로(csc `dispatch_directory.py`), 전화 회선만 만든 신규 구성원은 실제로는
            // 생성됐는데 화면은 «저장 실패» 로 남는다.
            // 원격 청취는 싣지 않는다(역할 배정의 결과, 서버가 `not_editable` 로 거절한다).
            val needProfile = pttLine != null && pttLine.msisdn.isNotBlank() &&
                f.allowCreateGroup != (_form.value?.orig?.allowCreateGroup ?: false)
            if (needProfile) {
                val r = m.putPttProfile(userId, mapOf("allowCreateGroup" to f.allowCreateGroup))
                if (!r.ok) { _form.value = _form.value?.copy(busy = false, error = r.reason); syncDirectory(); return@launch }
            }

            _form.value = null
            s.notify(NoticeLevel.INFO, if (f.isNew) "구성원 «${f.name.trim()}» 생성 완료" else "구성원 «${f.name.trim()}» 저장 완료")
            reload(force = true)        // 저장 뒤 한 벌 재조회(§4.5)
            syncDirectory()
        }.also { j ->
            // 예외·취소로 끝나도 «저장 중» 에 잠기지 않는다 — 저장 중에는 [취소]·다른 구성원 열기를 받지 않으므로 여기서 풀지 않으면
            //   로그아웃 전에는 폼을 닫을 길이 없다(정상 경로는 이미 풀었거나 폼을 닫았다).
            j.invokeOnCompletion { _form.value?.takeIf { it.busy }?.let { _form.value = it.copy(busy = false) } }
        }
    }

    /** 저장에 성공한 회선을 폼 기준(`orig`)에 반영한다 — 재시도가 같은 변경을 다시 보내지 않게. */
    private fun noteLineSaved(kind: String, saved: NumberInfo?) {
        _form.value = _form.value?.let { cur ->
            val o = cur.orig ?: return@let cur
            cur.copy(orig = when (kind) {
                LineKind.VOLTE -> o.copy(volte = saved)
                LineKind.VOIP -> o.copy(voip = saved)
                else -> o.copy(ptt = saved)
            })
        }
    }

    fun deleteMember(m: MemberInfo) {
        if (saving) return
        val c = s.management() ?: return
        _error.value = ""
        job = scope.launch {
            val r = c.deleteMember(m.userId)
            if (!r.ok) { _error.value = r.reason; return@launch }
            _form.value = null
            s.notify(NoticeLevel.INFO, "구성원 «${m.name}» 삭제 완료")
            reload(force = true)
            syncDirectory()
        }
    }

    // ── 조직 폼 ───────────────────────────────────────────────────────────────

    /** [+ 새 조직] — 고른 조직 아래에 만든다. */
    fun newOrg() = openOrgForm(OrgNode("", "", _org.value, 0), isNew = true)

    fun editOrg(o: OrgNode) = openOrgForm(o, isNew = false)

    private fun openOrgForm(o: OrgNode, isNew: Boolean) {
        _error.value = ""
        _orgFormIsNew.value = isNew
        orgFormBase = o
        _orgForm.value = o
    }

    fun updateOrgForm(f: (OrgNode) -> OrgNode) { _orgForm.value = _orgForm.value?.let(f) }

    fun closeOrgForm() { _orgForm.value = null; orgFormBase = null }

    fun saveOrg() {
        val o = _orgForm.value ?: return
        val m = s.management() ?: return
        if (o.code.isBlank() || o.name.isBlank()) { _error.value = "조직 코드와 이름이 필요합니다"; return }
        val isNew = _orgFormIsNew.value
        _error.value = ""
        job = scope.launch {
            val code = o.code.trim()
            val r = if (isNew) m.createOrg(code, o.name.trim(), o.parent, o.sort)
                    else m.updateOrg(o.code, o.name.trim(), o.parent, o.sort)
            if (!r.ok) { _error.value = r.reason; return@launch }
            closeOrgForm()
            s.notify(NoticeLevel.INFO, if (isNew) "조직 «${o.name.trim()}» 생성 완료" else "조직 «${o.name.trim()}» 저장 완료")
            reload(force = true)
            // 방금 만든·고친 조직을 고른 채로 돌아온다 — 트리에서 어디에 섰는지 보인다.
            if (_view.value.orgs.any { it.code == code }) { _org.value = code; project() }
            syncDirectory()
        }
    }

    fun deleteOrg(code: String) {
        val m = s.management() ?: return
        _error.value = ""
        job = scope.launch {
            val r = m.deleteOrg(code)
            if (!r.ok) { _error.value = r.reason; return@launch }
            if (_org.value == code) _org.value = ""
            closeOrgForm()
            s.notify(NoticeLevel.INFO, "조직 삭제 완료")
            reload(force = true)
            syncDirectory()
        }
    }

    companion object {
        /**
         * 접속서비스 후보 — **순수 함수**다.
         *
         * 화면이 관측 중인 [AdminView] 에서 바로 계산해야 한다. VM 의 현재 값을 읽는 형태로 두면
         * Compose 가 그 읽기를 추적하지 못해 [새로고침] 뒤에도 옛 후보가 남는다.
         */
        fun serviceChoicesOf(view: AdminView, kind: String, current: String): List<ServiceRef> {
            val base = view.servicesOf(kind)
            if (current.isBlank() || base.any { it.name == current }) return base
            return base + ServiceRef(kind, current)
        }

        /**
         * 폼을 열 때의 회선 한 장.
         *
         * 기존 회선은 **저장된 값 그대로**다(접속서비스·transport 를 다른 값으로 바꿔 놓으면 저장마다 변경으로 읽힌다).
         * 없는 회선은 새로 개설할 카드라 그 종류의 **첫 후보**와 **TLS**(관제 소프트폰 규약)를 미리 골라 둔다 —
         * 비워 두면 `voip` 는 서버가 400 `service_ref required` 로 거절한다.
         */
        fun openLine(view: AdminView, kind: String, line: NumberInfo?): LineForm =
            if (line == null || line.isEmpty)
                LineForm(serviceRef = view.servicesOf(kind).firstOrNull()?.name.orEmpty(), sipTransport = "TLS")
            else LineForm.of(line)

        /**
         * 회선 하나에 대해 저장이 할 일.
         *
         * 규칙(§4.5 — 서버 `admin.py` 가입 경로와 맞물린다):
         * - 번호를 비우면 **회선 삭제**(없던 회선이면 할 일 없음).
         * - 번호·접속서비스·transport 가 그대로고 비밀번호도 비었으면 **보내지 않는다** — 보내면
         *   서버가 H(A1) 재결박을 요구해 저장마다 400 이 난다.
         * - 같은 번호를 다시 실을 때는 **저장된 IMSI 를 그대로** 싣는다. 비우면 서버가 번호 숫자로 채워
         *   "IMSI 변경" 으로 오판한다. 번호가 바뀌면 새 회선이라 IMSI 를 비운다.
         * - transport 만 바뀐 회선도 PUT 한다(H(A1) 무관, 비밀번호 불필요).
         */
        fun lineAction(orig: NumberInfo?, form: LineForm): LineAction {
            val msisdn = form.msisdn.trim()
            if (msisdn.isEmpty()) return if (orig == null || orig.isEmpty) LineAction.None else LineAction.Delete

            val sameNumber = orig != null && orig.msisdn == msisdn
            val sameService = orig != null && orig.serviceRef == form.serviceRef
            val sameTransport = orig != null && orig.sipTransport == form.sipTransport
            if (orig != null && sameNumber && sameService && sameTransport && form.password.isBlank())
                return LineAction.None

            return LineAction.Put(
                NumberInfo(
                    msisdn = msisdn,
                    imsi = if (sameNumber) orig!!.imsi else "",
                    serviceRef = form.serviceRef,
                    sipTransport = form.sipTransport),
                form.password)
        }

        /** 비밀번호가 반드시 필요한가 — 새 회선·번호 변경·접속서비스 변경(서버가 H(A1) 를 다시 만든다). */
        fun needsPassword(orig: NumberInfo?, form: LineForm): Boolean {
            val msisdn = form.msisdn.trim()
            if (msisdn.isEmpty()) return false
            if (orig == null || orig.isEmpty) return true
            return orig.msisdn != msisdn || orig.serviceRef != form.serviceRef
        }

        /** 회선의 짧은 이름 — «VoLTE»·«VoIP»·«PTT»(문구용). */
        fun lineName(kind: String): String = LineKind.label(kind).removeSuffix(" 번호")

        /**
         * 저장을 막는 사유 — 서버가 400 을 낼 것이 **확실한** 입력(데스크톱 `SaveMember` 의 선검사). 없으면 빈 문자열.
         *
         * - 새 회선인데 접속서비스가 없다(후보 0건 — 서버 설정 문제라 운영자가 풀어야 한다).
         * - 새 회선·번호 변경·접속서비스 변경인데 SIP 비밀번호가 없다(서버가 H(A1) 를 만들 재료가 없다).
         */
        fun saveBlocker(f: MemberForm): String {
            for (kind in LineKind.all) {
                val line = f.lines[kind] ?: LineForm()
                val orig = f.orig?.line(kind)
                if (lineAction(orig, line) !is LineAction.Put) continue
                val what = lineName(kind)
                val opening = orig == null || orig.isEmpty
                if (opening && line.serviceRef.isBlank())
                    return "$what 회선을 개설하려면 접속서비스가 필요합니다 — 후보가 없으면 서버 설정(운영자) 문제입니다"
                if (needsPassword(orig, line) && line.password.isBlank()) return when {
                    opening -> "$what 새 회선에는 SIP 비밀번호가 필요합니다(H(A1) 결박)"
                    orig!!.msisdn != line.msisdn.trim() -> "$what 번호를 바꾸려면 SIP 비밀번호가 필요합니다(H(A1) 재결박)"
                    else -> "$what 접속서비스를 바꾸려면 SIP 비밀번호가 필요합니다(H(A1) 재결박)"
                }
            }
            return ""
        }

        /**
         * 이 저장이 **지울** 회선 — (종류, 저장된 번호). 지우면 그 단말의 등록이 끊기므로 화면이 저장 전에 확인을 받는다.
         */
        fun linesToDelete(f: MemberForm): List<Pair<String, String>> =
            LineKind.all.mapNotNull { kind ->
                val orig = f.orig?.line(kind)
                if (lineAction(orig, f.lines[kind] ?: LineForm()) == LineAction.Delete) kind to orig!!.msisdn else null
            }

        /**
         * 구성원 검색 — 이름·로그인 아이디(대소문자 무시) 또는 번호. 번호는 **어느 표기로 쳐도** 걸린다
         * (`010…` 로 쳐도 `+8210…` 로 저장된 회선이 나온다 — 주소록 제안과 같은 규칙).
         */
        fun memberMatches(m: MemberInfo, query: String): Boolean {
            val q = query.trim()
            if (q.isEmpty()) return true
            if (m.name.contains(q, ignoreCase = true) || m.loginId.contains(q, ignoreCase = true)) return true
            val numberLike = q.all { it.isDigit() || it in "+-() " }
            val qd = q.filter { it.isDigit() || it == '+' }
            if (!numberLike || qd.isEmpty()) return false
            return LineKind.all.any { k ->
                val n = m.line(k)?.msisdn.orEmpty()
                n.isNotEmpty() && DirectoryBook.numberForms(n).any { it.contains(qd) }
            }
        }

        /** 관리 범위의 한 줄 — 서버가 준 범위를 그대로 읽어 준다(판정이 아니라 표시다). 범위가 없으면 빈 값. */
        fun scopeText(view: AdminView): String = when {
            !view.scope.canWrite -> ""
            view.scope.directoryWrite == "all" -> "관리 범위: 전체 조직"
            else -> "관리 범위: ${view.orgPath(view.scope.orgCode).ifBlank { view.scope.orgCode }.ifBlank { "소속 조직" }} 하위"
        }
    }
}
