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
        ensureRegistered()
        return START_STICKY
    }

    fun ensureRegistered() {
        if (controller != null) return
        val cfg = ConfigStore(this).load()
        if (!cfg.isComplete()) { update("CIMS PTT", "설정 필요"); return }
        val mcpttId = "tel:+${cfg.msisdn}"
        val csc = CscConfig(host = cfg.serverHost)               // IdMS/GMS/CMS 4430 (dev: 자체서명)
        val c = PttController(cfg, mcpttId, csc, allowInsecureTls = true).also { controller = it }
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

    private fun startForegroundCompat(n: Notification) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            startForeground(NID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        else startForeground(NID, n)
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) stopForeground(STOP_FOREGROUND_REMOVE)
        else @Suppress("DEPRECATION") stopForeground(true)
    }

    companion object {
        private const val CH = "cims_ptt"
        private const val NID = 2001
        fun start(ctx: Context) {
            val i = Intent(ctx, PttService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i) else ctx.startService(i)
        }
    }
}
