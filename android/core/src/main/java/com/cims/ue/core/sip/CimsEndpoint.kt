package com.cims.ue.core.sip

import android.util.Log
import com.cims.ue.core.net.TlsPeerObserver
import org.pjsip.pjsua2.Endpoint
import org.pjsip.pjsua2.OnTransportStateParam
import org.pjsip.pjsua2.pjsip_transport_state

/**
 * pjsua2 Endpoint — transport 상태 콜백에서 서버 인증서 만료를 관측한다.
 *
 * SDK 코어 `PjEndpoint::onTransportState`(sdk/core/src/engine.cpp)와 같은 규칙: CONNECTED 이고
 * tlsInfo 가 있으며 remoteCertInfo 의 validityEnd 가 유효할 때만 기록. 검증 실패로 끊긴 연결은
 * CONNECTED 가 아니므로 기록되지 않는다. 콜백은 pjsip 워커 스레드 — pj API 를 부르지 않고 값만
 * 읽어 [TlsPeerObserver] 에 넘긴다.
 */
class CimsEndpoint : Endpoint() {

    override fun onTransportState(prm: OnTransportStateParam) {
        runCatching {
            if (prm.state != pjsip_transport_state.PJSIP_TP_STATE_CONNECTED) return
            val tls = prm.tlsInfo ?: return
            if (tls.isEmpty()) return
            val rc = tls.remoteCertInfo ?: return
            if (rc.isEmpty()) return
            val end = rc.validityEnd?.sec ?: 0
            if (end <= 0) return
            val subject = rc.subjectInfo.orEmpty().ifEmpty { rc.subjectCn.orEmpty() }
            TlsPeerObserver.recordSip(end.toLong(), subject, tls.remoteAddr.orEmpty())
        }.onFailure { Log.w("PjLib", "onTransportState cert read failed: ${it.message}") }
    }
}
