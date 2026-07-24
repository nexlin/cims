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

    /** 미디어 노드(CMP) 다운으로 relay 가 소실된 그룹의 활성 멤버 호를 능동 종료(BYE)하고 로컬 상태를
     *  정리한다. dead node 이므로 CmpClient(LeaveGroup/RemoveGroup, blocking)는 호출하지 않는다.
     *  종료한 멤버 호 수를 반환. */
    int TerminateGroupLocal( const std::string &strGroupId );
    void OnCallStarted( const std::string &strCallId, const std::string &strRemoteIp, int iRemotePort,
                        int iRemoteFloorPort = 0, int iRemoteVideoPort = 0 );

    /** Called by CSC interface when group/user config changes externally */
    void OnGroupConfigChanged();

    /**
     * @brief Send RFC 4575 conference-info NOTIFY to all active participants in a group
     * @param strGroupId  Group ID
     * @param strChangedUser  The user entity that changed (added/removed/joined/left)
     * @param strStatus  "connected", "disconnected", "pending"
     * @param strJoining "added", "removed", "updated"
     */
    void SendConferenceNotify( const std::string &strGroupId, const std::string &strChangedUser,
                               const std::string &strStatus, const std::string &strJoining );

private:
    void MonitorLoop();
    void SyncGroupsState();
    void CheckMemberState();
    void CheckGroupIntegrity();

    /** 그룹 멤버 구성(id:priority 순서)의 해시. SyncGroupsState 의 "Config Changed" 판정 기준.
     *  그룹 컨텍스트(m_mapGroupRtp)를 만드는 모든 경로(SyncGroupsState/InviteMember/CheckGroupIntegrity)
     *  에서 동일하게 저장해야 한다. 0(미설정)으로 두면 다음 SyncGroupsState 가 실제해시와 불일치로
     *  착각해 스퓨리어스 ModifyGroup + group_change NOTIFY storm 을 일으켜 멤버 무더기 drop 됨. */
    static size_t ComputeMemberHash( const class CspPttGroup &group );

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
        int iPort;
        int iFloorPort;
        int iVideoPort;
        std::string strIp;
        size_t nMemberHash;
        std::string strSessionCallId;
        std::string strCallerId;
        bool bVideoEnabled;
        int iConfVersion;  // RFC 4575 conference-info version counter
    };
    std::map<std::string, GroupRtpInfo> m_mapGroupRtp;

    /** 그룹 세션의 현재 condition(0=normal/1=imminent/2=emergency). 진행 중 emergency/imminent 상태.
     *  ProcessGroupCall(개시) 시 설정, fan-out INVITE(mcptt-info emergency-ind 광고)·업그레이드에서 참조. */
    std::map<std::string, int> m_mapGroupCondition;
    /** condition 을 마지막으로 올린 멤버(actor) — 취소(하향) 권한 판정용(개시자만 취소). */
    std::map<std::string, std::string> m_mapGroupCondActor;

    /** 그룹 세션 단위 통일 sesid: ADD_PTT_GROUP ~ JOIN/LEAVE ~ INVITE ~ REMOVE_PTT_GROUP 모두 동일 sesid 사용.
     *  key = group_id, value = sesid (형식: `{group_id}::csp::{us_ts}::{counter}`).
     *  GetOrIssueGroupSesId() 로 조회/발행, RemoveGroupSesId() 로 세션 종료 시 정리. */
    std::map<std::string, std::string> m_mapGroupSesId;
    /** 그룹 세션 sesid 조회. 없으면 새로 발행하여 저장. */
    std::string GetOrIssueGroupSesId( const std::string &strGroupId );
    /** 그룹 세션 종료 시 캐시 제거 (REMOVE_PTT_GROUP 호출 시점) */
    void RemoveGroupSesId( const std::string &strGroupId );

    struct CallSessionInfo {
        std::string strGroupId;
        std::string strMemberId;
        std::string strSessionId;
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
