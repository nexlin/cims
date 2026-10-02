/*
 * Group Call Service Header
 */

#ifndef _GROUP_CALL_SERVICE_H_
#define _GROUP_CALL_SERVICE_H_

#include <chrono>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "CmpClient.h"  // CmpGroupSession
#include "McpttInfo.h"  // McpttIndicators

class CSipCallRtp;
class CSipCallRoute;
class CSipMessage;
class CspPttGroup;

/** dialog 초기 full 스냅샷용 PTT 세션 참가 leg 1건 (CspServer 가 DialogNotifyState 로 변환 — dispatch_center.md §5.6a).
 */
struct PttDialogSnapshot {
    std::string strCallId;      ///< 참가자 leg Call-ID = dialog id
    std::string strSessionUri;  ///< 세션 URI `sip:<group id>@<ptt 도메인>` (dialog remote)
    std::string strExtXml;      ///< `<mcptt …/>` 확장 요소 (세션 종류·개시자·조건)
    bool bEstablished = false;  ///< true=confirmed / false=early(fan-out 초대 대기)
    bool bInitiator = false;    ///< 세션 개시자 leg
};

/**
 * @ingroup CspServer
 * @brief Service class to handle Group Calls
 */
class CGroupCallService {
public:
    CGroupCallService();
    ~CGroupCallService();

    /**
     * @brief Process a call to a group
     * @param pszGroupId The group ID being called
     * @param pszCallerInfo Caller information (From)
     * @param pszCallId Incoming Call-ID
     * @param pclsRtp RTP info of caller
     * @param pclsRoute Route info
     * @param bBroadcastInd INVITE mcptt-info 의 <broadcast-ind>true (TS 24.379 §6.2.8.2) — 세션을 **개시**하는
     *        INVITE 에서만 세션 속성이 된다. 진행 중 세션에 합류하는 INVITE 에서는 무시한다(§4.12, R7).
     * @return true if group call initiated, false if group not found or error
     */
    /** MCPTT 세션 식별자(TS 24.379 §4.5 — GRUU) 의 `gr` 토큰. bIssue=false 면 세션이 없을 때 빈 값. */
    std::string SessionIdentityToken( const std::string &strGroupId, bool bIssue = true );
    /** 재합류 Request-URI 의 세션 식별자가 지금 진행 중인 이 그룹 세션인가 (§10.1.1.4.5.1 2) — 아니면 404). */
    bool IsSessionIdentityActive( const std::string &strGroupId, const std::string &strToken );

    bool ProcessGroupCall( const char *pszGroupId, const char *pszCallerInfo, const char *pszCallId,
                           CSipCallRtp *pclsRtp, CSipCallRoute *pclsRoute, int iCondition = 0,
                           bool bBroadcastInd = false );

    /**
     * @brief Invite a member to a group call
     * @param pszUserId User ID to invite
     * @param pszGroupId Group ID
     * @return true if invitation initiated
     */
    bool InviteMember( const char *pszUserId, const char *pszGroupId );

    /**
     * @brief Forcibly clear any stale active call entry for userId.
     *        Called before auto-invite on REGISTER to handle WS reconnect races.
     */
    void ClearUserCall( const std::string &strUserId );

    /** callId가 PTT 그룹콜에 속하는지 확인. 속하면 groupId 반환, 아니면 빈 문자열 */
    std::string GetGroupIdByCallId( const std::string &strCallId );

    /** callId → (groupId, memberId) 조회. 활성 PTT 그룹콜 세션이면 true. (re-INVITE 식별용) */
    bool GetGroupCallSession( const std::string &strCallId, std::string &strGroupId, std::string &strMemberId );

    /** PTT 세션 dialog 이벤트 (RFC 4235, dispatch_center.md §5.6a) — 관제 앱이 PTT 회선을 감시해 타인 간 사설콜·
     *  애드혹·그룹 세션 참가를 아는 경로. 참가 leg 가 18x(early)/확립(confirmed)/종료(terminated)될 때 참가자
     *  회선의 dialog 구독자에게 낸다. 청취 leg(recvonly)는 참가가 아니라 내지 않는다(은닉과 무관하게 일관). */
    void NotifyPttDialog( const std::string &strCallId, const char *pszState );
    /** 감시 대상 PTT 회선 strAor 가 참가 중인 세션 leg 전부 (초기 full 스냅샷, 청취 leg 제외). */
    void CollectPttDialogs( const std::string &strAor, std::vector<PttDialogSnapshot> &vecOut );

    /** in-call 조건 re-INVITE 판정 결과 (TS 24.379 §10.1.1.4.7·§10.1.1.4.8). */
    struct InCallConditionVerdict {
        int iStatus = 0;          ///< 0 = 받아들인다(스택이 200 OK), 403 = 거절(다이얼로그는 그대로)
        std::string strBody;      ///< 403 의 mcptt-info 본문
        std::string strWarning;   ///< 200 OK 의 Warning 값(149 SIP INFO request pending — §4.4) — 비면 없음
        std::string strInfoBody;  ///< Warning 149 로 답한 뒤 ACK 에 보낼 INFO 의 mcptt-info (§6.3.3.1.18)
    };
    /** 진행 중 그룹 호의 조건 요청(re-INVITE 의 emergency-ind·imminentperil-ind·alert-ind) — 인가·상태 전이·재광고·
     *  비참여 제휴 멤버 통지까지 한 곳에서 한다(TS 24.379 §10.1.1.4.7 3)·4)·6)~8) · §10.1.1.4.8). 요소가 없는
     *  re-INVITE(세션 갱신·코덱 재협상)는 조건 요청이 아니다. 결과가 403 이면 호출측이 거절을 보내고, 아니면 흐름을
     *  잇는다(Warning 이 있으면 스택의 200 OK 에 싣는다). */
    InCallConditionVerdict OnInCallConditionRequest( const std::string &strCallId, const std::string &strGroupId,
                                                     const std::string &strMemberId, const CMcpttInfo &clsMi );
    /** 애드혹 그룹 호 참가자 변경 re-INVITE 판정·반영 결과 (TS 24.379 §17.4.5.1.1). */
    struct AdhocModifyVerdict {
        bool bHandled = false;   ///< 참가자 변경 요청이었다(애드혹 세션 + method=INVITE/BYE 항목)
        int iStatus = 0;         ///< 0 = 받아들였다(스택이 200 OK) · 403 = 거절
        std::string strWarning;  ///< 거절의 Warning 값(190 권한 · 189 상한 — §4.4)
    };
    /** 애드혹 그룹 호의 re-INVITE 에 실린 resource-lists 의 method=INVITE 항목을 초대하고 method=BYE 항목을 내보낸다
     *  (§17.4.5.1.1 4)a)ii)~iv)). 권한(3)) = user profile <allow-to-modify-adhoc-group-call-participants-info>(CSC 는
     *  false) 또는 로컬 정책 — CIMS 는 그 호의 개시자만. 상한(4)a)i)) = service configuration <max-no-participants>
     *  (개시자 밖 참가자 + 새 초대). 받아들이면 스택의 200 OK 에 Supported norefersub·tdialog(7)·8)). */
    AdhocModifyVerdict OnAdhocParticipantsModify( const std::string &strCallId, const std::string &strGroupId,
                                                  const std::string &strMemberId, const std::string &strBody );
    /** 2xx ACK 수신 — Warning 149 로 답한 re-INVITE 면 같은 다이얼로그에 INFO(Info-Package g.3gpp.mcptt-info)를 보낸다
     *  (TS 24.379 §10.1.1.4.7 끝 · §6.3.3.1.18). 대기 INFO 가 없으면 무동작. */
    void OnInDialogAck( const std::string &strCallId );

