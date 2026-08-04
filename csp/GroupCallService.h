/*
 * Group Call Service Header
 */

#ifndef _GROUP_CALL_SERVICE_H_
#define _GROUP_CALL_SERVICE_H_

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class CSipCallRtp;
class CSipCallRoute;

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
     * @return true if group call initiated, false if group not found or error
     */
    bool ProcessGroupCall( const char *pszGroupId, const char *pszCallerInfo, const char *pszCallId,
                           CSipCallRtp *pclsRtp, CSipCallRoute *pclsRoute, int iCondition = 0 );

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

    /** 진행 중 호의 condition(emergency/imminent) 변경 적용 (re-INVITE 업그레이드/취소, TS 24.379).
     *  iNewCond: 2=emergency/1=imminent/0=normal. 상향=멤버가 개시(누구나), 하향=개시자만(권한).
     *  floor tier(CMP)·m_mapGroupCondition 갱신 + 이벤트 로깅. 미디어 재협상은 기존 UA 경로가 처리. */
    void ApplyInCallCondition( const std::string &strGroupId, const std::string &strMemberId, int iNewCond );

    // Recovery & Monitor
    void StartMonitor();
    void StopMonitor();
    void OnCmpStatusChanged( bool bConnected );
    bool OnCallTerminated( const std::string &strCallId );
    void OnCallStarted( const std::string &strCallId, const std::string &strRemoteIp, int iRemotePort,
                        int iRemoteFloorPort = 0, int iRemoteVideoPort = 0, class CSipCallRtp *pclsRtp = NULL );

    /** Called by CSC interface when group/user config changes externally */
    void OnGroupConfigChanged();

    /** CMP 가 유휴 그룹(멤버·활동 없음)을 자체 회수(PTT_GROUP_ABORTED)했을 때 CSP 캐시를 정리한다.
     *  다음 그룹 사용 시 SyncGroupsState/AddGroup 경로가 깨끗한 sesid 로 재수립한다. CmpClient 이벤트 핸들러가 호출. */
    void OnGroupAborted( const std::string &strGroupId );

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
    static void GetLegPt( const std::string &strCallId, bool bServerOffered,
                          int &iUserPt, int &iUserSrcPt, int &iUserTePt, int &iUserSrcTePt,
                          std::string *pstrCodec = NULL );

    /** 멤버 SDP(m=application)의 a=fmtp:MCPTT 협상 결과 파싱 (TS 24.380 §12.1.2.3) —
     *  mc_queueing/mc_priority=N/mc_granted → PTT_JOIN 의 queueing/max_priority/granted.
     *  fmtp:MCPTT 부재(레거시 단말·cspsim 구버전)면 clsFmtp 를 미전송 상태로 둔다
     *  (CMP 기본 동작 유지). fmtp 가 있는데 mc_queueing 이 없으면 queueing=0 (규격: 미협상
     *  멤버의 비선점 요청은 Deny #1). */
    static void ParseMcpttFmtp( class CSipCallRtp *pclsRtp, struct McpttFmtp &clsFmtp );

