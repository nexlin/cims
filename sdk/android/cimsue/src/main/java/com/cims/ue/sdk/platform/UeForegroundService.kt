// Android 접점 — 등록 유지 Foreground Service (docs/design/features/android_dispatch_tablet.md §5·§6.1)
//
// **이 층은 프로토콜을 모른다.** 알림 채널·startForeground·wakelock 같은 **수명주기 껍데기**만 제공하고,
// 그 안에서 무엇을 살릴지(엔진·세션)는 서브클래스(앱)가 정한다. Windows 는 프로세스 수명이 곧 앱
// 수명이라 대응물이 없다.
//
// 이 서비스가 필요한 이유는 §6.1 — Android 는 Activity 가 재생성되고 배경 프로세스가 회수된다.
// 등록·세션이 화면 수명에 묶이면 회전 한 번에 끊긴다.
package com.cims.ue.sdk.platform

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat

/**
 * 등록을 유지하는 Foreground Service 의 뼈대. 앱이 서브클래싱해 세션을 소유한다.
 *
 * 서브클래스 책임: [channelName]·[buildNotification] 을 채우고, [onCreate]/[onDestroy] 에서
 * 엔진·세션을 열고 닫는다(super 를 먼저 부른다). 매니페스트에 서비스와 필요한 foregroundServiceType,
 * FOREGROUND_SERVICE 권한을 선언하는 것도 앱 몫이다.
 */
abstract class UeForegroundService : Service() {

    /** 알림 채널 이름(사용자에게 보인다). */
    protected abstract val channelName: String

    /** 상태 알림. 등록·세션이 바뀔 때 [updateNotification] 으로 다시 그린다. */
    protected abstract fun buildNotification(builder: NotificationCompat.Builder): Notification

    /**
     * 세션이 없을 때의 기본 foregroundServiceType — **명시가 필수다**.
     *
     * 0 은 매니페스트 상속이 아니라 `FOREGROUND_SERVICE_TYPE_NONE` 이고, targetSdk 34+ 에서는 유효한
     * 타입으로 전경 승격해야 한다. 등록만 유지하는 동안은 보통 `FOREGROUND_SERVICE_TYPE_DATA_SYNC`,
     * 마이크를 쓰는 동안은 여기에 `FOREGROUND_SERVICE_TYPE_MICROPHONE` 이 더해진다([setMicrophoneActive]).
     * 매니페스트에 같은 타입을 선언하는 것도 앱 몫이다(선언하지 않은 타입으로 승격하면 거부된다).
     */
    protected abstract val baseServiceType: Int

    /**
     * 전경 승격이 실패했을 때 앱에 알린다. 기본은 아무것도 하지 않지만 **삼키지는 않는다** —
     * 실패하면 서비스가 전경이 아니고 곧 회수되므로 앱이 사용자에게 알리거나 재시도해야 한다.
     * targetSdk 34+ 에서는 권한 없는 타입·배경 시작 제한이 예외로 온다.
     */
    protected open fun onForegroundFailed(cause: Throwable) {}

    private var wakeLock: PowerManager.WakeLock? = null
    /** 지금 적용된 타입 — 바뀔 때만 다시 승격한다. */
    private var currentType: Int = Int.MIN_VALUE
    private var micActive = false

    override fun onCreate() {
        super.onCreate()
        createChannel()
        startForegroundNow()
    }

    override fun onDestroy() {
        releaseWakeLock()
        super.onDestroy()
    }

    /** 바인딩하지 않는다 — 상태는 앱의 세션 객체(싱글턴)로 나눈다. */
    override fun onBind(intent: Intent?) = null

    /** 프로세스가 회수됐다 살아나면 다시 붙는다. 상태 복원은 앱이 §6.1 규칙대로 한다. */
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    /** 알림을 다시 그린다(등록 상태·세션 수 변화). 타입은 건드리지 않는다. */
    protected fun updateNotification() {
        runCatching {
            getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION_ID, notification())
        }
    }

    /**
     * 마이크 사용 여부를 알린다 — 세션이 캡처를 열고 닫을 때 앱이 부른다.
     *
     * **타입을 바꾸려면 startForeground 를 다시 불러야 한다** — 알림만 다시 그리면 타입은 그대로다.
     * 등록 유지에서 통화로 넘어갈 때 이것을 빠뜨리면 배경에서 마이크가 막힌다(원천 PttService 의
     * 재승격 경로와 같은 자리).
     */
    fun setMicrophoneActive(active: Boolean) {
        if (micActive == active) return
        micActive = active
        promote()
    }

    /** 지금 필요한 타입 — 기본 + (마이크 쓰는 중이면) 마이크. */
    private fun requiredType(): Int =
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) 0
        else if (micActive && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            baseServiceType or android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        else baseServiceType

    private fun startForegroundNow() { currentType = Int.MIN_VALUE; promote() }

    /** 전경 승격 — 타입이 바뀔 때만. 실패는 [onForegroundFailed] 로 올린다(삼키지 않는다). */
    private fun promote() {
        val type = requiredType()
        if (type == currentType) { updateNotification(); return }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                ServiceCompat.startForeground(this, NOTIFICATION_ID, notification(), type)
            } else {
                startForeground(NOTIFICATION_ID, notification())
            }
            currentType = type
        } catch (t: Throwable) {
            onForegroundFailed(t)
        }
    }

    /** 세션이 살아 있는 동안 CPU 를 재우지 않는다. 세션이 끝나면 반드시 [releaseWakeLock]. */
    protected fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        runCatching {
            val pm = getSystemService(PowerManager::class.java) ?: return
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKELOCK_TAG).apply { acquire() }
        }
    }

    protected fun releaseWakeLock() {
        runCatching { wakeLock?.takeIf { it.isHeld }?.release() }
        wakeLock = null
    }

    private fun notification(): Notification =
        buildNotification(NotificationCompat.Builder(this, CHANNEL_ID)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE))

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        runCatching {
            getSystemService(NotificationManager::class.java)?.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, channelName, NotificationManager.IMPORTANCE_LOW))
        }
    }

    companion object {
        const val CHANNEL_ID = "cimsue-ue"
        const val NOTIFICATION_ID = 1001
        private const val WAKELOCK_TAG = "cimsue:ue"

        /** 앱이 서비스를 띄울 때 쓰는 공통 진입 — 배경에서도 안전하게 전경 서비스로 시작한다. */
        fun start(context: Context, intent: Intent) {
            runCatching {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) context.startForegroundService(intent)
                else context.startService(intent)
            }
        }
    }
}
