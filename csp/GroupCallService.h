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

    // Recovery & Monitor
    void StartMonitor();
    void StopMonitor();
    void OnCmpStatusChanged( bool bConnected );
    bool OnCallTerminated( const std::string& strCallId );
    void OnCallStarted( const std::string& strCallId, const std::string& strRemoteIp, int iRemotePort );

    /** Called by CSC interface when group/user config changes externally */
    void OnGroupConfigChanged();

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
        std::string strIp;
        size_t nMemberHash;
        std::string strSessionCallId; // active CSP-initiated session call_id for DB logging
        std::string strCallerId;      // 그룹 통화를 개시한 사용자 ID (mcptt-calling-user-id)
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
