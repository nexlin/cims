package com.cims.ue.ptt

import android.accessibilityservice.AccessibilityService
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.provider.Settings
import android.util.Log
import android.view.KeyEvent
import android.view.accessibility.AccessibilityEvent

/**
 * 전역 하드웨어 PTT/SOS 키 수신 — 접근성 키 필터(FLAG_REQUEST_FILTER_KEY_EVENTS).
 *
 * MainActivity.dispatchKeyEvent 는 앱이 전면일 때만 키를 받는다. 무전기 사용 패턴(다른 앱 사용 중,
 * 홈 화면)에서도 측면 PTT 버튼이 동작해야 하므로, 접근성 서비스의 [onKeyEvent] 로 시스템 전역
 * 키를 받아 같은 컨트롤러 경로(pttDown/pttUp/startEmergency)에 넣는다 — 선탑재 PoC 앱(Corget 등)과
 * 동일한 방식이며 설정→접근성에서 1회 활성화가 필요하다(설정 탭에 바로가기 행).
 *
 * PTT/SOS 키는 여기서 소비(true)하므로 전면일 때도 Activity 로는 가지 않는다(이중 처리 없음).
 * 서비스 미활성 시엔 기존 Activity 경로가 폴백. 마이크 점유는 컨트롤러가 처리한다:
 * down=setTalkCapture(true)(volte 양보+전이중), up=setTalkCapture(false)(스피커 전용 복귀) —
 * "누르는 동안만 점유"가 백그라운드에서도 동일하게 성립.
 */
class PttKeyService : AccessibilityService() {

    override fun onServiceConnected() {
        super.onServiceConnected()
        connected = true
        HwPtt.init(this)    // Activity 이전(부팅 직후) 연결 대비 — 학습 매핑 적재. 재호출 무해.
        Log.i(TAG, "connected — 전역 PTT 키 필터 활성")
    }

    override fun onUnbind(intent: Intent?): Boolean {
        connected = false
        Log.i(TAG, "disconnected")
        return super.onUnbind(intent)
    }

    override fun onDestroy() {
        connected = false
        super.onDestroy()
    }

    override fun onKeyEvent(event: KeyEvent): Boolean {
        // 버튼 학습(설정 UI) — Activity 경로와 동일 규칙: DOWN 에서 캡처, UP 은 소비만
        if (HwPtt.learning.value != null && !HwPtt.isSystemNav(event.keyCode)) {
            if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0)
                HwPtt.consumeLearn(this, event.keyCode)
            return true
        }
        val kind = HwPtt.classify(event.keyCode)
        if (kind == HwPtt.Kind.NONE) return false
        HwPtt.noteKeyEvent()    // 같은 눌림의 벤더 브로드캐스트(VendorPttReceiver) 억제
        val ctl = PttService.instance?.controller
        if (ctl == null) {
            // 서비스가 죽어 있으면(스와이프 종료 등) 이 눌림으로 되살린다 — 이번 눌림은 유실
            if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                Log.w(TAG, "controller 없음 — PttService 재기동")
                runCatching { PttService.start(this) }
            }
            return true
        }
        when (kind) {
            HwPtt.Kind.PTT -> {
                HwPtt.markSeen(this)
                when (event.action) {
                    KeyEvent.ACTION_DOWN -> if (event.repeatCount == 0) ctl.pttDown()
                    KeyEvent.ACTION_UP -> ctl.pttUp()
                }
            }
            HwPtt.Kind.SOS ->
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) ctl.startEmergency()
            HwPtt.Kind.NONE -> Unit
        }
        return true
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) = Unit
    override fun onInterrupt() = Unit

    companion object {
        /** 접근성 키 필터 연결 여부 — 연결 중엔 벤더 브로드캐스트 경로를 무시(이중 트리거 방지). */
        @Volatile var connected = false
            private set

        /** 설정→접근성에서 이 서비스가 활성화돼 있는지 (설정 탭 표시용). */
        fun isEnabled(context: Context): Boolean {
            val me = ComponentName(context, PttKeyService::class.java)
            val enabled = Settings.Secure.getString(
                context.contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES,
            ) ?: return false
            return enabled.split(':').any { ComponentName.unflattenFromString(it) == me }
        }

        private const val TAG = "PttKeyService"
    }
}
