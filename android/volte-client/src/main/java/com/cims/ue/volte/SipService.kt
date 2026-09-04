package com.cims.ue.volte

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.hardware.camera2.CameraManager
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.Ringtone
import android.media.RingtoneManager
import android.net.ConnectivityManager
import android.net.Network
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import androidx.core.app.NotificationCompat
import androidx.core.app.Person
import com.cims.ue.core.config.ConfigStore
import com.cims.ue.core.message.MessageEntry
import com.cims.ue.core.message.MessageStore
import com.cims.ue.core.message.MsgDirection
import com.cims.ue.core.message.SendState
import com.cims.ue.core.sip.CallState
import com.cims.ue.core.sip.PjLib
import com.cims.ue.core.sip.RegState
import com.cims.ue.core.sip.SipController
import com.cims.ue.core.sip.extractSipNumber
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch

/**
 * 등록 유지 Foreground Service (설계서 §8). SipController 를 소유하고, REGISTER 를 유지하며
 * 통화/대기 상태를 알림으로 노출한다. Activity 는 [LocalBinder] 로 바인드해 컨트롤러 flow 를 관찰한다.
 *
 * 수명: START_STICKY — 시스템이 죽여도 재기동되어 재등록. 명시 종료는 [stopSip].
 */
class SipService : Service() {

    // 상태 collector 에서 예상 못한 예외(플랫폼 정책 예외 등)가 나도 프로세스를 죽이지 않는다.
    private val scope = CoroutineScope(SupervisorJob() +
        kotlinx.coroutines.CoroutineExceptionHandler { _, e ->
            android.util.Log.e("SipService", "service scope 예외", e)
        })
    private var controller: SipController? = null
    private var stateJob: Job? = null
    private var netCallback: ConnectivityManager.NetworkCallback? = null
    private var ringtone: Ringtone? = null

    /** 화면 최상단 전역 상태 아이콘 배지(오버레이, 전화 아이콘=중앙 좌측) — main 스레드에서만 갱신. */
    private val overlay by lazy {
        com.cims.ue.core.ui.StatusIconOverlay(this, android.R.drawable.sym_action_call, xOffsetDp = -22)
    }
    private val mainHandler = Handler(Looper.getMainLooper())

    val regState: StateFlow<RegState>? get() = controller?.regState
    val callState: StateFlow<CallState>? get() = controller?.callState

    /** 문자 저장소 변경 신호(수신/발신 시 증가) — UI 는 이걸 관찰해 목록을 다시 읽는다. */
    val messagesVersion = MutableStateFlow(0L)

    inner class LocalBinder : Binder() {
        val service: SipService get() = this@SipService
    }

