package com.cims.ue.core.sip

import android.util.Log
import org.pjsip.pjsua2.CodecFmtp
import org.pjsip.pjsua2.CodecFmtpVector
import org.pjsip.pjsua2.Endpoint

/**
 * 코덱 우선순위·파라미터 서버 정합 (설계서 §3.1 / §4.3 / §4.4). [PjLib.boot] 직후 pj-ctl 스레드에서 1회 적용.
 *
 * 음성: **AMR-WB 최우선**, `octet-align=1; mode-set=0,1,2`(enc/dec 양쪽 주입), pt=99. 나머지(PCMU/PCMA) 비활성.
 * 영상: **H.264 최우선**(packetization-mode/profile 은 서버 협상값 추종 — 별도 강제 안 함).
 *
 * codecId 문자열은 하드코딩하지 않고 `codecEnum2()` 실제 출력에서 부분일치로 찾는다(And-Media 테이블 표기 차이 흡수).
 */
object CodecConfig {

    private const val TAG = "CodecConfig"
    private const val AMRWB_PT = 99L

    fun apply(ep: Endpoint) = runCatching {
        val audioIds = ep.codecEnum2().let { v -> (0 until v.size).map { v[it].codecId } }
        val amrwb = audioIds.firstOrNull { it.contains("AMR-WB", ignoreCase = true) }

        if (amrwb == null) {
            Log.w(TAG, "AMR-WB codec not found in $audioIds — 음성 협상 불가 가능")
        } else {
            ep.codecSetPriority(amrwb, 254.toShort())            // 최우선
            applyAmrWbFmtp(ep, amrwb)
        }
        // 그 외 음성코덱 비활성(협상 표면 축소). 존재하는 것만.
        audioIds.filter { it != amrwb && !it.startsWith("PCMA") /* G.711 안전망 1종은 남겨도 무방 */ }
            .forEach { runCatching { ep.codecSetPriority(it, 0.toShort()) } }

        // 영상: H.264 최우선 + 인코딩 해상도 480x640(세로) 고정
        val vidIds = ep.videoCodecEnum2().let { v -> (0 until v.size).map { v[it].codecId } }
        vidIds.firstOrNull { it.contains("H264", ignoreCase = true) }
            ?.let { h264 ->
                ep.videoCodecSetPriority(h264, 254.toShort())
                applyVideoSize(ep, h264, 480, 640)
            }

        Log.i(TAG, "codec priorities applied. audio=$audioIds video=$vidIds amrwb=$amrwb")
    }.onFailure { Log.w(TAG, "codec config failed: ${it.message}") }

    /** AMR-WB fmtp: octet-align=1, mode-set=0,1,2 를 enc/dec 양쪽에 명시 주입 + pt=99 (설계서 §4.4). */
    private fun applyAmrWbFmtp(ep: Endpoint, codecId: String) {
        val cp = ep.codecGetParam(codecId)
        cp.info.pt = AMRWB_PT
        cp.setting.encFmtp = amrwbFmtp()
        cp.setting.decFmtp = amrwbFmtp()
        ep.codecSetParam(codecId, cp)
    }

    private fun amrwbFmtp(): CodecFmtpVector = CodecFmtpVector().apply {
        add(CodecFmtp().apply { name = "octet-align"; `val` = "1" })
        add(CodecFmtp().apply { name = "mode-set"; `val` = "0,1,2" })
    }

    /** H.264 인코딩 해상도 고정(세로 480x640). 캡처(카메라)·인코더가 이 크기로 협상된다. */
    private fun applyVideoSize(ep: Endpoint, codecId: String, w: Long, h: Long) = runCatching {
        val vp = ep.getVideoCodecParam(codecId)
        vp.encFmt.apply {
            width = w
            height = h
            fpsNum = 15
            fpsDenum = 1
        }
        ep.setVideoCodecParam(codecId, vp)
        Log.i(TAG, "H264 enc size set ${w}x$h")
    }.onFailure { Log.w(TAG, "applyVideoSize failed: ${it.message}") }
}
