package com.cims.ue.core.power

import android.content.Context
import android.os.PowerManager

/**
 * 등록 유지 서비스의 부분 wakelock — 로그인 중에는 CPU 를 재우지 않는다.
 *
 * 왜 필요한가: pjsip 의 NAT keepalive 는 사용자 공간 타이머라 CPU 가 잠들면 발화하지 않는다. 그러면
 * 공유기가 UDP 매핑을 유휴로 회수하고, 다음에 깨어나 보낸 패킷은 새 공인 포트를 받는데 서버는 옛 포트를
 * 계속 쓴다 — 착신이 조용히 사라진다(실측 09-17, registration_binding_set.md §4.1a). 배터리 최적화
 * 예외([BatteryExemption])는 망만 열어주고 CPU 는 깨워주지 않으므로 이 wakelock 이 따로 필요하다.
 *
 * 비용은 배터리다 — 서비스가 사는 동안 기기가 깊은 잠에 들지 못한다. 획득은 Foreground 승격 뒤,
 * 해제는 onDestroy 에서. `sdk/android` 의 UeForegroundService 와 같은 규율.
 */
class PartialWakeLock(private val context: Context, private val tag: String) {

    private var wakeLock: PowerManager.WakeLock? = null

    fun acquire() {
        if (wakeLock?.isHeld == true) return
        runCatching {
            val pm = context.getSystemService(PowerManager::class.java) ?: return
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, tag).apply { acquire() }
        }
    }

    fun release() {
        runCatching { wakeLock?.takeIf { it.isHeld }?.release() }
        wakeLock = null
    }
}
