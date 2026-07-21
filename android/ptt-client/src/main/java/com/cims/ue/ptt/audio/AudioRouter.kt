package com.cims.ue.ptt.audio

import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import com.cims.ue.core.sip.SipController
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * 이어폰(유선/블루투스 — 무선 다중 연결 포함) 열거 + 통화 오디오 출력 장치 지정.
 * 스피커폰/수화기는 PJSIP 라우팅(setOutputRoute)이 담당하고, 이 클래스는 이어폰 계열
 * 장치 선택(API 31+ setCommunicationDevice, 이하 BT SCO/유선 자동 라우팅)만 맡는다.
 */
class AudioRouter(context: Context) {

    /** 선택 가능한 이어폰 — [id]=AudioDeviceInfo.id(리부팅 간 비보존), [wireless]=블루투스/BLE. */
    data class Headset(val id: Int, val name: String, val wireless: Boolean)

    private val am = context.applicationContext.getSystemService(AudioManager::class.java)

    private val _headsets = MutableStateFlow<List<Headset>>(emptyList())
    /** 연결 중인 이어폰 목록 — 연결/해제 시 갱신(무선 다중 연결이면 여러 개). */
    val headsets: StateFlow<List<Headset>> = _headsets

    private val cb = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) = refresh()
        override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) = refresh()
    }

    /** 현재 통화(in-call) 모드 여부 — 중복 setMode/볼륨 강제 방지. */
    private var inCall = false

    init {
        runCatching { am?.registerAudioDeviceCallback(cb, null) }
        refresh()
    }

    /**
     * PTT 통화 진입/이탈 시 오디오 모드 전환. **VoIP 라우팅·음량의 전제**:
     *  - `MODE_IN_COMMUNICATION` 이라야 스피커폰/수화기(setOutputRoute→setSpeakerphoneOn)와
     *    이어폰(setCommunicationDevice/BT SCO) 라우팅이 실제 통화 경로에 적용된다(MODE_NORMAL 이면 무시).
     *  - 통화 경로가 `STREAM_VOICE_CALL` 로 잡히며, 무전(PTT)은 수신을 크게 들어야 하므로 진입 시
     *    voice-call 스트림 음량을 최대로 올린다(그룹별 미세조절은 conference RxLevel 슬라이더가 담당).
     * 통화 종료 시 `MODE_NORMAL` 복원.
     */
    fun setInCall(on: Boolean) {
        val a = am ?: return
        if (on == inCall) return
        inCall = on
        runCatching {
            if (on) {
                a.mode = AudioManager.MODE_IN_COMMUNICATION
                val max = a.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL)
                a.setStreamVolume(AudioManager.STREAM_VOICE_CALL, max, 0)
            } else {
                a.mode = AudioManager.MODE_NORMAL
            }
        }
    }

    private fun isHeadset(d: AudioDeviceInfo): Boolean = when (d.type) {
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_USB_HEADSET,
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> true
        else -> Build.VERSION.SDK_INT >= 31 && d.type == AudioDeviceInfo.TYPE_BLE_HEADSET
    }

    private fun isWireless(d: AudioDeviceInfo): Boolean =
        d.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ||
            (Build.VERSION.SDK_INT >= 31 && d.type == AudioDeviceInfo.TYPE_BLE_HEADSET)

    private fun candidates(): List<AudioDeviceInfo> = runCatching {
        val a = am ?: return emptyList()
        // API 31+ 는 통화 라우팅 가능 장치만(availableCommunicationDevices) — SCO 미개시 BT 포함
        if (Build.VERSION.SDK_INT >= 31) a.availableCommunicationDevices.filter(::isHeadset)
        else a.getDevices(AudioManager.GET_DEVICES_OUTPUTS).filter(::isHeadset)
    }.getOrDefault(emptyList())

    private fun refresh() {
        _headsets.value = candidates().map { d ->
            val name = d.productName.toString().ifBlank { if (isWireless(d)) "블루투스 이어폰" else "유선 이어폰" }
            Headset(d.id, name, isWireless(d))
        }
    }

    /** 이어폰 선택 — [id] 가 없으면(장치 교체 등) 첫 이어폰으로 폴백. @return 적용 여부. */
    fun select(id: Int): Boolean {
        val a = am ?: return false
        val dev = candidates().let { list -> list.firstOrNull { it.id == id } ?: list.firstOrNull() }
            ?: return false
        return runCatching {
            if (Build.VERSION.SDK_INT >= 31) {
                a.setCommunicationDevice(dev)
            } else @Suppress("DEPRECATION") {
                a.isSpeakerphoneOn = false
                if (isWireless(dev)) { a.startBluetoothSco(); a.isBluetoothScoOn = true }
                else { a.stopBluetoothSco(); a.isBluetoothScoOn = false }   // 유선=시스템 자동 라우팅
                true
            }
        }.getOrDefault(false)
    }

    /** 이어폰 지정 해제 — 스피커폰/수화기(PJSIP 라우팅)로 복귀할 때 호출. */
    fun clear() {
        val a = am ?: return
        runCatching {
            if (Build.VERSION.SDK_INT >= 31) a.clearCommunicationDevice()
            else @Suppress("DEPRECATION") { a.stopBluetoothSco(); a.isBluetoothScoOn = false }
        }
    }

    /**
     * 스피커폰/수화기 강제 — pjsua `setOutputRoute` 는 오디오 백엔드에 따라 미지원(무시)될 수
     * 있어(단말별 편차 실측: 스피커폰 설정이 수화기로만 출력) AudioManager 로도 직접 적용한다
     * (이중 적용 무해). `MODE_IN_COMMUNICATION`([setInCall]) 전제.
     */
    fun setSpeakerphone(on: Boolean) {
        val a = am ?: return
        runCatching {
            if (Build.VERSION.SDK_INT >= 31) {
                if (on) {
                    val spk = a.availableCommunicationDevices
                        .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
                    if (spk != null) a.setCommunicationDevice(spk)
                    else @Suppress("DEPRECATION") { a.isSpeakerphoneOn = true }
                } else {
                    a.clearCommunicationDevice()   // 통신모드 기본(수화기)으로 복귀
                }
            } else @Suppress("DEPRECATION") {
                a.isSpeakerphoneOn = on
            }
        }
    }

    fun close() {
        runCatching { am?.unregisterAudioDeviceCallback(cb) }
    }
}