    /** 긴급 그룹 상태 해제 인가 — local policy(TS 24.379 §6.3.3.1.13.4 — 예시: 관제사·개시자) = 상태를 세운 개시자 ∨
     *  사용자 프로파일 allow-cancel-group-emergency(TS 24.484). 거부면 strReason. */
    bool IsEmergencyCancelAuthorized( const std::string &strGroupId, const std::string &strUserId,
                                      std::string &strReason );
    /** 임박 위험 해제 인가 (§6.3.3.1.13.6) = 사용자 프로파일 allow-cancel-imminent-peril. */
    bool IsImminentCancelAuthorized( const std::string &strUserId );
    /** 긴급 해제를 막는 «다른 긴급 사용자의 송출» (§10.1.1.4.7 7a)) — FLOOR_TALKERS 로 아는 발언자 중 요청자가 아닌
     *  긴급 상태 사용자가 있으면 그 사용자. 없으면 빈 문자열. */
    std::string EmergencyTalkerOtherThan( const std::string &strGroupId, const std::string &strUserId );

    /** 그룹의 진행 중 조건 (2=긴급·1=임박·0=없음). */
    int GroupConditionOf( const std::string &strGroupId );
    /** 그룹에 진행 중 호(참가 leg)가 있는가 — MESSAGE 경로가 §12.1.3.2(호 진행 중)/§12.1.3.3(호 없음)을 가른다. */
    bool HasGroupCallInProgress( const std::string &strGroupId );
    /** 인가된 긴급 그룹 상태 해제 (§10.1.1.4.7 8)·§12.1.3.2 1)b)·2)d)·§12.1.3.3 2)·TNG2 만료 §6.3.3.1.16) — 상태·긴급
     *  사용자 캐시 정리, floor tier 복귀, TNG2 정지·TNG3 재시작(§6.3.3.5.2), 참여 leg 재광고(§6.3.3.1.6·§6.3.3.1.10),
     *  비참여 제휴 멤버 통지(§6.3.3.1.11). strCanceller = 해제한 사용자(TNG2 는 빈 값 — PSI 가 알린다), clsAlert =
     *  재광고·통지에 같이 실을 경보 지시자(iAlert·strOriginatedBy — 없으면 -1). strExclude = 재광고·통지를 받지 않을
     * 사용자 (요청자 — 자기 요청의 응답으로 안다). 상태가 긴급이 아니면 false. */
    bool CancelGroupEmergency( const std::string &strGroupId, const std::string &strCanceller,
                               const McpttIndicators &clsAlert, const std::string &strExclude, const char *pszBy );

    /** 긴급 경보 캐시 (§12.1.3.1 4)b)iii)A)·§12.1.3.2 2)a)·b)) — 호와 무관하게 그룹·발령 사용자 단위로 남는다. */
    void SetAlertOutstanding( const std::string &strGroupId, const std::string &strUserId, bool bOn );
    bool HasOutstandingAlert( const std::string &strGroupId, const std::string &strUserId );
    /** 경보 발령 인가 (§6.3.3.1.13.1) = 그룹 allow-MCPTT-emergency-alert ∧ 사용자 allow-activate-emergency-alert ∧
     *  <EmergencyAlert> entry 가 DedicatedGroup 이면 대상 = 그 전용 그룹(1)a)i)). 거부면 pstrReason 에 사유. */
    bool IsAlertActivateAuthorized( const class CspPttGroup &clsGroup, const std::string &strUserId,
                                    std::string *pstrReason = nullptr );
    /** 경보 대상 그룹 제휴 (§12.1.3.1 4)b)i)) — 제휴했으면 true. 아니면 암묵적 제휴 자격(§9.2.2.3.6 → §9.2.2.3.8 =
     *  그룹 존재 + 멤버)을 보고 자격이 있으면 암묵적 제휴(§9.2.2.3.7)를 하고 true, 자격이 없으면 false(호출측 403 120).
     */
    bool AffiliateForAlert( const class CspPttGroup &clsGroup, const std::string &strUserId );
    /** 경보 취소 인가 (§6.3.3.1.13.3) = 사용자 allow-cancel-emergency-alert. */
    bool IsAlertCancelAuthorized( const std::string &strUserId );

    /** 조건 상태 통지 MESSAGE (TS 24.379 §6.3.3.1.11) — 그룹의 제휴 멤버(affiliation 요구 그룹은 affiliate 된
     * 멤버)에게. bNonParticipantsOnly 면 호에 참여하지 않은 멤버만(참여자는 re-INVITE 로 안다). strCallingUser =
     *  <mcptt-calling-user-id>(비면 싣지 않는다), strExclude = 받지 않을 사용자. 보낸 수. */
    int NotifyConditionToAffiliated( const std::string &strGroupId, const std::string &strCallingUser,
                                     const McpttIndicators &clsInd, bool bNonParticipantsOnly,
                                     const std::string &strExclude );

    /** CMP 발언자 집합(FLOOR_TALKERS, cmp_media_api.md §8) — 7a) 판정용 캐시. */
    void OnFloorTalkers( const std::string &strGroupId, const std::vector<std::string> &vecTalkers );

    /** condition(긴급·임박) 개시 인가 (TS 24.379 §6.3.3.1.13.2) — 3중 판정:
     *  그룹 capability(emergency_call) + 사용자 allow-emergency-group-call +
     *  DedicatedGroup 모드의 대상 일치. DB 불가 시 프로파일 축은 fail-open(그룹 축만 판정).
     *  거부 시 strReason 에 사유. */
    bool IsConditionInitAuthorized( const CspPttGroup &clsGroup, const std::string &strUserId, std::string &strReason );

