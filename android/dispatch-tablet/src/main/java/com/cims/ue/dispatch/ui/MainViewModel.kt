// 화면 상태 (docs/design/features/android_dispatch_tablet.md §6.1)
//
// **ViewModel 은 화면 상태만 갖는다.** 서버 상태는 전부 DispatchSession(Service 수명)의 Flow 를 접어
// 만든다 — 그래야 Activity 가 재생성돼도 세션이 끊기지 않는다.
package com.cims.ue.dispatch.ui

import com.cims.ue.dispatch.session.dial
import com.cims.ue.dispatch.session.startPrivateCall
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.cims.ue.dispatch.session.refreshGroups
import com.cims.ue.dispatch.ui.ptt.PttActivityViewModel
import com.cims.ue.dispatch.ui.ptt.PttChannelsViewModel
import com.cims.ue.dispatch.ui.ptt.PttMessagesViewModel
import com.cims.ue.dispatch.ui.ptt.ScopedChannelsViewModel
import com.cims.ue.dispatch.ui.call.CallDeskViewModel
import com.cims.ue.dispatch.ui.call.SmsMessagesViewModel
import com.cims.ue.dispatch.ui.admin.AdminViewModel
import com.cims.ue.dispatch.ui.groups.PttGroupsViewModel
import com.cims.ue.dispatch.ui.history.HistoryViewModel
import com.cims.ue.dispatch.session.DispatchService
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.SessionState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

class MainViewModel : ViewModel() {

    private val session: DispatchSession? get() = DispatchService.session

    /** 지금 패널 VM 이 묶여 있는 세션. 교체 처리가 끝난 뒤에만 갱신된다. */
    private var boundSession: DispatchSession? = null

    /** 지금 패널 VM 이 묶여 있는 **로그인 세대**. 다르면 캐시를 버린다(계정 격리). */
    private var boundGeneration: Int = 0
    private var genJob: kotlinx.coroutines.Job? = null

    // **`init` 보다 앞에 둔다.** 아래 수집자는 `Main.immediate` 라 세션이 이미 떠 있으면(부팅 자동 기동·알림에서 열기·Activity
    //   재생성) 생성자 안에서 곧바로 `rebind → closePanels` 까지 돈다. 그때 뒤에 선언한 `val` 은 아직 null 이다 — 예외로 수집이
    //   죽으면 `boundSession` 이 영영 서지 않아 모든 면이 «준비 중» 에 멈춘다.
    private var adminDirtyJob: kotlinx.coroutines.Job? = null
    private val _adminDirty = MutableStateFlow(false)
    /** [관리] 메뉴의 점 배지 — 관측 가능한 꼴(레일이 이것을 읽는다). */
    val adminDirtyFlow: StateFlow<Boolean> = _adminDirty.asStateFlow()

    init {
        // **세션 교체는 Flow 로 처리한다.** 예전에는 VM 게터가 컴포지션 도중 `rebindIfNeeded` 를
        // 불렀는데, 그 안에서 코루틴을 끊고 플레이어를 멈추고 발언을 해제한다 — 재구성이 여러 번
        // 일어나거나 취소되는 Compose 규약에서 파괴적 부작용을 컴포지션에 두면 안 된다.
        viewModelScope.launch(com.cims.ue.dispatch.session.UnhandledGuard) {
            DispatchService.sessionFlow.collect { s ->
                rebind(s)
                // **로그인 세대도 관측한다.** 세션 객체는 Service 수명이라 로그아웃해도 안 바뀐다 —
                // 객체 교체만 보면 다음 사람이 로그인했을 때 앞 사람의 관리 목록·이력·폼이 남는다.
                genJob?.cancel()
                genJob = s?.let { sess ->
                    launch {
                        sess.loginGeneration.collect { g ->
                            // 계정이 바뀐다 — 패널 VM 과 함께 화면 좌표·고른 사람도 비운다(다음 사람의 [채널 추가] 에 앞 사람이
                            //   고른 사람이 남지 않게).
                            if (g != boundGeneration) {
                                closePanels(); boundGeneration = g
                                _nav.value = NavState(); _picked.value = emptyList()
                            }
                        }
                    }
                }
            }
        }
    }

