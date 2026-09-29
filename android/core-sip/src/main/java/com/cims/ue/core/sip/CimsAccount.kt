package com.cims.ue.core.sip

import android.util.Log
import org.pjsip.pjsua2.Account
import org.pjsip.pjsua2.CallOpParam
import org.pjsip.pjsua2.OnIncomingCallParam
import org.pjsip.pjsua2.OnInstantMessageParam
import org.pjsip.pjsua2.OnRegStateParam
import org.pjsip.pjsua2.OnSendRequestParam
import org.pjsip.pjsua2.pjsip_event_id_e
import org.pjsip.pjsua2.pjsip_status_code

/**
 * `Account` 서브클래스 — 등록/착신 콜백을 [SipController] 로 중계 (설계서 §3.7).
 *
 * ⚠️ SWIG 실측 교정:
 *  - `OnRegStateParam.getCode()` 는 이미 int(설계 스켈레톤의 `.swigValue()` 는 이 빌드에 없음).
 *  - 등록 활성 여부는 `OnRegStateParam` 이 아니라 `getInfo().getRegIsActive()` 에서 읽는다.
 */
class CimsAccount(private val owner: SipController) : Account() {

    override fun onRegState(prm: OnRegStateParam) {
        val active = runCatching { info.regIsActive }.getOrDefault(false)
        owner.dispatchReg(active, prm.code, prm.reason)
        Log.i("CimsAccount", "reg: code=${prm.code} active=$active reason=${prm.reason}")
    }

    /** [SipController.sendRequest] (affiliation PUBLISH 등) 트랜잭션 상태 콜백 — 최종 응답(≥200)만 중계.
     *  같은 트랜잭션이 COMPLETED/TERMINATED 두 번 올 수 있어 수신측이 token 당 1회 처리한다.
     *  응답 원문에서 `SIP-ETag`(RFC 3903)를 뽑아 함께 넘긴다 — 갱신 PUBLISH 의 `SIP-If-Match` 근거.
     *  ⚠️`src.rdata` 는 tsxState.type 이 RX_MSG 일 때만 유효(union) — 타입 확인 없이 접근 금지. */
    override fun onSendRequest(prm: OnSendRequestParam) {
        runCatching {
            if (prm.e.type != pjsip_event_id_e.PJSIP_EVENT_TSX_STATE) return
            val ts = prm.e.body.tsxState
            val tsx = ts.tsx
            if (tsx.statusCode < 200) return
            var etag: String? = null
            if (ts.type == pjsip_event_id_e.PJSIP_EVENT_RX_MSG) {
                val whole = runCatching { ts.src.rdata.wholeMsg }.getOrNull()
                if (!whole.isNullOrEmpty()) {
                    etag = Regex("(?im)^SIP-ETag:\\s*(.+)$").find(whole)?.groupValues?.get(1)?.trim()
                }
            }
            owner.dispatchSendReqResult(prm.userData, tsx.method ?: "", tsx.statusCode, tsx.statusText ?: "", etag)
        }.onFailure { Log.w("CimsAccount", "onSendRequest: ${it.message}") }
    }