    // Recovery & Monitor
    void StartMonitor();
    void StopMonitor();
    void OnCmpStatusChanged( bool bConnected );
    /** leg 종료. pszReason = 상대가 BYE 에 실은 Reason(RFC 3326, 첫 값 — 없으면 NULL) — 애드혹 그룹 호 개시자의
     *  «User requested release» 면 전원을 해제한다(TS 24.379 §6.3.3.2.4 3A)). */
    bool OnCallTerminated( const std::string &strCallId, const char *pszReason = nullptr );
    /** 개시자 응답 게이트의 leg 종료 — 개시자 CANCEL(세션 개시 중단) 또는 멤버 초대의 최종 실패 응답
     *  (TS 24.379 §6.3.3.3·§10.1.1.4.2). ModuleDispatcher::EventCallEnd 가 OnCallTerminated 전에 부른다. */
    void OnAckGateLegEnd( const std::string &strCallId, int iSipStatus );
    /** 게이트 중인 초대 leg 의 18x — 사설 호면 첫 180 을 개시자에게 옮긴다(TS 24.379 §11.1.1.4.2). 게이트 leg 이면
     * true. */
    bool OnAckGateRinging( const std::string &strCallId, int iSipStatus );
    /** 멤버 초대 leg 의 INVITE 응답 원문(100 제외, ModuleDispatcher::EventInviteResponse — Ring/Start/End 보다 먼저).
     *  · 신뢰성 18x(Require: 100rel)는 PRACK 으로 받는다(RFC 3262 — 제어 기능이 UAC).
     *  · 게이트 중이면 Warning 을 모아 개시자 200 OK 에 옮긴다(TS 24.379 §6.3.3.2.3.2 7)).
     *  · 게이트 중 183 + P-Answer-State: Unconfirmed(RFC 4964) 이고 TNG1 이 돌지 않으며 CMP 가 미디어 버퍼링을 하면
     * 개시자에게 200 OK(P-Answer-State: Unconfirmed)를 준다(§10.1.1.4.2 · 사설 호 §11.1.1.4.2). */
    void OnMemberInviteResponse( const std::string &strCallId, CSipMessage *pclsResponse );
    /** MCPTT 그룹 호 leg 이 받은 re-INVITE(세션 갱신 포함, ModuleDispatcher::EventReInvite) — answer 의 `a=fmtp:MCPTT`
     * 를 이 re-offer 에 있던 파라미터로 다시 짓는다(TS 24.380 §14.3.1). 스택은 직전 로컬 선언을 되풀이하므로 그대로
     * 두면 개시 answer 의 mc_implicit_request·제어 기능 offer 의 mc_priority 가 offer 에 없어도 남는다. 이어지는 offer
     * 의 mc_implicit_request 는 긴급·임박 격상에서만 뜻이 있고(§14.5 · TS 24.379 §6.4) 제어 기능은 새 세션 개시에서만
     * 받는다(§14.3.5) — answer 에 싣지 않는다. 이 서비스의 floor 채널 leg 이면 true. */
    bool RebuildReInviteFloorFmtp( const std::string &strCallId, CSipCallRtp *pclsRemoteRtp,
                                   CSipCallRtp *pclsLocalRtp );

    /** 미디어 노드(CMP) 다운으로 relay 가 소실된 그룹의 활성 멤버 호를 능동 종료(BYE)하고 로컬 상태를
     *  정리한다. dead node 이므로 CmpClient(LeaveGroup/RemoveGroup, blocking)는 호출하지 않는다.
     *  종료한 멤버 호 수를 반환. */
    int TerminateGroupLocal( const std::string &strGroupId );
    void OnCallStarted( const std::string &strCallId, const std::string &strRemoteIp, int iRemotePort,
                        int iRemoteFloorPort = 0, class CSipCallRtp *pclsRtp = NULL );

    /** Called by CSC interface when group/user config changes externally.
     *  strChangedGroupId = CSC 가 알린 그룹(GROUP_CHANGED) — 내용 비교와 무관하게 그 그룹 문서를 통지한다.
     *  strEtag = 그 그룹 문서의 새 ETag(xcap-diff new-etag). 비면 전체 재동기(CSC_RESTART). */
    void OnGroupConfigChanged( const std::string &strChangedGroupId = "", const std::string &strEtag = "" );

    /** CMP 가 유휴 그룹(멤버·활동 없음)을 자체 회수(PTT_GROUP_ABORTED)했을 때 CSP 캐시를 정리한다.
     *  다음 그룹 사용 시 SyncGroupsState/AddGroup 경로가 깨끗한 sesid 로 재수립한다. CmpClient 이벤트 핸들러가 호출. */
    void OnGroupAborted( const std::string &strGroupId );

    /** CMP T4(Inactivity) 만료(PTT_FLOOR_INACTIVITY) — 호를 해제한다: 편성·일제·애드혹 그룹 호 = TS 24.379 §6.3.8.1 1),
     *  개별 호 = §6.3.8.2 1). chat 그룹 호는 해제 목록에 없다(T4 를 걸지 않는다). strSesId 가 현재 세션과 다르면 지난
     *  세션의 이벤트라 무시. */
    void OnFloorInactivity( const std::string &strGroupId, const std::string &strSesId );

    /**
     * @brief Send RFC 4575 conference-info NOTIFY to all active participants in a group
     * @param strGroupId  Group ID
     * @param strChangedUser  The user entity that changed (added/removed/joined/left)
     * @param strStatus  "connected", "disconnected", "pending"
     * @param strJoining "added", "removed", "updated"
     */
    void SendConferenceNotify( const std::string &strGroupId, const std::string &strChangedUser,
                               const std::string &strStatus, const std::string &strJoining );

    /** conference-info+xml 본문(현재 로스터 스냅샷, RFC 4575) 생성. 확립 leg 만 싣고 version 을 증가시킨다.
     *  @param pvecLegsOut  NULL 이 아니면 확립 leg 의 (Call-ID, 멤버 ID) 목록을 담아 준다 —
     *                      in-dialog 폴백을 구독 없는 멤버에게만 보내기 위해 멤버 ID 가 필요하다.
     *  구독 수락 직후 초기 NOTIFY(RFC 6665 §4.1.1)에도 사용 — 변경 인자 없이 호출하면 순수 스냅샷. */
    std::string BuildConferenceInfoBody( const std::string &strGroupId, const std::string &strChangedUser = "",
                                         const std::string &strStatus = "", const std::string &strJoining = "",
                                         std::vector<std::pair<std::string, std::string>> *pvecLegsOut = NULL );

    /** leg 별 PT 재작성 파라미터 산출 (docs/api/cmp_media_api.md §6.1/§7.4).
     *  user_pt/user_te_pt = 이 leg 의 원격 SDP 가 수신 선언한 audio/TE wire PT,
     *  user_src_pt/user_src_te_pt = 서버가 그 leg 쪽에 낸 SDP 의 PT(= UE 송신 PT).
     *  pstrCodec != NULL 이면 협상 오디오 코덱 문자열("AMR-WB/16000")도 반환 — 녹취 메타용.
     *  VoLTE relay leg(remote_* 계열)도 동일 규칙으로 사용한다. */
    static void GetLegPt( const std::string &strCallId, bool bServerOffered, int &iUserPt, int &iUserSrcPt,
                          int &iUserTePt, int &iUserSrcTePt, std::string *pstrCodec = NULL );

    /** 멤버 SDP(m=application)의 a=fmtp:MCPTT 협상 결과 파싱 (TS 24.380 §12.1.2.3) —
     *  mc_queueing/mc_priority=N/mc_granted → PTT_JOIN 의 queueing/max_priority/granted.
     *  fmtp:MCPTT 부재(레거시 단말·cspsim 구버전)면 clsFmtp 를 미전송 상태로 둔다
     *  (CMP 기본 동작 유지). fmtp 가 있는데 mc_queueing 이 없으면 queueing=0 (규격: 미협상
     *  멤버의 비선점 요청은 Deny #1). */
    static void ParseMcpttFmtp( class CSipCallRtp *pclsRtp, struct McpttFmtp &clsFmtp );

    /** ad-hoc 그룹 생성·해제 감사 (E-AUD-010 regroup_changed) — created/released.
     *  임시 그룹은 GroupMap 에서 조용히 사라져 존재했다는 사실조차 안 남았다. 생성은
     *  ModuleDispatcher(개시 경로), 해제는 teardown 경로에서 부른다.
     *  사설콜(priv-)은 대상이 아니다 — 정의는 ad-hoc/regroup 이고, 1:1 통화마다 감사가
     *  쌓이는 것은 그 요구가 아니다.
     *  @param pszScope 세션 종류 — 현행 "ad-hoc". */
    static void EmitRegroupEvent( const char *pszAction, const std::string &strGroupId, const char *pszScope );

