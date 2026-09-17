// 관제 세션 — 코어 투영 + 관제 동작 진입점 (docs/design/features/android_dispatch_tablet.md §6.1·§6.7)
//
// Windows 의 `windows/dispatch-desktop/Services/DispatchSession.cs` 에 대응한다. 엔진·CSC 를 소유하고
// 등록·프로파일·세션 목록을 StateFlow 로 낸다.
//
// **수명은 Service 가 갖는다.** Activity 가 재생성돼도(회전·구성 변경) 등록과 세션이 끊기지 않아야 하기
// 때문이다. 프로세스가 회수되면 이 객체와 코어 상태가 함께 사라지므로 이전 호는 복원 대상이 아니고,
// 저장된 자격으로 **재로그인 → 프로파일 → 등록 → 재구독**까지만 되돌린다(§6.1).
//
// **UI 는 코어 상태의 투영이다.** 여기서 별도 상태 기계를 만들지 않는다 — 화면은 이 Flow 와
// `ue.callInfo()` 스냅샷에서 파생한다.
package com.cims.ue.dispatch.session

import android.content.Context
import com.cims.ue.sdk.AccountConfig
import com.cims.ue.sdk.Account
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.CimsUe
import com.cims.ue.sdk.CscClient
import com.cims.ue.sdk.CscEndpoint
import com.cims.ue.sdk.DispatchProfile
import com.cims.ue.sdk.EngineConfig
import com.cims.ue.sdk.FloorInfo
import com.cims.ue.sdk.Profile
import com.cims.ue.sdk.RegInfo
import com.cims.ue.sdk.RegState
import com.cims.ue.sdk.ServiceProfile
import com.cims.ue.sdk.TokenSet
import com.cims.ue.sdk.TrustAnchors
import com.cims.ue.sdk.platform.AudioRouter
import com.cims.ue.sdk.platform.SecureStore
import kotlinx.coroutines.CoroutineScope
import com.cims.ue.sdk.CallState
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** 계정 갈래 — 전화(volte/voip)와 PTT 는 쓰임이 달라 구분해 들고 있는다. */
enum class AccountKind { PHONE, PTT }

/** 앱이 보는 기동 단계. */
enum class SessionState { LOGGED_OUT, LOGGING_IN, PROFILE_READY, STARTING, READY, FAILED }

