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

    init {
        runCatching { am?.registerAudioDeviceCallback(cb, null) }
        refresh()
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
}