    /** conference 이벤트(RFC 4575) 구독 인가 — TS 24.379 §10.1.3.4.1 (dispatch_center.md §5.6).
     *  0=허용. 아니면 보낼 SIP 상태(403 = 그룹 문서 <on-network-allow-conference-state> 불허·Warning 138 /
     *  480 = 브로드캐스트 그룹·Warning 105)와 Warning 헤더 값·거절 사유(로그용). 멤버는 그룹 속성으로, 비멤버 관제사는
     *  청취 leg 와 같은 2단(allow_ambient_listening + ptt_listen 범위)으로 판정한다 — 즉석 세션(adhoc/priv)·미지 자원은
     * 통과. bAuthzOnly = 인가만 판정한다(일제 통화의 일시 480/105 는 인가 상실이 아니다 — 권한 재점검 스윕·수락 직후
     * 재검사용. 기존 구독을 rejected 로 끊으면 단말은 재구독하지 않는다, RFC 6665 §4.1.3). */
    static int CheckConferenceSubscribe( const std::string &strGroupId, const std::string &strUserId,
                                         std::string &strWarning, std::string &strReason, bool *pbUnavailable = nullptr,
                                         bool bAuthzOnly = false );

    /** PTT 청취 인가 판정 — **합류(§5.6)·합류 중 재확인·회수 스윕이 같은 식을 쓰게 하는 단일 지점.**
     *  셋이 갈라지면 허용된 것을 걷거나 잃은 것을 남긴다. 반환 = "" 허용, 그 외 거절 사유(로그·감사용).
     *  2단 = 자격 `allow_ambient_listening`(TS 24.484, 역할 배정에 맞춰 CSC 가 동기) + 범위(즉석 세션은
     *  `CanObserveEphemeral`, 그 외는 역할 `ptt_listen`). DB(프로파일)를 읽으므로 락 밖에서 부른다.
     *  @param pbUnavailable (선택) 거절 사유가 **조회 불능**이면 true. 합류(새 권한)는 이것을 무시하고 막지만
     *         (fail closed), 회수(기존 권한)는 이때 걷지 않는다(fail open) — DB 장애를 «자격 상실» 로 읽으면
     *         멀쩡한 청취가 끊긴다(dispatch_center.md §5.10). */
    static std::string ListenDenyReason( const CspPttGroup &clsGroup, const std::string &strListener,
                                         bool *pbUnavailable = nullptr );

    /** CMP 멤버 해제 요청 — 실패하면 재시도 대기열에 넣는다. **PTT_LEAVE 를 보내는 모든 경로가 이것만 쓴다.**
     *  `LeaveGroup` 은 실제 전송 결과를 돌려주는데(`CmpClient.cpp`) 지금까지 그 값을 아무도 보지 않았다.
     *  요청이 유실되면 세션 맵에서는 지워졌는데 **CMP 에는 멤버가 남아 RTP 를 계속 받는다** — 청취 leg 이면
     *  회수가 성립하지 않고, 일반 멤버면 끊은 뒤에도 소리가 간다. 재시도할 주인이 없으므로 여기서 잡는다. */
    void LeaveGroupOrQueue( const std::string &strGroupId, const std::string &strSessionId, const std::string &strSesId,
                            const char *pszWhy );

    /** 대기열에서 만기된 건을 재시도한다 — MonitorLoop 1초 틱에서 부른다. 한 틱 처리 상한을 둔다. */
    void RetryPendingLeaves();

    /**
     * 그 멤버의 밀린 `PTT_LEAVE` 를 버린다 — **JOIN 이 «지금 있다» 는 권위다.**
     *
     * CMP 멤버 키는 `(group, user)` 라 재합류해도 같다. 그래서 옛 LEAVE 재시도가 **방금 들어온 멤버의
     * 미디어·floor 를 걷어 간다** — SIP 는 살아 있는데 무음이 된다. 이것은 Call-ID 재사용 같은 비정상
     * 입력이 아니라 **앱이 BYE 없이 재INVITE 하는 정상 재합류**에서 난다. 그래서 `ProcessGroupCall` 의
     * 재조인 처리도 옛 leg 에 CMP LEAVE 를 보내지 않는다(같은 파일, 재조인 주석). 밀린 것도 같은 이유로
     * 버려야 한다. */
    void PurgePendingLeave( const std::string &strGroupId, const std::string &strMemberId );

    /** 인가를 잃은 PTT 청취 leg 회수 (dispatch_center.md §5.10) — 역할 재적재 뒤 전수 재판정한다.
     *  판정은 합류 시(§5.6 ProcessGroupCall)와 **같은 2단**이다: 자격 `allow_ambient_listening`(CSC 가 역할
     *  배정에 맞춰 동기) + 범위(즉석 세션은 `CanObserveEphemeral`, 그 외는 `CanListenPtt`). 회수는 청취자에게
     *  BYE — 원 세션과 다른 참가자는 건드리지 않는다.
     *  @return 걷어낸 leg 수. */
    int RevokeUnauthorizedListeners( const char *pszWhy );

private:
    void MonitorLoop();
    /** 그룹 맵 재적재(DB 우선, 파일 폴백) + 바뀐 그룹 문서의 xcap-diff 통지(RFC 5875 · TS 24.481).
     *  통지 대상 = 재적재 **전 멤버 ∪ 후 멤버** — 새 그룹·새 멤버는 그룹이 생겼음을, 삭제·빠진 멤버는
     *  사라졌음을 알아야 한다. 그룹 통지는 이 함수 한 곳에서만 낸다(SyncGroupsState 는 CMP 동기만). */
    void ReloadGroupMap( const std::string &strChangedGroupId, const std::string &strEtag );
    void SyncGroupsState();
    void CheckMemberState();
    void CheckGroupIntegrity();

    /** 멤버 포트 캐시 무효화 — PTT_LEAVE 는 CMP 멤버 유닛을 풀로 반납하므로 재조인 시 다른
     *  유닛(포트)이 배정될 수 있다. LeaveGroup 을 보내는 모든 경로에서 호출해, 다음 InviteMember
     *  가 스테일 포트로 SDP offer 를 만들어 UE 상향이 옛 유닛으로 향하는(전량 pre-join drop 무음)
     *  것을 막는다. */
    void InvalidateMemberPort( const std::string &strGroupId, const std::string &strMemberId );

    /** 그룹 멤버 구성(id:priority 순서)의 해시. SyncGroupsState 의 "Config Changed" 판정 기준.
     *  그룹 컨텍스트(m_mapGroupRtp)를 만드는 모든 경로(SyncGroupsState/InviteMember/CheckGroupIntegrity)
     *  에서 동일하게 저장해야 한다. 0(미설정)으로 두면 다음 SyncGroupsState 가 실제해시와 불일치로
     *  착각해 스퓨리어스 ModifyGroup storm 을 일으켜 멤버 무더기 drop 됨.
     *  ReloadGroupMap 의 그룹 문서 변경 판정은 이 지문을 포함하는 ComputeGroupDocHash 를 쓴다. */
    static size_t ComputeGroupConfigHash( const class CspPttGroup &group );
    /** 그룹 문서(TS 24.481)에 드러나는 그룹 설정의 지문 — ReloadGroupMap 이 재적재 전후를 비교해 xcap-diff 를 보낼지
     * 정한다 (CMP 지문 + 이름·속성·멤버 표시·MCVideo 몫). */
    static size_t ComputeGroupDocHash( const class CspPttGroup &group );