private:
    void MonitorLoop();
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
     *  착각해 스퓨리어스 ModifyGroup + group_change NOTIFY storm 을 일으켜 멤버 무더기 drop 됨. */
    static size_t ComputeGroupConfigHash( const class CspPttGroup &group );

    /**
     * @brief Build MCPTT call control info XML (application/vnd.3gpp.mcptt-info+xml, TS 24.379)
     * @param clsGroup PTT group info
     * @return XML string
     */
    static std::string BuildGroupInfoXml( const class CspPttGroup &clsGroup, const std::string &strUserId,
                                          const std::string &strCallerId, int iCondition = 0 );

    /**
     * @brief Build group member roster (application/resource-lists+xml, RFC 5366 +
     *        MCPTT group-info 확장으로 멤버별 role/priority 표기)
     * @param clsGroup PTT group info
     * @return XML string
     */
    static std::string BuildResourceListXml( const class CspPttGroup &clsGroup );

    /**
     * @brief 그룹 자기완결 디스크립터 JSON 생성 (group.json 기록용)
     *        — docs/design/features/mcptt_authorization.md §5.
     *        state/updated_at 은 CCallDir::PttSessionStart 가 주입한다.
     * @param clsGroup PTT group info
     * @return JSON object string
     */
    static std::string BuildGroupDescriptor( const class CspPttGroup &clsGroup );

    /**
     * @brief Wrap SDP + MCPTT info XML + roster into multipart/mixed body, update INVITE message
     * @param pclsInvite   INVITE message to modify
     * @param strGroupXml  MCPTT call control info XML (mcptt-info)
     * @param strRosterXml 멤버 로스터 XML (resource-lists); 비면 생략
     * @param strFloorIp   Floor control IP (shared RTP IP)
     * @param iFloorPort   Floor control UDP port
     */
    static void WrapMultipartBody( class CSipMessage *pclsInvite, const std::string &strGroupXml,
                                   const std::string &strRosterXml, const std::string &strFloorIp, int iFloorPort,
                                   const std::string &strGroupUri = "" );

    bool m_bMonitorRunning;
    std::thread m_threadMonitor;

    struct GroupRtpInfo {
        int iFloorPort;  // 그룹 공유 floor control 포트 (>0 = CMP 그룹 유효)
        std::string strIp;
        size_t nConfigHash;  // CMP 재전달이 필요한 설정(로스터·floor 정책)의 지문 — 변경 감지용
        std::string strSessionCallId;
        std::string strCallerId;
        bool bVideoEnabled;
        int iConfVersion;  // RFC 4575 conference-info version counter
        // 멤버별 CMP 전용 RTP 포트 (sid → {audio, video}) — 각 멤버의 SDP 에 이 포트를 광고.
        std::map<std::string, std::pair<int, int>> memberPorts;
    };
    std::map<std::string, GroupRtpInfo> m_mapGroupRtp;

    /** 멤버 전용 CMP 포트 조회 — 캐시(memberPorts) 우선, 없으면 PTT_JOIN ①(선할당)으로 확보.
     *  (늦은 참가자/로스터 외 멤버의 SDP offer 생성 전 호출.) 실패 시 false. */
    bool GetOrAllocMemberPort( const std::string &strGroupId, const std::string &strMemberId, int &iAudioPort,
                               int &iVideoPort );

    /** 그룹 세션의 현재 condition(0=normal/1=imminent/2=emergency). 진행 중 emergency/imminent 상태.
     *  ProcessGroupCall(개시) 시 설정, fan-out INVITE(mcptt-info emergency-ind 광고)·업그레이드에서 참조. */
    std::map<std::string, int> m_mapGroupCondition;
    /** condition 을 마지막으로 올린 멤버(actor) — 취소(하향) 권한 판정용(개시자만 취소). */
    std::map<std::string, std::string> m_mapGroupCondActor;

    /** 그룹 세션 단위 통일 sesid: PTT_GROUP_ADD ~ JOIN/LEAVE ~ INVITE ~ PTT_GROUP_REMOVE 모두 동일 sesid 사용.
     *  key = group_id, value = sesid (형식: `{group_id}::csp::{us_ts}::{counter}`).
     *  GetOrIssueGroupSesId() 로 조회/발행, RemoveGroupSesId() 로 세션 종료 시 정리. */
    std::map<std::string, std::string> m_mapGroupSesId;
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
    };
    // CallId -> Info
    std::map<std::string, CallSessionInfo> m_mapCallSession;

    // Track Active Calls ((UserId, GroupId) -> CallId)
    //   멀티그룹 동시 참여: 사용자는 그룹별 독립 다이얼로그를 가진다 (그룹당 1콜).
    std::map<std::pair<std::string, std::string>, std::string> m_mapUserCall;

    // BYE 처리 중 race condition 방지: OnCallTerminated 호출 시 그룹별 최종 종료 시각 기록.
    // CheckGroupIntegrity가 BYE 처리 틈새에서 재-INVITE하지 않도록 5초 grace period 부여.
    std::map<std::string, std::chrono::steady_clock::time_point> m_mapGroupLastTerminate;

    std::recursive_mutex m_mutex;
};

extern CGroupCallService gclsGroupCallService;

#endif
