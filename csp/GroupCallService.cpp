/*
 * Group Call Service Source
 */

#include "GroupCallService.h"
#include "GroupMap.h"
#include "SipServer.h"
#include "Log.h"
#include "SipUserAgent.h"
#include "CallMap.h"
#include "CmpClient.h"
#include "RtpMap.h"
#include "UserMap.h"
#include "SipServerSetup.h"
#include "CspPttGroup.h"
#include "SipMessage.h"
#include <sstream>

// Notify subscribers about group changes
extern void SendSipNotify(const std::string& uri, const std::string& etag, const std::string& action);


// External global objects
extern CSipUserAgent gclsUserAgent;

CGroupCallService gclsGroupCallService;

CGroupCallService::CGroupCallService() : m_bMonitorRunning(false) {
}

CGroupCallService::~CGroupCallService() {
    StopMonitor();
}

/**
 * @brief Process Incoming Group Call (A calling Group)
 */
bool CGroupCallService::ProcessGroupCall( const char *pszGroupId, const char *pszCallerInfo, const char *pszCallId,
                                          CSipCallRtp *pclsRtp, CSipCallRoute *pclsRoute ) {
    CspPttGroup clsGroup;

    if ( gclsGroupMap.Select( pszGroupId, clsGroup ) == false ) {
        return false;
    }

    CLog::Print( LOG_INFO, "Processing Group Call GroupId(%s) Name(%s) Caller(%s)",
                 pszGroupId, clsGroup._name.c_str(), pszCallerInfo );

    // 1. CMP 공유 RTP 포트 확보 (포트가 0이면 재시도)
    int iSharedPort = -1;
    std::string strSharedIp;
    if ( m_mapGroupRtp.find(pszGroupId) != m_mapGroupRtp.end() &&
         m_mapGroupRtp[pszGroupId].iPort > 0 ) {
        iSharedPort = m_mapGroupRtp[pszGroupId].iPort;
        strSharedIp = m_mapGroupRtp[pszGroupId].strIp;
    }
    if ( iSharedPort <= 0 ) {
        if ( gclsCmpClient.AddGroup( pszGroupId, clsGroup._pusers, strSharedIp, iSharedPort ) ) {
            m_mapGroupRtp[pszGroupId] = { iSharedPort, strSharedIp, 0 };
        }
    }

    // 2. 발신자(Caller)에게 공유 RTP 포트로 200 OK 응답
    if ( iSharedPort > 0 ) {
        CSipCallRtp clsCallerRtp;
        clsCallerRtp.SetIpPort( strSharedIp.c_str(), iSharedPort, SOCKET_COUNT_PER_MEDIA );
        clsCallerRtp.m_iCodec = pclsRtp ? pclsRtp->m_iCodec : 0;
        clsCallerRtp.m_clsCodecList.push_back(0);
        clsCallerRtp.m_clsCodecList.push_back(8);
        if ( !gclsUserAgent.AcceptCall( pszCallId, &clsCallerRtp ) ) {
            CLog::Print( LOG_ERROR, "ProcessGroupCall: AcceptCall failed for Caller(%s)", pszCallerInfo );
            return false;
        }
        // 발신자 호출 추적
        m_mapUserCall[pszCallerInfo] = pszCallId;
        m_mapCallSession[pszCallId] = { pszGroupId, pszCallerInfo, pszCallerInfo };
        CLog::Print( LOG_INFO, "ProcessGroupCall: AcceptCall OK → Caller(%s) SharedPort(%d)", pszCallerInfo, iSharedPort );
    } else {
        CLog::Print( LOG_ERROR, "ProcessGroupCall: No shared RTP port for Group(%s)", pszGroupId );
        return false;
    }

    // 3. 나머지 멤버들에게 INVITE
    for ( const auto& pUser : clsGroup._pusers ) {
        if (!pUser) continue;
        std::string strMember = pUser->_id;
        if (strMember == pszCallerInfo) continue;
        InviteMember(strMember.c_str(), pszGroupId);
    }

    return true;
}

/**
 * @brief Invite a member to a group call using Shared RTP Session
 */
