package com.cims.ue.volte

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
import com.cims.ue.core.sip.CallState
import com.cims.ue.core.sip.RegState
import com.cims.ue.core.sip.SipController
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
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

    private val scope = CoroutineScope(SupervisorJob())
    private var controller: SipController? = null
    private var stateJob: Job? = null

    val regState: StateFlow<RegState>? get() = controller?.regState
    val callState: StateFlow<CallState>? get() = controller?.callState

    inner class LocalBinder : Binder() {
        val service: SipService get() = this@SipService
    }

    private val binder = LocalBinder()
    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        createChannel()
        startForegroundCompat(buildNotification("CIMS VoLTE", "시작 중…"))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        ensureRegistered()
        return START_STICKY
    }

    /** 설정이 완성되어 있으면 컨트롤러를 만들고 REGISTER. 멱등. */
    fun ensureRegistered() {
        if (controller != null) return
        val cfg = ConfigStore(this).load()
        if (!cfg.isComplete()) {
            updateNotification("CIMS VoLTE", "설정 필요")
            return
        }
        val c = SipController(cfg).also { controller = it }
        observe(c)
        c.register()
    }

    fun makeCall(dst: String) = controller?.makeCall(dst)
    fun answer(callId: Int) = controller?.answer(callId)
    fun reject(callId: Int) = controller?.reject(callId)
    fun hangup(callId: Int) = controller?.hangup(callId)

    /** M1.3 영상: 발신 전 영상 on/off + 수신 영상 렌더 Surface 전달. */
    fun setVideoEnabled(on: Boolean) { controller?.videoEnabled = on }
    fun setVideoSurface(surface: Any?) { controller?.setVideoSurface(surface) }

    /** 등록 해제 + Endpoint 정리 + 서비스 종료. */
    fun stopSip() {
        controller?.unregister()
        controller?.shutdown()
        controller = null
        stopForegroundCompat()
        stopSelf()
    }

    private fun observe(c: SipController) {
        stateJob?.cancel()
        stateJob = scope.launch {
            c.regState.onEach { reg ->
                val line = when (reg) {
                    is RegState.Registered -> "등록됨 (${reg.code})"
                    RegState.Registering -> "등록 중…"
                    RegState.Unregistered -> "등록 해제"
                    is RegState.Failed -> "등록 실패: ${reg.reason}"
                    RegState.Idle -> "대기"
                }
                updateNotification("CIMS VoLTE", line)
            }.launchIn(this)

            c.callState.onEach { call ->
                val line = when (call) {
                    is CallState.Incoming -> "수신: ${call.remote}"
                    is CallState.Outgoing -> "발신: ${call.remote}"
                    is CallState.Active -> "통화 중: ${call.remote}"
                    is CallState.Disconnected -> null
                    CallState.Null -> null
                }
                if (line != null) updateNotification("CIMS VoLTE", line)
            }.launchIn(this)
        }
    }

    override fun onDestroy() {
        stateJob?.cancel()
        controller?.shutdown()
        controller = null
        super.onDestroy()
    }

    // ── 알림 ──

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(CHANNEL_ID, "VoLTE 등록/통화", NotificationManager.IMPORTANCE_LOW)
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(ch)
        }
    }

    private fun buildNotification(title: String, text: String): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(title)
            .setContentText(text)
            .setSmallIcon(android.R.drawable.sym_action_call)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()

    private fun updateNotification(title: String, text: String) {
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .notify(NOTIF_ID, buildNotification(title, text))
    }

    private fun startForegroundCompat(n: Notification) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        } else {
            startForeground(NOTIF_ID, n)
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
        private const val CHANNEL_ID = "cims_volte"
        private const val NOTIF_ID = 1001

        fun start(ctx: Context) {
            val i = Intent(ctx, SipService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i)
            else ctx.startService(i)
        }
    }
}
