// Android 접점 — 하드웨어 PTT 키 (docs/design/features/android_dispatch_tablet.md §5·§7, dispatch_desktop_ui.md §8)
//
// **이 층은 프로토콜을 모른다.** 키를 눌렀다/뗐다만 알리고, 그것으로 무엇을 할지(발언 대상 전부에
// floor 요청 / 긴급 개시)는 앱이 정한다. Windows 의 전역 핫키와 입력원은 다르지만 **누름·해제의 동작
// 의미는 같다**.
//
// 원천은 android/ptt-client/HwPtt.kt·VendorPttReceiver.kt. 싱글턴(object)이던 것을 클래스로 바꿔
// 시험에서 상태를 격리할 수 있게 했다.
package com.cims.ue.sdk.platform

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import android.view.InputDevice
import android.view.KeyEvent
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** 측면 하드 키의 종류. */
enum class PttKey { NONE, TALK, ALERT }

/** 러기드 단말 측면 키 실측 keycode(학습값이 없을 때 폴백). */
private const val KEYCODE_RUGGED_TALK = 309
private const val KEYCODE_RUGGED_ALERT = 310
internal val DEFAULT_TALK = intArrayOf(KeyEvent.KEYCODE_F11, KEYCODE_RUGGED_TALK)
internal val DEFAULT_ALERT = intArrayOf(KeyEvent.KEYCODE_F10, KEYCODE_RUGGED_ALERT)

/**
 * 러기드 단말(UNIWA W999 등)의 측면 물리 키 입력.
 *
 * **버튼 학습**: 기종마다 keycode 가 달라 하드코딩만으로는 신규 단말을 못 덮는다. [startLearn]/
 * [onKeyDown] 으로 사용자가 실제 버튼을 눌러 keycode 를 학습·영속하면 [classify] 가 그 값을 우선 쓴다.
 *
 * **존재 감지 3중화**(화면 PTT 버튼을 숨길지 판단):
 *   ① 과거 하드 키 수신 이력(영속) — 가장 확실
 *   ② 기종 allowlist — GPIO 입력장치가 InputDevice 열거에 안 나오는 기기용
 *   ③ 입력장치 키 능력 스캔
 */
class HwPtt(context: Context) {

    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private val _present = MutableStateFlow(false)
    /** 하드 키 단말 여부 — true 면 앱이 화면 PTT 버튼을 숨긴다. */
    val present: StateFlow<Boolean> = _present.asStateFlow()

    private val _mapping = MutableStateFlow(KeyMapping.UNSET)
    /** 학습된 매핑(설정 화면 표시용). -1 = 미학습(내장 기본으로 폴백). */
    val mapping: StateFlow<KeyMapping> = _mapping.asStateFlow()

    private val _learning = MutableStateFlow<PttKey?>(null)
    /** 학습 진행 중인 대상. null 이면 학습 모드가 아니다. */
    val learning: StateFlow<PttKey?> = _learning.asStateFlow()

    private val _pressed = MutableStateFlow(false)
    /** TALK 키를 누르고 있는 중인지. 앱이 발언 상태와 맞춘다. */
    val pressed: StateFlow<Boolean> = _pressed.asStateFlow()

    /**
     * 학습된 keycode 매핑. -1 = 미학습.
     *
     * 판정은 여기 있다 — Android 를 타지 않는 순수 로직이라 기기 없이 시험할 수 있고,
     * 이 우선순위가 어긋나면 하드 키가 조용히 안 먹는다.
     */
    data class KeyMapping(val talk: Int, val alert: Int) {
        /** 학습값이 내장 기본을 이긴다. 한 종류가 학습돼 있으면 그 종류의 기본값은 더 쓰지 않는다. */
        fun classify(keyCode: Int): PttKey = when {
            talk > 0 && keyCode == talk -> PttKey.TALK
            alert > 0 && keyCode == alert -> PttKey.ALERT
            talk <= 0 && keyCode in DEFAULT_TALK -> PttKey.TALK
            alert <= 0 && keyCode in DEFAULT_ALERT -> PttKey.ALERT
            else -> PttKey.NONE
        }