bool CGroupCallService::InviteMember( const char *pszUserId, const char *pszGroupId ) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    
    // Check active
    if ( m_mapUserCall.find(pszUserId) != m_mapUserCall.end() ) {
        return false;
    }
    
    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    int iSharedPort = -1;
    std::string strSharedIp;

    // 0. Verify Group Membership (Requirement: Only invite if user is explicitly in group config)
    CspPttGroup clsGroup;
    if ( gclsGroupMap.Select( pszGroupId, clsGroup ) ) {
        bool bIsMember = false;
        for ( const auto& pUser : clsGroup._pusers ) {
            if ( pUser && pUser->_id == pszUserId ) {
                bIsMember = true;
                break;
            }
        }
        if ( !bIsMember ) {
            CLog::Print( LOG_DEBUG, "InviteMember(%s) User NOT in Group(%s) member list. Skipping invitation.", pszUserId, pszGroupId );
            return false;
        }
    } else {
        CLog::Print( LOG_ERROR, "InviteMember(%s) Group config not found for %s", pszUserId, pszGroupId );
        return false;
    }

    // 1. Check User 
    if ( !gclsUserMap.Select( pszUserId, clsUserInfo ) ) {
        // CspUser (JSON) does not store dynamic IP/Port. Only UserMap (Cache) does.
        // If not in UserMap, user is not registered/active.
        CLog::Print( LOG_ERROR, "InviteMember(%s) User not found in UserMap", pszUserId );
        return false;
    }
    clsUserInfo.GetCallRoute( clsRoute );

    // 2. Get Shared Group Port
    if ( m_mapGroupRtp.find(pszGroupId) != m_mapGroupRtp.end() ) {
        iSharedPort = m_mapGroupRtp[pszGroupId].iPort;
        strSharedIp = m_mapGroupRtp[pszGroupId].strIp;
    } else {
        // Try to allocate now
        CspPttGroup clsGroup;
        if ( gclsGroupMap.Select( pszGroupId, clsGroup ) ) {
            if ( gclsCmpClient.AddGroup( pszGroupId, clsGroup._pusers, strSharedIp, iSharedPort ) ) {
                 m_mapGroupRtp[pszGroupId] = { iSharedPort, strSharedIp, 0 }; // Initial hash 0 or calc
            } else {
                 CLog::Print( LOG_ERROR, "InviteMember(%s) Failed to get/alloc Shared Port for Group %s", pszUserId, pszGroupId );
                 return false;
            }
        } else {
             CLog::Print( LOG_ERROR, "InviteMember(%s) Group config not found for %s", pszUserId, pszGroupId );
             return false;
        }
    }

    // 3. Prepare RTP Info (SDP with Shared Port)
    CSipCallRtp clsRtp;
    // Use Shared IP/Port
    clsRtp.SetIpPort( strSharedIp.c_str(), iSharedPort, SOCKET_COUNT_PER_MEDIA );
    
    // Codecs
    clsRtp.m_clsCodecList.push_back(99); 
    clsRtp.m_clsCodecList.push_back(98);
    clsRtp.m_clsCodecList.push_back(0);   
    clsRtp.m_clsCodecList.push_back(8);   
    clsRtp.m_clsCodecList.push_back(101); 
    clsRtp.m_iCodec = 99; 

    // 4. Create Call
    std::string strCallId;
    CSipMessage *pclsInvite = NULL;

    if ( gclsUserAgent.CreateCall( pszGroupId, pszUserId, &clsRtp, &clsRoute, strCallId, &pclsInvite ) ) {

         // 4-1. Add PTT group info XML to INVITE (multipart/mixed: mcptt-info+xml + SDP)
         if ( pclsInvite != NULL ) {
             // To 헤더: 개인 AOR → 그룹 PSI로 변경 (prearranged 그룹콜 식별)
             pclsInvite->m_clsTo.m_clsUri.Set( SIP_PROTOCOL, pszGroupId, gclsSetup.m_strRealm.c_str() );

             std::string strGroupXml = BuildGroupInfoXml( clsGroup, pszUserId );
             WrapMultipartBody( pclsInvite, strGroupXml, strSharedIp, iSharedPort + 1 );

             // MCPTT capability required
             pclsInvite->AddHeader( "Accept-Contact",
                 "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";+g.3gpp.mcptt;require;explicit" );
             // Callee identity
             char szPCalledParty[256];
             snprintf( szPCalledParty, sizeof(szPCalledParty), "<sip:%s@%s>",
                       pszUserId, gclsSetup.m_strRealm.c_str() );
             pclsInvite->AddHeader( "P-Called-Party-ID", szPCalledParty );
             // Group call priority
             pclsInvite->AddHeader( "Resource-Priority", "mcpttp.6" );
             // isfocus: indicate server is conference focus for this group call
             const std::string& strLocalIp = gclsUserAgent.m_clsSipStack.m_clsSetup.m_strLocalIp;
             int iLocalPort = gclsUserAgent.m_clsSipStack.m_clsSetup.m_iLocalUdpPort;
             char szContact[256];
             snprintf(szContact, sizeof(szContact), "<sip:%s@%s:%d>;isfocus",
                      pszGroupId, strLocalIp.c_str(), iLocalPort);
             pclsInvite->AddHeader("Contact", szContact);
         }

         // Insert into CallMap (But manage Port cleanup ourselves)
         CCallInfo clsCallInfo;
         clsCallInfo.m_bRecv = false;
         clsCallInfo.m_iPeerRtpPort = iSharedPort;
         // Note: CallMap::Delete would try to delete this port if we don't intercept it.
         // Intercept logic handled in EventCallEnd -> OnCallTerminated.

         gclsCallMap.Insert( strCallId.c_str(), clsCallInfo );

         // Track Session Info
         m_mapUserCall[pszUserId] = strCallId;
         m_mapCallSession[strCallId] = { pszGroupId, pszUserId, pszUserId }; // Use UserId as SessionId
         CLog::Print( LOG_DEBUG, "InviteMember(%s): Added to Maps. CallId=%s", pszUserId, strCallId.c_str() );

         if ( !gclsUserAgent.StartCall( strCallId.c_str(), pclsInvite ) ) {
             CLog::Print( LOG_ERROR, "InviteMember StartCall failed" );
             gclsCallMap.Delete( strCallId.c_str() );
             std::unique_lock<std::recursive_mutex> lock(m_mutex);
             m_mapUserCall.erase(pszUserId);
             m_mapCallSession.erase(strCallId);
             return false;
         }
    } else {
         CLog::Print( LOG_ERROR, "InviteMember CreateCall failed" );
         return false;
    }

    CLog::Print( LOG_INFO, "InviteMember(%s) Group(%s) SharedPort(%d) CallId(%s) Initiated", 
                 pszUserId, pszGroupId, iSharedPort, strCallId.c_str() );
    return true;
}

