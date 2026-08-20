package com.cims.ue.core.sip

import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import com.cims.ue.core.config.SipAccountConfig
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import org.pjsip.pjsua2.AccountConfig
import org.pjsip.pjsua2.AuthCredInfo
import org.pjsip.pjsua2.CallOpParam
import org.pjsip.pjsua2.SdpSession
import org.pjsip.pjsua2.SendRequestParam
import org.pjsip.pjsua2.SipHeader
import org.pjsip.pjsua2.SipHeaderVector
import org.pjsip.pjsua2.SipMediaType
import org.pjsip.pjsua2.SipMultipartPart
import org.pjsip.pjsua2.SipMultipartPartVector
import org.pjsip.pjsua2.SipTxOption
import org.pjsip.PjCamera2
import org.pjsip.pjsua2.pjmedia_dir
import org.pjsip.pjsua2.pjmedia_vid_dev_std_index
import org.pjsip.pjsua2.pjsip_cred_data_type
import org.pjsip.pjsua2.pjsip_status_code
import org.pjsip.pjsua2.pjsua_stun_use
import java.util.concurrent.ConcurrentHashMap

/**
 * VoLTE 1:1 SIP 진입 클래스 (설계서 §3.6, M1.1~M1.2).
 *
 * **스레딩 규약(사고 최빈 영역):** 모든 pjsua2 호출을 단일 `pj-ctl` HandlerThread 로 직렬화하고,
 * 그 스레드에서 1회 [PjLib.ensureThread]. UI/코루틴은 이 스레드로 post 만 한다. 콜백은 PJSIP 스레드.
 *
 * **수명:** `account` 강참조, `Call` 은 [calls] 맵 강참조 → DISCONNECTED 에서만 remove+delete (GC 방지).
 *
 * 상태는 [regState]/[callState] StateFlow 로만 노출 — pjsua2 타입이 UI 로 새지 않는 경계.
 */
class SipController(private val config: SipAccountConfig) {

    private val _reg = MutableStateFlow<RegState>(RegState.Idle)
    val regState: StateFlow<RegState> = _reg.asStateFlow()

    private val _call = MutableStateFlow<CallState>(CallState.Null)
    val callState: StateFlow<CallState> = _call.asStateFlow()

    /** 수신 문자(SIP MESSAGE) 이벤트. PJSIP 콜백 스레드에서 tryEmit — 구독자(서비스)가 저장/알림. */
    private val _incomingMessage = MutableSharedFlow<ImMessage>(extraBufferCapacity = 32)
    val incomingMessage: SharedFlow<ImMessage> = _incomingMessage.asSharedFlow()

    /** in-dialog conference NOTIFY(RFC 4575) 본문 — (callId, XML). PTT 참가자 목록 갱신용. */
    private val _conferenceInfo = MutableSharedFlow<Pair<Int, String>>(extraBufferCapacity = 16)
    val conferenceInfo: SharedFlow<Pair<Int, String>> = _conferenceInfo.asSharedFlow()

    /** MCData MSRP 미디어평면 호 이벤트 — [CallState] 와 격리(그룹 URI 동일로 인한 세션 오염 방지). */
    private val _msrpEvents = MutableSharedFlow<MsrpEvent>(extraBufferCapacity = 32)
    val msrpEvents: SharedFlow<MsrpEvent> = _msrpEvents.asSharedFlow()

    /** [sendRequest] 트랜잭션의 최종 응답(≥200) — token 으로 요청과 상관(affiliation PUBLISH 확인용).
     *  같은 트랜잭션이 COMPLETED/TERMINATED 로 중복 통지될 수 있어 구독자가 token 당 1회만 처리한다. */
    private val _sendReqResults = MutableSharedFlow<SendReqResult>(extraBufferCapacity = 32)
    val sendReqResults: SharedFlow<SendReqResult> = _sendReqResults.asSharedFlow()

    /** 미인가 in-call 긴급 상향 거절(403 + emergency-ind=false, TS 24.379 §6.3.3.1.14) — callId.
     *  재-INVITE 거절은 통화를 끊지 않으므로 별도 이벤트로 올려 긴급 latch 를 되돌리게 한다. */
    private val _emergencyDenied = MutableSharedFlow<Int>(extraBufferCapacity = 8)
    val emergencyDenied: SharedFlow<Int> = _emergencyDenied.asSharedFlow()

    /** 세션 긴급 상태 재광고(TS 24.379 §6.3.3.1.15/16) — (callId, active). CSP 가 in-call
     *  상향/하향 시 멤버 leg 에 보내는 re-INVITE(mcptt-info emergency-ind)와, 조인/재조인
     *  200 OK 에 동봉된 현재 상태를 [CimsCall] 이 관측해 올린다. */
    private val _sessionEmergency = MutableSharedFlow<Pair<Int, Boolean>>(extraBufferCapacity = 8)
    val sessionEmergency: SharedFlow<Pair<Int, Boolean>> = _sessionEmergency.asSharedFlow()

    /** REGISTER Contact 에 부가할 파라미터(예: MCData ICSI feature tag) — [register] 전에 설정.
     *  예: `;+g.3gpp.icsi-ref="urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds"` */
    @Volatile var contactParams: String = ""

