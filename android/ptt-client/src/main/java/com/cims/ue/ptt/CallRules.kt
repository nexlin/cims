package com.cims.ue.ptt

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
     * 잃은 prearranged 영상 호에 세션 식별자로 재합류할 것인가(TS 24.281 §9.2.1.2.4.1 «커버리지 복귀 때»). 성립했던 호가 **망 문제로**
     * 끊겼을 때만 — 요청 시한(408)·전송 실패(503)·세션 타이머(pjsip 은 408 로 끝낸다). 서버·상대의 정상 종료(BYE = 200)나
     * 다른 거절이면 세션이 끝난 것이라 재합류하지 않는다(다음 초대를 기다린다). chat 호는 그냥 다시 합류하므로 대상이 아니다.
     */
    fun rejoinVideoSession(prearranged: Boolean, wasActive: Boolean, lastCode: Int, sessionUri: String): Boolean =
        prearranged && wasActive && sessionUri.isNotBlank() && (lastCode == 408 || lastCode == 503)

    /**
     * 호가 거절됐을 때 사용자에게 보일 사유 — 응답의 Warning 문구 번호(TS 24.379 §4.4.2 · TS 24.281 §4.4.2)로 가른다.
     * 모르는 번호·Warning 없는 실패는 null(호출자가 일반 문구를 쓴다).
     */
    fun rejectionText(statusCode: Int, warningCode: Int): String? = when (warningCode) {
        102 -> "참여할 수 있는 그룹 수를 넘었습니다"                       // too many simultaneous affiliations (N2)
        103 -> "동시에 참여할 수 있는 그룹 통화 수를 넘었습니다"            // maximum simultaneous MCPTT group calls reached (N6)
        107 -> "개별 통화 권한이 없습니다"                                // user not authorised to make private calls
        110 -> "상대가 통화를 받지 않았습니다"                            // user declined the call invitation
        115 -> "사용이 중지된 그룹입니다"                                 // group is disabled
        116 -> "이 그룹의 멤버가 아닙니다"                                // user is not part of the MCPTT group
        120 -> "이 그룹에 참여(제휴)하지 않았습니다"                       // user is not affiliated to this group
        122 -> "그룹 통화 정원이 찼습니다"                                // too many participants
        127 -> "상대가 개별 통화를 받을 수 없습니다"                       // user not authorised to be called in private call
        144 -> "이 사용자에게는 개별 통화를 걸 수 없습니다"                 // user not authorised to call this particular user
        else -> null
    }.takeIf { statusCode >= 300 }
}