void CGroupCallService::StartMonitor() {
    if ( !m_bMonitorRunning ) {
        m_bMonitorRunning = true;
        m_threadMonitor = std::thread(&CGroupCallService::MonitorLoop, this);
        CLog::Print( LOG_INFO, "GroupCallService Monitor Started" );
    }
}

void CGroupCallService::StopMonitor() {
    m_bMonitorRunning = false;
    if ( m_threadMonitor.joinable() ) {
        m_threadMonitor.join();
        CLog::Print( LOG_INFO, "GroupCallService Monitor Stopped" );
    }
}

void CGroupCallService::MonitorLoop() {
    while ( m_bMonitorRunning ) {
        std::this_thread::sleep_for( std::chrono::seconds(5) );
        if ( !m_bMonitorRunning ) break;
        
        // 1. Reload Group Map (File Monitoring)
        if ( !gclsSetup.m_strGroupDataFolder.empty() ) {
            gclsGroupMap.Load( gclsSetup.m_strGroupDataFolder.c_str() );
        }
        
        // 2. Sync Existence (Add New, Remove Deleted)
        SyncGroupsState();
        
        // 3. Check Calls (Kick removed members)
        CheckMemberState();

        // 4. Invite Missing Members
        CheckGroupIntegrity();
    }
}