    /**
     * 세션이 바뀌면(로그아웃→재로그인·Service 재생성) 패널 VM 을 **닫고** 버린다.
     *
     * 닫지 않으면 버려진 VM 의 코루틴(`stateIn(Eagerly)`·`collect`)이 계속 돌아 세션 Flow 구독이
     * 쌓이고, 녹취 플레이어·발언 해제 같은 정리가 죽는다. 이들은 androidx `ViewModel` 이 아니라
     * [ScreenViewModel] 이라 수명을 여기서 명시적으로 든다.
     */
    private fun rebind(s: DispatchSession?) {
        if (boundSession === s) return
        closePanels()
        boundSession = s
        boundGeneration = s?.loginGeneration?.value ?: 0
    }

    /**
     * 지금 VM 을 만들어도 되는 세션.
     *
     * 교체 처리가 끝난 세션만 돌려준다 — 아직이면 null 이라 화면이 한 프레임 «준비 중» 에 머문다.
     * 게터가 하는 일은 «없으면 만든다» 뿐이고(멱등), 버리는 일은 [rebind] 만 한다.
     */
    private fun bound(): DispatchSession? = session?.takeIf { it === boundSession }

    private fun closePanels() {
        _history?.onLeave()                      // 녹취 재생을 멈추고 임시 파일을 정리한다
        listOf(_ptt, _scoped, _messages, _activity, _calls, _sms, _history, _groups, _admin)
            .forEach { runCatching { it?.close() } }
        _ptt = null; _scoped = null; _messages = null; _activity = null; _calls = null
        _sms = null; _history = null; _groups = null; _admin = null
        adminDirtyJob?.cancel(); adminDirtyJob = null; _adminDirty.value = false
        panelDraft = false
    }

    /** 이 VM 이 진짜 `ViewModel` 이라 여기서 사슬이 끝난다 — Activity 가 끝나면 전부 닫힌다. */
    override fun onCleared() {
        closePanels()
        super.onCleared()
    }

    // ── 화면 좌표(§6.3) ───────────────────────────────────────────────────────
    // 레일(관제·이력·PTT 그룹·관리) · 관제의 모드와 면 · 오른쪽 사이드 패널 — **한 값**이다. 따로 두면 규칙(뒤로가기·
    //   패널 닫힘)이 여러 값을 한꺼번에 바꿀 때 중간 상태가 한 프레임 그려진다.
    private val _nav = MutableStateFlow(NavState())
    val nav: StateFlow<NavState> = _nav.asStateFlow()
    private fun update(f: (NavState) -> NavState) = setNav(f(_nav.value))

    /**
     * 화면 좌표를 바꾼다. **«새 PTT 그룹» 흐름(채널 추가 ↔ 새 그룹)을 벗어나면 쓰던 새 그룹 폼을 닫는다** — 패널의 [취소]·× 만
     * 닫으면, 뒤로가기·면 넘기기·레일로 나갔을 때 보이지 않는 폼이 잠금을 쥔 채 남아 채널 ⋮[편집] 이 거절되고 채널 상세의
     * «편성» 이 영영 «받는 중» 이다. ← 로 채널 추가에 물러난 것은 흐름 안이다(사람을 더 고르고 돌아온다 — 쓰던 이름이 남는다).
     */
    private fun setNav(next: NavState) {
        val prev = _nav.value
        _nav.value = next
        fun inFlow(p: SidePanel?) = p == SidePanel.NewGroup || p == SidePanel.AddChannel
        // **패널 흐름에서 연 폼만** — [PTT 그룹] 화면의 [새 그룹] 으로 연 폼은 그 화면의 것이라 여기서 닫지 않는다.
        if (panelDraft && inFlow(prev.panel) && !inFlow(next.panel)) {
            panelDraft = false
            _groups?.takeIf { it.editingNew }?.cancelEdit()
        }
    }

