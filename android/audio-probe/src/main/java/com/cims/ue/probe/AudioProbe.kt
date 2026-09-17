// 오디오 라우팅 탐침 — 판정 로직 (docs/design/features/android_dispatch_tablet.md §8, F5)
//
// 재려는 것: VOICE_COMMUNICATION 스트림이 살아 있는 동안 MEDIA 스트림을 다른 장치로 보낼 수 있는가.
// 존중되면 관제석의 "통화=헤드셋 / 무전=스피커" 비대칭 라우팅이 성립한다.
//
// v1 에서 배운 것 둘 —
//   ① `AudioTrack.getRoutedDevice()` 는 오디오 정책이 **의도한** 장치를 보고한다. 물리 링크(SCO)가
//      실제로 열렸는지와 다를 수 있다. 실측에서 "SCO 로 갔다" 고 보고했지만 소리는 내장 스피커로 났다.
//   ② SCO 링크 수립은 **비동기**다. setCommunicationDevice() 직후 트랙을 열면 라우팅이 따라오지 못한다.
//      v1 은 4초 뒤 정리했는데 그때서야 링크가 열려 "잠깐 들리다 말았다".
// 그래서 v2 는 **링크가 열린 것을 확인한 뒤** 재생하고, 판정을 귀가 아니라 **타임라인**으로 남긴다.
package com.cims.ue.probe

import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Build
import android.os.SystemClock
import kotlin.math.PI
import kotlin.math.sin

private const val SAMPLE_RATE = 48_000

/** 출력 장치 한 줄. */
data class Sink(val id: Int, val type: Int, val name: String) {
    val kind: String get() = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "수화기"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "내장 스피커"
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> "유선 헤드셋"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "유선 이어폰"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "USB 헤드셋"
        AudioDeviceInfo.TYPE_USB_DEVICE -> "USB 오디오"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "BT 통화(SCO)"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "BT 음악(A2DP)"
        AudioDeviceInfo.TYPE_BLE_HEADSET -> "BT LE 헤드셋"
        AudioDeviceInfo.TYPE_BLE_SPEAKER -> "BT LE 스피커"
        AudioDeviceInfo.TYPE_TELEPHONY -> "통신망"
        24 -> "내장 스피커(보호)"
        else -> "기타($type)"
    }
    /** 소리가 기기 밖으로 나가는 장치인가 — 이어폰에서 들리는지 판별의 기준. */
    val isExternal: Boolean get() = type in setOf(
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO, AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
        AudioDeviceInfo.TYPE_BLE_HEADSET, AudioDeviceInfo.TYPE_BLE_SPEAKER,
        AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_USB_HEADSET, AudioDeviceInfo.TYPE_USB_DEVICE)
    override fun toString() = "$kind · $name (id=$id)"
}

/** 재생 모드 — 한 번에 하나만 내면 "이어폰에서 나나" 를 귀로도 명확히 가릴 수 있다. */
enum class PlayMode(val label: String) {
    CALL_ONLY("통화만 (440Hz)"),
    RADIO_ONLY("무전만 (880Hz)"),
    BOTH("동시 (440 + 880Hz)"),
}

/** 타임라인 한 줄 — 0.5초마다 실제 상태를 찍는다. */
data class Tick(
    val atMs: Long,
    val scoOn: Boolean,
    val commDevice: String,
    val callRouted: String,
    val radioRouted: String,
)

data class ProbeResult(
    val device: String,
    val sdk: Int,
    val mode: PlayMode,
    val requestedCall: Sink?,
    val requestedRadio: Sink?,
    /** 통신 장치 지정이 받아들여졌는가(setCommunicationDevice 반환값). */
    val commAccepted: Boolean?,
    /** 재생 전에 링크가 열린 것을 확인했는가. false 면 기다렸지만 안 열렸다는 뜻. */
    val linkReady: Boolean,
    val linkWaitMs: Long,
    val preferredAccepted: Boolean?,
    val timeline: List<Tick>,
    val verdict: String,
    val detail: String,
)

class AudioProbe(private val am: AudioManager) {

    fun outputs(): List<Sink> =
        am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
            .filter { it.isSink }
            .map { Sink(it.id, it.type, it.productName?.toString().orEmpty()) }

