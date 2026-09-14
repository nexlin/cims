package com.cims.ue.core.net

import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import okhttp3.Interceptor
import java.security.cert.X509Certificate

/**
 * 서버 인증서 만료 관측 수집기 — 앱 프로세스당 하나. SIP TLS 와 CSC HTTPS 두 접속의 peer 인증서
 * 만료를 [TlsPeerExpiry] 로 보관하고 StateFlow 로 낸다(설정 화면이 구독).
 *
 * - SIP: [com.cims.ue.core.sip.CimsEndpoint] 가 pjsua2 `onTransportState`(CONNECTED, tlsInfo 있음)에서
 *   [recordSip] 를 부른다. 로그아웃(libDestroy)이면 [clearSip] — 다음 로그인의 관측이 이어진다.
 * - HTTPS: [CimsTls.apply] 가 붙이는 [httpInterceptor] 가 응답의 handshake peer 인증서에서 읽는다.
 *   요청 자체에는 영향을 주지 않는다(읽기 실패는 무시).
 *
 * 값은 마지막 관측으로 덮어쓴다 — 서버가 인증서를 무중단 교체하면 다음 연결에서 새 만료일이 반영된다.
 */
object TlsPeerObserver {

    private const val TAG = "TlsPeer"

    private val _sip = MutableStateFlow<TlsPeerExpiry?>(null)
    /** SIP TLS 접속(CSP)에서 관측한 서버 인증서. TLS 를 안 쓰면 null. */
    val sip: StateFlow<TlsPeerExpiry?> = _sip

    private val _csc = MutableStateFlow<TlsPeerExpiry?>(null)
    /** CSC HTTPS 접속에서 관측한 서버 인증서. */
    val csc: StateFlow<TlsPeerExpiry?> = _csc

    fun recordSip(notAfterEpoch: Long, subject: String, remote: String) {
        if (notAfterEpoch <= 0) return
        val e = TlsPeerExpiry(notAfterEpoch, TlsPeerExpiry.nowEpoch(), subject, remote)
        if (_sip.value != e) Log.i(TAG, "sip peer cert: $remote notAfter=${e.notAfterDate()} (${e.daysLeft()}d)")
        _sip.value = e
    }

    fun clearSip() { _sip.value = null }

    fun recordCsc(notAfterEpoch: Long, subject: String, remote: String) {
        if (notAfterEpoch <= 0) return
        val e = TlsPeerExpiry(notAfterEpoch, TlsPeerExpiry.nowEpoch(), subject, remote)
        if (_csc.value != e) Log.i(TAG, "csc peer cert: $remote notAfter=${e.notAfterDate()} (${e.daysLeft()}d)")
        _csc.value = e
    }

    /** OkHttp 인터셉터 — 응답 handshake 의 첫 peer 인증서(= 서버 leaf) notAfter 를 기록한다. */
    val httpInterceptor: Interceptor = Interceptor { chain ->
        val req = chain.request()
        val resp = chain.proceed(req)
        runCatching {
            val leaf = resp.handshake?.peerCertificates?.firstOrNull() as? X509Certificate
            if (leaf != null) {
                recordCsc(
                    notAfterEpoch = leaf.notAfter.time / 1000L,
                    subject = leaf.subjectX500Principal.name,
                    remote = "${req.url.host}:${req.url.port}",
                )
            }
        }.onFailure { Log.w(TAG, "peer cert read failed: ${it.message}") }
        resp
    }
}