/** 오디오 라우팅 선택 영속화 — 리부팅/앱 재기동 후 복원. 기본=스피커폰. */
class AudioRoutePrefs(context: Context) {
    private val prefs = context.applicationContext
        .getSharedPreferences("audio_route", Context.MODE_PRIVATE)

    var route: Int
        get() = prefs.getInt("route", SipController.AUDIO_ROUTE_SPEAKER)
        set(v) = prefs.edit().putInt("route", v).apply()

    /** 이어폰 라우팅일 때 선택 장치 id — AudioDeviceInfo.id 는 리부팅 간 비보존이라 best-effort. */
    var headsetId: Int
        get() = prefs.getInt("headset_id", -1)
        set(v) = prefs.edit().putInt("headset_id", v).apply()

    /** 무전 스피커 출력 게인(장치단, ×1.0~×3.0) — 설정 화면에서 조절, 통화 진입 시 적용. */
    var spkGain: Float
        get() = prefs.getFloat("spk_gain", DEFAULT_SPK_GAIN)
        set(v) = prefs.edit().putFloat("spk_gain", v).apply()

    /** 무전 마이크 송신 게인(장치단, ×1.0~×3.0). */
    var micGain: Float
        get() = prefs.getFloat("mic_gain", DEFAULT_MIC_GAIN)
        set(v) = prefs.edit().putFloat("mic_gain", v).apply()

    companion object {
        /** 게인 기본값 — 실측상 ×2 는 과대, ×1.5 가 무전 체감 적정 출발점. */
        const val DEFAULT_SPK_GAIN = 1.5f
        const val DEFAULT_MIC_GAIN = 1.5f
        const val GAIN_MIN = 1f
        const val GAIN_MAX = 3f
    }
}
