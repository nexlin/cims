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

    init {
        // **세션 교체는 Flow 로 처리한다.** 예전에는 VM 게터가 컴포지션 도중 `rebindIfNeeded` 를
        // 불렀는데, 그 안에서 코루틴을 끊고 플레이어를 멈추고 발언을 해제한다 — 재구성이 여러 번
        // 일어나거나 취소되는 Compose 규약에서 파괴적 부작용을 컴포지션에 두면 안 된다.
        viewModelScope.launch {
            DispatchService.sessionFlow.collect { s ->
                rebind(s)
                // **로그인 세대도 관측한다.** 세션 객체는 Service 수명이라 로그아웃해도 안 바뀐다 —
                // 객체 교체만 보면 다음 사람이 로그인했을 때 앞 사람의 관리 목록·이력·폼이 남는다.
                genJob?.cancel()
                genJob = s?.let { sess ->
                    launch {
                        sess.loginGeneration.collect { g ->
                            if (g != boundGeneration) { closePanels(); boundGeneration = g }
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
    }

    /** 이 VM 이 진짜 `ViewModel` 이라 여기서 사슬이 끝난다 — Activity 가 끝나면 전부 닫힌다. */
    override fun onCleared() {
        closePanels()
        super.onCleared()
    }

    private val _screen = MutableStateFlow(AppScreen.HISTORY)
    val screen: StateFlow<AppScreen> = _screen.asStateFlow()

    // ── 화면 안의 이동(§6.3) ──────────────────────────────────────────────────
    // 하단 내비가 «어느 일을 하는가» 라면 아래 둘은 «그 안에서 무엇을 보는가» 다. 뒤로가기로 되돌린다.

    /** [무전]에서 연 채널(그룹 id 또는 세션 카드 id). null = 목록. */
    private val _channel = MutableStateFlow<String?>(null)
    val channel: StateFlow<String?> = _channel.asStateFlow()

    /** [더보기]에서 연 화면. null = 목록. */
    private val _more = MutableStateFlow<MoreItem?>(null)
    val more: StateFlow<MoreItem?> = _more.asStateFlow()

    /** [무전] 안의 면 — 메뉴를 오가도 보던 면이 남는다. */
    private val _pttPane = MutableStateFlow(PttPane.CHANNELS)
    val pttPane: StateFlow<PttPane> = _pttPane.asStateFlow()
    fun setPttPane(p: PttPane) { _pttPane.value = p }

    /** [통화] 안의 면. */
    private val _callPane = MutableStateFlow(CallPane.CALLS)
    val callPane: StateFlow<CallPane> = _callPane.asStateFlow()
    fun setCallPane(p: CallPane) { _callPane.value = p }

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
    val admin: AdminViewModel? get() = bound()?.let { s -> _admin ?: AdminViewModel(s).also { _admin = it } }

    /** [관리] 메뉴의 점 배지 — 저장하지 않은 폼이 있다는 뜻이다(§4.5). 전환은 막지 않는다. */
    val adminDirty: Boolean get() = _admin?.dirty == true

    /** 잠금 발언 설정 — 설정 화면이 바꾼다. */
    val lockTalk: Boolean get() = session?.settingsSnapshot()?.lockTalk == true

    /** 세션 준비·교체 — 화면이 이걸 구독해야 Service 가 늦게 서도 대기 화면에 머물지 않는다(§F8). */
    val sessionFlow: StateFlow<DispatchSession?> = DispatchService.sessionFlow

    val state: StateFlow<SessionState>? get() = session?.state
    val error: StateFlow<String?>? get() = session?.error

    /** 하단 내비 — 규칙은 [onNav] 가 갖는다(두 칸 이상 떨어져 있어도 한 번에 간다). */
    fun show(s: AppScreen) = applyNav(nav().onNav(s))

    /**
     * 스와이프가 다른 장으로 넘어갔다 — 메뉴와 면을 **함께** 옮긴다([APP_PAGES], §6.3).
     *
     * 하나씩 옮기면 안 된다: 메뉴만 먼저 바꾸면 그 순간의 좌표가 «통화 메뉴 + 이전 면» 이 되어
     * 쪽 번호가 다시 계산되고 화면이 되튕긴다.
     *
     * **기억한 면은 덮어쓴다.** 밀어서 «통화›주소록» 에 닿았으면 그 다음에 하단 내비로 [통화] 를
     * 눌렀을 때도 주소록이어야 한다 — 본 곳이 곧 그 메뉴의 현재 자리다.
     */
    fun showPage(page: AppPage) {
        page.pttPane?.let { _pttPane.value = it }
        page.callPane?.let { _callPane.value = it }
        if (_screen.value != page.screen) _screen.value = page.screen
    }

    /**
     * [무전] 목록 → 채널 화면. `groupId` 는 채널 카드의 id(그룹 id 또는 세션 id)다.
     *
     * 포커스도 같이 옮긴다 — ④⑤(메시지·이벤트)가 포커스를 따라간다는 불변(§6.3)은 배치가 바뀌어도
     * 그대로다. 채널 화면의 세 면이 곧 그 포커스의 상세다.
     */
    fun openChannel(id: String) {
        if (id.isBlank()) return
        ptt?.setFocus(id)
        _channel.value = id
        _screen.value = AppScreen.PTT
    }

    fun closeChannel() { _channel.value = null }

    fun openMore(item: MoreItem) { _more.value = item }

    /** 뒤로가기 한 단계 — 규칙은 [onBack] 이 갖는다(가로채기 판정도 같은 함수를 쓴다). */
    fun back(): Boolean {
        val next = nav().onBack() ?: return false
        applyNav(next)
        return true
    }

    /** 지금 좌표 — 이동 규칙([onNav]·[onBack])에 넘길 값. */
    private fun nav() = NavState(
        screen = _screen.value, channel = _channel.value, more = _more.value,
        pttPane = _pttPane.value, callPane = _callPane.value)

    private fun applyNav(n: NavState) {
        _screen.value = n.screen
        _channel.value = n.channel
        _more.value = n.more
        _pttPane.value = n.pttPane
        _callPane.value = n.callPane
    }

    /**
     * 세션을 만드는 조작 뒤의 **자동 복귀**(dispatch_desktop_ui.md §3.4).
     *
     * 배너에서 전화를 받으면 관제 > 일반통화 로 돌아간다 — 보류·전달·DTMF·종료가 거기 있다.
     * 받자마자 [이력] 화면에 남아 있으면 끊을 방법이 없다.
     */
    fun goToCalls() {
        _channel.value = null
        _screen.value = AppScreen.CALLS
    }

    /**
     * 사람 메뉴가 고른 행동을 잇는다 — 데스크톱 `MainViewModel` 이 `PersonActionsViewModel` 의 이벤트를 잇는
     * 것과 같은 자리다(§6.2f).
     *
     * **왜 여기인가.** 행동마다 가는 화면이 다르다(무전·통화·그 안의 면). 메뉴를 띄운 패널은 다른 화면의
     * 상태를 모르므로, 전부를 아는 이 VM 이 잇는다.
     *
     * **세션을 만드는 조작은 관제로 돌아간다**(dispatch_desktop_ui.md §3.4) — 개인 통화를 걸어 놓고 [이력]
     * 화면에 남아 있으면 끊을 방법이 없다.
     */
    fun runPersonAction(action: PersonAction, number: String) {
        if (number.isBlank()) return
        val s = bound() ?: return
        when (action) {
            PersonAction.CALL -> {
                viewModelScope.launch { s.dial(number) }
                goToCalls()
            }
            PersonAction.PRIVATE_CALL -> {
                viewModelScope.launch { s.startPrivateCall(number) }
                _channel.value = null
                _screen.value = AppScreen.PTT
            }
            PersonAction.ADHOC_ADD -> {
                // 시트는 [무전] 화면이 소유하는 상태라 여기서 직접 못 연다 — 씨앗만 심고 화면을 옮긴다.
                ptt?.seedAdhoc(number)
                _channel.value = null
                _screen.value = AppScreen.PTT
            }
            PersonAction.SDS -> {
                messages?.openThread(number)
                _channel.value = null
                _pttPane.value = PttPane.MESSAGES
                _screen.value = AppScreen.PTT
            }
            PersonAction.SMS -> {
                sms?.openTo(number)
                _callPane.value = CallPane.MESSAGES
                _screen.value = AppScreen.CALLS
            }
            // 통화 기록 = «통화내역» 면을 그 사람으로 걸러 연다. 최상위 [이력] 이 아닌 이유는 그쪽이
            //   날짜를 골라 보는 과거 조회라 «이 사람» 축이 없기 때문이다(§6.11).
            PersonAction.HISTORY -> {
                calls?.setPersonFilter(number)
                _callPane.value = CallPane.LOG
                _screen.value = AppScreen.CALLS
            }
        }
    }

    /** 통합 검색의 «채널로» — 그 채널 화면을 연다(데스크톱 `PttChannels.FocusGroup`). */
    fun focusChannel(groupId: String) = openChannel(groupId)

    /**
     * «편성 전원 보기» — 그 그룹의 [PTT 그룹] 화면 상세를 연다(§6.12).
     *
     * 채널 화면의 [로스터] 면은 **지금 접속한 사람**이고, 이쪽은 **편성된 전원 × 지금 상태**다. 둘은 다른
     * 질문이라 둘 다 둔다. 데스크톱도 같은 자리에서 같은 곳으로 보낸다(`PttChannelsPanel.xaml`).
     */
    fun showRoster(groupId: String) {
        if (groupId.isBlank()) return
        pttGroups?.selectById(groupId)
        _more.value = MoreItem.PTT_GROUPS
        _screen.value = AppScreen.MORE
    }

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
        viewModelScope.launch { s.refreshGroups() }
    }

    /** 하드 키보드 Ctrl+n — n 번째 채널로 포커스. */
    fun focusChannel(n: Int) { ptt?.focusIndex(n) }

    fun login(host: String, port: Int, id: String, pw: String, onDone: (String?) -> Unit) {
        val s = session ?: return onDone("세션이 아직 준비되지 않았습니다")
        _busy.value = true
        viewModelScope.launch {
            val r = s.login(host, port, id, pw)
            if (r.ok) {
                val started = s.start()
                onDone(if (started.ok) null else started.reason)   // 그룹·구독은 start() 가 한다(§F7)
            } else onDone(r.reason)
            _busy.value = false
        }
    }

    fun logout() { session?.logout() }
}
