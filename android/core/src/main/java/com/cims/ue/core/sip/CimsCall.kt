package com.cims.ue.core.sip

import android.util.Log
import org.pjsip.pjsua2.Account
import org.pjsip.pjsua2.AudioMedia
import org.pjsip.pjsua2.Call
import org.pjsip.pjsua2.OnCallMediaStateParam
import org.pjsip.pjsua2.OnCallSdpCreatedParam
import org.pjsip.pjsua2.OnCallStateParam
import org.pjsip.pjsua2.pjmedia_type
import org.pjsip.pjsua2.pjsip_inv_state
import org.pjsip.pjsua2.pjsua_call_media_status

/**
 * `Call` 서브클래스 — 호 상태/미디어/SDP 콜백을 [SipController] 로 중계 (설계서 §3.7, §5.1).
 *
 * 콜백은 PJSIP 워커 스레드에서 온다(추가 스레드 등록 불필요). 콜백 내부 블로킹 금지.
 *
 * ⚠️ SWIG 실측 교정: enum 은 정수 상수다(`swigValue()` 없음). `CallInfo.getState()/getType()/getStatus()`
 * 가 모두 int 를 돌려주므로 정수 상수와 직접 비교한다.
 */
class CimsCall : Call {

    private val owner: SipController

    constructor(owner: SipController, acc: Account) : super(acc) { this.owner = owner }
    constructor(owner: SipController, acc: Account, callId: Int) : super(acc, callId) { this.owner = owner }

    override fun onCallState(prm: OnCallStateParam) {
        val ci = info
        val mapped = when (ci.state) {
            pjsip_inv_state.PJSIP_INV_STATE_CALLING,
            pjsip_inv_state.PJSIP_INV_STATE_EARLY ->
                CallState.Outgoing(id, ci.remoteUri)

            pjsip_inv_state.PJSIP_INV_STATE_CONNECTING,
            pjsip_inv_state.PJSIP_INV_STATE_CONFIRMED ->
                CallState.Active(id, ci.remoteUri)

            pjsip_inv_state.PJSIP_INV_STATE_DISCONNECTED ->
                CallState.Disconnected(id, ci.lastStatusCode, ci.lastReason)

            else -> return
        }
        owner.dispatchCallState(id, mapped)
    }

    /**
     * SDP 생성/협상 훅 (설계서 §5.1). PTT 그룹콜에서:
     *  - 송신 SDP(offer/answer)에 `m=application` floor 라인 **주입**([SipController.injectApplicationSdp]).
     *  - 수신 SDP 의 `m=application` 포트 **파싱** → CMP floor 목적지 학습(RTP+1 고정 금지).
     */
    override fun onCallSdpCreated(prm: OnCallSdpCreatedParam) {
        runCatching {
            owner.injectApplicationSdp?.let { extra ->
                val sdp = prm.sdp
                val whole = sdp.wholeSdp
                if (!whole.contains("m=application")) {
                    sdp.wholeSdp = whole.trimEnd('\r', '\n') + "\r\n" + extra.trim('\r', '\n') + "\r\n"
                }
            }
            prm.remSdp?.wholeSdp?.let { rem ->
                parseApplication(rem)?.let { (ip, port) -> owner.onRemoteFloorLearned(ip, port) }
            }
        }.onFailure { Log.w(TAG, "onCallSdpCreated: ${it.message}") }
    }

    /**
     * 미디어 활성 시 conference bridge 결선 (설계서 §6).
     *  - VoLTE(전이중): mic↔통화↔spk 상시.
     *  - PTT(반이중, [SipController.halfDuplex]): spk(청취)만 상시 연결, mic 는 floor GRANT 시 [setMic].
     */
    override fun onCallMediaState(prm: OnCallMediaStateParam) {
        runCatching {
            connectListen()
            if (!owner.halfDuplex) setMic(true)
        }.onFailure { Log.w(TAG, "onCallMediaState: ${it.message}") }
    }

    /** 활성 오디오 미디어(없으면 null). Endpoint 소유이므로 보관 금지 — 매번 재취득(설계서 §3.4). */
    private fun audioMedia(): AudioMedia? {
        val ci = info
        for (i in 0 until ci.media.size) {
            val m = ci.media[i]
            if (m.type == pjmedia_type.PJMEDIA_TYPE_AUDIO &&
                m.status == pjsua_call_media_status.PJSUA_CALL_MEDIA_ACTIVE
            ) return AudioMedia.typecastFromMedia(getMedia(m.index))
        }
        return null
    }

    /** 통화 stream → 스피커(수신 항상 청취). */
    fun connectListen() {
        audioMedia()?.startTransmit(PjLib.ep.audDevManager().playbackDevMedia)
    }

    /** 마이크 송신 토글. PTT: GRANT→true(발화), RELEASE/REVOKE→false. */
    fun setMic(on: Boolean) {
        val aud = audioMedia() ?: return
        val cap = PjLib.ep.audDevManager().captureDevMedia
        if (on) cap.startTransmit(aud) else cap.stopTransmit(aud)
    }

    private fun parseApplication(sdp: String): Pair<String, Int>? {
        var sessionIp: String? = null
        var port = -1
        var mediaIp: String? = null
        var inApp = false
        for (raw in sdp.split("\n")) {
            val line = raw.trim()
            when {
                line.startsWith("c=IN IP4 ") && !inApp && port < 0 -> sessionIp = line.removePrefix("c=IN IP4 ").trim()
                line.startsWith("m=application ") -> {
                    inApp = true
                    port = line.removePrefix("m=application ").trim().substringBefore(' ').toIntOrNull() ?: -1
                }
                line.startsWith("m=") -> inApp = false
                line.startsWith("c=IN IP4 ") && inApp -> mediaIp = line.removePrefix("c=IN IP4 ").trim()
            }
        }
        val ip = mediaIp ?: sessionIp
        return if (port > 0 && ip != null) ip to port else null
    }

    private companion object {
        const val TAG = "CimsCall"
    }
}
