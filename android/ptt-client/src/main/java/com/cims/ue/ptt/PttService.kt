package com.cims.ue.ptt

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.cims.ue.core.config.ConfigStore
import com.cims.ue.ptt.csc.CscConfig
import com.cims.ue.ptt.mcdata.McDataCodec
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch

/**
 * MCPTT 등록 유지 Foreground Service. [PttController] 를 소유하고 REGISTER 를 유지하며 상태를 알림에 노출.
 * Activity 는 [LocalBinder] 로 바인드해 컨트롤러 flow 를 관찰·제어한다.
 */
class PttService : Service() {

    // 상태 collector 에서 예상 못한 예외(플랫폼 정책 예외 등)가 나도 프로세스를 죽이지 않는다.
    private val scope = CoroutineScope(SupervisorJob() +
        kotlinx.coroutines.CoroutineExceptionHandler { _, e ->
            android.util.Log.e("PttService", "service scope 예외", e)
        })
    // StateFlow — 바인드 시점에 아직 컨트롤러가 없어도(SSO 재취득 중) 생성 시 UI 재구성
    private val _controller = kotlinx.coroutines.flow.MutableStateFlow<PttController?>(null)
    val controllerFlow: kotlinx.coroutines.flow.StateFlow<PttController?> = _controller
    val controller: PttController? get() = _controller.value
    private var job: Job? = null
    private var routeJob: Job? = null

    /** 이어폰 장치 열거(연결/해제 감시) — 컨트롤러 재생성과 무관하게 서비스 수명. */
    val audioRouter by lazy { com.cims.ue.ptt.audio.AudioRouter(this) }