    /**
     * MCPTT 착신 INVITE 의 응답 SDP 에 실을 floor(m=application) 섹션 공급자 — 앱이 등록한다.
     *
     * ⚠️ **호출 시점이 계약의 본질**: pjsua 는 착신 INVITE 를 처리하며(180 Ringing) 로컬 응답
     * SDP 를 **한 번** 만들고 그것을 200 OK 에 재사용한다. 그 시점 이후에 `pendingAppSdp` 를
     * 넣어봐야 [CimsCall.onCallSdpCreated] 는 이미 지나갔으므로 주입이 영구히 스킵되고,
     * pjsua 가 미지원 스트림에 붙인 `m=application 0` 이 그대로 나간다. 그러면 CSP 는 착신
     * leg 의 floor 포트를 알 수 없어 관례 fallback(audio+1 = RTCP 포트)으로 CMP 에 JOIN 을
     * 보내고, floor 메시지는 CMP 의 주소 latch(단말 Floor Ack 학습)에 의존해서만 도달한다.
     * 따라서 앱은 **INVITE 를 받는 순간 floor 소켓을 이미 갖고** 있어야 하며, 이 공급자가
     * 그 시점(=180 응답 전)에 호출된다.
     *
     * 반환값 = 주입할 SDP 섹션(예: `m=application 40603 UDP MCPTT\r\na=floorid:0 ...`).
     * null 이면 주입하지 않는다(floor 를 쓰지 않는 착신).
     */
    @Volatile var incomingFloorSdp: ((IncomingFloorInfo) -> String?)? = null

    private val ctl = HandlerThread("pj-ctl").apply { start() }
    private val h = Handler(ctl.looper)

    private var account: CimsAccount? = null                       // 강참조
    private val calls = ConcurrentHashMap<Int, CimsCall>()         // callId → Call 강참조

    @Volatile
    var videoEnabled = false                                       // M1.3 토글

    /** 수신 영상 렌더 대상(Android Surface). UI SurfaceView 준비 시 [setVideoSurface] 로 주입. */
    @Volatile var videoRenderSurface: Any? = null
        private set

    /** 렌더 Surface 설정/해제 + 활성 호에 재결선(미디어가 이미 active 였던 경우). */
    fun setVideoSurface(surface: Any?) = onCtl {
        videoRenderSurface = surface
        if (surface != null) calls.values.forEach { runCatching { it.attachVideo(surface) } }
    }

    /**
     * 로컬 카메라 프리뷰(내 화면). UI SurfaceView 준비 시 Surface 주입, null 이면 중지.
     *
     * 카메라를 **두 번째로 열지 않는다**(과거 VideoPreview/pjsua_vid_preview_start 방식은 Android
     * 카메라 단일 오픈 제약으로 활성 영상통화 중 PJMEDIA_EVID_SYSERR). 대신 PJSIP 캡처가 이미 연
     * CameraDevice 의 CaptureSession 에 셀프뷰 surface 를 **출력 target 으로 추가**하도록
     * [PjCamera2.SetPreviewSurface] 에 등록한다(Camera2 다중 출력 surface). 통화 전 등록해두면
     * 캡처 시작 시 승계되고, 통화 중 등록하면 세션이 재구성된다.
     */
    fun setPreviewSurface(surface: Any?) = onCtl {
        runCatching { PjCamera2.SetPreviewSurface(surface as? android.view.Surface) }
    }

    // pj-ctl 스레드에서만 호출.
    private fun stopPreview() {
        runCatching { PjCamera2.SetPreviewSurface(null) }
    }

    // 현재 캡처 카메라 장치 id(초기 -1 → 최초 전환 시 전면으로 확정). 전면↔후면 토글 추적.
    @Volatile
    private var camDev = -1

    /** 전면↔후면 카메라 전환 — 활성 영상호의 캡처 장치를 현재와 다른 Android 카메라로 교체. */
    fun switchCamera() = onCtl {
        val call = calls.values.firstOrNull() ?: run { Log.w(TAG, "switchCamera: no active call"); return@onCtl }
        val devs = PjLib.ep.vidDevManager().enumDev2()
        val caps = ArrayList<Int>()
        for (i in 0 until devs.size) {
            val d = devs[i]
            if ((d.dir and pjmedia_dir.PJMEDIA_DIR_CAPTURE) == 0) continue
            if (!d.driver.equals("Android", ignoreCase = true)) continue   // Colorbar 등 합성 장치 제외
            caps.add(d.id)
        }
        if (caps.size < 2) { Log.w(TAG, "switchCamera: 카메라 ${caps.size}개(전환 불가) caps=$caps"); return@onCtl }
        if (camDev < 0) camDev = frontCaptureDev()
        val next = caps.firstOrNull { it != camDev } ?: caps[0]
        Log.i(TAG, "switchCamera: $camDev -> $next (caps=$caps)")
        call.switchCaptureDevice(next)
        camDev = next
    }

    /** 통화중 마이크 음소거(VoLTE 전이중). 미디어 재협상(onCallMediaState 재진입) 후에도 유지. */
    @Volatile var muted = false
        private set

    fun setMuted(callId: Int, on: Boolean) = onCtl {
        muted = on
        calls[callId]?.setMic(!on)
    }

    // ── PTT(M2) 지원 ──
    /** 반이중(PTT): true 면 mic 는 floor GRANT 시에만 송신, spk 는 상시 청취. (VoLTE=false 전이중) */
    @Volatile var halfDuplex = false

    /** 수신 SDP 에서 학습한 CMP floor 목적지 — (callId, ip, port). 그룹별 FloorClient 가 여기로 송신. */
    private val _floorRemote = MutableStateFlow<Triple<Int, String, Int>?>(null)
    val floorRemote: StateFlow<Triple<Int, String, Int>?> = _floorRemote.asStateFlow()

