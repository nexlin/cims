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

    // ── 콜백 진입점 (CimsAccount/CimsCall 에서 호출) ──

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

    private companion object {
        const val TAG = "SipController"
    }
}
