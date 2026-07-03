package com.cims.ue.core.sip

import android.util.Log
import org.pjsip.pjsua2.Account
import org.pjsip.pjsua2.AudioMedia
import org.pjsip.pjsua2.Call
import org.pjsip.pjsua2.CallVidSetStreamParam
import org.pjsip.pjsua2.OnCallMediaStateParam
import org.pjsip.pjsua2.OnCallSdpCreatedParam
import org.pjsip.pjsua2.OnCallStateParam
import org.pjsip.pjsua2.VideoWindowHandle
import org.pjsip.pjsua2.pjmedia_dir
import org.pjsip.pjsua2.pjmedia_type
import org.pjsip.pjsua2.pjsip_inv_state
import org.pjsip.pjsua2.pjsip_role_e
import org.pjsip.pjsua2.pjsua_call_media_status
import org.pjsip.pjsua2.pjsua_call_vid_strm_op

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
                // 착신(UAS)의 EARLY(자동 180)는 Incoming 을 덮어쓰면 안 됨 — 발신(UAC)만 Outgoing.
                if (ci.role == pjsip_role_e.PJSIP_ROLE_UAC) CallState.Outgoing(id, ci.remoteUri)
                else return

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
            if (!owner.halfDuplex) setMic(!owner.muted)         // 통화중 음소거 유지(재협상 후에도)
            owner.videoRenderSurface?.let { attachVideo(it) }   // M1.3 수신 영상 렌더
            startVideoTransmit()                                // 발신 영상(카메라) 송신 개시 → 셀프뷰 소스
        }.onFailure { Log.w(TAG, "onCallMediaState: ${it.message}") }
    }

    /**
     * 활성 영상 미디어의 **발신(카메라) 송신을 명시적으로 개시**한다. 계정 autoTransmitOutgoing
     * 만으로는 캡처가 시작되지 않는 경우가 있어(협상 dir 이 수신 위주로 열림), START_TRANSMIT 로
     * 확실히 카메라(PjCamera2)를 열어 상대에게 우리 영상을 보내고 셀프뷰 소스도 확보한다.
     */
    fun startVideoTransmit() {
        val ci = runCatching { info }.getOrNull() ?: return
        for (i in 0 until ci.media.size) {
            val m = ci.media[i]
            if (m.type == pjmedia_type.PJMEDIA_TYPE_VIDEO &&
                m.status == pjsua_call_media_status.PJSUA_CALL_MEDIA_ACTIVE
            ) {
                val hasEnc = (m.dir and pjmedia_dir.PJMEDIA_DIR_ENCODING) != 0
                Log.i(TAG, "startVideoTransmit: medIdx=${m.index} dir=${m.dir} hasEnc=$hasEnc")
                val p = CallVidSetStreamParam().apply { medIdx = m.index.toInt() }
                // 송신 방향이 아직 없으면 sendrecv 로 방향 변경(캡처 개시), 있으면 송신 시작.
                val op = if (hasEnc) pjsua_call_vid_strm_op.PJSUA_CALL_VID_STRM_START_TRANSMIT
                else pjsua_call_vid_strm_op.PJSUA_CALL_VID_STRM_CHANGE_DIR
                if (!hasEnc) p.dir = pjmedia_dir.PJMEDIA_DIR_ENCODING_DECODING
                runCatching { vidSetStream(op, p) }
                    .onFailure { Log.w(TAG, "startVideoTransmit vidSetStream(op=$op): ${it.message}") }
            }
        }
    }

    /**
     * 수신 H.264 영상을 [surface](Android Surface)에 렌더 (M1.3). 미디어 active 시 또는 surface 가
     * 나중에 준비됐을 때 [SipController.setVideoSurface] 가 재호출. 송신(카메라)은 PJSIP 영상 디바이스 자동.
     */
    fun attachVideo(surface: Any) {
        val ci = info
        for (i in 0 until ci.media.size) {
            val m = ci.media[i]
            if (m.type == pjmedia_type.PJMEDIA_TYPE_VIDEO &&
                m.status == pjsua_call_media_status.PJSUA_CALL_MEDIA_ACTIVE &&
                (m.dir and pjmedia_dir.PJMEDIA_DIR_DECODING) != 0
            ) {
                val vw = m.videoWindow
                vw.setWindow(VideoWindowHandle().apply { handle.setWindow(surface) })
                vw.Show(true)
            }
        }
    }

    /**
     * 활성 영상 송신(ENCODING) 미디어의 캡처 장치 ID(없으면 null). 로컬 셀프뷰 프리뷰가
     * 통화 송신과 **동일한 장치 ID** 로 시작해야 PJSIP 가 카메라를 공유(ref++)하고, Android
     * 카메라 단일 오픈 제약으로 인한 PJMEDIA_EVID_SYSERR(장치 2중 오픈)을 피한다.
     */
    fun videoCapDev(): Int? {
        val ci = runCatching { info }.getOrNull() ?: return null
        for (i in 0 until ci.media.size) {
            val m = ci.media[i]
            if (m.type == pjmedia_type.PJMEDIA_TYPE_VIDEO &&
                m.status == pjsua_call_media_status.PJSUA_CALL_MEDIA_ACTIVE &&
                (m.dir and pjmedia_dir.PJMEDIA_DIR_ENCODING) != 0
            ) {
                val dev = runCatching { m.videoCapDev }.getOrDefault(-1)
                if (dev >= 0) return dev
            }
        }
        return null
    }

    /** 활성 영상 미디어의 캡처 장치를 [dev]로 전환(전면↔후면). PJSIP 이 카메라를 재오픈하며,
     * 셀프뷰 프리뷰 surface(PjCamera2 정적 등록)는 새 인스턴스에 자동 재결선된다. */
    fun switchCaptureDevice(dev: Int) {
        val ci = runCatching { info }.getOrNull() ?: return
        var done = false
        for (i in 0 until ci.media.size) {
            val m = ci.media[i]
            if (m.type == pjmedia_type.PJMEDIA_TYPE_VIDEO &&
                m.status == pjsua_call_media_status.PJSUA_CALL_MEDIA_ACTIVE
            ) {
                val p = CallVidSetStreamParam().apply { medIdx = m.index.toInt(); capDev = dev }
                runCatching { vidSetStream(pjsua_call_vid_strm_op.PJSUA_CALL_VID_STRM_CHANGE_CAP_DEV, p) }
                    .onSuccess { done = true; Log.i(TAG, "switchCaptureDevice: medIdx=${m.index} capDev=$dev OK") }
                    .onFailure { Log.w(TAG, "switchCaptureDevice medIdx=${m.index}: ${it.message}") }
            }
        }
        if (!done) Log.w(TAG, "switchCaptureDevice: no active video media")
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
