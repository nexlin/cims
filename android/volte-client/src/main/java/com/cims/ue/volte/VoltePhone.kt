// VoLTE 전화 세션 — 단말 SDK(:cimsue) 위 (docs/design/features/ue_sdk.md §5.3 P2)
//
// SipService 가 쓰던 기존 래퍼(SipController)의 계약 — 등록·호 상태 StateFlow, 호 명령, 영상, 캡처 게이트, 문자(MESSAGE) — 을
// 그대로 내고 속은 SDK 로 한다. 화면(MainActivity)은 SipModels(CallState·RegState)만 보므로 바뀌지 않는다.
// 판단(Digest·SRTP·sec-agree·호 상태 매핑·AGC)은 코어에 있고 여기는 투영과 배선만 한다.
package com.cims.ue.volte

import android.content.Context
import android.util.Log
import android.view.Surface
import com.cims.ue.core.config.SipAccountConfig
import com.cims.ue.core.sip.CallState
import com.cims.ue.core.sip.CimsTrustStore
import com.cims.ue.core.sip.ImMessage
import com.cims.ue.core.sip.RegState
import com.cims.ue.core.sip.SendReqResult
import com.cims.ue.sdk.Account
import com.cims.ue.sdk.AccountConfig
import com.cims.ue.sdk.AuthScheme
import com.cims.ue.sdk.CallOptions
import com.cims.ue.sdk.CimsUe
import com.cims.ue.sdk.EngineConfig
import com.cims.ue.sdk.MediaSecurity
import com.cims.ue.sdk.Transport
import com.cims.ue.sdk.VideoRequestEvent
import com.cims.ue.sdk.VideoRequestEventKind
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import com.cims.ue.sdk.CallDir as SdkCallDir
import com.cims.ue.sdk.CallInfo as SdkCallInfo
import com.cims.ue.sdk.CallState as SdkCallState
import com.cims.ue.sdk.RegInfo as SdkRegInfo
import com.cims.ue.sdk.RegState as SdkRegState

/**
 * 프로세스당 하나(SipService 소유). 명령은 호출 순서대로 한 줄에서 실행한다 — 등록 뒤 발신 같은 순서가 뒤집히지 않게
 * (코어 명령은 제어 스레드를 기다리는 동기 호출이라 메인 스레드에서 부르지 않는다, CimsUe 머리말).
 */
