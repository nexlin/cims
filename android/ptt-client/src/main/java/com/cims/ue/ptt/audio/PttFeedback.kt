package com.cims.ue.ptt.audio

import android.content.Context
import android.media.AudioManager
import android.media.ToneGenerator
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager

/**
 * PTT 사용자 피드백(톤+진동) — TS 24.380 floor 통지(GRANT/DENY/REVOKE)의 청각·촉각 표현.
 *
 * 무전 UX 관례를 따른다: 승인=이중 삑("삑 후 말하기"), 거부/회수=승인과 뚜렷이 구별되는 저음.
 * 마이크는 승인 톤이 스피커로 나간 **뒤**에 열어야 톤이 그룹으로 송출되지 않는다 —
 * [grantTone] 이 반환하는 ms 만큼 기다렸다가 mic 을 개방한다(컨트롤러 몫).
 */
class PttFeedback(context: Context) {

    private val tone = runCatching { ToneGenerator(AudioManager.STREAM_VOICE_CALL, TONE_VOLUME) }.getOrNull()

    private val vibrator: Vibrator? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)?.defaultVibrator
        else @Suppress("DEPRECATION") context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator

    /** 발언권 승인 — 이중 삑 + 짧은 진동. 반환값 = mic 개방까지 기다릴 ms. */
    fun grantTone(): Long {
        play(ToneGenerator.TONE_PROP_BEEP2)
        vibrate(longArrayOf(0, 40))
        return GRANT_TONE_MS
    }

    /** 발언권 거부 — 저음 NACK + 이중 진동(승인과 촉각으로도 구별). */
    fun denyTone() {
        play(ToneGenerator.TONE_PROP_NACK)
        vibrate(longArrayOf(0, 60, 80, 60))
    }

    /** 발언권 강제 회수(선점) — 에러 톤 + 긴 이중 진동. 즉시 송신 중단과 함께 재생. */
    fun revokeTone() {
        play(ToneGenerator.TONE_SUP_ERROR, 400)
        vibrate(longArrayOf(0, 80, 80, 80))
    }

    /** 발언 종료(내 RELEASE) 로컬 확인 — 짧은 단일 삑. */
    fun releaseTone() = play(ToneGenerator.TONE_PROP_BEEP)

    /** 긴급(SOS) 개시/수신 — 긴 경고 톤 + 강한 삼중 진동(다른 톤과 뚜렷이 구별). */
    fun emergencyTone() {
        play(ToneGenerator.TONE_CDMA_EMERGENCY_RINGBACK, 800)
        vibrate(longArrayOf(0, 150, 100, 150, 100, 300))
    }

    private fun play(toneType: Int, durationMs: Int = -1) {
        val t = tone ?: run { android.util.Log.w("PttFeedback", "ToneGenerator unavailable"); return }
        runCatching {
            t.stopTone()
            val ok = if (durationMs > 0) t.startTone(toneType, durationMs) else t.startTone(toneType)
            android.util.Log.i("PttFeedback", "tone $toneType started=$ok")
        }.onFailure { android.util.Log.w("PttFeedback", "tone failed: ${it.message}") }
    }

    private fun vibrate(pattern: LongArray) {
        val v = vibrator ?: return
        runCatching { v.vibrate(VibrationEffect.createWaveform(pattern, -1)) }
    }

    fun close() {
        runCatching { tone?.release() }
    }

    private companion object {
        const val TONE_VOLUME = 90
        /** TONE_PROP_BEEP2(이중 삑) 실재생 길이 — 이 이후 mic 개방. */
        const val GRANT_TONE_MS = 350L
    }
}
