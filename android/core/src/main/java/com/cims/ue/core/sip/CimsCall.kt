package com.cims.ue.core.sip

import android.util.Log
import org.pjsip.pjsua2.Account
import org.pjsip.pjsua2.AudioMedia
import org.pjsip.pjsua2.Call
import org.pjsip.pjsua2.OnCallMediaStateParam
import org.pjsip.pjsua2.OnCallStateParam
import org.pjsip.pjsua2.pjmedia_type
import org.pjsip.pjsua2.pjsip_inv_state
import org.pjsip.pjsua2.pjsua_call_media_status

/**
 * `Call` 서브클래스 — 호 상태/미디어 콜백을 [SipController] 로 중계 (설계서 §3.7).
 *
 * 콜백은 PJSIP 워커 스레드에서 온다(추가 스레드 등록 불필요). 콜백 내부 블로킹 금지.
 *
 * ⚠️ SWIG 실측 교정: enum 은 정수 상수다(`swigValue()` 없음). `CallInfo.getState()/getType()/getStatus()`
 * 가 모두 int 를 돌려주므로 정수 상수와 직접 비교한다.
 */
class CimsCall : Call {

    private val owner: SipController

    /** 발신 — 앱이 만든다. */
    constructor(owner: SipController, acc: Account) : super(acc) { this.owner = owner }

    /** 착신 — onIncomingCall 의 callId 로 래핑. */
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

            else -> return  // NULL/INCOMING 은 별도 경로(착신은 dispatchIncoming)
        }
        owner.dispatchCallState(id, mapped)
    }

    /**
     * 미디어 활성 시 conference bridge 결선 (설계서 §6).
     * VoLTE(전이중): mic↔통화↔spk 상시 connect. (PTT 반이중 토글은 M2 ptt-client 에서.)
     */
    override fun onCallMediaState(prm: OnCallMediaStateParam) {
        val ci = info
        for (i in 0 until ci.media.size) {
            val m = ci.media[i]
            if (m.type == pjmedia_type.PJMEDIA_TYPE_AUDIO &&
                m.status == pjsua_call_media_status.PJSUA_CALL_MEDIA_ACTIVE
            ) {
                runCatching {
                    val aud = AudioMedia.typecastFromMedia(getMedia(m.index))
                    val adm = PjLib.ep.audDevManager()
                    adm.captureDevMedia.startTransmit(aud)   // mic → 통화
                    aud.startTransmit(adm.playbackDevMedia)  // 통화 → spk
                }.onFailure { Log.w("CimsCall", "audio bridge failed: ${it.message}") }
            }
            // VIDEO(M1.3): CallMediaInfo.videoWindow → Surface attach (별도 설계)
        }
    }
}
