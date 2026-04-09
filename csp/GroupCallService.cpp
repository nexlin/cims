/*
 * Group Call Service Source
 */

#include "GroupCallService.h"
#include "GroupMap.h"
#include "SipServer.h"
#include "DbManager.h"
#include "Log.h"
#include "SipUserAgent.h"
#include "CallMap.h"
#include "CmpClient.h"
#include "RtpMap.h"
#include "UserMap.h"
#include "RecordPath.h"
#include "CallDir.h"
#include "SipServerSetup.h"
#include "CspPttGroup.h"
#include "SipMessage.h"
#include <sstream>
#include <ctime>

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

    CLog::Print( LOG_INFO, "Processing Group Call GroupId(%s) Name(%s) Caller(%s) Priority(%d)",
                 pszGroupId, clsGroup._name.c_str(), pszCallerInfo, clsGroup._priority );

    // 세션 시간 확인: 현재시간이 session_start~session_end 범위 내인지
    time_t tNow = time(NULL);
    if ( clsGroup._sessionStart > 0 && tNow < clsGroup._sessionStart ) {
        CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) session not started yet", pszGroupId );
        return false;
    }
    if ( clsGroup._sessionEnd > 0 && tNow > clsGroup._sessionEnd ) {
        CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) session expired", pszGroupId );
        return false;
    }

    // 1. CMP 공유 RTP 포트 확보 (포트가 0이면 재시도)
    int iSharedPort = -1;
    int iSharedVideoPort = 0;
    std::string strSharedIp;
    std::string strRecordDir;  // 녹취 경로 (CSP가 결정)
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        if ( m_mapGroupRtp.find(pszGroupId) != m_mapGroupRtp.end() &&
             m_mapGroupRtp[pszGroupId].iPort > 0 ) {
            iSharedPort = m_mapGroupRtp[pszGroupId].iPort;
            iSharedVideoPort = m_mapGroupRtp[pszGroupId].iVideoPort;
            strSharedIp = m_mapGroupRtp[pszGroupId].strIp;
        }
    }
    if ( iSharedPort <= 0 ) {
        // 녹취 경로: CallDir 통합 디렉터리 사용
        if ( gclsCallDir.IsEnabled() ) {
            strRecordDir = gclsCallDir.GetPttDir(pszGroupId);
            gclsCallDir.PttSessionStart(pszGroupId, pszCallId, pszCallerInfo);
        }
        if ( gclsCmpClient.AddGroup( pszGroupId, clsGroup._pusers, strSharedIp, iSharedPort, iSharedVideoPort, strRecordDir ) ) {
            std::unique_lock<std::recursive_mutex> lock(m_mutex);
            m_mapGroupRtp[pszGroupId] = { iSharedPort, iSharedVideoPort, strSharedIp, 0, "", "", clsGroup._videoEnabled, 0 };
        }
    }

    // 발신자 ID 저장 (XML mcptt-calling-user-id 용)
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        if ( m_mapGroupRtp.find(pszGroupId) != m_mapGroupRtp.end() ) {
            m_mapGroupRtp[pszGroupId].strCallerId = pszCallerInfo;
        }
    }

    // 2. 발신자(Caller)에게 공유 RTP 포트로 200 OK 응답
    if ( iSharedPort > 0 ) {
        CSipCallRtp clsCallerRtp;
        clsCallerRtp.SetIpPort( strSharedIp.c_str(), iSharedPort, SOCKET_COUNT_PER_MEDIA );
        clsCallerRtp.m_iCodec = 99;  // AMR-WB (기본 코덱, 서버 설정으로 추후 변경)
        clsCallerRtp.m_clsCodecList.push_back(99);
        if ( !gclsUserAgent.AcceptCall( pszCallId, &clsCallerRtp ) ) {
            CLog::Print( LOG_ERROR, "ProcessGroupCall: AcceptCall failed for Caller(%s)", pszCallerInfo );
            return false;
        }
        // 발신자 호출 추적
        {
            std::unique_lock<std::recursive_mutex> lock(m_mutex);
            m_mapUserCall[pszCallerInfo] = pszCallId;
            m_mapCallSession[pszCallId] = { pszGroupId, pszCallerInfo, pszCallerInfo };
        }
        CLog::Print( LOG_INFO, "ProcessGroupCall: AcceptCall OK → Caller(%s) SharedPort(%d)", pszCallerInfo, iSharedPort );

        // [CALL LOG] PTT 그룹 세션 기록
        if ( gclsDbManager.IsConnected() ) {
            gclsDbManager.InsertCallLog( pszCallId, true, pszGroupId, pszCallerInfo, pszGroupId );
            gclsDbManager.InsertGroupParticipant( pszGroupId, pszCallerInfo );
            gclsDbManager.UpdateParticipantJoined( pszGroupId, pszCallerInfo );
            // 녹취 DB 레코드 삽입
            if ( gclsSetup.m_bRecordEnable && !strRecordDir.empty() ) {
                gclsDbManager.InsertRecording( pszCallId, "ptt", pszGroupId,
                    pszCallerInfo, pszGroupId, strRecordDir, false );
            }
        }
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

std::string CGroupCallService::GetGroupIdByCallId(const std::string& strCallId) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_mapCallSession.find(strCallId);
    if (it != m_mapCallSession.end()) return it->second.strGroupId;
    return "";
}

