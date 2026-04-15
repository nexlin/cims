/*
 * Group Call Service Header
 */

#ifndef _GROUP_CALL_SERVICE_H_
#define _GROUP_CALL_SERVICE_H_

#include <string>
#include <map>
#include <mutex>
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
                           CSipCallRtp *pclsRtp, CSipCallRoute *pclsRoute );

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
    void ClearUserCall( const std::string& strUserId );

    /** callId가 PTT 그룹콜에 속하는지 확인. 속하면 groupId 반환, 아니면 빈 문자열 */
    std::string GetGroupIdByCallId(const std::string& strCallId);

    // Recovery & Monitor
    void StartMonitor();
    void StopMonitor();
    void OnCmpStatusChanged( bool bConnected );
    bool OnCallTerminated( const std::string& strCallId );
    void OnCallStarted( const std::string& strCallId, const std::string& strRemoteIp, int iRemotePort, int iRemoteFloorPort = 0, int iRemoteVideoPort = 0 );

    /** Called by CSC interface when group/user config changes externally */
    void OnGroupConfigChanged();

    /**
     * @brief Send RFC 4575 conference-info NOTIFY to all active participants in a group
     * @param strGroupId  Group ID
     * @param strChangedUser  The user entity that changed (added/removed/joined/left)
     * @param strStatus  "connected", "disconnected", "pending"
     * @param strJoining "added", "removed", "updated"
     */
    void SendConferenceNotify(const std::string& strGroupId,
                              const std::string& strChangedUser,
                              const std::string& strStatus,
                              const std::string& strJoining);

private:
    void MonitorLoop();
    void SyncGroupsState();
    void CheckMemberState();
    void CheckGroupIntegrity();

    /**
     * @brief Build PTT group info XML body (application/vnd.oma.poc.groups+xml)
     * @param clsGroup PTT group info
     * @return XML string
     */
    static std::string BuildGroupInfoXml( const class CspPttGroup& clsGroup,
                                          const std::string& strUserId,
                                          const std::string& strCallerId );

    /**
     * @brief Wrap SDP and group XML into multipart/mixed body, update INVITE message
     * @param pclsInvite   INVITE message to modify
     * @param strGroupXml  PTT group info XML string
     * @param strFloorIp   Floor control IP (shared RTP IP)
     * @param iFloorPort   Floor control UDP port
     */
    static void WrapMultipartBody( class CSipMessage * pclsInvite, const std::string& strGroupXml,
                                   const std::string& strFloorIp, int iFloorPort );

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
        int iConfVersion;   // RFC 4575 conference-info version counter
    };
    std::map<std::string, GroupRtpInfo> m_mapGroupRtp;

    struct CallSessionInfo {
        std::string strGroupId;
        std::string strMemberId;
        std::string strSessionId;
    };
    // CallId -> Info
    std::map<std::string, CallSessionInfo> m_mapCallSession;
    
    // Track Active Calls (UserId -> CallId)
    std::map<std::string, std::string> m_mapUserCall; 
    std::recursive_mutex m_mutex;
};

extern CGroupCallService gclsGroupCallService;

#endif
