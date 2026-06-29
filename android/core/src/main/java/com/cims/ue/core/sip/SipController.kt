package com.cims.ue.core.sip

import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import com.cims.ue.core.config.SipAccountConfig
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.pjsip.pjsua2.AccountConfig
import org.pjsip.pjsua2.AuthCredInfo
import org.pjsip.pjsua2.CallOpParam
import org.pjsip.pjsua2.SendRequestParam
import org.pjsip.pjsua2.SipHeader
import org.pjsip.pjsua2.SipHeaderVector
import org.pjsip.pjsua2.SipMediaType
import org.pjsip.pjsua2.SipMultipartPart
import org.pjsip.pjsua2.SipMultipartPartVector
import org.pjsip.pjsua2.SipTxOption
import org.pjsip.pjsua2.pjsip_status_code
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

    private val ctl = HandlerThread("pj-ctl").apply { start() }
    private val h = Handler(ctl.looper)

    private var account: CimsAccount? = null                       // 강참조
    private val calls = ConcurrentHashMap<Int, CimsCall>()         // callId → Call 강참조

    @Volatile
    var videoEnabled = false                                       // M1.3 토글

    // ── PTT(M2) 지원 ──
    /** 반이중(PTT): true 면 mic 는 floor GRANT 시에만 송신, spk 는 상시 청취. (VoLTE=false 전이중) */
    @Volatile var halfDuplex = false

    /** 그룹콜 SDP 에 주입할 floor 라인(`m=application …`). makeGroupCall 직전 설정. */
    @Volatile var injectApplicationSdp: String? = null

    /** 수신 SDP 에서 학습한 CMP floor 목적지(ip,port). FloorClient 가 여기로 송신. */
    private val _floorRemote = MutableStateFlow<Pair<String, Int>?>(null)
    val floorRemote: StateFlow<Pair<String, Int>?> = _floorRemote.asStateFlow()

    private fun onCtl(block: () -> Unit) = h.post {
        runCatching {
            PjLib.ensureThread("pj-ctl")
            block()
        }.onFailure {
            Log.e(TAG, "pj-ctl error", it)
            _reg.value = RegState.Failed(it.message ?: "error")
        }
    }

    // ── 외부 명령 ──

    fun register() = onCtl {
        PjLib.boot()
        CodecConfig.apply(PjLib.ep)
        _reg.value = RegState.Registering
        val acc = CimsAccount(this).also { account = it }
        acc.create(buildAccountConfig(config))                    // registerOnAdd=true → REGISTER 발신
    }

    fun unregister() = onCtl { account?.setRegistration(false) }  // de-REGISTER(expires=0)

    fun makeCall(dstNumber: String) = onCtl {
        val acc = account ?: run {
            _call.value = CallState.Disconnected(-1, 0, "not registered"); return@onCtl
        }
        halfDuplex = false                                        // VoLTE = 전이중
        injectApplicationSdp = null
        val call = CimsCall(this, acc)
        val prm = CallOpParam(true).apply {
            opt.audioCount = 1L
            opt.videoCount = if (videoEnabled) 1L else 0L
        }
        call.makeCall("sip:$dstNumber@${config.domain}", prm)
        calls[call.id] = call
    }

    fun answer(callId: Int) = onCtl {
        calls[callId]?.answer(CallOpParam().apply { statusCode = pjsip_status_code.PJSIP_SC_OK })
    }

    fun reject(callId: Int) = onCtl {
        calls[callId]?.answer(CallOpParam().apply { statusCode = pjsip_status_code.PJSIP_SC_BUSY_HERE })
    }

    fun hangup(callId: Int) = onCtl { calls[callId]?.hangup(CallOpParam()) }

    // ── PTT(M2): affiliation PUBLISH / 그룹콜(multipart+floor SDP) / 반이중 mic ──

    /** 마이크 송신 토글 (PTT floor GRANT→true, RELEASE/REVOKE→false). */
    fun setMicEnabled(callId: Int, on: Boolean) = onCtl { calls[callId]?.setMic(on) }

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
    ) = onCtl {
        val acc = account ?: return@onCtl
        val tx = SipTxOption().apply {
            this.targetUri = targetUri
            if (contentType != null) this.contentType = contentType
            if (body != null) this.msgBody = body
            if (headers.isNotEmpty()) this.headers = headers.toSipHeaders()
        }
        acc.sendRequest(SendRequestParam().apply { this.method = method; txOption = tx })
    }

    /**
     * 키업 그룹 INVITE — multipart 본문(mcptt-info + resource-lists, ptt-client 가 규격대로 구성) +
     * SDP 에 `m=application` floor 라인 주입. 응답 SDP 의 floor 포트는 [floorRemote] 로 학습.
     * @param applicationSdp 주입할 floor SDP 라인(예: "m=application <port> UDP MCPTT\r\nc=IN IP4 ..\r\na=floorid:0 mstrm:audio")
     */
    fun makeGroupCall(
        groupUri: String,
        parts: List<SipBodyPart>,
        applicationSdp: String,
    ) = onCtl {
        val acc = account ?: return@onCtl
        halfDuplex = true
        injectApplicationSdp = applicationSdp
        _floorRemote.value = null
        val call = CimsCall(this, acc)
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
    }

    // ── 콜백 진입점 (CimsAccount/CimsCall 에서 호출) ──

    internal fun onRemoteFloorLearned(ip: String, port: Int) {
        _floorRemote.value = ip to port
    }

    internal fun dispatchReg(active: Boolean, code: Int, reason: String) {
        _reg.value = when {
            active && code in 200..299 -> RegState.Registered(code)
            !active && code in 200..299 -> RegState.Unregistered      // de-REGISTER 200
            else -> RegState.Failed("$code $reason")
        }
    }

    internal fun dispatchIncoming(call: CimsCall, from: String) {
        calls[call.id] = call
        _call.value = CallState.Incoming(call.id, from)
    }

    internal fun dispatchCallState(callId: Int, s: CallState) {
        _call.value = s
        if (s is CallState.Disconnected) {
            calls.remove(callId)?.let { runCatching { it.delete() } }
        }
    }

    fun shutdown() = onCtl {
        calls.values.forEach { runCatching { it.delete() } }
        calls.clear()
        account?.let { runCatching { it.delete() } }
        account = null
        PjLib.shutdown()
    }

    // ── config → pjsua2 매핑 (설계서 §3.2, Digest 계약) ──

    private fun buildAccountConfig(c: SipAccountConfig): AccountConfig {
        val ac = AccountConfig()
        ac.idUri = if (c.displayName.isBlank()) c.aor                 // sip:msisdn@domain (공개 ID)
        else "\"${c.displayName}\" <${c.aor}>"

        ac.regConfig.registrarUri = "sip:${c.domain}:${c.serverPort};transport=udp"
        ac.regConfig.timeoutSec = c.expiresSec.toLong()              // 희망값(서버 200 OK Expires 추종)
        ac.regConfig.registerOnAdd = true

        // Digest: username = IMPI(IMSI@domain), realm="*"(challenge realm echo — 도메인 오타/불일치 무한401 회피, §3.3)
        ac.sipConfig.authCreds.add(
            AuthCredInfo("digest", "*", c.digestUsername, 0, c.password),
        )

        // 도메인 DNS 미해석 회피: 실제 서버 IP:port 로 route 강제(;lr)
        ac.sipConfig.proxies.add("sip:${c.serverHost}:${c.serverPort};transport=udp;lr")
        return ac
    }

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

    private companion object {
        const val TAG = "SipController"
    }
}

/** multipart INVITE 본문 한 파트 (예: type="application", subType="vnd.3gpp.mcptt-info+xml"). */
data class SipBodyPart(val type: String, val subType: String, val body: String)