    /** 지금의 새 그룹 폼을 [채널 추가] › [그룹 추가 ›] 가 열었다 — 그 흐름을 벗어나면 닫는다([setNav]). */
    private var panelDraft = false

    fun setPttPane(p: PttPane) = showPage(pageOf(p))
    fun setCallPane(p: CallPane) = showPage(pageOf(p))
    fun setMode(m: DispatchMode) = update { it.toMode(m) }

    // ── 사이드 패널 ──
    fun togglePanel(p: SidePanel) = update { it.togglePanel(p) }
    fun showPanel(p: SidePanel) = update { it.showPanel(p) }
    fun closePanel() = update { it.closePanel() }
    fun togglePin() = update { it.togglePin() }
    /** 패널 안의 ← — 한 겹 들어온 것(새 그룹)에서 채널 추가로. */
    fun panelBack() = update { n -> n.panel?.parent?.let { n.copy(panel = it) } ?: n }

    /**
     * [채널 추가] 패널에서 고른 사람(PTT 번호) — 개별 통화·애드혹 통화·그룹 추가가 쓴다. 패널을 닫아도 남는다: «그룹 추가» 한
     * 겹을 들어갔다 나와도 고른 것이 그대로여야 한다. 쓰고 나면 비운다.
     */
    private val _picked = MutableStateFlow<List<String>>(emptyList())
    val picked: StateFlow<List<String>> = _picked.asStateFlow()
    fun togglePick(number: String) {
        val cur = _picked.value
        _picked.value = if (number in cur) cur - number else cur + number
    }
    fun clearPicked() { _picked.value = emptyList() }
    /** 고름에 **더한다**(토글하지 않는다) — 사람 메뉴 [애드혹에 추가] 가 이미 고른 사람을 빼 버리면 안 된다. */
    fun addPick(number: String) {
        if (number.isBlank()) return
        val key = com.cims.ue.dispatch.session.DirectoryBook.normalize(number)
        if (_picked.value.none { com.cims.ue.dispatch.session.DirectoryBook.normalize(it) == key }) _picked.value = _picked.value + number
    }

    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy.asStateFlow()

    // 패널 VM 은 세션이 선 뒤에만 만들 수 있다. 화면 수명 동안 하나씩 유지한다(§6.1 폼 유지).
    private var _ptt: PttChannelsViewModel? = null
    private var _scoped: ScopedChannelsViewModel? = null
    private var _messages: PttMessagesViewModel? = null
    private var _activity: PttActivityViewModel? = null
    private var _calls: CallDeskViewModel? = null
    private var _sms: SmsMessagesViewModel? = null
    private var _history: HistoryViewModel? = null
    private var _groups: PttGroupsViewModel? = null
    private var _admin: AdminViewModel? = null

    val ptt: PttChannelsViewModel? get() = bound()?.let { s -> _ptt ?: PttChannelsViewModel(s).also { _ptt = it } }
    val scoped: ScopedChannelsViewModel? get() = bound()?.let { s -> _scoped ?: ScopedChannelsViewModel(s).also { _scoped = it } }
    val messages: PttMessagesViewModel? get() = bound()?.let { s -> _messages ?: PttMessagesViewModel(s).also { _messages = it } }
    val activity: PttActivityViewModel? get() = bound()?.let { s -> _activity ?: PttActivityViewModel(s).also { _activity = it } }
    val calls: CallDeskViewModel? get() = bound()?.let { s -> _calls ?: CallDeskViewModel(s).also { _calls = it } }
    val sms: SmsMessagesViewModel? get() = bound()?.let { s -> _sms ?: SmsMessagesViewModel(s).also { _sms = it } }