void CGroupCallService::SyncGroupsState() {
    // A. Add New Groups
    gclsGroupMap.IterateInternal([this](const CspPttGroup& group) {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        
        // Calculate Hash
        std::string strHashInput;
        for(const auto& pUser : group._pusers) {
            if (!pUser) continue;
            strHashInput += pUser->_id + ":" + std::to_string(pUser->_priority) + ";";
        }
        size_t nHash = std::hash<std::string>{}(strHashInput);

        auto itRtp = m_mapGroupRtp.find(group._id);
        if ( itRtp == m_mapGroupRtp.end() ) {
            // NEW GROUP
            lock.unlock(); // Release lock for network op
            
            std::string ip; int port;
            if ( gclsCmpClient.AddGroup( group._id, group._pusers, ip, port ) ) {
                std::unique_lock<std::recursive_mutex> lock2(m_mutex);
                m_mapGroupRtp[group._id] = { port, ip, nHash };
                CLog::Print( LOG_INFO, "SyncGroupsState: Added Group(%s) -> %s:%d (MemHash:%lu)", group._id.c_str(), ip.c_str(), port, nHash );
            }
        } else {
            // EXISTING GROUP - Check for Diff
            if (itRtp->second.nMemberHash != nHash) {
                // CHANGED
                lock.unlock();
                CLog::Print( LOG_INFO, "SyncGroupsState: Group(%s) Config Changed. Sending ModifyGroup.", group._id.c_str() );
                if ( gclsCmpClient.ModifyGroup( group._id, group._pusers ) ) {
                    std::unique_lock<std::recursive_mutex> lock2(m_mutex);
                    m_mapGroupRtp[group._id].nMemberHash = nHash;
                }
                // Notify GMS subscribers that group config changed
                SendSipNotify("tel:" + group._id, "change_" + std::to_string(time(NULL)), "PUT");
            }
        }
    });

    // B. Remove Deleted Groups
    std::vector<std::string> vecToRemove;
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        for(auto it = m_mapGroupRtp.begin(); it != m_mapGroupRtp.end(); ++it) {
            CspPttGroup group;
            if ( !gclsGroupMap.Select(it->first.c_str(), group) ) {
                vecToRemove.push_back(it->first);
            }
        }
    }

    for(const auto& strGroupId : vecToRemove) {
        CLog::Print( LOG_INFO, "SyncGroupsState: Group(%s) removed from config. Cleaning up.", strGroupId.c_str() );
        // Notify GMS subscribers about group deletion before removing
        SendSipNotify("tel:" + strGroupId, "deleted_" + std::to_string(time(NULL)), "DELETE");
        gclsCmpClient.RemoveGroup(strGroupId);

        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        m_mapGroupRtp.erase(strGroupId);
    }
}

void CGroupCallService::CheckMemberState() {
    std::vector<std::string> vecToKick;
    
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        for(auto it = m_mapUserCall.begin(); it != m_mapUserCall.end(); ++it) {
            std::string strUserId = it->first;
            std::string strCallId = it->second;
            
            auto itSess = m_mapCallSession.find(strCallId);
            if (itSess != m_mapCallSession.end()) {
                std::string strGroupId = itSess->second.strGroupId;
                
                CspPttGroup group;
                if ( !gclsGroupMap.Select(strGroupId.c_str(), group) ) {
                    // Group Gone
                    vecToKick.push_back(strCallId);
                } else {
                    // Check if member still in group
                    bool bFound = false;
                    for(const auto& pUser : group._pusers) {
                        if (!pUser) continue;
                        if (pUser->_id == strUserId) {
                            bFound = true;
                            break;
                        }
                    }
                    if (!bFound) vecToKick.push_back(strCallId);
                }
            }
        }
    }
    
    for(const auto& strCallId : vecToKick) {
        CLog::Print( LOG_INFO, "CheckMemberState: Call(%s) no longer valid (Group/Member removed). Terminating.", strCallId.c_str() );
        gclsUserAgent.StopCall(strCallId.c_str());
        // Force cleanup immediately as EventCallEnd might be delayed or not propagated for local stop
        OnCallTerminated(strCallId);
    }
}

