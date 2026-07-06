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
 *
 * PTT/SOS 키 구분(W999 실측): GPIO 장치(droi_gpio_keys)는 두 측면 버튼(scan 68=KEY_F10,
 * scan 87=KEY_F11)을 Generic.kl 에서 **둘 다 keycode 309("PTT")** 로 매핑한다 — keycode 만으로는
 * 구분 불가, **scanCode 로 분리**한다: PTT 버튼=scan 87(기존 실측), SOS(2번째 버튼)=scan 68.
 */
object HwPtt {
    /** W999 측면 키 공통 keycode — Generic.kl 이 scan 68/87 둘 다 "PTT"(309)로 매핑. */
    private const val KEYCODE_W999_PTT = 309
    /** W999 측면 PTT 버튼 scancode (KEY_F11) — 실측. */
    private const val SCAN_W999_PTT = 87
    /** W999 측면 SOS(2번째) 버튼 scancode (KEY_F10) — GPIO 장치 키 능력 실측(getevent -p). */
    private const val SCAN_W999_SOS = 68

    private val PTT_KEYCODES = intArrayOf(KeyEvent.KEYCODE_F11, KEYCODE_W999_PTT)
    private val KNOWN_MODELS = setOf("W999")

    /** 측면 하드웨어 키 분류. */
    enum class Kind { NONE, PTT, SOS }

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

    /** 키 이벤트 분류 — W999 는 두 측면 키가 동일 keycode(309)라 scanCode 로 PTT/SOS 를 가른다.
     *  scanCode 미보고(0) 단말은 PTT 로 폴백. 일반 단말은 F11=PTT / F10=SOS. */
    fun classify(keyCode: Int, scanCode: Int): Kind = when (keyCode) {
        KEYCODE_W999_PTT -> if (scanCode == SCAN_W999_SOS) Kind.SOS else Kind.PTT
        KeyEvent.KEYCODE_F11 -> Kind.PTT
        KeyEvent.KEYCODE_F10 -> Kind.SOS
        else -> Kind.NONE
    }

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
