package com.cims.ue.ptt

import com.cims.ue.sdk.CommencementMode

/**
 * 호 수락·거절 사유의 순수 판정 — 기기 없이 시험한다([CallRulesTest]).
 */
internal object CallRules {

    /**
     * 제어 기능의 MCVideo 멤버 초대(TS 24.281 §9.2.1.3)를 받을 것인가 — 초대 그룹이 지금 영상 채널이고 그 그룹의 영상 호에 아직 없을 때만.
     * 아니면 받기 전에 거절한다(코어가 480 + Warning «110 user declined the call invitation» 으로 답한다 — §6.2.3.2.2 2)).
     * 받고 나서 끊으면(BYE) 내가 첫 응답자일 때 개시자 호가 성립하자마자 «참가자 1명 이하» 로 풀린다.
     */
    fun acceptVideoInvitation(inviteGroup: String, videoChannel: String?, alreadyInCall: Boolean): Boolean =
        inviteGroup.isNotEmpty() && inviteGroup == videoChannel && !alreadyInCall

    /**
     * 잃은 prearranged 호(무전 = MCPTT · 영상 = MCVideo)에 세션 식별자로 재합류할 것인가(TS 24.379 §10.1.1.2.4.1 · TS 24.281 §9.2.1.2.4.1
     * «커버리지 복귀 때»). 성립했던 호가 **망 문제로** 끊겼을 때만 — 요청 시한(408)·전송 실패(503)·세션 타이머(pjsip 은 408 로 끝낸다).
     * 서버·상대의 정상 종료(BYE = 200)나 다른 거절이면 세션이 끝난 것이라 재합류하지 않는다(다음 초대를 기다린다). chat 호는 그냥 다시
     * 합류하므로, 애드혹·개별 호는 재합류 대상이 아니다(prearranged 만).
     */
    fun rejoinLostSession(prearranged: Boolean, wasActive: Boolean, lastCode: Int, sessionUri: String): Boolean =
        prearranged && wasActive && sessionUri.isNotBlank() && (lastCode == 408 || lastCode == 503)

    /**
     * 개별 통화 발신에 실을 상대 응답 방식(TS 24.379 §11.1.1.2.1.1 14) — RFC 5373) — 사용자가 고른 [chosen] 이 user profile 인가([allowed] —
     * allow-*-commencement·allow-force-auto-answer)에 있으면 그대로, 없으면 지정 안 함(헤더 없음 — 상대 단말 설정대로). 인가가 바뀌어
     * 고른 방식이 막혔는데도 그대로 실으면 서버가 403(125·126·143)으로 호를 거절한다. 프로파일을 아직 못 받았으면(null) 고른 값 그대로.
     */
    fun effectiveCommencement(chosen: CommencementMode, allowed: Set<CommencementMode>?): CommencementMode =
        if (allowed == null || chosen in allowed) chosen else CommencementMode.UNSPECIFIED

    /**
     * 착신 개별 통화가 사용자 수락을 기다리는가 — 코어가 따른 개시 방식이 수동(상대가 `Answer-Mode: Manual` 을 요청, TS 24.379
     * §11.1.1.2.1.2 10))이고 아직 받지 않았다. 그동안 1:1 화면은 [받기]·[거절] 을 보인다(거절 = 코어가 480 + Warning 110).
     */
    fun awaitingAnswer(incoming: Boolean, ringing: Boolean, commencement: CommencementMode): Boolean =
        incoming && ringing && commencement == CommencementMode.MANUAL

    /**
     * 이 그룹으로 호·경보를 열어도 되는가 — 그룹 문서의 `<preconfigured-group-use-only>` 가 true 면 열지 않고 사용자에게 알린다
     * (TS 24.379 §10.1.1.2.1.1·§10.1.2.2.1.1 · TS 24.281 §9.2.1.2.1.1·§9.2.2.2.1.1·§12.1.1.1 — 재편성의 설정 원본으로만 쓰는 그룹).
     * 문서를 아직 받지 못했으면(null) 막지 않는다 — 서버가 403 Warning 167·168 로 판정한다.
     */
    fun groupUsable(preconfiguredOnly: Boolean?): Boolean = preconfiguredOnly != true