        /** 이 종류의 keycode 를 학습값으로 바꾼 사본. */
        fun learn(target: PttKey, keyCode: Int): KeyMapping = when (target) {
            PttKey.TALK -> copy(talk = keyCode)
            PttKey.ALERT -> copy(alert = keyCode)
            PttKey.NONE -> this
        }

        companion object {
            /** 미학습 상태. */
            val UNSET = KeyMapping(-1, -1)
        }
    }

    init {
        _mapping.value = KeyMapping(prefs.getInt(KEY_TALK, -1), prefs.getInt(KEY_ALERT, -1))
        _present.value = prefs.getBoolean(KEY_SEEN, false) ||
            _mapping.value.talk > 0 ||
            KNOWN_MODELS.any { Build.MODEL.equals(it, ignoreCase = true) } ||
            scanInputDevices()
    }

    /** keycode 분류 — 판정은 [KeyMapping.classify] 에 있다. */
    fun classify(keyCode: Int): PttKey = _mapping.value.classify(keyCode)

    /** 설정 화면에서 학습 시작. 다음 [onKeyDown] 의 keycode 를 이 대상으로 저장한다. */
    fun startLearn(target: PttKey) { if (target != PttKey.NONE) _learning.value = target }
    fun cancelLearn() { _learning.value = null }

    /**
     * Activity/Service 의 키 down 을 넘긴다. 소비했으면 true.
     * 학습 중이면 매핑을 저장하고, 아니면 [pressed] 를 올린다.
     */
    fun onKeyDown(keyCode: Int): Boolean {
        _learning.value?.let { target ->
            val next = _mapping.value.learn(target, keyCode)
            _mapping.value = next
            prefs.edit().putInt(KEY_TALK, next.talk).putInt(KEY_ALERT, next.alert)
                .putBoolean(KEY_SEEN, true).apply()
            _present.value = true
            _learning.value = null
            return true
        }
        val kind = classify(keyCode)
        if (kind == PttKey.NONE) return false
        markSeen()
        if (kind == PttKey.TALK) _pressed.value = true
        return true
    }

    /** 키 up. TALK 였으면 [pressed] 를 내린다. 소비했으면 true. */
    fun onKeyUp(keyCode: Int): Boolean {
        val kind = classify(keyCode)
        if (kind == PttKey.NONE) return false
        markSeen()
        if (kind == PttKey.TALK) _pressed.value = false
        return true
    }

    /** 화면이 꺼져 있거나 앱이 배경일 때 벤더 브로드캐스트로 오는 입력. */
    fun onVendorEvent(kind: PttKey, down: Boolean): Boolean {
        if (kind == PttKey.NONE) return false
        markSeen()
        if (kind == PttKey.TALK) _pressed.value = down
        return true
    }

    /** 앱이 세션을 정리할 때 눌림 상태를 되돌린다(키 up 을 못 받고 끝난 경우). */
    fun releaseStuck() { _pressed.value = false }

    private fun markSeen() {
        if (!prefs.getBoolean(KEY_SEEN, false)) prefs.edit().putBoolean(KEY_SEEN, true).apply()
        _present.value = true
    }

    private fun scanInputDevices(): Boolean = runCatching {
        InputDevice.getDeviceIds().any { id ->
            val d = InputDevice.getDevice(id) ?: return@any false
            d.hasKeys(*DEFAULT_TALK).any { it }
        }
    }.getOrDefault(false)

    companion object {
        private const val PREFS = "cimsue-hwptt"
        private const val KEY_TALK = "talk"
        private const val KEY_ALERT = "alert"
        private const val KEY_SEEN = "seen"

        /** GPIO 입력장치가 InputDevice 열거에 안 나와 능력 감지가 안 되는 기종. */
        private val KNOWN_MODELS = setOf("W999")
    }
}