void CGroupCallService::CheckGroupIntegrity() {
    // Re-invite missing members
    gclsGroupMap.IterateInternal([this](const CspPttGroup& group) {
        // First ensure Group Context exists
        {
            std::unique_lock<std::recursive_mutex> lock(m_mutex);
            if (m_mapGroupRtp.find(group._id) == m_mapGroupRtp.end()) {
                // Try sync 
                // Unlock to avoid Sync calling AddGroup blocking? CmpClient is separate lock.
                lock.unlock();
                std::string ip; int port;
                if (gclsCmpClient.AddGroup(group._id, group._pusers, ip, port)) {
                     lock.lock();
                     m_mapGroupRtp[group._id] = { port, ip, 0 }; // Hash 0 for now or calc it
                } else {
                     return; // Skip this group if alloc fails
                }
            }
        }
        
        // CLog::Print( LOG_DEBUG, "CheckGroupIntegrity: Checking Group(%s) Members(%d)", group._id.c_str(), (int)group._pusers.size() );
        for ( const auto& pUser : group._pusers ) {
             if ( !pUser ) continue;
             
             // [GROUP CALL] Only invite if active or online? 
             // Logic: InviteMember sends INVITE.
             // We need User ID.
             std::string strUserId = pUser->_id;
             
             // ...
             // Previously: mem.m_strId
             // Now: pUser->_id (pointer)
             
             // Check if user is already in session? 
             // Logic seems to be: Iterate members, Join/Invite them.
             
             CUserInfo clsUser;
             if ( gclsUserMap.Select( strUserId.c_str(), clsUser ) ) {
                 std::unique_lock<std::recursive_mutex> lock(m_mutex);
                 if ( m_mapUserCall.find(strUserId) == m_mapUserCall.end() ) {
                     lock.unlock();
                     CLog::Print( LOG_DEBUG, "CheckGroupIntegrity: User(%s) in Group(%s) missing from active calls. Inviting.", strUserId.c_str(), group._id.c_str() );
                     InviteMember( strUserId.c_str(), group._id.c_str() );
                 }
             } else {
                 // Debug why user not found
                 // CLog::Print( LOG_DEBUG, "CheckGroupIntegrity: User(%s) not found in UserMap (Not Registered?)", strUserId.c_str() );
             }
        }
    });
}

void CGroupCallService::OnCmpStatusChanged( bool bConnected ) {
    if ( bConnected ) {
        CLog::Print( LOG_INFO, "OnCmpStatusChanged: Connected -> Syncing Groups" );
        SyncGroupsState();
    } else {
        CLog::Print( LOG_INFO, "OnCmpStatusChanged: Disconnected" );
        // Cleanup?
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        m_mapGroupRtp.clear(); 
        // We probably shouldn't clear user calls immediately unless we destroy SIP dialogs.
    }
}

// 200 OK Received -> Join Group Helper
void CGroupCallService::OnCallStarted( const std::string& strCallId, const std::string& strRemoteIp, int iRemotePort ) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_mapCallSession.find(strCallId);
    if (it != m_mapCallSession.end()) {
        CallSessionInfo& info = it->second;
        // Join Group in CMP (Register Peer)
        if ( gclsCmpClient.JoinGroup(info.strGroupId, info.strSessionId, strRemoteIp, iRemotePort) ) {
             CLog::Print( LOG_INFO, "OnCallStarted: Joined Group(%s) Peer(%s:%d)", info.strGroupId.c_str(), strRemoteIp.c_str(), iRemotePort );
        } else {
             CLog::Print( LOG_ERROR, "OnCallStarted: JoinGroup failed for %s", info.strGroupId.c_str() );
        }
    }
}