    // 관제 밖 화면 VM — 화면 수명 동안 하나라 탭을 오가도 폼·조회 결과가 남는다(§6.2).
    val history: HistoryViewModel? get() = bound()?.let { s -> _history ?: HistoryViewModel(s, s.cacheDir).also { _history = it } }
    val pttGroups: PttGroupsViewModel? get() = bound()?.let { s -> _groups ?: PttGroupsViewModel(s).also { _groups = it } }
    val admin: AdminViewModel? get() = bound()?.let { s -> _admin ?: AdminViewModel(s).also { vm ->
        _admin = vm
        // 점 배지 — 폼의 변경을 따라간다. 패널을 닫을 때(로그아웃·세션 교체) 수집을 끝내고 점을 끈다(`closePanels`).
        adminDirtyJob?.cancel()
        adminDirtyJob = viewModelScope.launch(com.cims.ue.dispatch.session.UnhandledGuard) { vm.dirtyFlow.collect { if (_admin === vm) _adminDirty.value = it } }
    } }

    /**
     * 관리 범위가 있는가 — 없으면 레일 [관리] 가 흐리다(데스크톱 `CanManage`). 범위 판정은 서버가 준 `dispatch.directoryWrite`
     * (전환기 `directoryAdmin`)다.
     */
    val canAdmin: Boolean get() = session?.dispatch?.canAdminDirectory == true

    /** 흐린 [관리] 를 눌렀다 — 왜 못 여는지 알린다(데스크톱은 툴팁, 태블릿엔 툴팁이 없다). */
    fun adminDenied() {
        session?.notify(com.cims.ue.dispatch.session.NoticeLevel.WARN,
            "관리 범위가 없습니다", "조직/구성원·번호 관리는 관제 역할의 관리 범위(콘솔 관리 > 역할)가 있어야 합니다")
    }

    /** [관리] 메뉴의 점 배지 — 저장하지 않은 폼이 있다는 뜻이다(§4.5). 전환은 막지 않는다. */
    val adminDirty: Boolean get() = _admin?.dirty == true

    /** 잠금 발언 설정 — 설정 화면이 바꾼다. */
    val lockTalk: Boolean get() = session?.settingsSnapshot()?.lockTalk == true

    /** 세션 준비·교체 — 화면이 이걸 구독해야 Service 가 늦게 서도 대기 화면에 머물지 않는다(§F8). */
    val sessionFlow: StateFlow<DispatchSession?> = DispatchService.sessionFlow

    val state: StateFlow<SessionState>? get() = session?.state
    val error: StateFlow<String?>? get() = session?.error

    /** 레일 — 규칙은 [onNav] 가 갖는다([관제] 를 다시 누르면 패널을 닫는다). */
    fun show(s: AppScreen) = update { it.onNav(s) }

    /** 탭·스와이프로 관제의 면을 옮겼다 — 모드와 면을 **함께** 옮긴다([DISPATCH_PAGES]). 고정하지 않은 패널은 닫힌다. */
    fun showPage(page: DispatchPage) = update { it.toPage(page) }

    /**
     * 채널을 **찾아가서** 연다 — 긴급 배너·검색·[PTT 그룹] «채널로». [무전] › «채널» 면에 그 채널 상세 패널이 선다. `id` 는 채널
     * 카드의 id(그룹 id 또는 세션 id)다. 포커스도 같이 옮긴다 — «메시지»·«이벤트» 면이 포커스를 따라간다(§6.3).
     */
    fun openChannel(id: String) {
        if (id.isBlank()) return
        ptt?.setFocus(id)
        focusPane(id)
        update { it.openChannel(id) }
    }

    /** 채널 카드·타 채널 행을 눌렀다 — 같은 채널이면 패널을 닫고, 다른 채널이면 바꾼다. 면은 그대로다. */
    fun toggleChannel(id: String) {
        if (id.isBlank()) return
        ptt?.setFocus(id)
        focusPane(id)                 // «메시지» 의 [따라가기] 가 이 채널의 대화로 옮겨 간다(데스크톱 카드 선택과 같다)
        update { it.togglePanel(SidePanel.Channel(id)) }
    }

    /** 메시지 [채널 정보] — 보던 대화 옆에 그 채널 상세를 세운다(면을 옮기지 않는다). */
    fun channelInfo(id: String) {
        if (id.isBlank()) return
        update { it.showPanel(SidePanel.Channel(id)) }
    }

    fun closeChannel() = closePanel()