class VoltePhone(
    private val context: Context,
    private val config: SipAccountConfig,
    private val userAgent: String,
    private val instanceId: String?,
) {
    @OptIn(ExperimentalCoroutinesApi::class)
    private val serial = Dispatchers.IO.limitedParallelism(1)
    private val scope = CoroutineScope(SupervisorJob() + serial +
        kotlinx.coroutines.CoroutineExceptionHandler { _, e -> Log.e(TAG, "phone scope 예외", e) })
    private val ue = CimsUe()
    @Volatile private var account: Account? = null

    private val _reg = MutableStateFlow<RegState>(RegState.Idle)
    val regState: StateFlow<RegState> = _reg.asStateFlow()

    private val _call = MutableStateFlow<CallState>(CallState.Null)
    /** 마지막 호 이벤트 — 기존 래퍼와 같은 의미(호마다 오는 이벤트를 하나에 투영). */
    val callState: StateFlow<CallState> = _call.asStateFlow()

    private val _results = MutableSharedFlow<SendReqResult>(extraBufferCapacity = 64, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    /** [sendRequest] 의 최종 응답 — token 은 호출자가 준 값(문자 말풍선 상관). */
    val sendReqResults: SharedFlow<SendReqResult> = _results.asSharedFlow()

    private val _messages = MutableSharedFlow<ImMessage>(extraBufferCapacity = 64, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    val incomingMessage: SharedFlow<ImMessage> = _messages.asSharedFlow()

    /** 지금 호의 영상 여부 — 발신은 발신 전 토글(M1.3), 착신은 응답 방식(영상/음성)이 정하고, 통화 중에는 협상 결과([callVideo])를
     *  따른다(통화 중 영상 전환). 근접 센서 화면 꺼짐(SipService)도 이 값을 본다. */
    @Volatile var videoEnabled = false

    private val _callVideo = MutableStateFlow(false)
    /** 통화 중인 호의 영상 협상 결과(코어 CallInfo.video) — 통화 중 영상 전환(요청 수락·상대의 영상 제거)이 바꾼다. 영상 칸·근접 센서가 본다. */
    val callVideo: StateFlow<Boolean> = _callVideo.asStateFlow()

    private val _videoRequests = MutableSharedFlow<VideoRequestEvent>(extraBufferCapacity = 16, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    /** 통화 중 영상 전환(RFC 3264 §8.1) — 상대의 요청(RECEIVED → [answerVideoRequest])·내 요청의 결과. 명령이 곧바로 실패해도 FAILED 로 온다. */
    val videoRequests: SharedFlow<VideoRequestEvent> = _videoRequests.asSharedFlow()

    /** 마지막으로 본 호 — 카메라 전환 대상. */
    @Volatile private var lastCallId = -1

    // 요청 상관 — 코어 token 은 요청을 보낸 뒤에야 안다. 그 사이 최종 응답이 먼저 오면 잠시 두었다가 짝을 맞춘다.
    private val tokLock = Any()
    private val appTokenOf = HashMap<Long, Long>()                 // 코어 token → 호출자 token
    private val early = HashMap<Long, SendReqResult>()             // 짝을 모르는 채 온 결과(코어 token 키)

    init {
        scope.launch { ue.log.collect { Log.println(pjPriority(it.level), "PJ", it.message.trimEnd()) } }
        scope.launch { ue.regState.collect { onReg(it) } }
        scope.launch { ue.incomingCall.collect { onCall(it, incoming = true) } }
        scope.launch { ue.callState.collect { onCall(it, incoming = false) } }
        scope.launch { ue.callMedia.collect { onMedia(it) } }
        scope.launch { ue.videoRequest.collect { _videoRequests.tryEmit(it) } }
        scope.launch { ue.requestResult.collect { r -> onResult(SendReqResult(r.token, r.method, r.code, r.reason, r.etag.ifBlank { null })) } }
        scope.launch { ue.message.collect { m -> _messages.tryEmit(ImMessage(m.fromUri, m.contentType, m.body)) } }
    }

    // ── 등록 ──
    fun register() = scope.launch {
        if (account != null) return@launch
        _reg.value = RegState.Registering
        val started = ue.start(
            EngineConfig(
                userAgent = userAgent,
                logLevel = 4,
                tlsCaPem = CimsTrustStore.CA_BUNDLE,            // APK 동봉 앵커(sip_tls_signaling.md §8.5)
                tlsVerifyServer = true,
                udpNoTcpSwitch = config.udpNoTcpSwitch,
            ),
            context,                                              // 카메라 열거(기동 전에 CameraManager)
        )
        if (!started.ok) { _reg.value = RegState.Failed("${started.code} ${started.reason}"); return@launch }
        val acc = ue.addAccount(accountConfig(config)).getOrNull()
            ?: run { _reg.value = RegState.Failed("addAccount"); return@launch }
        account = acc
        val r = acc.register()
        if (!r.ok) _reg.value = RegState.Failed("${r.code} ${r.reason}")
    }

    /** 즉시 재-REGISTER — 망 복귀·포그라운드 복귀. */
    fun reregister() = scope.launch {
        val acc = account ?: return@launch
        _reg.value = RegState.Registering
        acc.refreshRegistration()
    }

    fun unregister() = scope.launch { account?.unregister() }
    fun hasAccount(): Boolean = account != null

    // ── 호 ──
    fun makeCall(dstNumber: String) = scope.launch {
        val acc = account ?: run { _call.value = CallState.Disconnected(-1, 0, "not registered"); return@launch }
        val r = acc.dial("sip:$dstNumber@${config.domain}", CallOptions(video = videoEnabled))
        if (!r.ok) _call.value = CallState.Disconnected(-1, r.code, r.reason)
    }

    /** 착신 응답. [withVideo]=true 면 영상까지(상대가 m=video 를 offer 한 경우). */
    fun answer(callId: Int, withVideo: Boolean = false) = scope.launch {
        videoEnabled = withVideo   // 음성 응답이면 앞 영상 호의 값을 지운다
        ue.call(callId).answer(CallOptions(video = withVideo))
    }

    fun reject(callId: Int) = scope.launch { ue.call(callId).reject(486) }
    fun hangup(callId: Int) = scope.launch { ue.call(callId).hangup() }
    fun setMuted(callId: Int, on: Boolean) = scope.launch { ue.call(callId).setMuted(on) }

    /** 캡처 게이트 — PTT 발언 동안 마이크 양보(재생 유지). */
    fun setCaptureEnabled(on: Boolean) = scope.launch { ue.setCaptureEnabled(on) }

    // ── 영상 ──
    /** 통화 중 영상 전환 — on = 영상 추가 요청(상대가 [수락]하면 [callVideo]), off = 영상 제거(묻지 않는다). */
    fun setCallVideo(on: Boolean) = scope.launch {
        val id = lastCallId
        if (id < 0) return@launch
        val r = ue.call(id).setVideo(on)
        if (!r.ok) {
            Log.w(TAG, "setCallVideo($on): ${r.code} ${r.reason}")
            _videoRequests.tryEmit(VideoRequestEvent(VideoRequestEventKind.FAILED, id, r.code, r.reason))
        }
    }
    /** 상대의 영상 전환 요청에 답한다 — accept = 영상으로, false = 음성 그대로. */
    fun answerVideoRequest(callId: Int, accept: Boolean) = scope.launch {
        ue.call(callId).answerVideoRequest(accept).let { if (!it.ok) Log.w(TAG, "answerVideoRequest: ${it.code} ${it.reason}") }
    }

    fun setVideoSurface(surface: Any?) = scope.launch { ue.setVideoSurface(surface as? Surface) }
    fun setPreviewSurface(surface: Any?) { ue.setPreviewSurface(surface as? Surface) }
    fun switchCamera() = scope.launch {
        val id = lastCallId
        if (id >= 0) ue.call(id).switchCamera().let { if (!it.ok) Log.w(TAG, "switchCamera: ${it.code} ${it.reason}") }
    }

    // ── 임의 요청(문자 MESSAGE) ──
    /** [token] = 호출자 상관 키 — 최종 응답이 [sendReqResults] 에 이 값으로 온다. 요청을 못 만들면 곧바로 실패 결과. */
    fun sendRequest(method: String, targetUri: String, contentType: String, body: String, token: Long) = scope.launch {
        val acc = account
        val r = acc?.sendRequest(method, targetUri, contentType, body)
        val coreToken = r?.getOrNull()
        if (coreToken == null) {
            _results.tryEmit(SendReqResult(token, method, r?.code ?: -1, r?.reason ?: "not registered"))
            return@launch
        }
        val pending = synchronized(tokLock) {
            early.remove(coreToken) ?: run { appTokenOf[coreToken] = token; null }
        }
        pending?.let { _results.tryEmit(it.copy(token = token)) }
    }

    private fun onResult(r: SendReqResult) {
        val app = synchronized(tokLock) {
            appTokenOf.remove(r.token) ?: run { early[r.token] = r; null }
        } ?: return
        _results.tryEmit(r.copy(token = app))
    }

    /** 등록 해제·엔진 정지·해제. 이 객체는 다시 쓰지 않는다(SipService 가 새로 만든다). */
    fun shutdown() = scope.launch {
        runCatching { ue.stop() }
        runCatching { ue.close() }
        account = null
        scope.cancel()
    }

    // ── 투영 ──
    private fun onReg(r: SdkRegInfo) {
        _reg.value = when (r.state) {
            SdkRegState.REGISTERED -> RegState.Registered(r.code)
            SdkRegState.REGISTERING -> RegState.Registering
            SdkRegState.UNREGISTERED -> if (r.code in 200..299) RegState.Unregistered else RegState.Idle
            SdkRegState.FAILED -> RegState.Failed("${r.code} ${r.reason}")
        }
    }

    private fun onCall(c: SdkCallInfo, incoming: Boolean) {
        lastCallId = c.callId
        val mapped = when {
            incoming || (c.state == SdkCallState.INCOMING && c.dir == SdkCallDir.INCOMING) ->
                CallState.Incoming(c.callId, c.remoteUri, c.video, c.isMcptt, c.mcptt.emergency, c.mcptt.privateCall)
            c.state == SdkCallState.OUTGOING -> CallState.Outgoing(c.callId, c.remoteUri)
            c.state == SdkCallState.ACTIVE || c.state == SdkCallState.HELD -> CallState.Active(c.callId, c.remoteUri)
            c.state == SdkCallState.DISCONNECTED -> CallState.Disconnected(c.callId, c.lastCode, c.lastReason)
            else -> return
        }
        when (c.state) {
            SdkCallState.ACTIVE -> { videoEnabled = c.video; _callVideo.value = c.video }   // 협상 결과(영상 발신을 음성으로 받았으면 false)
            SdkCallState.DISCONNECTED -> _callVideo.value = false
            else -> {}
        }
        _call.value = mapped
    }

    /** 미디어 결선 — 통화 중(성립 뒤)에는 협상된 영상이 이 호의 영상 여부다(통화 중 영상 전환·상대의 영상 거절). */
    private fun onMedia(c: SdkCallInfo) {
        if (c.callId != lastCallId || c.state != SdkCallState.ACTIVE) return
        videoEnabled = c.video
        _callVideo.value = c.video
    }

    /** 기존 래퍼(`SipController.buildAccountConfig`)와 같은 입력 — 매핑 규칙 자체는 코어(account_map.cpp)가 같다. */
    private fun accountConfig(c: SipAccountConfig): AccountConfig = AccountConfig(
        serverHost = c.serverHost, serverPort = c.serverPort,
        transport = when (c.transport) {
            SipAccountConfig.Transport.TCP -> Transport.TCP
            SipAccountConfig.Transport.TLS -> Transport.TLS
            else -> Transport.UDP
        },
        domain = c.domain, msisdn = c.msisdn, imsi = c.imsi, authId = c.authId,
        displayName = c.displayName, ha1 = c.sipHa1, password = c.password,
        authScheme = if (c.authScheme.equals("aka", ignoreCase = true)) AuthScheme.AKA else AuthScheme.DIGEST,
        akaK = c.akaK, akaOpc = c.akaOpc, akaAmf = c.akaAmf,
        secMechanisms = c.secMechanisms,
        mediaSecurity = when (c.mediaSecurity.lowercase()) {
            "required" -> MediaSecurity.REQUIRED
            "optional" -> MediaSecurity.OPTIONAL
            else -> MediaSecurity.OFF
        },
        expiresSec = c.expiresSec,
        videoAutoTransmit = true,                               // 영상 협상되면 카메라 송신(기존 autoTransmitOutgoing=true)
        autoAnswerMcptt = false,                                // VoLTE 앱은 MCPTT 착신을 받지 않는다
        instanceId = instanceId.orEmpty(),
    )

    private companion object {
        const val TAG = "VoltePhone"

        fun pjPriority(level: Int): Int = when {
            level <= 1 -> Log.ERROR
            level == 2 -> Log.WARN
            else -> Log.INFO
        }

    }
}
