// Android 접점 — 오디오 라우팅 (docs/design/features/android_dispatch_tablet.md §5, ue_sdk.md §4.5·§5.1)
//
// **이 층은 프로토콜을 모른다.** AudioManager 모드·포커스·블루투스 SCO·통신 장치 선택만 다루고,
// 코어는 pjmedia 장치 id 와 재생 라우트만 다룬다. 둘을 잇는 것은 앱이다 — 예: 무전 수신을 스피커로,
// 통화를 헤드셋으로 보내려면 앱이 여기서 장치를 고르고 코어의 addPlaybackRoute/setCallRoute 를 부른다.
//
// 원천은 android/ptt-client/audio/AudioRouter.kt 이며, 거기 있던 코어 상수 참조
// (SipController.AUDIO_ROUTE_SPEAKER)는 층 경계를 넘으므로 이 층의 [Route] 로 대체했다.
package com.cims.ue.sdk.platform

import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** 라우트 의미 — 코어(ue_sdk.md §4.5)와 같은 어휘. 장치 선택의 결과가 아니라 **의도**다. */
enum class Route { EARPIECE, SPEAKER, HEADSET, BLUETOOTH }

/** 연결된 이어폰 하나. [id] 는 AudioDeviceInfo.id 로 재부팅 간 보존되지 않는다. */
data class Headset(val id: Int, val name: String, val wireless: Boolean)

/**
 * 통화/무전 오디오 경로 제어. 앱(또는 Foreground Service)이 하나만 만들어 쓰고 [close] 로 놓는다.
 *
 * 관제석의 **무전/통화 분리 출력**은 두 층이 나눠 맡는다 — 어느 물리 장치로 낼지는 여기,
 * 어느 호를 어느 라우트로 보낼지는 코어(`addPlaybackRoute`/`setCallRoute`)다.
 */
class AudioRouter(context: Context) {

    private val am: AudioManager? = context.applicationContext.getSystemService(AudioManager::class.java)

    private val _headsets = MutableStateFlow<List<Headset>>(emptyList())
    /** 연결 중인 이어폰 목록 — 연결·해제 시 갱신(무선 다중 연결이면 여러 개). */
    val headsets: StateFlow<List<Headset>> = _headsets.asStateFlow()

    private val _route = MutableStateFlow(Route.SPEAKER)
    /** 마지막으로 요청한 라우트. 실제 적용 여부는 기기가 정한다. */
    val route: StateFlow<Route> = _route.asStateFlow()

    /** 통신 모드를 이 객체가 쥐고 있는지 — 중복 setMode 방지. */
    private var inCall = false

    /** 다른 통화 스택에 경로를 양보 중인지. */
    @Volatile
    private var yielded = false
    val isYielded: Boolean get() = yielded