    override fun onIncomingCall(prm: OnIncomingCallParam) {
        val call = CimsCall(owner, this, prm.callId)
        val from = runCatching { call.info.remoteUri }.getOrDefault("")
        val whole = runCatching { prm.rdata.wholeMsg }.getOrDefault("")
        // MCData MSRP 배포 INVITE(TS 24.282 §9.2.3, m=message TCP/MSRP) — 통화가 아니므로
        // 벨소리/Incoming UI 없이 msrpMode 로 격리 처리(수락·수신은 상위 계층 몫).
        if (whole.contains("TCP/MSRP") && whole.contains("a=path:")) {
            call.msrpMode = true
            owner.dispatchMsrpIncoming(call, from, whole)
            return
        }
        // 영상 여부 = 수신 INVITE 원문의 SDP 에 m=video offer 존재(협상 전이라 CallInfo.media 는 비어 있음).
        val video = whole.contains("m=video")
        // MCPTT 그룹콜 = multipart 의 mcptt-info 본문(ptt_ue.md §7) — PTT 앱이 자동 수락
        val mcptt = whole.contains("mcptt-info")
        // 긴급 그룹콜 = fan-out INVITE mcptt-info 의 emergency-ind=true (TS 24.379) — 긴급 UI/톤
        val emergency = mcptt && Regex("<emergency-ind>\\s*true", RegexOption.IGNORE_CASE).containsMatchIn(whole)
        // 1:1 private call = mcptt-info session-type=private (TS 24.379 §11.1). 상대는
        // mcptt-calling-user-id(tel:번호)로 식별 — From 은 서버 표기라 규격 필드를 우선한다.
        val privateCall = mcptt &&
            Regex("<session-type>\\s*private", RegexOption.IGNORE_CASE).containsMatchIn(whole)
        val callerId = if (privateCall)
            Regex("<mcptt-calling-user-id>\\s*(?:tel:|sip:)?([+\\d]+)", RegexOption.IGNORE_CASE)
                .find(whole)?.groupValues?.get(1) ?: "" else ""
        // 전이중 1:1 = INVITE 의 floor fmtp 에 mc_no_floor_ctrl (G17) — floor 없이 mic 상시 개방
        val noFloorCtrl = privateCall && whole.contains("mc_no_floor_ctrl")
        // ⚠️ floor 섹션 주입은 **180 응답 전**에 끝내야 한다 — pjsua 는 여기서 응답 SDP 를 한 번
        //   만들고 200 OK 에 재사용하므로, 그 뒤에 pendingAppSdp 를 넣으면 onCallSdpCreated 가
        //   이미 지나가 주입이 영구히 스킵된다(= m=application 0 송신 → CSP 가 착신 leg floor
        //   포트를 모른 채 audio+1 fallback). 계약은 [SipController.incomingFloorSdp] KDoc 참조.
        if (mcptt) {
            runCatching {
                owner.incomingFloorSdp?.invoke(
                    IncomingFloorInfo(prm.callId, from, callerId, privateCall, noFloorCtrl),
                )?.let { call.pendingAppSdp = it }
            }.onFailure { Log.w("CimsAccount", "incomingFloorSdp: ${it.message}") }
        }
        // 180 Ringing 만 자동 응답 — 실제 200 OK 는 사용자 answer().
        runCatching {
            call.answer(CallOpParam().apply { statusCode = pjsip_status_code.PJSIP_SC_RINGING })
        }
        owner.dispatchIncoming(call, from, video, mcptt, emergency, privateCall, callerId, noFloorCtrl)
    }

    /** 문자(SIP MESSAGE) 수신 — 200 OK 응답은 PJSIP 가 자동, 본문만 컨트롤러로 중계.
     *
     * ⚠️ pjsua2 Java 바인딩 실측: `multipart/mixed` MESSAGE 는 [OnInstantMessageParam.msgBody] 가
     *  **빈 문자열**로, `contentType` 도 boundary 파라미터가 빠진 "multipart/mixed" 만 넘어온다
     *  (pjsip 이 multipart body 를 String 으로 재구성하지 않음 — MCData 그룹 SDS/FD 수신 불가).
     *  이 경우 착신 INVITE 와 동일하게 원문([SipRxData.wholeMsg])에서 Content-Type(boundary 포함)
     *  헤더와 본문을 직접 추출한다. text/plain 등 단일 파트는 종전대로 msgBody 사용. */
    override fun onInstantMessage(prm: OnInstantMessageParam) {
        var ct = prm.contentType ?: ""
        var body = prm.msgBody ?: ""
        if (body.isEmpty() || (ct.startsWith("multipart/", true) && !ct.contains("boundary", true))) {
            val whole = runCatching { prm.rdata.wholeMsg }.getOrDefault("")
            if (whole.isNotEmpty()) {
                val sep = whole.indexOf("\r\n\r\n").let { if (it >= 0) it to 4 else whole.indexOf("\n\n") to 2 }
                if (sep.first >= 0) {
                    val headers = whole.substring(0, sep.first)
                    body = whole.substring(sep.first + sep.second)
                    Regex("(?im)^Content-Type:\\s*(.+)$").find(headers)?.groupValues?.get(1)?.trim()
                        ?.let { ct = it }
                }
            }
        }
        owner.dispatchInstantMessage(prm.fromUri, ct, body)
    }
}
