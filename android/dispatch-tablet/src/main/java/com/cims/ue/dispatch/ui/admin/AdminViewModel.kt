// [관리] 화면 상태 (docs/design/features/android_dispatch_tablet.md §6.7, dispatch_desktop_ui.md §4.5)
//
// 조직·구성원·VoLTE/VoIP/PTT 번호. **앱은 범위 enum 을 해석하지 않는다** — 서버가 걸러 준 조직·구성원만
// 보이고 쓰기 판정도 서버가 한다. 앱이 하는 판단은 하나뿐이다: *무엇이 바뀌었나*. 바뀐 회선만 PUT 하고,
// 바뀐 것이 없으면 보내지 않는다(서버가 H(A1) 재결박을 요구해 저장마다 400 이 나기 때문).
package com.cims.ue.dispatch.ui.admin

import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.AdminView
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.LineKind
import com.cims.ue.dispatch.session.MemberInfo
import com.cims.ue.dispatch.session.NumberInfo
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.session.ServiceRef
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
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
    val heading: String get() = if (isNew) "새 구성원" else "편집 — ${orig!!.name}"

    /** 열 때와 달라졌는가 — 클릭만으로 폼이 열리므로 "열림 ≠ 변경" 이다(§4.5). */
    val dirty: Boolean get() {
        if (isNew) return name.isNotBlank() || lines.values.any { it.msisdn.isNotBlank() }
        val o = orig!!
        if (name != o.name || title != o.title || org != o.org || loginId != o.loginId) return true
        if (password.isNotBlank()) return true
        if (allowCreateGroup != o.allowCreateGroup) return true
        return LineKind.all.any { k -> lines[k] != LineForm.of(o.line(k)) }
    }

    val canSave: Boolean get() = !busy && name.isNotBlank()
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

    /** 조직 편집 폼 — null 이면 닫혀 있다. */
    private val _orgForm = MutableStateFlow<OrgNode?>(null)
    val orgForm: StateFlow<OrgNode?> = _orgForm.asStateFlow()
    private val _orgFormIsNew = MutableStateFlow(true)
    val orgFormIsNew: StateFlow<Boolean> = _orgFormIsNew.asStateFlow()

    private val _loading = MutableStateFlow(false)
    val loading: StateFlow<Boolean> = _loading.asStateFlow()

    private val _error = MutableStateFlow("")
    val error: StateFlow<String> = _error.asStateFlow()

    private var etag = ""
    private var job: Job? = null

    /** 관리 범위가 없으면 화면이 잠긴다 — 메뉴는 보이되 비활성이다(§3.4). */
    val available: Boolean get() = s.dispatch.canAdminDirectory

    /** 저장하지 않은 변경 — [관리] 메뉴 점 배지와 화면 머리 배지의 근거. */
    val dirty: Boolean get() = _form.value?.dirty == true

    // ── 적재 ──────────────────────────────────────────────────────────────────

    fun load(force: Boolean = false) {
        job?.cancel()
        job = scope.launch { reload(force) }
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

    fun selectOrg(code: String) { _org.value = if (_org.value == code) "" else code; project() }
    fun search(q: String) { _query.value = q; project() }

    private fun project() {
        val v = _view.value
        val subtree = v.orgSubtree(_org.value)
        val q = _query.value.trim().lowercase()
        _members.value = v.members.filter { m ->
            (subtree == null || m.org in subtree) &&
                (q.isEmpty() || m.name.lowercase().contains(q) || m.loginId.lowercase().contains(q) ||
                    LineKind.all.any { k -> m.line(k)?.msisdn?.contains(q) == true })
        }
    }

    // ── 구성원 폼 ─────────────────────────────────────────────────────────────

    /** 행 클릭 = 바로 편집. 저장하지 않은 변경이 있으면 화면이 먼저 확인을 받는다(§4.5). */
    fun open(m: MemberInfo) {
        _form.value = MemberForm(
            orig = m, name = m.name, title = m.title, org = m.org, loginId = m.loginId,
            lines = LineKind.all.associateWith { LineForm.of(m.line(it)) },
            allowCreateGroup = m.allowCreateGroup)
    }

    fun newMember() {
        _form.value = MemberForm(org = _org.value)
    }

    fun closeForm() { _form.value = null }

    fun update(f: (MemberForm) -> MemberForm) { _form.value = _form.value?.let(f) }

    fun updateLine(kind: String, f: (LineForm) -> LineForm) {
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
        _form.value = f.copy(busy = true, error = "")
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
                        if (!r.ok) { _form.value = _form.value?.copy(busy = false, error = r.reason); return@launch }
                        noteLineSaved(kind, null)
                    }
                    is LineAction.Put -> {
                        val r = m.putNumber(userId, kind, act.number, act.password)
                        if (!r.ok) { _form.value = _form.value?.copy(busy = false, error = r.reason); return@launch }
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
                if (!r.ok) { _form.value = _form.value?.copy(busy = false, error = r.reason); return@launch }
            }

            _form.value = null
            reload(force = true)        // 저장 뒤 한 벌 재조회(§4.5)
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
        val c = s.management() ?: return
        job = scope.launch {
            val r = c.deleteMember(m.userId)
            if (!r.ok) { _error.value = r.reason; return@launch }
            _form.value = null
            reload(force = true)
        }
    }

    // ── 조직 폼 ───────────────────────────────────────────────────────────────

    fun newOrg() { _orgFormIsNew.value = true; _orgForm.value = OrgNode("", "", _org.value, 0) }

    fun editOrg(o: OrgNode) { _orgFormIsNew.value = false; _orgForm.value = o }

    fun updateOrgForm(f: (OrgNode) -> OrgNode) { _orgForm.value = _orgForm.value?.let(f) }

    fun closeOrgForm() { _orgForm.value = null }

    fun saveOrg() {
        val o = _orgForm.value ?: return
        val m = s.management() ?: return
        if (o.code.isBlank() || o.name.isBlank()) { _error.value = "조직 코드와 이름이 필요합니다"; return }
        val isNew = _orgFormIsNew.value
        job = scope.launch {
            val r = if (isNew) m.createOrg(o.code.trim(), o.name.trim(), o.parent, o.sort)
                    else m.updateOrg(o.code, o.name.trim(), o.parent, o.sort)
            if (!r.ok) { _error.value = r.reason; return@launch }
            _orgForm.value = null
            reload(force = true)
        }
    }

    fun deleteOrg(code: String) {
        val m = s.management() ?: return
        job = scope.launch {
            val r = m.deleteOrg(code)
            if (!r.ok) { _error.value = r.reason; return@launch }
            if (_org.value == code) _org.value = ""
            _orgForm.value = null
            reload(force = true)
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
    }
}