    /** [사용자] 패널에서 걸었다 — «채널» 면으로 간다(새 카드가 거기 선다). 고정한 패널은 그대로 둔다. */
    fun showChannelsKeepingPanel() = update { it.copy(screen = AppScreen.DISPATCH, mode = DispatchMode.PTT, pttPane = PttPane.CHANNELS) }

    /** 뒤로가기 한 단계 — 규칙은 [onBack] 이 갖는다(가로채기 판정도 같은 함수를 쓴다). */
    fun back(): Boolean {
        val next = _nav.value.onBack() ?: return false
        setNav(next)
        return true
    }

    /**
     * 세션을 만드는 조작 뒤의 **자동 복귀**(dispatch_desktop_ui.md §3.4).
     *
     * 배너에서 전화를 받으면 관제 > 일반통화 로 돌아간다 — 보류·전달·DTMF·종료가 거기 있다.
     * 받자마자 [이력] 화면에 남아 있으면 끊을 방법이 없다.
     */
    fun goToCalls() = update { it.toMode(DispatchMode.CALL) }   // 내 통화 카드는 [통화] 의 고정 칸이라 어느 통화 면이든 보인다

    /**
     * 사람 메뉴가 고른 행동을 잇는다 — 데스크톱 `MainViewModel` 이 `PersonActionsViewModel` 의 이벤트를 잇는
     * 것과 같은 자리다(§6.2f).
     *
     * **왜 여기인가.** 행동마다 가는 화면이 다르다(무전·통화·그 안의 면). 메뉴를 띄운 패널은 다른 화면의
     * 상태를 모르므로, 전부를 아는 이 VM 이 잇는다.
     *
     * **세션을 만드는 조작은 관제로 돌아간다**(dispatch_desktop_ui.md §3.4) — 개별 통화를 걸어 놓고 [이력]
     * 화면에 남아 있으면 끊을 방법이 없다.
     */
    fun runPersonAction(action: PersonAction, number: String) {
        if (number.isBlank()) return
        val s = bound() ?: return
        when (action) {
            PersonAction.CALL -> {
                viewModelScope.launch(com.cims.ue.dispatch.session.UnhandledGuard) { s.dial(number) }
                goToCalls()
            }
            PersonAction.PRIVATE_CALL -> {
                viewModelScope.launch(com.cims.ue.dispatch.session.UnhandledGuard) {
                    // 곧바로 실패한 까닭(자격 없음·PTT 계정 없음)을 여기서 알린다 — [채널 추가] 패널과 달리 적을 자리가 없다
                    val r = s.startPrivateCall(number)
                    if (!r.ok) s.notify(if (r.code < 0) com.cims.ue.dispatch.session.NoticeLevel.WARN
                                        else com.cims.ue.dispatch.session.NoticeLevel.ERROR, r.reason)
                }
                showPage(pageOf(PttPane.CHANNELS))
            }
            PersonAction.ADHOC_ADD -> {
                // 고름은 이 VM 이 든다 — 그 사람을 더하고 «채널» 면에 [채널 추가] 패널을 세운다(한 번에 — 면을 옮기며 닫힌
                //   패널이 한 프레임 비치지 않게).
                addPick(number)
                update { it.toPage(pageOf(PttPane.CHANNELS)).showPanel(SidePanel.AddChannel) }
            }
            PersonAction.SDS -> {
                messages?.openThread(number)
                showPage(pageOf(PttPane.MESSAGES))
            }
            PersonAction.SMS -> {
                sms?.openTo(number)
                showPage(pageOf(CallPane.MESSAGES))
            }
            // 통화 기록 = «통화내역» 면을 그 사람으로 걸러 연다. 최상위 [이력] 이 아닌 이유는 그쪽이
            //   날짜를 골라 보는 과거 조회라 «이 사람» 축이 없기 때문이다(§6.11).
            PersonAction.HISTORY -> {
                calls?.setPersonFilter(number)
                showPage(pageOf(CallPane.LOG))
            }
        }
    }

    /** 통합 검색의 «채널로» — 그 채널의 상세 패널을 연다(데스크톱 `PttChannels.FocusGroup`). */
    fun focusChannel(groupId: String) = openChannel(groupId)