    /**
     * @brief Build MCPTT call control info XML (application/vnd.3gpp.mcptt-info+xml, TS 24.379)
     * @param clsGroup PTT group info
     * @return XML string
     */
    /** 초기 INVITE·합류 200 OK 는 활성 지시자만(iCondition). pInd 가 있으면 그 지시자를 그대로 싣는다 —
     *  조건 재광고 re-INVITE(§6.3.3.1.6 긴급·§6.3.3.1.10 긴급 해제·§6.3.3.1.15 임박 위험)는 절이 정한 요소만.
     *  bBroadcast = 세션이 일제 통화 — `<broadcast-ind>true` 를 싣는다(TS 24.379 §6.3.3.1 — session-type 은 그룹 종류).
     */
    static std::string BuildGroupInfoXml( const class CspPttGroup &clsGroup, const std::string &strUserId,
                                          const std::string &strCallerId, int iCondition = 0,
                                          const McpttIndicators *pInd = NULL, bool bBroadcast = false );

    /**
     * @brief 그룹 자기완결 디스크립터 JSON 생성 (group.json 기록용)
     *        — docs/design/features/mcptt_authorization.md §5.
     *        state/updated_at 은 CCallDir::PttSessionStart 가 주입한다.
     * @param clsGroup PTT group info
     * @return JSON object string
     */
    static std::string BuildGroupDescriptor( const class CspPttGroup &clsGroup, bool bBroadcast = false );

    /**
     * @brief Wrap SDP + MCPTT info XML into multipart/mixed body, update INVITE message
     *        (멤버 명단은 싣지 않는다 — TS 24.379 §6.3.3.1.2)
     * @param pclsInvite   INVITE message to modify
     * @param strGroupXml  MCPTT call control info XML (mcptt-info)
     * @param strFloorIp   Floor control IP (shared RTP IP)
     * @param iFloorPort   Floor control UDP port
     */
    static void WrapMultipartBody( class CSipMessage *pclsInvite, const std::string &strGroupXml,
                                   const std::string &strFloorIp, int iFloorPort, const std::string &strGroupUri = "",
                                   bool bNoFloorCtrl = false, const std::string &strFloorFmtp = "" );

    /** 기존 바디(psip AddSdp 산출 SDP)를 유지한 채 mcptt-info part 를 앞세운 multipart/mixed 로
     *  감싼다 — in-call 조건 재광고 re-INVITE·조인 200 OK 동봉용(SDP 는 손대지 않는다). */
    static void WrapInfoMultipart( class CSipMessage *pclsMessage, const std::string &strInfoXml );

    /** 진행 중 세션의 condition 변경(상향/하향·긴급 조인·TNG2 만료)을 확립 참여 leg(청취 leg 포함)에 re-INVITE 로
     *  재광고 (TS 24.379 §6.3.3.1.6 긴급·§6.3.3.1.10 긴급 해제·§6.3.3.1.15 임박 위험). SDP = 그 leg 에 성립한 미디어
     *  그대로(§6.3.3.1.6 1)·§6.3.3.1.15 2)), mcptt-info = clsInd + mcptt-calling-user-id(상태를 세운 사용자), Resource-
     *  Priority = iCond 값(§6.3.3.1.19). strExcludeMemberId = 변경을 일으킨 멤버(자기 요청의 응답으로 이미 안다).
     *  전송한 leg 수를 반환. */
    int PropagateConditionToMembers( const std::string &strGroupId, int iCond, const std::string &strExcludeMemberId,
                                     const McpttIndicators &clsInd );

    bool m_bMonitorRunning;
    std::thread m_threadMonitor;

    struct GroupRtpInfo {
        int iFloorPort;  // 그룹 공유 floor control 포트 (>0 = CMP 그룹 유효)
        std::string strIp;
        size_t nConfigHash;  // CMP 재전달이 필요한 설정(로스터·floor 정책)의 지문 — 변경 감지용
        std::string strSessionCallId;
        int iConfVersion;  // RFC 4575 conference-info version counter
        // 멤버별 CMP 전용 RTP 포트 (sid → audio) — 각 멤버의 SDP 에 이 포트를 광고. MCPTT 는 음성만(그룹 영상 =
        // MCVideo).
        std::map<std::string, int> memberPorts;
    };
    std::map<std::string, GroupRtpInfo> m_mapGroupRtp;

    /** 멤버 전용 CMP 포트 조회 — 캐시(memberPorts) 우선, 없으면 PTT_JOIN ①(선할당)으로 확보.
     *  (늦은 참가자/로스터 외 멤버의 SDP offer 생성 전 호출.) 실패 시 false. */
    bool GetOrAllocMemberPort( const std::string &strGroupId, const std::string &strMemberId, int &iAudioPort );

    /** 그룹의 진행 중 조건 (TS 24.379 — in-progress emergency / imminent peril state). 수명 = 그룹 세션
     *  (RemoveGroupSesId 가 지운다 — 규격은 명시 해제·TNG2 까지 유지, mcptt_emergency_modes.md §4.2 편차 표). */
    struct GroupCondition {
        int iCond = 0;                            ///< 2=긴급 · 1=임박 위험 (0 은 맵에 두지 않는다)
        std::string strInitiator;                 ///< 상태를 세운 사용자 — 재광고 mcptt-calling-user-id(§6.3.3.1.6 2))
        std::set<std::string> setEmergencyUsers;  ///< 긴급 상태 사용자 캐시(§10.1.1.4.7 6)a)·c)·8)b)) — 7a) 판정·tier
        time_t tTng2Start = 0;                    ///< TNG2 기점 (§6.3.3.1.16 — 긴급 첫 설정)
    };
    std::map<std::string, GroupCondition> m_mapGroupCond;
    /** 긴급 경보 발령 사용자 (group → MCPTT ID) — 세션과 무관(경보는 호 없이도 선다, §12.1.3.1). */
    std::map<std::string, std::set<std::string>> m_mapGroupAlerts;
    /** CMP 발언자 집합 (FLOOR_TALKERS) */
    std::map<std::string, std::set<std::string>> m_mapGroupTalkers;
    /** Warning 149 로 답한 re-INVITE 의 ACK 대기 INFO 본문 (callId → mcptt-info, §6.3.3.1.18) */
    std::map<std::string, std::string> m_mapPendingInfo;

    /** 그룹 세션 단위 통일 sesid: PTT_GROUP_ADD ~ JOIN/LEAVE ~ INVITE ~ PTT_GROUP_REMOVE 모두 동일 sesid 사용.
     *  key = group_id, value = sesid (형식: `{group_id}::csp::{us_ts}::{counter}`).
     *  GetOrIssueGroupSesId() 로 조회/발행, RemoveGroupSesId() 로 세션 종료 시 정리. */
    std::map<std::string, std::string> m_mapGroupSesId;