    /** 통신용으로 쓸 수 있는 장치 — 이 목록에 없으면 setCommunicationDevice 로 고를 수 없다. */
    fun communicationDevices(): List<Sink> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            am.availableCommunicationDevices.map { Sink(it.id, it.type, it.productName?.toString().orEmpty()) }
        else emptyList()

    private fun info(id: Int): AudioDeviceInfo? =
        am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull { it.id == id }

    private fun sinkOf(d: AudioDeviceInfo?): String =
        d?.let { Sink(it.id, it.type, it.productName?.toString().orEmpty()).toString() } ?: "(없음)"

    /**
     * 본 시험.
     *
     * 순서가 v1 과 다르다 — **링크를 먼저 세우고, 열린 것을 확인한 뒤** 트랙을 만든다. SCO 는 수립에
     * 수 초가 걸리고, 이미 재생 중인 트랙은 재라우팅되지 않기 때문이다.
     */
    fun run(mode: PlayMode, call: Sink?, radio: Sink?,
            playMs: Long = 8_000, linkTimeoutMs: Long = 10_000): ProbeResult {
        var callTrack: AudioTrack? = null
        var radioTrack: AudioTrack? = null
        val notes = StringBuilder()
        val timeline = mutableListOf<Tick>()
        var commAccepted: Boolean? = null
        var linkReady = false
        var waited = 0L
        var prefAccepted: Boolean? = null

        try {
            am.mode = AudioManager.MODE_IN_COMMUNICATION

            // ── ① 통신 장치 지정 + **링크가 실제로 설 때까지 대기** ──
            val wantCall = call?.takeIf { mode != PlayMode.RADIO_ONLY }
            if (wantCall != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    val d = am.availableCommunicationDevices.firstOrNull { it.id == wantCall.id }
                    if (d == null) {
                        notes.append("· 통신 장치 목록에 없음 — A2DP 전용이거나 권한 부족\n")
                        commAccepted = false
                    } else {
                        commAccepted = runCatching { am.setCommunicationDevice(d) }.getOrDefault(false)
                        if (!commAccepted!!) notes.append("· setCommunicationDevice 거부\n")
                    }
                } else {
                    notes.append("· API 31 미만 — setCommunicationDevice 없음\n")
                    commAccepted = false
                }

                // 링크 확인: 통신 장치가 실제로 바뀌었고, BT 면 SCO 가 켜졌는지까지.
                val start = SystemClock.elapsedRealtime()
                while (SystemClock.elapsedRealtime() - start < linkTimeoutMs) {
                    val cur = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) am.communicationDevice else null
                    val isBt = wantCall.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO
                    val ok = cur?.id == wantCall.id && (!isBt || am.isBluetoothScoOn)
                    if (ok) { linkReady = true; break }
                    Thread.sleep(200)
                }
                waited = SystemClock.elapsedRealtime() - start
                if (!linkReady) notes.append("· 링크가 ${waited}ms 안에 서지 않았다 — 그대로 진행한다\n")
                else notes.append("· 링크 확인까지 ${waited}ms\n")
            }

            // ── ② 링크가 선 뒤에 트랙 생성 ──
            if (mode != PlayMode.RADIO_ONLY) {
                callTrack = tone(440.0, AudioAttributes.USAGE_VOICE_COMMUNICATION, AudioAttributes.CONTENT_TYPE_SPEECH)
                callTrack.play()
            }
            if (mode != PlayMode.CALL_ONLY) {
                radioTrack = tone(880.0, AudioAttributes.USAGE_MEDIA, AudioAttributes.CONTENT_TYPE_MUSIC)
                radio?.let { r -> info(r.id)?.let { prefAccepted = radioTrack!!.setPreferredDevice(it) } }
                radioTrack.play()
            }