    /**
     * pj-ctl 스레드에서 제어 작업 실행. [affectsReg]=true(등록 계열)일 때만 실패를 등록
     * 상태(_reg)로 반영한다. 프리뷰·렌더·음소거 등 **미디어/부가 작업의 실패가 등록 배지를
     * "오프라인"으로 오표시하지 않도록** 분리 — 등록의 정본은 onRegState(dispatchReg) 이다.
     * (예: 영상통화 중 pjsua_vid_preview_start 실패(PJMEDIA_EVID_SYSERR)가 REGISTER 와 무관하게
     * 배지를 오프라인으로 만들던 버그 수정.)
     */
    private fun onCtl(affectsReg: Boolean = false, block: () -> Unit) = h.post {
        runCatching {
            if (PjLib.booted) PjLib.ensureThread("pj-ctl")   // 부팅 전(최초 register)엔 ep 미초기화 → skip
            block()
        }.onFailure {
            Log.e(TAG, "pj-ctl error (affectsReg=$affectsReg)", it)
            if (affectsReg) _reg.value = RegState.Failed(it.message ?: "error")
        }
    }

    // ── 외부 명령 ──

    fun register() = onCtl(affectsReg = true) {
        PjLib.boot()
        PjLib.ensureThread("pj-ctl")                  // 부팅 직후 pj-ctl 스레드 1회 등록
        CodecConfig.apply(PjLib.ep)
        _reg.value = RegState.Registering
        val acc = CimsAccount(this).also { account = it }
        acc.create(buildAccountConfig(config))                    // registerOnAdd=true → REGISTER 발신
    }

    /**
     * 등록 갱신(재-REGISTER) — 서버가 등록을 잃은 경우(서버 재기동 등) 즉시 복구용.
     * 단말은 자기 갱신 타이머(Expires 절반)까지 등록 소실을 알 수 없어, 그동안 제휴·착신이 막힌다.
     * ⚠️[register] 처럼 Account 를 재생성하지 않는다 — 프로세스 내 PJSIP 재부팅은 지뢰(Endpoint 수명).
     */
    fun refreshRegistration() = onCtl {
        val acc = account ?: return@onCtl
        runCatching { acc.setRegistration(true) }
            .onFailure { Log.w(TAG, "refreshRegistration: ${it.message}") }
    }

    // de-REGISTER(expires=0). 미등록 상태면 PJSIP 가 EINVALIDOP 를 던지므로 조용히 무시.
    fun unregister() = onCtl { account?.let { runCatching { it.setRegistration(false) } } }

    /**
     * 강제 재등록 — 네트워크 복귀/포그라운드 복귀 시 호출(등록 keepalive).
     * 계정이 이미 있으면 즉시 REGISTER 재발신, 없으면(서비스가 죽었다 살아난 경우) 무시(호출부에서 register).
     */
    fun reregister() = onCtl {
        val acc = account ?: return@onCtl
        if (!PjLib.booted) return@onCtl
        _reg.value = RegState.Registering
        runCatching { acc.setRegistration(true) }
            .onFailure { Log.w(TAG, "reregister failed", it) }
    }

    /** 등록된 계정이 있는지(서비스에서 reregister vs register 판단용). */
    fun hasAccount(): Boolean = account != null

    fun makeCall(dstNumber: String) = onCtl {
        val acc = account ?: run {
            _call.value = CallState.Disconnected(-1, 0, "not registered"); return@onCtl
        }
        halfDuplex = false                                        // VoLTE = 전이중
        muted = false                                             // 새 호는 음소거 해제로 시작
        val call = CimsCall(this, acc)
        val prm = CallOpParam(true).apply {
            opt.audioCount = 1L
            opt.videoCount = if (videoEnabled) 1L else 0L
        }
        call.makeCall("sip:$dstNumber@${config.domain}", prm)
        calls[call.id] = call
    }

    /**
     * MCPTT 그룹콜 착신 자동 수락(ptt_ue.md §12.3) — 응답 SDP 에 `m=application`(floor) 주입,
     * 상대(INVITE offer)의 floor 포트는 [floorRemote] 로 학습(onCallSdpCreated remSdp).
     */
    /** [fullDuplex]=전이중 1:1 수락(mc_no_floor_ctrl 협상) — 마이크 상시 개방(VoLTE 와 동일). */
    fun answerGroupCall(callId: Int, applicationSdp: String, fullDuplex: Boolean = false) = onCtl {
        halfDuplex = !fullDuplex
        muted = false
        calls[callId]?.apply {
            pendingAppSdp = applicationSdp
            answer(
                CallOpParam(true).apply {
                    statusCode = pjsip_status_code.PJSIP_SC_OK
                    opt.audioCount = 1L
                    opt.videoCount = 0L
                },
            )
        }
    }

    /** 착신 응답. [withVideo]=true 면 영상까지 협상(상대가 m=video 를 offer 한 경우). */
    fun answer(callId: Int, withVideo: Boolean = false) = onCtl {
        muted = false
        if (withVideo) videoEnabled = true
        calls[callId]?.answer(
            CallOpParam(true).apply {
                statusCode = pjsip_status_code.PJSIP_SC_OK
                opt.audioCount = 1L
                opt.videoCount = if (withVideo) 1L else 0L
            },
        )
    }

    fun reject(callId: Int) = onCtl {
        calls[callId]?.answer(CallOpParam().apply { statusCode = pjsip_status_code.PJSIP_SC_BUSY_HERE })
    }

    fun hangup(callId: Int) = onCtl { calls[callId]?.hangup(CallOpParam()) }

    // ── PTT(M2): affiliation PUBLISH / 그룹콜(multipart+floor SDP) / 반이중 mic ──

    /** 마이크 송신 토글 (PTT floor GRANT→true, RELEASE/REVOKE→false). */
    fun setMicEnabled(callId: Int, on: Boolean) = onCtl { calls[callId]?.setMic(on) }

    /** 캡처 게이트 현재 상태 — 중복 재오픈 방지. 초기값 true(전이중, VoLTE 기본). */
    @Volatile private var captureEnabled = true

