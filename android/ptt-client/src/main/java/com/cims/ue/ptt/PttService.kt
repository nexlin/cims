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
    var controller: PttController? = null
        private set
    private var job: Job? = null

    inner class LocalBinder : Binder() {
        val service: PttService get() = this@PttService
    }

    private val binder = LocalBinder()
    override fun onBind(intent: Intent?): IBinder = binder

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

    /** SSO(공유 계정) → /provisioning/me(kind=ptt) → ConfigStore 저장 → 등록. 블로킹(IO). */
    private fun ssoAutoConfigure() {
        val prof = com.cims.ue.core.account.SsoProvisioner.fetchProfile(this) ?: return
        val svc = prof.service("ptt") ?: return
        val cfg = svc.toSipAccountConfig(
            loginId = prof.loginId ?: svc.msisdn,
            displayName = prof.displayName ?: svc.msisdn,
            loginPassword = com.cims.ue.core.account.SsoProvisioner.loginPassword(this),
        )
        ConfigStore(this).save(cfg)
        ensureRegistered()
    }

    private var activeConfig: com.cims.ue.core.config.SipAccountConfig? = null

    fun ensureRegistered() {
        val cfg = ConfigStore(this).load()
        if (!cfg.isComplete()) { update("CIMS-McPtt", "로그인 필요"); return }
        if (controller != null && activeConfig == cfg) return   // 동일 설정 → 그대로
        controller?.let { runCatching { it.shutdown() } }       // 설정 변경(포트/비번) → 재등록
        val mcpttId = "tel:+${cfg.msisdn}"
        val csc = CscConfig(host = cfg.serverHost)               // IdMS/GMS/CMS 4430 (dev: 자체서명)
        val c = PttController(cfg, mcpttId, csc, allowInsecureTls = true).also { controller = it; activeConfig = cfg }
        observe(c)
        c.register()
    }

    fun stopSip() {
        controller?.shutdown()
        controller = null
        stopForegroundCompat()
        stopSelf()
    }

    private fun observe(c: PttController) {
        job?.cancel()
        job = scope.launch {
            c.status.onEach { update("CIMS PTT", it) }.launchIn(this)
        }
    }

    override fun onDestroy() {
        job?.cancel()
        controller?.shutdown()
        controller = null
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
        fun start(ctx: Context) {
            // autostart=true → 공유 계정(SSO)으로 자동 구성(별도 로그인 없음).
            val i = Intent(ctx, PttService::class.java).putExtra("autostart", true)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i) else ctx.startService(i)
        }
    }
}
