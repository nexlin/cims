package com.cims.ue.ptt

import android.content.Context
import android.os.Build
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * 하드웨어 PTT 버튼 지원 — 러기드 단말(UNIWA W999 등)의 측면 PTT 키.
 * 하드웨어 버튼 단말에서는 화면 PTT 버튼을 숨기고 키 down/up 을 floor request/release 에 매핑한다.
 *
 * 감지 3중화(순서대로):
 *  1) 과거 하드웨어 PTT 키 수신 이력(영속) — 가장 확실
 *  2) 기종 allowlist — W999 는 GPIO 입력장치(droi_gpio_keys, KEY_F10/F11)가 앱의
 *     InputDevice 열거에 노출되지 않아 능력 기반 감지가 불가능
 *  3) 입력장치 키 능력 스캔 (F10/F11 을 광고하는 장치)
 * ※ F10/F11 중 어느 쪽이 PTT/SOS 인지는 단말별 상이 — 우선 둘 다 PTT 로 취급하고
 *   긴급(SOS) 기능 도입 시 실측으로 분리한다.
 */
object HwPtt {
    /** W999 측면 PTT 키 실측값 — scancode 87(KEY_F11)을 OEM 이 keycode 309 로 매핑. */
    private const val KEYCODE_W999_PTT = 309

    private val PTT_KEYCODES = intArrayOf(KeyEvent.KEYCODE_F10, KeyEvent.KEYCODE_F11, KEYCODE_W999_PTT)
    private val KNOWN_MODELS = setOf("W999")

    private val _present = MutableStateFlow(false)
    /** 하드웨어 PTT 버튼 단말 여부 — true 면 화면 PTT 버튼 숨김. */
    val present: StateFlow<Boolean> = _present

    fun init(context: Context) {
        _present.value = prefs(context).getBoolean(KEY_SEEN, false) ||
            KNOWN_MODELS.any { Build.MODEL.equals(it, ignoreCase = true) } ||
            scanInputDevices()
        Log.i(TAG, "model=${Build.MODEL} hwPtt=${_present.value}")
    }

    fun isPttKey(keyCode: Int): Boolean = PTT_KEYCODES.contains(keyCode)

    /** 하드웨어 PTT 키 첫 수신 → 학습·영속(모든 단말에서 이후 화면 버튼 숨김). */
    fun markSeen(context: Context) {
        if (_present.value) return
        prefs(context).edit().putBoolean(KEY_SEEN, true).apply()
        _present.value = true
    }

    private fun scanInputDevices(): Boolean = runCatching {
        InputDevice.getDeviceIds().any { id ->
            val d = InputDevice.getDevice(id) ?: return@any false
            !d.isVirtual && d.hasKeys(*PTT_KEYCODES).any { it }
        }
    }.getOrDefault(false)

    private fun prefs(context: Context) = context.getSharedPreferences("hw_ptt", Context.MODE_PRIVATE)

    private const val TAG = "HwPtt"
    private const val KEY_SEEN = "hw_ptt_seen"
}