    /**
     * snd dev 캡처 게이트 — 마이크(AudioRecord) 보유를 재생과 분리해 제어한다.
     *  - `false`: SPEAKER_ONLY — 재생은 유지하고 캡처 스트림 자체를 열지 않는다. OS 동시 캡처
     *    중재(경합 앱 무음화)에서 완전히 빠진다. PTT 유휴/청취, VoLTE 의 발언 양보(MIC_YIELD) 구간.
     *  - `true`: 전이중 복귀 — PTT 발언(floor GRANT 경로), VoLTE 통화/양보 해제.
     *  NO_IMMEDIATE_OPEN 동반: snd dev 가 닫혀 있으면 모드만 저장(다음 conference 결선의
     *  on-demand 오픈에 적용), 열려 있으면 즉시 재오픈 — conference 결선은 브리지에 남아
     *  재오픈을 넘어 생존한다(pjsua_set_snd_dev2).
     */
    fun setCaptureEnabled(on: Boolean) = onCtl {
        if (captureEnabled == on) return@onCtl
        captureEnabled = on
        if (!PjLib.booted) return@onCtl                 // 부팅 전 — register() 이후 호출 전제(onCtl 직렬)
        val mode = (if (on) 0 else org.pjsip.pjsua2.pjsua_snd_dev_mode.PJSUA_SND_DEV_SPEAKER_ONLY) or
            org.pjsip.pjsua2.pjsua_snd_dev_mode.PJSUA_SND_DEV_NO_IMMEDIATE_OPEN
        runCatching { PjLib.ep.audDevManager().setSndDevMode(mode.toLong()) }
            .onSuccess { Log.i(TAG, "setCaptureEnabled($on) snd mode=$mode") }
            .onFailure { Log.w(TAG, "setCaptureEnabled($on) 실패: ${it.message}") }
        applyDeviceAudioBoost()    // (재)오픈으로 slot0 레벨 초기화 + 전이중 전환 시 mic 축 적용
    }

    /** 사운드 장치 재오픈(현재 캡처 게이트 모드 재적용) — 열려 있으면 즉시 닫고 다시 열어
     *  재생/캡처 트랙을 재생성한다(conference 결선은 브리지에 생존, ~100ms 공백).
     *  용도: 라우팅 중이던 출력 장치가 소멸(BT/이어폰 해제)하는 순간 일부 단말(MF52/A15 실측)이
     *  재생 트랙에 시스템 뮤트(streamVolume)를 건 채 재라우팅 후에도 해제하지 않는다 — 볼륨
     *  변경으로도 안 풀리며, 트랙을 새로 만들어야 뮤트 평가가 리셋된다. */
    fun bounceSndDev() = onCtl {
        if (!PjLib.booted) return@onCtl
        val mode = (if (captureEnabled) 0 else org.pjsip.pjsua2.pjsua_snd_dev_mode.PJSUA_SND_DEV_SPEAKER_ONLY) or
            org.pjsip.pjsua2.pjsua_snd_dev_mode.PJSUA_SND_DEV_NO_IMMEDIATE_OPEN
        runCatching { PjLib.ep.audDevManager().setSndDevMode(mode.toLong()) }
            .onSuccess { Log.i(TAG, "bounceSndDev snd mode=$mode") }
            .onFailure { Log.w(TAG, "bounceSndDev 실패: ${it.message}") }
        applyDeviceAudioBoost()    // 재오픈으로 slot0 레벨 초기화 — 재적용
    }

    /** 오디오 출력 라우팅 — PTT(무전) UX 용 스피커폰 토글. */
    fun setLoudspeaker(on: Boolean) =
        setAudioRoute(if (on) AUDIO_ROUTE_SPEAKER else AUDIO_ROUTE_EARPIECE)

    /** 오디오 출력 3단 라우팅 — [AUDIO_ROUTE_DEFAULT](자동: 이어폰 연결 시 이어폰)/수화구/스피커. */
    fun setAudioRoute(route: Int) = onCtl {
        // 부팅 전/종료 후(libDestroy) native 호출 금지 — 로그아웃→재로그인처럼 프로세스가 산 채
        // 재부팅되는 경로에서 파괴된 endpoint 호출은 abort. register() 후 재적용이 라우팅을 확보.
        if (!PjLib.booted) return@onCtl
        PjLib.ep.audDevManager().setOutputRoute(
            when (route) {
                AUDIO_ROUTE_SPEAKER -> org.pjsip.pjsua2.pjmedia_aud_dev_route.PJMEDIA_AUD_DEV_ROUTE_LOUDSPEAKER
                AUDIO_ROUTE_EARPIECE -> org.pjsip.pjsua2.pjmedia_aud_dev_route.PJMEDIA_AUD_DEV_ROUTE_EARPIECE
                else -> org.pjsip.pjsua2.pjmedia_aud_dev_route.PJMEDIA_AUD_DEV_ROUTE_DEFAULT
            },
        )
    }

    /** 진단용 — pj-ctl 스레드에 지연 실행 예약 (pjsua2 호출 직렬화 규약 준수). */
    fun postCtlDelayed(ms: Long, block: () -> Unit) {
        h.postDelayed(
            {
                runCatching {
                    if (PjLib.booted) PjLib.ensureThread("pj-ctl")
                    block()
                }.onFailure { Log.w(TAG, "postCtlDelayed: ${it.message}") }
            },
            ms,
        )
    }

    /** 통화별 청취(수신 오디오 → 스피커) 토글 — 멀티그룹 듣기 정책용. */
    fun setCallListen(callId: Int, on: Boolean) = onCtl { calls[callId]?.setListen(on) }

    /** 통화별 수신 음량(1.0=원음, 0=무음) — 채널별 볼륨 슬라이더용. */
    fun setCallRxLevel(callId: Int, level: Float) = onCtl { calls[callId]?.setRxLevel(level) }