    /**
     * 채널의 메시지 — 그 그룹의 SDS 스레드를 [무전] › «메시지» 에 연다(데스크톱 ① 카드 `OpenThread` → ④). 채널 패널에
     * 메시지를 두지 않는 대신의 한 걸음이다(§6.3a — 두 곳에 두면 «이 채널 것인가» 가 흐려진다).
     */
    fun openThread(key: String) {
        if (key.isBlank()) return
        messages?.openThread(key)
        showPage(pageOf(PttPane.MESSAGES))
    }

    /** 채널 패널 ⋮ › [편집] — [PTT 그룹] 화면의 그 그룹 편집 폼으로(데스크톱 채널 상세 ⋮ [편집], §6.12). */
    fun editGroup(groupId: String) {
        if (groupId.isBlank()) return
        val g = pttGroups ?: return
        // 고치던 폼을 말없이 덮지 않는다 — 그 폼으로 데려가고 이유를 적는다(데스크톱 `OpenDrawerEdit` 와 같다).
        if (g.locked) session?.notify(com.cims.ue.dispatch.session.NoticeLevel.WARN,
            "편집 중인 폼이 있습니다", "저장하거나 취소한 뒤 다시 시도하세요")
        else g.editById(groupId)
        show(AppScreen.PTT_GROUPS)
    }

    /** 채널 패널 ⋮ › [삭제] — 확인은 패널이 받았다. 지운 채널은 «사라졌습니다» 대신 패널을 닫는다. */
    fun deleteGroup(groupId: String) {
        pttGroups?.deleteById(groupId)
        closePanel()
    }

    /**
     * [채널 추가] › [그룹 추가 ›] — 고른 사람으로 새 그룹 폼을 채워 패널 안 한 겹으로 연다(§6.12). 그룹 만들기는 [PTT 그룹]
     * 화면이 아니라 여기다 — 사람을 고르는 자리에서 곧바로 묶는다. 고치던 폼이 있으면 덮지 않는다.
     */
    fun startNewGroup() {
        val g = pttGroups ?: return
        if (!g.locked) panelDraft = false                  // 앞 폼은 저장·취소로 닫혔다
        // 고치던 폼([PTT 그룹] 화면에서 연 것 — 기존 그룹이든 새 그룹이든)이 있으면 덮지 않는다. **이 패널에서** 쓰던 새 그룹 폼
        //   (← 로 물러나 사람을 더 고르고 돌아왔다)은 이어 쓴다.
        if (g.locked && !(panelDraft && g.editingNew)) {
            session?.notify(com.cims.ue.dispatch.session.NoticeLevel.WARN,
                "편집 중인 폼이 있습니다", "[PTT 그룹] 에서 저장하거나 취소한 뒤 다시 시도하세요"); return
        }
        if (!g.locked) { g.newGroup(); panelDraft = true }
        // 고른 사람을 싣는다 — 이미 있는 번호는 무시된다(이어 쓰는 폼에는 새로 고른 사람만 더해진다)
        val book = session?.pttBook?.value
        _picked.value.forEach { n -> g.addMember(n, book?.nameOf(n).orEmpty()) }
        showPanel(SidePanel.NewGroup)
    }

    /**
     * 그룹을 고칠 수 있는가 — 내 소유(GMS `is_owner`)이거나 서버가 관리 범위로 준 행(`canManage`). 관리 목록은
     * [PTT 그룹] 화면을 열기 전이면 비어 있어 [ensureManaged] 가 받아 둔다.
     */
    fun canManageGroup(groupId: String, managed: List<com.cims.ue.dispatch.session.ManagedGroup>): Boolean {
        if (groupId.isBlank()) return false
        val g = session?.groups?.value?.firstOrNull { it.id == groupId }
        return g?.isOwner == true || managed.any { it.id == groupId && it.canManage }
    }

    fun ensureManaged() { pttGroups?.ensureLoaded() }