// BYE/Error -> Leave Group
bool CGroupCallService::OnCallTerminated( const std::string& strCallId ) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    CLog::Print( LOG_DEBUG, "OnCallTerminated: Enter CallId=%s", strCallId.c_str() );
    
    // Check if it's a group call session
    auto it = m_mapCallSession.find(strCallId);
    if (it != m_mapCallSession.end()) {
        CallSessionInfo& info = it->second;
        gclsCmpClient.LeaveGroup(info.strGroupId, info.strSessionId);
        
        m_mapCallSession.erase(it);
        
        // Remove from user map
        for(auto uIt = m_mapUserCall.begin(); uIt != m_mapUserCall.end(); ++uIt) {
            if (uIt->second == strCallId) {
                m_mapUserCall.erase(uIt);
                break;
            }
        }
        CLog::Print( LOG_INFO, "OnCallTerminated: Group Call Terminated. CallId=%s", strCallId.c_str() );
        return true;
    }

    return false;
}

/**
 * @brief Build PTT group info XML body per 3GPP TS 24.379 MCPTT spec
 *        Content-Type: application/vnd.3gpp.mcptt-info+xml
 */
std::string CGroupCallService::BuildGroupInfoXml( const CspPttGroup& clsGroup, const std::string& strUserId )
{
    std::ostringstream oss;

    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        << "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\""
        << " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
        << "  <mcptt-Params>\r\n"
        << "    <session-type>prearranged</session-type>\r\n"
        << "    <mcptt-request-uri>tel:" << strUserId << "</mcptt-request-uri>\r\n"
        << "    <mcptt-calling-user-id>tel:" << clsGroup._id << "</mcptt-calling-user-id>\r\n"
        << "    <mcptt-calling-group-id>tel:" << clsGroup._id << "</mcptt-calling-group-id>\r\n"
        << "  </mcptt-Params>\r\n"
        << "</mcpttinfo>\r\n";

    return oss.str();
}

/**
 * @brief Replace INVITE body with multipart/mixed per 3GPP TS 24.379:
 *        Part 1: application/vnd.3gpp.mcptt-info+xml  (XML first)
 *        Part 2: application/sdp  (SDP with MCPTT floor control m= line)
 */
void CGroupCallService::WrapMultipartBody( CSipMessage * pclsInvite, const std::string& strGroupXml,
                                           const std::string& strFloorIp, int iFloorPort )
{
    if ( pclsInvite == NULL || pclsInvite->m_strBody.empty() ) return;

    const std::string strBoundary = "mcptt";
    std::string strSdp = pclsInvite->m_strBody;

    // SDP 끝에 MCPTT floor control 미디어 라인 추가 (3GPP TS 24.379)
    // m=application: PTT floor control (Grant/Deny/Release) 전용 UDP 포트
    std::ostringstream sdpFloor;
    sdpFloor << "m=application " << iFloorPort << " UDP MCPTT\r\n"
             << "c=IN IP4 " << strFloorIp << "\r\n"
             << "a=floorid:0 mstrm:audio\r\n"
             << "a=fmtp:MCPTT mc_queueing;mc_priority=3\r\n";
    strSdp += sdpFloor.str();

    std::ostringstream oss;
    // Part 1: mcptt-info XML (3GPP MCPTT format)
    oss << "--" << strBoundary << "\r\n"
        << "Content-Type: application/vnd.3gpp.mcptt-info+xml\r\n"
        << "Content-Length: " << strGroupXml.size() << "\r\n"
        << "\r\n"
        << strGroupXml
        << "\r\n";
    // Part 2: SDP with floor control
    oss << "--" << strBoundary << "\r\n"
        << "Content-Type: application/sdp\r\n"
        << "Content-Disposition: render\r\n"
        << "Content-Length: " << strSdp.size() << "\r\n"
        << "\r\n"
        << strSdp
        << "\r\n";
    oss << "--" << strBoundary << "--\r\n";

    pclsInvite->m_strBody = oss.str();
    pclsInvite->m_iContentLength = (int)pclsInvite->m_strBody.size();
    pclsInvite->m_clsContentType.Set( "multipart", "mixed" );
    pclsInvite->m_clsContentType.InsertParam( "boundary", strBoundary.c_str() );
}