    private val deviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) = refresh()
        override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) = refresh()
    }

    init {
        am?.registerAudioDeviceCallback(deviceCallback, null)
        refresh()
    }

    fun close() {
        runCatching { am?.unregisterAudioDeviceCallback(deviceCallback) }
        clearCommunicationDevice()
        setInCall(false)
    }

    // ── 모드 ─────────────────────────────────────────────────────────────────
    /**
     * 통신 모드 진입/이탈(MODE_IN_COMMUNICATION). 세션이 하나라도 살아 있는 동안 켜 둔다.
     * 양보 중이면 요청을 넣지 않는다.
     */
    fun setInCall(on: Boolean) {
        val a = am ?: return
        if (inCall == on) return
        inCall = on
        if (yielded) return
        runCatching {
            a.mode = if (on) AudioManager.MODE_IN_COMMUNICATION else AudioManager.MODE_NORMAL
        }
    }

    /**
     * 경로 양보 — 다른 통화 스택(예: 단말 기본 전화)이 경로를 잡아야 할 때 이 객체의 통신 장치 요청과
     * 모드 소유를 함께 반납한다. 실측(러기드 단말 다수): 통화 라우팅 요청은 **모드 스택의 선점 소유자**
     * 기준으로 매칭돼, 우리가 모드를 쥔 채면 나중에 잡은 쪽의 요청이 무시된다.
     * @return 상태가 실제로 바뀌었으면 true.
     */
    fun yieldRoute(on: Boolean): Boolean {
        if (yielded == on) return false
        yielded = on
        val a = am
        if (on) {
            clearCommunicationDevice()
            if (inCall) runCatching { a?.mode = AudioManager.MODE_NORMAL }
        } else if (inCall) {
            runCatching { a?.mode = AudioManager.MODE_IN_COMMUNICATION }
            applyRoute(_route.value)
        }
        return true
    }

    // ── 경로 ─────────────────────────────────────────────────────────────────
    /** 라우트 요청. 양보 중이면 의도만 기억하고 적용은 복귀 시로 미룬다. */
    fun setRoute(r: Route): Boolean {
        _route.value = r
        if (yielded) return false
        return applyRoute(r)
    }

    private fun applyRoute(r: Route): Boolean {
        val a = am ?: return false
        return when (r) {
            Route.SPEAKER -> selectType(AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) || legacySpeakerphone(a, true)
            Route.EARPIECE -> selectType(AudioDeviceInfo.TYPE_BUILTIN_EARPIECE) || legacySpeakerphone(a, false)
            Route.HEADSET -> selectHeadset(wireless = false)
            Route.BLUETOOTH -> selectHeadset(wireless = true)
        }
    }

    /** 이어폰을 id 로 직접 고른다(무선 다중 연결에서 사용자가 하나를 선택). */
    fun selectHeadset(id: Int): Boolean {
        val dev = candidates().firstOrNull { it.id == id } ?: return false
        _route.value = if (dev.wireless) Route.BLUETOOTH else Route.HEADSET
        if (yielded) return false
        return selectDeviceId(id)
    }

    private fun selectHeadset(wireless: Boolean): Boolean {
        val dev = candidates().firstOrNull { it.wireless == wireless } ?: return false
        return selectDeviceId(dev.id)
    }

    private fun selectDeviceId(id: Int): Boolean {
        val a = am ?: return false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val d = a.availableCommunicationDevices.firstOrNull { it.id == id } ?: return false
            return runCatching { a.setCommunicationDevice(d) }.getOrDefault(false)
        }
        // API 31 미만: setCommunicationDevice 가 없다. 유선은 기기가 자동 라우팅하지만 **블루투스는
        // 일반 전화 밖에서 SCO 를 직접 열어야** 소리가 간다 — 자동 라우팅에 맡기면 이어폰이 목록에
        // 보여도 음성이 연결되지 않는다(AudioManager.startBluetoothSco 계약).
        val dev = candidates().firstOrNull { it.id == id } ?: return false
        return if (dev.wireless) startSco(a) else stopSco(a).let { true }
    }

    private fun selectType(type: Int): Boolean {
        val a = am ?: return false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val d = a.availableCommunicationDevices.firstOrNull { it.type == type } ?: return false
            return runCatching { a.setCommunicationDevice(d) }.getOrDefault(false)
        }
        // 스피커/수화기로 갈 때는 SCO 를 먼저 닫아야 경로가 넘어온다.
        stopSco(a)
        return false   // 나머지는 legacySpeakerphone 이 처리한다
    }

    @Suppress("DEPRECATION")
    private fun startSco(a: AudioManager): Boolean = runCatching {
        if (!a.isBluetoothScoOn) { a.startBluetoothSco(); a.isBluetoothScoOn = true }
        true
    }.getOrDefault(false)

    @Suppress("DEPRECATION")
    private fun stopSco(a: AudioManager) {
        runCatching { if (a.isBluetoothScoOn) { a.isBluetoothScoOn = false; a.stopBluetoothSco() } }
    }

    @Suppress("DEPRECATION")
    private fun legacySpeakerphone(a: AudioManager, on: Boolean): Boolean =
        runCatching { a.isSpeakerphoneOn = on; true }.getOrDefault(false)

    /** 이 객체의 통신 장치 요청을 놓는다(기기 기본 라우팅으로 복귀). SCO 도 함께 닫는다. */
    fun clearCommunicationDevice() {
        val a = am ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            runCatching { a.clearCommunicationDevice() }
        } else {
            stopSco(a)
            @Suppress("DEPRECATION") runCatching { a.isSpeakerphoneOn = false }
        }
    }

    // ── 열거 ─────────────────────────────────────────────────────────────────
    private fun refresh() { _headsets.value = candidates() }

    private fun candidates(): List<Headset> {
        val a = am ?: return emptyList()
        val devs = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) a.availableCommunicationDevices
                   else a.getDevices(AudioManager.GET_DEVICES_OUTPUTS).toList()
        return devs.filter { isHeadset(it) }.map {
            val wireless = isWireless(it)
            Headset(it.id, it.productName?.toString()?.ifBlank { null }
                ?: if (wireless) "무선 이어폰" else "유선 이어폰", wireless)
        }
    }

    private fun isHeadset(d: AudioDeviceInfo): Boolean = when (d.type) {
        AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_USB_HEADSET, AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> true
        AudioDeviceInfo.TYPE_BLE_HEADSET ->
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
        else -> false
    }

    private fun isWireless(d: AudioDeviceInfo): Boolean = when (d.type) {
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> true
        AudioDeviceInfo.TYPE_BLE_HEADSET -> Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
        else -> false
    }

    // ── 음량 ─────────────────────────────────────────────────────────────────
    /**
     * 수신 음량을 목표치 이상으로 확보한다. 최대 강제는 과청감이라 목표치 방식 —
     * 목표 미만일 때만 끌어올리고, 사용자가 수동으로 키운 값은 존중한다.
     */
    fun ensureRxVolume(ratio: Float = RX_VOLUME_RATIO) {
        val a = am ?: return
        runCatching {
            val stream = AudioManager.STREAM_VOICE_CALL
            val target = (a.getStreamMaxVolume(stream) * ratio).toInt().coerceAtLeast(1)
            if (a.getStreamVolume(stream) < target) a.setStreamVolume(stream, target, 0)
        }
    }

    companion object {
        /** 수신 스트림 목표 음량 비율 — 최대 강제는 과청감(실사용 피드백), 중간이 기준. */
        const val RX_VOLUME_RATIO = 0.5f
    }
}