    /** 그룹 세션 속성 (mcptt_broadcast_group_call.md §3.1) — 세션을 **개시**한 INVITE 에서 한 번 정하고 세션 수명
     *  동안 바꾸지 않는다. 합류·재참여·청취 leg 는 참가자일 뿐이다(TS 24.380 §6.3.5.3.4). 세션 종료
     *  (RemoveGroupSesId)에서 지운다. CMP 재수립 ADD 도 이 값을 싣는다(CmpGroupSession). */
    struct GroupSession {
        std::string strInitiator;  ///< 개시자 — mcptt-calling-user-id·dialog initiator·CMP initiator_id
        bool bBroadcast = false;   ///< 일제 통화 (TS 24.379 §4.12)
        time_t tStart = 0;         ///< 세션 개시 시각 — TNG3(그룹 호 최대 시간)·개별 호 최대 통화 시간 판정
        int iStartCond = 0;        ///< 개시 INVITE 의 조건(2=긴급·1=임박) — 긴급·임박 애드혹 호는 TNG3 없음
        /** 개시 INVITE 처리 중(개시자 leg 확립 전) — 같은 그룹에 거의 동시에 온 INVITE 는 이 선점을 보고 합류로
         *  처리한다(개시자 = 세션을 연 사용자, TS 24.380 §6.3.5.3.4). 개시자 leg 확립에서 풀고, 개시가 실패하면
         *  속성째 지운다. 일제 통화 **진행 중** 판정(구독 480/105, TS 24.379 §10.1.3.4.1)은 확정된 세션만 본다. */
        bool bPending = false;
    };
    /** 개시 선점 유효 시간 — INVITE 트랜잭션 시한(64*T1)을 넘긴 선점은 버려진 것으로 본다. */
    static constexpr time_t kPendingSessionSec = 32;
    /** 암묵적 affiliation 만료 (TS 24.379 §9.2.2.3.7 4)d)ii) — 로컬 정책). 명시 affiliation PUBLISH 의 기본 만료와
     * 같다. */
    static constexpr int kImplicitAffiliationSec = 3600;
    /** 암묵적 affiliation 기록 (§9.2.2.3.7) — affiliation 행 + 제휴 변경 통지 + 사용자 제휴 상태 NOTIFY(§9.2.2.3.5).
     *  자격(§9.2.2.3.6)은 호출측이 본다. pszWhy = 로그 사유. 기록에 실패하면 false. */
    bool ImplicitAffiliate( const std::string &strGroupId, const std::string &strUserId, const char *pszWhy );
    std::map<std::string, GroupSession> m_mapGroupSession;