    /**
     * 장치단(conference bridge slot0) 오디오 gain — 무전(PTT) 체감 음량 보강용.
     * [spk]=bridge→스피커 출력 gain, [mic]=마이크→bridge 입력 gain (1.0=원음).
     * 시스템 스트림 음량·라우팅과 무관하게 디지털 레벨 자체가 낮은 단말(캡처 게인 약함)을
     * 보정한다. 통화별 RxLevel(슬라이더)과 곱으로 적용되므로 과도값은 클리핑 유발 — 2.0 권장.
     */
    fun setDeviceAudioBoost(spk: Float, mic: Float) = onCtl {
        boostSpk = spk; boostMic = mic
        applyDeviceAudioBoost()
    }

    /** 저장된 boost 재적용. 두 축을 **개별** runCatching 으로 적용한다 — 캡처 게이트로 마이크가
     *  닫힌 동안은 captureDevMedia 가 없어 실패하는데, 한 블록에 묶으면 뒤 축이 조용히 유실된다
     *  (실측: 발언 녹취 RMS 가 mic 게인과 무관 — 미적용). 또 snd dev (재)오픈마다 bridge slot0
     *  포트가 재생성돼 레벨이 초기화되므로, 캡처 게이트 전환([setCaptureEnabled])·재오픈
     *  ([bounceSndDev]) 직후 재적용이 필수.
     *
     *  ⚠ 캡처 장치는 `PJSUA_SND_DEV_NO_IMMEDIATE_OPEN` 으로 **지연 개방**된다 — 게이트를 연
     *  직후엔 captureDevMedia 가 아직 없어 mic 축이 실패하고, 재시도가 없으면 그 발언 내내
     *  게인이 빠진 원음이 나간다(서버 녹취 RMS 비교로 실측). 게이트가 열린 상태에서 mic 축이
     *  실패하면 장치가 열릴 때까지 짧게 재시도한다. */
    private fun applyDeviceAudioBoost(retriesLeft: Int = MIC_BOOST_RETRY_MAX) {
        if (!PjLib.booted) return
        val adm = runCatching { PjLib.ep.audDevManager() }.getOrNull() ?: return
        runCatching { adm.playbackDevMedia.adjustTxLevel(boostSpk) }
        val micOk = runCatching { adm.captureDevMedia.adjustRxLevel(boostMic) }.isSuccess
        if (micOk) {
            if (retriesLeft < MIC_BOOST_RETRY_MAX)
                Log.i(TAG, "mic boost=$boostMic 적용 (지연 개방 재시도 ${MIC_BOOST_RETRY_MAX - retriesLeft}회)")
            return
        }
        if (!captureEnabled) return          // 게이트가 닫힌 상태 — 열릴 때 setCaptureEnabled 가 다시 건다
        if (retriesLeft <= 0) {
            Log.w(TAG, "mic boost=$boostMic 미적용 — 캡처 장치 개방 대기 초과")
            return
        }
        h.postDelayed({
            runCatching {
                if (PjLib.booted) PjLib.ensureThread("pj-ctl")
                applyDeviceAudioBoost(retriesLeft - 1)
            }
        }, MIC_BOOST_RETRY_MS)
    }

    @Volatile private var boostSpk = 1f
    @Volatile private var boostMic = 1f

    /**
     * 임의 SIP 요청 송신 (affiliation 은 method="PUBLISH"). body/헤더는 호출자(ptt-client)가 규격대로 구성.
     * @param targetUri Request-URI (예: 그룹 `sip:group@domain`)
     */
    fun sendRequest(
        method: String,
        targetUri: String,
        contentType: String?,
        body: String?,
        headers: Map<String, String> = emptyMap(),
        token: Long = 0,
    ) = onCtl {
        val acc = account ?: return@onCtl
        val tx = SipTxOption().apply {
            this.targetUri = targetUri
            if (contentType != null) this.contentType = contentType
            if (body != null) this.msgBody = body
            if (headers.isNotEmpty()) this.headers = headers.toSipHeaders()
        }
        acc.sendRequest(SendRequestParam().apply { this.method = method; txOption = tx; userData = token })
    }

    /**
     * conference 구독(RFC 4575 참가자 로스터) 시작·갱신·해지 — 그룹 AoR 로 SUBSCRIBE.
     *
     * native(pjproject CIMS 패치 §2-13)가 이 요청을 가로채 evsub 기반 구독으로 만든다.
     * 따라서 구독 생성·**in-dialog 갱신**(같은 Call-ID/양측 tag/CSeq+1)·종료·
     * Subscription-State 해석·매칭 없는 NOTIFY 의 481 응답은 전부 스택이 담당하며,
     * 앱은 "언제 어느 그룹을 구독할지"만 정한다.
     *
     * ⚠️ 단발 트랜잭션이 아니므로 결과가 [sendReqResults] 로 오지 않는다 — 구독 성립의
     *  확인 신호는 **NOTIFY 도착**이다. NOTIFY 본문은 [incomingMessage] 로 올라온다
     *  (contentType=`application/conference-info+xml`, fromUri=그룹 AoR=conference focus).
     *  Event/Accept/Expires 헤더는 스택이 생성하므로 여기서 싣는 값은 의도 전달용이다.
     *
     * @param expiresSec 0 이면 구독 해지(SUBSCRIBE Expires: 0).
     */
    fun subscribeConference(groupUri: String, expiresSec: Int = CONF_SUB_EXPIRES_SEC) = sendRequest(
        method = "SUBSCRIBE",
        targetUri = groupUri,
        contentType = null,
        body = null,
        headers = mapOf("Event" to "conference", "Expires" to "$expiresSec"),
    )