class DispatchSession(
    private val context: Context,
    private val scope: CoroutineScope,
    private val settings: SettingsStore,
) : AutoCloseable {

    private var ue: CimsUe? = null
    private var csc: CscClient? = null
    private var tokens: TokenSet? = null

    /**
     * access token 만료 시각(epoch ms). CSC 기본 TTL 은 3600초다(`csc/src/services/mcptt.py`
     * `ACCESS_TOKEN_TTL`). 관제석은 며칠씩 떠 있으므로 **갱신하지 않으면 한 시간 뒤 관리·이력·그룹
     * API 가 전부 401** 이 된다(SIP 등록은 H(A1) 이라 영향 없다 — 그래서 겉보기에 멀쩡하다).
     */
    private var tokenExpiresAtMs: Long = 0L

    /** 갱신은 한 번에 하나만 — 여러 화면이 동시에 만료를 만나도 refresh 가 한 번만 나간다. */
    private val tokenMutex = kotlinx.coroutines.sync.Mutex()

    /**
     * **자격 갱신이 실패하고 있다** — 연속 실패 횟수와 마지막 사유.
     *
     * 0 이 아니면 CSC 쪽 기능(이력·관리·PTT 그룹·주소록·녹취)이 곧 막힌다. 조회를 누른 **그 순간에야**
     * 튕기지 않도록 미리 띠로 알린다. 통화·무전은 H(A1) 이라 멀쩡해서 아무 표시가 없으면 관제사는
     * 자격이 죽어 가는 줄 모른다.
     */
    private val _credentialWarning = MutableStateFlow<String?>(null)
    val credentialWarning: StateFlow<String?> = _credentialWarning.asStateFlow()
    private var refreshFailures = 0

    private fun noteTokens(t: TokenSet?) {
        tokens = t
        // 만료 60초 전을 만료로 본다 — 왕복 지연 중에 만료되는 것을 피한다.
        tokenExpiresAtMs =
            if (t == null || t.expiresInSec <= 0) 0L
            else System.currentTimeMillis() + (t.expiresInSec - 60).coerceAtLeast(30) * 1000L
    }

    /**
     * 지금 쓸 수 있는 access token — 만료가 가까우면 refresh 한 뒤 돌려준다.
     *
     * 관리 API 는 이것을 통해서만 토큰을 얻는다. 갱신에 실패하면 옛 토큰을 그대로 돌려준다 —
     * 서버가 401 로 판정하게 두는 편이, 앱이 «만료됐을 것» 이라 추측해 조용히 막는 것보다 낫다.
     */
    /**
     * @param force 지역 만료 시각과 **무관하게** 갱신한다.
     *
     * 401 복구에 쓴다. 지역 시계만 믿으면 서버가 토큰을 버린 경우(CSC `configure` 재실행으로
     * `IdMs.JwtSecret` 재생성 — 그 설정의 help 가 "재생성 시 발급된 모든 토큰 무효화" 라고 적고 있다,
     * 서버 시계 차이, 토큰 폐기)를 못 넘는다. 그때는 «아직 안 만료됐다» 고 판단해 낡은 토큰을 계속 보내고
     * 사용자는 «다시 로그인하세요» 만 본다 — refresh token 은 서버가 **저장한 기록**으로 검증하므로
     * (`mcptt.py` refresh 핸들러가 JWT 서명이 아니라 `storage.get_refresh_token` 을 본다) 시크릿이
     * 바뀌어도 갱신은 성립한다. 즉 되살릴 수 있는데 시도하지 않고 있었다.
     */
    private suspend fun validAccessToken(force: Boolean = false): String? {
        val cur = tokens ?: return null
        if (!force && (tokenExpiresAtMs == 0L || System.currentTimeMillis() < tokenExpiresAtMs))
            return cur.accessToken
        tokenMutex.withLock {
            // 기다리는 동안 다른 호출이 이미 갱신했을 수 있다 — 그때는 그 결과를 쓴다.
            val now = tokens ?: return null
            val renewed = now.accessToken != cur.accessToken
            if (renewed) return now.accessToken
            if (!force && tokenExpiresAtMs != 0L && System.currentTimeMillis() < tokenExpiresAtMs)
                return now.accessToken
            val gen = epoch
            val rt = now.refreshToken.ifEmpty { store.get(SecureStore.KEY_REFRESH_TOKEN).orEmpty() }
            if (rt.isEmpty()) return now.accessToken
            val c = csc ?: return now.accessToken
            val r = c.refresh(rt)
            if (gen != epoch) return null                       // 갱신 중 로그아웃 — 게시하지 않는다
            if (!r.ok) {
                // **세션이 끝난 것과 일시적 장애를 가른다.**
                //   · invalid_grant(400) = 서버가 이 자격을 버렸다(폐기·회전 실패·refresh 만료).
                //     되살릴 방법이 없으므로 **앱 전체를 로그아웃**한다 — «조회만 안 되는 반쯤 로그인»
                //     상태를 두지 않는다. 관제사에게 로그인은 하나다.
                //   · 그 밖(네트워크·5xx)은 일시적이라 옛 토큰을 그대로 쓰고 띠로만 알린다.
                if (isSessionEnded(r.code, r.reason)) {
                    endSession("로그인 자격이 만료됐습니다 — 다시 로그인하세요")
                    return null
                }
                refreshFailures++
                _credentialWarning.value =
                    "자격 갱신 실패 ${refreshFailures}회 — 이력·관리·PTT 그룹이 곧 막힙니다 (${r.reason.take(60)})"
                return now.accessToken
            }
            refreshFailures = 0
            _credentialWarning.value = null
            noteTokens(r.value)
            r.value?.refreshToken?.takeIf { it.isNotEmpty() }
                ?.let { store.put(SecureStore.KEY_REFRESH_TOKEN, it) }   // 회전한 refresh token 도 저장
            return r.value?.accessToken
        }
    }
    /** sipHa1 이 없는 계정의 평문 폴백 — 프로파일에 H(A1) 이 있으면 쓰이지 않는다. */
    private var loginPassword: String = ""

    private val store = SecureStore(context)

    /**
     * SDS 보관 — 앱을 껐다 켜도 스레드가 남는다(§6.9).
     *
     * 서버에 SDS 이력 API 가 없으므로 **이 로컬 보관이 유일한 근거**다. DB 작업은 IO 로 보낸다 —
     * 이벤트 처리는 Main 에서 도는데 거기서 디스크를 만지면 프레임이 밀린다.
     */
    private val messageStore = MessageStore(context)

    private fun storeAsync(block: MessageStore.() -> Unit) {
        scope.launch(Dispatchers.IO) { runCatching { messageStore.block() } }
    }
    val audio = AudioRouter(context)

    // ── 상태 ──────────────────────────────────────────────────────────────────
    private val _state = MutableStateFlow(SessionState.LOGGED_OUT)
    val state: StateFlow<SessionState> = _state.asStateFlow()

    private val _profile = MutableStateFlow<Profile?>(null)
    val profile: StateFlow<Profile?> = _profile.asStateFlow()

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error.asStateFlow()

    private val _accounts = MutableStateFlow<Map<AccountKind, Account>>(emptyMap())
    val accounts: StateFlow<Map<AccountKind, Account>> = _accounts.asStateFlow()

    // 엔진이 서기 전에도 화면이 구독할 수 있어야 하므로 고정 Flow 를 하나 두고 엔진 값을 흘려 넣는다.
    // (접근할 때마다 새 Flow 를 만들면 구독이 끊긴다.)
    private val _registrations = MutableStateFlow<Map<Int, RegInfo>>(emptyMap())
    /** 계정별 등록 상태 — 화면 점등의 소스(권위는 코어 스냅샷). */
    val registrations: StateFlow<Map<Int, RegInfo>> = _registrations.asStateFlow()

    private val _sessions = MutableStateFlow<List<SessionItem>>(emptyList())
    /** 살아 있는 세션. 재구성·재시작 후에는 `refreshSessions()` 로 코어 스냅샷에서 다시 그린다. */
    val sessions: StateFlow<List<SessionItem>> = _sessions.asStateFlow()

    private val _groups = MutableStateFlow<List<GroupInfo>>(emptyList())
    /** PTT 그룹 — 멤버(①) + 청취 범위(②). */
    val groups: StateFlow<List<GroupInfo>> = _groups.asStateFlow()

    private val _activity = MutableStateFlow<List<ActivityRow>>(emptyList())
    /** ⑤ PTT 이벤트 링 버퍼 — 진행 중 행은 없다(최신이 앞). */
    val activity: StateFlow<List<ActivityRow>> = _activity.asStateFlow()

    private val _messages = MutableStateFlow<Map<String, List<Message>>>(emptyMap())
    /** ④ PTT 메시지 — 그룹 id 별 스레드(시간 오름차순). */
    val messages: StateFlow<Map<String, List<Message>>> = _messages.asStateFlow()

    private val _dialogs = MutableStateFlow<List<DialogRow>>(emptyList())
    /** 감시 중인 dialog(RFC 4235) — ③ 그룹원 띠·대표번호 대기열의 소스. */
    val dialogs: StateFlow<List<DialogRow>> = _dialogs.asStateFlow()

    private val _callLog = MutableStateFlow<List<CallLogRow>>(emptyList())
    /** ⑥ 통화 내역 — 최신이 앞. */
    val callLog: StateFlow<List<CallLogRow>> = _callLog.asStateFlow()

    /**
     * **착신 스택** — 울리고 있는 전화. 최신이 위다(dispatch_desktop_ui.md §3.2 착신 배너).
     *
     * 화면 어디에 있든 보여야 하므로 세션이 든다. 이것이 없으면 착신 표면이 [일반통화] 탭의 카드
     * 하나뿐이라, 다른 탭·화면에 있는 동안 걸려 온 전화를 **아무도 못 본다**.
     */
    val incoming: StateFlow<List<SessionItem>> =
        _sessions.map { list ->
            // 사설콜도 같은 배너를 쓴다(§3.2 청록). 지금은 코어가 MCPTT 를 자동 수락해 거의 뜨지 않지만,
            // 자동 수락 플래그가 갈라지면(§11) 조건 하나 없이 그대로 동작한다.
            list.filter {
                (it.kind == SessionKind.PHONE_CALL || it.kind == SessionKind.PTT_PRIVATE) &&
                    it.info.state == CallState.INCOMING
            }
                .sortedByDescending { it.startedAtMs }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /**
     * **전화 주소록** — 전화 가족(이동 volte ∪ 유선 voip). 발신 대상은 여기서만 고른다.
     *
     * PTT 주소록과 따로 든다: PTT 번호는 MCPTT 신원이라 전화 계정으로 걸면 닿지 않는다. 섞어 두면
     * 주소록에서 고른 사람에게 전화가 안 걸린다.
     */
    private val _phoneBook = MutableStateFlow(DirectoryBook())
    val phoneBook: StateFlow<DirectoryBook> = _phoneBook.asStateFlow()

    /** **PTT 주소록** — 사설콜·그룹 멤버 후보. 전화 발신에는 쓰지 않는다. */
    private val _pttBook = MutableStateFlow(DirectoryBook())
    val pttBook: StateFlow<DirectoryBook> = _pttBook.asStateFlow()

    /** 정규형 번호 → 이름. 조회가 잦아 목록을 매번 훑지 않는다. */
    private var nameIndex: Map<String, String> = emptyMap()

    private val _tally = MutableStateFlow(DeskTally())
    /** 오늘 데스크 — ③ 상단 칩. */
    val tally: StateFlow<DeskTally> = _tally.asStateFlow()

    /** 지금 dialog 를 구독 중인 AoR 집합. */
    /**
     * dialog 를 구독 중인 AoR — 대표번호 + 관제 범위의 감시 대상(`dispatch.members[]`).
     *
     * **관측 가능해야 한다.** 이 집합이 비면 ⑥ 진행 중 행이 영원히 비는데, 화면에 아무 말이 없으면
     * «앱이 고장났다» 와 «편성이 안 됐다» 를 구분할 수 없다.
     */
    private val _watched = MutableStateFlow<Set<String>>(emptySet())
    val watchedAors: StateFlow<Set<String>> = _watched.asStateFlow()

    /**
     * dialog NOTIFY 를 **실제로 받은** AoR(번호부 기준).
     *
     * `dialogWatch` 는 SUBSCRIBE 를 **보낸 것**만 성공으로 돌려준다(코어 `Engine::dialogWatch` — 최종
     * 응답은 `onRequestResult` 로 따로 온다). 그래서 «구독을 걸었다» 는 «구독이 성립했다» 가 아니다.
     * RFC 6665 상 구독이 성립하면 즉시 NOTIFY 가 오므로, **NOTIFY 를 받은 수**가 성립한 구독 수다.
     */
    private val _notified = MutableStateFlow<Set<String>>(emptySet())
    val notifiedAors: StateFlow<Set<String>> = _notified.asStateFlow()

    /** 마지막 SUBSCRIBE 실패 — 조용히 삼키면 «구독했는데 아무것도 안 온다» 의 원인을 못 찾는다. */
    private val _subscribeError = MutableStateFlow("")
    val subscribeError: StateFlow<String> = _subscribeError.asStateFlow()

    /**
     * **dialog 를 실제로 실어 온** NOTIFY 를 받은 AoR.
     *
     * 구독 성립([notifiedAors])과 갈라 둔다. 구독은 섰는데 이쪽이 계속 비면 서버가 **초기 NOTIFY 만
     * 보내고 상태 변화를 안 보내는** 것이다 — 인가 거절과 전혀 다른 문제라 섞으면 못 가린다.
     */
    private val _dialogSeen = MutableStateFlow<Set<String>>(emptySet())
    val dialogSeenAors: StateFlow<Set<String>> = _dialogSeen.asStateFlow()

    internal fun noteDialogNotify(watchedAor: String, carriedDialog: Boolean) {
        val id = userPart(watchedAor)
        if (id.isEmpty()) return
        if (id !in _notified.value) _notified.value = _notified.value + id
        if (carriedDialog && id !in _dialogSeen.value) _dialogSeen.value = _dialogSeen.value + id
    }

    private var watched: Set<String>
        get() = _watched.value
        set(v) { _watched.value = v }

    /** 세션을 만든 관제 동작 — 종료 문구 선택에 쓴다. */
    private val operations = mutableMapOf<Int, Operation>()

    /**
     * 로그인 세대. 로그아웃하면 올라간다 — **진행 중이던 기동이 뒤늦게 상태를 게시하는 것을 막는다**.
     *
     * 수동 로그인은 화면 수명(viewModelScope)에서 돌고 로그아웃은 Service 수명에서 오므로, 계정 추가
     * 중에 로그아웃하면 이후 기동이 계속되어 계정과 READY 를 다시 게시할 수 있다(§6.1 위반).
     */
    private var epoch: Int = 0

    /**
     * **로그인 세대** — 로그아웃할 때마다 오른다.
     *
     * 세션 **객체**는 Service 수명이라 로그아웃해도 그대로다. 화면 VM 이 객체 교체만 보고 정리하면,
     * 다음 사람이 로그인했을 때 **앞 사람이 조회한 관리 목록·이력 결과·편집 폼이 그대로 남는다** —
     * 범위가 좁은 계정에 앞 계정의 정보가 보인다. 화면은 이 값을 관측해 캐시를 버린다.
     */
    private val _loginGeneration = MutableStateFlow(0)
    val loginGeneration: StateFlow<Int> = _loginGeneration.asStateFlow()

    /** 관제 데스크 — 없으면 소프트폰 모드(§6.2). */
    val dispatch: DispatchProfile get() = _profile.value?.dispatch ?: DispatchProfile.NONE
    val hasDesk: Boolean get() = dispatch.present
    val phoneAccount: Account? get() = _accounts.value[AccountKind.PHONE]
    val pttAccount: Account? get() = _accounts.value[AccountKind.PTT]
    val isReady: Boolean get() = _state.value == SessionState.READY

    /** 화면이 읽는 설정 스냅샷. 관측이 필요하면 [settingsFlow] 를 쓴다. */
    fun settingsSnapshot(): Settings = settings.current

    /** 설정 — 관측 가능. 설정 화면이 읽고 쓰며, 잠금 발언·자동 보류를 다른 화면이 즉시 따른다. */
    val settingsFlow: kotlinx.coroutines.flow.StateFlow<Settings> = settings.flow

    /**
     * 설정 변경. 오디오 경로처럼 **즉시 반영해야 하는 것**은 여기서 다시 적용한다 —
     * 저장만 하고 끝내면 다음 통화부터 바뀌어 «눌렀는데 아무 일도 없다» 가 된다.
     */
    fun updateSettings(f: (Settings) -> Settings) {
        val before = settings.current
        settings.update(f)
        val after = settings.current
        if (after.audioRoute != before.audioRoute) runCatching { applyAudio() }
    }

    /** 저장된 자격이 있는가 — 부팅 재등록·자동 로그인의 조건. */
    val hasSavedLogin: Boolean get() = store.contains(SecureStore.KEY_REFRESH_TOKEN)

    // ── 로그인 ────────────────────────────────────────────────────────────────
    /** IdMS PKCE 로그인 → `/provisioning/me`. 성공하면 자격을 저장한다. */
    suspend fun login(host: String, port: Int, loginId: String, password: String): CimsResult<Unit> {
        _state.value = SessionState.LOGGING_IN
        val client = makeCsc(host, port)
        val tok = client.login(loginId, password)
        if (!tok.ok) return fail(tok.code, tok.reason)
        noteTokens(tok.value)
        loginPassword = password
        settings.update { it.copy(cscHost = host, cscPort = port, loginId = loginId) }
        tok.value?.refreshToken?.takeIf { it.isNotEmpty() }?.let {
            store.put(SecureStore.KEY_REFRESH_TOKEN, it)
            store.put(SecureStore.KEY_LOGIN_ID, loginId)
            store.put(SecureStore.KEY_CSC_HOST, "$host:$port")
        }
        return fetchProfile()
    }

    /**
     * 저장된 refresh token 으로 재로그인 — 프로세스 회수 뒤 복귀 경로(§6.1).
     * 평문 비밀번호는 남기지 않으므로, H(A1) 이 없는 프로파일은 이 경로로 등록하지 못한다.
     */
    suspend fun resume(): CimsResult<Unit> {
        val rt = store.get(SecureStore.KEY_REFRESH_TOKEN)
            ?: return fail(-1, "저장된 로그인 없음")
        val s = settings.current
        _state.value = SessionState.LOGGING_IN
        val client = makeCsc(s.cscHost, s.cscPort)
        val tok = client.refresh(rt)
        if (!tok.ok) {
            store.remove(SecureStore.KEY_REFRESH_TOKEN)
            return fail(tok.code, tok.reason)
        }
        noteTokens(tok.value)
        tok.value?.refreshToken?.takeIf { it.isNotEmpty() }?.let { store.put(SecureStore.KEY_REFRESH_TOKEN, it) }
        return fetchProfile()
    }

    private suspend fun fetchProfile(): CimsResult<Unit> {
        val c = csc ?: return fail(-1, "로그인 전")
        val t = tokens ?: return fail(-1, "로그인 전")
        val p = c.fetchProfile(t.accessToken)
        if (!p.ok) return fail(p.code, p.reason)
        _profile.value = p.value
        _state.value = SessionState.PROFILE_READY
        _error.value = null
        return CimsResult.ok(Unit)
    }

    // ── 기동 ──────────────────────────────────────────────────────────────────
    /**
     * 엔진 기동 → 계정 추가·등록.
     *
     * 계정으로 올리는 서비스 = **PTT 전부 + 전화 계열은 하나**(`Profile.phoneService` — 유선 voip 우선,
     * 없으면 이동 volte). 둘 다 등록하면 이동 번호까지 관제석에 바인딩돼 착신이 이 앱으로 포크되고
     * 전화 계정 참조를 마지막 계정이 덮어쓴다.
     */
    suspend fun start(): CimsResult<Unit> {
        val p = _profile.value ?: return fail(-1, "프로파일 없음")
        val gen = epoch
        _state.value = SessionState.STARTING
        val s = settings.current

        val engine = ue ?: CimsUe().also { ue = it }
        if (!engine.running.value) {
            val r = engine.start(EngineConfig(
                userAgent = USER_AGENT,
                logLevel = s.logLevel,
                tlsCaPem = trustAnchors(),
                tlsVerifyServer = s.verifyServer))
            if (!r.ok) return fail(r.code, r.reason)
        }
        applyAudio()

        val toRegister = buildList {
            p.phoneService?.let { add(it to AccountKind.PHONE) }
            p.services.filter { it.kind == "ptt" }.forEach { add(it to AccountKind.PTT) }
        }
        // **기동 작업이 자기가 만든 계정을 소유한다.** `logout()` 은 게시된 `_accounts` 만 보므로,
        // 게시 전에 로그아웃이 끼어들면 이미 등록된 계정이 정리 대상에서 빠진다 — 화면은 로그아웃인데
        // SIP 등록과 자격이 엔진에 살아남는다. 소유권을 여기 두고 어느 경로로 빠져나가든 정리한다.
        val added = mutableMapOf<AccountKind, Account>()
        var published = false
        try {
            for ((sp, kind) in toRegister) {
                if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
                val cfg = accountConfig(sp, p.displayName, kind)
                val a = engine.addAccount(cfg)
                if (!a.ok) { _error.value = "${sp.kind} 계정 추가 실패: ${a.reason}"; continue }
                added[kind] = a.value!!                        // 등록을 걸기 **전에** 소유로 잡는다
                val reg = a.value!!.register()
                if (!reg.ok) _error.value = "${sp.kind} 등록 요청 실패: ${reg.reason}"
            }
            if (added.isEmpty()) return fail(-1, "등록할 계정이 없다")
            // 게시는 세대 확인 **뒤**에만 — 먼저 게시하면 로그아웃이 비운 것을 되살린다.
            if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
            _accounts.value = added
            published = true
        } finally {
            // 게시하지 못하고 빠져나갔다면(로그아웃·예외) 만든 계정을 전부 되돌린다.
            if (!published && added.isNotEmpty()) {
                val orphans = added.values.toList()
                scope.launch {
                    orphans.forEach { a ->
                        runCatching { a.unregister() }
                        runCatching { a.remove() }
                    }
                }
            }
        }

        observe()
        if (hasDesk) watchAll()                    // 대표번호 + 감시 대상 dialog 구독(§6.7 ③)
        refreshGroups()                            // PTT 그룹·affiliation·conference 구독(§6.7 ①②)
        refreshDirectory()                         // 번호 → 이름. 실패해도 진행한다
        restoreMessages()                          // 보관된 SDS 스레드(§6.9)
        refreshSessions()
        if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
        _state.value = SessionState.READY
        return CimsResult.ok(Unit)
    }

    /**
     * 프로파일의 접속서비스 하나 → 계정 설정.
     * MCPTT 자동 수락은 PTT 계정만 — 관제석은 그룹콜 자동·사설콜 수동이 맞지만 코어 플래그가 아직
     * 공통이라(§11) 우선 PTT 전체를 자동으로 둔다.
     */
    private fun accountConfig(sp: ServiceProfile, displayName: String, kind: AccountKind): AccountConfig =
        sp.toAccountConfig(loginPw = loginPassword).copy(
            displayName = displayName,
            autoAnswerMcptt = kind == AccountKind.PTT)

    /** 화면 재구성·프로세스 복귀 후 세션을 코어 스냅샷에서 다시 그린다(§6.7). */
    fun refreshSessions() {
        val engine = ue ?: return
        // **`calls()` 는 끝난 호도 준다** — 코어가 조회·최종 통계용으로 64건을 보존한다
        // (`engine.cpp` `callInfos` · `pruneFinished`). 거르지 않으면 화면 복귀마다 끝난 통화가
        // «내 통화» 로 되살아난다.
        val live = engine.calls().mapNotNull { engine.callInfo(it) }
            .filter { it.state != CallState.DISCONNECTED && it.state != CallState.NULL }
        val prev = _sessions.value.associateBy { it.callId }
        _sessions.value = live.map { ci ->
            prev[ci.callId]?.copy(info = ci) ?: newSession(ci)
        }
        // floor 는 스냅샷 조회가 제어 스레드를 타므로 따로 당긴다(§F3) — 화면 복귀 때 발언 표시가 살아난다.
        live.filter { it.isMcptt }.forEach { pullFloor(it.callId) }
    }

    private fun newSession(ci: CallInfo): SessionItem = SessionItem(
        callId = ci.callId,
        account = if (ci.isMcptt) AccountKind.PTT else AccountKind.PHONE,
        operation = operations.remove(ci.callId)
            ?: if (ci.dir == com.cims.ue.sdk.CallDir.INCOMING) Operation.INCOMING else Operation.DIAL,
        info = ci,
        connectedAtMs = if (ci.state == com.cims.ue.sdk.CallState.ACTIVE) System.currentTimeMillis() else null,
        title = titleOf(ci))

    private fun titleOf(ci: CallInfo): String = when {
        ci.isMcptt && ci.groupId.isNotEmpty() -> groupNameOf(ci.groupId)
        else -> displayName(ci.remoteUri)
    }

    // ── PttPlane 이 쓰는 접근자 ──────────────────────────────────────────────
    /** 세션 수명의 코루틴 — 이벤트 처리 중 명령을 걸 때 쓴다(이벤트 콜백은 suspend 가 아니다). */
    internal fun scopeLaunch(block: suspend () -> Unit) { scope.launch { block() } }

    internal fun engineOrNull(): CimsUe? = ue
    internal fun cscOrNull(): CscClient? = csc

    /** 녹취 임시 파일 자리 — 캐시라 OS 가 지워도 다시 받으면 된다(§6.5). */
    val cacheDir: java.io.File get() = context.cacheDir
    /**
     * **유효한** access token. 만료가 가까우면 갱신한 뒤 준다.
     *
     * 예전 이름(`accessTokenOrNull`)은 «지금 들고 있는 값» 이라는 뜻이라 만료를 다루지 않았다 —
     * CSC 직접 호출(GMS 그룹 목록·문서 PUT/DELETE)이 한 시간 뒤 401 로 죽던 경로다.
     */
    internal suspend fun accessToken(force: Boolean = false): String? = validAccessToken(force)

    /** 401 을 받은 호출자가 한 번 되살려 본다. */
    internal suspend fun renewAccessToken(): String? = validAccessToken(force = true)

    /**
     * 세션이 **끝난** 실패인가 — 되살릴 수 없는 것만 참이다.
     *
     * CSC 는 폐기·만료·회전 실패를 전부 `400 {"error":"invalid_grant"}` 로 낸다
     * (`csc/src/services/mcptt.py` refresh 핸들러). 401 도 자격 거부다. 네트워크 오류(음수 코드)와
     * 5xx 는 서버가 잠깐 아픈 것이라 **로그아웃하면 안 된다** — 관제석을 네트워크 흔들림으로 튕기는
     * 것이 만료로 튕기는 것보다 나쁘다.
     */
    internal fun isSessionEnded(code: Int, reason: String): Boolean =
        code == 401 || (code == 400 && reason.contains("invalid_grant"))

    /**
     * 되살릴 수 없는 자격 만료 — **앱 전체를 로그아웃한다.**
     *
     * SIP 등록은 H(A1) 이라 CSC 토큰과 무관하게 살아 있지만(그래서 통화는 되고 조회만 막힌다),
     * 관제사에게 로그인은 **하나**다. 반쯤 로그인된 상태를 두면 무엇이 되고 무엇이 안 되는지 알 수 없다.
     * 등록까지 내리고 로그인 화면으로 보낸 뒤, 왜 그랬는지 남긴다.
     */
    private fun endSession(why: String) {
        logout()
        _error.value = why
    }

    private var mgmt: Pair<CscClient, ManagementClient>? = null

    /**
     * 관리 평면 접점(§6.4) — 조직·구성원·이력·녹취.
     *
     * 현재 CSC 연결에 묶인다. 재로그인으로 연결이 바뀌면 낡은 접점을 버리고 새로 만든다 —
     * 닫힌 클라이언트를 들고 있으면 조용히 실패한다.
     */
    fun management(): ManagementClient? {
        val c = csc ?: return null
        mgmt?.let { if (it.first === c) return it.second }
        return ManagementClient(c, { validAccessToken() }, { validAccessToken(force = true) })
            .also { mgmt = c to it }
    }
    /**
     * floor 스냅샷을 당겨 세션에 채운다.
     *
     * 코어의 `floorInfo()` 는 제어 스레드를 기다리므로(`runSync`) **이벤트 핸들러에서 바로 부를 수 없다**.
     * 그래서 이벤트를 받은 뒤 별도 코루틴으로 당긴다. 이것이 필요한 이유는 이벤트만으로 부족하기
     * 때문이다 — 코어의 `request`/`release` 는 이벤트 없이 상태를 바꾸는 구간이 있다
     * (sdk/core/src/floor/floor_participant.cpp). 화면 복귀(refreshSessions)에서도 같이 채운다.
     */
    internal fun pullFloor(callId: Int) {
        val engine = ue ?: return
        scope.launch {
            val fi = engine.floorInfo(callId) ?: return@launch
            updateSession(callId) { it.copy(floor = fi) }
        }
    }

    /** PTT 서비스의 MC service ID — GMS 조회 키(`tel:`). */
    val myPttId: String
        get() = _profile.value?.pttService?.let { it.mcpttId.ifBlank { "tel:" + it.msisdn } } ?: ""

    /**
     * 내 회선의 비교 정규형 집합 — 사람 목록에서 나를 빼는 데 쓴다.
     *
     * 전화 계열과 PTT 를 모두 넣는다. 한쪽만 빼면 다른 축의 주소록에서 «나» 가 남아, 자기에게 사설콜을
     * 거는 항목이 목록에 보인다.
     */
    fun myLineKeys(): Set<String> {
        val p = _profile.value ?: return emptySet()
        return listOfNotNull(p.phoneService?.msisdn, p.pttService?.msisdn)
            .filter { it.isNotBlank() }
            .map { DirectoryBook.normalize(it) }
            .toSet()
    }

    /**
     * 지금 열려 있는 청취 — 통화 감청(Join)과 PTT 청취를 **합쳐** 센다.
     *
     * 둘을 합치는 이유는 상한의 근거가 자원이기 때문이다 — 서버에서는 어느 쪽이든 청취 leg 하나다.
     */
    val listenCount: Int
        get() = _sessions.value.count { it.isLive && it.kind.isSheet }

    /** 동시 청취 상한에 걸렸나 — 새로 열기 전에 본다. */
    fun listenLimitReached(): Boolean = listenCount >= settingsSnapshot().maxListen

    /** 청취 범위가 있는가 — 없으면 ② 청취 섹션이 비활성된다. */
    val canListenPtt: Boolean get() = dispatch.pttListen != "none" && dispatch.pttTargets.isNotEmpty()

    internal fun setGroups(next: List<GroupInfo>) { _groups.value = next }

    internal fun updateGroup(id: String, f: (GroupInfo) -> GroupInfo) {
        _groups.value = _groups.value.map { if (it.id == id) f(it) else it }
    }

    internal fun sessionOf(callId: Int): SessionItem? = _sessions.value.firstOrNull { it.callId == callId }

    internal fun updateSession(callId: Int, f: (SessionItem) -> SessionItem) {
        _sessions.value = _sessions.value.map { if (it.callId == callId) f(it) else it }
    }

    internal fun upsertSession(ci: CallInfo) {
        val cur = _sessions.value
        _sessions.value =
            if (cur.any { it.callId == ci.callId })
                cur.map { if (it.callId == ci.callId) it.copy(
                    info = ci,
                    connectedAtMs = it.connectedAtMs
                        ?: if (ci.state == com.cims.ue.sdk.CallState.ACTIVE) System.currentTimeMillis() else null,
                    title = it.title.ifEmpty { titleOf(ci) }) else it }
            else cur + newSession(ci)
    }

    internal fun removeSession(callId: Int) {
        _sessions.value = _sessions.value.filterNot { it.callId == callId }
        adhocMembers.remove(callId)
    }

    internal fun noteOperation(callId: Int, op: Operation) { operations[callId] = op }

    /**
     * 애드혹 참가자 — **앱이 기억한다**.
     *
     * 임시 그룹이라 서버에 편성이 없고 로스터 구독 대상도 아니다. 카드에 «3명» 을 적으려면 개설할 때
     * 실어 보낸 목록밖에 근거가 없다. 호가 끝나면 지운다.
     */
    private val adhocMembers = mutableMapOf<Int, List<String>>()

    internal fun rememberAdhocMembers(callId: Int, members: List<String>) {
        adhocMembers[callId] = members
        updateSession(callId) { it.copy(adhocMembers = members) }
    }

    internal fun adhocMembersOf(callId: Int): List<String> = adhocMembers[callId].orEmpty()

    // ── PhonePlane 이 쓰는 접근자 ────────────────────────────────────────────
    internal fun setDialogs(next: List<DialogRow>) { _dialogs.value = next }
    internal fun setWatched(next: Set<String>) {
        watched = next
        _notified.value = emptySet()      // 다시 걸면 성립도 다시 센다
        _dialogSeen.value = emptySet()
        _subscribeError.value = ""
    }

    /** 내 회선인가 — 내가 당사자면 데스크 집계에 넣고, 감시 대상이면 뺀다. */
    internal fun isMine(aor: String): Boolean {
        val me = _profile.value?.phoneService?.msisdn ?: return false
        return userPart(aor) == userPart(me)
    }

    /** 내 대표번호인가 — 대기열 판정. */
    fun isPilot(aor: String): Boolean {
        val p = dispatch.pilotId
        return p.isNotEmpty() && userPart(aor) == userPart(p)
    }

    internal fun addCallLog(row: CallLogRow) {
        _callLog.value = (listOf(row) + _callLog.value).take(CALL_LOG_LIMIT)
        if (!row.others) _tally.value = _tally.value.let { t ->
            when (row.kind) {
                CallLogKind.ANSWERED, CallLogKind.PICKUP -> t.copy(answered = t.answered + 1)
                CallLogKind.MISSED -> t.copy(missed = t.missed + 1)
                CallLogKind.OUTGOING -> t.copy(outgoing = t.outgoing + 1)
                CallLogKind.TRANSFER -> t.copy(transfer = t.transfer + 1)
                CallLogKind.MONITOR -> t.copy(monitor = t.monitor + 1)
            }
        }
    }

    internal fun addActivity(groupId: String, groupName: String, text: String,
                             kind: ActivityKind, emergency: Boolean = false) {
        val row = ActivityRow(System.currentTimeMillis(), groupId, groupName, text, kind, emergency)
        _activity.value = (listOf(row) + _activity.value).take(ACTIVITY_LIMIT)
    }

    internal fun groupNameOf(groupId: String): String =
        _groups.value.firstOrNull { it.id == groupId }?.name ?: groupId

    /** URI → 표시명. 전화번호부에 있으면 이름, 없으면 번호부(user part)를 그대로 쓴다. */
    internal fun displayName(uri: String): String {
        val num = userPart(uri).ifEmpty { return uri }
        return nameIndex[DirectoryBook.normalize(num, countryCode())] ?: num
    }

    /** "1003 이순경" 병기(dispatch_desktop_ui.md §3.2 신원 표시). 이름이 없으면 번호만. */
    fun displayLabel(uri: String): String {
        val num = userPart(uri).ifEmpty { return uri }
        val name = nameIndex[DirectoryBook.normalize(num, countryCode())]
        return if (name.isNullOrBlank()) num else "$num $name"
    }

    private fun countryCode(): String = _profile.value?.countryCode?.ifBlank { "82" } ?: "82"

    /**
     * 회사 전화번호부 적재 — 전화(voip·volte)와 PTT 를 합친다.
     *
     * 실패해도 조용히 넘긴다. 이름이 없으면 번호로 보일 뿐이고, 여기서 막으면 등록까지 막힌다.
     */
    suspend fun refreshDirectory() {
        val m = management() ?: return
        // **전화 가족 축은 `volte` 다** — 서버가 `service=volte` 에 이동(volte)과 유선(voip)을 합산해 준다
        // (csc `handle_provisioning_directory`). 내 회선이 유선이라고 `service=voip` 로 물으면 유선 가입자만
        // 와서 전화번호부가 거의 빈다. 관제 그룹원 `volteAor` 와도 같은 어휘다.
        m.directory("volte").let { r -> if (r.ok) r.value?.let { _phoneBook.value = it } }
        m.directory("ptt").let { r -> if (r.ok) r.value?.let { _pttBook.value = it } }
        reindexNames()
    }

    /**
     * 이름 색인 — 전화·PTT 를 **합쳐서** 만든다.
     *
     * 표시는 둘을 가릴 이유가 없다(같은 사람이다). 가르는 것은 «걸 수 있는 대상» 뿐이라 목록만 따로 둔다.
     */
    private fun reindexNames() {
        val cc = countryCode()
        val idx = HashMap<String, String>()
        (_phoneBook.value.entries + _pttBook.value.entries).forEach { e ->
            val key = DirectoryBook.normalize(e.msisdn, cc)
            if (key.isEmpty() || e.name.isBlank()) return@forEach
            idx.putIfAbsent(key, e.name)        // 먼저 온 이름이 이긴다 — 전화 주소록이 앞이다
        }
        nameIndex = idx
    }

    /**
     * 보관된 SDS 를 되살린다 — 기동 때 한 번.
     *
     * 잔존 PENDING 을 먼저 FAILED 로 닫는다(앱이 죽는 순간 보낸 것은 최종 응답을 받을 길이 없다),
     * 그다음 보관 기간을 넘긴 것을 지우고 적재한다.
     */
    private suspend fun restoreMessages() {
        val loaded = withContext(Dispatchers.IO) {
            runCatching {
                messageStore.failPending()
                messageStore.prune()
                messageStore.load()
            }.getOrElse { emptyMap() }
        }
        if (loaded.isEmpty()) return
        // 기동 중 이미 들어온 것이 있으면 그것이 최신이다 — 보관분 위에 얹는다.
        val live = _messages.value
        _messages.value = loaded.toMutableMap().apply {
            live.forEach { (g, msgs) ->
                val existing = this[g].orEmpty()
                val ids = existing.map { it.id }.toHashSet()
                this[g] = existing + msgs.filter { it.id !in ids }
            }
        }
    }

    internal fun addOutgoingMessage(groupId: String, text: String, msgId: String, token: Long) {
        val m = Message(
            id = "out-" + System.nanoTime(), groupId = groupId,
            fromUri = myPttId, fromName = "나", text = text,
            atMs = System.currentTimeMillis(), outgoing = true,
            msgId = msgId, token = token, state = SendState.PENDING)
        _messages.value = _messages.value + (groupId to ((_messages.value[groupId] ?: emptyList()) + m))
        storeAsync { insert(m) }
    }

    internal fun addIncomingMessage(groupId: String, msg: com.cims.ue.sdk.SdsMessage) {
        val m = Message(
            id = msg.msgId.ifEmpty { "in-" + System.nanoTime() }, groupId = groupId,
            fromUri = msg.fromUri, fromName = displayName(msg.fromUri), text = msg.text,
            atMs = if (msg.timeSec > 0) msg.timeSec * 1000L else System.currentTimeMillis(),
            outgoing = false, msgId = msg.msgId, read = false)
        _messages.value = _messages.value + (groupId to ((_messages.value[groupId] ?: emptyList()) + m))
        storeAsync { insert(m) }
    }

    /** disposition 통지(1 미달·2 전달·3 읽음) → 발신 말풍선 상태. */
    internal fun updateSendState(msgId: String, notifType: Int) {
        if (msgId.isEmpty()) return
        val st = when (notifType) { 2 -> SendState.DELIVERED; 3, 4 -> SendState.READ; else -> SendState.FAILED }
        _messages.value = _messages.value.mapValues { (_, list) ->
            list.map { if (it.msgId == msgId && it.outgoing) it.copy(state = st) else it }
        }
        storeAsync { setStateByMsgId(msgId, st) }
    }

    /**
     * SIP 요청의 최종 응답.
     *
     * 메시지 발신은 token 으로 상관하고, **SUBSCRIBE 실패는 드러낸다** — 조용히 삼키면 구독이
     * 거절된 것과 통화가 없는 것을 구분할 수 없다(감시 대상 통화가 «안 보이는» 가장 흔한 원인).
     */
    internal fun applyRequestResult(r: com.cims.ue.sdk.RequestResult) {
        if (r.method.equals("SUBSCRIBE", ignoreCase = true) && r.code !in 200..299) {
            _subscribeError.value = "구독 실패 ${r.code}${if (r.reason.isNotBlank()) " ${r.reason}" else ""}"
            return
        }
        applyRequestResult(r.token, r.code)
    }

    /** 발신 최종 응답(token 상관) → 보냄/실패. */
    internal fun applyRequestResult(token: Long, code: Int) {
        if (token <= 0) return
        val st = if (code in 200..299) SendState.SENT else SendState.FAILED
        _messages.value = _messages.value.mapValues { (_, list) ->
            list.map {
                if (it.token == token && it.outgoing && it.state == SendState.PENDING) it.copy(state = st)
                else it
            }
        }
        storeAsync { setStateByToken(token, st) }
    }

    /** 그룹 스레드를 읽음 처리 — ① 카드의 ✉ 배지가 내려간다. */
    fun markRead(groupId: String) {
        _messages.value = _messages.value.mapValues { (g, list) ->
            if (g == groupId) list.map { if (!it.read) it.copy(read = true) else it } else list
        }
        storeAsync { markRead(groupId) }
    }

    /** URI 에서 번호부만 — `tel:+8210…`·`sip:1001@dom` 둘 다. */
    /**
     * **1초 틱** — 경과 시간을 흐르게 한다(Windows `MainViewModel.Tick(now)` 대응).
     *
     * `SessionItem.elapsedMs` 는 계산 속성이라 값이 바뀌어도 Compose 가 알지 못한다. 데스크톱이
     * `DispatcherTimer` 로 `Elapsed` 를 갱신하듯(`Models/Sessions.cs` `Tick`) 여기서 같은 주기로
     * 목록을 다시 낸다. 살아 있는 세션·dialog 가 있을 때만 돈다 — 대기 중에는 깨우지 않는다.
     */
    private val _tick = MutableStateFlow(0L)
    val tick: StateFlow<Long> = _tick.asStateFlow()

    private var ticker: kotlinx.coroutines.Job? = null

    private fun startTicker() {
        ticker?.cancel()
        ticker = scope.launch {
            while (true) {
                kotlinx.coroutines.delay(1000)
                if (_sessions.value.isNotEmpty() || _dialogs.value.isNotEmpty()) _tick.value++
            }
        }
    }

    private var observing = false

    /** 코어 이벤트를 받아 상태를 갱신한다. 엔진이 선 뒤 [start] 가 한 번만 부른다. */
    private fun observe() {
        if (observing) return
        observing = true
        startTicker()
        val engine = ue ?: return
        scope.launch { engine.registrations.collect { _registrations.value = it } }
        scope.launch { engine.callState.collect { applyCallState(it) } }
        scope.launch { engine.incomingCall.collect { upsertSession(it) } }
        scope.launch { engine.callMedia.collect { upsertSession(it) } }
        scope.launch { engine.floor.collect { applyFloor(it) } }
        scope.launch { engine.roster.collect { applyRoster(it) } }
        scope.launch { engine.sds.collect { applySds(it) } }
        scope.launch { engine.requestResult.collect { applyRequestResult(it) } }
        scope.launch { engine.dialogInfo.collect { applyDialog(it) } }
    }

    // ── 오디오 ────────────────────────────────────────────────────────────────
    /**
     * 저장된 설정대로 경로를 적용한다.
     *
     * 무전/통화 분리 출력은 **헤드셋 종류에 따라 되는 조합이 다르다**(§8 실측) — 현재는 단일 경로만
     * 적용하고, 분리는 코어에 스트림별 출력 통로가 생긴 뒤 켠다(§11).
     */
    fun applyAudio() {
        audio.setInCall(true)
        audio.setRoute(settings.current.audioRoute)
        audio.ensureRxVolume()
    }

    // ── 종료 ──────────────────────────────────────────────────────────────────
    /**
     * 로그아웃 — 살아 있는 세션을 끊고 등록을 풀고 자격을 지운다.
     *
     * 엔진은 살려 둔다(재로그인이 재사용한다). 계정은 **제거**한다 — 남겨 두면 다음 로그인에서 같은 계정이
     * 두 번 올라간다.
     */
    fun logout() {
        epoch++                     // 진행 중인 기동의 게시를 무효화한다(§F4)
        _loginGeneration.value++    // 화면 캐시·폼을 버리게 한다(계정 격리)
        val engine = ue
        val accs = _accounts.value.values.toList()
        scope.launch {
            // 통화·무전을 먼저 끊는다 — 등록을 풀기 전에 BYE 가 나가야 서버 쪽 세션이 남지 않는다.
            engine?.calls()?.forEach { runCatching { engine.call(it).hangup() } }
            accs.forEach { a ->
                runCatching { a.unregister() }
                runCatching { a.remove() }
            }
        }
        store.clear()
        noteTokens(null)
        loginPassword = ""
        _profile.value = null
        _accounts.value = emptyMap()
        _sessions.value = emptyList()
        _groups.value = emptyList()
        _activity.value = emptyList()
        // 보관은 지우지 않는다 — 같은 관제석에 다시 로그인하면 스레드가 이어져야 한다.
        // (사람이 바뀌는 자리의 격리는 별건 — §11)
        _messages.value = emptyMap()
        _dialogs.value = emptyList()
        _callLog.value = emptyList()
        _phoneBook.value = DirectoryBook()
        _pttBook.value = DirectoryBook()
        nameIndex = emptyMap()
        _tally.value = DeskTally()
        watched = emptySet()
        refreshFailures = 0
        _credentialWarning.value = null
        _notified.value = emptySet()
        _dialogSeen.value = emptySet()
        _subscribeError.value = ""
        _registrations.value = emptyMap()
        operations.clear()
        adhocMembers.clear()
        _error.value = null
        _state.value = SessionState.LOGGED_OUT
    }

    override fun close() {
        ticker?.cancel(); ticker = null
        runCatching { messageStore.close() }
        runCatching { audio.close() }
        runCatching { csc?.close() }
        runCatching { ue?.close() }
        csc = null; ue = null; mgmt = null
    }

    // ── 내부 ──────────────────────────────────────────────────────────────────
    /**
     * 신뢰 앵커 — **APK 동봉 루트**(sip_tls_signaling.md §8) + 설정의 추가 앵커.
     *
     * Android 에는 OpenSSL 이 읽는 기본 인증서 경로가 없다(신뢰 저장소가 Java 층에 있다). 그래서 이 값을
     * 비워 두면 코어가 `SSL_CTX_set_default_verify_paths` 로 폴백해 아무것도 못 찾고
     * `unable to get local issuer certificate` 로 떨어진다 — 앵커는 반드시 넘긴다.
     */
    private fun trustAnchors(): String {
        val extra = settings.current.extraCaPem
        return if (extra.isBlank()) TrustAnchors.CA_BUNDLE else TrustAnchors.CA_BUNDLE + "\n" + extra
    }

    private fun makeCsc(host: String, port: Int): CscClient {
        csc?.close()
        mgmt = null
        val s = settings.current
        return CscClient(CscEndpoint(
            host = host, port = port,
            caPem = trustAnchors(), verifyServer = s.verifyServer)).also { csc = it }
    }

    private fun fail(code: Int, reason: String): CimsResult<Unit> {
        _error.value = reason
        _state.value = SessionState.FAILED
        return CimsResult.fail(code, reason)
    }

    /** 등록이 하나라도 살아 있는가 — 상단 바 점등. */
    fun anyRegistered(): Boolean =
        _registrations.value.values.any { it.state == RegState.REGISTERED }

    private companion object {
        const val USER_AGENT = "CIMS-Dispatch-Tablet/0.1"
        /** ⑤ 이벤트 링 버퍼 크기 — 화면은 필터로 좁혀 본다. */
        const val ACTIVITY_LIMIT = 500
        /** ⑥ 통화 내역 보관 — 그 이전은 서버 통합 이력이 가진다. */
        const val CALL_LOG_LIMIT = 300
    }
}
