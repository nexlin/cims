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
        // 영상 여부 = 수신 INVITE 원문의 SDP 에 m=video offer 존재(협상 전이라 CallInfo.media 는 비어 있음).
        val video = whole.contains("m=video")
        // MCPTT 그룹콜 = multipart 의 mcptt-info 본문(ptt_ue.md §7) — PTT 앱이 자동 수락
        val mcptt = whole.contains("mcptt-info")
        // 180 Ringing 만 자동 응답 — 실제 200 OK 는 사용자 answer().
        runCatching {
            call.answer(CallOpParam().apply { statusCode = pjsip_status_code.PJSIP_SC_RINGING })
        }
        owner.dispatchIncoming(call, from, video, mcptt)
    }

    /** 문자(SIP MESSAGE) 수신 — 200 OK 응답은 PJSIP 가 자동, 본문만 컨트롤러로 중계. */
    override fun onInstantMessage(prm: OnInstantMessageParam) {
        owner.dispatchInstantMessage(prm.fromUri, prm.contentType, prm.msgBody)
    }
}