    /**
     * 호가 거절됐을 때 사용자에게 보일 사유 — 응답의 Warning 문구 번호(TS 24.379 §4.4.2 · TS 24.281 §4.4.2)로 가른다.
     * 모르는 번호·Warning 없는 실패는 null(호출자가 일반 문구를 쓴다).
     */
    fun rejectionText(statusCode: Int, warningCode: Int): String? = when (warningCode) {
        100 -> "지금은 이 그룹으로 통화할 수 없습니다"                     // function not allowed due to <reason> (운용 시간 밖 등 로컬 정책)
        102 -> "참여할 수 있는 그룹 수를 넘었습니다"                       // too many simultaneous affiliations (N2)
        103 -> "동시에 참여할 수 있는 그룹 통화 수를 넘었습니다"            // maximum simultaneous MCPTT group calls reached (N6)
        107 -> "개별 통화 권한이 없습니다"                                // user not authorised to make private calls
        110 -> "상대가 통화를 받지 않았습니다"                            // user declined the call invitation
        113 -> "없는 그룹입니다"                                         // group document does not exist
        115 -> "사용이 중지된 그룹입니다"                                 // group is disabled
        116 -> "이 그룹의 멤버가 아닙니다"                                // user is not part of the MCPTT group
        120 -> "이 그룹에 참여(제휴)하지 않았습니다"                       // user is not affiliated to this group
        121 -> "이 그룹 통화에 참여할 권한이 없습니다"                     // user is not authorised to join the group call
        122 -> "그룹 통화 정원이 찼습니다"                                // too many participants
        125 -> "자동 수락 개별 통화 권한이 없습니다"                       // not authorised to make private call with automatic commencement
        126 -> "수동 수락 개별 통화 권한이 없습니다"                       // not authorised to make private call with manual commencement
        127 -> "상대가 개별 통화를 받을 수 없습니다"                       // user not authorised to be called in private call
        144 -> "이 사용자에게는 개별 통화를 걸 수 없습니다"                 // user not authorised to call this particular user
        167 -> "이 그룹으로는 통화할 수 없습니다"                         // call is not allowed on the preconfigured group
        168 -> "이 그룹에는 경보를 보낼 수 없습니다"                       // alert is not allowed on the preconfigured group
        185 -> "애드혹 통화 권한이 없습니다"                              // user not authorised to initiate the adhoc group call
        186 -> "애드혹 통화를 지원하지 않는 시스템입니다"                   // the MCPTT system do not support adhoc group call
        189 -> "애드혹 통화에 부를 수 있는 인원을 넘었습니다"               // maximum number of allowed adhoc group participants exceeded
        190 -> "애드혹 통화 참가자를 바꿀 권한이 없습니다"                  // not authorised to modify adhoc group call participants
        else -> null
    }.takeIf { statusCode >= 300 }

    /**
     * 문자·파일(MCData) 전송이 거절됐을 때 사용자에게 보일 사유 — 응답의 Warning 문구 번호(TS 24.282 §4.9)로 가른다. 모르는 번호·Warning 없는
     * 실패는 null(호출자가 일반 문구를 쓴다).
     */
    fun sendRejectionText(statusCode: Int, warningCode: Int): String? = when (warningCode) {
        116 -> "이 그룹의 멤버가 아닙니다"                                // user is not part of the MCData group
        120 -> "이 그룹에 참여(제휴)하지 않았습니다"                       // user is not affiliated to this group
        206 -> "이 그룹은 문자를 쓸 수 없습니다"                          // short data service not allowed for this group
        213 -> "이 그룹은 파일 전송을 쓸 수 없습니다"                      // file distribution not allowed for this group
        217 -> "메시지가 너무 큽니다"                                    // unable to send due to message size
        else -> null
    }.takeIf { statusCode >= 300 }

    /**
     * 그룹 문자(SDS)를 보내기 전의 단말 검사(TS 24.282 §9.2.1.1 1) → §11.1) — 그룹 문서가 문자를 허용하지 않거나 본문이 그룹의 크기 상한
     * (`mcdata-on-network-max-data-size-for-SDS`)을 넘으면 보내지 않고 사유를 돌려준다. 보낼 수 있으면 null. 문서를 아직 받지 못했으면
     * (allowSds = null) 막지 않는다 — 서버가 403 206·217 로 판정한다.
     */
    fun sdsBlockReason(allowSds: Boolean?, maxSdsBytes: Int?, payloadBytes: Int): String? = when {
        allowSds == false -> "이 그룹은 문자를 쓸 수 없습니다"
        maxSdsBytes != null && maxSdsBytes > 0 && payloadBytes > maxSdsBytes -> "메시지가 너무 큽니다(최대 ${maxSdsBytes}바이트)"
        else -> null
    }
}
