package com.cims.ue.core.power

import android.content.Context
import android.os.PowerManager
import android.util.Log

/**
 * 통화 중 근접 센서 화면 꺼짐 — 귀에 대면 화면을 끄고 터치를 막는다.
 *
 * 왜 필요한가: 통화·무전 중 단말을 귀에 대면 얼굴·귀가 화면을 눌러 의도치 않은 조작이 생긴다(실측 09-28:
 * 측면 PTT 키로 발언하는 동안 채널 수신 음량 슬라이더가 ×0.2 로 끌려 저장됨). 안드로이드 전화 앱과 같은
 * `PROXIMITY_SCREEN_OFF_WAKE_LOCK` 을 통화 동안 잡으면, 센서가 가려질 때만 시스템이 화면을 끄고 터치를
 * 버린다. 가려지지 않으면 화면은 평소대로다.
 *
 * 해제는 `RELEASE_FLAG_WAIT_FOR_NO_PROXIMITY` — 통화가 끝나도 귀에서 뗄 때까지 화면을 켜지 않는다(귀에 댄
 * 채 화면이 켜져 다시 눌리는 것을 막는다). 센서가 없는 단말이면 아무 것도 하지 않는다.
 */
class ProximityScreenLock(private val context: Context, private val tag: String) {

    private var lock: PowerManager.WakeLock? = null

    val held: Boolean get() = lock?.isHeld == true

    fun acquire() {
        if (held) return
        runCatching {
            val pm = context.getSystemService(PowerManager::class.java) ?: return
            if (!pm.isWakeLockLevelSupported(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK)) {
                Log.i(TAG, "$tag: 근접 센서 화면 꺼짐 미지원 단말")
                return
            }
            lock = pm.newWakeLock(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK, tag).apply {
                setReferenceCounted(false)
                acquire()
            }
            Log.i(TAG, "$tag: acquire")
        }.onFailure { Log.w(TAG, "$tag: acquire 실패 — ${it.message}") }
    }

    fun release() {
        runCatching {
            lock?.takeIf { it.isHeld }?.release(PowerManager.RELEASE_FLAG_WAIT_FOR_NO_PROXIMITY)
            if (lock != null) Log.i(TAG, "$tag: release")
        }
        lock = null
    }

    private companion object { const val TAG = "ProximityLock" }
}
