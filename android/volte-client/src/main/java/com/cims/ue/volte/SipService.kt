package com.cims.ue.volte

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.Network
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
    private var netCallback: ConnectivityManager.NetworkCallback? = null

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
        registerNetworkCallback()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // 포그라운드 복귀/네트워크 복귀 등 keepalive 트리거 — 등록만 재시도(계정 있으면 reregister).
        if (intent?.getBooleanExtra("reregister", false) == true) {
            if (controller?.hasAccount() == true) controller?.reregister() else ensureRegistered()
            return START_STICKY
        }
        // 부팅/SSO 자동시작: 설정이 비어 있고 공유 계정이 있으면 프로비저닝으로 자동 구성 후 등록.
        val autostart = intent?.getBooleanExtra("autostart", false) == true
        if (autostart && !ConfigStore(this).load().isComplete()) {
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

    /** SSO(공유 계정) → /provisioning/me(kind=volte) → ConfigStore 저장 → 등록. 블로킹(IO). */
    private fun ssoAutoConfigure() {
        val prof = com.cims.ue.core.account.SsoProvisioner.fetchProfile(this) ?: return
        val svc = prof.service("volte") ?: return
        val cfg = svc.toSipAccountConfig(
            loginId = prof.loginId ?: svc.msisdn,
            displayName = prof.displayName ?: svc.msisdn,
            loginPassword = com.cims.ue.core.account.SsoProvisioner.loginPassword(this),  // sipPassword=null → 공유 로그인 비번 재사용
        )
        ConfigStore(this).save(cfg)
        ensureRegistered()
    }

    private var activeConfig: com.cims.ue.core.config.SipAccountConfig? = null

    /** 설정이 완성되어 있으면 컨트롤러를 만들고 REGISTER. 설정이 바뀌면(재프로비저닝) 재등록. 멱등. */
    fun ensureRegistered() {
        val cfg = ConfigStore(this).load()
        if (!cfg.isComplete()) {
            updateNotification("CIMS Phone", "로그인 필요")
            return
        }
        if (controller != null && activeConfig == cfg) return   // 동일 설정 → 그대로
        // 최초 또는 설정 변경(포트/비번 등) → 기존 컨트롤러 정리 후 재등록
        controller?.let { runCatching { it.unregister() }; runCatching { it.shutdown() } }
        val c = SipController(cfg).also { controller = it; activeConfig = cfg }
        observe(c)
        c.register()
    }

    fun makeCall(dst: String) = controller?.makeCall(dst)

    /** 문자(SIP MESSAGE, RFC 3428 page-mode) 송신. 대상=sip:번호@도메인. */
    fun sendMessage(dst: String, text: String) {
        val cfg = ConfigStore(this).load()
        controller?.sendRequest(
            method = "MESSAGE",
            targetUri = "sip:${dst}@${cfg.domain}",
            contentType = "text/plain",
            body = text,
        )
    }
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
                elevateForCall(call is CallState.Active || call is CallState.Outgoing || call is CallState.Incoming)
                val line = when (call) {
                    is CallState.Incoming -> "수신: ${call.remote}"
                    is CallState.Outgoing -> "발신: ${call.remote}"
                    is CallState.Active -> "통화 중: ${call.remote}"
                    is CallState.Disconnected -> null
                    CallState.Null -> null
                }
                if (line != null) updateNotification("CIMS Phone", line)
            }.launchIn(this)
        }
    }

    override fun onDestroy() {
        stateJob?.cancel()
        netCallback?.let { cb -> runCatching { getSystemService(ConnectivityManager::class.java)?.unregisterNetworkCallback(cb) } }
        netCallback = null
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

    /** 통화 활성/종료 시 FGS 타입 승격/복귀 (마이크 접근 권한 보장). */
    private fun elevateForCall(active: Boolean) {
        if (Build.VERSION.SDK_INT >= 34) {
            startForegroundCompat(buildNotification("CIMS Phone", if (active) "통화 중" else "등록 유지"), inCall = active)
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

        /** 포그라운드 복귀 시 등록 재시도 트리거(keepalive). 서비스가 죽어 있었으면 기동+등록. */
        fun poke(ctx: Context) {
            val i = Intent(ctx, SipService::class.java).putExtra("reregister", true)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i)
            else ctx.startService(i)
        }
    }
}
