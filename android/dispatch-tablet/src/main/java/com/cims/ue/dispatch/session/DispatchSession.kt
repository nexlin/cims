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
import com.cims.ue.sdk.Capabilities
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.CimsUe
import com.cims.ue.sdk.CommencementMode
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
import com.cims.ue.sdk.UeInitConfigDoc
import com.cims.ue.sdk.UserProfileDoc
import com.cims.ue.sdk.platform.AudioRouter
import com.cims.ue.sdk.platform.DeviceIdentity
import com.cims.ue.sdk.platform.HwPtt
import com.cims.ue.sdk.platform.SecureStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.async
import kotlinx.coroutines.delay
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

    /** 음소거 토글의 줄 — 연타가 같은 값 둘로 합쳐지지 않게(`toggleMuted`). */
    internal val muteToggle = SerialToggle()

    /**
     * SDS 전달 확인 회신의 token → 무엇을 보냈나. 최종 응답이 오면 지운다 — 거절만 로그로 남긴다(`applySds`).
     * 메인 스레드 전용(세션·화면 VM 의 스코프가 모두 `Dispatchers.Main.immediate`). 로그아웃 때 비운다.
     */
    internal val notificationTokens = HashMap<Long, String>()

    /** token 보다 먼저 온 최종 응답 — 발신 명령이 돌아와 token 을 알게 되면 꺼내 간다(`TokenLedger`, `sendTracked`). 메인 스레드 전용. */
    internal val earlyResults = TokenLedger<com.cims.ue.sdk.RequestResult>()

    /**
     * 발신 명령을 [earlyResults] 의 창 안에서 보낸다 — 명령이 도는 동안 온 최종 응답을 놓치지 않는다.
     * @return 명령 결과와, 명령이 돌아오기 전에 이미 와 있던 그 발신의 최종 응답(없으면 null)
     */
    internal suspend fun <T> sendTracked(
        tokenOf: (T) -> Long,
        send: suspend () -> CimsResult<T>,
    ): Pair<CimsResult<T>, com.cims.ue.sdk.RequestResult?> {
        earlyResults.begin()
        val r = try { send() } catch (t: Throwable) { earlyResults.end(0); throw t }
        return r to earlyResults.end(r.value?.let(tokenOf) ?: 0L)
    }

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

    private val _serverCert = MutableStateFlow<com.cims.ue.sdk.TlsPeerExpiry?>(null)
    /**
     * 서버 인증서 만료 관측 — SIP TLS(엔진)·HTTPS(CSC) 중 먼저 만료되는 것(`ServerCert.worst`). 관측 전(평문·미접속)엔
     * null. 만료 배너(§6.2a-3)와 설정 «서버 인증서» 행의 입력이다.
     */
    val serverCert: StateFlow<com.cims.ue.sdk.TlsPeerExpiry?> = _serverCert.asStateFlow()

    /**
     * 관측을 다시 읽는다 — 로그인(HTTPS)·TLS 등록 성공(핸드셰이크 = 관측 갱신 시점)과 1분마다. 잔여 일수는 하루에 한 번
     * 바뀌므로 더 자주 볼 까닭이 없다(데스크톱 `UpdateServerCertBanner` 와 같은 시점). 경고 구간에 드는 순간을 로그에 남긴다.
     */
    private fun refreshServerCert() {
        // 로그인 전·로그아웃 뒤에는 보지 않는다 — 엔진은 로그아웃 뒤에도 살아 있어 앞 접속의 관측을 들고 있다.
        if (_state.value == SessionState.LOGGED_OUT || _state.value == SessionState.FAILED) {
            _serverCert.value = null; return
        }
        val next = ServerCert.worst(ue?.tlsPeerExpiry(), csc?.tlsPeerExpiry())
        val now = System.currentTimeMillis() / 1000L
        val was = _serverCert.value?.let { ServerCert.level(it, now) } ?: CertLevel.OK
        val lvl = next?.let { ServerCert.level(it, now) } ?: CertLevel.OK
        if (lvl != was && lvl != CertLevel.OK) android.util.Log.w("DispatchSession",
            "server cert $lvl: ${next?.remote} subject=\"${next?.subject}\" not_after=${next?.let(ServerCert::day)}")
        _serverCert.value = next
    }

    private fun noteTokens(t: TokenSet?) {
        tokens = t
        // 만료 60초 전을 만료로 본다 — 왕복 지연 중에 만료되는 것을 피한다.
        tokenExpiresAtMs =
            if (t == null || t.expiresInSec <= 0) 0L
            else System.currentTimeMillis() + (t.expiresInSec - 60).coerceAtLeast(30) * 1000L
        // MC 서비스 인가 토큰(TS 24.379 §7.2.2) — 코어는 다음 인가부터 새 토큰을 싣고, 인가 안 된 서비스는 지금 다시 인가한다
        val access = t?.accessToken.orEmpty()
        val ptt = pttAccount
        if (access.isNotEmpty() && ptt != null) scope.launch { ptt.setAccessToken(access) }
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
            // **갱신은 호출자가 취소돼도 끝까지 간다.** 서버는 이 요청으로 옛 refresh token 을 이미 폐기한다(회전) — 화면이 조회를
            //   끊으며(이력 ◀◀·새로고침) 이 요청의 결과를 버리면, 다음 갱신이 옛 토큰으로 나가 `invalid_grant` → 통화·무전까지
            //   끊기는 전체 로그아웃이 된다. 받은 토큰은 그 자리에서 적는다.
            val r = withContext(kotlinx.coroutines.NonCancellable) {
                c.refresh(rt).also { res ->
                    if (!res.ok) return@also
                    if (gen == epoch) noteTokens(res.value)
                    // 회전한 refresh token — «자동 로그인» 이고, 같은 로그인이거나 **저장분이 방금 쓴 그 토큰**일 때 적는다. 갱신
                    //   도중 [앱 종료](자격을 남기는 로그아웃)가 끼어들어도 저장분이 폐기된 토큰으로 남지 않는다(남으면 다음 기동의
                    //   자동 로그인이 `invalid_grant` 로 끝난다). 자격을 지운 로그아웃·다른 사람의 로그인은 저장분이 달라 적지 않는다.
                    val next = res.value?.refreshToken.orEmpty()
                    if (next.isNotEmpty() && settings.current.autoLogin &&
                        (gen == epoch || store.get(SecureStore.KEY_REFRESH_TOKEN) == rt))
                        store.put(SecureStore.KEY_REFRESH_TOKEN, next)
                }
            }
            if (gen != epoch) return null                       // 갱신 중 로그아웃 — 게시하지 않는다
            if (!r.ok) {
                // **세션이 끝난 것과 일시적 장애를 가른다.**
                //   · invalid_grant(400) = 서버가 이 자격을 버렸다(폐기·회전 실패·refresh 만료).
                //     되살릴 방법이 없으므로 **앱 전체를 로그아웃**한다 — «조회만 안 되는 반쯤 로그인»
                //     상태를 두지 않는다. 관제사에게 로그인은 하나다.
                //   · 그 밖(네트워크·5xx)은 일시적이라 옛 토큰을 그대로 쓰고 띠로만 알린다.
                if (isSessionEnded(r.code, r.reason)) {
                    endSession("로그인이 만료되었습니다 — 다시 로그인하세요")
                    return null
                }
                refreshFailures++
                // 데스크톱과 같은 문장 — 무엇이 막히고 무엇은 괜찮은지, 무엇을 볼지. 원문 사유는 로그에 남긴다.
                android.util.Log.w("DispatchSession", "token refresh failed #$refreshFailures: ${r.code} ${r.reason}")
                _credentialWarning.value =
                    "서버 자격 갱신 실패 · 이력·관리·PTT 그룹 조회가 곧 막힐 수 있습니다 — 통화는 계속됩니다. 서버 연결을 확인하세요"
                return now.accessToken
            }
            refreshFailures = 0
            _credentialWarning.value = null
            // 토큰·회전한 refresh token 은 위(취소되지 않는 구간)에서 이미 적었다 — «자동 로그인» 일 때만 디스크에 남는다.
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

    /**
     * DB 쓰기는 **한 줄로** 보낸다 — 보낸 순서대로 실행된다. 쓰기마다 따로 IO 로 띄우면 «말풍선 INSERT» 보다
     * «상태 UPDATE» 가 먼저 돌 수 있고, 그러면 UPDATE 는 대상이 없어 사라지고 뒤늦은 INSERT 가 옛 상태를 남긴다.
     */
    private val storeDispatcher = Dispatchers.IO.limitedParallelism(1)

    private fun storeAsync(block: MessageStore.() -> Unit) {
        scope.launch(storeDispatcher) { runCatching { messageStore.block() } }
    }
    val audio = AudioRouter(context)

    /**
     * 측면 하드키(§7) — 누름·뗌이 발언 바와 같은 일을 한다(데스크톱 전역 핫키 `ptt`). 키는 앱 창(시트·대화상자 포함)의
     * 키 이벤트로만 받는다 — 화면이 꺼진 상태는 고려하지 않는다(관제 전용 기기). 세션이 드는 것은 창이 여럿이고 학습 매핑이
     * 로그인과 무관하게 남아야 해서다. 무엇을 할지(발언 대상 전부에 floor 요청)는 발언 바를 가진 ① VM 이
     * [HwPtt.pressed] 를 보고 정한다.
     */
    val hwPtt = HwPtt(context)

    // ── 상태 ──────────────────────────────────────────────────────────────────
    private val _state = MutableStateFlow(SessionState.LOGGED_OUT)
    val state: StateFlow<SessionState> = _state.asStateFlow()

    private val _profile = MutableStateFlow<Profile?>(null)
    val profile: StateFlow<Profile?> = _profile.asStateFlow()

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error.asStateFlow()

    // ── 토스트 — 명령 실패의 사유(§6.2a-2, dispatch_desktop_ui.md §3.2·§9) ─────────────────
    private val _notices = MutableStateFlow<List<Notice>>(emptyList())
    /** 최신 위. 오류는 닫을 때까지, 정보·경고는 6초. */
    val notices: StateFlow<List<Notice>> = _notices.asStateFlow()
    private var noticeSeq = 0L

    internal fun notify(level: NoticeLevel, text: String, detail: String = "") {
        val n = Notice(++noticeSeq, level, text, detail)
        _notices.value = NoticeBoard.push(_notices.value, n)
        if (n.autoClose) scope.launch { delay(NoticeBoard.AUTO_CLOSE_MS); dismissNotice(n.id) }
    }

    fun dismissNotice(id: Long) { _notices.value = NoticeBoard.dismiss(_notices.value, id) }

    private var internalErrorAtMs = 0L

    /**
     * 예상 밖 예외 — 로그에 적고 **한 번** 알린다. 같은 원인이 이벤트마다 되풀이될 수 있어 토스트는 30초에 하나다(오류 토스트는
     * 손으로 닫을 때까지 남는다 — 쌓이면 진짜 명령 실패를 밀어낸다).
     */
    internal fun noteInternalError(what: String, e: Throwable) {
        runCatching { android.util.Log.e("Dispatch", "unhandled · $what", e) }
        val now = System.currentTimeMillis()
        if (now - internalErrorAtMs < INTERNAL_ERROR_QUIET_MS) return
        internalErrorAtMs = now
        notify(NoticeLevel.ERROR, "내부 오류 — 동작은 계속됩니다. 되풀이되면 앱을 다시 시작하세요",
            "$what · ${e.javaClass.simpleName}: ${e.message.orEmpty()}")
    }

    /** 같은 것을 메인에서 — 스코프의 예외 처리기는 어느 스레드에서든 불린다. */
    internal fun noteInternalErrorLater(what: String, e: Throwable) { scope.launch { noteInternalError(what, e) } }

    /**
     * 이벤트 하나를 접는다 — **던져도 수집은 이어진다.** 수집 코루틴은 예외 하나로 끝나고, 끝나면 그 종류의 이벤트(호 상태·
     * floor·로스터…)가 다시는 접히지 않는다 — 앱은 떠 있는데 귀가 먹는다. 건마다 받아 적고 다음 이벤트를 계속 받는다
     * (영상 평면의 `guard` 와 같은 규칙).
     */
    private inline fun guarded(what: String, block: () -> Unit) {
        try { block() }
        catch (e: kotlinx.coroutines.CancellationException) { throw e }
        catch (e: Exception) { noteInternalError(what, e) }
    }

    /**
     * 명령 결과를 토스트로 — 실패면 사전 문장(§9) + 원문 코드 ▸상세. 결과는 그대로 돌려준다(호출자가 이어서 쓴다).
     *
     * 앱이 먼저 막은 실패(코드 없음 — 상한·계정 없음 등)는 **경고**다(데스크톱 `Fail(why)` = `Notify.Warn`). 서버나 엔진이
     * 거절한 것은 **오류**다.
     */
    internal fun <T> report(area: TextArea, r: CimsResult<T>): CimsResult<T> {
        if (r.ok) return r
        if (r.code < 0) notify(NoticeLevel.WARN, r.reason.ifBlank { "실패" })
        else notify(NoticeLevel.ERROR, ResponseText.sip(area, r.code, r.reason), "${r.code} ${r.reason}".trim())
        return r
    }

    private val _accounts = MutableStateFlow<Map<AccountKind, Account>>(emptyMap())
    val accounts: StateFlow<Map<AccountKind, Account>> = _accounts.asStateFlow()
    /** 이 로그인이 만든 계정(id → 종류) — 게시 전에 닿는 등록 이벤트의 이름을 대고, 지운 계정의 낡은 줄을 가른다. */
    private val accountKinds = HashMap<Int, AccountKind>()
    /** `_accounts` 에 오르지 않은 계정(같은 종류의 둘째 서비스) — 로그아웃이 함께 푼다. */
    private var extraAccounts: List<Account> = emptyList()
    /** 이번 기동이 시작한 때 — 그 뒤에 보낸 메시지는 «지난 기동의 잔존 PENDING» 이 아니다(`restoreMessages`). */
    private var startEnteredMs = 0L

    // ── CMS·UE 초기 설정 문서(TS 24.484) ──────────────────────────────────────
    private val _capabilities = MutableStateFlow(Capabilities())          // 기본값 = 문서 미수신(전부 허용) — of(null, null) 과 같다
    /**
     * 정책 게이트 — user profile ruleset 인가(ue_sdk.md §4.2). 받지 못한 문서는 허용으로 둔다(UX 선차단일 뿐, 최종 판정은 서버).
     * 개별 통화·애드혹·긴급 호출의 선차단과 배너·채널 상세의 [긴급 해제]·[경보 해제] 자격이 이것을 읽는다
     * (데스크톱 `DispatchSession.Capabilities` 와 같은 규약).
     */
    val capabilities: StateFlow<Capabilities> = _capabilities.asStateFlow()
    private var userProfile: UserProfileDoc? = null
    /** service configuration(TS 24.484 §8.4) — Resource-Priority 값의 정본(TS 24.379 §6.2.8.1.15). 못 받으면 코어 기본값. */
    private var serviceConfig: com.cims.ue.sdk.ServiceConfigDoc? = null
    /** 참여 기능 PSI(MCPTT = 경보 Request-URI, MCData = disposition 통지 Request-URI — TS 24.484 §7.2.2.1 10)·14)). */
    private var ueInit: UeInitConfigDoc? = null

    /** 영상 평면(MCVideo 영상 채널 — `VideoPlane.kt`)의 상태. 영상 호는 [sessions] 에 들지 않고 여기에 든다. */
    internal val video = VideoPlaneState()
    /** 유지 평면(`UpkeepPlane.kt`) — 등록에 묶인 서버 상태(제휴·구독)를 등록이 새로 설 때·수명 절반마다 다시 싣는다. */
    internal val upkeep = UpkeepState()
    /** UE initial configuration — 영상 평면이 MCVideo PSI 를 읽는다(TS 24.484 §7.2.2.1). */
    internal fun ueInitDoc(): UeInitConfigDoc? = ueInit
    /** 앱 컨텍스트 — 영상 평면이 카메라 방향(Camera2 센서 방향)을 맞출 때 쓴다. */
    internal fun appContext(): Context = context.applicationContext

    /** 하향(해제)을 보낸 호 — 거절 문구를 상향과 가른다(같은 조건 이벤트 흐름, 데스크톱 `_conditionCancel`). */
    internal val conditionCancel: MutableSet<Int> = java.util.Collections.synchronizedSet(mutableSetOf())

    private val _alertBanners = MutableStateFlow<List<EmergencyAlertBanner>>(emptyList())
    /**
     * 긴급 경보 배너(TS 24.379 §12.1.1.3 — SIP MESSAGE alert-ind) — 그룹·발신자마다 하나, 최신 위. 세션 조건(긴급·임박 — [alerts])과
     * 별개 신호다: 그룹 세션이 없어도 온다. 발신자의 취소(또는 제3자 취소의 originated-by)로 내린다(`AlertPlane.kt`).
     */
    val alertBanners: StateFlow<List<EmergencyAlertBanner>> = _alertBanners.asStateFlow()
    internal fun setAlertBanners(next: List<EmergencyAlertBanner>) { _alertBanners.value = next }

    /** 경보 취소 MESSAGE 의 token → 내린 배너. 최종 응답이 403(미인가)이면 되살린다(`applyAlertCancelResult`). 메인 스레드 전용. */
    internal val alertCancelTokens = HashMap<Long, EmergencyAlertBanner>()

    /** `/provisioning/me` 의 ETag — 관제 편성 재조회(60초)가 304 로 끝나게 한다(`refreshDispatch`). */
    internal var profileEtag: String = ""
    /** xcap-diff 통지의 차례 — 연속 통지를 0.5초 합친다(`applyXcapDiff`). 메인 스레드 전용. */
    internal var groupRefreshSeq: Int = 0

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

    /**
     * 전화 축 문자(SMS/LMS) 스레드 — SDS 와 **맵을 나눈다**.
     *
     * 둘 다 스레드 키가 번호일 수 있어(PTT 1:1 SDS) 한 맵에 담으면 두 망의 글이 한 대화에 섞인다.
     * 발신 상태 상관(`applyRequestResult`)·보관은 같은 길을 쓴다.
     */
    private val _sms = MutableStateFlow<Map<String, List<Message>>>(emptyMap())
    val sms: StateFlow<Map<String, List<Message>>> = _sms.asStateFlow()

    private val _dialogs = MutableStateFlow<List<DialogRow>>(emptyList())
    /** 감시 중인 dialog(RFC 4235) — ③ 그룹원 띠·대표번호 대기열의 소스. */
    val dialogs: StateFlow<List<DialogRow>> = _dialogs.asStateFlow()

    private val _callLog = MutableStateFlow<List<CallLogRow>>(emptyList())
    /** ⑥ 통화 내역 — 최신이 앞. */
    val callLog: StateFlow<List<CallLogRow>> = _callLog.asStateFlow()

    /**
     * 긴급·임박 스택 — **채널마다 하나**, 최신 위(데스크톱 배너 레이어 `BannerOfGroup`, dispatch_desktop_ui.md §3.2).
     *
     * 닫기가 없다. 조건이 풀리거나(코어 스냅샷의 `emergency`/`imminentPeril` 이 내려감) 세션이 끝나면 스스로 빠진다.
     */
    val alerts: StateFlow<List<SessionItem>> =
        _sessions.map(::alertStack).stateIn(scope, SharingStarted.Eagerly, emptyList())

    /**
     * **착신 스택** — 울리고 있는 전화. 최신이 위다(dispatch_desktop_ui.md §3.2 착신 배너).
     *
     * 화면 어디에 있든 보여야 하므로 세션이 든다. 이것이 없으면 착신 표면이 [일반통화] 탭의 카드
     * 하나뿐이라, 다른 탭·화면에 있는 동안 걸려 온 전화를 **아무도 못 본다**.
     */
    val incoming: StateFlow<List<SessionItem>> =
        _sessions.map { list ->
            // 개별 통화도 같은 배너를 쓴다(§3.2 청록) — 상대가 수동 응답을 요청한 착신만(코어가 따른 개시 방식 MANUAL,
            //   TS 24.379 §11.1.1.2.1.2 10)). 자동 개시는 코어가 곧바로 받으므로 배너가 깜박이지 않게 거른다.
            list.filter {
                it.info.state == CallState.INCOMING && (it.kind == SessionKind.PHONE_CALL ||
                    (it.kind == SessionKind.PTT_PRIVATE && it.info.commencement == CommencementMode.MANUAL))
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

    /** **PTT 주소록** — 개별 통화·그룹 멤버 후보. 전화 발신에는 쓰지 않는다. */
    private val _pttBook = MutableStateFlow(DirectoryBook())
    val pttBook: StateFlow<DirectoryBook> = _pttBook.asStateFlow()

    /** 정규형 번호 → 이름. 조회가 잦아 목록을 매번 훑지 않는다. */
    private var nameIndex: Map<String, String> = emptyMap()

    private val _tally = MutableStateFlow(DeskTally())
    /** 오늘 데스크 — ③ 상단 칩. */
    val tally: StateFlow<DeskTally> = _tally.asStateFlow()

    /** dialog 를 구독 중인 AoR — 대표번호 + 관제 범위의 감시 대상(`dispatch.members[]`). */
    private var watched: Set<String> = emptySet()

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
    /** 청취·감청이 로스터에 드러나지 않는가 — 역할 `listen_visibility`(`hidden`|`visible`, mcptt_authorization.md). 모르면 은닉으로 읽는다. */
    val listenHidden: Boolean get() = dispatch.listenVisibility != "visible"
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
        if (after.audioRoute != before.audioRoute || after.preferredHeadset != before.preferredHeadset)
            runCatching { applyAudio() }
    }

    /** 이어폰 하나를 고른다 — 선호 이어폰으로 남기고 경로를 그 종류(유선·무선)로 옮긴다(설정 «오디오»). */
    fun selectHeadset(h: com.cims.ue.sdk.platform.Headset) {
        updateSettings { it.copy(preferredHeadset = h.name,
            audioRoute = if (h.wireless) com.cims.ue.sdk.platform.Route.BLUETOOTH else com.cims.ue.sdk.platform.Route.HEADSET) }
    }

    /** 지금 붙어 있는 이어폰 이름 — 새로 붙은 것을 가려 자동 복귀를 건다. null = 아직 첫 값 전(첫 값은 «새로 붙음» 이 아니다). */
    private var headsetNames: Set<String>? = null

    /**
     * 이어폰이 붙거나 빠졌다 — **선호 이어폰이 다시 붙었고** 경로 의도가 헤드셋·블루투스면 그리로 되돌린다(데스크톱
     * `OnEndpointsChanged` 의 «다시 붙으면 복귀(설정 이름 기준)»). 사람이 스피커를 고른 뒤에는 되돌리지 않는다 — 의도가 이긴다.
     * 빠질 때는 할 일이 없다: 플랫폼이 기본 장치로 돌아간다.
     */
    private fun onHeadsetsChanged(list: List<com.cims.ue.sdk.platform.Headset>) {
        val names = list.map { it.name }.toSet()
        val before = headsetNames
        headsetNames = names
        if (before == null) return
        val s = settings.current
        if (!returnToPreferred(s, names - before)) return
        runCatching { applyAudio() }
        notify(NoticeLevel.INFO, "선호 이어폰으로 돌아왔습니다", s.preferredHeadset)
    }

    init {
        // 이어폰 붙음·빠짐 — 선호 이어폰 자동 복귀(§8). [headsetNames] 선언 뒤에 둔다(초기화 순서).
        scope.launch { audio.headsets.collect(::onHeadsetsChanged) }
    }

    /** 저장된 자격이 있는가 — 부팅 재등록·자동 로그인의 조건. */
    /** 저장된 로그인으로 이어 갈 수 있는가 — «자동 로그인» 이 켜져 있고 토큰이 있다(끄면 그 순간부터 자동으로 들어가지 않는다). */
    val hasSavedLogin: Boolean get() = settings.current.autoLogin && store.contains(SecureStore.KEY_REFRESH_TOKEN)

    // ── 로그인 ────────────────────────────────────────────────────────────────
    /**
     * 로그인 흐름(수동 로그인·자동 로그인, 각각 기동까지)은 **한 번에 하나**다. 자동 로그인이 느린 서버를 기다리는 동안 사람이
     * 로그인을 누르면 두 흐름이 저마다 `fetchProfile → start` 를 돌아 엔진을 두 번 세우거나 계정을 두 벌 올린다(앞 벌은
     * 고아가 되어 로그아웃해도 등록이 남는다).
     */
    private val loginMutex = kotlinx.coroutines.sync.Mutex()

    /** 사람이 로그인을 시도했다 — 자동 로그인 재시도는 여기서 그친다(사람이 넣은 자격이 우선이다). 로그아웃하면 되돌아간다. */
    @Volatile var manualLoginTried = false
        private set

    /** 이미 로그인 흐름 안이거나 끝났는가 — 그러면 새로 시작하지 않는다. */
    private val loggedInOrBusy: Boolean
        get() = _state.value != SessionState.LOGGED_OUT && _state.value != SessionState.FAILED

    /** 로그인·기동의 결과 — 어느 단계에서 그쳤는지(문구가 다르다). */
    data class LoginOutcome(val login: CimsResult<Unit>, val start: CimsResult<Unit>? = null) {
        val ok: Boolean get() = login.ok && start?.ok != false
    }

    /**
     * 수동 로그인 → 기동. **세션 수명에서 돈다** — 화면(Activity)이 닫혀도 기동은 끝까지 간다. 화면 수명에서 돌리면 닫는 순간
     * 취소돼 `STARTING` 에 멈추고, 다시 열었을 때 편성·이력 주기가 돌지 않는 반쪽 세션이 남는다.
     */
    suspend fun loginAndStart(host: String, port: Int, loginId: String, password: String): LoginOutcome =
        scope.async {
            manualLoginTried = true
            loginMutex.withLock {
                // 기다리는 사이 자동 로그인이 끝났다 — 화면은 이미 셸이다. 다른 사람으로 들어가려면 로그아웃이 먼저다.
                if (loggedInOrBusy) return@withLock LoginOutcome(CimsResult.ok(Unit))
                val r = login(host, port, loginId, password)
                // 서버에 닿지 못해 실패한 시도(음수 코드)는 «사람이 넣은 자격» 이 아니다 — 자동 로그인이 망 복귀 때 다시 이어 가게 둔다
                if (!r.ok && r.code < 0) manualLoginTried = false
                if (!r.ok) LoginOutcome(r) else LoginOutcome(r, start())
            }
        }.await()

    /**
     * 저장된 자격으로 로그인 → 기동(부팅·프로세스 복귀·재시도). 이미 로그인됐거나 사람이 로그인을 시도했으면 하지 않는다.
     */
    suspend fun resumeAndStart(): CimsResult<Unit> = scope.async {
        // **세션 수명에서 돈다**([loginAndStart] 와 같다) — 망 전환이 자동 로그인을 다시 걸며 앞 시도를 취소해도 로그인 자체는
        //   끝까지 간다. 호출자 수명에서 돌면 토큰 갱신 도중 끊겨 회전한 refresh token 을 잃고(다음 시도가 `invalid_grant` →
        //   저장된 로그인 삭제), 기동 도중 끊기면 `STARTING` 에 멈춘 반쪽 세션이 남는다.
        loginMutex.withLock {
            if (loggedInOrBusy) return@withLock CimsResult.ok(Unit)
            if (manualLoginTried) return@withLock CimsResult.fail(-1, "수동 로그인 시도 중")
            val r = resume()
            if (!r.ok) r else start()
        }
    }.await()

    /**
     * IdMS PKCE 로그인 → `/provisioning/me`. «자동 로그인» 이 켜져 있으면 refresh token 만 저장한다(비밀번호·H(A1) 는 저장하지
     * 않는다 — `sipHa1` 은 매 로그인 프로파일에서 받는다). 끄고 한 로그인은 저장된 토큰을 지운다 — 다음 기동은 로그인 화면이다.
     */
    private suspend fun login(host: String, port: Int, loginId: String, password: String): CimsResult<Unit> {
        val gen = epoch
        _state.value = SessionState.LOGGING_IN
        val client = makeCsc(host, port)
        val tok = client.login(loginId, password)
        // 그사이 로그아웃([앱 종료]) — 로그인은 세션 수명에서 돌아 호출자의 취소로 멈추지 않는다. 비운 상태를 되살리지 않는다.
        if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
        if (!tok.ok) return fail(tok.code, tok.reason)
        noteTokens(tok.value)
        loginPassword = password
        settings.update { it.copy(cscHost = host, cscPort = port, loginId = loginId) }
        val rt = tok.value?.refreshToken.orEmpty()
        if (settings.current.autoLogin && rt.isNotEmpty()) {
            store.put(SecureStore.KEY_REFRESH_TOKEN, rt)
            store.put(SecureStore.KEY_LOGIN_ID, loginId)
            store.put(SecureStore.KEY_CSC_HOST, "$host:$port")
        } else store.remove(SecureStore.KEY_REFRESH_TOKEN)
        return fetchProfile()
    }

    /**
     * 저장된 refresh token 으로 재로그인 — 프로세스 회수 뒤 복귀 경로(§6.1).
     * 평문 비밀번호는 남기지 않으므로, H(A1) 이 없는 프로파일은 이 경로로 등록하지 못한다.
     */
    private suspend fun resume(): CimsResult<Unit> {
        val rt = store.get(SecureStore.KEY_REFRESH_TOKEN)
            ?: return fail(-1, "저장된 로그인 없음")
        val s = settings.current
        val gen = epoch
        _state.value = SessionState.LOGGING_IN
        val client = makeCsc(s.cscHost, s.cscPort)
        // 갱신은 끝까지 가고 회전한 refresh token 은 **그 자리에서** 적는다 — 서버는 이 요청으로 옛 토큰을 폐기했다. 서비스가 내려가며
        //   끊겨도 저장분이 폐기된 토큰으로 남지 않는다. 그사이 자격을 지웠으면(저장분이 다르다) 적지 않는다.
        val tok = withContext(kotlinx.coroutines.NonCancellable) {
            client.refresh(rt).also { r ->
                val next = r.value?.refreshToken.orEmpty()
                if (r.ok && next.isNotEmpty() && store.get(SecureStore.KEY_REFRESH_TOKEN) == rt)
                    store.put(SecureStore.KEY_REFRESH_TOKEN, next)
            }
        }
        if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
        if (!tok.ok) {
            // 되살릴 수 없는 실패(폐기·회전 실패·만료)일 때만 저장된 로그인을 버린다 — 서버에 닿지 않은 것(망 단절·5xx)은 남겨
            //   다음 기동·다음 시도가 다시 이어 간다(데스크톱 `ResumeAsync`).
            if (isSessionEnded(tok.code, tok.reason)) store.remove(SecureStore.KEY_REFRESH_TOKEN)
            return fail(tok.code, "자동 로그인 실패 — ${tok.reason}")
        }
        noteTokens(tok.value)
        return fetchProfile()
    }

    private suspend fun fetchProfile(): CimsResult<Unit> {
        val c = csc ?: return fail(-1, "로그인 전")
        val t = tokens ?: return fail(-1, "로그인 전")
        val gen = epoch
        val p = c.fetchProfile(t.accessToken)
        if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
        if (!p.ok) return fail(p.code, p.reason)
        // 보관 주인 = 로그인 ID(§6.9) — 프로파일을 게시하기 전에, 다른 DB 쓰기와 같은 줄에서 정한다(적재 `restoreMessages` 보다 먼저).
        withContext(storeDispatcher) { messageStore.setOwner(OwnerRule.pick(p.value?.loginId.orEmpty(), settings.current.loginId)) }
        if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
        _profile.value = p.value
        HomeCountry.code = p.value?.countryCode?.ifBlank { "82" } ?: "82"     // 번호의 표시(국내 표기)만 가른다
        _state.value = SessionState.PROFILE_READY
        _error.value = null
        refreshServerCert()                     // 로그인 = HTTPS 핸드셰이크 — CSC 인증서가 막 관측됐다
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
    private suspend fun start(): CimsResult<Unit> {
        val p = _profile.value ?: return fail(-1, "프로파일 없음")
        val gen = epoch
        startEnteredMs = System.currentTimeMillis()
        _state.value = SessionState.STARTING
        val s = settings.current

        val toRegister = buildList {
            p.phoneService?.let { add(it to AccountKind.PHONE) }
            p.services.filter { it.kind == "ptt" }.forEach { add(it to AccountKind.PTT) }
        }
        val engine = ue ?: CimsUe().also { ue = it }
        if (!engine.running.value) {
            val r = engine.start(EngineConfig(
                userAgent = userAgent(),
                logLevel = s.logLevel,
                tlsCaPem = trustAnchors(),
                tlsVerifyServer = s.verifyServer,
                // UDP→TCP 승격 비활성(sip.udpNoTcpSwitch)은 엔진 전역 — 올리는 서비스 중 하나라도 사이트 옵션이면 켠다(통제된 망 전용)
                udpNoTcpSwitch = toRegister.any { it.first.udpNoTcpSwitch }),
                context.applicationContext)      // 카메라(MCVideo [영상 보내기]) — 장치 열거가 기동 때 한 번이라 기동 전에 넘긴다
            if (!r.ok) return fail(r.code, r.reason)
        }
        applyAudio()
        // 기기 URN — Contact +sip.instance 이자 UE initial configuration 의 MCS UE ID(TS 24.484 §7.2.1.1). 데스크톱·ptt-client 와 같은 규칙.
        val instanceId = DeviceIdentity.instanceUrn(context).orEmpty()
        if (p.services.any { it.kind == "ptt" }) {
            fetchUeInitConfig(instanceId)
            // CMS user profile·service config — 정책 게이트와 Resource-Priority 값. 못 받아도 기동은 계속한다(게이트 없음 = 허용, RP = 코어 기본값).
            refreshCms()
            // MCVideo(§6.14) — 사이트 PSI ∧ 이용 자격이 있을 때만 PTT 계정에 싣는다. 계정 태그는 로그인 때 한 번 정한다.
            prepareVideo()
        }
        // **기동 작업이 자기가 만든 계정을 소유한다.** `logout()` 은 게시된 `_accounts` 만 보므로,
        // 게시 전에 로그아웃이 끼어들면 이미 등록된 계정이 정리 대상에서 빠진다 — 화면은 로그아웃인데
        // SIP 등록과 자격이 엔진에 살아남는다. 소유권을 여기 두고 어느 경로로 빠져나가든 정리한다.
        val added = mutableMapOf<AccountKind, Account>()
        val created = mutableListOf<Account>()             // 만든 계정 전부 — `added` 는 종류마다 하나(그 축의 계정)
        var published = false
        try {
            for ((sp, kind) in toRegister) {
                if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
                val cfg = accountConfig(sp, p.displayName, kind, instanceId)
                val a = engine.addAccount(cfg)
                // 한 계정만 실패하면 셸은 선다 — 로그인 화면의 오류 줄은 보이지 않으므로 토스트로도 알린다(전화가 왜 안 되는지)
                val line = if (kind == AccountKind.PTT) "PTT" else "전화"
                if (!a.ok) {
                    _error.value = "${sp.kind} 계정 추가 실패: ${a.reason}"
                    notify(NoticeLevel.ERROR, "$line 계정 추가 실패", "${a.code} ${a.reason}".trim())
                    continue
                }
                val acc = a.value!!
                created.add(acc)                               // 등록을 걸기 **전에** 소유로 잡는다
                accountKinds[acc.id] = kind
                // MC 서비스 인가 토큰 — 등록이 서면 코어가 이것으로 MCPTT·MCData·MCVideo 를 인가한다(TS 24.379 §7.2.2, 결과 = serviceAuth)
                if (kind == AccountKind.PTT) accessToken()?.let { acc.setAccessToken(it) }
                // 종류마다 **첫** 계정이 그 축의 계정이다 — PTT 서비스가 둘이어도 `pttAccount`·`myPttId` 는 첫 서비스다(데스크톱
                //   `PttService`). 나머지는 등록만 하고 로그아웃이 함께 푼다(`extraAccounts`).
                if (kind !in added) added[kind] = acc
                val reg = acc.register()
                if (!reg.ok) {
                    _error.value = "${sp.kind} 등록 요청 실패: ${reg.reason}"
                    notify(NoticeLevel.ERROR, "$line 등록 요청 실패", "${reg.code} ${reg.reason}".trim())
                }
            }
            if (added.isEmpty()) return fail(-1, "등록할 계정이 없다")
            // 게시는 세대 확인 **뒤**에만 — 먼저 게시하면 로그아웃이 비운 것을 되살린다.
            if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
            _accounts.value = added
            extraAccounts = created.filterNot { c -> added.values.any { it === c } }
            published = true
        } finally {
            // 게시하지 못하고 빠져나갔다면(로그아웃·예외) 만든 계정을 전부 되돌린다.
            if (!published && created.isNotEmpty()) {
                val orphans = created.toList()
                orphans.forEach { accountKinds.remove(it.id) }
                scope.launch {
                    orphans.forEach { a ->
                        runCatching { a.unregister() }
                        runCatching { a.remove() }
                    }
                }
            }
        }

        observe()
        beginUpkeep()                                  // 아래 단계가 처음 구독을 건다 — 갱신 기준 시각
        // **단계마다 세대를 본다.** 구독·조회가 이어지는 몇 초 사이(느린 CSC 면 더 길다) 로그아웃이 끼어들면, 남은 단계가 비운
        //   화면에 앞 사람의 주소록·보관 스레드·세션을 다시 싣는다 — 각 단계는 제 진입 때의 세대만 보므로 이미 지난 로그아웃을
        //   모른다. 다음 로그인은 그것을 «이미 있던 것» 으로 읽어 제 것 위에 얹는다.
        val steps: List<suspend () -> Unit> = listOf(
            { if (hasDesk) watchAll() },               // 대표번호 + 감시 대상 dialog 구독(§6.7 ③)
            { refreshGroups() },                       // PTT 그룹·affiliation·conference 구독(§6.7 ①②)
            { subscribeGroupChanges() },               // 서버발 그룹 변경(xcap-diff, RFC 5875) → 목록 자동 재조회
            { refreshDirectory() },                    // 번호 → 이름. 실패해도 진행한다
            { restoreMessages() },                     // 보관된 SDS 스레드(§6.9)
            { refreshSessions() })
        for (step in steps) {
            if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
            step()
        }
        if (gen != epoch) return CimsResult.fail(-1, "로그아웃됨")
        _state.value = SessionState.READY
        // 데스크가 없는 계정 — 그룹원·대기열·감청·이력이 비는 까닭을 로그인 직후에 말한다
        if (!hasDesk) notify(NoticeLevel.INFO, "관제 데스크 미배정 — 일반 소프트폰 모드",
            "콘솔 관리 › 가입자에서 전화 그룹·관제 역할을 배정하면 그룹원 상태·대기열·감청이 열립니다")
        historyJob?.cancel()                                // 기동이 두 번 불려도 폴링은 한 벌
        historyJob = scope.launch { runCatching { runHistoryFeed() } }   // 서버 통합 이력 — 없으면 탐침에서 조용히 꺼진다
        scope.launch { runCatching { loadGroupTypes() } }               // 관리 범위가 있으면 그룹 종류(chat)를 미리 안다
        return CimsResult.ok(Unit)
    }

    /**
     * 프로파일의 접속서비스 하나 → 계정 설정.
     * MCPTT 자동 수락은 PTT 계정만 — 관제석은 그룹콜 자동·개별 통화 수동이 맞지만 코어 플래그가 아직
     * 공통이라(§11) 우선 PTT 전체를 자동으로 둔다.
     */
    private fun accountConfig(sp: ServiceProfile, displayName: String, kind: AccountKind, instanceId: String): AccountConfig {
        val base = sp.toAccountConfig(loginPw = loginPassword)
        val ptt = kind == AccountKind.PTT
        val ui = ueInit.takeIf { ptt }
        val sc = serviceConfig.takeIf { ptt }
        return base.copy(
            displayName = displayName,
            autoAnswerMcptt = ptt,
            instanceId = instanceId.ifEmpty { base.instanceId },     // PTT·전화 계정이 같은 기기 값(RFC 5626 — 한 UA 인스턴스)
            // 큰 그룹 SDS 는 media plane(MSRP) — 상한은 프로파일 mcdata(`toAccountConfig` 가 옮긴다), 서버발 MSRP 배포도 받는다(TS 24.282 §9.2.3)
            mcdataMsrp = ptt || base.mcdataMsrp,
            // MCData FD 지원(TS 24.282 §7.2.1 3), FilePlane) — SDS 와 같이만: CSP 는 ICSI 목록의 «mcdata» 로 MSRP 대상을 가른다(MCData REG-2)
            mcdataFd = ptt || base.mcdataFd,
            // 참여 기능 PSI(ue-init-config) — 광고하지 않은 서비스는 비워 둔다(경보 = 그룹 URI, 통지 = 원 발신자 직행 — 코어 전환기 경로)
            mcpttServerUri = ui?.mcpttServerUri?.ifEmpty { null } ?: base.mcpttServerUri,
            mcdataServerUri = ui?.mcdataServerUri?.ifEmpty { null } ?: base.mcdataServerUri,
            // Resource-Priority 정본 = service-config on-network *-resource-priority(TS 24.379 §6.2.8.1.15) — 문서에 없으면 코어 기본값
            rpEmergency = sc?.rpEmergency?.ifEmpty { null } ?: base.rpEmergency,
            rpImminentPeril = sc?.rpImminentPeril?.ifEmpty { null } ?: base.rpImminentPeril,
            rpNormal = sc?.rpNormal?.ifEmpty { null } ?: base.rpNormal)
            // MCVideo — REGISTER Contact 태그(TS 24.281 §7.2.1AA)·참여 기능 PSI·편성 초대 자동 합류. 사이트 PSI ∧ 자격이 있을 때만(`prepareVideo`)
            .let { if (ptt) videoAccount(it) else it }
    }

    /**
     * REGISTER `User-Agent` — `CIMS-Dispatch/<앱 버전> (Android <판>; <모델>)`(mcptt_management_views.md §4.1). 제품명이 서버의
     * 단말 유형(dispatch) 판정 키라 데스크톱 관제 앱과 같은 이름을 쓴다. 형식 규칙은 코어 하나(`CimsUe.userAgentOf`).
     */
    private fun userAgent(): String {
        val version = runCatching { context.packageManager.getPackageInfo(context.packageName, 0).versionName }
            .getOrNull().orEmpty().ifEmpty { "0.0.0" }
        return CimsUe.userAgentOf(USER_AGENT_PRODUCT, version, "Android ${android.os.Build.VERSION.RELEASE}", android.os.Build.MODEL.orEmpty())
    }

    /** UE initial configuration(TS 24.484 §7.2.1.1) — 로그인 전 문서라 토큰 없이. 못 받으면 PSI 없이 올린다. */
    private suspend fun fetchUeInitConfig(instanceId: String) {
        val c = csc ?: return
        if (instanceId.isEmpty()) return
        val r = c.fetchUeInitConfig(instanceId, ueInit?.etag.orEmpty())
        if (r.ok) r.value?.let { ueInit = it }                        // null = 304(가진 사본 그대로)
        else android.util.Log.w("DispatchSession", "ue-init-config: ${r.code} ${r.reason}")
        android.util.Log.i("DispatchSession", "ue-init-config mcptt=${ueInit?.mcpttServerUri} mcdata=${ueInit?.mcdataServerUri}")
    }

    /**
     * CMS 문서(TS 24.484 user profile §8.3 · service configuration §8.4) → [capabilities] 와 Resource-Priority.
     *
     * ETag 로 묻는다 — 안 바뀌었으면 304(가진 사본 그대로). 못 받으면 게이트 없음(허용)·코어 기본 RP 다 — 최종 판정은 서버(403).
     * 기동 때 한 번, 그 뒤 [CMS_POLL_MS] 마다 다시 받는다(데스크톱 `RefreshCmsAsync`) — 관제사의 자격이 바뀌면 선차단과
     * 배너·채널 상세의 [해제] 가 재로그인 없이 따라온다.
     */
    internal suspend fun refreshCms() {
        val c = csc ?: return
        val me = myPttId.ifEmpty { return }
        val token = accessToken() ?: return
        val gen = epoch
        val up = c.fetchUserProfile(token, me, userProfile?.etag.orEmpty())
        val sc = c.fetchServiceConfig(token, me, serviceConfig?.etag.orEmpty())
        if (gen != epoch) return                                      // 받는 사이 로그아웃 — 앞 사람의 문서를 남기지 않는다
        if (up.ok) up.value?.let { userProfile = it }                 // null = 304
        else android.util.Log.w("DispatchSession", "cms user-profile: ${up.code} ${up.reason}")
        if (sc.ok) sc.value?.let { serviceConfig = it }
        else android.util.Log.w("DispatchSession", "cms service-config: ${sc.code} ${sc.reason}")
        val caps = Capabilities.of(userProfile, serviceConfig)
        if (caps == _capabilities.value) return
        _capabilities.value = caps
        android.util.Log.i("DispatchSession", "capabilities up=${caps.userProfileKnown} sc=${caps.serviceConfigKnown} " +
            "private=${caps.privateCall} emgGroup=${caps.emergencyGroupCall} peril=${caps.imminentPerilCall} alert=${caps.emergencyAlert} " +
            "alertCancel=${caps.cancelEmergencyAlert} adhoc=${caps.adhocGroupCall} n2=${caps.maxAffiliationsN2} " +
            "emgCancel=${caps.cancelGroupEmergency} perilCancel=${caps.cancelImminentPeril}")
    }

    /** 프로파일을 갈아 끼운다 — 관제 편성 재조회(`refreshDispatch`)가 바뀐 편성을 게시할 때. */
    internal fun setProfile(p: Profile) { _profile.value = p; HomeCountry.code = p.countryCode.ifBlank { "82" } }

    /** 지금 dialog 를 구독 중인 대상 — 편성이 바뀌면 차분만 다시 건다(`rewatch`). */
    internal fun watchedTargets(): Set<String> = watched

    /** 화면 재구성·프로세스 복귀 후 세션을 코어 스냅샷에서 다시 그린다(§6.7). */
    fun refreshSessions() {
        val engine = ue ?: return
        // **`calls()` 는 끝난 호도 준다** — 코어가 조회·최종 통계용으로 64건을 보존한다
        // (`engine.cpp` `callInfos` · `pruneFinished`). 거르지 않으면 화면 복귀마다 끝난 통화가
        // «내 통화» 로 되살아난다.
        val live = engine.calls().mapNotNull { engine.callInfo(it) }
            .filter { it.state != CallState.DISCONNECTED && it.state != CallState.NULL }
            .let(::resyncVideoCalls)             // MCVideo 호는 영상 평면 몫 — 세션 목록에 넣지 않는다(§6.14)
        // 끝났는데 종료 이벤트가 아직 접히지 않은 호 — 여기서 목록만 갈면 그 호의 통화 내역·세션 종료 줄·실패 사유가 사라진다
        //   (종료 이벤트가 닿았을 때는 이미 세션이 없다). 종료로 접고 간다.
        val liveIds = live.mapTo(HashSet()) { it.callId }
        _sessions.value.filter { it.callId !in liveIds }.forEach { s ->
            val end = engine.callInfo(s.callId)?.takeIf { it.state == CallState.DISCONNECTED }
            if (end != null) applyCallState(end) else removeSession(s.callId)
        }
        val prev = _sessions.value.associateBy { it.callId }
        _sessions.value = live.map { ci ->
            val was = prev[ci.callId]
            withAlert(was, was?.copy(info = ci) ?: newSession(ci))
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
        title = titleOf(ci),
        // 애드혹 참가자 — 명령이 먼저 돌아와 지도에 적어 둔 것을 싣는다(이벤트가 먼저면 `rememberAdhocMembers` 가 세션을 고친다)
        adhocMembers = adhocMembers[ci.callId].orEmpty(),
        consultFor = consultOf.remove(ci.callId))

    /**
     * 세션의 표시 이름 — 그룹 호는 그룹 이름, **개별 통화는 상대의 이름**이다. 개별 통화의 `groupId` 에는 코어가 상대의 번호를
     * 싣는다(그룹이 아니다) — 그룹 이름으로 풀면 못 찾아 번호가 그대로 선다. Request-URI 가 참여 기능 PSI 일 수 있어
     * `remoteUri` 보다 그 번호를 먼저 본다(데스크톱 `TitleOf`).
     */
    private fun titleOf(ci: CallInfo): String = when {
        ci.isMcptt && ci.mcptt.privateCall -> displayName(ci.groupId.ifEmpty { ci.remoteUri })
        ci.isMcptt && ci.groupId.isNotEmpty() -> groupNameOf(ci.groupId)
        else -> displayName(ci.remoteUri)
    }

    // ── PttPlane 이 쓰는 접근자 ──────────────────────────────────────────────
    /** 세션 수명의 코루틴 — 이벤트 처리 중 명령을 걸 때 쓴다(이벤트 콜백은 suspend 가 아니다). */
    internal fun scopeLaunch(block: suspend () -> Unit) { scope.launch { block() } }

    internal fun engineOrNull(): CimsUe? = ue
    internal fun cscOrNull(): CscClient? = csc

    private var fdCsc: Pair<CscClient, CscClient>? = null

    /**
     * 파일 전송 전용 CSC 핸들(`FilePlane`) — 한 핸들의 요청은 직렬화되므로 큰 파일이 이력·관리·그룹 조회를 막지 않게 따로 둔다.
     * 지금의 CSC 연결에 묶인다 — 재로그인으로 연결이 바뀌면 낡은 것을 닫고 새로 만든다([management] 와 같은 규칙).
     */
    internal fun fdCscOrNull(): CscClient? {
        val c = csc ?: return null
        fdCsc?.let { (bound, fd) -> if (bound === c) return fd; closeLater(fd) }
        val s = settings.current
        return CscClient(CscEndpoint(host = s.cscHost, port = s.cscPort, caPem = trustAnchors(), verifyServer = s.verifyServer))
            .also { fdCsc = c to it }
    }

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
        code == 401 || (code == 400 && (reason.contains("invalid_grant", ignoreCase = true) ||
            reason.contains("invalid_token", ignoreCase = true)))

    /**
     * 되살릴 수 없는 자격 만료 — **앱 전체를 로그아웃한다.**
     *
     * SIP 등록은 H(A1) 이라 CSC 토큰과 무관하게 살아 있지만(그래서 통화는 되고 조회만 막힌다),
     * 관제사에게 로그인은 **하나**다. 반쯤 로그인된 상태를 두면 무엇이 되고 무엇이 안 되는지 알 수 없다.
     * 등록까지 내리고 로그인 화면으로 보낸 뒤, 왜 그랬는지 남긴다.
     */
    private fun endSession(why: String) {
        logout(forgetLogin = true)
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
     * 전화 계열과 PTT 를 모두 넣는다. 한쪽만 빼면 다른 축의 주소록에서 «나» 가 남아, 자기에게 개별 통화를
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
    /** 그룹 목록 조회가 실패하고 있다 — 알림을 한 번만 낸다(`refreshGroups`). */
    internal var groupListFailed = false

    /**
     * 관리 목록이 알려 준 그룹 종류(`prearranged`·`chat`)를 세션 그룹에 적어 둔다(데스크톱 `NoteGroupTypes`). GMS 목록은 종류를
     * 싣지 않아, 모르는 채로 두면 채팅 그룹에도 [일제 통화] 가 켜져 보이고 한 번 눌러 거절당한 뒤에야 꺼진다.
     */
    internal fun noteGroupTypes(types: Map<String, String>) {
        if (types.isEmpty()) return
        _groups.value = _groups.value.map { g ->
            types[g.id]?.takeIf { it.isNotBlank() && it != g.sessionType }?.let { g.copy(sessionType = it) } ?: g
        }
    }

    /** 로그인 뒤 한 번 — 관리 범위가 있는 관제사만(없으면 관리 목록 API 가 403 이다). [PTT 그룹] 화면이 목록을 받을 때도 적는다. */
    private suspend fun loadGroupTypes() {
        if (!dispatch.canAdminDirectory) return
        val gen = epoch
        val r = management()?.listGroups() ?: return
        if (gen != epoch || !r.ok) return
        noteGroupTypes(r.value.orEmpty().associate { it.id to it.sessionType })
    }

    internal fun updateGroup(id: String, f: (GroupInfo) -> GroupInfo) {
        _groups.value = _groups.value.map { if (it.id == id) f(it) else it }
    }

    internal fun sessionOf(callId: Int): SessionItem? = _sessions.value.firstOrNull { it.callId == callId }

    internal fun updateSession(callId: Int, f: (SessionItem) -> SessionItem) {
        _sessions.value = _sessions.value.map { if (it.callId == callId) f(it) else it }
    }

    /**
     * 착신·미디어 이벤트 → 세션. 이 둘은 호 상태와 **다른 흐름**으로 와서, 끝난 호의 것이 종료 뒤에 접힐 수 있다 — 낡은
     * 스냅샷을 그대로 얹으면 끝난 호가 카드·착신 배너(벨소리)로 되살아난다. **지금 상태를 코어에 다시 물어** 끝났으면(또는
     * 코어가 이미 잊었으면) 버리고, 살아 있으면 그 최신 값으로 접는다 — 낡은 스냅샷이 조건·상태를 되돌리지도 않는다.
     */
    internal fun applyCallSnapshot(ci: CallInfo, incoming: Boolean) {
        val now = ue?.callInfo(ci.callId) ?: return
        if (now.state == com.cims.ue.sdk.CallState.DISCONNECTED || now.state == com.cims.ue.sdk.CallState.NULL) {
            // 받기 전에 끝난 착신(상대가 곧바로 거뒀다) — 착신의 세션은 이 이벤트로만 선다(코어는 착신의 호 상태를 응답·종료 때에만
            //   낸다). 여기서 버리기만 하면 뒤따르는(또는 앞서 지나간) 종료가 세션을 못 찾아 «부재» 줄이 남지 않는다 — 세웠다가
            //   종료로 접는다. 호 번호는 다시 쓰이므로 같은 상대의 호일 때만이다. 영상 호는 영상 평면이 제 길로 닫는다.
            if (incoming && ci.service != com.cims.ue.sdk.McService.MCVIDEO && now.remoteUri == ci.remoteUri &&
                _sessions.value.none { it.callId == ci.callId }) {
                upsertSession(ci)
                applyCallState(now)
            }
            return
        }
        upsertSession(now)
        // «멤버 확인 전 연결» — 코어는 200 OK 의 `P-Answer-State` 를 성립 이벤트를 낸 **뒤에** 적는다(그 이벤트의 스냅샷에는 비어
        //   있다). 다시 물은 값에는 들어 있으므로 여기서도 본다(한 호에 한 번만 적힌다).
        noteAnswerState(now.callId)
    }

    internal fun upsertSession(ci: CallInfo) {
        if (takeVideoCall(ci)) return            // MCVideo 호 — 카드가 아니라 그 그룹의 «영상» 절에 붙는다(§6.14)
        val cur = _sessions.value
        _sessions.value =
            if (cur.any { it.callId == ci.callId })
                cur.map { if (it.callId == ci.callId) withAlert(it, it.copy(
                    info = ci,
                    connectedAtMs = it.connectedAtMs
                        ?: if (ci.state == com.cims.ue.sdk.CallState.ACTIVE) System.currentTimeMillis() else null,
                    title = it.title.ifEmpty { titleOf(ci) })) else it }
            else cur + withAlert(null, newSession(ci).also(::noteSessionStart))
    }

    /**
     * 긴급 상태를 이어 붙인다 — «언제부터» 를 넘겨주고 개시·해제를 ⑤ 에 남긴다(데스크톱 `UpdateEmergencyBanner`).
     *
     * 세션을 새로 쓰는 길(`upsertSession`·`refreshSessions`)이 전부 여기를 지난다 — 한 곳이라도 빠지면 배너 경과가
     * 처음부터 다시 세어지거나 «개시» 가 두 번 남는다. 규칙 자체는 순수 함수 `alertTransition` 이 갖는다.
     */
    private fun withAlert(prev: SessionItem?, next: SessionItem): SessionItem {
        val (since, change) = alertTransition(prev, next, System.currentTimeMillis())
        val gid = next.info.groupId
        when (change) {
            is AlertChange.Started -> addActivity(gid, channelNameOf(next),
                "${change.kind.label} 개시" + next.alertInitiator
                    .takeIf { it.isNotBlank() }?.let { " · " + displayName(it) }.orEmpty(),
                ActivityKind.EMERGENCY, emergency = true)
            is AlertChange.Cleared -> addActivity(gid, channelNameOf(next), "${change.kind.label} 해제",
                ActivityKind.EMERGENCY, emergency = true)
            AlertChange.None -> Unit
        }
        return if (next.alertSinceMs == since) next else next.copy(alertSinceMs = since)
    }

    /**
     * 세션 조건 변화(서버 재광고·내 상향/하향의 확정·거절, `onMcpttCondition`) — **조건만** 옮긴다. 이 이벤트는 호 상태와
     * 다른 흐름으로 와서 끝난 호의 것이 늦게 닿을 수 있다 — 그 호를 되살리지 않고, 상태·미디어 같은 다른 필드를 이 이벤트의
     * 스냅샷으로 되돌리지 않는다.
     */
    internal fun applyCondition(ch: com.cims.ue.sdk.ConditionChange) {
        val next = ch.call.condition
        // 내가 올린 상향·하향을 서버가 거절했다 — 코어가 이전 값으로 되돌렸다. 배너가 그대로인 이유를 적는다(§6.2a-2).
        val callId = ch.call.callId
        val cancel = if (ch.cause == com.cims.ue.sdk.ConditionCause.LOCAL) callId in conditionCancel else conditionCancel.remove(callId)
        if (ch.cause == com.cims.ue.sdk.ConditionCause.DENIED) {
            // 어느 채널이 거절됐는지 적는다 — 여러 채널에 참여 중이면 문장만으로는 알 수 없다
            val where = sessionOf(callId)?.let { channelNameOf(it) }.orEmpty()
            notify(NoticeLevel.ERROR, (if (where.isNotEmpty()) "$where — " else "") +
                ResponseText.sip(if (cancel) TextArea.EMERGENCY_CANCEL else TextArea.EMERGENCY, next.lastCode, ""),
                "${next.lastCode}".trim())
        }
        _sessions.value = _sessions.value.map { s ->
            if (s.callId != ch.call.callId || s.info.condition == next) s
            else withAlert(s, s.copy(info = s.info.copy(condition = next)))
        }
    }

    internal fun removeSession(callId: Int) {
        _sessions.value = _sessions.value.filterNot { it.callId == callId }
        adhocMembers.remove(callId)
        operations.remove(callId)               // 쓰이지 않은 조작 표시 — 그 id 를 물려받은 다음 호에 붙지 않게
        conditionCancel.remove(callId)          // 답을 못 받은 해제 요청 — 다음 호의 거절 문구가 되지 않게
        broadcastPending.remove(callId)
        if (callId in _rxLevels.value) _rxLevels.value = _rxLevels.value - callId
    }

    private val _rxLevels = MutableStateFlow<Map<Int, Float>>(emptyMap())
    /**
     * 호별 수신 음량(1.0 = 원음) — 감청·청취 행의 음량 막대가 읽는다. 바꾼 적 없는 호는 없다(= 1.0). 호가 끝나면 빠진다
     * (pjsua 가 callId 를 되쓰므로 남기면 다음 호가 앞 호의 음량으로 선다).
     */
    val rxLevels: StateFlow<Map<Int, Float>> = _rxLevels.asStateFlow()

    internal fun noteRxLevel(callId: Int, level: Float) { _rxLevels.value = _rxLevels.value + (callId to level) }

    /**
     * 이 호를 만든 조작을 적는다. **호 이벤트가 명령 반환보다 먼저** 왔으면 세션이 이미 기본 동작(발신·착신)으로 서 있다 —
     * 그 자리에서 고친다(코어는 `makeCall` 중의 이벤트를 다른 스레드로 넘겨 순서가 정해지지 않는다). 지도에 남겨 두면 당겨받기가
     * «발신» 으로 적히고, 그 id 를 물려받은 다음 호가 이 조작으로 적힌다.
     */
    internal fun noteOperation(callId: Int, op: Operation) {
        if (_sessions.value.any { it.callId == callId }) updateSession(callId) { it.copy(operation = op) }
        else operations[callId] = op
    }

    /**
     * 내가 끊은 호 — 연결 전에 끊으면 SIP 는 CANCEL → `487 Request Terminated` 로 끝난다. 그것은 실패가 아니라 내가 거둔 것이라
     * 오류 토스트를 띄우지 않는다(`noteFailedAttempt`). 호가 끝나면 지운다(pjsua 가 callId 를 되쓴다).
     */
    private val localHangups = HashSet<Int>()
    /** 살아 있는 호에만 적는다 — 이미 끝난 호에 적어 두면 지워지지 않고 남아, 그 id 를 물려받은 다음 호의 실패 사유를 삼킨다. */
    internal fun noteLocalHangup(callId: Int) { if (isCallAlive(callId)) localHangups.add(callId) }

    /** 그 호가 코어에 살아 있는가 — 끝난 호(코어는 끝난 호의 정보를 얼마간 보존한다)·없는 id 는 아니다. */
    internal fun isCallAlive(callId: Int): Boolean =
        ue?.callInfo(callId)?.let { it.state != CallState.DISCONNECTED && it.state != CallState.NULL } == true

    /** floor 요청·해제는 **한 줄로** 보낸다 — 서로 다른 코루틴에서 나가면 짧게 눌렀다 뗄 때 해제가 요청을 앞질러 요청만 남는다. */
    internal val floorCmd = kotlinx.coroutines.sync.Mutex()
    internal fun takeLocalHangup(callId: Int): Boolean = localHangups.remove(callId)

    /** 상담 호 → 원 통화 — 세션이 설 때(`newSession`) 붙인다. 이미 서 있으면(이벤트가 먼저 왔다) 곧바로 붙인다. */
    private val consultOf = mutableMapOf<Int, Int>()

    internal fun noteConsult(callId: Int, originalCallId: Int) {
        if (sessionOf(callId) != null) {
            updateSession(callId) { it.copy(consultFor = originalCallId, operation = Operation.TRANSFER) }
            operations.remove(callId)
        } else consultOf[callId] = originalCallId
    }

    internal fun noteTransfer(callId: Int, note: String) = updateSession(callId) { it.copy(transferNote = note) }

    /**
     * 일제 통화로 연 내 호 — 첫 서버 floor 메시지가 B-bit 를 싣는지 본다(`applyFloor`). 없으면 서버가 일반 통화로 연 것이다
     * (진행 중 통화 합류 · 서버가 이 호 종류의 일제 통화를 받지 않음). 이벤트 스레드와 화면 스레드가 함께 쓴다.
     */
    internal val broadcastPending: MutableSet<Int> = java.util.Collections.synchronizedSet(HashSet())

    /**
     * 애드혹 참가자 — **앱이 기억한다**.
     *
     * 서버에 편성이 없는 그룹이라 로스터 구독 대상도 아니다. 카드에 «3명» 을 적으려면 개설할 때
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
    internal fun setWatched(next: Set<String>) { watched = next }
    /** 감시 구독 실패를 이미 알렸다 — 재시도가 또 실패해도 토스트를 쌓지 않는다(성공하면 풀린다). */
    internal var watchFailureNoted = false

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

    private var callLogSeq = 0L

    internal fun addCallLog(row: CallLogRow) {
        _callLog.value = (listOf(row.copy(id = ++callLogSeq)) + _callLog.value).take(CALL_LOG_LIMIT)
        refreshTally()
    }

    /** «오늘 데스크» — 오늘 끝난 줄에서 다시 센다. 줄이 설 때와 1분마다(자정을 넘기면 0 에서 다시 시작한다). */
    internal fun refreshTally() {
        val midnight = java.time.LocalDate.now().atStartOfDay(java.time.ZoneId.systemDefault()).toInstant().toEpochMilli()
        _tally.value = deskTallyOf(_callLog.value, midnight)
    }

    /** 대표번호 dialog 에서 본 발신자와 그 시각 — 포크된 그룹원 leg·내 포크 leg 의 종료를 «대표번호 호의 일부» 로 가를 때 쓴다. */
    private val pilotCallers = HashMap<String, Long>()
    internal fun notePilotCaller(caller: String) {
        if (caller.isEmpty()) return
        val now = System.currentTimeMillis()
        if (pilotCallers.size > 64) pilotCallers.entries.removeAll { now - it.value > 60_000L }
        pilotCallers[caller] = now
    }
    /** 그 발신자의 대표번호 dialog 를 마지막으로 본 때(없으면 null). */
    internal fun pilotCallerSeenAt(caller: String): Long? = pilotCallers[caller]

    /** 서버 통합 이력 폴링의 진행 상태(커서·중복 제거) — `HistoryFeed.kt`. */
    internal val historyFeed = HistoryFeedState()
    private var historyJob: kotlinx.coroutines.Job? = null

    private var activitySeq = 0L

    /**
     * ⑤ 에 **지난 시각**의 줄을 끼운다 — 서버 이력은 수초 늦게 오고 제 시각을 싣는다. 최신이 앞이라는 순서를 지키려고
     * 시각으로 자리를 찾는다(대개 맨 앞 근처라 싸다).
     */
    internal fun addActivityAt(atMs: Long, groupId: String, groupName: String, text: String,
                               kind: ActivityKind, emergency: Boolean = false) {
        val row = ActivityRow(atMs, groupId, groupName, text, kind, emergency, ++activitySeq)
        val cur = recentActivity()
        val at = cur.indexOfFirst { it.atMs <= atMs }.let { if (it < 0) cur.size else it }
        _activity.value = (cur.subList(0, at) + row + cur.subList(at, cur.size)).take(ACTIVITY_LIMIT)
    }

    /**
     * 하루 지난 줄을 뺀 목록 — ⑤ 는 «오늘의 작업 메모리» 다(데스크톱 `ActivityLog.Prune`). 표의 시각은 시:분:초뿐이라 며칠 켜 둔
     * 관제석에서 어제 줄이 남으면 오늘 줄과 구별되지 않는다. 지난 것은 [이력] 이 날짜로 보인다.
     */
    private fun recentActivity(): List<ActivityRow> {
        val cutoff = System.currentTimeMillis() - ACTIVITY_KEEP_MS
        val cur = _activity.value
        return if (cur.isNotEmpty() && cur.last().atMs < cutoff) cur.filter { it.atMs >= cutoff } else cur
    }

    internal fun addActivity(groupId: String, groupName: String, text: String,
                             kind: ActivityKind, emergency: Boolean = false) {
        val row = ActivityRow(System.currentTimeMillis(), groupId, groupName, text, kind, emergency, ++activitySeq)
        _activity.value = (listOf(row) + recentActivity()).take(ACTIVITY_LIMIT)
    }

    /**
     * 그룹 id → 표시 이름. 애드혹은 편성이 없는 임시 그룹이라 이름이 없다 — id(`adhoc-<번호>-<초>`)를 그대로
     * 보이지 않고 «애드혹» 으로 적는다(데스크톱 `TitleOf` 와 같다). 표시 전용이다 — 키로 쓰지 않는다.
     */
    internal fun groupNameOf(groupId: String): String =
        _groups.value.firstOrNull { it.id == groupId }?.name ?: if (isAdhocId(groupId)) "애드혹" else groupId

    /**
     * 세션의 ⑤ 이벤트 줄 이름 — 그룹 호는 지금의 그룹 이름, **개별 통화는 상대 이름**이다(그 `groupId` 는 번호라 그룹 이름으로
     * 풀면 번호가 그대로 선다 — `titleOf` 와 같은 이유). 표시 전용이다.
     */
    internal fun channelNameOf(s: SessionItem): String =
        if (s.info.isMcptt && s.info.mcptt.privateCall) s.title.ifEmpty { displayName(s.info.groupId) }
        else groupNameOf(s.info.groupId)

    /** URI → 표시명. 전화번호부에 있으면 이름, 없으면 번호부(user part)를 그대로 쓴다. */
    internal fun displayName(uri: String): String {
        val num = userPart(uri).ifEmpty { return uri }
        return nameIndex[DirectoryBook.normalize(num, countryCode())] ?: DirectoryBook.displayNumber(num)
    }

    /**
     * 전화 회선의 지금 상태 — «통화 중»·«링잉». **감시 중인 회선만** 안다(dialog 구독 — 관제 그룹원·감시 범위), 모르면 빈 문자열.
     * 주소록·검색이 걸기 전에 보인다(데스크톱 `RefreshStatus`).
     */
    fun lineStatusOf(number: String): String {
        val key = DirectoryBook.normalize(userPart(number))
        if (key.isEmpty()) return ""
        val mine = _dialogs.value.filter { it.isLive && DirectoryBook.normalize(userPart(it.watched)) == key }
        return when {
            mine.any { it.isConfirmed } -> "통화 중"
            mine.any { it.isEarly } -> "링잉"
            else -> ""
        }
    }

    /**
     * PTT 가입자의 지금 상태 — «<그룹> 발언»·«<그룹> 참여». 로스터에 접속으로 잡힌 그룹이 근거라(내 멤버 그룹·청취 범위) 모르면
     * 빈 문자열. 발언은 내가 참여·청취 중인 세션에서만 안다.
     */
    fun pttStatusOf(number: String): String = pttStatusMap()[DirectoryBook.normalize(userPart(number))].orEmpty()

    /**
     * 번호(비교 정규형) → 상태. 목록 하나를 그릴 때는 이것을 **한 번** 만들어 넘긴다 — 사람마다 전 그룹의 로스터를 다시 훑으면
     * 가입자가 수백인 사이트에서 발언 이벤트마다 메인이 묶인다.
     */
    fun pttStatusMap(): Map<String, String> {
        val out = HashMap<String, String>()
        _groups.value.forEach { g ->
            val speaker = _sessions.value.firstOrNull { it.info.groupId == g.id }?.speaker.orEmpty()
            g.roster.forEach { e ->
                if (!e.status.equals("connected", true)) return@forEach
                val key = DirectoryBook.normalize(userPart(e.uri))
                if (key.isEmpty() || key in out) return@forEach
                out[key] = g.name + if (speaker.isNotEmpty() && nameOrEmpty(e.uri) == speaker) " 발언" else " 참여"
            }
        }
        return out
    }

    /** URI → 주소록 이름. 없으면 빈 문자열(번호를 돌려주지 않는다 — «이름이 있는가» 를 가를 때 쓴다). */
    internal fun nameOrEmpty(uri: String): String {
        val num = userPart(uri).ifEmpty { return "" }
        return nameIndex[DirectoryBook.normalize(num, countryCode())].orEmpty()
    }

    /** "1003 이순경" 병기(dispatch_desktop_ui.md §3.2 신원 표시). 이름이 없으면 번호만. */
    fun displayLabel(uri: String): String {
        val num = userPart(uri).ifEmpty { return uri }
        val name = nameIndex[DirectoryBook.normalize(num, countryCode())]
        val shown = DirectoryBook.displayNumber(num)
        return if (name.isNullOrBlank()) shown else "$shown $name"
    }

    private fun countryCode(): String = _profile.value?.countryCode?.ifBlank { "82" } ?: "82"

    /**
     * 회사 전화번호부 적재 — 전화(voip·volte)와 PTT 를 합친다.
     *
     * 실패해도 조용히 넘긴다. 이름이 없으면 번호로 보일 뿐이고, 여기서 막으면 등록까지 막힌다.
     */
    suspend fun refreshDirectory() {
        val m = management() ?: return
        val gen = epoch
        // 로그인 뒤 첫 적재 — 캐시·CSV 로 먼저 그린다(켜자마자 이름이 서게). 서버에는 그 ETag 로 묻는다.
        if (serverBooks.isEmpty()) {
            val (cache, csv) = withContext(Dispatchers.IO) { dirFiles.loadCache() to dirFiles.loadCsv() }
            if (gen != epoch) return                    // 읽는 사이 로그아웃 — 앞 사람의 주소록을 다시 싣지 않는다
            serverBooks.putAll(cache)
            _csvContacts.value = csv
            publishBooks()
        }
        // **전화 가족 축은 `volte` 다** — 서버가 `service=volte` 에 이동(volte)과 유선(voip)을 합산해 준다
        // (csc `handle_provisioning_directory`). 내 회선이 유선이라고 `service=voip` 로 물으면 유선 가입자만
        // 와서 전화번호부가 거의 빈다. 관제 그룹원 `volteAor` 와도 같은 어휘다.
        var changed = false
        var failed = false
        for (svc in listOf(DIR_PHONE, DIR_PTT)) {
            val r = m.directory(svc, serverBooks[svc]?.etag.orEmpty())
            if (gen != epoch) return
            if (!r.ok) failed = true
            r.value?.takeIf { r.ok }?.let { serverBooks[svc] = it; changed = true }   // null = 304(그대로)
        }
        // 캐시도 없는데 못 받았다 — 이름 대신 번호만 보이는 까닭을 한 번 알린다(캐시가 있으면 그것으로 보이므로 조용히)
        if (failed && serverBooks.isEmpty() && !directoryFailureNoted) {
            directoryFailureNoted = true
            notify(NoticeLevel.WARN, "전화번호부를 받지 못했습니다", "이름 대신 번호로 보입니다 — 서버 연결을 확인하세요")
        } else if (!failed) directoryFailureNoted = false
        publishBooks()
        if (changed) serverBooks.toMap().let { snap -> withContext(Dispatchers.IO) { dirFiles.saveCache(snap) } }
    }

    private var directoryFailureNoted = false

    /** 서버 전화번호부(캐시의 원천) — 서비스(`volte`·`ptt`)별 서버가 준 그대로. CSV 를 섞기 전이다. */
    private val serverBooks = HashMap<String, DirectoryBook>()
    private val dirFiles = DirectoryFiles(context.filesDir)

    private val _csvContacts = MutableStateFlow<List<CsvContact>>(emptyList())
    /** 가져온 로컬 CSV 줄 — 설정 «주소록» 이 수를 보인다. */
    val csvContacts: StateFlow<List<CsvContact>> = _csvContacts.asStateFlow()

    /** 서버 + CSV → 두 주소록. 전화는 ext·external 줄, PTT 는 ptt 줄을 섞는다(§6.2b). */
    private fun publishBooks() {
        val cc = countryCode()
        val csv = _csvContacts.value
        _phoneBook.value = mergeBook(serverBooks[DIR_PHONE] ?: DirectoryBook(), csv, setOf(CsvKind.EXT, CsvKind.EXTERNAL), cc)
        _pttBook.value = mergeBook(serverBooks[DIR_PTT] ?: DirectoryBook(), csv, setOf(CsvKind.PTT), cc)
        reindexNames()
    }

    /**
     * 로컬 CSV 가져오기 — 내용을 앱 저장소에 복사해 두고(원본 파일의 접근 권한에 기대지 않는다) 곧바로 섞는다. 반환 = 읽은 줄 수.
     * 한 줄도 못 읽었으면 저장하지 않는다 — 엉뚱한 파일로 앞의 CSV 를 지우지 않게.
     */
    suspend fun importDirectoryCsv(text: String): Int {
        val rows = DirectoryCsv.parse(text)
        if (rows.isEmpty()) return 0
        withContext(Dispatchers.IO) { dirFiles.saveCsv(text) }
        _csvContacts.value = rows
        publishBooks()
        return rows.size
    }

    suspend fun clearDirectoryCsv() {
        withContext(Dispatchers.IO) { dirFiles.deleteCsv() }
        _csvContacts.value = emptyList()
        publishBooks()
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
     * 그다음 보관 기간을 넘긴 것을 지우고(그 말풍선만 가리키던 기기 파일도 함께 — `sweepFiles`) **지금 주인(로그인 ID)의 것만** 적재한다.
     */
    private suspend fun restoreMessages() {
        val gen = epoch
        val before = startEnteredMs
        // 다른 DB 쓰기와 **같은 줄**(`storeDispatcher`)에서 — 따로 돌면 기동 중에 보낸 메시지의 저장과 PENDING 마감이 엇갈린다.
        val (loaded, sms) = withContext(storeDispatcher) {
            runCatching {
                messageStore.failPending(before)
                messageStore.prune(settings.current.messageRetentionDays)   // 설정한 보관 일수(기본 30)
                messageStore.localPaths()?.let { sweepFiles(FileRules.root(context.filesDir), it) }
                messageStore.load(MessageKind.SDS)
            }.getOrElse { emptyMap() } to runCatching { messageStore.load(MessageKind.SMS) }.getOrElse { emptyMap() }
        }
        // 읽는 사이 로그아웃 — 앞 사람의 스레드를 비운 화면에 다시 싣지 않는다(다음 로그인이 그 위에 제 것을 얹게 된다)
        if (gen != epoch) return
        mergeStoredSms(sms)
        if (loaded.isEmpty()) return
        // 기동 중 이미 들어온 것이 있으면 그것이 최신이다 — 보관분 위에 얹는다.
        val live = _messages.value
        _messages.value = loaded.toMutableMap().apply {
            live.forEach { (g, msgs) ->
                // 같은 id 는 **화면의 것**이 최신이다(기동 중에 보낸 메시지 — 보관분은 그 직전 상태다)
                val liveById = msgs.associateBy { it.id }
                val existing = this[g].orEmpty()
                val ids = existing.map { it.id }.toHashSet()
                this[g] = existing.map { liveById[it.id] ?: it } + msgs.filter { it.id !in ids }
            }
        }
    }

    /** 발신 말풍선. 최종 응답이 명령보다 먼저 와 있었으면([early]) 그 상태로 선다 — 처음 저장부터 최종 상태다. */
    internal fun addOutgoingMessage(groupId: String, text: String, msgId: String, token: Long,
                                    early: com.cims.ue.sdk.RequestResult? = null, failed: Boolean = false) {
        val m = Message(
            id = "out-" + System.nanoTime(), groupId = groupId,
            fromUri = myPttId, fromName = "나", text = text,
            atMs = System.currentTimeMillis(), outgoing = true,
            msgId = msgId, token = token,
            // 명령이 곧바로 실패했으면([failed]) 처음부터 실패로 선다 — 응답이 오지 않을 발신이다(문자 `addOutgoingSms` 와 같다)
            state = if (failed) SendState.FAILED else early.sendState() ?: SendState.PENDING)
        _messages.value = _messages.value + (groupId to ((_messages.value[groupId] ?: emptyList()) + m))
        storeAsync { insert(m) }
        if (m.state == SendState.FAILED && early != null) noteSendFailure(MessageKind.SDS, early.code, early.reason)
    }

    /**
     * 재전송을 시작해도 되는가 — 화면이 넘긴 사본이 아니라 **지금의 말풍선**이 실패일 때만 참이고, 그 자리에서 «보내는 중»
     * 으로 바꿔 둔다. 발신 명령이 도는 동안 [재전송] 을 한 번 더 눌러도 두 번 나가지 않는다(메인 스레드 전용 — 첫 대기 전에 끝난다).
     */
    internal fun beginResend(m: Message): Boolean {
        if (!Resend.allowed(threadsOf(m.kind).value, m)) return false
        threadsOf(m.kind).value = Resend.patch(threadsOf(m.kind).value, m.id) { it.copy(state = SendState.PENDING) }
        return true
    }

    /**
     * 재전송한 말풍선 — **같은 말풍선**(행 id)이 새 token 을 받고 다시 «보내는 중»(응답이 먼저 와 있었으면 그 상태)이
     * 된다. msgId 는 처음 것 그대로다(SDS 는 처음의 msgId 로 다시 보낸다 — [resendSds]; 코어가 다른 값을 돌려주면 그 값).
     * 새 말풍선을 세우지 않는다 — 같은 말이 두 번 보이면 두 번 보낸 줄 안다(데스크톱 `ResendCore`).
     */
    internal fun markResent(m: Message, msgId: String, token: Long, failed: Boolean,
                            early: com.cims.ue.sdk.RequestResult?) {
        val st = if (failed) SendState.FAILED else early.sendState() ?: SendState.PENDING
        threadsOf(m.kind).value = Resend.patch(threadsOf(m.kind).value, m.id) {
            it.copy(msgId = msgId.ifEmpty { it.msgId }, token = token, state = st)
        }
        storeAsync { updateResend(m.id, msgId.ifEmpty { m.msgId }, token, st) }
        if (!failed && st == SendState.FAILED && early != null) noteSendFailure(m.kind, early.code, early.reason)
    }

    /** 종류별 스레드 지도 — SDS 는 PTT 채널, SMS 는 전화 축(섞지 않는다 — [MessageKind]). */
    private fun threadsOf(kind: MessageKind) = if (kind == MessageKind.SMS) _sms else _messages

    private fun com.cims.ue.sdk.RequestResult?.sendState(): SendState? =
        this?.let { if (it.code in 200..299) SendState.SENT else SendState.FAILED }

    /**
     * 받은 SDS·FD 알림 → 말풍선. FD(`msg.fd` — FILEURL + 이름·크기·종류)면 **파일 말풍선**으로 선다. 관제석은 자동으로 받지
     * 않는다 — 그룹 파일이 쌓이는 자리라 관제사가 [받기] 로 고른다(`downloadFile`, 데스크톱 §4.4).
     *
     * @return 새 말풍선이 섰는가 — 이미 받은 메시지(같은 id)면 false.
     */
    internal fun addIncomingMessage(groupId: String, msg: com.cims.ue.sdk.SdsMessage): Boolean {
        val m = Message(
            id = msg.msgId.ifEmpty { "in-" + System.nanoTime() }, groupId = groupId,
            fromUri = msg.fromUri, fromName = displayName(msg.fromUri), text = msg.text,
            atMs = if (msg.timeSec > 0) msg.timeSec * 1000L else System.currentTimeMillis(),
            outgoing = false, msgId = msg.msgId, read = false,
            fileName = msg.fileName.ifEmpty { if (msg.fd) FileRules.DEFAULT_NAME else "" }, fileUrl = msg.fileUrl,
            fileType = msg.fileType, fileSize = msg.fileSize)
        // 같은 id 가 이미 있다 = 상대의 재전송(처음 msgId 그대로 — TS 24.282 의 식별자는 메시지에 붙는다)이나 중복 배달. 말풍선을
        //   또 세우면 스레드 목록의 키가 겹쳐 화면이 죽고, 보관은 덮어써져 읽음·받은 파일 경로를 잃는다. 처음 것을 둔다.
        val thread = _messages.value[groupId] ?: emptyList()
        if (thread.any { it.id == m.id }) return false
        _messages.value = _messages.value + (groupId to (thread + m))
        storeAsync { insert(m) }
        return true
    }

    /**
     * 발신 파일 말풍선 — **올리기 전에** 먼저 선다([note] = «올리는 중…», [localPath] = 앱이 둔 사본). FILEURL·msgId·token 은
     * 업로드·FD 알림이 끝나며 채워진다([patchMessage] — `FilePlane.sendFile`).
     */
    internal fun addOutgoingFile(groupId: String, name: String, size: Long, type: String, localPath: String, note: String): Message {
        val m = Message(
            id = "out-" + System.nanoTime(), groupId = groupId,
            fromUri = myPttId, fromName = "나", text = "",
            atMs = System.currentTimeMillis(), outgoing = true, state = SendState.PENDING,
            fileName = name, fileSize = size, fileType = type, localPath = localPath, transferNote = note)
        _messages.value = _messages.value + (groupId to ((_messages.value[groupId] ?: emptyList()) + m))
        storeAsync { insert(m) }
        return m
    }

    /**
     * SDS 말풍선 하나를 고친다(행 id) — 파일 평면이 진행 문구·기기 경로·FILEURL·발신 상태를 바꾼다. 고친 값을 돌려준다(그 사이
     * 로그아웃 등으로 말풍선이 사라졌으면 null — 호출자는 거기서 멈춘다). [persist] = 보관에도 적는다 — 진행 문구만 바꿀 때는
     * 끈다(보관하지 않는 값이다).
     */
    internal fun patchMessage(id: String, persist: Boolean = true, f: (Message) -> Message): Message? {
        _messages.value = Resend.patch(_messages.value, id, f)
        val now = _messages.value.values.firstNotNullOfOrNull { list -> list.firstOrNull { it.id == id } } ?: return null
        if (persist) storeAsync { updateFile(now) }
        return now
    }

    /** disposition 통지(1 미달·2 전달·3 읽음) → 발신 말풍선 상태. */
    internal fun updateSendState(msgId: String, notifType: Int) {
        if (msgId.isEmpty()) return
        // TS 24.282 §15.2.5 — 1 UNDELIVERED · 2 DELIVERED · 3 READ · 4 DELIVERED AND READ
        val st = when (notifType) { 1 -> SendState.FAILED; 2 -> SendState.DELIVERED; 3, 4 -> SendState.READ; else -> return }
        var changed = false
        _messages.value = _messages.value.mapValues { (key, list) ->
            list.map { m ->
                if (m.msgId != msgId || !m.outgoing) return@map m
                val keep = when (st) {
                    // 그룹 SDS 는 받는 사람이 여럿이다 — 한 명의 미전달로 말풍선 전체를 실패로 바꾸지 않는다(재전송은 그룹 전체로
                    //   나간다). 이미 전달·읽음이 온 말풍선도 되돌리지 않는다.
                    SendState.FAILED -> groups.value.any { it.id == key } || m.state == SendState.DELIVERED || m.state == SendState.READ
                    SendState.DELIVERED -> m.state == SendState.READ          // 읽음을 전달로 되돌리지 않는다
                    else -> false
                }
                if (keep || m.state == st) m else { changed = true; m.copy(state = st) }
            }
        }
        if (changed) storeAsync { setStateByMsgId(msgId, st) }
    }

    /**
     * SIP 요청의 최종 응답.
     *
     * 메시지 발신은 token 으로 상관하고, **SUBSCRIBE 실패는 로그로 남긴다** — 조용히 삼키면 구독이
     * 거절된 것과 통화가 없는 것을 구분할 수 없다(감시 대상 통화가 «안 보이는» 가장 흔한 원인).
     */
    internal fun applyRequestResult(r: com.cims.ue.sdk.RequestResult) {
        // 제휴 PUBLISH — 유지 평면이 최종 응답을 기다린다(2xx 만 제휴로 적는다, `affiliateConfirmed`).
        upkeep.waiters.remove(r.token)?.let { it.complete(r); return }
        // SDS 전달 확인 회신 — 말풍선이 없는 발신이라 최종 거절은 로그로만 남는다(`applySds`).
        notificationTokens.remove(r.token)?.let { what -> logNotificationResult(what, r); return }
        // 경보 취소 — 배너는 보낼 때 먼저 내렸다. 서버가 거절(403)했으면 되살린다.
        alertCancelTokens.remove(r.token)?.let { b -> applyAlertCancelResult(b, r); return }
        if (r.method.equals("SUBSCRIBE", ignoreCase = true) && r.code !in 200..299) {
            android.util.Log.w("DispatchSession", "dialog 구독 실패 ${r.code} ${r.reason}")
            return
        }
        // 짝이 아직 token 을 모르면(발신 명령이 아직 안 돌아왔다) 들고 있는다 — 말풍선·회신이 설 때 꺼내 간다.
        if (!applyRequestResult(r.token, r.code, r.reason)) earlyResults.park(r.token, r)
    }

    /** 보낸 문자가 거절됐다 — 말풍선의 ⚠ 만으로는 이유를 모른다(데스크톱 `OnRequestResult` 의 토스트). */
    private fun noteSendFailure(kind: MessageKind, code: Int, reason: String) {
        val area = if (kind == MessageKind.SMS) TextArea.SMS else TextArea.SDS
        notify(NoticeLevel.ERROR, ResponseText.sip(area, code, reason), "$code $reason".trim())
    }

    internal fun logNotificationResult(what: String, r: com.cims.ue.sdk.RequestResult) {
        if (r.code !in 200..299) android.util.Log.w(SDS_TAG, "전달 확인 회신 거절 $what: ${r.code} ${r.reason}")
    }

    /** 발신 최종 응답(token 상관) → 보냄/실패. 기다리던 말풍선이 있었으면 true. 실패면 토스트도 띄운다. */
    internal fun applyRequestResult(token: Long, code: Int, reason: String = ""): Boolean {
        if (token <= 0) return false
        fun waiting(m: Map<String, List<Message>>) =
            m.values.any { list -> list.any { it.token == token && it.outgoing && it.state == SendState.PENDING } }
        val sds = waiting(_messages.value)
        if (!sds && !waiting(_sms.value)) return false
        val st = if (code in 200..299) SendState.SENT else SendState.FAILED
        if (st == SendState.FAILED) noteSendFailure(if (sds) MessageKind.SDS else MessageKind.SMS, code, reason)
        fun patch(m: Map<String, List<Message>>) = m.mapValues { (_, list) ->
            list.map {
                if (it.token == token && it.outgoing && it.state == SendState.PENDING) it.copy(state = st)
                else it
            }
        }
        _messages.value = patch(_messages.value)
        _sms.value = patch(_sms.value)          // 문자도 같은 token 으로 온다
        storeAsync { setStateByToken(token, st) }
        return true
    }

    // ── 전화 축 문자(SMS/LMS) ──────────────────────────────────────────────
    private fun mergeStoredSms(stored: Map<String, List<Message>>) {
        if (stored.isEmpty()) return
        // 대화 키는 정규형이다(`smsKey`) — 원 번호로 보관된 옛 대화를 같은 사람의 대화로 모으고 보관도 고쳐 둔다
        val loaded = LinkedHashMap<String, List<Message>>()
        stored.forEach { (k, msgs) ->
            val key = smsKey(k)
            loaded[key] = (loaded[key].orEmpty() + msgs.map { if (it.groupId == key) it else it.copy(groupId = key) }).sortedBy { it.atMs }
            if (key != k) storeAsync { rekey(k, key, MessageKind.SMS) }
        }
        val live = _sms.value
        _sms.value = loaded.toMutableMap().apply {
            live.forEach { (k, msgs) ->
                val liveById = msgs.associateBy { it.id }
                val ids = this[k].orEmpty().map { it.id }.toHashSet()
                this[k] = this[k].orEmpty().map { liveById[it.id] ?: it } + msgs.filter { it.id !in ids }
            }
        }
    }

    /**
     * 문자 발신 말풍선. 명령이 곧바로 실패했으면([failed]) 처음부터 실패로 선다 — 응답이 오지 않을 발신이라
     * «보내는 중» 으로 세우면 재기동 때까지 그대로 멈춘다. 최종 응답이 먼저 와 있었으면([early]) 그 상태로 선다.
     */
    internal fun addOutgoingSms(peer: String, text: String, token: Long, failed: Boolean = false,
                                early: com.cims.ue.sdk.RequestResult? = null) {
        val st = if (failed) SendState.FAILED else early.sendState() ?: SendState.PENDING
        val m = Message(
            id = "sms-out-" + System.nanoTime(), groupId = peer,
            fromUri = "", fromName = "나", text = text,
            atMs = System.currentTimeMillis(), outgoing = true,
            token = token, state = st, kind = MessageKind.SMS)
        _sms.value = _sms.value + (peer to ((_sms.value[peer] ?: emptyList()) + m))
        storeAsync { insert(m) }
        if (!failed && st == SendState.FAILED && early != null) noteSendFailure(MessageKind.SMS, early.code, early.reason)
    }

    internal fun addIncomingSms(peer: String, fromUri: String, text: String) {
        val m = Message(
            id = "sms-in-" + System.nanoTime(), groupId = peer,
            fromUri = fromUri, fromName = displayName(fromUri), text = text,
            atMs = System.currentTimeMillis(), outgoing = false, read = false,
            kind = MessageKind.SMS)
        _sms.value = _sms.value + (peer to ((_sms.value[peer] ?: emptyList()) + m))
        storeAsync { insert(m) }
    }

    /** 문자 스레드를 읽음 처리. */
    fun markSmsRead(peer: String) {
        _sms.value = _sms.value.mapValues { (k, list) ->
            if (k == peer) list.map { if (!it.read) it.copy(read = true) else it } else list
        }
        storeAsync { markRead(peer, MessageKind.SMS) }
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
                // 끝난 dialog 행은 3초 보이고 내려간다 — 다음 NOTIFY 를 기다리면 조용한 동안 남아 틱이 멈추지 않는다
                if (_dialogs.value.any { it.isTerminated && it.elapsedMs > 3_000L })
                    _dialogs.value = _dialogs.value.filterNot { it.isTerminated && it.elapsedMs > 3_000L }
                if (_sessions.value.isNotEmpty() || _dialogs.value.isNotEmpty()) _tick.value++
            }
        }
    }

    private var observing = false

    /** 코어 이벤트를 받아 상태를 갱신한다. 엔진이 선 뒤 [start] 가 한 번만 부른다. */
    /** 로그아웃 상태 — 엔진 이벤트 수집자가 늦게 온 이벤트를 버릴 때 본다. */
    private val loggedOut: Boolean get() = _state.value == SessionState.LOGGED_OUT

    private fun observe() {
        if (observing) return
        observing = true
        startTicker()
        val engine = ue ?: return
        scope.launch {
            engine.registrations.collect { regs ->
                if (loggedOut) return@collect
                guarded("registrations") {
                    noteRegistrationFailures(_registrations.value, regs)
                    _registrations.value = regs
                    if (regs.values.any { it.registered }) refreshServerCert()   // TLS 등록 = 새 핸드셰이크
                }
            }
        }
        scope.launch {                          // 잔여 일수는 시간이 가며 줄어든다 — 관측이 그대로여도 다시 판정
            while (true) {
                kotlinx.coroutines.delay(60_000); guarded("minute") { refreshServerCert(); refreshTally() }
                // 토큰 선제 갱신 — 조회가 하나도 없는 자리(전화 전용 계정)에서도 만료 전에 갱신하고, 실패하면 경고 띠가 미리 선다
                if (isReady) runCatching { accessToken() }
            }
        }
        // 이벤트는 건마다 받는다([guarded]) — 하나가 던져도 그 종류의 수집이 끝나지 않는다.
        // 로그아웃한 뒤(다음 로그인 전)에 늦게 닿은 이벤트는 버린다([loggedOut]) — 로그아웃은 호를 끊고 등록을 푸는 동안에도
        //   이벤트가 오고, 그것을 접으면 비운 화면에 앞 사람의 말풍선·경보·통화 줄이 다시 선다.
        scope.launch { engine.serviceAuth.collect { if (!loggedOut) guarded("serviceAuth") { applyServiceAuth(it) } } }
        scope.launch { engine.callState.collect { if (!loggedOut) guarded("callState") { applyCallState(it) } } }
        scope.launch { engine.incomingCall.collect { if (!loggedOut) guarded("incomingCall") { applyCallSnapshot(it, incoming = true) } } }
        scope.launch { engine.callMedia.collect { if (!loggedOut) guarded("callMedia") { applyCallSnapshot(it, incoming = false) } } }
        scope.launch { engine.condition.collect { if (!loggedOut) guarded("condition") { applyCondition(it) } } }     // 진행 중 긴급·임박(§6.2a-1)
        scope.launch { engine.floor.collect { if (!loggedOut) guarded("floor") { applyFloor(it) } } }
        scope.launch { engine.roster.collect { if (!loggedOut) guarded("roster") { applyRoster(it) } } }
        scope.launch { engine.sds.collect { if (!loggedOut) guarded("sds") { applySds(it) } } }
        scope.launch { engine.requestResult.collect { if (!loggedOut) guarded("requestResult") { applyRequestResult(it) } } }
        // SIP MESSAGE text/plain — 전화 축 문자(volte_supplementary_services.md §4.3) · xcap-diff NOTIFY — 그룹 문서 변경(RFC 5875).
        scope.launch { engine.message.collect { if (!loggedOut) { guarded("message") { applySipMessage(it) }; guarded("xcap-diff") { applyXcapDiff(it) } } } }
        scope.launch { engine.dialogInfo.collect { if (!loggedOut) guarded("dialogInfo") { applyDialog(it) } } }
        scope.launch { engine.emergencyAlert.collect { if (!loggedOut) guarded("emergencyAlert") { applyEmergencyAlert(it) } } }      // 긴급 경보(TS 24.379 §12.1.1.3)
        scope.launch { engine.nonAcknowledged.collect { if (!loggedOut) guarded("nonAcknowledged") { applyNonAcknowledged(it) } } }    // 미응답 필수 멤버(§6.3.3.3)
        observeVideo(engine)                    // MCVideo 송출·수신 제어(TS 24.581 §6.2.4·§6.2.5)·영상 채널 맞춤(§6.14)
        observeUpkeep(engine)                   // 등록에 묶인 제휴·구독의 복원과 갱신(§6.7a)
        // 주기 재조회 — 관제 편성(`/provisioning/me`, 60초)과 CMS 문서(5분). 둘 다 ETag 라 안 바뀌었으면 304 로 끝난다.
        scope.launch {
            while (true) {
                kotlinx.coroutines.delay(DISPATCH_POLL_MS)
                if (isReady && hasDesk) runCatching { refreshDispatch() }
            }
        }
        scope.launch {
            while (true) {
                kotlinx.coroutines.delay(CMS_POLL_MS)
                if (isReady && pttAccount != null) runCatching { refreshCms() }
            }
        }
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
        val s = settings.current
        // 헤드셋·블루투스면 **선호 이어폰**을 먼저 고른다 — 여럿이 붙어 있을 때 첫 것으로 가지 않게(§8).
        val h = pickHeadset(s.audioRoute, audio.headsets.value, s.preferredHeadset)
        if (h != null) audio.selectHeadset(h.id) else audio.setRoute(s.audioRoute)
        audio.ensureRxVolume()
    }

    // ── 종료 ──────────────────────────────────────────────────────────────────
    /**
     * 로그아웃 — 살아 있는 세션을 끊고 등록을 풀고 자격을 지운다.
     *
     * 엔진은 살려 둔다(재로그인이 재사용한다). 계정은 **제거**한다 — 남겨 두면 다음 로그인에서 같은 계정이
     * 두 번 올라간다.
     *
     * @param forgetLogin 저장된 자동 로그인(refresh token)도 지운다. [로그아웃]·자격 만료는 지우고(다음 기동은 로그인 화면),
     *   **[앱 종료] 는 남긴다** — 다음 기동이 그 토큰으로 이어 로그인한다(데스크톱 `Logout(forgetLogin)` 과 같은 규칙).
     */
    fun logout(forgetLogin: Boolean = true) {
        manualLoginTried = false
        epoch++                     // 진행 중인 기동의 게시를 무효화한다(§F4)
        _loginGeneration.value++    // 화면 캐시·폼을 버리게 한다(계정 격리)
        resetVideo()                // 영상 채널 — 재합류하지 않게 호를 끊기 전에 먼저 비운다(§6.14)
        upkeep.reset()              // 앞 사람의 제휴·구독을 다음 로그인이 «다시 세울 것» 으로 읽지 않게
        val engine = ue
        val accs = _accounts.value.values.toList() + extraAccounts
        extraAccounts = emptyList()
        accountKinds.clear()
        scope.launch {
            // 통화·무전을 먼저 끊는다 — 등록을 풀기 전에 BYE 가 나가야 서버 쪽 세션이 남지 않는다.
            engine?.calls()?.forEach { runCatching { engine.call(it).hangup() } }
            accs.forEach { a ->
                runCatching { a.unregister() }
                runCatching { a.remove() }
            }
        }
        if (forgetLogin) store.clear()
        noteTokens(null)
        loginPassword = ""
        _profile.value = null
        _accounts.value = emptyMap()
        userProfile = null; serviceConfig = null; ueInit = null; conditionCancel.clear()
        _capabilities.value = Capabilities()
        _alertBanners.value = emptyList(); alertCancelTokens.clear()
        historyJob?.cancel(); historyJob = null
        historyFeed.clear()
        profileEtag = ""; groupRefreshSeq++            // 기다리던 그룹 재조회는 차례가 어긋나 스스로 그만둔다
        _sessions.value = emptyList()
        _groups.value = emptyList()
        _activity.value = emptyList()
        // 보관은 지우지 않는다 — 같은 로그인 ID 로 다시 로그인하면 스레드가 이어져야 한다. 화면의 스레드는 둘 다 비운다 —
        //   보관은 로그인 ID 로 격리돼 있어(`MessageStore.owner`) 다음 로그인이 제 것만 다시 읽는다(§6.9).
        _messages.value = emptyMap()
        _sms.value = emptyMap()
        _dialogs.value = emptyList()
        _callLog.value = emptyList()
        _phoneBook.value = DirectoryBook()
        _pttBook.value = DirectoryBook()
        serverBooks.clear()                     // 다음 로그인은 캐시부터 다시 그린다(파일은 남는다)
        nameIndex = emptyMap()
        _tally.value = DeskTally()
        watched = emptySet()
        refreshFailures = 0
        _credentialWarning.value = null
        _registrations.value = emptyMap()
        operations.clear()
        consultOf.clear()
        adhocMembers.clear()
        broadcastPending.clear()
        localHangups.clear()
        pilotCallers.clear()
        // 실패 알림 깃발 — 앞 사람 때 이어지던 실패가 다음 사람의 첫 경고를 삼키지 않게
        groupListFailed = false; watchFailureNoted = false; directoryFailureNoted = false
        notificationTokens.clear()              // 끝난 계정의 회신은 더 맞출 곳이 없다
        earlyResults.clear()
        _notices.value = emptyList()
        hwPtt.releaseStuck()                    // 누른 채 나간 키 — 다음 사람의 발언이 되지 않게
        _serverCert.value = null
        _rxLevels.value = emptyMap()
        _error.value = null
        _state.value = SessionState.LOGGED_OUT
    }

    override fun close() {
        ticker?.cancel(); ticker = null
        runCatching { messageStore.close() }
        runCatching { audio.close() }
        csc?.let(::closeLater)
        fdCsc?.second?.let(::closeLater)
        // 엔진 닫기는 진행 중인 호출이 끝나기를 기다린 뒤 pjsua 를 내린다(등록 해제 응답까지) — 메인에서 하면 망이 끊겼을 때
        //   [앱 종료] 가 그만큼 멎는다. 참조를 먼저 비우고 닫기만 따로 보낸다.
        ue?.let { e -> Thread({ runCatching { e.close() } }, "ue-close").apply { isDaemon = true }.start() }
        ue = null
        csc = null; ue = null; mgmt = null; fdCsc = null
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

    /**
     * 낡은 CSC 핸들을 **메인 밖에서** 닫는다. `CscClient.close()` 는 진행 중인 요청이 끝날 때까지 기다린다(요청 시한 = 소켓 동작당
     * 15초) — 메인에서 닫으면 서버가 느리거나 안 닿을 때 다시 로그인하는 순간 화면이 그만큼 멈춘다. 참조는 호출자가 먼저 갈아
     * 끼우고, 닫기만 따로 보낸다(세션 스코프가 이미 끝났을 수 있어 스레드를 쓴다).
     */
    private fun closeLater(c: CscClient) {
        Thread({ runCatching { c.close() } }, "csc-close").apply { isDaemon = true }.start()
    }

    private fun makeCsc(host: String, port: Int): CscClient {
        csc?.let(::closeLater)
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

    /**
     * 등록 실패 → 토스트(데스크톱 `OnReg` 의 «등록 실패 — …»). **실패로 바뀔 때만** 띄운다 — 계정의 자동 재시도
     * (`regConfig.retryIntervalSec`, 30초)가 실패할 때마다 오류 토스트가 쌓이면 다른 실패를 가린다. 데스크톱의 앱 쪽
     * 재등록 백오프는 두지 않는다 — 코어 계정이 이미 재시도한다(두 겹이면 REGISTER 가 두 배로 나간다).
     */
    private fun noteRegistrationFailures(before: Map<Int, RegInfo>, after: Map<Int, RegInfo>) {
        after.forEach { (id, ri) ->
            if (ri.state != RegState.FAILED || before[id]?.state == RegState.FAILED) return@forEach
            // 이 로그인이 만든 계정만 — 엔진의 등록 표는 지운 계정의 줄을 남기고(계정 id 는 늘기만 한다), 재로그인 뒤 첫
            //   스냅샷에 그 FAILED 줄이 «새 실패» 로 보인다. 게시 전에 닿은 이벤트도 종류를 안다(`accountKinds`).
            val name = when (accountKinds[id]) {
                AccountKind.PTT -> "PTT"
                AccountKind.PHONE -> "전화"
                null -> return@forEach
            }
            notify(NoticeLevel.ERROR, "$name 등록 실패 — ${ResponseText.sip(TextArea.REGISTER, ri.code, ri.reason)}",
                "${ri.code} ${ri.reason}".trim())
        }
    }

    /** 등록이 하나라도 살아 있는가 — 상단 바 점등. */
    fun anyRegistered(): Boolean {
        // 지금 계정의 것만 본다 — 엔진의 등록 표는 지운 계정의 줄을 남긴다(계정 id 는 늘기만 한다)
        val mine = _accounts.value.values.mapTo(HashSet()) { it.id }
        return _registrations.value.any { (id, r) -> id in mine && r.state == RegState.REGISTERED }
    }

    /**
     * 망이 돌아왔거나 바뀌었다 — 코어에 알린다. **등록 복구는 코어가 한다**(`Engine::handleNetworkChange` — 옛 TCP/TLS
     * 연결을 닫고 등록을 켠 계정마다 다시 등록하며, 앞 등록이 걸려 있으면 끝난 뒤 한 번 더). 판정(«복귀·전환인가»)은
     * `DispatchService` 의 SDK 접점 `NetworkWatcher` 가 한다. 엔진이 없으면(로그인 전) 할 일이 없다.
     *
     * 계정마다 REGISTER 만 다시 거는 것(`refreshRegistration`)으로는 모자라다 — 옛 망의 연결을 재사용하고, 진행 중 등록이
     * 있으면 `PJSIP_EBUSY` 로 거절돼 요청이 사라진다. 망 콜백 스레드에서 불린다 — 엔진은 세션 스코프 안에서 읽는다.
     *
     * **등록에 묶인 것은 앱이 다시 싣는다** — 서버는 끊긴 연결의 바인딩을 회수하며 제휴(affiliation)를 내리고, 코어의 재등록은
     * 등록만 되살린다. 망이 바뀐 것을 적어 두면 그 뒤 첫 등록 성공에 유지 평면이 제휴·구독을 다시 싣는다(`UpkeepPlane.kt` —
     * 등록 상태가 줄곧 «등록됨» 으로만 보여도).
     */
    fun handleNetworkChange() {
        scope.launch {
            val engine = ue ?: return@launch
            noteNetworkChanged()
            val r = engine.handleNetworkChange()
            if (!r.ok) android.util.Log.w("DispatchSession", "망 변경 처리 실패: ${r.code} ${r.reason}")
        }
    }

    private companion object {
        /** User-Agent 제품명 — 서버가 단말 유형 «dispatch» 로 읽는 이름(데스크톱 관제 앱과 같다). */
        const val USER_AGENT_PRODUCT = "CIMS-Dispatch"
        /** 관제 편성(`/provisioning/me`) 재조회 주기 — 데스크톱 `DispatchPollSec` 와 같은 60초. */
        const val DISPATCH_POLL_MS = 60_000L
        /** 내부 오류 토스트의 최소 간격. */
        const val INTERNAL_ERROR_QUIET_MS = 30_000L
        /** CMS 문서(user profile·service config) 재조회 주기 — 데스크톱 `CmsPollSec` 와 같은 5분. */
        const val CMS_POLL_MS = 300_000L
        /** ⑤ 이벤트 링 버퍼 크기 — 화면은 필터로 좁혀 본다. */
        const val ACTIVITY_LIMIT = 500
        const val ACTIVITY_KEEP_MS = 24 * 60 * 60 * 1000L
        /** ⑥ 통화 내역 보관 — 그 이전은 서버 통합 이력이 가진다. */
        const val CALL_LOG_LIMIT = 300
        /** 전화번호부 서비스 키 — 전화 가족은 `volte`(서버가 volte∪voip 를 합산), PTT 는 `ptt`. 캐시 파일의 키이기도 하다. */
        const val DIR_PHONE = "volte"
        const val DIR_PTT = "ptt"
    }
}