    /** 개시자 응답 게이트 — 새 세션 개시의 200 OK 를 멤버 응답 뒤로 미룬다.
     *  편성 그룹 = 확인 통화 설정(TS 24.379 §6.3.3.3 TNG1·§10.1.1.4.2): 필수 멤버(<on-network-required>, 초대 대상)가
     * 있으면 TNG1 을 초대 전에 켜고 그 멤버 전원의 200 과 멤버 200 누계 ≥ <on-network-minimum-number-to-start> 에
     * 응답한다. TNG1 만료·필수 멤버 거절은 <on-network-action-upon-expiration-…> 대로 proceed(200 + Warning 111) 또는
     * abandon(480·받은 최종 응답 + Warning 112). 필수 멤버가 없으면 누계가 최소 인원에 닿을 때(0 = 기다리지 않음 —
     * 게이트 없음). 사설 호 = 착신자의 200 뒤 (§11.1.1.4.2 — 최소 1). 초대한 멤버 전원이 거절하면 캐시한 최종 응답을
     * 개시자에게 준다. */
    struct AckGate {
        std::string strCallId;                     ///< 개시자 leg
        std::string strInitiator;                  ///< 개시자
        std::set<std::string> setPending;          ///< 초대했고 최종 응답 전인 멤버
        std::set<std::string> setRequiredPending;  ///< 그 가운데 필수 멤버
        std::set<std::string> setInvited;          ///< 초대한 멤버 전원
        std::set<std::string> setAnswered;         ///< 200 을 보낸 멤버
        int iOkCount = 0;                          ///< 멤버 200 누계 (§10.1.1.4.1.1 3))
        int iMinToStart = 0;                       ///< <on-network-minimum-number-to-start>
        bool bTng1 = false;                        ///< TNG1 동작 중
        bool bTng1Expired = false;                 ///< TNG1 만료됨 — 최소 인원을 기다리는 중일 수 있다
        std::chrono::steady_clock::time_point tTng1End;
        bool bProceed = false;         ///< 만료 동작 = proceed (아니면 abandon)
        bool bRequiredMissed = false;  ///< 필수 멤버 하나 이상 없이 진행 — 200 에 Warning 111
        int iBestFinal = 0;            ///< 캐시한 최종 거절 코드 (전원 거절 시 개시자에게)
        bool bPrivate = false;         ///< 사설 호 — 착신자의 180 을 개시자에게 옮긴다(§11.1.1.4.2)
        bool bRingSent = false;        ///< 개시자에게 180 을 보냈다
        std::vector<std::string>
            vecWarnings;  ///< 멤버 응답에서 받은 Warning 값 — 개시자 200 OK 에 옮긴다(§6.3.3.2.3.2 7))
        /** 개시자 수락(Warning 값, P-Answer-State: Unconfirmed 여부) — 0 계속·1 청취·-1 실패 */
        std::function<int( const std::string &, bool )> fnAnswer;
    };
    std::map<std::string, AckGate> m_mapAckGate;  ///< 그룹 → 게이트 (m_mutex)
    /** 미응답 멤버 알림(TS 24.379 §6.3.3.3) 대기 — 개시자 200 OK(Warning 111) 뒤 ACK 가 닿을 만큼 두고 INFO 로 보낸다
     *  (psip 은 ACK 를 올리지 않는다 — 1초 주기 CheckAckGates 가 기한이 지난 것을 보낸다). */
    struct PendingNonAckInfo {
        std::string strCallId;  ///< 개시자 leg
        std::string strGroupId;
        std::vector<std::string> vecNonAck;  ///< 200 을 보내지 않은 초대 멤버
        time_t tDue = 0;
    };
    std::vector<PendingNonAckInfo> m_vecNonAckInfo;  ///< (m_mutex)
    /** 개시자가 미응답 멤버 알림을 받을 자격이면(user profile allow-to-receive-non-acknowledged-users-information)
     * 예약한다. */
    void QueueNonAckInfo( const std::string &strGroupId, const AckGate &clsGate );
    void SendDueNonAckInfo();
    /** 멤버 초대 결과(200 또는 최종 거절 코드)를 게이트에 반영하고 판정한다. */
    void AckGateMemberResult( const std::string &strGroupId, const std::string &strMemberId, int iSipStatus );
    /** 게이트 판정 — 응답·중단·대기. m_mutex 밖에서 부른다. */
    void AckGateEvaluate( const std::string &strGroupId );
    /** TNG1 만료 검사 — MonitorLoop 1초 주기. */
    void CheckAckGates();
    /** 개시 중단 — 개시자에게 최종 응답(iSipStatus>0, Warning)을 주고(CANCEL 이면 0) 초대 leg 을 걷고 세션을 해제한다.
     */
    void AbortAckGate( const std::string &strGroupId, const AckGate &clsGate, int iSipStatus,
                       const std::string &strWarning, const char *pszReason );
    /** 개시 선점을 푼다(개시자 leg 확립) 또는 지운다(개시 실패). 이 호출자가 선점한 것일 때만. */
    void SettlePendingSession( const std::string &strGroupId, const std::string &strInitiator, bool bEstablished );
    /** 일제 통화가 진행 중인가 — 확정된(개시자 leg 확립) 세션만. m_mutex 를 잡는다. */
    bool IsBroadcastInProgress( const std::string &strGroupId );
    /** 세션 속성 스냅샷 (없으면 기본값). m_mutex 를 잡는다. */
    GroupSession SessionOf( const std::string &strGroupId );
    /** CMP 로 싣는 세션 속성 — 개시자·일제 통화 + T4. T4 출처는 호 종류별(TS 24.380 표 11.1.3-1): 편성 그룹 호 = 그룹
     *  hang-timer, 애드혹 그룹 호·개별 호 = service configuration(<adhoc-group-call>·<private-call>, TS 24.484
     *  §8.4.2.7), chat = 0 (CspSessionT4Sec). */
    CmpGroupSession CmpSessionOf( const CspPttGroup &clsGroup );
    /** on-demand 편성 그룹 호인가 — chat(상시)·즉석 세션(private·ad hoc)은 아니다. */
    static bool IsOnDemandGroupCall( const CspPttGroup &clsGroup );
    /** 일제 통화로 개시할 수 있는 세션인가 — 편성 그룹 on-demand 호 또는 ad hoc 그룹 호(TS 24.379 §4.12 · §17.2.2.1.1
     * 9) — broadcast adhoc group call). chat(상시 채널 합류)·개별 호(private)는 아니다. */
    static bool IsBroadcastCapable( const CspPttGroup &clsGroup );
    /** 개시 INVITE 의 암묵적 발언 요청을 받아들이는가 — TS 24.380 §14.3.5: offer 의 `mc_implicit_request`(§14.2.5)를 새
     * 세션 개시에서만 받고 chat 그룹 호 합류·진행 중 편성/ad hoc 호 합류·청취(recvonly) 합류는 제외한다. offer 의
     * `mc_granted` 는 능력 표시라 여기서 보지 않는다(§12.1.2.2 NOTE 2). */
    static bool AcceptsImplicitFloorRequest( const struct McpttFmtp &clsOffer, const CspPttGroup &clsGroup,
                                             bool bNewSession, bool bListen );
    /** 개시자 200 OK answer 의 `a=fmtp:MCPTT` 파라미터(§14.3.1 — offer 에 없던 파라미터는 싣지 않는다): mc_queueing 은
     * offer 가 실었을 때(fmtp 없는 구단말 offer 는 종전대로 광고), mc_implicit_request 는 암묵 요청을 받아들였을
     * 때(§14.3.5 — 승인 뜻은 아니다, §12.1.2.2 NOTE 4). 승인은 CMP 의 Floor Granted 로만 알린다(answer mc_granted 는
     * 선택 "may" — §14.3.4). */
    /** 제어 기능 answer 의 fmtp:MCPTT (TS 24.380 §14.3) — offer 에 있던 파라미터만. bQueueSupported = 이 호가 큐잉을
     *  지원하는가(개별 호 아님), iNegotiatedPrio = NegotiatedFloorPriority 값(offer 에 mc_priority 가 있을 때만
     * 싣는다). */
    static std::string AnswerFloorFmtp( const struct McpttFmtp &clsOffer, bool bImplicitAccepted, bool bQueueSupported,
                                        int iNegotiatedPrio );
    /** 제어 기능의 멤버 초대 offer fmtp:MCPTT (§14.2.2·§14.2.3) — mc_queueing(개별 호 아님) · mc_priority = 그 멤버
     *  <user-priority>. 빈 값이면 fmtp 를 싣지 않는다. */
    static std::string MemberFloorOfferFmtp( const class CspPttGroup &clsGroup, const std::string &strMember );
    /** floor 우선순위 협상값 (§14.3.3 2)a)) = min(offer mc_priority, 그룹 문서 <user-priority>,
     *  <num-levels-priority-hierarchy>). iOffered <= 0(미협상)이면 0. */
    static int NegotiatedFloorPriority( const class CspPttGroup &clsGroup, const std::string &strMember, int iOffered );
    /** 제어 기능이 보내는 이어지는 offer(조건 재광고 re-INVITE)의 `a=fmtp:MCPTT` 에서 개시 전용 파라미터를 뺀다 —
     * mc_granted 는 이어지는 offer 에 싣지 않고(TS 24.380 §14.5), mc_implicit_request 는 단말의 격상 요청에만 뜻이
     * 있다(§14.5 · TS 24.379 §6.4). 응답한 leg 의 재광고는 다이얼로그의 로컬 선언(개시 answer)으로 offer 를 만들므로
     * 본문(단일 SDP)을 고친다. 남는 파라미터가 없으면 fmtp 줄을 지운다(빈 fmtp 는 RFC 4566 문법 밖). */
    static void StripInitialOnlyFloorFmtp( CSipMessage *pclsOffer );
    /** 그룹 호 해제 (TS 24.379 §6.3.8.1) — 참가 leg(확립·미확립·청취) 전부 BYE/CANCEL 후 마지막 leg 의 teardown 이
     *  CMP REMOVE·세션 정리를 끝낸다. pszReason 은 로그용. */
    void ReleaseGroupSession( const std::string &strGroupId, const char *pszReason );
    /** 최대 시간 만료 세션 해제 — 편성 그룹 호 TNG3(그룹 문서 on-network-maximum-duration) · 애드혹 그룹 호
     *  TNG3(service configuration max-duration-of-call, §17.4.2.2 13)) · 개별 호 최대 통화 시간(§6.3.8.2 2)) — 와
     *  TNG2(진행 중 긴급 그룹콜 타이머) 만료 긴급 해제. MonitorLoop 1초 주기. 긴급 상태 동안은 TNG3 를 세지 않는다
     *  (TS 24.379 §6.3.3.5.2). */
    void CheckSessionLimits();
    /** OnInCallConditionRequest 의 판정·전이 본체 — Warning 149 의 스택 연결은 호출측(공개 함수)이 한다. */
    InCallConditionVerdict EvaluateInCallCondition( const std::string &strGroupId, const std::string &strMemberId,
                                                    const CMcpttInfo &clsMi );
    /** 조건 설정 공통 — 상태·개시자·긴급 사용자·TNG2 기점(긴급 첫 설정). m_mutex 보유 상태에서 호출. */
    void SetGroupConditionLocked( const std::string &strGroupId, int iCond, const std::string &strUser );
    /** 그룹 세션 sesid 조회. 없으면 새로 발행하여 저장. */
    std::string GetOrIssueGroupSesId( const std::string &strGroupId );
    /** 그룹 세션 종료 시 캐시 제거 (PTT_GROUP_REMOVE 호출 시점) */
    void RemoveGroupSesId( const std::string &strGroupId );