    private val binder = LocalBinder()
    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        instance = this
        // PJSIP boot 전(영상 캡처 디바이스 열거 전)에 CameraManager 주입 — 발신 영상/셀프뷰 카메라 열거의 전제.
        PjLib.cameraManager = getSystemService(Context.CAMERA_SERVICE) as? CameraManager
        createChannel()
        startForegroundCompat(buildNotification("CIMS VoLTE", "시작 중…"))
        registerNetworkCallback()
        registerMicHandoffReceiver()
    }

    // ── PTT 발언 협조(마이크 핸드오프) ──

    /** PTT 발언 동안 마이크 양보 상태 — 워치독으로 고아 양보(RESUME 유실/PTT 앱 사망) 자동 복귀. */
    private var micYielded = false
    private val micResumeWatchdog = Runnable { applyMicYield(false) }

    private val micHandoffReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                com.cims.ue.core.CimsSuite.ACTION_MIC_YIELD -> applyMicYield(true)
                com.cims.ue.core.CimsSuite.ACTION_MIC_RESUME -> applyMicYield(false)
            }
        }
    }

    /** 스위트 서명 권한으로 보호된 핸드오프 수신 등록 — PTT 앱(발신측)만 통과. */
    private fun registerMicHandoffReceiver() {
        val filter = android.content.IntentFilter().apply {
            addAction(com.cims.ue.core.CimsSuite.ACTION_MIC_YIELD)
            addAction(com.cims.ue.core.CimsSuite.ACTION_MIC_RESUME)
        }
        androidx.core.content.ContextCompat.registerReceiver(
            this, micHandoffReceiver, filter, com.cims.ue.core.CimsSuite.PERMISSION, null,
            androidx.core.content.ContextCompat.RECEIVER_EXPORTED,
        )
    }

    /**
     * PTT 발언 동안 통화 마이크만 해제(재생 유지) — OS 동시 캡처 중재는 일반 앱 두 개의 동시
     * 캡처를 허용하지 않으므로(한쪽 무음 배달), 양보해야 PTT 마이크가 확정적으로 열린다.
     * 통화가 없으면 무시 — 유휴 상태에서 스피커 전용 모드가 저장되면 다음 통화가 마이크 없이
     * 열리는 사고를 막는다(벨울림 중 발언 같은 교차 상황은 드물고 자기해소됨).
     */
    private fun applyMicYield(on: Boolean) {
        mainHandler.removeCallbacks(micResumeWatchdog)
        if (on) {
            val call = controller?.callState?.value
            if (call !is CallState.Active && call !is CallState.Outgoing) return
            mainHandler.postDelayed(micResumeWatchdog, MIC_YIELD_MAX_MS)
        }
        if (micYielded == on) return
        micYielded = on
        controller?.setCaptureEnabled(!on)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // 착신 알림 "거절" 액션 — UI 없이 서비스에서 바로 거절.
        if (intent?.action == ACTION_REJECT) {
            controller?.reject(intent.getIntExtra(EXTRA_CALL_ID, -1))
            return START_STICKY
        }
        // 포그라운드 복귀/네트워크 복귀 등 keepalive 트리거 — 등록만 재시도(계정 있으면 reregister).
        if (intent?.getBooleanExtra("reregister", false) == true) {
            if (controller?.hasAccount() == true) controller?.reregister() else ensureRegistered()
            return START_STICKY
        }
        // 부팅/SSO 자동시작: 공유 계정이 있으면 프로비저닝으로 최신 구성을 받고 등록한다(PTT 와 동일).
        //   ⚠ 예전에는 "설정이 비어 있을 때만" 받았다 — 한 번 채워진 뒤에는 서버가 포트/가용 transport
        //   목록을 바꿔도 단말이 영구히 몰랐다. 수동 설정 모드는 ssoAutoConfigure 안에서 걸러진다.
        val autostart = intent?.getBooleanExtra("autostart", false) == true
        if (autostart && com.cims.ue.core.account.SsoProvisioner.hasAccount(this)) {
            scope.launch(kotlinx.coroutines.Dispatchers.IO) { ssoAutoConfigure() }
        }
        ensureRegistered()
        return START_STICKY
    }

    /** 기본 네트워크 복귀 시 재등록 — doze/슬립/와이파이↔LTE 전환 후 등록 끊김 자동 복구. */
    private fun registerNetworkCallback() {
        val cm = getSystemService(ConnectivityManager::class.java) ?: return
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                if (controller?.hasAccount() == true) controller?.reregister()
            }
        }
        runCatching { cm.registerDefaultNetworkCallback(cb); netCallback = cb }
    }

    /** SSO(공유 계정) → /provisioning/me(kind=volte) → ConfigStore 저장 → 등록. 블로킹(IO).
     *  실패(owner 앱 bind failure 등)해도 던지지 않는다 — 캐시 설정으로 ensureRegistered 가 진행. */
    private fun ssoAutoConfigure() {
        if (ConfigStore(this).isManual()) return   // 수동 설정 모드 — 프로비저닝이 덮어쓰지 않음
        val prof = runCatching { com.cims.ue.core.account.SsoProvisioner.fetchProfile(this) }
            .getOrNull() ?: return
        val svc = prof.service("volte") ?: return
        val cfg = svc.toSipAccountConfig(
            loginId = prof.loginId ?: svc.msisdn,
            displayName = prof.displayName ?: svc.msisdn,
            loginPassword = com.cims.ue.core.account.SsoProvisioner.loginPassword(this),  // sipPassword=null → 공유 로그인 비번 재사용
            countryCode = prof.countryCode.orEmpty(),
        )
        // 사용자가 고른 transport 는 유지하며 저장한다 — 서버 transport 는 기본값(권장)이고
        //   선택권은 단말에 있다(ConfigStore.saveProvisioned).
        ConfigStore(this).saveProvisioned(cfg)
        ensureRegistered()
    }

    private var activeConfig: com.cims.ue.core.config.SipAccountConfig? = null

    /** 설정이 완성되어 있으면 컨트롤러를 만들고 REGISTER. 설정이 바뀌면(재프로비저닝) 재등록. 멱등. */
    fun ensureRegistered() {
        val store = ConfigStore(this)
        // 로그아웃 상태(계정 없음, 수동 모드 아님) — 캐시 설정이 남아 있어도 등록하지 않고 종료.
        // 로그아웃 브로드캐스트 유실(강제종료 등) 후 START_STICKY 재기동이 stale 자격증명으로
        // 재등록하는 사고를 막는 안전망.
        if (!store.isManual() && !com.cims.ue.core.account.SsoProvisioner.hasAccount(this)) {
            updateNotification("CIMS Phone", "로그인 필요")
            stopSip()
            return
        }
        val cfg = store.load()
        if (!cfg.isComplete()) {
            updateNotification("CIMS Phone", "로그인 필요")
            return
        }
        // 가용 transport 목록만 바뀐 경우는 등록에 영향이 없다 — sameRegistration 이 그것만 걸러낸다.
        if (controller != null && cfg.sameRegistration(activeConfig)) return   // 동일 설정 → 그대로
        // 설정 변경(포트/비번/전송 프로토콜 등) — 프로세스 내 PJSIP 재부팅(libDestroy→Endpoint 재생성)은
        // Endpoint/LogWriter 수명 지뢰라 로그아웃과 동일하게 프로세스 재시작이 정석:
        // un-REGISTER 송신 여유(2s) 후 killProcess → START_STICKY 재기동 → 새 설정 첫 부팅.
        controller?.let {
            runCatching { it.unregister() }
            updateNotification("CIMS Phone", "설정 변경 — 재시작")
            android.os.Handler(mainLooper).postDelayed(
                { android.os.Process.killProcess(android.os.Process.myPid()) }, 2000)
            return
        }
        val c = SipController(cfg).also { controller = it; activeConfig = cfg }
        observe(c)
        c.register()
    }

    /** 발신 — 키패드 로컬 표기("013…")는 E.164 로 정규화(가입자 정본은 E.164, 로컬 표기는 서버 404).
     *  cc=프로비저닝 countryCode, 없으면 내 msisdn 에서 유도. 키패드/통화이력/연락처 공통 경로. */
    fun makeCall(dst: String) = controller?.makeCall(normalizeDial(dst))

    private fun normalizeDial(dst: String): String {
        val cfg = ConfigStore(this).load()
        val cc = cfg.countryCode.ifBlank {
            com.cims.ue.core.sip.countryCodeOf(cfg.msisdn).orEmpty()
        }
        return com.cims.ue.core.sip.toE164(dst, cc)
    }

    /** [SipController.sendRequest] token 발급 + 문자 token→msgId 대응(최종 응답을 말풍선 상태로). */
    private val reqSeq = java.util.concurrent.atomic.AtomicLong(1)
    private val msgPending = java.util.concurrent.ConcurrentHashMap<Long, String>()

    /** 문자(SIP MESSAGE, RFC 3428 page-mode) 송신 + 인박스(발신) 기록. 대상=sip:번호@도메인.
     *  대상 번호는 발신과 동일하게 E.164 정규화 — 저장 peer 도 정규화해 수신(From=E.164) 스레드와 합치.
     *  발신은 PENDING 으로 저장하고 MESSAGE 트랜잭션 최종 응답(2xx=SENT, 그 외·타임아웃=FAILED)으로 확정한다 —
     *  등록 flow 밖(UDP 등록 단말의 1300B 초과 요청 TCP 승격)에서 받는 401 은 native 가 재발행하므로 여기엔
     *  최종 결과만 온다(mcdata_messaging.md §4 와 같은 경로). 실패는 말풍선 탭으로 재전송([resendMessage]). */
    fun sendMessage(dst: String, text: String) {
        if (text.isBlank()) return
        val to = normalizeDial(dst)
        val msgId = java.util.UUID.randomUUID().toString().replace("-", "")
        MessageStore(this).add(to, text, MsgDirection.OUT, msgId = msgId, sendState = SendState.PENDING)
        messagesVersion.value++
        sendMessageRequest(to, text, msgId)
    }

    /** 실패 문자 재전송(말풍선 탭) — 같은 항목(msgId)을 PENDING 으로 되돌리고 다시 보낸다. */
    fun resendMessage(e: MessageEntry) {
        if (e.msgId.isBlank() || e.text.isBlank()) return
        if (MessageStore(this).setSendState(e.msgId, SendState.PENDING)) messagesVersion.value++
        sendMessageRequest(e.peer, e.text, e.msgId)
    }

    private fun sendMessageRequest(to: String, text: String, msgId: String) {
        val cfg = ConfigStore(this).load()
        val token = reqSeq.getAndIncrement()
        msgPending[token] = msgId
        controller?.sendRequest(
            method = "MESSAGE",
            targetUri = "sip:${to}@${cfg.domain}",
            contentType = "text/plain",
            body = text,
            token = token,
        ) ?: run {
            msgPending.remove(token)
            if (MessageStore(this).setSendState(msgId, SendState.FAILED)) messagesVersion.value++
        }
    }
    fun answer(callId: Int, video: Boolean = false) = controller?.answer(callId, video)
    fun reject(callId: Int) = controller?.reject(callId)
    fun hangup(callId: Int) = controller?.hangup(callId)

    /** 대화 읽음 처리(대화 화면 진입 시) — 문자 알림도 함께 걷어낸다. 불변이면 신호 생략(재갱신 루프 방지). */
    fun markThreadRead(peer: String) {
        if (MessageStore(this).markRead(peer)) messagesVersion.value++
        notificationManager().cancel(NOTIF_MESSAGE)
    }

    /** M1.3 영상: 발신 전 영상 on/off + 수신 영상 렌더/로컬 프리뷰 Surface 전달. */
    fun setVideoEnabled(on: Boolean) { controller?.videoEnabled = on }
    fun setVideoSurface(surface: Any?) { controller?.setVideoSurface(surface) }
    fun setPreviewSurface(surface: Any?) { controller?.setPreviewSurface(surface) }
    fun switchCamera() { controller?.switchCamera() }

    /** 통화중 마이크 음소거 토글. */
    fun setMuted(callId: Int, on: Boolean) { controller?.setMuted(callId, on) }

    /** 통화 오디오 세션 소유 여부 — 중복 setMode 방지. */
    private var inCallAudio = false

    /**
     * 통화 진입/이탈 시 오디오 모드 전환 — **VoIP 재생·라우팅의 전제** (ptt AudioRouter 와 동일 원리).
     * pjsua 는 STREAM_VOICE_CALL 로 재생하는데, 앱이 `MODE_IN_COMMUNICATION` 을 잡지 않으면
     * 일부 단말(HAL)에서 voice-call 스트림이 어떤 출력으로도 라우팅되지 않아 **수화기/스피커 모두
     * 완전 무음**이 된다(W999 실측 — RTP 수신·디코드·AudioTrack 정상인데 무음). 스피커폰
     * (setSpeaker/setCommunicationDevice)도 MODE_NORMAL 에선 무시된다. 진입 시 voice-call
     * 스트림 음량도 확보한다(단말 저장값이 최소(2/7)로 남아 무음처럼 들리는 사고 예방).
     * 진입 시 출력 라우팅도 기본(수화기/이어폰)으로 **명시 적용**하고 이탈 시 해제([applyRoute]).
     */
    private fun setInCallAudio(on: Boolean) {
        if (on == inCallAudio) return
        inCallAudio = on
        mainHandler.removeCallbacks(routeYieldTicker)
        mainHandler.removeCallbacks(claimInCallAudio)
        mainHandler.removeCallbacks(verifyRoute)
        if (on) {
            sendRouteHandoff(true)                      // PTT 라우팅+모드 양보 (아래 주석 참조)
            // 🔑 모드 claim 은 PTT 의 모드 반납 **이후** — 일부 단말의 라우팅 브로커는 전역 모드가
            // NORMAL→IN_COMMUNICATION 으로 바뀌는 에지에서만 소유자를 기록해(실측: 소유자만 바뀌면
            // miss → 이 앱의 수화기/스피커 요청 전부 무시), PTT 가 모드를 쥔 채 먼저 claim 하면
            // 에지가 없다. YIELD 배달 지연이 가변적(수십 ms~수백 ms)이라 고정 지연 대신
            // **모드가 NORMAL 로 떨어지는 것을 폴링**으로 확인하고 claim 한다(무 PTT 단말=즉시,
            // 미반납/미설치=타임아웃 후 claim — 기능 저하일 뿐 통화는 정상).
            claimTriesLeft = MODE_CLAIM_MAX_TRIES
            mainHandler.post(claimInCallAudio)
        } else {
            val am = getSystemService(AudioManager::class.java) ?: return
            runCatching {
                unregisterRouteCallback()
                releaseRoute()
                am.mode = AudioManager.MODE_NORMAL
            }
            // 종료: 자기 해제(모드 NORMAL 에지)가 브로커에 기록될 **간격을 두고** RESUME —
            // 해제와 PTT 재claim 이 수십 ms 안에 겹치면 일부 단말이 모드 전이를 코레이싱해
            // 에지가 유실된다(실측: ~190ms 간격은 기록, ~50ms 는 유실 → 무전 스피커 미복귀).
            mainHandler.postDelayed({ if (!inCallAudio) sendRouteHandoff(false) }, ROUTE_RESUME_DELAY_MS)
        }
    }

    /** [claimInCallAudio] 폴링 잔여 횟수. */
    private var claimTriesLeft = 0

    /** 통화당 모드 재에지(self-bounce) 잔여 횟수 — [verifyRoute] 참조. */
    private var reEdgeTriesLeft = 0

    /** 통화 오디오 세션 claim — PTT 모드 반납(전역 모드 NORMAL) 확인 폴링 후
     *  모드 소유 + 음량 확보 + 기본 라우팅(수화기) 명시 적용. */
    private val claimInCallAudio = object : Runnable {
        override fun run() {
            if (!inCallAudio) return                     // 대기 중 통화 종료
            val am = getSystemService(AudioManager::class.java) ?: return
            if (am.mode == AudioManager.MODE_IN_COMMUNICATION && --claimTriesLeft > 0) {
                mainHandler.postDelayed(this, MODE_CLAIM_POLL_MS)
                return
            }
            android.util.Log.i("SipService",
                "claimInCallAudio (mode=${am.mode} triesLeft=$claimTriesLeft)")
            reEdgeTriesLeft = MODE_REEDGE_MAX_TRIES
            runCatching {
                am.mode = AudioManager.MODE_IN_COMMUNICATION
                val max = am.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL)
                am.setStreamVolume(AudioManager.STREAM_VOICE_CALL, max, 0)
                speakerOn = false                       // 통화 시작 라우팅 기본=수화기(UI 토글 초기값과 일치)
                applyRoute()
                registerRouteCallback()
                mainHandler.postDelayed(routeYieldTicker, ROUTE_YIELD_TICK_MS)
            }
        }
    }

    /**
     * 라우팅 적용 검증 + 자가 재에지 — 적용 후에도 실제 통신 장치가 의도(스피커폰/비스피커)와
     * 다르면, 라우팅 브로커가 이 앱을 모드 소유자로 기록하지 못한 것(claim 에지가 타이밍 경쟁으로
     * 유실 — 수신측 실측)이므로 **모드를 짧게 반납→재claim** 해 NORMAL 에지를 다시 만들어 소유자를
     * 재기록시키고 라우팅을 재적용한다. 통화당 [MODE_REEDGE_MAX_TRIES]회 한도(수렴 보장).
     */
    private val verifyRoute = object : Runnable {
        override fun run() {
            if (!inCallAudio) return
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return
            val am = getSystemService(AudioManager::class.java) ?: return
            val actual = runCatching { am.communicationDevice?.type }.getOrNull() ?: return
            val ok = if (speakerOn) actual == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
            else actual != AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
            if (ok || reEdgeTriesLeft-- <= 0) return
            android.util.Log.i("SipService",
                "route mismatch(actual=$actual speakerOn=$speakerOn) — mode re-edge")
            runCatching { am.mode = AudioManager.MODE_NORMAL }
            mainHandler.postDelayed({
                if (!inCallAudio) return@postDelayed
                runCatching {
                    am.mode = AudioManager.MODE_IN_COMMUNICATION
                    applyRoute()
                }
            }, MODE_REEDGE_GAP_MS)
        }
    }

    /**
     * PTT 앱 라우팅 양보 통지(스위트 협조, [com.cims.ue.core.CimsSuite]) — PTT 는 무전 스피커폰
     * communication device 요청을 상시 유지하는데, 일부 단말(W999/MTK)은 어느 앱이든 스피커
     * 요청이 서 있으면 통화 라우팅이 스피커로 고정돼 이 앱의 수화기 요청이 무시된다(실측).
     * 통화 시작=YIELD/종료=RESUME, 통화 중 주기 재송신([routeYieldTicker])으로 수신측 워치독을
     * 갱신해 이 앱이 죽어도 PTT 라우팅이 워치독 시한 내 자동 복귀하게 한다.
     */
    private fun sendRouteHandoff(yield: Boolean) {
        val action = if (yield) com.cims.ue.core.CimsSuite.ACTION_ROUTE_YIELD
        else com.cims.ue.core.CimsSuite.ACTION_ROUTE_RESUME
        runCatching {
            sendBroadcast(
                Intent(action).setPackage(com.cims.ue.core.CimsSuite.PTT_PACKAGE),
                com.cims.ue.core.CimsSuite.PERMISSION,
            )
        }
    }

    private val routeYieldTicker = object : Runnable {
        override fun run() {
            if (!inCallAudio) return
            sendRouteHandoff(true)
            mainHandler.postDelayed(this, ROUTE_YIELD_TICK_MS)
        }
    }

    /** 사용자 스피커폰 선택 — 통화 중 라우팅 재적용(이어폰 연결/해제)에도 쓰인다. */
    private var speakerOn = false

    /** 스피커폰 전환(통화중 UI 토글). 통화 종료 시 자동 원복([setInCallAudio]). */
    fun setSpeaker(on: Boolean) {
        speakerOn = on
        applyRoute()
    }

    /** 이어폰 계열(유선/USB/BT SCO/BLE) — 수화기 모드에서 이어폰 연결 시 그쪽 우선(기본 정책과 동일). */
    private fun isHeadset(d: AudioDeviceInfo): Boolean = when (d.type) {
        AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_USB_HEADSET, AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> true
        else -> Build.VERSION.SDK_INT >= 31 && d.type == AudioDeviceInfo.TYPE_BLE_HEADSET
    }

    /**
     * 통화 중 출력 라우팅 적용 — **자기 요청을 항상 유지**한다(수화기=이어폰>BUILTIN_EARPIECE,
     * 스피커폰=BUILTIN_SPEAKER 명시 요청).
     *
     * 🔑 `clearCommunicationDevice`(요청 제거)로 수화기를 표현하면 같은 통신 모드를 쥔 다른 앱의
     * 상시 요청(PTT 앱 무전 스피커폰)으로 폴백돼 수화기 선택이 무시된다(W999 실측: 수화기 선택에도
     * preferred=speaker 유지). 반대로 `setCommunicationDevice` 요청 자체를 라우팅에 반영하지 않는
     * 단말 편차도 있어(MF52/AOSP15 실측: 요청 등록·활성인데 preferred=null·force use 0) 레거시
     * `isSpeakerphoneOn` 을 병행 적용한다(이중 적용 무해 — ptt AudioRouter 와 동일 원리).
     * 순서 주의: 레거시 off 는 (AOSP 구현상) 자기 communication device 요청의 clear 라서,
     * 수화기 전환은 **레거시 off 먼저 → 명시 요청 등록** 순서여야 방금 넣은 요청이 지워지지 않는다.
     */
    private fun applyRoute() {
        if (!inCallAudio) return
        // 적용 후 검증(자가 재에지) 예약 — 토글/기본 적용 공통
        mainHandler.removeCallbacks(verifyRoute)
        mainHandler.postDelayed(verifyRoute, ROUTE_VERIFY_DELAY_MS)
        val am = getSystemService(AudioManager::class.java) ?: return
        runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (speakerOn) {
                    val spk = am.availableCommunicationDevices
                        .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
                    val ok = spk?.let { am.setCommunicationDevice(it) } ?: false
                    @Suppress("DEPRECATION")
                    am.isSpeakerphoneOn = true
                    android.util.Log.i("SipService", "applyRoute speaker ok=$ok")
                } else {
                    @Suppress("DEPRECATION")
                    am.isSpeakerphoneOn = false
                    val dev = am.availableCommunicationDevices.firstOrNull(::isHeadset)
                        ?: am.availableCommunicationDevices
                            .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE }
                    val ok = dev?.let { am.setCommunicationDevice(it) } ?: false
                    android.util.Log.i("SipService", "applyRoute earpiece/headset dev=${dev?.type} ok=$ok")
                }
            } else @Suppress("DEPRECATION") {
                am.isSpeakerphoneOn = speakerOn
            }
        }
    }

    /** 통화 종료 — 자기 라우팅 요청 해제(다른 앱 라우팅 복원: PTT 무전 스피커 등). */
    private fun releaseRoute() {
        val am = getSystemService(AudioManager::class.java) ?: return
        runCatching {
            @Suppress("DEPRECATION")
            am.isSpeakerphoneOn = false
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) am.clearCommunicationDevice()
        }
    }

    /** 통화 중 이어폰 연결/해제 시 현재 선택(스피커폰/수화기) 기준으로 라우팅 재적용 —
     *  명시 요청 유지 방식이라 기본 정책의 자동 전환이 동작하지 않으므로 직접 따라간다. */
    private val routeDeviceCallback = object : android.media.AudioDeviceCallback() {
        override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) { applyRoute() }
        override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) { applyRoute() }
    }
    private var routeCallbackRegistered = false

    private fun registerRouteCallback() {
        if (routeCallbackRegistered) return
        routeCallbackRegistered = true
        runCatching {
            getSystemService(AudioManager::class.java)
                ?.registerAudioDeviceCallback(routeDeviceCallback, mainHandler)
        }
    }

    private fun unregisterRouteCallback() {
        if (!routeCallbackRegistered) return
        routeCallbackRegistered = false
        runCatching {
            getSystemService(AudioManager::class.java)
                ?.unregisterAudioDeviceCallback(routeDeviceCallback)
        }
    }

    /** 등록 해제 + Endpoint 정리 + 서비스 종료. */
    fun stopSip() {
        controller?.unregister()
        controller?.shutdown()
        controller = null
        mainHandler.post { overlay.hide() }
        stopForegroundCompat()
        stopSelf()
    }

    private fun observe(c: SipController) {
        stateJob?.cancel()
        // 이전 프로세스의 미결 PENDING — 결과 이벤트 유실 상태이므로 실패로 마감(재전송 가능)
        if (MessageStore(this).failStalePending()) messagesVersion.value++
        stateJob = scope.launch {
            // 등록 상태 = 상시 알림(상태바 아이콘) + 화면 최상단 전역 배지(오버레이) — 한눈에 통화 가능 여부.
            c.regState.onEach { reg ->
                val (line, icon) = when (reg) {
                    is RegState.Registered -> "통화 가능" to android.R.drawable.sym_action_call
                    RegState.Registering -> "연결 중…" to android.R.drawable.presence_away
                    RegState.Idle -> "대기" to android.R.drawable.presence_away
                    RegState.Unregistered -> "등록 해제됨" to android.R.drawable.presence_invisible
                    is RegState.Failed -> "오프라인 (${reg.reason})" to android.R.drawable.stat_notify_error
                }
                updateNotification("CIMS Phone", line, icon)
                val color = when (reg) {
                    is RegState.Registered -> 0xFF00C853.toInt()
                    RegState.Registering, RegState.Idle -> 0xFFF9A825.toInt()
                    RegState.Unregistered -> 0xFF9E9E9E.toInt()
                    is RegState.Failed -> 0xFFEA4335.toInt()
                }
                // 아이콘만 표시 — 상태는 tint 색으로(텍스트 없음), 라벨은 접근성용.
                mainHandler.post { overlay.update(color, if (reg is RegState.Failed) "오프라인" else line) }
            }.launchIn(this)

            c.callState.onEach { call ->
                // 🔑 Incoming(응답 전)은 승격하지 않는다 — 백그라운드 착신 시 microphone 타입 승격은
                // API 34+ 에서 금지(ForegroundServiceStartNotAllowedException → 앱 크래시).
                // 사용자가 받으면(Active) 통화 UI 가 포그라운드라 승격 가능. Outgoing 은 사용자 발신=포그라운드.
                elevateForCall(call is CallState.Active || call is CallState.Outgoing)
                // 통화 오디오 세션 소유(MODE_IN_COMMUNICATION) — 미소유 시 일부 단말 완전 무음(setInCallAudio 참조)
                setInCallAudio(call is CallState.Active || call is CallState.Outgoing)
                // 착신 — 기본 전화앱처럼 벨소리 + 풀스크린/헤드업 착신 알림(받기/거절).
                if (call is CallState.Incoming) {
                    showIncomingCallNotification(call)
                    startRinging()
                } else {
                    stopRinging()
                    notificationManager().cancel(NOTIF_INCOMING)
                }
                if (call is CallState.Disconnected) {
                    applyMicYield(false)                                // 발언 양보 잔존 해제(다음 통화 대비)
                }
                val line = when (call) {
                    is CallState.Incoming -> "수신: ${call.remote}"
                    is CallState.Outgoing -> "발신: ${call.remote}"
                    is CallState.Active -> "통화 중: ${call.remote}"
                    is CallState.Disconnected -> null
                    CallState.Null -> null
                }
                if (line != null) updateNotification("CIMS Phone", line)
            }.launchIn(this)

            // 문자 MESSAGE 최종 응답(token 상관) → 말풍선 상태 SENT/FAILED. token 당 1회(remove 로 dedupe).
            c.sendReqResults.onEach { r ->
                val msgId = msgPending.remove(r.token) ?: return@onEach
                val ok = r.code in 200..299
                if (!ok) android.util.Log.w("SipService", "MESSAGE $msgId 실패: ${r.code} ${r.reason}")
                if (MessageStore(this@SipService).setSendState(msgId, if (ok) SendState.SENT else SendState.FAILED))
                    messagesVersion.value++
            }.launchIn(this)

            // 수신 문자 — 인박스 저장 + 알림(백그라운드에서도 도착 확인 가능).
            c.incomingMessage.onEach { im ->
                val peer = extractSipNumber(im.fromUri)
                MessageStore(this@SipService).add(peer, im.body, MsgDirection.IN)
                messagesVersion.value++
                showMessageNotification(peer, im.body)
            }.launchIn(this)
        }
    }

    override fun onDestroy() {
        instance = null
        stopRinging()
        runCatching { unregisterReceiver(micHandoffReceiver) }
        mainHandler.removeCallbacks(micResumeWatchdog)
        mainHandler.post { overlay.hide() }
        stateJob?.cancel()
        netCallback?.let { cb -> runCatching { getSystemService(ConnectivityManager::class.java)?.unregisterNetworkCallback(cb) } }
        netCallback = null
        controller?.shutdown()
        controller = null
        super.onDestroy()
    }

    // ── 알림 ──

    private fun notificationManager(): NotificationManager =
        getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = notificationManager()
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "VoLTE 등록/통화", NotificationManager.IMPORTANCE_LOW),
            )
            // 착신 — 헤드업/풀스크린용 HIGH. 벨소리는 채널음 대신 서비스가 루프 재생(전화앱처럼 계속 울림).
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_INCOMING, "수신 전화", NotificationManager.IMPORTANCE_HIGH).apply {
                    setSound(null, null)
                    enableVibration(true)
                },
            )
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_MESSAGE, "문자 수신", NotificationManager.IMPORTANCE_HIGH),
            )
        }
    }

    // ── 착신 알림(기본 전화앱 스타일) ──

    /**
     * CallStyle 착신 알림 — 화면 꺼짐/잠금이면 fullScreenIntent 로 통화화면을 직접 띄우고,
     * 사용 중이면 헤드업으로 받기/거절을 노출한다. "받기"는 MainActivity 경유(통화 UI 필요),
     * "거절"은 서비스 액션으로 즉시 처리.
     */
    private fun showIncomingCallNotification(call: CallState.Incoming) {
        val number = extractSipNumber(call.remote)
        val piFlags = PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        val fullScreen = PendingIntent.getActivity(
            this, 1,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            piFlags,
        )
        val answer = PendingIntent.getActivity(
            this, 2,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                .putExtra(EXTRA_ANSWER_CALL_ID, call.id)
                .putExtra(EXTRA_ANSWER_VIDEO, call.video),
            piFlags,
        )
        val reject = PendingIntent.getService(
            this, 3,
            Intent(this, SipService::class.java).setAction(ACTION_REJECT)
                .putExtra(EXTRA_CALL_ID, call.id),
            piFlags,
        )
        val caller = Person.Builder().setName(number.ifBlank { "알 수 없음" }).setImportant(true).build()
        val n = NotificationCompat.Builder(this, CHANNEL_INCOMING)
            .setSmallIcon(android.R.drawable.sym_call_incoming)
            .setContentTitle(if (call.video) "영상 수신 전화" else "수신 전화")
            .setContentText(number)
            .setCategory(NotificationCompat.CATEGORY_CALL)
            .setPriority(NotificationCompat.PRIORITY_MAX)
            .setOngoing(true)
            .setFullScreenIntent(fullScreen, true)
            .setStyle(NotificationCompat.CallStyle.forIncomingCall(caller, reject, answer).setIsVideo(call.video))
            .build()
        notificationManager().notify(NOTIF_INCOMING, n)
    }

    private fun startRinging() {
        if (ringtone != null) return
        val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE) ?: return
        ringtone = RingtoneManager.getRingtone(this, uri)?.apply {
            audioAttributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build()
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) isLooping = true
            runCatching { play() }
        }
    }

    private fun stopRinging() {
        ringtone?.let { runCatching { it.stop() } }
        ringtone = null
    }

    /** 문자 수신 알림 — 탭하면 문자 탭으로 진입. */
    private fun showMessageNotification(peer: String, body: String) {
        val open = PendingIntent.getActivity(
            this, 4,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                .putExtra(EXTRA_OPEN_MESSAGES, true),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val n = NotificationCompat.Builder(this, CHANNEL_MESSAGE)
            .setSmallIcon(android.R.drawable.sym_action_email)
            .setContentTitle(peer)
            .setContentText(body)
            .setStyle(NotificationCompat.BigTextStyle().bigText(body))
            .setCategory(NotificationCompat.CATEGORY_MESSAGE)
            .setAutoCancel(true)
            .setContentIntent(open)
            .build()
        notificationManager().notify(NOTIF_MESSAGE, n)
    }

    private fun buildNotification(
        title: String,
        text: String,
        icon: Int = android.R.drawable.sym_action_call,
    ): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(title)
            .setContentText(text)
            .setSmallIcon(icon)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()

    private fun updateNotification(title: String, text: String, icon: Int = android.R.drawable.sym_action_call) {
        notificationManager().notify(NOTIF_ID, buildNotification(title, text, icon))
    }

    /**
     * 등록유지 FGS. Android 14+(API34)에서 microphone 타입은 **백그라운드(부팅)에서 시작 불가**이므로
     * 등록 단계는 specialUse 로 시작하고(부팅 자동시작 가능), 통화 활성 시 [elevateForCall] 로
     * microphone 으로 승격한다. 13 이하는 종전대로 microphone.
     */
    private fun fgsType(inCall: Boolean): Int = when {
        Build.VERSION.SDK_INT >= 34 ->
            if (inCall) ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            else ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q ->
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        else -> 0
    }

    private fun startForegroundCompat(n: Notification, inCall: Boolean = false) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, n, fgsType(inCall))
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    /** 통화 활성/종료 시 FGS 타입 승격/복귀 (마이크 접근 권한 보장).
     *  승격이 플랫폼 정책으로 거부돼도(백그라운드 등) 던지지 않고 specialUse 로 유지 — 통화 UI 가
     *  포그라운드로 오면 while-in-use 마이크 권한으로 통화는 정상 동작한다. */
    private fun elevateForCall(active: Boolean) {
        if (Build.VERSION.SDK_INT >= 34) {
            val n = buildNotification("CIMS Phone", if (active) "통화 중" else "등록 유지")
            runCatching { startForegroundCompat(n, inCall = active) }
                .onFailure {
                    android.util.Log.w("SipService", "FGS 승격 거부(inCall=$active) — specialUse 유지", it)
                    runCatching { startForegroundCompat(n, inCall = false) }
                }
        }
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION") stopForeground(true)
        }
    }

    companion object {
        /** 실행 중 서비스 — 로그아웃 리시버가 등록 해제·FGS 종료용으로 접근(Activity 는 bind 사용). */
        @Volatile var instance: SipService? = null
            private set
        private const val CHANNEL_ID = "cims_volte"
        private const val CHANNEL_INCOMING = "cims_incoming_call"
        private const val CHANNEL_MESSAGE = "cims_message"
        private const val NOTIF_ID = 1001
        private const val NOTIF_INCOMING = 1002
        private const val NOTIF_MESSAGE = 1003

        private const val ACTION_REJECT = "com.cims.ue.volte.action.REJECT"
        private const val EXTRA_CALL_ID = "callId"

        /** 마이크 양보 워치독 시한 — 서버 최대 발언시간을 여유 있게 초과(RESUME 유실 안전망). */
        private const val MIC_YIELD_MAX_MS = 40_000L
        /** 라우팅 양보 재송신 주기 — 수신측(PTT) 워치독(2주기+여유)보다 짧게. */
        private const val ROUTE_YIELD_TICK_MS = 300_000L
        /** 모드 claim 폴링 — PTT 모드 반납(YIELD 배달 지연 가변) 대기: 100ms × 20 = 최대 2s. */
        private const val MODE_CLAIM_POLL_MS = 100L
        private const val MODE_CLAIM_MAX_TRIES = 20
        /** 라우팅 적용 검증 지연/재에지 간격/통화당 재에지 한도. */
        private const val ROUTE_VERIFY_DELAY_MS = 800L
        private const val MODE_REEDGE_GAP_MS = 150L
        private const val MODE_REEDGE_MAX_TRIES = 2
        /** 통화 종료 후 RESUME 지연 — 자기 모드 해제 에지가 기록된 뒤 PTT 재claim 이 오게. */
        private const val ROUTE_RESUME_DELAY_MS = 300L

        /** 착신 알림 "받기" — MainActivity 가 이 extra 를 읽어 서비스 연결 후 응답한다. */
        const val EXTRA_ANSWER_CALL_ID = "answer_call_id"
        const val EXTRA_ANSWER_VIDEO = "answer_video"

        /** 문자 알림 탭 — MainActivity 가 문자 탭으로 진입. */
        const val EXTRA_OPEN_MESSAGES = "open_messages"

        fun start(ctx: Context) {
            val i = Intent(ctx, SipService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i)
            else ctx.startService(i)
        }

        /** 포그라운드 복귀 시 등록 재시도 트리거(keepalive). 서비스가 죽어 있었으면 기동+등록. */
        fun poke(ctx: Context) {
            val i = Intent(ctx, SipService::class.java).putExtra("reregister", true)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i)
            else ctx.startService(i)
        }
    }
}
