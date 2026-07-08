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
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch

/**
 * MCPTT 등록 유지 Foreground Service. [PttController] 를 소유하고 REGISTER 를 유지하며 상태를 알림에 노출.
 * Activity 는 [LocalBinder] 로 바인드해 컨트롤러 flow 를 관찰·제어한다.
 */
class PttService : Service() {

    private val scope = CoroutineScope(SupervisorJob())
    // StateFlow — 바인드 시점에 아직 컨트롤러가 없어도(SSO 재취득 중) 생성 시 UI 재구성
    private val _controller = kotlinx.coroutines.flow.MutableStateFlow<PttController?>(null)
    val controllerFlow: kotlinx.coroutines.flow.StateFlow<PttController?> = _controller
    val controller: PttController? get() = _controller.value
    private var job: Job? = null

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

    override fun onCreate() {
        super.onCreate()
        createChannel()
        startForegroundCompat(notification("CIMS PTT", "시작 중…"))
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
        val cfg = ConfigStore(this).load()
        if (!cfg.isComplete()) { update("CIMS-McPtt", "로그인 필요"); return }
        if (controller != null && activeConfig == cfg) return   // 동일 설정 → 그대로
        controller?.let { runCatching { it.shutdown() } }       // 설정 변경(포트/비번) → 재등록
        // msisdn 은 프로비저닝에 따라 "+8250..."/"8250..." 혼재 — tel: URI 로 정규화(+ 중복 방지)
        val mcpttId = "tel:" + cfg.msisdn.removePrefix("tel:").let { if (it.startsWith("+")) it else "+$it" }
        val csc = CscConfig(host = cfg.serverHost)               // IdMS/GMS/CMS 4430 (dev: 자체서명)
        val c = PttController(cfg, mcpttId, csc, allowInsecureTls = true).also { _controller.value = it; activeConfig = cfg }
        c.feedback = com.cims.ue.ptt.audio.PttFeedback(this)
        observe(c)
        c.register()
    }

    fun stopSip() {
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
        job?.cancel()
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

    /** talk(floor) 활성 시 mic 타입 승격 (포그라운드 사용 시). */
    private fun elevateForCall(active: Boolean) {
        if (Build.VERSION.SDK_INT >= 34) startForegroundCompat(notification("CIMS-McPtt", if (active) "PTT 사용 중" else "등록 유지"), inCall = active)
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) stopForeground(STOP_FOREGROUND_REMOVE)
        else @Suppress("DEPRECATION") stopForeground(true)
    }

    companion object {
        private const val CH = "cims_ptt"
        private const val NID = 2001
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
