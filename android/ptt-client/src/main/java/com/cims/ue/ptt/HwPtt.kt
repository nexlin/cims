package com.cims.ue.ptt

import android.content.Context
import android.os.Build
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * 하드웨어 PTT/SOS 버튼 지원 — 러기드 단말(UNIWA W999 등)의 측면 물리 키.
 * 하드웨어 버튼 단말에서는 화면 PTT 버튼을 숨기고 키 down/up 을 floor request/release 에,
 * SOS(2번째) 키를 긴급 개시에 매핑한다.
 *
 * **버튼 학습(설정)**: 기종마다 측면 키의 keycode 가 달라(예: W999 는 PTT=309, SOS=310) 하드코딩
 * 만으로는 신규 단말을 커버할 수 없다. [startLearn]/[consumeLearn] 으로 사용자가 실제 버튼을 눌러
 * keycode 를 학습·영속([prefs])하면 [classify] 가 그 값을 우선 적용한다. 학습값이 없으면 내장
 * 기본(W999 실측 309/310 + 일반 F11/F10)으로 폴백한다.
 *
 * 존재 감지 3중화(화면 PTT 버튼 숨김 판단):
 *  1) 과거 하드웨어 PTT 키 수신 이력(영속) — 가장 확실
 *  2) 기종 allowlist(W999 — GPIO 입력장치가 InputDevice 열거에 미노출되어 능력감지 불가)
 *  3) 입력장치 키 능력 스캔 (PTT/F11 을 광고하는 장치)
 */
object HwPtt {
    /** W999 측면 PTT(1번째) 키 기본값 — 실측 keycode 309. */
    private const val KEYCODE_W999_PTT = 309
    /** W999 측면 SOS(2번째) 키 기본값 — 실측 keycode 310(scan 231). */
    private const val KEYCODE_W999_SOS = 310

    /** 내장 기본 PTT/SOS keycode(학습값이 없을 때 폴백). */
    private val DEFAULT_PTT = intArrayOf(KeyEvent.KEYCODE_F11, KEYCODE_W999_PTT)
    private val DEFAULT_SOS = intArrayOf(KeyEvent.KEYCODE_F10, KEYCODE_W999_SOS)
    private val KNOWN_MODELS = setOf("W999")

    /** 측면 하드웨어 키 분류. */
    enum class Kind { NONE, PTT, SOS }

    private val _present = MutableStateFlow(false)
    /** 하드웨어 PTT 버튼 단말 여부 — true 면 화면 PTT 버튼 숨김. */
    val present: StateFlow<Boolean> = _present

    // ── 학습된 keycode(영속) ──
    @Volatile private var learnedPtt: Int = -1
    @Volatile private var learnedSos: Int = -1

    /** 현재 학습 대상(설정 UI 진행 중). null 이면 학습 모드 아님. */
    private val _learning = MutableStateFlow<Kind?>(null)
    val learning: StateFlow<Kind?> = _learning

    /** 학습된 매핑(UI 표시용) — 없으면 null. */
    private val _mapping = MutableStateFlow(KeyMapping(-1, -1))
    val mapping: StateFlow<KeyMapping> = _mapping

    data class KeyMapping(val ptt: Int, val sos: Int)

    fun init(context: Context) {
        val p = prefs(context)
        learnedPtt = p.getInt(KEY_PTT, -1)
        learnedSos = p.getInt(KEY_SOS, -1)
        _mapping.value = KeyMapping(learnedPtt, learnedSos)
        _present.value = p.getBoolean(KEY_SEEN, false) ||
            learnedPtt > 0 ||
            KNOWN_MODELS.any { Build.MODEL.equals(it, ignoreCase = true) } ||
            scanInputDevices()
        Log.i(TAG, "model=${Build.MODEL} hwPtt=${_present.value} learnedPtt=$learnedPtt learnedSos=$learnedSos")
    }

    fun isPttKey(keyCode: Int): Boolean = classify(keyCode) == Kind.PTT