    /** GMS/CMS 문서 변경 구독 (RFC 5875 xcap-diff) — 대상은 서버 PSI(`sip:gms_psi@domain`).
     *  conference 와 같은 native evsub 경로를 탄다(빌드 패치가 두 패키지를 함께 등록).
     *  NOTIFY 본문은 "어느 문서가 바뀌었고 새 ETag 는 무엇"뿐이라, 실제 내용은 앱이 XCAP
     *  HTTP GET 으로 따로 가져와야 한다.
     *
     * @param expiresSec 0 이면 구독 해지.
     */
    fun subscribeXcapDiff(psiUri: String, expiresSec: Int = CONF_SUB_EXPIRES_SEC) = sendRequest(
        method = "SUBSCRIBE",
        targetUri = psiUri,
        contentType = null,
        body = null,
        headers = mapOf("Event" to "xcap-diff", "Expires" to "$expiresSec"),
    )

    /**
     * 키업 그룹 INVITE — multipart 본문(mcptt-info + resource-lists, ptt-client 가 규격대로 구성) +
     * SDP 에 `m=application` floor 라인 주입. 응답 SDP 의 floor 포트는 [floorRemote] 로 학습.
     * @param applicationSdp 주입할 floor SDP 라인(예: "m=application <port> UDP MCPTT\r\nc=IN IP4 ..\r\na=floorid:0 mstrm:audio")
     */
    fun makeGroupCall(
        groupUri: String,
        parts: List<SipBodyPart>,
        applicationSdp: String,
        fullDuplex: Boolean = false,
        onCallId: ((Int) -> Unit)? = null,
    ) = onCtl {
        val acc = account ?: return@onCtl
        halfDuplex = !fullDuplex
        val call = CimsCall(this, acc)
        call.pendingAppSdp = applicationSdp
        val prm = CallOpParam(true).apply {
            opt.audioCount = 1L
            opt.videoCount = 0L
            if (parts.isNotEmpty()) {
                txOption.multipartContentType = SipMediaType().apply { type = "multipart"; subType = "mixed" }
                txOption.multipartParts = parts.toMultipart()
            }
        }
        call.makeCall(groupUri, prm)
        calls[call.id] = call
        // 발신 call id 통지 — 호출자(PTT 1:1)가 세션에 즉시 바인딩(remote URI 문자열 매칭 불요).
        onCallId?.invoke(call.id)
    }

    /**
     * MCData MSRP 발신 INVITE (TS 24.282 §9.2.3 SDS over media plane) — pjsua 생성 SDP(m=audio)에
     * `m=message TCP/MSRP` 섹션([msrpSdp])을 주입해 송신. 호 상태는 [msrpEvents] 로만 흐른다.
     * 서버(CSP)는 오디오를 포트≠0 + a=inactive 로 응답(계약)하고 200 의 a=path 가
     * [MsrpEvent.PathReady] 로 학습된다.
     */
    fun makeMsrpInvite(
        targetUri: String,
        msrpSdp: String,
        headers: Map<String, String> = emptyMap(),
    ) = onCtl {
        val acc = account ?: run {
            _msrpEvents.tryEmit(MsrpEvent.Closed(-1, 0, "not registered")); return@onCtl
        }
        val call = CimsCall(this, acc)
        call.msrpMode = true
        call.pendingMsrpSdp = msrpSdp
        val prm = CallOpParam(true).apply {
            opt.audioCount = 1L
            opt.videoCount = 0L
            if (headers.isNotEmpty()) txOption.headers = headers.toSipHeaders()
        }
        runCatching { call.makeCall(targetUri, prm) }
            .onSuccess {
                calls[call.id] = call
                _msrpEvents.tryEmit(MsrpEvent.Started(call.id, targetUri))
            }
            .onFailure {
                Log.w(TAG, "makeMsrpInvite failed: ${it.message}")
                _msrpEvents.tryEmit(MsrpEvent.Closed(-1, 0, it.message ?: "invite error"))
            }
    }

    /**
     * 서버발 MSRP 배포 INVITE 수락(UAS) — [answerSdp](완전한 answer SDP)로 응답
     * (`CallOpParam.sdp` = answer_with_sdp). pjsua UAS 는 착신 처리 시점에 answer SDP 를
     * 미리 생성해 두므로 `onCallSdpCreated` 패치로는 늦는다(m=message 가 포트 0 으로 나감 —
     * 실기기 확인). 진행은 [msrpEvents] 로 흐른다.
     */
    fun acceptMsrpCall(callId: Int, answerSdp: String) = onCtl {
        calls[callId]?.answer(
            CallOpParam(true).apply {
                statusCode = pjsip_status_code.PJSIP_SC_OK
                opt.audioCount = 1L
                opt.videoCount = 0L
                sdp = SdpSession().apply { wholeSdp = answerSdp }
            },
        )
    }

    /** MSRP 배포 INVITE 거절/정리 — 협상 전이면 486, 이후면 BYE (pjsua hangup 이 자동 판별). */
    fun rejectMsrpCall(callId: Int) = onCtl { calls[callId]?.hangup(CallOpParam()) }

    /**
     * in-dialog re-INVITE — multipart 본문(mcptt-info) 교체 송신. MCPTT 긴급/임박 상태의
     * 통화 중 상향·하향(TS 24.379)에 사용. SDP 는 재협상되며 floor(m=application) 라인은
     * 호별 [CimsCall.pendingAppSdp] 로 재주입된다.
     */
    fun reinviteWithBody(callId: Int, parts: List<SipBodyPart>) = onCtl {
        calls[callId]?.reinvite(
            CallOpParam(true).apply {
                opt.audioCount = 1L
                opt.videoCount = 0L
                if (parts.isNotEmpty()) {
                    txOption.multipartContentType = SipMediaType().apply { type = "multipart"; subType = "mixed" }
                    txOption.multipartParts = parts.toMultipart()
                }
            },
        )
    }