    /** 화면 최상단 전역 상태 아이콘 배지(오버레이, PTT 아이콘=중앙 우측 — CIMS-Phone 전화 아이콘과
     *  자리를 나눔). 아이콘만 표시하고 상태는 tint 색으로. main 스레드에서만 갱신. */
    private val overlay by lazy {
        com.cims.ue.core.ui.StatusIconOverlay(this, R.drawable.ic_ptt, xOffsetDp = 22)
    }
    private val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())

    inner class LocalBinder : Binder() {
        val service: PttService get() = this@PttService
    }

    private val binder = LocalBinder()
    override fun onBind(intent: Intent?): IBinder = binder

    /** 통화이력/그룹문자 저장소 — UI(Activity)가 바인드해 함께 사용. */
    val history by lazy { com.cims.ue.ptt.history.HistoryStore(this) }
    val messages by lazy { com.cims.ue.core.message.MessageStore(this) }
    private val _messageTick = kotlinx.coroutines.flow.MutableStateFlow(0)
    /** 문자 저장 변경 틱 — UI 재조회 트리거(MessageStore 자체엔 변경 알림이 없음). */
    val messageTick: kotlinx.coroutines.flow.StateFlow<Int> = _messageTick

    /** 그룹 문자(MCData SDS) 발신 + 로컬 스레드 저장 — msgId 보존(delivered 통지 대사용).
     *  MSRP(미디어평면) 경로는 수 초 걸리고 실패 가능 → PENDING 으로 시작(결과는 sendResult 반영). */
    fun sendGroupMessage(groupId: String, text: String) {
        val viaMsrp = controller?.willUseMsrp(text) == true
        val msgId = controller?.sendGroupMessage(groupId, text) ?: return
        if (msgId.isEmpty()) return
        messages.add(groupId, text, com.cims.ue.core.message.MsgDirection.OUT, msgId = msgId,
            sendState = if (viaMsrp) com.cims.ue.core.message.SendState.PENDING
            else com.cims.ue.core.message.SendState.SENT)
        _messageTick.value++
    }

    /** 실패 문자 재전송(말풍선 탭) — PENDING 복귀 후 같은 msgId 로 재시도. */
    fun resendMessage(e: com.cims.ue.core.message.MessageEntry) {
        if (e.msgId.isBlank() || e.text.isBlank()) return
        if (messages.setSendState(e.msgId, com.cims.ue.core.message.SendState.PENDING)) _messageTick.value++
        controller?.resendGroupMessage(e.peer, e.text, e.msgId)
    }

    /** MSRP 발신 진행률(msgId → 0f~1f) — 말풍선 진행 바용(런타임 전용, 미영속). */
    private val _sendProgress = kotlinx.coroutines.flow.MutableStateFlow<Map<String, Float>>(emptyMap())
    val sendProgress: kotlinx.coroutines.flow.StateFlow<Map<String, Float>> = _sendProgress

    fun markThreadRead(peer: String) {
        if (messages.markRead(peer)) _messageTick.value++
    }

    /** 문자 삭제(1건/선택) — 첨부 로컬 파일·전송 진행률도 함께 정리. */
    fun deleteMessages(entries: Collection<com.cims.ue.core.message.MessageEntry>) {
        if (entries.isEmpty()) return
        entries.forEach { e ->
            if (e.attPath.isNotBlank()) runCatching { java.io.File(e.attPath).delete() }
        }
        _sendProgress.value = _sendProgress.value -
            entries.mapNotNull { it.msgId.takeIf(String::isNotBlank) }.toSet()
        if (messages.delete(entries.map { it.key }.toSet())) _messageTick.value++
    }

    /** 대화(스레드) 통째 삭제. */
    fun deleteThread(peer: String) = deleteMessages(messages.thread(peer))

    /** 전체 문자 삭제. */
    fun deleteAllMessages() =
        deleteMessages(messages.threads().flatMap { messages.thread(it.peer) })

    // ── MCData FD (파일전송) ──

    /** 첨부 전송 — content Uri 읽기 → CSC 업로드 → FD SIGNALLING (mcdata_messaging.md). */
    fun sendGroupAttachment(groupId: String, uri: android.net.Uri) {
        val c = controller ?: return
        scope.launch(kotlinx.coroutines.Dispatchers.IO) {
            val picked = readContent(uri) ?: run {
                mainHandler.post { toast("첨부 파일을 읽을 수 없습니다") }; return@launch
            }
            val (data, name, mime) = picked
            val sent = c.sendGroupAttachment(groupId, data, name, mime)
            if (sent == null) { mainHandler.post { toast("첨부 전송 실패 (그룹 파일전송 허용 여부 확인)") }; return@launch }
            // 발신본은 로컬 파일로 바로 보관 (재다운로드 불필요)
            val path = writeAttachment(sent.msgId, name, data)
            messages.add(groupId, "", com.cims.ue.core.message.MsgDirection.OUT, msgId = sent.msgId,
                attName = name, attUrl = sent.url, attSize = sent.size, attPath = path ?: "")
            _messageTick.value++
        }
    }

    /** 수신 첨부 다운로드(수동/자동 공용) — 완료 시 스토어에 로컬 경로 기록. */
    fun downloadAttachment(entry: com.cims.ue.core.message.MessageEntry) {
        if (entry.attPath.isNotBlank()) return
        downloadAttachment(entry.msgId, entry.attUrl, entry.attName)
    }

    private fun downloadAttachment(msgId: String, url: String, name: String) {
        val c = controller ?: return
        if (url.isBlank()) return
        scope.launch(kotlinx.coroutines.Dispatchers.IO) {
            val data = c.downloadAttachment(url) ?: run {
                mainHandler.post { toast("파일 다운로드 실패") }; return@launch
            }
            val path = writeAttachment(msgId, name.ifBlank { "file.bin" }, data)
            if (path != null && messages.setAttachmentPath(msgId, path)) _messageTick.value++
        }
    }

    /** 다운로드된 첨부 열기 — FileProvider 경유 ACTION_VIEW. */
    fun openAttachment(entry: com.cims.ue.core.message.MessageEntry) {
        if (entry.attPath.isBlank()) return
        val file = java.io.File(entry.attPath)
        if (!file.exists()) { toast("파일이 없습니다 (다시 다운로드)"); return }
        runCatching {
            val uri = androidx.core.content.FileProvider.getUriForFile(
                this, "$packageName.fileprovider", file)
            val mime = contentResolver.getType(uri)
                ?: android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(file.extension.lowercase())
                ?: "application/octet-stream"
            startActivity(Intent(Intent.ACTION_VIEW).apply {
                setDataAndType(uri, mime)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
            })
        }.onFailure { toast("열 수 있는 앱이 없습니다") }
    }

    private fun toast(msg: String) =
        android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_SHORT).show()

    /** content Uri → (bytes, 표시이름, mime). 50MB 상한(서버 게이트와 동일). */
    private fun readContent(uri: android.net.Uri): Triple<ByteArray, String, String>? = runCatching {
        var name = "file.bin"; var size = -1L
        contentResolver.query(uri, null, null, null, null)?.use { cur ->
            if (cur.moveToFirst()) {
                val ni = cur.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                val si = cur.getColumnIndex(android.provider.OpenableColumns.SIZE)
                if (ni >= 0) cur.getString(ni)?.let { name = it }
                if (si >= 0) size = cur.getLong(si)
            }
        }
        if (size > MAX_ATTACH) return null
        val data = contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: return null
        if (data.size > MAX_ATTACH) return null
        Triple(data, name, contentResolver.getType(uri) ?: "application/octet-stream")
    }.getOrNull()

    /** 첨부 로컬 저장 — files/mcdata/{msgId}_{name} (FileProvider file_paths 와 일치). */
    private fun writeAttachment(msgId: String, name: String, data: ByteArray): String? = runCatching {
        val dir = java.io.File(filesDir, "mcdata").apply { mkdirs() }
        val safe = name.replace(Regex("[/\\\\:*?\"<>|]"), "_")
        java.io.File(dir, "${msgId.take(12)}_$safe").apply { writeBytes(data) }.absolutePath
    }.getOrNull()

    /** 벤더 PTT 키 브로드캐스트 동적 수신 — 실행 중 확실한 배달(정적 등록은 프로세스 사망 대비). */
    private val vendorKeyReceiver = VendorPttReceiver()

    // ── 스위트 라우팅 양보(volte 통화 협조) — CimsSuite.ACTION_ROUTE_YIELD/RESUME ──

    /** RESUME 유실(volte 사망) 대비 자동 복귀 — volte 가 통화 중 주기(5분) 재송신으로 갱신한다. */
    private val routeResumeWatchdog = Runnable { applyRouteYield(false) }

    private val routeHandoffReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                com.cims.ue.core.CimsSuite.ACTION_ROUTE_YIELD -> applyRouteYield(true)
                com.cims.ue.core.CimsSuite.ACTION_ROUTE_RESUME -> applyRouteYield(false)
            }
        }
    }

    /**
     * volte 통화 동안 무전 라우팅 요청(스피커폰/이어폰 communication device) 양보 — 일부 단말
     * (W999/MTK)은 어느 앱이든 스피커 요청이 서 있으면 통화 라우팅이 스피커로 고정돼 volte 의
     * 수화기 선택이 무시된다(실측). 재생 자체는 계속되며 volte 가 정한 경로(수화기 등)로 나온다.
     */
    private fun applyRouteYield(on: Boolean) {
        mainHandler.removeCallbacks(routeResumeWatchdog)
        mainHandler.removeCallbacks(verifyResumeRoute)
        if (on) mainHandler.postDelayed(routeResumeWatchdog, ROUTE_YIELD_MAX_MS)
        val changed = audioRouter.yieldRoute(on)
        if (changed) android.util.Log.i("PttService", "route yield=$on")
        if (changed && !on) {
            // 복귀 — 영속 선택(스피커폰/이어폰) 재적용. 컨트롤러 부재면 다음 생성 시 적용됨.
            controller?.let { it.setAudioRoute(it.audioRoute.value, it.headsetId.value) }
            resumeVerifyTries = RESUME_VERIFY_MAX_TRIES
            mainHandler.postDelayed(verifyResumeRoute, RESUME_VERIFY_DELAY_MS)
        }
        if (changed && on) {
            // 통화 동안 무전 트랙 핀 주기 재적용 — 통화 수립/전환기의 오디오 정책 재라우팅이
            // 트랙의 preferred device 를 반복 이탈시키므로(실측: 안정화 후의 재핀은 유지됨),
            // 양보가 끝날 때까지 주기적으로 재적용해 수렴을 보장한다. 같은 값 재설정은 no-op
            // 이라 해제→재설정 바운스로 재평가를 강제. 전역 요청은 양보 중이라 sip(트랙 단위)만.
            mainHandler.removeCallbacks(repinTicker)
            mainHandler.postDelayed(repinTicker, ROUTE_REPIN_PERIOD_MS)
        }
    }

    private val repinTicker = object : Runnable {
        override fun run() {
            if (!audioRouter.isYielded) return
            // 이미 올바르게 라우팅 중이면 네이티브(set_track_preferred_device)가 no-op 처리 —
            // 이탈 시에만 해제→재설정 바운스가 일어나므로 주기 호출이 재생을 흔들지 않는다.
            controller?.let { it.setAudioRoute(it.audioRoute.value, it.headsetId.value) }
            mainHandler.postDelayed(this, ROUTE_REPIN_PERIOD_MS)
        }
    }

    /** [verifyResumeRoute] 재에지 잔여 횟수. */
    private var resumeVerifyTries = 0

    /**
     * 라우팅 복귀 검증 + 자가 재에지 — volte 통화 종료 복귀 후에도 무전 스피커폰이 실제로
     * 적용되지 않았으면(라우팅 브로커의 모드 소유 기록이 stale — 해제/재claim 이 겹치면 전이가
     * 코레이싱돼 에지 유실, 실측), 모드를 짧게 반납→재claim 해 소유자를 재기록시키고 재적용한다.
     */
    private val verifyResumeRoute = object : Runnable {
        override fun run() {
            if (Build.VERSION.SDK_INT < 31) return
            val c = controller ?: return
            // 스피커폰 의도일 때만 판정(수화기/이어폰은 기본 라우팅과 구분 모호)
            if (c.audioRoute.value != com.cims.ue.core.sip.SipController.AUDIO_ROUTE_SPEAKER) return
            val am = getSystemService(android.media.AudioManager::class.java) ?: return
            // 무전 세션 모드 보유 중일 때만 의미(미보유면 통화 경로 아님)
            if (am.mode != android.media.AudioManager.MODE_IN_COMMUNICATION) return
            val actual = runCatching { am.communicationDevice?.type }.getOrNull() ?: return
            if (actual == android.media.AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) return
            if (resumeVerifyTries-- <= 0) return
            android.util.Log.i("PttService", "resume route mismatch(actual=$actual) — mode re-edge")
            runCatching { am.mode = android.media.AudioManager.MODE_NORMAL }
            mainHandler.postDelayed({
                runCatching { am.mode = android.media.AudioManager.MODE_IN_COMMUNICATION }
                controller?.let { it.setAudioRoute(it.audioRoute.value, it.headsetId.value) }
                mainHandler.postDelayed(this, RESUME_VERIFY_DELAY_MS)
            }, REEDGE_GAP_MS)
        }
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        createChannel()
        startForegroundCompat(notification("CIMS PTT", "시작 중…"))
        runCatching {
            if (Build.VERSION.SDK_INT >= 33)
                registerReceiver(vendorKeyReceiver, VendorPttReceiver.filter(), RECEIVER_EXPORTED)
            else registerReceiver(vendorKeyReceiver, VendorPttReceiver.filter())
        }
        runCatching {
            androidx.core.content.ContextCompat.registerReceiver(
                this, routeHandoffReceiver,
                android.content.IntentFilter().apply {
                    addAction(com.cims.ue.core.CimsSuite.ACTION_ROUTE_YIELD)
                    addAction(com.cims.ue.core.CimsSuite.ACTION_ROUTE_RESUME)
                },
                com.cims.ue.core.CimsSuite.PERMISSION, null,
                androidx.core.content.ContextCompat.RECEIVER_EXPORTED,
            )
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val autostart = intent?.getBooleanExtra("autostart", false) == true
        // 사용자 실행(포그라운드)이면 talk 대비 mic 로 승격; 부팅 자동시작이면 specialUse 유지.
        if (!autostart) elevateForCall(true)
        // 공유 계정 있으면 최신 정보 재취득(포트/비번 변경 반영). 없으면 캐시 설정으로 등록.
        if (autostart && com.cims.ue.core.account.SsoProvisioner.hasAccount(this)) {
            scope.launch(kotlinx.coroutines.Dispatchers.IO) { ssoAutoConfigure() }
        } else {
            ensureRegistered()
        }
        return START_STICKY
    }

    /** SSO(공유 계정) → /provisioning/me(kind=ptt) → ConfigStore 저장 → 등록. 블로킹(IO).
     *  실패(owner 앱 bind failure 등)해도 던지지 않고 캐시 설정으로 등록을 진행한다. */
    private fun ssoAutoConfigure() {
        runCatching {
            val prof = com.cims.ue.core.account.SsoProvisioner.fetchProfile(this) ?: return@runCatching
            val svc = prof.service("ptt") ?: return@runCatching
            val cfg = svc.toSipAccountConfig(
                loginId = prof.loginId ?: svc.msisdn,
                displayName = prof.displayName ?: svc.msisdn,
                loginPassword = com.cims.ue.core.account.SsoProvisioner.loginPassword(this),
                countryCode = prof.countryCode.orEmpty(),
            )
            ConfigStore(this).save(cfg)
        }
        ensureRegistered()
    }

    private var activeConfig: com.cims.ue.core.config.SipAccountConfig? = null

    fun ensureRegistered() {
        val store = ConfigStore(this)
        // 로그아웃 상태(계정 없음, 수동 모드 아님) — 캐시 설정이 남아 있어도 등록하지 않고 종료.
        // 로그아웃 브로드캐스트 유실 후 START_STICKY/키 경로 재기동의 stale 재등록 방지 안전망.
        if (!store.isManual() && !com.cims.ue.core.account.SsoProvisioner.hasAccount(this)) {
            update("CIMS-McPtt", "로그인 필요")
            stopSip()
            return
        }
        val cfg = store.load()
        if (!cfg.isComplete()) { update("CIMS-McPtt", "로그인 필요"); return }
        if (controller != null && activeConfig == cfg) {        // 동일 설정 → 그대로(토큰만 보강)
            controller?.let { injectSsoToken(it) }
            return
        }
        // 설정 변경(포트/비번) — 프로세스 내 PJSIP 재부팅(libDestroy→Endpoint 재생성)은
        // Endpoint/LogWriter 수명 지뢰라 로그아웃과 동일하게 프로세스 재시작이 정석:
        // un-REGISTER 송신 여유(2s) 후 killProcess → 접근성/START_STICKY 재기동 →
        // 새 설정 첫 부팅 + 참여 채널 자동 복원(ChannelStore).
        controller?.let {
            runCatching { it.sip.unregister() }
            update("CIMS-McPtt", "설정 변경 — 재시작")
            mainHandler.postDelayed(
                { android.os.Process.killProcess(android.os.Process.myPid()) }, 2000)
            return
        }
        // msisdn 은 프로비저닝에 따라 "+8250..."/"8250..." 혼재 — tel: URI 로 정규화(+ 중복 방지)
        val mcpttId = "tel:" + cfg.msisdn.removePrefix("tel:").let { if (it.startsWith("+")) it else "+$it" }
        val csc = CscConfig(host = cfg.serverHost)               // IdMS/GMS/CMS 4430 (dev: 자체서명)
        val c = PttController(cfg, mcpttId, csc, allowInsecureTls = true).also { _controller.value = it; activeConfig = cfg }
        c.feedback = com.cims.ue.ptt.audio.PttFeedback(this)
        c.volumeStore = com.cims.ue.ptt.audio.GroupVolumeStore(this)
        c.channelStore = ChannelStore(this)         // 참여 채널 영속 — 재시작 자동 재조인
        c.audioRouter = audioRouter
        val rp = com.cims.ue.ptt.audio.AudioRoutePrefs(this)
        c.routePrefs = rp
        c.setAudioRoute(rp.route, rp.headsetId)     // 저장된 라우팅 복원(기본=스피커폰)
        c.setAudioGain(rp.spkGain, rp.micGain)      // 저장된 무전 게인 복원(기본=×1.5)
        c.micHandoff = { talk -> sendMicHandoff(talk) }
        observeHeadsets(c)
        observe(c)
        c.register()
        // 유휴 기본 = 스피커 전용(마이크 미보유) — 발언(setTalkCapture)에서만 전이중.
        // register() 뒤에 두어 PjLib 부팅 이후 적용됨을 보장(onCtl 직렬).
        c.sip.setCaptureEnabled(false)
        // 라우팅 재적용 — PjLib 부팅 후 keep 저장을 확보해 첫 snd 오픈부터 무전 트랙이
        // 분리 라우팅(STREAM_MUSIC+트랙 장치 고정, pjsip 패치)으로 생성되게 한다.
        c.setAudioRoute(rp.route, rp.headsetId)
        injectSsoToken(c)
    }

    /** volte 앱 마이크 핸드오프 통지 — 발언 시작=YIELD/종료=RESUME. 서명(CIMS_SUITE) 보호 명시적 브로드캐스트.
     *  발언 시작 시 FGS microphone 승격도 함께 시도 — 부팅 자동시작(specialUse) 후 첫 발언 대비. */
    private fun sendMicHandoff(talk: Boolean) {
        if (talk) elevateForCall(true)
        val action = if (talk) com.cims.ue.core.CimsSuite.ACTION_MIC_YIELD
        else com.cims.ue.core.CimsSuite.ACTION_MIC_RESUME
        runCatching {
            sendBroadcast(
                Intent(action).setPackage(com.cims.ue.core.CimsSuite.VOLTE_PACKAGE),
                com.cims.ue.core.CimsSuite.PERMISSION,
            )
        }
    }

    /** CIMS 공유 계정의 MCPTT 토큰(TS 33.180)을 서비스에서 직접 주입 — **로그인만으로**(PTT 앱을
     *  열지 않아도) 그룹 조회 → 선택 그룹 affiliation PUBLISH 까지 진행돼 REGISTER 직후부터
     *  CSP fan-out(그룹콜/SDS) 대상이 된다. UI(AppRoot) 주입과 중복돼도 무해(보유 시 생략). */
    private fun injectSsoToken(c: PttController) {
        if (c.hasAccessToken) return
        if (!com.cims.ue.core.account.SsoProvisioner.hasAccount(this)) return
        scope.launch(kotlinx.coroutines.Dispatchers.IO) {
            runCatching {
                val am = android.accounts.AccountManager.get(this@PttService)
                val acct = com.cims.ue.core.account.CimsAccounts.get(am) ?: return@launch
                val tok = com.cims.ue.core.account.CimsAccounts.blockingToken(
                    am, acct, com.cims.ue.core.account.CimsAccounts.TOKEN_MCPTT)
                if (tok != null && controller === c && !c.hasAccessToken) c.setAccessToken(tok)
            }
        }
    }

    /** 이어폰 연결/해제 자동 전환 — 연결=그 이어폰으로, (이어폰 사용 중) 해제=남은 이어폰 또는 스피커폰. */
    private fun observeHeadsets(c: PttController) {
        routeJob?.cancel()
        routeJob = scope.launch {
            var prev = audioRouter.headsets.value
            audioRouter.headsets.collect { cur ->
                val added = cur.filter { h -> prev.none { it.id == h.id } }
                if (added.isNotEmpty()) {
                    c.setAudioRoute(PttController.AUDIO_ROUTE_HEADSET, added.last().id)
                } else if (c.audioRoute.value == PttController.AUDIO_ROUTE_HEADSET &&
                    cur.none { it.id == c.headsetId.value }
                ) {
                    if (cur.isNotEmpty()) c.setAudioRoute(PttController.AUDIO_ROUTE_HEADSET, cur.first().id)
                    else c.setAudioRoute(com.cims.ue.core.sip.SipController.AUDIO_ROUTE_SPEAKER)
                    // 라우팅 중이던 장치 소멸 — 재생 트랙에 시스템 뮤트가 고착되는 단말(MF52/A15
                    // 실측)이 있어, 정책 재라우팅이 가라앉은 뒤 장치를 재오픈해 트랙을 재생성한다.
                    delay(500)
                    c.recoverFromDeviceLoss()
                }
                prev = cur
            }
        }
    }

    fun stopSip() {
        controller?.let { runCatching { it.sip.unregister() } }   // 명시 종료 — 서버 등록도 해제
        controller?.shutdown()
        _controller.value = null
        mainHandler.post { overlay.hide() }
        stopForegroundCompat()
        stopSelf()
    }

    private fun observe(c: PttController) {
        job?.cancel()
        // 이전 세션의 미결 PENDING — 결과 이벤트 유실 상태이므로 실패로 마감(재전송 가능)
        if (messages.failStalePending()) _messageTick.value++
        c.onEvent = { e -> history.add(e.groupId, e.kind.name, e.peer, e.durationMs) }
        job = scope.launch {
            c.status.onEach { update("CIMS PTT", it) }.launchIn(this)
            // 전역 상태 아이콘 배지 — 등록됨=초록/연결 중=황색/해제=회색/실패=적색 (CIMS-Phone 과 동일 색).
            c.regState.onEach { reg ->
                val color = when (reg) {
                    is com.cims.ue.core.sip.RegState.Registered -> 0xFF00C853.toInt()
                    com.cims.ue.core.sip.RegState.Registering,
                    com.cims.ue.core.sip.RegState.Idle -> 0xFFF9A825.toInt()
                    com.cims.ue.core.sip.RegState.Unregistered -> 0xFF9E9E9E.toInt()
                    is com.cims.ue.core.sip.RegState.Failed -> 0xFFEA4335.toInt()
                }
                mainHandler.post { overlay.update(color, "PTT ${if (reg is com.cims.ue.core.sip.RegState.Registered) "가능" else "연결 안 됨"}") }
            }.launchIn(this)
            // 수신 문자(SIP MESSAGE) → 인박스 영속.
            //  - MCData SDS(multipart/mixed): mcdata-info 의 그룹 URI 로 그룹 스레드 귀속 +
            //    disposition 요청 시 DELIVERED 통지 회신, DELIVERED 통지 수신 시 발신 문자에 반영.
            //  - text/plain(구버전 앱 호환): 발신자 스레드로 저장.
            c.incomingMessage.onEach { im ->
                val sender = PttController.bareId(im.fromUri)
                if (im.contentType.lowercase().startsWith("multipart/mixed")) {
                    when (val p = McDataCodec.parse(im.contentType, im.body)) {
                        is McDataCodec.SdsMessage -> onSdsParsed(p, sender)
                        is McDataCodec.FdMessage -> {
                            if (p.fileUrl.isNotBlank()) {
                                val gid = p.groupUri?.let(PttController::bareId)
                                    ?.takeUnless { it.isBlank() } ?: sender
                                messages.add(gid, "", com.cims.ue.core.message.MsgDirection.IN,
                                    sender = sender, msgId = p.msgId,
                                    attName = p.fileName.ifBlank { "file.bin" },
                                    attUrl = p.fileUrl, attSize = p.fileSize)
                                _messageTick.value++
                                // 자동 다운로드 — 그룹문서 auto-recv 임계 이내 (TS 24.481)
                                val autoRecv = c.groupDocs.value[gid]?.autoRecvBytes ?: DEFAULT_AUTO_RECV
                                if (p.fileSize in 1..autoRecv.toLong()) {
                                    downloadAttachment(p.msgId, p.fileUrl, p.fileName)
                                }
                            }
                        }
                        is McDataCodec.SdsNotification -> {
                            if (p.type == McDataCodec.NOTIF_DELIVERED ||
                                p.type == McDataCodec.NOTIF_DELIVERED_READ) {
                                if (messages.markDelivered(p.msgId)) _messageTick.value++
                            }
                        }
                        null -> {}
                    }
                } else if (im.contentType.startsWith("text/")) {
                    messages.add(sender, im.body, com.cims.ue.core.message.MsgDirection.IN)
                    _messageTick.value++
                }
            }.launchIn(this)
            // MSRP 발신 결과 → 말풍선 상태(SENT/FAILED) 반영 + 진행률 정리
            c.sendResult.onEach { (msgId, ok) ->
                _sendProgress.value = _sendProgress.value - msgId
                val st = if (ok) com.cims.ue.core.message.SendState.SENT
                else com.cims.ue.core.message.SendState.FAILED
                if (messages.setSendState(msgId, st)) _messageTick.value++
            }.launchIn(this)
            // MSRP 발신 진행률(청크 단위) → 말풍선 진행 바
            c.sendProgress.onEach { p ->
                if (p.total > 0) _sendProgress.value =
                    _sendProgress.value + (p.msgId to p.sent.toFloat() / p.total)
            }.launchIn(this)
            // MSRP 미디어평면 수신 SDS (대용량 — TS 24.282 §9.2.3) → 동일 저장·통지 경로.
            // 발신자 미상(구서버 — mcdata-info 없는 배포 레그, sender==groupId 폴백)이면
            // 통지 대상이 그룹이 되므로 회신 억제(notifiable=false).
            c.incomingSds.onEach { m ->
                onSdsParsed(m.msg, m.sender, gidOverride = m.groupId,
                    notifiable = m.sender != m.groupId)
            }.launchIn(this)
        }
    }

    /** 수신 SDS 공통 처리 — C-plane MESSAGE 와 MSRP 미디어평면 공용(저장·tick·DELIVERED 통지). */
    private fun onSdsParsed(
        p: McDataCodec.SdsMessage,
        sender: String,
        gidOverride: String? = null,
        notifiable: Boolean = true,
    ) {
        val gid = gidOverride
            ?: p.groupUri?.let(PttController::bareId)?.takeUnless { it.isBlank() }
            ?: sender
        if (p.text.isNotEmpty()) {
            messages.add(gid, p.text, com.cims.ue.core.message.MsgDirection.IN,
                sender = sender, msgId = p.msgId)
            _messageTick.value++
        }
        if (notifiable && p.dispositionReq and McDataCodec.DISP_REQ_DELIVERY != 0) {
            controller?.sendSdsNotification(sender, p.convId, p.msgId, McDataCodec.NOTIF_DELIVERED)
        }
    }

    override fun onDestroy() {
        instance = null
        runCatching { unregisterReceiver(vendorKeyReceiver) }
        runCatching { unregisterReceiver(routeHandoffReceiver) }
        mainHandler.removeCallbacks(routeResumeWatchdog)
        job?.cancel()
        routeJob?.cancel()
        audioRouter.close()
        controller?.shutdown()
        _controller.value = null
        mainHandler.post { overlay.hide() }
        super.onDestroy()
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
                .createNotificationChannel(NotificationChannel(CH, "PTT 등록/통화", NotificationManager.IMPORTANCE_LOW))
        }
    }

    private fun notification(title: String, text: String): Notification =
        NotificationCompat.Builder(this, CH)
            .setContentTitle(title).setContentText(text)
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .setOngoing(true).setPriority(NotificationCompat.PRIORITY_LOW).build()

    private fun update(title: String, text: String) =
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).notify(NID, notification(title, text))

    // Android 14+: 부팅 등록단계는 specialUse(백그라운드 시작 가능), talk 시 mic 로 승격.
    private fun fgsType(inCall: Boolean): Int = when {
        Build.VERSION.SDK_INT >= 34 ->
            if (inCall) ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            else ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q -> ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        else -> 0
    }

    private fun startForegroundCompat(n: Notification, inCall: Boolean = false) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) startForeground(NID, n, fgsType(inCall))
        else startForeground(NID, n)
    }

    /** talk(floor) 활성 시 mic 타입 승격 (포그라운드 사용 시).
     *  백그라운드(START_STICKY 재기동 등)에서 microphone 승격은 API 34+ 금지 —
     *  거부돼도 던지지 않고 specialUse 로 유지(포그라운드 진입 후 while-in-use 마이크로 동작). */
    private fun elevateForCall(active: Boolean) {
        if (Build.VERSION.SDK_INT >= 34) {
            val n = notification("CIMS-McPtt", if (active) "PTT 사용 중" else "등록 유지")
            runCatching { startForegroundCompat(n, inCall = active) }
                .onFailure {
                    android.util.Log.w("PttService", "FGS 승격 거부(inCall=$active) — specialUse 유지", it)
                    runCatching { startForegroundCompat(n, inCall = false) }
                }
        }
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) stopForeground(STOP_FOREGROUND_REMOVE)
        else @Suppress("DEPRECATION") stopForeground(true)
    }

    companion object {
        /** 실행 중 서비스 — 백그라운드 키 경로(PttKeyService/VendorPttReceiver)가 컨트롤러 접근용.
         *  Activity 는 기존대로 bind 사용. */
        @Volatile var instance: PttService? = null
            private set
        private const val CH = "cims_ptt"
        private const val NID = 2001
        /** 라우팅 양보 워치독 — volte 재송신 주기(5분)의 2주기+여유. */
        private const val ROUTE_YIELD_MAX_MS = 660_000L
        /** 통화 중 무전 트랙 핀 재적용 주기(양보 동안 지속 — 정책 재라우팅 수렴 보장). */
        private const val ROUTE_REPIN_PERIOD_MS = 4_000L
        /** 복귀 검증 지연/재에지 간격/한도. */
        private const val RESUME_VERIFY_DELAY_MS = 1_200L
        private const val REEDGE_GAP_MS = 250L
        private const val RESUME_VERIFY_MAX_TRIES = 2
        /** 첨부 크기 상한 — CSC McDataFd.MaxBytes 기본값과 동일(50MB). */
        private const val MAX_ATTACH = 52428800L
        /** 그룹문서에 auto-recv 미지정 시 자동 다운로드 임계 (1MB). */
        private const val DEFAULT_AUTO_RECV = 1048576
        fun start(ctx: Context) {
            // autostart=true → 공유 계정(SSO)으로 자동 구성(별도 로그인 없음).
            val i = Intent(ctx, PttService::class.java).putExtra("autostart", true)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i) else ctx.startService(i)
        }
    }
}
