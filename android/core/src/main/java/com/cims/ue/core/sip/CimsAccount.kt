package com.cims.ue.core.sip

import android.util.Log
import org.pjsip.pjsua2.Account
import org.pjsip.pjsua2.CallOpParam
import org.pjsip.pjsua2.OnIncomingCallParam
import org.pjsip.pjsua2.OnInstantMessageParam
import org.pjsip.pjsua2.OnRegStateParam
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
        // 180 Ringing 만 자동 응답 — 실제 200 OK 는 사용자 answer().
        runCatching {
            call.answer(CallOpParam().apply { statusCode = pjsip_status_code.PJSIP_SC_RINGING })
        }
        owner.dispatchIncoming(call, from, video, mcptt, emergency)
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