    // ── 콜백 진입점 (CimsAccount/CimsCall 에서 호출) ──

    internal fun onRemoteFloorLearned(callId: Int, ip: String, port: Int) {
        _floorRemote.value = Triple(callId, ip, port)
    }

    internal fun onMsrpPathLearned(callId: Int, path: String) {
        _msrpEvents.tryEmit(MsrpEvent.PathReady(callId, path))
    }

    internal fun onEmergencyUpgradeDenied(callId: Int) {
        _emergencyDenied.tryEmit(callId)
    }

    internal fun onSessionEmergencyAdvertised(callId: Int, active: Boolean) {
        _sessionEmergency.tryEmit(callId to active)
    }

    /** MSRP 호 상태 — [dispatchCallState] 와 분리(전역 _call 미접촉, 호 수명만 관리). */
    internal fun dispatchMsrpCallState(callId: Int, s: CallState) {
        when (s) {
            is CallState.Active -> _msrpEvents.tryEmit(MsrpEvent.Answered(callId))
            is CallState.Disconnected -> {
                calls.remove(callId)?.let { runCatching { it.delete() } }
                _msrpEvents.tryEmit(MsrpEvent.Closed(callId, s.code, s.reason))
            }
            else -> Unit
        }
    }

    internal fun dispatchMsrpIncoming(call: CimsCall, from: String, inviteMsg: String) {
        calls[call.id] = call
        _msrpEvents.tryEmit(MsrpEvent.Incoming(call.id, from, inviteMsg))
    }

    internal fun dispatchSendReqResult(
        token: Long,
        method: String,
        code: Int,
        reason: String,
        etag: String? = null,
    ) {
        _sendReqResults.tryEmit(SendReqResult(token, method, code, reason, etag))
    }

    internal fun dispatchReg(active: Boolean, code: Int, reason: String) {
        _reg.value = when {
            active && code in 200..299 -> RegState.Registered(code)
            !active && code in 200..299 -> RegState.Unregistered      // de-REGISTER 200
            else -> RegState.Failed("$code $reason")
        }
    }

    internal fun dispatchIncoming(
        call: CimsCall,
        from: String,
        video: Boolean,
        mcptt: Boolean = false,
        emergency: Boolean = false,
        privateCall: Boolean = false,
        callerId: String = "",
        noFloorCtrl: Boolean = false,
    ) {
        calls[call.id] = call
        _call.value = CallState.Incoming(call.id, from, video, mcptt, emergency, privateCall, callerId, noFloorCtrl)
    }

    internal fun dispatchConferenceInfo(callId: Int, xml: String) {
        _conferenceInfo.tryEmit(callId to xml)
    }

    internal fun dispatchCallState(callId: Int, s: CallState) {
        _call.value = s
        if (s is CallState.Disconnected) {
            calls.remove(callId)?.let { runCatching { it.delete() } }
            onCtl { if (calls.isEmpty()) stopPreview() }        // 마지막 호 종료 → 프리뷰 정리
        }
    }

    internal fun dispatchInstantMessage(fromUri: String, contentType: String, body: String) {
        _incomingMessage.tryEmit(ImMessage(fromUri, contentType, body))
    }

    fun shutdown() = onCtl {
        stopPreview()
        calls.values.forEach { runCatching { it.delete() } }
        calls.clear()
        account?.let { runCatching { it.delete() } }
        account = null
        PjLib.shutdown()
        // 컨트롤러는 shutdown 후 폐기·재생성되는 계약(ensureRegistered/stopSip) — 전용 pj-ctl
        // 스레드도 함께 마감해 로그아웃→재로그인 사이클마다 스레드가 누적되지 않게 한다.
        ctl.quitSafely()
    }

    // ── config → pjsua2 매핑 (설계서 §3.2, Digest 계약) ──

