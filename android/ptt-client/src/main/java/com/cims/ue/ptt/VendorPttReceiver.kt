package com.cims.ue.ptt

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log

/**
 * 벤더 PTT 키 브로드캐스트 폴백 — 러기드 단말 프레임워크가 측면 키를 전역 방송하는 관례
 * (`android.intent.action.PTT.down`/`up` — W999(droi) 선탑재 Corget 이 수신하는 액션,
 * Motorola Solutions 계열 포함). 정본 경로는 접근성 키 필터([PttKeyService])이고,
 * 이 리시버는 접근성 미활성 단말의 보조 경로다:
 *  - 접근성 연결 중이거나 방금 실제 키 이벤트를 처리했으면 무시(같은 눌림 이중 트리거 방지)
 *  - down 만 방송하고 up 이 없는 벤더가 있어(Corget 정적 필터에도 down 만 존재) 워치독으로
 *    발언을 강제 종료한다 — up 유실 시 마이크가 눌린 채 고착되는 것을 막는다
 * 매니페스트 정적 등록(프로세스 사망 시 down 으로 서비스 재기동) + PttService 동적 등록
 * (실행 중 확실한 수신) 이중화 — 같은 방송이 두 번 와도 디바운스로 1회만 처리.
 */
class VendorPttReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        val down = action in DOWN_ACTIONS
        if (!down && action !in UP_ACTIONS) return
        // 정적+동적 이중 등록 디바운스 — 같은 방송의 두 번째 배달은 무시
        val now = SystemClock.elapsedRealtime()
        if (action == lastAction && now - lastActionMs < 300) return
        lastAction = action; lastActionMs = now
        Log.i(TAG, "vendor key broadcast: $action")
        if (PttKeyService.connected || HwPtt.recentKeyEvent()) return
        val ctl = PttService.instance?.controller
        if (ctl == null) {
            if (down) runCatching { PttService.start(context) }
            return
        }
        if (down) {
            HwPtt.markSeen(context)
            ctl.pttDown()
            // up 미방송 벤더 대비 워치독 — 서버 최대 발언시간 언저리에서 강제 해제
            release?.let(handler::removeCallbacks)
            release = Runnable { PttService.instance?.controller?.pttUp() }
                .also { handler.postDelayed(it, RELEASE_WATCHDOG_MS) }
        } else {
            release?.let(handler::removeCallbacks); release = null
            ctl.pttUp()
        }
    }

    companion object {
        private val DOWN_ACTIONS = setOf(
            "android.intent.action.PTT.down",
            "com.android.action.ptt.down",
            "com.motorolasolutions.intent.action.ACTION_PTT_BUTTON_DOWN",
        )
        private val UP_ACTIONS = setOf(
            "android.intent.action.PTT.up",
            "com.android.action.ptt.up",
            "com.motorolasolutions.intent.action.ACTION_PTT_BUTTON_UP",
        )

        /** PttService 동적 등록용 필터(실행 중 수신 보장 — 암시적 방송의 정적 배달 차단 대비). */
        fun filter() = IntentFilter().apply {
            (DOWN_ACTIONS + UP_ACTIONS).forEach(::addAction)
            priority = IntentFilter.SYSTEM_HIGH_PRIORITY - 1
        }

        private val handler = Handler(Looper.getMainLooper())
        private var release: Runnable? = null
        @Volatile private var lastAction: String? = null
        @Volatile private var lastActionMs = 0L
        private const val RELEASE_WATCHDOG_MS = 35_000L
        private const val TAG = "VendorPttReceiver"
    }
}
