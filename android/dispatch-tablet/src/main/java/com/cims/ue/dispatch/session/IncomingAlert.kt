// 착신 알림 — 화면 밖에서도 울린다 (docs/design/features/android_dispatch_tablet.md §6.2)
//
// 배너(`ui/IncomingBanner`)는 앱 화면이 보일 때만 소용이 있다. 관제 태블릿은 화면이 꺼져 있거나 다른 앱을
// 쓰고 있을 수 있으므로, **서비스가** 착신을 알림과 소리로 낸다 — 전경 알림(등록 유지)과는 채널도 id 도
// 다르다. 등록 알림은 조용해야 하고 착신 알림은 울려야 하기 때문이다.
//
// 벨소리는 채널 소리가 아니라 `Ringtone` 반복 재생이다 — 채널 소리는 한 번만 나고 끝나서 전화에 맞지 않는다.
package com.cims.ue.dispatch.session

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import com.cims.ue.dispatch.ui.MainActivity

class IncomingAlert(private val context: Context) {

    private val nm = NotificationManagerCompat.from(context)
    private var ringtone: Ringtone? = null
    private var loop: Handler? = null
    private var shownCallId: Int? = null

    init { createChannel() }

    /**
     * 착신 상태를 알림·소리에 반영한다.
     *
     * 같은 호가 계속 울리는 동안 알림을 다시 만들지 않는다 — 다시 만들면 소리가 처음부터 나고
     * 전체 화면 인텐트가 되풀이된다. 착신이 없어지면 전부 내린다.
     */
    fun apply(top: SessionItem?, label: String) {
        if (top == null) return clear()
        if (shownCallId == top.callId) return
        shownCallId = top.callId
        notify(top.callId, label)
        startRinging()
    }

    fun clear() {
        if (shownCallId == null) return
        shownCallId = null
        stopRinging()
        runCatching { nm.cancel(NOTIFICATION_ID) }
    }

    private fun notify(callId: Int, label: String) {
        val full = PendingIntent.getActivity(
            context, 0,
            // 클래스를 직접 가리킨다 — 이름 문자열로 찾으면 난독화·리팩터에 조용히 깨진다.
            Intent(context, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)

        val n = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.sym_call_incoming)
            .setContentTitle("착신")
            .setContentText(label)
            .setCategory(NotificationCompat.CATEGORY_CALL)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setOngoing(true)
            .setAutoCancel(false)
            .setContentIntent(full)
            // 화면이 꺼져 있으면 잠금 화면 위로 띄운다 — 관제석은 놓치면 안 된다.
            .setFullScreenIntent(full, true)
            .addAction(0, "응답", action(DispatchService.ACTION_ANSWER, callId))
            .addAction(0, "거절", action(DispatchService.ACTION_REJECT, callId))
            .build()
        // 알림 권한이 없으면 notify 가 실패한다. **조용히 넘기지 않는다** — 관제석에서 착신을
        // 놓치는데 아무 표시가 없으면 원인을 찾을 수 없다. 배너는 화면이 보일 때만 뜬다.
        val posted = runCatching { nm.notify(NOTIFICATION_ID, n); true }.getOrDefault(false)
        if (!posted || !nm.areNotificationsEnabled())
            android.util.Log.w(TAG, "착신 알림을 띄우지 못했다 — 알림 권한을 확인하라 (화면이 꺼져 있으면 착신을 놓친다)")
    }

    private fun action(what: String, callId: Int): PendingIntent =
        PendingIntent.getService(
            context, what.hashCode(),
            Intent(context, DispatchService::class.java).setAction(what).putExtra(EXTRA_CALL_ID, callId),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)

    /**
     * 벨소리 시작.
     *
     * `Ringtone.isLooping` 은 **API 28+** 다. minSdk 는 26 이라 그 아래에서는 한 번 울리고 끝나
     * 관제사가 착신을 놓친다 — 구형에서는 주기적으로 다시 틀어 반복을 만든다.
     */
    private fun startRinging() {
        stopRinging()
        runCatching {
            val uri = RingtoneManager.getActualDefaultRingtoneUri(context, RingtoneManager.TYPE_RINGTONE)
                ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE) ?: return@runCatching
            ringtone = RingtoneManager.getRingtone(context, uri)?.apply {
                audioAttributes = AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build()
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) isLooping = true
                play()
            }
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) startLoopFallback()
        }
        runCatching {
            vibrator()?.vibrate(VibrationEffect.createWaveform(VIBRATE_PATTERN, 0))
        }
    }

    /** API 28 미만의 반복 — 멎으면 다시 튼다. 착신이 끝나면 [stopRinging] 이 루프를 끊는다. */
    private fun startLoopFallback() {
        loop?.removeCallbacksAndMessages(null)
        val h = Handler(Looper.getMainLooper())
        loop = h
        val beat = object : Runnable {
            override fun run() {
                val r = ringtone ?: return
                if (!r.isPlaying) runCatching { r.play() }
                h.postDelayed(this, LOOP_BEAT_MS)
            }
        }
        h.postDelayed(beat, LOOP_BEAT_MS)
    }

    private fun stopRinging() {
        loop?.removeCallbacksAndMessages(null)
        loop = null
        runCatching { ringtone?.stop() }
        ringtone = null
        runCatching { vibrator()?.cancel() }
    }

    private fun vibrator(): Vibrator? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            context.getSystemService(VibratorManager::class.java)?.defaultVibrator
        else @Suppress("DEPRECATION") context.getSystemService(Vibrator::class.java)

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val ch = NotificationChannel(CHANNEL_ID, "착신", NotificationManager.IMPORTANCE_HIGH).apply {
            description = "걸려 온 전화 — 화면이 꺼져 있어도 알린다"
            // 소리·진동은 Ringtone 이 맡는다(반복 재생). 채널이 같이 울리면 두 번 난다.
            setSound(null, null)
            enableVibration(false)
            lockscreenVisibility = android.app.Notification.VISIBILITY_PUBLIC
        }
        context.getSystemService(NotificationManager::class.java)?.createNotificationChannel(ch)
    }

    companion object {
        private const val TAG = "IncomingAlert"
        const val CHANNEL_ID = "cimsue-dispatch-incoming"
        const val NOTIFICATION_ID = 1002
        const val EXTRA_CALL_ID = "callId"
        private val VIBRATE_PATTERN = longArrayOf(0, 700, 900)
        private const val LOOP_BEAT_MS = 1_000L
    }
}