void CGroupCallService::ClearUserCall( const std::string& strUserId )
{
    std::string strGroupId, strSessionId;
    bool bStillActive = false;
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        auto it = m_mapUserCall.find(strUserId);
        if ( it == m_mapUserCall.end() ) return;

        std::string strCallId = it->second;
        CLog::Print( LOG_INFO, "ClearUserCall(%s): clearing stale callId=%s",
                     strUserId.c_str(), strCallId.c_str() );

        auto itSess = m_mapCallSession.find(strCallId);
        if ( itSess != m_mapCallSession.end() ) {
            strGroupId  = itSess->second.strGroupId;
            strSessionId = itSess->second.strSessionId;
            m_mapCallSession.erase(itSess);
        }
        m_mapUserCall.erase(it);

        // 해당 그룹에 아직 다른 멤버가 남아있는지 확인
        for ( const auto& kv : m_mapCallSession ) {
            if ( kv.second.strGroupId == strGroupId ) { bStillActive = true; break; }
        }
        if ( !bStillActive && !strGroupId.empty() ) {
            auto itRtp = m_mapGroupRtp.find(strGroupId);
            if ( itRtp != m_mapGroupRtp.end() ) {
                itRtp->second.strSessionCallId.clear();
            }
        }
    }
    // lock 해제 후 CMP/DB 호출
    if ( !strGroupId.empty() ) {
        gclsCmpClient.LeaveGroup( strGroupId, strSessionId );

        if ( gclsDbManager.IsConnected() ) {
            gclsDbManager.UpdateParticipantLeft( strGroupId, strUserId );
            if ( !bStillActive ) {
                gclsDbManager.EndGroupCallLog( strGroupId );
            }
        }
    }
}

/**
 * @brief Invite a member to a group call using Shared RTP Session
 */
