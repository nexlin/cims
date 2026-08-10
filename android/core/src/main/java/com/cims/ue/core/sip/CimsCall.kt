package com.cims.ue.core.sip

import android.util.Log
import org.pjsip.pjsua2.Account
import org.pjsip.pjsua2.AudioMedia
import org.pjsip.pjsua2.Call
import org.pjsip.pjsua2.CallVidSetStreamParam
import org.pjsip.pjsua2.OnCallMediaStateParam
import org.pjsip.pjsua2.OnCallSdpCreatedParam
import org.pjsip.pjsua2.OnCallStateParam
import org.pjsip.pjsua2.OnCallTsxStateParam
import org.pjsip.pjsua2.pjsip_event_id_e
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

    /** 이 호의 송신 SDP 에 주입할 floor(m=application) 섹션 — 그룹콜 발신/응답 시 설정.
     *  호별 보관(전역이면 멀티그룹 동시 세션에서 서로의 floor 포트가 섞인다). */
    @Volatile var pendingAppSdp: String? = null

    /** 이 호의 송신 SDP 에 주입할 MCData MSRP(m=message TCP/MSRP) 섹션 — MSRP 발신 시 설정. */
    @Volatile var pendingMsrpSdp: String? = null

    /** MCData MSRP 미디어평면 호 — 상태를 [CallState] 대신 [SipController.msrpEvents] 로 격리
     *  (대상 그룹 URI 가 PTT 음성 세션과 같아 CallState 로 흘리면 세션 callId 오염). */
    @Volatile var msrpMode = false

    constructor(owner: SipController, acc: Account) : super(acc) { this.owner = owner }
    constructor(owner: SipController, acc: Account, callId: Int) : super(acc, callId) { this.owner = owner }

    /** 지연 작업 가드 — 통화 종료 후 native call 접근(dump 등)은 SIGSEGV (MF52 tombstone 실측). */
    @Volatile
    private var alive = true

    /** 진단 dump 1회 예약 가드 — 미디어 이벤트마다 중복 예약 방지. */
    @Volatile
    private var dumpScheduled = false

    override fun onCallState(prm: OnCallStateParam) {
        val ci = info
        if (ci.state == pjsip_inv_state.PJSIP_INV_STATE_DISCONNECTED) alive = false
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
        if (msrpMode) owner.dispatchMsrpCallState(id, mapped) else owner.dispatchCallState(id, mapped)
    }

    /**
     * SDP 생성/협상 훅 (설계서 §5.1). PTT 그룹콜에서:
     *  - 송신 SDP(offer/answer)에 `m=application` floor 라인 **주입**.
     *  - 수신 SDP 의 `m=application` 포트 **파싱** → CMP floor 목적지 학습(RTP+1 고정 금지).
     *
     * ⚠️ 주입은 **append 금지, 기존 섹션 교체**가 원칙: pjsua 는 provisioning 한 미디어 수
     * (`med_prov_cnt` = aud+vid+txt, 기본 txt_cnt=1 로 m=text 슬롯이 항상 있음)와 로컬 SDP 의
     * m= 라인 수가 어긋나면 `pjsua_media_channel_update` 에서 assert/SIGSEGV 로 **네이티브 크래시**한다.
     * 그래서 pjsua 가 만든 `m=text`(offer) 또는 `m=application`(answer mirror/재협상) 섹션을
     * floor 섹션으로 **in-place 교체**해 media_count 를 불변으로 유지한다(m=message 도 동일).
     */
    override fun onCallSdpCreated(prm: OnCallSdpCreatedParam) {
        runCatching {
            // floor 주입 여부를 남긴다 — pendingAppSdp=false 면 SDP 생성이 주입보다 앞섰다는 뜻
            //   (착신 경로의 고전적 회귀 지점, [SipController.incomingFloorSdp] KDoc 참조).
            Log.i(TAG, "onCallSdpCreated(call=$id) floorInject=${pendingAppSdp != null} " +
                "localM=[${prm.sdp.wholeSdp.lineSequence().filter { it.startsWith("m=") }.joinToString("|") { it.trim() }}]")
            pendingAppSdp?.let { extra ->
                val sdp = prm.sdp
                val whole = sdp.wholeSdp
                sdp.wholeSdp = when {
                    whole.contains("m=application") -> replaceMediaSection(whole, "m=application", extra)
                    whole.contains("m=text") -> replaceMediaSection(whole, "m=text", extra)
                    else -> appendMediaSection(whole, extra)
                }
            }
            pendingMsrpSdp?.let { extra ->
                val sdp = prm.sdp
                val whole = sdp.wholeSdp
                // UAS 답 = pjsua 가 미지원 m=message 를 포트 0 으로 답하므로 그 섹션을 교체.
                // UAC 오퍼 = m=text 슬롯 교체(append 는 med_prov_cnt 초과 → 네이티브 크래시).
                sdp.wholeSdp = when {
                    whole.contains("m=message") -> replaceMediaSection(whole, "m=message", extra)
                    whole.contains("m=text") -> replaceMediaSection(whole, "m=text", extra)
                    else -> appendMediaSection(whole, extra)
                }
            }
            if (!msrpMode) prm.remSdp?.wholeSdp?.let { rem ->
                parseApplication(rem)?.let { (ip, port) -> owner.onRemoteFloorLearned(id, ip, port) }
            }
        }.onFailure { Log.w(TAG, "onCallSdpCreated: ${it.message}") }
    }

    /** [whole] SDP 끝에 미디어 섹션 [extra] 를 덧붙인다(주입 일반화 — floor/MSRP 공용). */
    private fun appendMediaSection(whole: String, extra: String): String =
        whole.trimEnd('\r', '\n') + "\r\n" + withConnLine(whole, extra) + "\r\n"

    /** [whole] 의 기존 [prefix](예: "m=text") 미디어 섹션(다음 m= 전까지)을 [extra] 로 교체.
     *  media_count 불변 — pjsua med_prov_cnt 정합 유지(주입 원칙, 클래스 KDoc 참조). */
    private fun replaceMediaSection(whole: String, prefix: String, extra: String): String {
        val lines = whole.trimEnd('\r', '\n').split("\r\n", "\n").toMutableList()
        val start = lines.indexOfFirst { it.trim().startsWith(prefix) }
        if (start < 0) return appendMediaSection(whole, extra)
        var end = start + 1
        while (end < lines.size && !lines[end].trim().startsWith("m=")) end++
        val section = withConnLine(whole, extra).split("\r\n", "\n")
        val out = lines.subList(0, start) + section + lines.subList(end, lines.size)
        return out.joinToString("\r\n") + "\r\n"
    }

    /** 주입 섹션에 c= 라인 보장 — pjsua SDP 는 세션 레벨 c= 없이 미디어별 c= 라
     *  섹션에 c= 이 없으면 pjmedia_sdp_validate EMISSINGCONN assert 로 네이티브 abort. */
    private fun withConnLine(whole: String, extra: String): String {
        var section = extra.trim('\r', '\n')
        if (!section.contains("c=IN ")) {
            val ip = whole.lineSequence().map { it.trim() }
                .firstOrNull { it.startsWith("c=IN IP4 ") }
                ?.removePrefix("c=IN IP4 ")?.trim()
            if (ip != null) {
                val lines = section.split("\r\n", "\n").toMutableList()
                lines.add(1, "c=IN IP4 $ip")
                section = lines.joinToString("\r\n")
            }
        }
        return section
    }

    /**
     * 발신(UAC) 응답의 floor 목적지 학습 — [onCallSdpCreated] 는 **로컬 SDP 생성 시에만** 호출되어
     * UAC 가 받는 응답(200 OK) SDP 를 볼 수 없다. 수신 트랜잭션 원문에서 `m=application` 을 파싱한다.
     */
    override fun onCallTsxState(prm: OnCallTsxStateParam) {
        runCatching {
            val e = prm.e ?: return
            if (e.type != pjsip_event_id_e.PJSIP_EVENT_TSX_STATE) return@runCatching
            val msg = e.body?.tsxState?.src?.rdata?.wholeMsg ?: return@runCatching
            // MSRP 호: 발신 응답(2xx) SDP 의 서버 a=path 학습 → TCP 접속 대상
            if (msrpMode) {
                if (msg.startsWith("SIP/2.0 2") && msg.contains("TCP/MSRP")) {
                    Regex("a=path:(\\S+)").find(msg)?.groupValues?.get(1)
                        ?.let { owner.onMsrpPathLearned(id, it) }
                }
                return@runCatching
            }
            // 발신 응답(200 OK) SDP 의 floor 목적지 학습
            if (msg.contains("m=application")) {
                val sdp = msg.substringAfter("\r\n\r\n", "")
                parseApplication(sdp)?.let { (ip, port) -> owner.onRemoteFloorLearned(id, ip, port) }
            }
            // 미인가 in-call 긴급 상향 거절 (TS 24.379 §6.3.3.1.14) — 서버가 재-INVITE 를
            //   403 + mcptt-info(emergency-ind=false)로 거절. 재-INVITE 거절은 통화를 끊지
            //   않으므로(CallState 불변) 이 tsx 원문에서만 관측된다. 초기 INVITE 403 은 본문이
            //   없어 여기 매치되지 않는다(Disconnected 경로가 처리).
            if (msg.startsWith("SIP/2.0 403") && msg.contains("emergency-ind>false")) {
                owner.onEmergencyUpgradeDenied(id)
            }
            // 참가자 변경 in-dialog NOTIFY (RFC 4575 conference-info) → 참가자 목록 갱신
            // ※ pjsip 다이얼로그는 evsub 미소유 NOTIFY 에 500 을 응답하지만, invite usage 의
            //   tsx 이벤트로 원문은 전달되므로 여기서 본문을 읽는다(응답코드와 무관).
            if (msg.startsWith("NOTIFY ") && msg.contains("conference-info")) {
                Log.i(TAG, "conference NOTIFY received (${msg.length}B)")
                val body = msg.substringAfter("\r\n\r\n", "")
                if (body.isNotBlank()) owner.dispatchConferenceInfo(id, body)
            }
        }.onFailure { Log.w(TAG, "onCallTsxState: ${it.message}") }
    }

    /**
     * 미디어 활성 시 conference bridge 결선 (설계서 §6).
     *  - VoLTE(전이중): mic↔통화↔spk 상시.
     *  - PTT(반이중, [SipController.halfDuplex]): spk(청취)만 상시 연결, mic 는 floor GRANT 시 [setMic].
     */
    override fun onCallMediaState(prm: OnCallMediaStateParam) {
        runCatching {
            if (msrpMode) return@runCatching                    // 더미 오디오(a=inactive) — 결선 없음
            val aud = audioMedia()
            Log.i(
                TAG,
                "onCallMediaState: audioSlot=${aud?.portId} " +
                    "spkSlot=${PjLib.ep.audDevManager().playbackDevMedia.portId} halfDuplex=${owner.halfDuplex}",
            )
            connectListen()
            Log.i(TAG, "connectListen OK (call→spk)")
            if (!owner.halfDuplex) {
                setMic(!owner.muted)                            // 통화중 음소거 유지(재협상 후에도)
                Log.i(TAG, "setMic(${!owner.muted}) OK")
            }
            owner.videoRenderSurface?.let { attachVideo(it) }   // M1.3 수신 영상 렌더
            startVideoTransmit()                                // 발신 영상(카메라) 송신 개시 → 셀프뷰 소스
            // 진단: 미디어 활성 8초 후 pjsua 내부 통계(RX/TX 패킷·jbuf·conf 결선) 덤프.
            // ⚠️ 통화가 먼저 끝나면 실행 금지 — 파괴된 native call 의 dump() 는 SIGSEGV 로
            //    프로세스가 죽는다(runCatching 으로 못 잡음). alive 가드 + 1회 예약.
            if (!dumpScheduled) {
                dumpScheduled = true
                owner.postCtlDelayed(8000) {
                    if (!alive) return@postCtlDelayed
                    runCatching { Log.i(TAG, "media dump(+8s):\n" + dump(true, "  ")) }
                        .onFailure { Log.w(TAG, "media dump failed: ${it.message}") }
                }
            }
        }.onFailure { Log.w(TAG, "onCallMediaState FAILED", it) }
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
        val aud = audioMedia() ?: run { Log.w(TAG, "setMic($on) skipped — no active audio media"); return }
        val cap = PjLib.ep.audDevManager().captureDevMedia
        if (on) cap.startTransmit(aud) else cap.stopTransmit(aud)
    }

    /** 수신(청취) 토글 — 멀티그룹 듣기 정책: 비채널 그룹은 참여 유지하되 음소거. */
    fun setListen(on: Boolean) {
        val aud = audioMedia() ?: return
        val spk = PjLib.ep.audDevManager().playbackDevMedia
        if (on) aud.startTransmit(spk) else aud.stopTransmit(spk)
    }

    /** 수신 음량(채널별) — conference bridge 유입 레벨(1.0=원음, 0=무음). 미디어 재협상 시 리셋되므로 재적용 필요. */
    fun setRxLevel(level: Float) {
        audioMedia()?.adjustRxLevel(level)
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