            // ── ③ 타임라인 — 귀 대신 이걸로 판정한다 ──
            val t0 = SystemClock.elapsedRealtime()
            while (SystemClock.elapsedRealtime() - t0 < playMs) {
                Thread.sleep(500)
                timeline += Tick(
                    atMs = SystemClock.elapsedRealtime() - t0,
                    scoOn = am.isBluetoothScoOn,
                    commDevice = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) sinkOf(am.communicationDevice) else "(API<31)",
                    callRouted = callTrack?.let { sinkOf(it.routedDevice) } ?: "(재생 안 함)",
                    radioRouted = radioTrack?.let { sinkOf(it.routedDevice) } ?: "(재생 안 함)")
            }

            return ProbeResult(
                device = "${Build.MANUFACTURER} ${Build.MODEL}",
                sdk = Build.VERSION.SDK_INT,
                mode = mode,
                requestedCall = wantCall, requestedRadio = radio.takeIf { mode != PlayMode.CALL_ONLY },
                commAccepted = commAccepted, linkReady = linkReady, linkWaitMs = waited,
                preferredAccepted = prefAccepted,
                timeline = timeline,
                verdict = verdict(mode, wantCall, radio, timeline, linkReady),
                detail = notes.toString().ifBlank { "(특이사항 없음)" })
        } catch (t: Throwable) {
            return ProbeResult("${Build.MANUFACTURER} ${Build.MODEL}", Build.VERSION.SDK_INT, mode,
                call, radio, commAccepted, linkReady, waited, prefAccepted, timeline,
                "오류", "${t::class.java.simpleName}: ${t.message}")
        } finally {
            runCatching { callTrack?.stop(); callTrack?.release() }
            runCatching { radioTrack?.stop(); radioTrack?.release() }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) runCatching { am.clearCommunicationDevice() }
            runCatching { am.mode = AudioManager.MODE_NORMAL }
        }
    }

    /** 판정은 **타임라인의 마지막 상태**로 한다 — 초반은 링크 전환 중이라 흔들린다. */
    private fun verdict(mode: PlayMode, call: Sink?, radio: Sink?,
                        timeline: List<Tick>, linkReady: Boolean): String {
        val last = timeline.lastOrNull() ?: return "측정 실패 — 타임라인이 비었다"
        return when (mode) {
            PlayMode.CALL_ONLY ->
                if (call == null) "대상 없음"
                else if (last.callRouted.contains("id=${call.id}")) "통화 → 요청한 장치로 나간다" + if (!linkReady) " (링크 미확인)" else ""
                else "통화가 요청한 장치로 가지 않는다 — 실제: ${last.callRouted}"
            PlayMode.RADIO_ONLY ->
                if (radio == null) "대상 없음"
                else if (last.radioRouted.contains("id=${radio.id}")) "무전 → 요청한 장치로 나간다"
                else "무전이 요청한 장치로 가지 않는다 — 실제: ${last.radioRouted}"
            PlayMode.BOTH -> {
                val callOk = call != null && last.callRouted.contains("id=${call.id}")
                val radioOk = radio != null && last.radioRouted.contains("id=${radio.id}")
                val split = last.callRouted != last.radioRouted
                when {
                    callOk && radioOk && split -> "성립 — 두 스트림이 각자 요청한 장치로 갈라진다"
                    split -> "부분 — 갈라지긴 하나 요청대로는 아니다"
                    else -> "불가 — 두 스트림이 같은 장치로 나간다 (${last.callRouted})"
                }
            }
        }
    }

    /**
     * 연속 사인파.
     *
     * 버퍼 길이를 **주기의 정수배**로 잡는다 — 그러지 않으면 루프 이음매에서 위상이 튀어 딱딱거린다
     * (v1 의 "뚜뚜뚜"). 48kHz 에서 440Hz 는 1200 샘플에 11 주기, 880Hz 는 600 샘플에 11 주기로 정확히 맞는다.
     */
    private fun tone(hz: Double, usage: Int, contentType: Int): AudioTrack {
        val g = gcd(SAMPLE_RATE, hz.toInt())
        val framesPerCycleSet = SAMPLE_RATE / g          // 이 샘플 수에 정확히 (hz/g) 주기가 들어간다
        val repeats = maxOf(1, 24_000 / framesPerCycleSet)
        val n = framesPerCycleSet * repeats
        val buf = ShortArray(n) { i -> (sin(2.0 * PI * hz * i / SAMPLE_RATE) * 11000).toInt().toShort() }

        val fmt = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(SAMPLE_RATE)
            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
            .build()
        val attrs = AudioAttributes.Builder().setUsage(usage).setContentType(contentType).build()
        val t = AudioTrack.Builder()
            .setAudioAttributes(attrs)
            .setAudioFormat(fmt)
            .setBufferSizeInBytes(n * 2)
            .setTransferMode(AudioTrack.MODE_STATIC)
            .build()
        t.write(buf, 0, n)
        t.setLoopPoints(0, n, -1)
        return t
    }

    private fun gcd(a: Int, b: Int): Int = if (b == 0) a else gcd(b, a % b)
}