bool CGroupCallService::InviteMember( const char *pszUserId, const char *pszGroupId ) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    // 활성 호가 남아있으면 정리 후 재시도
    if ( m_mapUserCall.find(pszUserId) != m_mapUserCall.end() ) {
        std::string strStaleCallId = m_mapUserCall[pszUserId];
        CLog::Print( LOG_INFO, "InviteMember(%s, %s): stale call exists (%s), clearing",
                     pszUserId, pszGroupId, strStaleCallId.c_str() );

        // stale 세션 정리
        std::string strStaleGroup, strStaleSession;
        auto itSess = m_mapCallSession.find(strStaleCallId);
        if (itSess != m_mapCallSession.end()) {
            strStaleGroup = itSess->second.strGroupId;
            strStaleSession = itSess->second.strSessionId;
            m_mapCallSession.erase(itSess);
        }
        m_mapUserCall.erase(pszUserId);

        // lock 해제하고 CMP 정리
        lock.unlock();
        if (!strStaleGroup.empty()) {
            gclsCmpClient.LeaveGroup(strStaleGroup, strStaleSession);
        }
        // 재획득 후 계속 진행
        lock.lock();
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
    int iSharedVideoPort = 0;
    bool bVideoEnabled = false;
    if ( m_mapGroupRtp.find(pszGroupId) != m_mapGroupRtp.end() ) {
        iSharedPort = m_mapGroupRtp[pszGroupId].iPort;
        iSharedVideoPort = m_mapGroupRtp[pszGroupId].iVideoPort;
        strSharedIp = m_mapGroupRtp[pszGroupId].strIp;
        bVideoEnabled = m_mapGroupRtp[pszGroupId].bVideoEnabled;
    } else {
        // Try to allocate now
        CspPttGroup clsGroup;
        if ( gclsGroupMap.Select( pszGroupId, clsGroup ) ) {
            std::string strRecordDir;
            if ( gclsCallDir.IsEnabled() ) {
                strRecordDir = gclsCallDir.GetPttDir(pszGroupId);
                gclsCallDir.PttSessionStart(pszGroupId, "autojoin", pszUserId);
            }
            if ( gclsCmpClient.AddGroup( pszGroupId, clsGroup._pusers, strSharedIp, iSharedPort, iSharedVideoPort, strRecordDir ) ) {
                 bVideoEnabled = clsGroup._videoEnabled;
                 m_mapGroupRtp[pszGroupId] = { iSharedPort, iSharedVideoPort, strSharedIp, 0, "", "", bVideoEnabled, 0 };
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

    // AMR-WB (기본 코덱, 서버 설정으로 추후 변경)
    clsRtp.m_clsCodecList.push_back(99);
    clsRtp.m_iCodec = 99;

    // 4. Create Call
    std::string strCallId;
    CSipMessage *pclsInvite = NULL;

    if ( gclsUserAgent.CreateCall( pszGroupId, pszUserId, &clsRtp, &clsRoute, strCallId, &pclsInvite ) ) {

         // 4-1. Add PTT group info XML to INVITE (multipart/mixed: mcptt-info+xml + SDP)
         if ( pclsInvite != NULL ) {
             // To: 는 개인 AOR 유지 (cwrtc가 WS 클라이언트를 찾는 데 필요)
             // 그룹 식별은 Contact(isfocus), P-Called-Party-ID, XML body로 전달

             // 발신자 ID 조회 (mcptt-calling-user-id)
             std::string strCallerId;
             {
                 auto itRtp = m_mapGroupRtp.find(pszGroupId);
                 if ( itRtp != m_mapGroupRtp.end() && !itRtp->second.strCallerId.empty() )
                     strCallerId = itRtp->second.strCallerId;
                 else
                     strCallerId = pszGroupId; // fallback
             }

             std::string strGroupXml = BuildGroupInfoXml( clsGroup, pszUserId, strCallerId );
             WrapMultipartBody( pclsInvite, strGroupXml, strSharedIp, iSharedPort + 1 );

             // MCPTT capability required
             pclsInvite->AddHeader( "Accept-Contact",
                 "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";+g.3gpp.mcptt;require;explicit" );
             // 단말 자동 응답 요구 (3GPP TS 24.379 §6.3.3.1)
             pclsInvite->AddHeader( "Answer-Mode", "Auto" );
             // Callee identity
             char szPCalledParty[256];
             snprintf( szPCalledParty, sizeof(szPCalledParty), "<sip:%s@%s>",
                       pszUserId, gclsSetup.m_strPttRealm.c_str() );
             pclsInvite->AddHeader( "P-Called-Party-ID", szPCalledParty );
             // Group call priority
             pclsInvite->AddHeader( "Resource-Priority", "mcpttp.6" );
             // isfocus: indicate server is conference focus for this group call (domain-based URI)
             char szContact[256];
             snprintf(szContact, sizeof(szContact), "<sip:%s@%s>;isfocus",
                      pszGroupId, gclsSetup.m_strPttRealm.c_str());
             pclsInvite->AddHeader("Contact", szContact);
             // Session timer (RFC 4028) — required by many MCPTT implementations
             pclsInvite->AddHeader("Session-Expires", "7200;refresher=uac");
             pclsInvite->AddHeader("Min-SE", "180");
             // 비디오 활성화 전달 (cwrtc가 SDP에 H.264 포함 여부 결정)
             if (bVideoEnabled && iSharedVideoPort > 0) {
                 char szVideo[64];
                 snprintf(szVideo, sizeof(szVideo), "%d", iSharedVideoPort);
                 pclsInvite->AddHeader("X-Video-Port", szVideo);
             }
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

         // [CALL LOG] PTT 멤버 초대 기록 (join_time은 OnCallStarted에서 설정)
         if ( gclsDbManager.IsConnected() ) {
             // auto-join 시 해당 그룹에 활성 call log가 없으면 자동 생성
             if ( !gclsDbManager.HasActiveGroupCall( pszGroupId ) ) {
                 std::string strAutoCallId = "csp-autojoin-" + std::string(pszGroupId)
                                             + "-" + std::to_string((long long)time(NULL));
                 gclsDbManager.InsertCallLog( strAutoCallId.c_str(), true,
                                              pszGroupId, "CSP", pszGroupId );
                 gclsDbManager.UpdateCallLogActivePtt( pszGroupId );
             }
             gclsDbManager.InsertGroupParticipant( pszGroupId, pszUserId );
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
    // Initial sync on startup
    SyncGroupsState();
    CheckGroupIntegrity();

    int iTickSec = 0;
    while ( m_bMonitorRunning ) {
        std::this_thread::sleep_for( std::chrono::seconds(1) );
        if ( !m_bMonitorRunning ) break;
        ++iTickSec;

        // Periodic member state check (every 10s) — detects dead calls
        if ( iTickSec % 10 == 0 ) {
            CheckMemberState();
            CheckGroupIntegrity();
        }

        // Heavy group config reload every 60s — DB primary, file fallback (matches CspServer.cpp policy)
        if ( iTickSec % 60 == 0 ) {
            if ( gclsDbManager.IsConnected() ) {
                gclsGroupMap.LoadFromDb();
            } else if ( !gclsSetup.m_strGroupDataFolder.empty() ) {
                gclsGroupMap.Load( gclsSetup.m_strGroupDataFolder.c_str() );
            }
            SyncGroupsState();
            iTickSec = 0;
        }
    }
}

void CGroupCallService::OnGroupConfigChanged() {
    CLog::Print( LOG_INFO, "OnGroupConfigChanged: Reloading group config and re-syncing" );
    if ( gclsDbManager.IsConnected() ) {
        gclsGroupMap.LoadFromDb();
    } else if ( !gclsSetup.m_strGroupDataFolder.empty() ) {
        gclsGroupMap.Load( gclsSetup.m_strGroupDataFolder.c_str() );
    }
    SyncGroupsState();
    CheckMemberState();
    CheckGroupIntegrity();
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
            
            std::string ip; int port; int videoPort = 0;
            if ( gclsCmpClient.AddGroup( group._id, group._pusers, ip, port, videoPort ) ) {
                std::unique_lock<std::recursive_mutex> lock2(m_mutex);
                m_mapGroupRtp[group._id] = { port, videoPort, ip, nHash, "", "", group._videoEnabled, 0 };
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
                lock.unlock();
                std::string ip; int port; int videoPort = 0;
                if (gclsCmpClient.AddGroup(group._id, group._pusers, ip, port, videoPort)) {
                     lock.lock();
                     m_mapGroupRtp[group._id] = { port, videoPort, ip, 0, "", "", group._videoEnabled, 0 };
                } else {
                     return; // Skip this group if alloc fails
                }
            }
        }

        // Collect registered members that need inviting
        std::vector<std::string> vecToInvite;
        for ( const auto& pUser : group._pusers ) {
             if ( !pUser ) continue;
             std::string strUserId = pUser->_id;
             CUserInfo clsUser;
             if ( gclsUserMap.Select( strUserId.c_str(), clsUser ) ) {
                 std::unique_lock<std::recursive_mutex> lock(m_mutex);
                 if ( m_mapUserCall.find(strUserId) == m_mapUserCall.end() ) {
                     vecToInvite.push_back(strUserId);
                 }
             }
        }

        if ( vecToInvite.empty() ) return;

        // Ensure a call log entry exists for this group session before inviting
        if ( gclsDbManager.IsConnected() ) {
            std::unique_lock<std::recursive_mutex> lock(m_mutex);
            auto itRtp = m_mapGroupRtp.find(group._id);
            if ( itRtp != m_mapGroupRtp.end() && itRtp->second.strSessionCallId.empty() ) {
                char szCallId[160];
                snprintf( szCallId, sizeof(szCallId), "csp-group-%s-%ld",
                          group._id.c_str(), (long)time(NULL) );
                itRtp->second.strSessionCallId = szCallId;
                lock.unlock();
                gclsDbManager.InsertCallLog( szCallId, true, group._id, "CSP", group._id );
                CLog::Print( LOG_INFO, "CheckGroupIntegrity: Created call log for Group(%s) CallId(%s)",
                             group._id.c_str(), szCallId );
            }
        }

        for ( const auto& strUserId : vecToInvite ) {
             CLog::Print( LOG_DEBUG, "CheckGroupIntegrity: User(%s) in Group(%s) missing from active calls. Inviting.",
                          strUserId.c_str(), group._id.c_str() );
             InviteMember( strUserId.c_str(), group._id.c_str() );
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
    std::string strGroupId, strSessionId, strMemberId;

    // 1. lock 보유 중 맵 조회만 수행
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        auto it = m_mapCallSession.find(strCallId);
        if (it == m_mapCallSession.end()) return;

        strGroupId  = it->second.strGroupId;
        strSessionId = it->second.strSessionId;
        strMemberId = it->second.strMemberId;
    }
    // 2. lock 해제 후 외부 호출 (CMP, DB)
    // 비디오 포트: cwrtc의 비디오 RTP = 오디오 RTP + 3 (포트 레이아웃: audio_dtls+0, audio_rtp+1, audio_rtcp+2, video_dtls+3, video_rtp+4)
    // iRemotePort = audio RTP → video RTP = iRemotePort + 3
    int iVideoPort = iRemotePort + 3;
    if ( gclsCmpClient.JoinGroup(strGroupId, strSessionId, strRemoteIp, iRemotePort, iVideoPort) ) {
         CLog::Print( LOG_INFO, "OnCallStarted: Joined Group(%s) Peer(%s:%d video=%d)", strGroupId.c_str(), strRemoteIp.c_str(), iRemotePort, iVideoPort );
    } else {
         CLog::Print( LOG_ERROR, "OnCallStarted: JoinGroup failed for %s", strGroupId.c_str() );
    }

    if ( gclsDbManager.IsConnected() ) {
        gclsDbManager.UpdateParticipantJoined( strGroupId, strMemberId );
        gclsDbManager.UpdateCallLogActivePtt( strGroupId );
    }

    // RFC 4575: Notify all active participants about new member joining
    SendConferenceNotify(strGroupId, strMemberId, "connected", "full");
}

// BYE/Error -> Leave Group
bool CGroupCallService::OnCallTerminated( const std::string& strCallId ) {
    std::string strGroupId, strMemberId, strSessionId;
    bool bStillActive = false;
    bool bFound = false;

    // 1. lock 보유 중 맵 조회/수정만 수행 (외부 호출 금지)
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        CLog::Print( LOG_DEBUG, "OnCallTerminated: Enter CallId=%s", strCallId.c_str() );

        auto it = m_mapCallSession.find(strCallId);
        if (it == m_mapCallSession.end()) return false;

        strGroupId  = it->second.strGroupId;
        strMemberId = it->second.strMemberId;
        strSessionId = it->second.strSessionId;

        m_mapCallSession.erase(it);

        for(auto uIt = m_mapUserCall.begin(); uIt != m_mapUserCall.end(); ++uIt) {
            if (uIt->second == strCallId) {
                m_mapUserCall.erase(uIt);
                break;
            }
        }

        for ( const auto& kv : m_mapCallSession ) {
            if ( kv.second.strGroupId == strGroupId ) { bStillActive = true; break; }
        }
        if ( !bStillActive ) {
            auto itRtp = m_mapGroupRtp.find(strGroupId);
            if ( itRtp != m_mapGroupRtp.end() ) {
                itRtp->second.strSessionCallId.clear();
            }
        }
        bFound = true;
    }
    // 2. lock 해제 후 외부 호출 (CMP, DB)
    CLog::Print( LOG_INFO, "OnCallTerminated: Group Call Terminated. CallId=%s", strCallId.c_str() );
    gclsCmpClient.LeaveGroup(strGroupId, strSessionId);

    if ( gclsDbManager.IsConnected() ) {
        gclsDbManager.UpdateParticipantLeft( strGroupId, strMemberId );
        if ( !bStillActive ) {
            gclsDbManager.EndGroupCallLog( strGroupId );
        }
    }

    // RFC 4575: Notify remaining participants about member leaving
    if (bStillActive) {
        SendConferenceNotify(strGroupId, strMemberId, "disconnected", "deleted");
    }

    return bFound;
}

// ─────────────────────────────────────────────────────────
// Conference Event Package (RFC 4575) — in-dialog NOTIFY
// ─────────────────────────────────────────────────────────

void CGroupCallService::SendConferenceNotify(const std::string& strGroupId,
                                              const std::string& strChangedUser,
                                              const std::string& strStatus,
                                              const std::string& strJoining)
{
    // 1. Collect all active call-IDs for this group + bump version
    std::vector<std::string> vecCallIds;
    int iVersion = 0;
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        auto itRtp = m_mapGroupRtp.find(strGroupId);
        if (itRtp != m_mapGroupRtp.end()) {
            itRtp->second.iConfVersion++;
            iVersion = itRtp->second.iConfVersion;
        }

        for (const auto& kv : m_mapCallSession) {
            if (kv.second.strGroupId == strGroupId) {
                vecCallIds.push_back(kv.first);
            }
        }
    }

    if (vecCallIds.empty()) return;

    // 2. Build conference-info+xml body (RFC 4575 partial update)
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        << "<conference-info xmlns=\"urn:ietf:params:xml:ns:conference-info\"\r\n"
        << "  entity=\"sip:" << strGroupId << "@" << gclsSetup.m_strPttRealm << "\"\r\n"
        << "  state=\"partial\" version=\"" << iVersion << "\">\r\n"
        << "  <users>\r\n"
        << "    <user entity=\"tel:" << strChangedUser << "\" state=\"" << strJoining << "\">\r\n"
        << "      <endpoint entity=\"tel:" << strChangedUser << "\">\r\n"
        << "        <status>" << strStatus << "</status>\r\n"
        << "      </endpoint>\r\n"
        << "    </user>\r\n"
        << "  </users>\r\n"
        << "</conference-info>\r\n";
    std::string strBody = oss.str();

    // 3. Send in-dialog NOTIFY to each active participant via SipUserAgent
    for (const auto& strCallId : vecCallIds) {
        gclsUserAgent.SendNotifyWithBody(strCallId.c_str(), "conference",
                                          "application", "conference-info+xml", strBody);
    }

    CLog::Print(LOG_INFO, "SendConferenceNotify: Group(%s) User(%s) Status(%s) Joining(%s) → %d participants",
                strGroupId.c_str(), strChangedUser.c_str(), strStatus.c_str(), strJoining.c_str(), (int)vecCallIds.size());
}


/**
 * @brief Build PTT group info XML body per 3GPP TS 24.379 MCPTT spec
 *        Content-Type: application/vnd.3gpp.mcptt-info+xml
 */
std::string CGroupCallService::BuildGroupInfoXml( const CspPttGroup& clsGroup,
                                                   const std::string& strUserId,
                                                   const std::string& strCallerId )
{
    std::ostringstream oss;

    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        << "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\""
        << " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
        << "  <mcptt-Params>\r\n"
        << "    <session-type>prearranged</session-type>\r\n"
        << "    <mcptt-request-uri>tel:" << strUserId << "</mcptt-request-uri>\r\n"
        << "    <mcptt-calling-user-id>tel:" << strCallerId << "</mcptt-calling-user-id>\r\n"
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