    private fun buildAccountConfig(c: SipAccountConfig): AccountConfig {
        val ac = AccountConfig()
        ac.idUri = if (c.displayName.isBlank()) c.aor                 // sip:msisdn@domain (공개 ID)
        else "\"${c.displayName}\" <${c.aor}>"

        val tp = c.transport.name.lowercase()                        // udp/tcp/tls — 설정 추종
        ac.regConfig.registrarUri = "sip:${c.domain}:${c.serverPort};transport=$tp"
        ac.regConfig.timeoutSec = c.expiresSec.toLong()              // 희망값(서버 200 OK Expires 추종)
        ac.regConfig.registerOnAdd = true

        // ── 등록 keepalive 보강(doze/슬립·네트워크 단절 후 자동 복구) ──
        ac.regConfig.retryIntervalSec = 30                           // 등록 실패 시 재시도 주기
        ac.regConfig.firstRetryIntervalSec = 5                       // 첫 재시도는 빠르게
        ac.regConfig.randomRetryIntervalSec = 5                      // 다수 단말 동시 재시도 분산
        ac.regConfig.delayBeforeRefreshSec = 10                      // 만료 전 미리 갱신(여유)
        ac.regConfig.dropCallsOnFail = false                         // 등록 일시 실패로 통화 끊지 않음

        // NAT 바인딩 유지 — UDP keep-alive 로 서버 도달성 유지(슬립 후 단방향 음성/미수신 방지)
        ac.natConfig.udpKaIntervalSec = 15                           // 15초 CRLF keep-alive
        ac.natConfig.contactRewriteUse = 1                           // 서버가 본 공인주소로 Contact 재작성
        ac.natConfig.viaRewriteUse = 1
        ac.natConfig.sipStunUse = pjsua_stun_use.PJSUA_STUN_USE_DISABLED    // STUN 서버 없음
        ac.natConfig.mediaStunUse = pjsua_stun_use.PJSUA_STUN_USE_DISABLED

        // Digest: username = IMPI(IMSI@domain), realm="*"(challenge realm echo — 도메인 오타/불일치 무한401 회피, §3.3)
        // 자료는 H(A1) 우선(PJSIP_CRED_DATA_DIGEST, sip_access_security.md §4.7) — 평문 비번 없이
        // response 를 계산하므로 서버 passwd 소거 후에도 인증된다. H(A1) 은 서버 realm 에 결박된
        // 값이라 challenge realm 을 따라가지 못한다 — 없을 때만 평문 cred(그때 계산)로 폴백.
        val hasHa1 = c.sipHa1.isNotBlank()
        ac.sipConfig.authCreds.add(
            if (hasHa1) {
                AuthCredInfo("digest", "*", c.digestUsername,
                    pjsip_cred_data_type.PJSIP_CRED_DATA_DIGEST, c.sipHa1)
            } else {
                AuthCredInfo("digest", "*", c.digestUsername,
                    pjsip_cred_data_type.PJSIP_CRED_DATA_PLAIN_PASSWD, c.password)
            },
        )
        Log.i(TAG, "auth cred: ${if (hasHa1) "ha1(digest)" else "plain-passwd"} user=${c.digestUsername}")

        // Contact 부가 파라미터(capability feature tag 등) — 서버가 MSRP 배포 대상 판정에 사용
        if (contactParams.isNotBlank()) ac.sipConfig.contactParams = contactParams

        // 도메인 DNS 미해석 회피: 실제 서버 IP:port 로 route 강제(;lr)
        ac.sipConfig.proxies.add("sip:${c.serverHost}:${c.serverPort};transport=$tp;lr")

        // ── 영상통화 캡처 설정 ──
        // autoTransmitOutgoing 기본 false → 명시하지 않으면 m=video sendrecv 로 협상돼도 카메라
        // 캡처(발신 영상)가 시작되지 않는다. 켜야 상대가 우리 카메라를 받고, 동시에 그 열린
        // 카메라(PjCamera2)에 셀프뷰 surface 를 붙일 수 있다([setPreviewSurface]).
        ac.videoConfig.autoTransmitOutgoing = true
        ac.videoConfig.autoShowIncoming = false                      // 수신 렌더는 앱이 setVideoSurface 로 직접 결선
        ac.videoConfig.defaultCaptureDevice = frontCaptureDev()      // 셀프뷰=전면 카메라
        return ac
    }

    /** 전면 카메라 캡처 장치 우선 선택(이름 "front" 매칭), 없으면 기본 캡처 장치. */
    private fun frontCaptureDev(): Int = runCatching {
        val devs = PjLib.ep.vidDevManager().enumDev2()
        Log.i(TAG, "vidDev count=${devs.size}")
        var fallback = pjmedia_vid_dev_std_index.PJMEDIA_VID_DEFAULT_CAPTURE_DEV
        for (i in 0 until devs.size) {
            val d = devs[i]
            Log.i(TAG, "vidDev[$i] id=${d.id} dir=${d.dir} name='${d.name}' drv='${d.driver}'")
            if ((d.dir and pjmedia_dir.PJMEDIA_DIR_CAPTURE) == 0) continue
            if (d.name.contains("front", ignoreCase = true)) return@runCatching d.id
            if (fallback == pjmedia_vid_dev_std_index.PJMEDIA_VID_DEFAULT_CAPTURE_DEV) fallback = d.id
        }
        Log.i(TAG, "frontCaptureDev -> $fallback")
        fallback
    }.onFailure { Log.w(TAG, "frontCaptureDev enum failed: ${it.message}") }
        .getOrDefault(pjmedia_vid_dev_std_index.PJMEDIA_VID_DEFAULT_CAPTURE_DEV)

    private fun Map<String, String>.toSipHeaders(): SipHeaderVector = SipHeaderVector().also { v ->
        forEach { (k, value) -> v.add(SipHeader().apply { hName = k; hValue = value }) }
    }

    private fun List<SipBodyPart>.toMultipart(): SipMultipartPartVector = SipMultipartPartVector().also { v ->
        forEach { p ->
            v.add(SipMultipartPart().apply {
                contentType = SipMediaType().apply { type = p.type; subType = p.subType }
                body = p.body
            })
        }
    }

    companion object {
        private const val TAG = "SipController"

        /** 캡처 장치 지연 개방(PJSUA_SND_DEV_NO_IMMEDIATE_OPEN) 대기 — mic boost 재시도 간격/횟수. */
        private const val MIC_BOOST_RETRY_MS = 120L
        private const val MIC_BOOST_RETRY_MAX = 8

        /** 오디오 출력 라우팅 상수 — [setAudioRoute]. */
        const val AUDIO_ROUTE_DEFAULT = 0   // 자동(이어폰 연결 시 이어폰)
        const val AUDIO_ROUTE_EARPIECE = 1  // 수화구
        const val AUDIO_ROUTE_SPEAKER = 2   // 외장 스피커

        /** conference 구독 희망 수명(초) — 갱신은 스택이 만료 전에 자동 수행. */
        const val CONF_SUB_EXPIRES_SEC = 3600

        /** conference 로스터 NOTIFY 본문 MIME (RFC 4575). */
        const val CONF_INFO_MIME = "application/conference-info+xml"
    }
}

/** multipart INVITE 본문 한 파트 (예: type="application", subType="vnd.3gpp.mcptt-info+xml"). */
data class SipBodyPart(val type: String, val subType: String, val body: String)
