// 관제 세션의 수명 — Foreground Service (docs/design/features/android_dispatch_tablet.md §6.1)
//
// Activity 가 재생성돼도 등록·세션이 끊기지 않게 **여기서 엔진을 든다**. 데스크톱은 프로세스 수명이 곧
// 앱 수명이라 대응물이 없는 층이다.
//
// 세션 객체는 프로세스 안에서 하나다 — 바인딩 대신 정적 접근으로 나눈다(Activity 가 재생성돼도
// 같은 객체를 본다). 프로세스가 회수되면 함께 사라지고, 복귀는 §6.1 규칙대로 재로그인부터 한다.
package com.cims.ue.dispatch.session

import android.app.Notification
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import androidx.core.app.NotificationCompat
import com.cims.ue.sdk.platform.UeForegroundService
import com.cims.ue.sdk.CallState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

class DispatchService : UeForegroundService() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** 착신 알림·벨소리 — 화면이 꺼져 있어도 울려야 한다(§6.2). */
    private val alert by lazy { IncomingAlert(applicationContext) }

    override val channelName: String get() = "관제 등록 유지"

    /**
     * 등록만 유지하는 동안의 타입. 캡처 중에는 [setMicrophoneActive] 가 마이크를 더한다.
     *
     * API 34+ 는 **specialUse** 다. `dataSync` 를 쓰지 않는 이유는 Android 15+ 에서 하루 6시간 누적
     * 제한이 걸리고 `BOOT_COMPLETED` 에서 시작할 수 없어 «상주·부팅 재등록»(§5·§6.1)이 성립하지
     * 않기 때문이다.
     *
     * API 29~33 은 **dataSync** 다 — `specialUse` 상수 자체가 API 34 에 생겼다. 그 제한들은 15+ 에만
     * 적용되므로 이 구간에서는 문제가 없다.
     *
     * **여기서 내는 값은 매니페스트 `foregroundServiceType` 의 부분집합이어야 한다.** 아니면
     * `startForeground` 가 `IllegalArgumentException` 으로 거절해 서비스가 서지 못한다(API 29+).
     * 그래서 매니페스트에 `specialUse|microphone|dataSync` 셋을 모두 선언한다.
     */
    override val baseServiceType: Int
        get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
            ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
        else ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC

    override fun onCreate() {
        super.onCreate()
        val s = DispatchSession(applicationContext, scope, SettingsStore(applicationContext))
        session = s
        _sessionFlow.value = s              // 화면이 «준비됨» 을 관측한다(§F8)
        acquireWakeLock()
        observeMicrophone(s)                // 캡처 중에는 FGS 타입에 마이크를 더한다(§F6)
        observeIncoming(s)                  // 착신 알림·벨소리
        // 저장된 자격이 있으면 화면 없이도 등록까지 되돌린다(부팅·프로세스 복귀).
        if (s.hasSavedLogin) scope.launch {
            if (s.resume().ok) s.start()
            updateNotification()
        }
    }

    /**
     * 기반 클래스는 START_STICKY 로 되살아난다(프로세스 회수 대비). [ACTION_SHUTDOWN] 은 **사용자가 앱을
     * 끝낸 것**이므로 등록을 풀고 내려간다 — 이 경로가 없으면 앱을 닫아도 서비스가 살아나 계속 등록 상태로
     * 남는다.
     */
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_SHUTDOWN -> {
                session?.logout()
                stopSelf()
                return START_NOT_STICKY
            }
            // 알림에서 바로 받거나 거절한다 — 화면을 켜지 않고도 응대해야 한다.
            ACTION_ANSWER, ACTION_REJECT -> {
                val id = intent.getIntExtra(IncomingAlert.EXTRA_CALL_ID, -1)
                val s = session
                if (id >= 0 && s != null) scope.launch {
                    if (intent.action == ACTION_ANSWER) s.answer(id) else s.reject(id)
                }
                alert.clear()
                return START_STICKY
            }
        }
        return super.onStartCommand(intent, flags, startId)
    }

    /**
     * 착신 스택 → 알림·벨소리.
     *
     * 배너는 앱 화면이 보일 때만 소용이 있다. 이 구독이 없으면 화면이 꺼진 동안 걸려 온 전화를
     * 아무도 모르고, 발신자가 끊어 서버 이력에 «거절» 로 남는다.
     */
    private fun observeIncoming(s: DispatchSession) {
        scope.launch {
            s.incoming.collect { list ->
                val top = list.firstOrNull()
                alert.apply(top, top?.let { s.displayLabel(it.info.remoteUri) }.orEmpty())
            }
        }
    }

    /**
     * 캡처가 필요한 세션이 있으면 FGS 타입에 마이크를 더한다(§5).
     *
     * 타입을 바꾸려면 `startForeground` 를 다시 불러야 하므로 접점층의 [setMicrophoneActive] 를 쓴다 —
     * 알림만 다시 그리면 타입은 그대로다. 이것이 없으면 화면을 벗어난 통화·잠금 발언에서 배경 캡처가 막힌다.
     */
    private fun observeMicrophone(s: DispatchSession) {
        scope.launch {
            s.sessions.collect { list ->
                // 청취 전용(recvonly)은 캡처하지 않는다 — 마이크 타입을 요구할 이유가 없다.
                val capturing = list.any {
                    it.isLive && !it.info.listenOnly && it.info.state != CallState.DISCONNECTED
                }
                setMicrophoneActive(capturing)
                updateNotification()
            }
        }
    }

    override fun onDestroy() {
        alert.clear()
        _sessionFlow.value = null
        session?.close()
        session = null
        scope.cancel()
        super.onDestroy()
    }

    override fun buildNotification(builder: NotificationCompat.Builder): Notification {
        val s = session
        val text = when {
            s == null -> "준비 중"
            s.isReady && s.anyRegistered() -> "등록됨 · ${s.profile.value?.displayName.orEmpty()}"
            s.isReady -> "등록 대기"
            else -> "로그인 필요"
        }
        return builder.setContentTitle("CIMS 관제").setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_speakerphone).build()
    }

    override fun onForegroundFailed(cause: Throwable) {
        // 전경 승격이 막히면 서비스가 곧 회수된다 — 조용히 넘기지 않는다.
        android.util.Log.e("DispatchService", "startForeground 실패", cause)
    }

    companion object {
        /** 프로세스 안의 단일 세션. Activity·ViewModel 이 이것을 본다. */
        @Volatile
        var session: DispatchSession? = null
            private set

        private val _sessionFlow = MutableStateFlow<DispatchSession?>(null)
        /**
         * 세션의 준비·교체·종료를 **관측 가능하게** 낸다.
         *
         * 정적 필드만으로는 Compose 가 재구성을 걸지 못해, Service 생성보다 화면이 먼저 서면 대기 화면에
         * 머무른다(§F8). 화면은 이 Flow 를 구독한다.
         */
        val sessionFlow: StateFlow<DispatchSession?> = _sessionFlow.asStateFlow()

        /** 사용자가 앱을 끝낸다 — 등록 해제 + 서비스 종료. */
        const val ACTION_SHUTDOWN = "com.cims.ue.dispatch.SHUTDOWN"

        /** 착신 알림의 [응답]·[거절]. */
        const val ACTION_ANSWER = "com.cims.ue.dispatch.ANSWER"
        const val ACTION_REJECT = "com.cims.ue.dispatch.REJECT"

        fun start(context: Context) =
            UeForegroundService.start(context, Intent(context, DispatchService::class.java))

        /** 완전 종료 — 이 뒤에는 서비스가 되살아나지 않는다. */
        fun shutdown(context: Context) {
            runCatching {
                context.startService(Intent(context, DispatchService::class.java).setAction(ACTION_SHUTDOWN))
            }
        }
    }
}
