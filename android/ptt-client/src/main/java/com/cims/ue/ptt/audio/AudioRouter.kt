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

    /** volte 통화 동안 라우팅 요청 양보 — true 면 [select]/[setSpeakerphone]/[setInCall] 이 요청을 넣지 않는다. */
    @Volatile private var yielded = false

    /** 현재 양보 중 여부 — 통화 중 무전 트랙 재핀 가드용. */
    val isYielded: Boolean get() = yielded

    /**
     * 스위트 라우팅 양보(volte 통화 협조) — 이 앱의 communication device 요청(무전 스피커폰/이어폰)과
     * **오디오 모드 소유(MODE_IN_COMMUNICATION)** 를 함께 반납하고 재적용을 막는다.
     * 실측(W999/MTK13·MF52/QC15 공통): 통화 라우팅 요청은 **모드 스택의 선점 소유자** 기준으로
     * 매칭돼, PTT 가 세션으로 모드를 쥔 채면 volte 가 모드를 나중에 잡아도 volte 의 수화기/스피커
     * 요청이 무시된다(preferred=null). 모드 반납은 자기 슬롯만 빠지는 것 — 전역 모드는 volte 가
     * IN_COMMUNICATION 으로 유지하므로 무전 재생은 계속되고 경로만 volte 선택을 따른다.
     * 선택 상태(PttController._audioRoute)는 유지되므로 복귀는 setAudioRoute 재호출로 한다.
     * @return 상태가 실제로 바뀌었으면 true.
     */
    fun yieldRoute(on: Boolean): Boolean {
        if (yielded == on) return false
        yielded = on
        val a = am
        if (on) {
            clear()
            if (inCall) runCatching { a?.mode = AudioManager.MODE_NORMAL }
        } else if (inCall) {
            runCatching {
                a?.mode = AudioManager.MODE_IN_COMMUNICATION
                a?.setStreamVolume(AudioManager.STREAM_VOICE_CALL,
                    rxTargetIndex(a, AudioManager.STREAM_VOICE_CALL), 0)
            }
        }
        return true
    }

    /** 무전 수신 목표 음량 인덱스 — 최대의 [RX_VOLUME_RATIO](중간). 최대 강제는 과청감(실사용
     *  피드백)이라 목표치 방식으로: 진입 시 이 값으로 맞추고, 확보([ensureRxVolume])는 이 값
     *  미만일 때만 끌어올린다(사용자가 통화 중 수동으로 키운 값은 존중). */
    private fun rxTargetIndex(a: AudioManager, stream: Int): Int =
        (a.getStreamMaxVolume(stream) * RX_VOLUME_RATIO).toInt().coerceAtLeast(1)

    companion object {
        /** 무전 수신 스트림 목표 음량 비율 — 최대 강제는 과청감(실사용 피드백), 중간이 기준. */
        const val RX_VOLUME_RATIO = 0.5f
    }

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
        if (yielded) return    // 양보 중(volte 통화) — 모드 소유 금지, 복귀([yieldRoute]) 시 재적용
        runCatching {
            if (on) {
                a.mode = AudioManager.MODE_IN_COMMUNICATION
                a.setStreamVolume(AudioManager.STREAM_VOICE_CALL,
                    rxTargetIndex(a, AudioManager.STREAM_VOICE_CALL), 0)
                // 무전 재생 트랙은 STREAM_MUSIC(트랙 단위 분리 라우팅용 — pjsip 패치) — 그 축도 기준값으로
                a.setStreamVolume(AudioManager.STREAM_MUSIC,
                    rxTargetIndex(a, AudioManager.STREAM_MUSIC), 0)
            } else {
                a.mode = AudioManager.MODE_NORMAL
            }
        }
    }

    /** 무전 수신 볼륨(MUSIC 축) 확보 — 볼륨 인덱스는 **장치별**이라 [setInCall] 시점의 최대화는
     *  그 순간 라우팅된 장치 축에만 적용된다(이후 스피커/BT 로 바뀌면 그 축은 낮은 채 남아 무전이
     *  작게 들림 — 실측: W999 speaker=1/15, MF52 bt_a2dp=2/15). 라우트 적용 후마다 호출해 현재
     *  장치 축을 확보한다. 스트림 재라우팅이 늦게 가라앉으므로 호출측이 지연 재호출을 병행한다. */
    fun ensureRxVolume() {
        if (!inCall) return
        val a = am ?: return
        runCatching {
            val target = rxTargetIndex(a, AudioManager.STREAM_MUSIC)
            if (a.getStreamVolume(AudioManager.STREAM_MUSIC) < target)
                a.setStreamVolume(AudioManager.STREAM_MUSIC, target, 0)
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
        if (yielded) return true    // 양보 중 — 선택만 기억(복귀 시 setAudioRoute 가 재적용)
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
     *
     * API 31+ 도 명시 장치 요청과 레거시 `isSpeakerphoneOn` 을 **병행 적용**한다 — 한쪽만 듣는
     * 단말 편차 실측 2형: ①MTK(W999/A13)=타 앱(volte)의 레거시 speakerphone off 가 전역 잠금으로
     * 남아 setCommunicationDevice(speaker) 만으로 못 뒤집음 ②QC(MF52/A15)=명시 요청이 라우팅
     * 계산에 반영되지 않아 레거시가 유일한 레버. 순서: on 은 명시→레거시, off 는 레거시(자기 요청
     * clear 부작용) 먼저→clear.
     */
    fun setSpeakerphone(on: Boolean) {
        if (yielded) return         // 양보 중 — 요청 금지(복귀 시 setAudioRoute 가 재적용)
        val a = am ?: return
        runCatching {
            if (Build.VERSION.SDK_INT >= 31) {
                if (on) {
                    a.availableCommunicationDevices
                        .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
                        ?.let { a.setCommunicationDevice(it) }
                    @Suppress("DEPRECATION")
                    a.isSpeakerphoneOn = true
                } else {
                    @Suppress("DEPRECATION")
                    a.isSpeakerphoneOn = false
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