    struct CallSessionInfo {
        std::string strGroupId;
        std::string strMemberId;
        std::string strSessionId;
        /** 확립된 leg 여부 — 발신자(AcceptCall)=즉시 true, fan-out 초대=200 OK(OnCallStarted)에서 true.
         *  세션 활성/마지막 이탈 판정은 확립 leg 만 센다 — 미응답 pending INVITE 가 세션을 붙들어
         *  전원 이탈 후에도 PTT_GROUP_REMOVE 가 밀리는 좀비 세션 방지. */
        bool bEstablished = false;
        /** PTT 그룹콜 청취 leg (dispatch_center.md §5.6 — 관제사의 recvonly 합류). 세션 활성·마지막 이탈 판정에서
         *  제외(청취자는 세션을 붙들지 못한다), 참가자 DB/이력에 남기지 않고 감사(E-AUD-016)로 남긴다. */
        bool bListenOnly = false;
        bool bListenHidden = true;   ///< 관제 그룹 listen_visibility=hidden — 로스터(RFC 4575)·통지에서 은닉
        std::string strListenGroup;  ///< 청취자의 역할 id (감사 E-AUD-016 `role` 상관 키)
        time_t tListenStart = 0;
        bool bInitiator = false;  ///< 세션 개시자 leg (ProcessGroupCall) — dialog direction=initiator
    };
    /** dialog 이벤트를 낼 참가 leg 스냅샷 — 맵 락 밖에서 통지하기 위해 복사해 둔다. */
    struct PttDialogLeg {
        std::string strCallId, strUser, strGroupId;
        bool bInitiator = false;
        bool bListen = false;
    };
    static PttDialogLeg _pttLegOf( const std::string &strCallId, const CallSessionInfo &clsInfo ) {
        PttDialogLeg leg;
        leg.strCallId = strCallId;
        leg.strUser = clsInfo.strMemberId;
        leg.strGroupId = clsInfo.strGroupId;
        leg.bInitiator = clsInfo.bInitiator;
        leg.bListen = clsInfo.bListenOnly;
        return leg;
    }
    /** 락 밖에서 호출 — 청취 leg 는 무동작. */
    void EmitPttDialog( const PttDialogLeg &leg, const char *pszState );
    /** dialog `<mcptt>` 확장 요소 — 세션 종류(private|adhoc|prearranged|chat)·session-id·개시자·일제
     * 통화(broadcast)·긴급/임박. */
    std::string BuildPttDialogExt( const std::string &strGroupId );
    static std::string PttSessionUri( const std::string &strGroupId );
    /** 즉석 세션(priv-/adhoc-) 관측 인가 — 청취 leg 합류·conference 구독 공용 (dispatch_center.md §5.6a).
     *  참가자는 항상. 그 외는 자격 allow_ambient_listening + 참가자 중 한 명이 관제 그룹 monitor_scope(CanWatch) 안. */
    static bool CanObserveEphemeral( const CspPttGroup &clsGroup, const std::string &strUserId, std::string &strReason,
                                     bool *pbUnavailable = nullptr );
    // CallId -> Info
    std::map<std::string, CallSessionInfo> m_mapCallSession;

    /** 그룹의 활성(확립·비청취) leg 존재 — 세션 활성 판정 단일 기준. m_mutex 보유 상태에서 호출. */
    bool HasActiveLeg( const std::string &strGroupId ) const;
    /** 사용자가 지금 들어 있는 MCPTT 그룹 호 수 — N6 판정(TS 24.379 §10.1.1.3.1.1 5)). 확립 leg 또는 그 사용자가 개시한
     *  leg 이 있는 그룹을 센다(서버가 보낸 초대에 아직 답하지 않은 그룹은 아니다). 개별 호(priv-)와 strExceptGroup(같은
     * 그룹 재합류·re-INVITE)은 빼고, 청취 leg 은 센다. 호출자가 m_mutex 보유. */
    int ActiveGroupCallsOfLocked( const std::string &strUser, const std::string &strExceptGroup ) const;
    /** 그룹 세션의 참가 leg 수(청취 leg 제외, 확립·초대 중 모두) — 정원 판정(§10.1.1.4.2 15)d)). strUser 의 leg 은 세지
     * 않고 있으면 *pbUserIn = true. 호출자가 m_mutex 보유. */
    int ParticipantLegsLocked( const std::string &strGroupId, const std::string &strUser, bool *pbUserIn ) const;
    /** 초대 대상을 정원 안으로 (§6.3.5.5) — iSlots(개시자를 뺀 자리)를 넘으면 필수 멤버(<on-network-required>)를 먼저
     * 두고 나머지는 순서대로 자른다. 잘랐으면 bCapped. iSlots < 0 = 상한 없음. */
    static std::vector<std::string> CapInvitees( const class CspPttGroup &clsGroup,
                                                 const std::vector<std::string> &vecIn, int iSlots, bool &bCapped );
    /** PTT 청취 감사 이벤트 (E-AUD-016 call_monitored, tap_mode=ptt_listen) — started/ended/denied.
     *  strRole = 청취자 역할 id (dispatch_center.md §5.7). */
    static void EmitPttListenAudit( const char *pszPhase, const std::string &strMonitor, const std::string &strRole,
                                    const std::string &strPttGroup, const std::string &strSesId, int iDurMs );
    /** 긴급/임박 모드 전이 이벤트 (E-STC-007 emergency_mode_changed) — activated/cancelled.
     *  세션 이력(PttLogEvent)에만 남던 전이를 운용 이벤트 스트림에도 올린다 — 세션을 열지
     *  않고 기간으로 조회할 수 있어야 한다 (alarm_catalog.md §10.1). */
    static void EmitEmergencyModeEvent( const char *pszAction, int iTier, const std::string &strGroupId,
                                        const std::string &strActor, const std::string &strSesId );

    // Track Active Calls ((UserId, GroupId) -> CallId)
    //   멀티그룹 동시 참여: 사용자는 그룹별 독립 다이얼로그를 가진다 (그룹당 1콜).
    std::map<std::pair<std::string, std::string>, std::string> m_mapUserCall;

    // BYE 처리 중 race condition 방지: OnCallTerminated 호출 시 그룹별 최종 종료 시각 기록.
    // CheckGroupIntegrity가 BYE 처리 틈새에서 재-INVITE하지 않도록 5초 grace period 부여.
    std::map<std::string, std::chrono::steady_clock::time_point> m_mapGroupLastTerminate;

    /** CMP 멤버 해제에 실패한 건 — 재시도 대기열(§5.10). 세션 맵에는 되돌리지 않는다(되돌리면 이미 끝난
     *  leg 이 살아 있는 것처럼 보인다). 상한 근거 = 마지막 멤버 이탈 시 `PTT_GROUP_REMOVE` 가 그룹을 통째로
     *  걷는다 — 최종 안전망이 따로 있다. */
    struct PendingLeave {
        std::string strGroupId, strSessionId, strSesId;
        int iTries = 0;
        time_t tNextTry = 0;
    };
    /** 대기열 상한 — 안전망. 같은 (group, member) 를 한 건으로 접으므로 정상 운용에서는 살아 있는 멤버 수를
     *  넘지 않는다. 넘었다면 회수가 아니라 상류가 고장난 상태다. */
    static const size_t PENDING_LEAVE_MAX = 256;
    std::vector<PendingLeave> m_vecPendingLeave;

    std::recursive_mutex m_mutex;
};

extern CGroupCallService gclsGroupCallService;

#endif