    /**
     * 키 이벤트 분류 — 학습값 우선, 없으면 내장 기본. PTT 와 SOS 는 별개 keycode.
     * 학습된 PTT/SOS 가 (일반 단말 폴백과) 충돌해도 학습값을 우선한다.
     */
    fun classify(keyCode: Int): Kind = when (keyCode) {
        learnedPtt -> Kind.PTT
        learnedSos -> Kind.SOS
        in DEFAULT_PTT.asIterable() -> Kind.PTT
        in DEFAULT_SOS.asIterable() -> Kind.SOS
        else -> Kind.NONE
    }

    // ── 버튼 학습 ──

    /** [kind] 버튼 학습 시작 — 다음 물리 키 입력을 그 버튼으로 매핑. */
    fun startLearn(kind: Kind) { if (kind != Kind.NONE) _learning.value = kind }

    /** 학습 취소. */
    fun cancelLearn() { _learning.value = null }

    /**
     * 학습 모드에서 물리 키 입력 소비. 학습 중이면 keyCode 를 대상 버튼으로 저장하고 true 반환
     * (이벤트 소비). 학습 중이 아니면 false(정상 dispatch 진행).
     * 뒤로가기/홈/볼륨 등 시스템 키는 학습 대상에서 제외해 UI 조작을 막지 않는다.
     */
    fun consumeLearn(context: Context, keyCode: Int): Boolean {
        val target = _learning.value ?: return false
        if (keyCode in SYSTEM_KEYS) return false
        val p = prefs(context)
        when (target) {
            Kind.PTT -> { learnedPtt = keyCode; p.edit().putInt(KEY_PTT, keyCode).apply() }
            Kind.SOS -> { learnedSos = keyCode; p.edit().putInt(KEY_SOS, keyCode).apply() }
            Kind.NONE -> return false
        }
        _mapping.value = KeyMapping(learnedPtt, learnedSos)
        _learning.value = null
        markSeen(context)
        Log.i(TAG, "learned $target = keycode $keyCode")
        return true
    }

    /** 학습된 매핑 초기화(내장 기본으로 복귀). */
    fun resetMapping(context: Context) {
        learnedPtt = -1; learnedSos = -1
        prefs(context).edit().remove(KEY_PTT).remove(KEY_SOS).apply()
        _mapping.value = KeyMapping(-1, -1)
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
            !d.isVirtual && d.hasKeys(*DEFAULT_PTT).any { it }
        }
    }.getOrDefault(false)

    private fun prefs(context: Context) = context.getSharedPreferences("hw_ptt", Context.MODE_PRIVATE)

    // ── 백그라운드 키 경로 중재 ──

    @Volatile private var lastKeyEventMs = 0L

    /** 실제 KeyEvent 경로(Activity/접근성)가 PTT/SOS 키를 처리했음을 기록 — 벤더 브로드캐스트 억제용. */
    fun noteKeyEvent() { lastKeyEventMs = android.os.SystemClock.elapsedRealtime() }

    /** 최근(1.5s 내) 실제 키 이벤트 처리 여부 — 같은 눌림의 벤더 브로드캐스트 중복 트리거 방지.
     *  누르고 있는 동안은 키 반복(repeat) DOWN 이 계속 갱신하므로 긴 눌림에도 유효하다. */
    fun recentKeyEvent(): Boolean =
        android.os.SystemClock.elapsedRealtime() - lastKeyEventMs < 1500

    /** 학습에서 제외할 시스템 키(뒤로/홈/최근/볼륨/전원) — UI 조작·안전 키.
     *  학습 중에도 이 키들은 소비하지 않아 뒤로가기 등으로 학습을 빠져나올 수 있다. */
    fun isSystemNav(keyCode: Int): Boolean = keyCode in SYSTEM_KEYS

    private val SYSTEM_KEYS = setOf(
        KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_HOME, KeyEvent.KEYCODE_APP_SWITCH,
        KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN, KeyEvent.KEYCODE_VOLUME_MUTE,
        KeyEvent.KEYCODE_POWER,
    )

    private const val TAG = "HwPtt"
    private const val KEY_SEEN = "hw_ptt_seen"
    private const val KEY_PTT = "hw_ptt_keycode"
    private const val KEY_SOS = "hw_sos_keycode"
}