    /**
     * [새 채널] 을 둘 것인가 — 그룹 생성 자격(`ptt.allowCreateGroup`)이나 관리 범위가 있고 PTT 회선이 섰을 때(데스크톱
     * `CanCreateGroups` 와 같은 판정). 최종 판정은 서버(GMS)다 — 여기서는 누를 수 없는 버튼을 세우지 않을 뿐이다.
     */
    val canCreateGroups: Boolean get() = session?.let { s ->
        (s.profile.value?.allowGroupCreation == true || s.dispatch.canAdminDirectory) && s.pttAccount != null
    } == true

    /** 채널을 고르면 «메시지»·«이벤트» 면이 그 채널을 따라간다 — 데스크톱 ④⑤ 의 불변(§6.3). */
    fun focusPane(id: String) {
        messages?.onFocusChanged(id)
        activity?.onFocusChanged(id)
    }

    /** 화면 복원 — 세션은 살아 있으므로 스냅샷만 다시 읽는다(§6.7). */
    fun refresh() {
        session?.refreshSessions()
        ptt?.pruneTargets()
    }

    /** PTT 그룹 목록을 다시 받는다(로그인 직후·xcap-diff 알림). */
    fun refreshGroups() {
        val s = session ?: return
        viewModelScope.launch(com.cims.ue.dispatch.session.UnhandledGuard) { s.refreshGroups() }
    }

    fun login(host: String, port: Int, id: String, pw: String, onDone: (String?) -> Unit) {
        val s = session ?: return onDone("세션이 아직 준비되지 않았습니다")
        _busy.value = true
        viewModelScope.launch(com.cims.ue.dispatch.session.UnhandledGuard) {
            // 로그인 → 기동은 세션 수명에서 돈다(`loginAndStart`) — 화면이 닫혀도 기동이 반쪽으로 멈추지 않는다. 그룹·구독은 start() 가 한다(§F7)
            try {
                val r = try { s.loginAndStart(host, port, id, pw) }
                catch (e: kotlinx.coroutines.CancellationException) { throw e }
                catch (e: Exception) {
                    // 예외는 화면에 적는다 — 토스트는 셸에서만 그려져 로그인 화면에서는 보이지 않는다
                    android.util.Log.e("Dispatch", "login", e)
                    onDone("로그인 중 오류 — ${e.message ?: e.javaClass.simpleName}")
                    return@launch
                }
                val started = r.start
                onDone(when {
                    !r.login.ok -> loginErrorText(r.login.code, r.login.reason)
                    started != null && !started.ok -> "엔진 기동 실패 — ${started.reason} (${started.code})"
                    else -> null
                })
            } finally { _busy.value = false }      // 예외로 끝나도 폼이 «로그인 중…» 에 잠기지 않는다
        }
    }

    fun logout() { session?.logout() }

    /**
     * [이력에서 보기] — 떠나온 면의 종류로 [이력] 을 연다(«이벤트» 에서 = 무전, «통화내역» 에서 = 통화). 종류를 넘기지 않으면
     * 통화내역에서 넘어왔는데 무전 세션이 뜬다.
     */
    fun showHistory(kind: com.cims.ue.dispatch.session.HistoryKind) {
        // 같은 종류면 다시 조회한다 — 방금 끝난 세션을 보러 온 것이다(데스크톱 `ShowHistory`). 종류가 바뀌면 `show` 가 조회한다.
        history?.let { h -> if (h.ui.value.kind == kind) h.refresh() else h.show(kind) }
        show(AppScreen.HISTORY)
    }
}

/**
 * 로그인 실패 문구(데스크톱 `LoginViewModel` 과 같은 문장) — 자격 거부(401·403)는 사람이 고칠 수 있는 말로, 그 밖(서버에 닿지
 * 않음·5xx)은 원문 사유와 코드를 붙여 그대로 보인다(조용히 뭉개면 주소가 틀린 것인지 서버가 죽은 것인지 알 수 없다).
 */
internal fun loginErrorText(code: Int, reason: String): String =
    if (code == 401 || code == 403) "아이디 또는 비밀번호가 올바르지 않습니다" else "로그인 실패 — $reason ($code)"
