/*
 * Group Call Service Source
 */

#include "GroupCallService.h"

#include <ctime>

#include "CspServiceMap.h"
#include "DbManager.h"
#include "GroupMap.h"
#include "Log.h"
#include "SipMessageLogger.h"
#include "SipServer.h"

// time_t → ISO string helper
static std::string TimeToIso( time_t t ) {
    if ( t == 0 ) return "";
    char buf[32];
    struct tm tm;
    localtime_r( &t, &tm );
    snprintf( buf, sizeof( buf ), "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
              tm.tm_hour, tm.tm_min, tm.tm_sec );
    return buf;
}
#include <ctime>
#include <sstream>

#include "CallDir.h"
#include "CallMap.h"
#include "CmpClient.h"
#include "CspLocalNodeMap.h"
#include "CspPttGroup.h"
#include "RecordPath.h"
#include "RtpMap.h"
#include "SipMessage.h"
#include "SipServerSetup.h"
#include "SipUserAgent.h"
#include "UserMap.h"

// Notify subscribers about group changes
extern void SendSipNotify( const std::string &uri, const std::string &etag, const std::string &action );

// External global objects
extern CSipUserAgent gclsUserAgent;

CGroupCallService gclsGroupCallService;

CGroupCallService::CGroupCallService() : m_bMonitorRunning( false ) {
}

// ── PTT 그룹 세션 통일 sesid ─────────────────────────────────────
// 그룹 세션이 존재하는 동안 발급된 동일한 sesid 를
// PTT_GROUP_ADD / PTT_JOIN / PTT_LEAVE / PTT_GROUP_REMOVE
// + PTT SIP INVITE/ACK/BYE/NOTIFY 모두에 전달.
std::string CGroupCallService::GetOrIssueGroupSesId( const std::string &strGroupId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    auto it = m_mapGroupSesId.find( strGroupId );
    if ( it != m_mapGroupSesId.end() && !it->second.empty() ) return it->second;
    // caller 자리에 group_id 를 넣어 PTT Flow 검색에서 "group_id in sesid" 매칭 가능
    std::string sid = CSipMessageLogger::IssueSesId( strGroupId, "csp" );
    m_mapGroupSesId[strGroupId] = sid;
    CLog::Print( LOG_INFO, "GroupSesId issued: group=%s sesid=%s", strGroupId.c_str(), sid.c_str() );
    return sid;
}

void CGroupCallService::RemoveGroupSesId( const std::string &strGroupId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    m_mapGroupSesId.erase( strGroupId );
}

void CGroupCallService::OnGroupAborted( const std::string &strGroupId ) {
    RemoveGroupSesId( strGroupId );
    CLog::Print( LOG_INFO, "GroupCallService: group=%s aborted by CMP (idle) — sesid 캐시 정리, 재사용 시 재수립",
                 strGroupId.c_str() );
}

bool CGroupCallService::GetOrAllocMemberPort( const std::string &strGroupId, const std::string &strMemberId,
                                              int &iAudioPort, int &iVideoPort ) {
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto itRtp = m_mapGroupRtp.find( strGroupId );
        if ( itRtp != m_mapGroupRtp.end() ) {
            auto itM = itRtp->second.memberPorts.find( strMemberId );
            if ( itM != itRtp->second.memberPorts.end() && itM->second.first > 0 ) {
                iAudioPort = itM->second.first;
                iVideoPort = itM->second.second;
                return true;
            }
        }
    }
    // 캐시에 없음(늦은 참가자/로스터 외) — PTT_JOIN ①(선할당, user_ip 없이)로 멤버 전용 포트 확보 (멱등)
    int iLocalAudio = 0, iLocalVideo = 0;
    if ( !gclsCmpClient.JoinGroup( strGroupId, strMemberId, "", 0, 0, 0, GetOrIssueGroupSesId( strGroupId ), "",
                                   &iLocalAudio, &iLocalVideo ) ||
         iLocalAudio <= 0 ) {
        CLog::Print( LOG_ERROR, "GetOrAllocMemberPort: PTT_JOIN prealloc failed group=%s member=%s", strGroupId.c_str(),
                     strMemberId.c_str() );
        return false;
    }
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto itRtp = m_mapGroupRtp.find( strGroupId );
        if ( itRtp != m_mapGroupRtp.end() ) itRtp->second.memberPorts[strMemberId] = { iLocalAudio, iLocalVideo };
    }
    iAudioPort = iLocalAudio;
    iVideoPort = iLocalVideo;
    return true;
}

CGroupCallService::~CGroupCallService() {
    StopMonitor();
}

/**
 * @brief Process Incoming Group Call (A calling Group)
 */
bool CGroupCallService::ProcessGroupCall( const char *pszGroupId, const char *pszCallerInfo, const char *pszCallId,
                                          CSipCallRtp *pclsRtp, CSipCallRoute *pclsRoute, int iCondition ) {
    CspPttGroup clsGroup;

    if ( gclsGroupMap.Select( pszGroupId, clsGroup ) == false ) {
        return false;
    }

    // condition(emergency/imminent) 능력 게이트 (TS 24.481). 그룹이 긴급 불허면 normal 로 강등.
    //   (imminent capability 의 per-condition 강제는 CSP DB 로드 보강 후 — 현재 기본 허용.)
    int iCond = iCondition;
    if ( iCond >= 2 && !clsGroup._emergencyCall ) {
        CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) emergency not allowed → downgrade to normal", pszGroupId );
        iCond = 0;
    } else if ( iCond == 1 && !clsGroup._imminentPerilCall ) {
        CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) imminent-peril not allowed → downgrade to normal",
                     pszGroupId );
        iCond = 0;
    }
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        if ( iCond > 0 ) {
            m_mapGroupCondition[pszGroupId] = iCond;
            m_mapGroupCondActor[pszGroupId] = pszCallerInfo;
        } else {
            m_mapGroupCondition.erase( pszGroupId );
            m_mapGroupCondActor.erase( pszGroupId );
        }
    }

    CLog::Print( LOG_INFO, "Processing Group Call GroupId(%s) Name(%s) Caller(%s) Priority(%d)", pszGroupId,
                 clsGroup._name.c_str(), pszCallerInfo, clsGroup._priority );

    // 세션 시간 확인: 현재시간이 session_start~session_end 범위 내인지
    time_t tNow = time( NULL );
    if ( clsGroup._sessionStart > 0 && tNow < clsGroup._sessionStart ) {
        CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) session not started yet", pszGroupId );
        return false;
    }
    if ( clsGroup._sessionEnd > 0 && tNow > clsGroup._sessionEnd ) {
        CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) session expired", pszGroupId );
        return false;
    }

    // 1. CMP 그룹 자원 확보 (floor 공유 포트 + 멤버별 전용 포트)
    int iSharedFloorPort = -1;
    std::string strSharedIp;
    std::string strRecordDir;  // 녹취 경로 (CSP가 결정)
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        if ( m_mapGroupRtp.find( pszGroupId ) != m_mapGroupRtp.end() && m_mapGroupRtp[pszGroupId].iFloorPort > 0 ) {
            iSharedFloorPort = m_mapGroupRtp[pszGroupId].iFloorPort;
            strSharedIp = m_mapGroupRtp[pszGroupId].strIp;
        }
    }
    // 세션 시작 기록 (그룹이 이미 CMP에 있어도 통화 기록은 필요)
    if ( gclsCallDir.IsEnabled() ) {
        strRecordDir = gclsCallDir.GetPttSessionDir( pszGroupId, TimeToIso( clsGroup._sessionStart ),
                                                     std::to_string( clsGroup._dbId ) );
        // 자기완결 그룹 디스크립터 (계획서 §5) — group.json
        std::string strDescriptor = BuildGroupDescriptor( clsGroup );
        gclsCallDir.PttSessionStart( pszGroupId, pszCallId, pszCallerInfo, strDescriptor );
    }

    if ( iSharedFloorPort <= 0 ) {
        // session_seq 증가 (그룹 세션 시작)
        int iSessionSeq = gclsDbManager.IncrementSessionSeq( pszGroupId );
        clsGroup._sessionSeq = iSessionSeq;
        CLog::Print( LOG_INFO, "GroupCall: session_seq=%d for group %s", iSessionSeq, pszGroupId );
        std::string strGroupSesId = GetOrIssueGroupSesId( pszGroupId );
        std::map<std::string, std::pair<int, int>> mapMemberPorts;
        if ( gclsCmpClient.AddGroup( pszGroupId, clsGroup._pusers, strSharedIp, iSharedFloorPort, mapMemberPorts,
                                     strRecordDir, clsGroup._videoEnabled, iSessionSeq, strGroupSesId,
                                     clsGroup._groupType, pszCallerInfo ) ) {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            // nMemberHash 실제값 (0 이면 다음 SyncGroupsState 오탐 → NOTIFY storm → drop).
            m_mapGroupRtp[pszGroupId] = {
                iSharedFloorPort, strSharedIp, ComputeMemberHash( clsGroup ), "", "", clsGroup._videoEnabled, 0,
                mapMemberPorts };
        }
    } else if ( !strRecordDir.empty() ) {
        // 그룹이 이미 CMP에 있지만 record_dir 전달이 필요 → addgroup 재호출 (기존 그룹 유지 — 멱등 경로,
        //   미녹취 그룹이면 이 record_dir 로 녹취 개시)
        std::string tmpIp;
        int tmpFPort = 0;
        std::map<std::string, std::pair<int, int>> tmpMemberPorts;
        std::string strGroupSesId = GetOrIssueGroupSesId( pszGroupId );
        gclsCmpClient.AddGroup( pszGroupId, clsGroup._pusers, tmpIp, tmpFPort, tmpMemberPorts, strRecordDir,
                                clsGroup._videoEnabled, 0, strGroupSesId, clsGroup._groupType, pszCallerInfo );
    }

    // 발신자 ID 저장 (XML mcptt-calling-user-id 용)
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        if ( m_mapGroupRtp.find( pszGroupId ) != m_mapGroupRtp.end() ) {
            m_mapGroupRtp[pszGroupId].strCallerId = pszCallerInfo;
        }
    }

    // 2. 발신자(Caller)에게 caller 전용 CMP 포트로 200 OK 응답 (leg 별 포트셋)
    int iCallerLocalAudio = 0, iCallerLocalVideo = 0;
    if ( iSharedFloorPort > 0 &&
         GetOrAllocMemberPort( pszGroupId, pszCallerInfo, iCallerLocalAudio, iCallerLocalVideo ) ) {
        // PTT 발신 Dialog 도 mcptt realm 사용 (200 OK 의 From/To/Contact 도메인)
        {
            std::string strMcpttDomain = gclsServiceMap.GetDomainByKind( "ptt" );
            if ( !strMcpttDomain.empty() ) gclsUserAgent.SetCallDomain( pszCallId, strMcpttDomain.c_str() );
        }
        CSipCallRtp clsCallerRtp;
        clsCallerRtp.SetIpPort( strSharedIp.c_str(), iCallerLocalAudio, SOCKET_COUNT_PER_MEDIA );
        clsCallerRtp.m_iCodec = 99;  // AMR-WB (기본 코덱, 서버 설정으로 추후 변경)
        clsCallerRtp.m_clsCodecList.push_back( 99 );
        // MCPTT floor (TS 24.379/24.380): 200 OK 에 m=application(SharedFloorPort) 광고 →
        //   개시자가 floor dest 를 학습해 floor REQUEST 를 올바른 포트로 송신(명시적 GRANT).
        clsCallerRtp.m_iApplicationPort = iSharedFloorPort;
        if ( !gclsUserAgent.AcceptCall( pszCallId, &clsCallerRtp ) ) {
            CLog::Print( LOG_ERROR, "ProcessGroupCall: AcceptCall failed for Caller(%s)", pszCallerInfo );
            return false;
        }
        // 발신자 호출 추적
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            m_mapUserCall[{ pszCallerInfo, pszGroupId }] = pszCallId;
            m_mapCallSession[pszCallId] = { pszGroupId, pszCallerInfo, pszCallerInfo };
        }
        CLog::Print( LOG_INFO, "ProcessGroupCall: AcceptCall OK → Caller(%s) MemberPort(%d)", pszCallerInfo,
                     iCallerLocalAudio );

        // 개시자(caller)를 CMP floor/RTP 멤버로 등록.
        //   AcceptCall 만으로는 caller 가 CMP _members 에 없어 onRtpPacket 이 caller RTP 를
        //   drop(미릴레이)하고 floor REQUEST 도 미매칭(GRANT 안 됨)이었다. caller 의 INVITE SDP
        //   audio 포트 + 관례(floor=audio+1, video=audio+2; cspsim RtpThread 와 동일)로 JoinGroup.
        //   (caller INVITE 에 m=application 이 있으면 GetApplicationPort 우선.)
        if ( pclsRtp ) {
            int iCallerAudio = pclsRtp->GetAudioPort();
            if ( iCallerAudio <= 0 ) iCallerAudio = pclsRtp->m_iPort;
            if ( iCallerAudio > 0 ) {
                int iCallerFloor = pclsRtp->GetApplicationPort();
                if ( iCallerFloor <= 0 ) iCallerFloor = iCallerAudio + 1;
                int iCallerVideo = pclsRtp->GetVideoPort();
                if ( iCallerVideo <= 0 && clsGroup._videoEnabled ) iCallerVideo = iCallerAudio + 2;
                std::string strCallerRole = "participant";
                for ( const auto &pUser : clsGroup._pusers ) {
                    if ( pUser && pUser->_id == pszCallerInfo ) {
                        strCallerRole = pUser->_role;
                        break;
                    }
                }
                // 개시자 leg NAT 판정 — SDP 선언 IP vs 등록 바인딩(received/rport latch)
                int iCallerNat = 0;
                std::string strCallerGuardIp;
                {
                    ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( pszCallerInfo, "ptt" );
                    std::string strSigIp;
                    CUserInfo clsCallerInfo2;
                    if ( gclsUserMap.Select( pszCallerInfo, clsCallerInfo2 ) ) strSigIp = clsCallerInfo2.m_strIp;
                    if ( CCspServiceMap::EvalMediaNat( clsNatSvc, pclsRtp->m_strIp, strSigIp, strCallerGuardIp ) )
                        iCallerNat = 1;
                }
                gclsCmpClient.JoinGroup( pszGroupId, pszCallerInfo, pclsRtp->m_strIp, iCallerAudio, iCallerFloor,
                                         iCallerVideo, GetOrIssueGroupSesId( pszGroupId ), strCallerRole, NULL, NULL,
                                         iCallerNat, strCallerGuardIp );
                CLog::Print( LOG_INFO, "ProcessGroupCall: Caller(%s) joined CMP group audio=%d floor=%d role=%s",
                             pszCallerInfo, iCallerAudio, iCallerFloor, strCallerRole.c_str() );
                // 긴급/임박 개시: 개시자에 floor tier 부여 → 하위 tier 발언자 선점 (TS 24.380, Phase 1 엔진).
                if ( iCond > 0 ) {
                    gclsCmpClient.SetFloorTier( pszGroupId, pszCallerInfo, iCond, GetOrIssueGroupSesId( pszGroupId ) );
                    const char *pszEvt = ( iCond >= 2 ) ? "emergency_activated" : "imminent_activated";
                    if ( gclsCallDir.IsEnabled() )
                        gclsCallDir.PttLogEvent(
                            pszGroupId, pszEvt,
                            std::string( "{\"actor\":\"" ) + pszCallerInfo + "\",\"by\":\"initiator\"}" );
                    CLog::Print( LOG_INFO, "ProcessGroupCall: %s on group(%s) initiator(%s) tier=%d", pszEvt,
                                 pszGroupId, pszCallerInfo, iCond );
                }
            }
        }

        // [CALL LOG] PTT 그룹 세션 기록
        if ( gclsDbManager.IsConnected() ) {
            gclsDbManager.InsertCallLog( pszCallId, true, pszGroupId, pszCallerInfo, pszGroupId );
            gclsDbManager.InsertGroupParticipant( pszGroupId, pszCallerInfo );
            gclsDbManager.UpdateParticipantJoined( pszGroupId, pszCallerInfo );
            // 녹취 DB 레코드 삽입
            if ( gclsSetup.m_bRecordEnable && !strRecordDir.empty() ) {
                gclsDbManager.InsertRecording( pszCallId, "ptt", pszGroupId, pszCallerInfo, pszGroupId, strRecordDir,
                                               false );
            }
        }
    } else {
        CLog::Print( LOG_ERROR, "ProcessGroupCall: No shared RTP port for Group(%s)", pszGroupId );
        return false;
    }

    // 3. 나머지 멤버들에게 INVITE (affiliation 요구 그룹은 affiliate 된 멤버만)
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( !pUser ) continue;
        std::string strMember = pUser->_id;
        if ( strMember == pszCallerInfo ) continue;
        if ( clsGroup._requireAffiliation && gclsDbManager.IsConnected() &&
             !gclsDbManager.IsAffiliated( pszGroupId, strMember ) ) {
            CLog::Print( LOG_INFO, "ProcessGroupCall: member %s not affiliated to %s — skip invite", strMember.c_str(),
                         pszGroupId );
            continue;
        }
        InviteMember( strMember.c_str(), pszGroupId );
    }

    return true;
}

std::string CGroupCallService::GetGroupIdByCallId( const std::string &strCallId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    auto it = m_mapCallSession.find( strCallId );
    if ( it != m_mapCallSession.end() ) return it->second.strGroupId;
    return "";
}

bool CGroupCallService::GetGroupCallSession( const std::string &strCallId, std::string &strGroupId,
                                             std::string &strMemberId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    auto it = m_mapCallSession.find( strCallId );
    if ( it == m_mapCallSession.end() ) return false;
    strGroupId = it->second.strGroupId;
    strMemberId = it->second.strMemberId;
    return true;
}

void CGroupCallService::ApplyInCallCondition( const std::string &strGroupId, const std::string &strMemberId,
                                              int iNewCond ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    int iCur = 0;
    {
        auto it = m_mapGroupCondition.find( strGroupId );
        if ( it != m_mapGroupCondition.end() ) iCur = it->second;
    }
    if ( iNewCond == iCur ) return;  // 변화 없음

    std::string strSesId = GetOrIssueGroupSesId( strGroupId );
    if ( iNewCond > iCur ) {
        // 상향(업그레이드): 누구나 emergency/imminent 개시 가능 (TS 24.379). 개시 멤버에 floor tier 부여.
        m_mapGroupCondition[strGroupId] = iNewCond;
        m_mapGroupCondActor[strGroupId] = strMemberId;
        gclsCmpClient.SetFloorTier( strGroupId, strMemberId, iNewCond, strSesId );
        const char *pszEvt = ( iNewCond >= 2 ) ? "emergency_activated" : "imminent_activated";
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.PttLogEvent( strGroupId, pszEvt,
                                     std::string( "{\"actor\":\"" ) + strMemberId + "\",\"by\":\"reinvite\"}" );
        CLog::Print( LOG_INFO, "ApplyInCallCondition: %s group(%s) by(%s) tier=%d", pszEvt, strGroupId.c_str(),
                     strMemberId.c_str(), iNewCond );
    } else {
        // 하향(취소): 개시자(actor)만 가능. 그 외 멤버의 취소 요청은 무시 (TS 24.379 authorized only).
        std::string strActor;
        auto ita = m_mapGroupCondActor.find( strGroupId );
        if ( ita != m_mapGroupCondActor.end() ) strActor = ita->second;
        if ( !strActor.empty() && strActor != strMemberId ) {
            CLog::Print( LOG_INFO, "ApplyInCallCondition: cancel by non-actor(%s) ignored (actor=%s) group(%s)",
                         strMemberId.c_str(), strActor.c_str(), strGroupId.c_str() );
            return;
        }
        const std::string &strTgt = strActor.empty() ? strMemberId : strActor;
        gclsCmpClient.SetFloorTier( strGroupId, strTgt, iNewCond, strSesId );
        const char *pszEvt = ( iCur >= 2 ) ? "emergency_cancelled" : "imminent_cancelled";
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.PttLogEvent( strGroupId, pszEvt,
                                     std::string( "{\"actor\":\"" ) + strTgt + "\",\"by\":\"reinvite\"}" );
        if ( iNewCond <= 0 ) {
            m_mapGroupCondition.erase( strGroupId );
            m_mapGroupCondActor.erase( strGroupId );
        } else {
            m_mapGroupCondition[strGroupId] = iNewCond;
        }
        CLog::Print( LOG_INFO, "ApplyInCallCondition: %s group(%s) by(%s) tier=%d", pszEvt, strGroupId.c_str(),
                     strTgt.c_str(), iNewCond );
    }
}

void CGroupCallService::ClearUserCall( const std::string &strUserId ) {
    // 멀티그룹: 사용자의 모든 그룹 콜을 정리한다 (그룹별 독립 다이얼로그).
    struct ClearItem {
        std::string strCallId;
        std::string strGroupId;
        std::string strSessionId;
        bool bStillActive = false;
    };
    std::vector<ClearItem> vecItems;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        for ( auto it = m_mapUserCall.begin(); it != m_mapUserCall.end(); ) {
            if ( it->first.first != strUserId ) {
                ++it;
                continue;
            }
            ClearItem clsItem;
            clsItem.strCallId = it->second;
            CLog::Print( LOG_INFO, "ClearUserCall(%s): clearing callId=%s", strUserId.c_str(),
                         clsItem.strCallId.c_str() );

            auto itSess = m_mapCallSession.find( clsItem.strCallId );
            if ( itSess != m_mapCallSession.end() ) {
                clsItem.strGroupId = itSess->second.strGroupId;
                clsItem.strSessionId = itSess->second.strSessionId;
                m_mapCallSession.erase( itSess );
            }
            it = m_mapUserCall.erase( it );
            vecItems.push_back( clsItem );
        }
        if ( vecItems.empty() ) return;

        // 그룹별로 아직 다른 멤버가 남아있는지 확인
        for ( auto &clsItem : vecItems ) {
            if ( clsItem.strGroupId.empty() ) continue;
            for ( const auto &kv : m_mapCallSession ) {
                if ( kv.second.strGroupId == clsItem.strGroupId ) {
                    clsItem.bStillActive = true;
                    break;
                }
            }
            if ( !clsItem.bStillActive ) {
                auto itRtp = m_mapGroupRtp.find( clsItem.strGroupId );
                if ( itRtp != m_mapGroupRtp.end() ) {
                    itRtp->second.strSessionCallId.clear();
                }
            }
        }
    }
    for ( const auto &clsItem : vecItems ) {
        // 기존 SIP 다이얼로그 정상 종료(BYE) — 고아 다이얼로그 누수 방지 (1E)
        if ( !clsItem.strCallId.empty() ) {
            gclsUserAgent.StopCall( clsItem.strCallId.c_str() );
            gclsCallMap.Delete( clsItem.strCallId.c_str(), false );
        }
        // lock 해제 후 CMP/DB 호출
        const std::string &strGroupId = clsItem.strGroupId;
        if ( strGroupId.empty() ) continue;
        gclsCmpClient.LeaveGroup( strGroupId, clsItem.strSessionId, GetOrIssueGroupSesId( strGroupId ) );

        // PTT history: member leave event
        if ( gclsCallDir.IsEnabled() ) {
            gclsCallDir.PttMemberLeave( strGroupId, strUserId );
            if ( !clsItem.bStillActive ) {
                gclsCallDir.PttSessionEnd( strGroupId );
            }
        }

        if ( gclsDbManager.IsConnected() ) {
            gclsDbManager.UpdateParticipantLeft( strGroupId, strUserId );
            if ( !clsItem.bStillActive ) {
                gclsDbManager.EndGroupCallLog( strGroupId );
            }
        }

        // on-demand 그룹(prearranged/broadcast): 마지막 멤버 이탈 시 세션 즉시 해제 (chat 은 상시 유지).
        //   stale 캐시로 JOIN→'Group Not Found' 되던 문제도 원천 차단.
        if ( !clsItem.bStillActive ) {
            CspPttGroup clsGrp;
            bool bSelected = gclsGroupMap.Select( strGroupId.c_str(), clsGrp );
            bool bChat = bSelected && clsGrp._groupType == "chat";
            if ( !bChat ) {
                gclsCmpClient.RemoveGroup( strGroupId, GetOrIssueGroupSesId( strGroupId ) );
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                m_mapGroupRtp.erase( strGroupId );
                RemoveGroupSesId( strGroupId );
                // ad hoc 임시 그룹: 통화 종료 시 GroupMap 에서도 제거(ephemeral — 다음 개시 시 새 멤버로 재생성)
                if ( bSelected && clsGrp._isAdhoc ) {
                    gclsGroupMap.Remove( strGroupId.c_str() );
                    CLog::Print( LOG_INFO, "GroupCall: ad-hoc group(%s) removed from map (session ended)",
                                 strGroupId.c_str() );
                }
            }
        }
    }
}

/**
 * @brief Invite a member to a group call using Shared RTP Session
 */
bool CGroupCallService::InviteMember( const char *pszUserId, const char *pszGroupId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );

    // 같은 그룹에 기존 콜이 있으면: 다이얼로그가 살아있는 한 이미 세션 참여 중 — 재초대하지 않는다.
    //   (선참여 멤버를 stale 로 오판해 LEAVE+재초대하면 CMP 멤버십이 끊기는 좀비 상태가 됐었음.
    //    다른 그룹의 콜은 멀티그룹 동시 참여이므로 여기서 건드리지 않는다.)
    auto itUC = m_mapUserCall.find( { pszUserId, pszGroupId } );
    if ( itUC != m_mapUserCall.end() ) {
        std::string strExistCallId = itUC->second;
        // 라이브 SIP 다이얼로그가 살아있으면 이미 참여 중 — 재초대 금지.
        //   개시자(AcceptCall)·CSP초대(StartCall) 양쪽 레그가 모두 UA 다이얼로그 맵에 있으므로
        //   CallMap(=CSP 초대 레그만) 대신 다이얼로그 맵으로 판정한다.
        SIP_CALL_ID_LIST clsCallIds;
        gclsUserAgent.GetCallIdList( clsCallIds );
        bool bAlive = false;
        for ( const auto &strId : clsCallIds ) {
            if ( strId == strExistCallId ) {
                bAlive = true;
                break;
            }
        }
        if ( bAlive ) {
            CLog::Print( LOG_DEBUG, "InviteMember(%s, %s): already in session (%s) — skip", pszUserId, pszGroupId,
                         strExistCallId.c_str() );
            return true;
        }

        // 죽은 다이얼로그만 stale 정리 후 재초대
        CLog::Print( LOG_INFO, "InviteMember(%s, %s): stale call exists (%s), clearing", pszUserId, pszGroupId,
                     strExistCallId.c_str() );
        std::string strStaleGroup, strStaleSession;
        auto itSess = m_mapCallSession.find( strExistCallId );
        if ( itSess != m_mapCallSession.end() ) {
            strStaleGroup = itSess->second.strGroupId;
            strStaleSession = itSess->second.strSessionId;
            m_mapCallSession.erase( itSess );
        }
        m_mapUserCall.erase( itUC );

        // lock 해제하고 CMP 정리
        lock.unlock();
        if ( !strStaleGroup.empty() ) {
            gclsCmpClient.LeaveGroup( strStaleGroup, strStaleSession, GetOrIssueGroupSesId( strStaleGroup ) );
        }
        // 재획득 후 계속 진행
        lock.lock();
    }

    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    int iSharedFloorPortIM = -1;
    std::string strSharedIp;

    // 0. Verify Group Membership (Requirement: Only invite if user is explicitly in group config)
    CspPttGroup clsGroup;
    if ( gclsGroupMap.Select( pszGroupId, clsGroup ) ) {
        bool bIsMember = false;
        for ( const auto &pUser : clsGroup._pusers ) {
            if ( pUser && pUser->_id == pszUserId ) {
                bIsMember = true;
                break;
            }
        }
        if ( !bIsMember ) {
            CLog::Print( LOG_DEBUG, "InviteMember(%s) User NOT in Group(%s) member list. Skipping invitation.",
                         pszUserId, pszGroupId );
            return false;
        }
        // affiliation 요구 그룹은 affiliate 된 멤버만 초대 (TS 24.379 §9)
        if ( clsGroup._requireAffiliation && gclsDbManager.IsConnected() &&
             !gclsDbManager.IsAffiliated( pszGroupId, pszUserId ) ) {
            CLog::Print( LOG_INFO, "InviteMember(%s) not affiliated to Group(%s). Skipping invitation.", pszUserId,
                         pszGroupId );
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

    // T2~T4 통합: PTT outbound leg 의 Via/Contact 자기 주소를 access_services(kind=ptt) 의
    //   첫 allowed_local_node_refs 에 매칭되는 listener 의 bind_ip:bind_port 로 결정.
    //   ref 없거나 dangling 시 hint 미설정 → SipDialog 가 stack primary fallback.
    {
        ServiceInfo pttSvc = gclsServiceMap.GetByKind( "ptt" );
        if ( pttSvc.id > 0 && !pttSvc.allowed_local_node_refs.empty() ) {
            LocalNodeInfo ln = gclsLocalNodeMap.GetByName( pttSvc.allowed_local_node_refs[0] );
            if ( ln.IsValid() ) {
                clsRoute.m_strOutboundLocalIp =
                    ( ln.bind_ip.empty() || ln.bind_ip == "0.0.0.0" ) ? gclsSetup.m_strLocalIp : ln.bind_ip;
                if ( ln.bind_port > 0 ) clsRoute.m_iOutboundLocalPort = ln.bind_port;
            }
        }
    }

    // 2. Get Shared Group Port
    //   세션 시작 판정: 이 그룹에 이미 활성 호(멤버)가 있으면 CMP 그룹은 유효
    //   (멤버>0 이라 CMP 의 유휴 timeout 회수 대상이 아님) → 캐시 사용.
    //   활성 멤버가 없으면(=세션 시작) CMP 가 유휴 그룹을 timeout 제거했을 수 있으므로
    //   캐시를 믿지 말고 PTT_GROUP_ADD 으로 재확보한다 (멱등: 살아있으면 기존 port,
    //   회수됐으면 신규 생성). stale 캐시로 JOIN → 'Group Not Found' → 멤버 무더기
    //   drop 되던 문제(상용 PTT 영구그룹/장기 유휴 후 재통화)를 방지.
    bool bVideoEnabled = false;
    bool bGroupHasActiveCall = false;
    for ( const auto &kv : m_mapCallSession ) {
        if ( kv.second.strGroupId == pszGroupId ) {
            bGroupHasActiveCall = true;
            break;
        }
    }
    bool bInCache = ( m_mapGroupRtp.find( pszGroupId ) != m_mapGroupRtp.end() );
    if ( bInCache && bGroupHasActiveCall ) {
        iSharedFloorPortIM = m_mapGroupRtp[pszGroupId].iFloorPort;
        strSharedIp = m_mapGroupRtp[pszGroupId].strIp;
        bVideoEnabled = m_mapGroupRtp[pszGroupId].bVideoEnabled;
    } else {
        if ( bInCache )
            CLog::Print( LOG_INFO,
                         "InviteMember(%s): Group(%s) session (re)start — refreshing CMP group (stale-cache guard)",
                         pszUserId, pszGroupId );
        // Try to allocate now ((재)확보)
        CspPttGroup clsGroup;
        if ( gclsGroupMap.Select( pszGroupId, clsGroup ) ) {
            std::string strRecordDir;
            if ( gclsCallDir.IsEnabled() ) {
                strRecordDir = gclsCallDir.GetPttSessionDir( pszGroupId, TimeToIso( clsGroup._sessionStart ),
                                                             std::to_string( clsGroup._dbId ) );
                // 자기완결 그룹 디스크립터 (계획서 §5) — group.json (autojoin 경로)
                std::string strDescriptor = BuildGroupDescriptor( clsGroup );
                gclsCallDir.PttSessionStart( pszGroupId, "autojoin", pszUserId, strDescriptor );
            }
            // broadcast 그룹 재생성 시 기존 개시자 유지 (m_mapGroupRtp 캐시)
            std::string strInitiator;
            {
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                auto itRtp = m_mapGroupRtp.find( pszGroupId );
                if ( itRtp != m_mapGroupRtp.end() ) strInitiator = itRtp->second.strCallerId;
            }
            std::map<std::string, std::pair<int, int>> mapMemberPorts;
            int iNewFloorPort = 0;
            if ( gclsCmpClient.AddGroup( pszGroupId, clsGroup._pusers, strSharedIp, iNewFloorPort, mapMemberPorts,
                                         strRecordDir, false, 0, GetOrIssueGroupSesId( pszGroupId ),
                                         clsGroup._groupType, strInitiator ) ) {
                bVideoEnabled = clsGroup._videoEnabled;
                iSharedFloorPortIM = iNewFloorPort;
                // nMemberHash 는 반드시 실제 멤버해시로 설정 — 0 으로 두면 다음 SyncGroupsState 가
                // 변경으로 오인해 스퓨리어스 ModifyGroup+group_change NOTIFY storm → 멤버 drop.
                m_mapGroupRtp[pszGroupId] = {
                    iNewFloorPort, strSharedIp, ComputeMemberHash( clsGroup ), "", "", bVideoEnabled, 0,
                    mapMemberPorts };
            } else {
                CLog::Print( LOG_ERROR, "InviteMember(%s) Failed to get/alloc Shared Port for Group %s", pszUserId,
                             pszGroupId );
                return false;
            }
        } else {
            CLog::Print( LOG_ERROR, "InviteMember(%s) Group config not found for %s", pszUserId, pszGroupId );
            return false;
        }
    }

    // 3. Prepare RTP Info — 이 멤버 전용 CMP 포트로 SDP offer 구성 (leg 별 포트셋)
    int iMemberAudioPort = 0, iMemberVideoPort = 0;
    if ( !GetOrAllocMemberPort( pszGroupId, pszUserId, iMemberAudioPort, iMemberVideoPort ) ) {
        CLog::Print( LOG_ERROR, "InviteMember(%s) Failed to alloc member port for Group %s", pszUserId, pszGroupId );
        return false;
    }
    CSipCallRtp clsRtp;
    clsRtp.SetIpPort( strSharedIp.c_str(), iMemberAudioPort, SOCKET_COUNT_PER_MEDIA );

    // AMR-WB (기본 코덱, 서버 설정으로 추후 변경)
    clsRtp.m_clsCodecList.push_back( 99 );
    clsRtp.m_iCodec = 99;

    // 4. Create Call
    std::string strCallId;
    CSipMessage *pclsInvite = NULL;

    // PTT 멤버 Dialog — mcptt realm 도메인으로 INVITE 생성 (From/To/Request-URI/PAI 모두 mcptt)
    std::string strMcpttDomain = gclsServiceMap.GetDomainByKind( "ptt" );
    if ( gclsUserAgent.CreateCall( pszGroupId, pszUserId, &clsRtp, &clsRoute, strCallId, &pclsInvite,
                                   strMcpttDomain.empty() ? NULL : strMcpttDomain.c_str() ) ) {
        // SIP flow에 그룹 세션 공통 sesid 등록 (ADD/JOIN/INVITE 동일 sesid 유지)
        std::string strGroupSesId = GetOrIssueGroupSesId( pszGroupId );
        gclsSipLogger.SetCallSesId( strCallId, strGroupSesId, std::to_string( clsGroup._sessionSeq ) );

        // 4-1. Add PTT group info XML to INVITE (multipart/mixed: mcptt-info+xml + SDP)
        if ( pclsInvite != NULL ) {
            // Request-URI = 등록된 Contact URI (proxy target refresh, RFC 3261 §16.5 —
            // S-CSCF→UE 라우팅과 동일 모델). 사설 주소여도 실제 전송 목적지는 아래
            // SendDest 오버라이드(latch 된 NAT 주소)가 담당하므로 무방하다.
            // Contact 미보관 등록이면 포트 없는 AOR fallback. (dialog 기본 생성은
            // override 도메인 + Contact 포트가 섞인 "sip:user@domain:5080" 형태가 되어
            // AOR 도 Contact 도 아닌 URI 로 실단말이 거부할 수 있다.)
            if ( !clsUserInfo.m_strContactUri.empty() ) {
                pclsInvite->m_clsReqUri.Parse( clsUserInfo.m_strContactUri.c_str(),
                                               (int)clsUserInfo.m_strContactUri.length() );
            } else {
                pclsInvite->m_clsReqUri.Set( SIP_PROTOCOL, pszUserId, strMcpttDomain.c_str(), 0 );
            }
            // 선탑재 Route 제거 — NAT 도달 주소는 SendDest 오버라이드로 헤더 노출 없이
            // 라우팅한다 (reg-event NOTIFY 의 Route 제거와 동일 원칙).
            pclsInvite->m_clsRouteList.clear();
            pclsInvite->m_strSendDestIp = clsRoute.m_strDestIp;
            pclsInvite->m_iSendDestPort = clsRoute.m_iDestPort;

            // To: 는 개인 AOR 유지 (cwrtc가 WS 클라이언트를 찾는 데 필요)
            // 그룹 식별은 Contact(isfocus), P-Called-Party-ID, XML body로 전달

            // 발신자 ID 조회 (mcptt-calling-user-id)
            std::string strCallerId;
            {
                auto itRtp = m_mapGroupRtp.find( pszGroupId );
                if ( itRtp != m_mapGroupRtp.end() && !itRtp->second.strCallerId.empty() )
                    strCallerId = itRtp->second.strCallerId;
                else
                    strCallerId = pszGroupId;  // fallback
            }

            int iGroupCond = 0;
            {
                auto itCond = m_mapGroupCondition.find( pszGroupId );
                if ( itCond != m_mapGroupCondition.end() ) iGroupCond = itCond->second;
            }
            std::string strGroupXml = BuildGroupInfoXml( clsGroup, pszUserId, strCallerId, iGroupCond );
            std::string strRosterXml = BuildResourceListXml( clsGroup );
            // CMP floor port 사용 (m_mapGroupRtp에서 조회)
            int iFloorPort = iSharedFloorPortIM > 0 ? iSharedFloorPortIM : iMemberAudioPort + 1;  // fallback
            {
                auto itRtp2 = m_mapGroupRtp.find( pszGroupId );
                if ( itRtp2 != m_mapGroupRtp.end() && itRtp2->second.iFloorPort > 0 )
                    iFloorPort = itRtp2->second.iFloorPort;
            }
            std::string strGroupUri = "sip:" + std::string( pszGroupId ) + "@" + strMcpttDomain;
            WrapMultipartBody( pclsInvite, strGroupXml, strRosterXml, strSharedIp, iFloorPort, strGroupUri );

            // MCPTT capability required (3GPP TS 24.379 §6.3.1)
            pclsInvite->AddHeader(
                "Accept-Contact",
                "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";+g.3gpp.mcptt;require;explicit" );
            // P-Preferred-Service: MCPTT ICSI 선언 (3GPP TS 24.379 §6.3.1, RFC 6050)
            pclsInvite->AddHeader( "P-Preferred-Service", "urn:urn-7:3gpp-service.ims.icsi.mcptt" );
            // 단말 자동 응답 요구 (3GPP TS 24.379 §6.3.3.1)
            pclsInvite->AddHeader( "Answer-Mode", "Auto" );
            // Resource-Priority (RFC 4412, TS 24.379) — namespace당 값 하나 (F-08 수정)
            if ( iGroupCond >= 2 )
                pclsInvite->AddHeader( "Resource-Priority", "mcpttp.4" );
            else if ( iGroupCond == 1 )
                pclsInvite->AddHeader( "Resource-Priority", "mcpttp.2" );
            else
                pclsInvite->AddHeader( "Resource-Priority", "mcpttp.6" );
            // Callee identity (MCPTT 도메인 사용)
            std::string strMcpttDomain = gclsServiceMap.GetDomainByKind( "ptt" );
            char szPCalledParty[256];
            snprintf( szPCalledParty, sizeof( szPCalledParty ), "<sip:%s@%s>", pszUserId, strMcpttDomain.c_str() );
            pclsInvite->AddHeader( "P-Called-Party-ID", szPCalledParty );
            // isfocus: indicate server is conference focus (TS 24.379).
            // INVITE 의 Contact 는 정확히 1개여야 한다(RFC 3261 §8.1.1.8). 스택은 전송 직전
            // m_clsContactList 가 비어 있을 때만 자동 Contact 를 넣으므로(SipStackComm),
            // 라우팅 가능한 자기 주소 Contact 를 구조화 리스트에 직접 1개 채운다.
            // (기존: AddHeader 원문 헤더로 도메인형 Contact 를 추가 → 리스트는 비어 있어
            //  스택 자동 Contact 와 중복 2개가 되고, 실단말이 INVITE 를 폐기하는 원인)
            {
                CSipFrom clsContact;
                clsContact.m_clsUri.m_strProtocol = "sip";
                clsContact.m_clsUri.m_strUser = pszGroupId;
                clsContact.m_clsUri.m_strHost =
                    !clsRoute.m_strOutboundLocalIp.empty() ? clsRoute.m_strOutboundLocalIp : gclsSetup.m_strLocalIp;
                clsContact.m_clsUri.m_iPort =
                    clsRoute.m_iOutboundLocalPort > 0 ? clsRoute.m_iOutboundLocalPort : gclsSetup.m_iUdpPort;
                clsContact.InsertParam( "isfocus", "" );
                pclsInvite->m_clsContactList.clear();
                pclsInvite->m_clsContactList.push_back( clsContact );
            }
            // Session timer (RFC 4028) — required by many MCPTT implementations
            pclsInvite->AddHeader( "Session-Expires", "7200;refresher=uac" );
            pclsInvite->AddHeader( "Min-SE", "180" );
            // 비디오 활성화 전달 (cwrtc가 SDP에 H.264 포함 여부 결정)
            if ( bVideoEnabled && iMemberVideoPort > 0 ) {
                char szVideo[64];
                snprintf( szVideo, sizeof( szVideo ), "%d", iMemberVideoPort );
                pclsInvite->AddHeader( "X-Video-Port", szVideo );
            }
        }

        // Insert into CallMap (But manage Port cleanup ourselves)
        CCallInfo clsCallInfo;
        clsCallInfo.m_bRecv = false;
        clsCallInfo.m_iPeerRtpPort = iMemberAudioPort;
        // Note: CallMap::Delete would try to delete this port if we don't intercept it.
        // Intercept logic handled in EventCallEnd -> OnCallTerminated.

        gclsCallMap.Insert( strCallId.c_str(), clsCallInfo );

        // Track Session Info
        m_mapUserCall[{ pszUserId, pszGroupId }] = strCallId;
        m_mapCallSession[strCallId] = { pszGroupId, pszUserId, pszUserId };  // Use UserId as SessionId
        CLog::Print( LOG_DEBUG, "InviteMember(%s): Added to Maps. CallId=%s", pszUserId, strCallId.c_str() );

        if ( !gclsUserAgent.StartCall( strCallId.c_str(), pclsInvite ) ) {
            CLog::Print( LOG_ERROR, "InviteMember StartCall failed" );
            gclsCallMap.Delete( strCallId.c_str() );
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            m_mapUserCall.erase( { pszUserId, pszGroupId } );
            m_mapCallSession.erase( strCallId );
            return false;
        }

        // [CALL LOG] PTT 멤버 초대 기록 (join_time은 OnCallStarted에서 설정)
        if ( gclsDbManager.IsConnected() ) {
            // auto-join 시 해당 그룹에 활성 call log가 없으면 자동 생성
            if ( !gclsDbManager.HasActiveGroupCall( pszGroupId ) ) {
                std::string strAutoCallId =
                    "csp-autojoin-" + std::string( pszGroupId ) + "-" + std::to_string( (long long)time( NULL ) );
                gclsDbManager.InsertCallLog( strAutoCallId.c_str(), true, pszGroupId, "CSP", pszGroupId );
                gclsDbManager.UpdateCallLogActivePtt( pszGroupId );
            }
            gclsDbManager.InsertGroupParticipant( pszGroupId, pszUserId );
        }
    } else {
        CLog::Print( LOG_ERROR, "InviteMember CreateCall failed" );
        return false;
    }

    CLog::Print( LOG_INFO, "InviteMember(%s) Group(%s) MemberPort(%d) CallId(%s) Initiated", pszUserId, pszGroupId,
                 iMemberAudioPort, strCallId.c_str() );
    return true;
}

void CGroupCallService::StartMonitor() {
    if ( !m_bMonitorRunning ) {
        m_bMonitorRunning = true;
        m_threadMonitor = std::thread( &CGroupCallService::MonitorLoop, this );
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
        std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
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

size_t CGroupCallService::ComputeMemberHash( const CspPttGroup &group ) {
    std::string strHashInput;
    for ( const auto &pUser : group._pusers ) {
        if ( !pUser ) continue;
        strHashInput += pUser->_id + ":" + std::to_string( pUser->_priority ) + ";";
    }
    return std::hash<std::string>{}( strHashInput );
}

void CGroupCallService::SyncGroupsState() {
    // A. Add New Groups
    gclsGroupMap.IterateInternal( [this]( const CspPttGroup &group ) {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );

        // Calculate Hash
        size_t nHash = ComputeMemberHash( group );

        auto itRtp = m_mapGroupRtp.find( group._id );
        if ( itRtp == m_mapGroupRtp.end() ) {
            // 규격 모델: CMP 그룹 컨텍스트를 proactive 하게 만들지 않는다.
            //   세션 생성은 발신 INVITE(on-demand: ProcessGroupCall) 또는
            //   affiliation 기반 합류(chat: CheckGroupIntegrity)가 담당. 여기선 무동작.
            return;
        } else {
            // EXISTING GROUP - Check for Diff
            if ( itRtp->second.nMemberHash != nHash ) {
                // CHANGED
                lock.unlock();
                CLog::Print( LOG_INFO, "SyncGroupsState: Group(%s) Config Changed. Sending ModifyGroup.",
                             group._id.c_str() );
                if ( gclsCmpClient.ModifyGroup( group._id, group._pusers, GetOrIssueGroupSesId( group._id ) ) ) {
                    std::unique_lock<std::recursive_mutex> lock2( m_mutex );
                    m_mapGroupRtp[group._id].nMemberHash = nHash;
                } else {
                    // MODIFY 실패 (NOT_FOUND: CMP 그룹 소실 등) — AddGroup 멱등 재수립.
                    //   재생성이면 floor/멤버 포트가 새로 할당되므로 캐시를 응답값으로 갱신한다.
                    std::string strIp, strRecordDir;
                    int iFloorPort = 0;
                    std::map<std::string, std::pair<int, int>> mapMemberPorts;
                    if ( gclsCallDir.IsEnabled() )
                        strRecordDir = gclsCallDir.GetPttSessionDir( group._id, TimeToIso( group._sessionStart ),
                                                                     std::to_string( group._dbId ) );
                    if ( gclsCmpClient.AddGroup( group._id, group._pusers, strIp, iFloorPort, mapMemberPorts,
                                                 strRecordDir, group._videoEnabled, group._sessionSeq,
                                                 GetOrIssueGroupSesId( group._id ), group._groupType, "" ) ) {
                        std::unique_lock<std::recursive_mutex> lock2( m_mutex );
                        auto it2 = m_mapGroupRtp.find( group._id );
                        if ( it2 != m_mapGroupRtp.end() ) {
                            it2->second.iFloorPort = iFloorPort;
                            it2->second.strIp = strIp;
                            it2->second.memberPorts = mapMemberPorts;
                            it2->second.nMemberHash = nHash;
                        }
                        CLog::Print( LOG_INFO, "SyncGroupsState: Group(%s) re-established on CMP (floor=%d)",
                                     group._id.c_str(), iFloorPort );
                    } else {
                        CLog::Print( LOG_ERROR, "SyncGroupsState: Group(%s) ModifyGroup/AddGroup 재수립 실패",
                                     group._id.c_str() );
                    }
                }
                // Notify GMS subscribers that group config changed
                SendSipNotify( "tel:" + group._id, "change_" + std::to_string( time( NULL ) ), "PUT" );
            }
        }
    } );

    // B. Remove Deleted Groups
    std::vector<std::string> vecToRemove;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        for ( auto it = m_mapGroupRtp.begin(); it != m_mapGroupRtp.end(); ++it ) {
            CspPttGroup group;
            if ( !gclsGroupMap.Select( it->first.c_str(), group ) ) {
                vecToRemove.push_back( it->first );
            }
        }
    }

    for ( const auto &strGroupId : vecToRemove ) {
        CLog::Print( LOG_INFO, "SyncGroupsState: Group(%s) removed from config. Cleaning up.", strGroupId.c_str() );
        // Notify GMS subscribers about group deletion before removing
        SendSipNotify( "tel:" + strGroupId, "deleted_" + std::to_string( time( NULL ) ), "DELETE" );
        gclsCmpClient.RemoveGroup( strGroupId, GetOrIssueGroupSesId( strGroupId ) );

        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        m_mapGroupRtp.erase( strGroupId );
        RemoveGroupSesId( strGroupId );
    }
}

void CGroupCallService::CheckMemberState() {
    std::vector<std::string> vecToKick;

    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        for ( auto it = m_mapUserCall.begin(); it != m_mapUserCall.end(); ++it ) {
            std::string strUserId = it->first.first;
            std::string strCallId = it->second;

            auto itSess = m_mapCallSession.find( strCallId );
            if ( itSess != m_mapCallSession.end() ) {
                std::string strGroupId = itSess->second.strGroupId;

                CspPttGroup group;
                if ( !gclsGroupMap.Select( strGroupId.c_str(), group ) ) {
                    // Group Gone
                    vecToKick.push_back( strCallId );
                } else {
                    // Check if member still in group
                    bool bFound = false;
                    for ( const auto &pUser : group._pusers ) {
                        if ( !pUser ) continue;
                        if ( pUser->_id == strUserId ) {
                            bFound = true;
                            break;
                        }
                    }
                    if ( !bFound ) vecToKick.push_back( strCallId );
                }
            }
        }
    }

    for ( const auto &strCallId : vecToKick ) {
        CLog::Print( LOG_INFO, "CheckMemberState: Call(%s) no longer valid (Group/Member removed). Terminating.",
                     strCallId.c_str() );
        gclsUserAgent.StopCall( strCallId.c_str() );
        // Force cleanup immediately as EventCallEnd might be delayed or not propagated for local stop
        OnCallTerminated( strCallId );
    }
}

void CGroupCallService::CheckGroupIntegrity() {
    // 규격 모델(TS 24.379): 세션을 상시 강제하지 않는다.
    //  - chat(group_type)            : 상시 세션 — affiliate+등록 멤버를 합류 유지(필요 시 컨텍스트 생성).
    //  - prearranged/broadcast       : on-demand — 이미 active 한 세션의 누락 멤버만 재초대(late entry/복구).
    //                                  active 세션이 없으면 무동작(발신 INVITE 가 세션을 만든다).
    //  멤버 자격 = 등록됨(UserMap) ∧ (require_affiliation 이면 affiliated).
    gclsGroupMap.IterateInternal( [this]( const CspPttGroup &group ) {
        const bool bPersistent = ( group._groupType == "chat" );

        // 1) eligible 멤버 수집 (등록 ∧ affiliation 게이트)
        std::vector<std::string> vecEligible;
        for ( const auto &pUser : group._pusers ) {
            if ( !pUser ) continue;
            const std::string &strUserId = pUser->_id;
            CUserInfo clsUser;
            if ( !gclsUserMap.Select( strUserId.c_str(), clsUser ) ) continue;
            if ( group._requireAffiliation && gclsDbManager.IsConnected() &&
                 !gclsDbManager.IsAffiliated( group._id.c_str(), strUserId.c_str() ) )
                continue;
            vecEligible.push_back( strUserId );
        }

        // 2) 세션 존재 판정
        bool bActive = false, bHasContext = false;
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            bHasContext = ( m_mapGroupRtp.find( group._id ) != m_mapGroupRtp.end() );
            for ( const auto &kv : m_mapCallSession ) {
                if ( kv.second.strGroupId == group._id ) {
                    bActive = true;
                    break;
                }
            }
        }
        if ( bPersistent ) {
            if ( vecEligible.empty() ) return;  // chat: 자격 멤버 없으면 빈 세션 안 만듦
        } else {
            if ( !bActive ) return;  // on-demand: active 세션 아니면 무동작
        }

        // BYE 처리 중 race condition 방지: 5초 grace period 동안 재-INVITE 보류.
        // 여러 멤버 BYE가 순차 처리되는 사이에 CheckGroupIntegrity가 끼어드는 것을 차단.
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            auto itTerm = m_mapGroupLastTerminate.find( group._id );
            if ( itTerm != m_mapGroupLastTerminate.end() ) {
                auto elapsed = std::chrono::steady_clock::now() - itTerm->second;
                if ( elapsed < std::chrono::seconds( 5 ) ) return;
                m_mapGroupLastTerminate.erase( itTerm );
            }
        }

        // 3) 컨텍스트 보장 (chat 최초 합류 시 생성; active on-demand 는 이미 존재)
        if ( !bHasContext ) {
            std::string ip;
            int floorPort = 0;
            std::map<std::string, std::pair<int, int>> mapMemberPorts;
            std::string strRecordDir;
            if ( gclsCallDir.IsEnabled() )
                strRecordDir = gclsCallDir.GetPttSessionDir( group._id, TimeToIso( group._sessionStart ),
                                                             std::to_string( group._dbId ) );
            std::string strGroupSesId = GetOrIssueGroupSesId( group._id );
            if ( !gclsCmpClient.AddGroup( group._id, group._pusers, ip, floorPort, mapMemberPorts, strRecordDir,
                                          group._videoEnabled, group._sessionSeq, strGroupSesId, group._groupType,
                                          "" ) )
                return;
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            m_mapGroupRtp[group._id] = { floorPort,     ip, ComputeMemberHash( group ), "", "", group._videoEnabled, 0,
                                         mapMemberPorts };
        }

        // 4) call log 보장
        if ( gclsDbManager.IsConnected() ) {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            auto itRtp = m_mapGroupRtp.find( group._id );
            if ( itRtp != m_mapGroupRtp.end() && itRtp->second.strSessionCallId.empty() ) {
                char szCallId[160];
                snprintf( szCallId, sizeof( szCallId ), "csp-group-%s-%ld", group._id.c_str(), (long)time( NULL ) );
                itRtp->second.strSessionCallId = szCallId;
                lock.unlock();
                gclsDbManager.InsertCallLog( szCallId, true, group._id, "CSP", group._id );
            }
        }

        // 5) 누락 멤버 초대 (chat 합류 / on-demand active 세션 late-entry·복구)
        for ( const auto &strUserId : vecEligible ) {
            bool bInCall;
            {
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                // 멀티그룹: '이 그룹' 참여 여부만 본다 (다른 그룹 통화 중이어도 초대 대상)
                bInCall = ( m_mapUserCall.find( { strUserId, group._id } ) != m_mapUserCall.end() );
            }
            if ( !bInCall ) {
                CLog::Print( LOG_DEBUG, "CheckGroupIntegrity: invite %s → %s (type=%s)", strUserId.c_str(),
                             group._id.c_str(), group._groupType.c_str() );
                InviteMember( strUserId.c_str(), group._id.c_str() );
            }
        }
    } );
}

void CGroupCallService::OnCmpStatusChanged( bool bConnected ) {
    if ( bConnected ) {
        CLog::Print( LOG_INFO, "OnCmpStatusChanged: Connected -> Syncing Groups" );
        SyncGroupsState();
    } else {
        CLog::Print( LOG_INFO, "OnCmpStatusChanged: Disconnected" );
        // Cleanup?
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        m_mapGroupRtp.clear();
        // We probably shouldn't clear user calls immediately unless we destroy SIP dialogs.
    }
}

// 200 OK Received -> Join Group Helper
void CGroupCallService::OnCallStarted( const std::string &strCallId, const std::string &strRemoteIp, int iRemotePort,
                                       int iRemoteFloorPort, int iRemoteVideoPort ) {
    std::string strGroupId, strSessionId, strMemberId;
    int iCmpFloorPort = 0;

    // 1. lock 보유 중 맵 조회만 수행
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto it = m_mapCallSession.find( strCallId );
        if ( it == m_mapCallSession.end() ) return;

        strGroupId = it->second.strGroupId;
        strSessionId = it->second.strSessionId;
        strMemberId = it->second.strMemberId;

        // CMP에서 할당한 floor_port 조회 (멤버 SDP에서 파싱 불필요)
        auto itRtp = m_mapGroupRtp.find( strGroupId );
        if ( itRtp != m_mapGroupRtp.end() ) {
            iCmpFloorPort = itRtp->second.iFloorPort;
        }
    }
    // 2. lock 해제 후 외부 호출 (CMP, DB)
    int iFloorPort = iRemoteFloorPort > 0 ? iRemoteFloorPort : ( iRemotePort + 1 );
    // video 는 협상된 경우만 전달 — 비협상 멤버에 audio+2 유령 포트를 광고하면 CMP 가
    //   무효 목적지로 video 를 송신한다 (cspsim 0.2.5 의 비협상 video 미송신 정합과 대칭).
    int iVideoPort = iRemoteVideoPort > 0 ? iRemoteVideoPort : 0;
    // 멤버 role 조회 (chair/participant) — CMP floor 선점 판정에 사용
    std::string strRole = "participant";
    {
        CspPttGroup clsGroup;
        if ( gclsGroupMap.Select( strGroupId.c_str(), clsGroup ) ) {
            for ( const auto &pUser : clsGroup._pusers ) {
                if ( pUser && pUser->_id == strMemberId ) {
                    strRole = pUser->_role;
                    break;
                }
            }
        }
    }
    // 멤버 leg NAT 판정 — answer SDP 선언 IP vs 멤버 등록 바인딩(received/rport latch)
    int iMemberNat = 0;
    std::string strMemberGuardIp;
    {
        ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( strMemberId, "ptt" );
        std::string strSigIp;
        CUserInfo clsMemberInfo;
        if ( gclsUserMap.Select( strMemberId.c_str(), clsMemberInfo ) ) strSigIp = clsMemberInfo.m_strIp;
        if ( CCspServiceMap::EvalMediaNat( clsNatSvc, strRemoteIp, strSigIp, strMemberGuardIp ) ) {
            iMemberNat = 1;
            CLog::Print( LOG_INFO, "OnCallStarted: member leg NAT (svc=%s member=%s sdp=%s sig=%s)",
                         clsNatSvc.name.c_str(), strMemberId.c_str(), strRemoteIp.c_str(), strSigIp.c_str() );
        }
    }
    if ( gclsCmpClient.JoinGroup( strGroupId, strSessionId, strRemoteIp, iRemotePort, iFloorPort, iVideoPort,
                                  GetOrIssueGroupSesId( strGroupId ), strRole, NULL, NULL, iMemberNat,
                                  strMemberGuardIp ) ) {
        CLog::Print( LOG_INFO, "OnCallStarted: Joined Group(%s) Peer(%s:%d floor=%d video=%d)", strGroupId.c_str(),
                     strRemoteIp.c_str(), iRemotePort, iFloorPort, iVideoPort );
        if ( gclsCallDir.IsEnabled() ) {
            gclsCallDir.PttMemberJoin( strGroupId, strMemberId, strCallId );
        }
    } else {
        CLog::Print( LOG_ERROR, "OnCallStarted: JoinGroup failed for %s", strGroupId.c_str() );
    }

    if ( gclsDbManager.IsConnected() ) {
        gclsDbManager.UpdateParticipantJoined( strGroupId, strMemberId );
        gclsDbManager.UpdateCallLogActivePtt( strGroupId );
    }

    // RFC 4575: Notify all active participants about new member joining
    SendConferenceNotify( strGroupId, strMemberId, "connected", "full" );
}

// BYE/Error -> Leave Group
bool CGroupCallService::OnCallTerminated( const std::string &strCallId ) {
    std::string strGroupId, strMemberId, strSessionId;
    bool bStillActive = false;
    bool bFound = false;

    // 1. lock 보유 중 맵 조회/수정만 수행 (외부 호출 금지)
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        CLog::Print( LOG_DEBUG, "OnCallTerminated: Enter CallId=%s", strCallId.c_str() );

        auto it = m_mapCallSession.find( strCallId );
        if ( it == m_mapCallSession.end() ) return false;

        strGroupId = it->second.strGroupId;
        strMemberId = it->second.strMemberId;
        strSessionId = it->second.strSessionId;

        m_mapCallSession.erase( it );

        for ( auto uIt = m_mapUserCall.begin(); uIt != m_mapUserCall.end(); ++uIt ) {
            if ( uIt->second == strCallId ) {
                m_mapUserCall.erase( uIt );
                break;
            }
        }

        for ( const auto &kv : m_mapCallSession ) {
            if ( kv.second.strGroupId == strGroupId ) {
                bStillActive = true;
                break;
            }
        }
        if ( !bStillActive ) {
            auto itRtp = m_mapGroupRtp.find( strGroupId );
            if ( itRtp != m_mapGroupRtp.end() ) {
                itRtp->second.strSessionCallId.clear();
            }
        }
        // BYE 처리 시각 기록: CheckGroupIntegrity race condition 방지용 (5초 grace period)
        m_mapGroupLastTerminate[strGroupId] = std::chrono::steady_clock::now();
        bFound = true;
    }
    // 2. lock 해제 후 외부 호출 (CMP, DB)
    CLog::Print( LOG_INFO, "OnCallTerminated: Group Call Terminated. CallId=%s", strCallId.c_str() );
    gclsCmpClient.LeaveGroup( strGroupId, strSessionId, GetOrIssueGroupSesId( strGroupId ) );

    // PTT history: member leave event
    if ( gclsCallDir.IsEnabled() ) {
        gclsCallDir.PttMemberLeave( strGroupId, strMemberId );
        if ( !bStillActive ) {
            gclsCallDir.PttSessionEnd( strGroupId );
        }
    }

    if ( gclsDbManager.IsConnected() ) {
        gclsDbManager.UpdateParticipantLeft( strGroupId, strMemberId );
        if ( !bStillActive ) {
            gclsDbManager.EndGroupCallLog( strGroupId );
        }
    }

    // RFC 4575: Notify remaining participants about member leaving
    if ( bStillActive ) {
        SendConferenceNotify( strGroupId, strMemberId, "disconnected", "deleted" );
    } else if ( !strGroupId.empty() ) {
        // on-demand 그룹(prearranged/broadcast): 마지막 멤버 이탈 시 세션 즉시 해제 (chat 은 상시 유지).
        CspPttGroup clsGrp;
        bool bChat = gclsGroupMap.Select( strGroupId.c_str(), clsGrp ) && clsGrp._groupType == "chat";
        if ( !bChat ) {
            gclsCmpClient.RemoveGroup( strGroupId, GetOrIssueGroupSesId( strGroupId ) );
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            m_mapGroupRtp.erase( strGroupId );
            RemoveGroupSesId( strGroupId );
        }
    }

    return bFound;
}

// ─────────────────────────────────────────────────────────
// Conference Event Package (RFC 4575) — in-dialog NOTIFY
// ─────────────────────────────────────────────────────────

void CGroupCallService::SendConferenceNotify( const std::string &strGroupId, const std::string &strChangedUser,
                                              const std::string &strStatus, const std::string &strJoining ) {
    // 1. Collect all active call-IDs for this group + bump version
    std::vector<std::string> vecCallIds;
    int iVersion = 0;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto itRtp = m_mapGroupRtp.find( strGroupId );
        if ( itRtp != m_mapGroupRtp.end() ) {
            itRtp->second.iConfVersion++;
            iVersion = itRtp->second.iConfVersion;
        }

        for ( const auto &kv : m_mapCallSession ) {
            if ( kv.second.strGroupId == strGroupId ) {
                vecCallIds.push_back( kv.first );
            }
        }
    }

    if ( vecCallIds.empty() ) return;

    // 2. Build conference-info+xml body (RFC 4575)
    //    F-09: version=1(첫 NOTIFY)은 state="full" + 전체 멤버 목록, 이후는 state="partial" + 변경분
    //    F-10: entity는 sip: URI (tel: → RFC 4575 §5.3 위반)
    std::string strMcpttDomain = gclsServiceMap.GetDomainByKind( "ptt" );
    std::ostringstream oss;

    if ( iVersion == 1 ) {
        // state="full": 현재 그룹에 속한 모든 참가자를 열거
        std::vector<std::string> vecAllMembers;
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            for ( const auto &kv : m_mapCallSession ) {
                if ( kv.second.strGroupId == strGroupId ) {
                    vecAllMembers.push_back( kv.second.strMemberId );
                }
            }
        }
        oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
            << "<conference-info xmlns=\"urn:ietf:params:xml:ns:conference-info\"\r\n"
            << "  entity=\"sip:" << strGroupId << "@" << strMcpttDomain << "\"\r\n"
            << "  state=\"full\" version=\"" << iVersion << "\">\r\n"
            << "  <users>\r\n";
        for ( const auto &strMember : vecAllMembers ) {
            std::string strMemberStatus = ( strMember == strChangedUser ) ? strStatus : "connected";
            std::string strMemberJoining = ( strMember == strChangedUser ) ? strJoining : "added";
            oss << "    <user entity=\"sip:" << strMember << "@" << strMcpttDomain << "\" state=\"" << strMemberJoining
                << "\">\r\n"
                << "      <endpoint entity=\"sip:" << strMember << "@" << strMcpttDomain << "\">\r\n"
                << "        <status>" << strMemberStatus << "</status>\r\n"
                << "      </endpoint>\r\n"
                << "    </user>\r\n";
        }
        oss << "  </users>\r\n"
            << "</conference-info>\r\n";
    } else {
        // state="partial": 변경된 멤버 하나만
        oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
            << "<conference-info xmlns=\"urn:ietf:params:xml:ns:conference-info\"\r\n"
            << "  entity=\"sip:" << strGroupId << "@" << strMcpttDomain << "\"\r\n"
            << "  state=\"partial\" version=\"" << iVersion << "\">\r\n"
            << "  <users>\r\n"
            << "    <user entity=\"sip:" << strChangedUser << "@" << strMcpttDomain << "\" state=\"" << strJoining
            << "\">\r\n"
            << "      <endpoint entity=\"sip:" << strChangedUser << "@" << strMcpttDomain << "\">\r\n"
            << "        <status>" << strStatus << "</status>\r\n"
            << "      </endpoint>\r\n"
            << "    </user>\r\n"
            << "  </users>\r\n"
            << "</conference-info>\r\n";
    }
    std::string strBody = oss.str();

    // 3. Send in-dialog NOTIFY to each active participant via SipUserAgent
    for ( const auto &strCallId : vecCallIds ) {
        gclsUserAgent.SendNotifyWithBody( strCallId.c_str(), "conference", "application", "conference-info+xml",
                                          strBody );
    }

    CLog::Print( LOG_INFO, "SendConferenceNotify: Group(%s) User(%s) Status(%s) Joining(%s) → %d participants",
                 strGroupId.c_str(), strChangedUser.c_str(), strStatus.c_str(), strJoining.c_str(),
                 (int)vecCallIds.size() );
}

/**
 * @brief Build PTT group info XML body per 3GPP TS 24.379 MCPTT spec
 *        Content-Type: application/vnd.3gpp.mcptt-info+xml
 */
std::string CGroupCallService::BuildGroupInfoXml( const CspPttGroup &clsGroup, const std::string &strUserId,
                                                  const std::string &strCallerId, int iCondition ) {
    std::ostringstream oss;

    // session-type 은 그룹 유형(prearranged/chat/broadcast)에 따라 구동 (TS 24.379)
    std::string strSessionType = clsGroup._groupType.empty() ? "prearranged" : clsGroup._groupType;

    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        << "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\""
        << " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
        << "  <mcptt-Params>\r\n"
        << "    <session-type>" << strSessionType << "</session-type>\r\n";
    // condition 지시자 (TS 24.379) — session-type 과 직교. fan-out 으로 멤버 UE 에 긴급/임박 광고.
    if ( iCondition >= 2 )
        oss << "    <emergency-ind>true</emergency-ind>\r\n";
    else if ( iCondition == 1 )
        oss << "    <imminentperil-ind>true</imminentperil-ind>\r\n";
    oss << "    <mcptt-request-uri>tel:" << strUserId << "</mcptt-request-uri>\r\n"
        << "    <mcptt-calling-user-id>tel:" << strCallerId << "</mcptt-calling-user-id>\r\n"
        << "    <mcptt-calling-group-id>tel:" << clsGroup._id << "</mcptt-calling-group-id>\r\n"
        << "  </mcptt-Params>\r\n"
        << "</mcpttinfo>\r\n";

    return oss.str();
}

/**
 * @brief Build group member roster per RFC 5366 (resource-lists) with MCPTT group-info
 *        extension for per-member role/priority.
 *        Content-Type: application/resource-lists+xml
 */
std::string CGroupCallService::BuildResourceListXml( const CspPttGroup &clsGroup ) {
    std::ostringstream oss;

    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        << "<resource-lists xmlns=\"urn:ietf:params:xml:ns:resource-lists\"\r\n"
        << "  xmlns:mcpttgi=\"urn:3gpp:ns:mcpttGroupInfo:1.0\">\r\n"
        << "  <list>\r\n";
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( !pUser ) continue;
        const std::string &strUri = pUser->_mcpttId.empty() ? ( "tel:" + pUser->_id ) : pUser->_mcpttId;
        oss << "    <entry uri=\"" << strUri << "\">\r\n"
            << "      <mcpttgi:participant-type>" << pUser->_role << "</mcpttgi:participant-type>\r\n"
            << "      <mcpttgi:user-priority>" << pUser->_priority << "</mcpttgi:user-priority>\r\n"
            << "    </entry>\r\n";
    }
    oss << "  </list>\r\n"
        << "</resource-lists>\r\n";

    return oss.str();
}

std::string CGroupCallService::BuildGroupDescriptor( const CspPttGroup &clsGroup ) {
    // 자기완결 디스크립터 (계획서 §5). state/updated_at 은 PttSessionStart 가 주입.
    auto jbool = []( bool b ) -> const char * { return b ? "true" : "false"; };
    std::ostringstream oss;
    oss << "{";
    oss << "\"id\":" << clsGroup._dbId;
    oss << ",\"mcptt_group_id\":\"" << CCallDir::JsonEsc( clsGroup._id ) << "\"";
    oss << ",\"name\":\"" << CCallDir::JsonEsc( clsGroup._name ) << "\"";
    if ( clsGroup._alias.empty() )
        oss << ",\"alias\":null";
    else
        oss << ",\"alias\":\"" << CCallDir::JsonEsc( clsGroup._alias ) << "\"";
    oss << ",\"group_type\":\"" << CCallDir::JsonEsc( clsGroup._groupType ) << "\"";
    oss << ",\"priority\":" << clsGroup._priority;
    oss << ",\"encryption\":" << jbool( clsGroup._encryption );
    oss << ",\"emergency_call\":" << jbool( clsGroup._emergencyCall );
    oss << ",\"video_enabled\":" << jbool( clsGroup._videoEnabled );
    oss << ",\"on_network\":" << jbool( clsGroup._onNetwork );
    oss << ",\"max_members\":" << clsGroup._maxMembers;
    oss << ",\"require_affiliation\":" << jbool( clsGroup._requireAffiliation );
    oss << ",\"org_code\":\"" << CCallDir::JsonEsc( clsGroup._orgCode ) << "\"";
    if ( clsGroup._authorizedUserId > 0 )
        oss << ",\"authorized_user_id\":" << clsGroup._authorizedUserId;
    else
        oss << ",\"authorized_user_id\":null";
    if ( clsGroup._authorizedUser.empty() )
        oss << ",\"authorized_user\":null";
    else
        oss << ",\"authorized_user\":\"tel:" << CCallDir::JsonEsc( clsGroup._authorizedUser ) << "\"";
    if ( clsGroup._createdAt.empty() )
        oss << ",\"created_at\":null";
    else
        oss << ",\"created_at\":\"" << CCallDir::JsonEsc( clsGroup._createdAt ) << "\"";
    // 멤버 + member_count
    int iCount = 0;
    std::ostringstream members;
    members << "[";
    bool bFirst = true;
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( !pUser ) continue;
        if ( !bFirst ) members << ",";
        bFirst = false;
        ++iCount;
        members << "{\"user_id\":\"" << CCallDir::JsonEsc( pUser->_id ) << "\"";
        members << ",\"priority\":" << pUser->_priority;
        members << ",\"role\":\"" << CCallDir::JsonEsc( pUser->_role ) << "\"";
        if ( pUser->_mcpttId.empty() )
            members << ",\"mcptt_id\":null";
        else
            members << ",\"mcptt_id\":\"" << CCallDir::JsonEsc( pUser->_mcpttId ) << "\"";
        members << "}";
    }
    members << "]";
    oss << ",\"member_count\":" << iCount;
    oss << ",\"members\":" << members.str();
    oss << "}";
    return oss.str();
}

/**
 * @brief Replace INVITE body with multipart/mixed per 3GPP TS 24.379:
 *        Part 1: application/vnd.3gpp.mcptt-info+xml  (XML first)
 *        Part 2: application/sdp  (SDP with MCPTT floor control m= line)
 */
void CGroupCallService::WrapMultipartBody( CSipMessage *pclsInvite, const std::string &strGroupXml,
                                           const std::string &strRosterXml, const std::string &strFloorIp,
                                           int iFloorPort, const std::string &strGroupUri ) {
    if ( pclsInvite == NULL || pclsInvite->m_strBody.empty() ) return;

    // F-16: boundary를 랜덤 hex 문자열로 생성 — body 내 "mcptt" 등장과 충돌 방지 (RFC 2046 §5.1.1)
    struct timespec _ts;
    clock_gettime( CLOCK_REALTIME, &_ts );
    unsigned _uRnd = (unsigned)( _ts.tv_nsec ^ (uintptr_t)pclsInvite );
    char _szBoundary[32];
    snprintf( _szBoundary, sizeof( _szBoundary ), "mcptt_%08x%08x", (unsigned)_ts.tv_sec, _uRnd );
    const std::string strBoundary = _szBoundary;
    std::string strSdp = pclsInvite->m_strBody;

    // SDP 끝에 MCPTT floor control 미디어 라인 추가 (3GPP TS 24.379)
    // m=application: PTT floor control (Grant/Deny/Release) 전용 UDP 포트
    std::ostringstream sdpFloor;
    sdpFloor << "m=application " << iFloorPort << " UDP MCPTT\r\n"
             << "c=IN IP4 " << strFloorIp << "\r\n"
             << "a=floorid:0 mstrm:audio\r\n"
             << "a=fmtp:MCPTT mc_queueing;mc_priority=3\r\n";
    if ( !strGroupUri.empty() ) sdpFloor << "a=mcptt-floor-request-uri:" << strGroupUri << "\r\n";  // TS 24.379 §C.3
    strSdp += sdpFloor.str();

    // INVITE 가 SIP UDP 패킷 한계(psip SIP_PACKET_MAX_SIZE=8192)를 넘으면 수신측에서
    // truncate/drop 되어 호가 성립하지 않는다. 대형 그룹의 멤버 로스터를 인라인하면
    // 본문이 한계를 초과하므로, 본문 추정치가 안전 한계(7000B; 헤더 여유 포함)를 넘으면
    // 로스터 part 를 생략한다. (대형 그룹 멤버 정보는 GMS 그룹문서 + conference NOTIFY 로 제공)
    const size_t kSafeBodyLimit = 7000;
    bool bIncludeRoster =
        !strRosterXml.empty() && ( strGroupXml.size() + strRosterXml.size() + strSdp.size() + 400 ) < kSafeBodyLimit;
    if ( !strRosterXml.empty() && !bIncludeRoster ) {
        CLog::Print( LOG_INFO, "WrapMultipartBody: roster(%zuB) 생략 — INVITE 본문이 UDP 한계 초과 우려 (GMS 로 제공)",
                     strRosterXml.size() );
    }

    std::ostringstream oss;
    // Part 1: mcptt-info XML (3GPP MCPTT call control)
    oss << "--" << strBoundary << "\r\n"
        << "Content-Type: application/vnd.3gpp.mcptt-info+xml\r\n"
        << "Content-Length: " << strGroupXml.size() << "\r\n"
        << "\r\n"
        << strGroupXml << "\r\n";
    // Part 2: 멤버 로스터 (resource-lists, RFC 5366) — 크기 안전할 때만
    if ( bIncludeRoster ) {
        oss << "--" << strBoundary << "\r\n"
            << "Content-Type: application/resource-lists+xml\r\n"
            << "Content-Length: " << strRosterXml.size() << "\r\n"
            << "\r\n"
            << strRosterXml << "\r\n";
    }
    // Part 3: SDP with floor control
    oss << "--" << strBoundary << "\r\n"
        << "Content-Type: application/sdp\r\n"
        << "Content-Disposition: render\r\n"
        << "Content-Length: " << strSdp.size() << "\r\n"
        << "\r\n"
        << strSdp << "\r\n";
    oss << "--" << strBoundary << "--\r\n";

    pclsInvite->m_strBody = oss.str();
    pclsInvite->m_iContentLength = (int)pclsInvite->m_strBody.size();
    pclsInvite->m_clsContentType.Set( "multipart", "mixed" );
    pclsInvite->m_clsContentType.InsertParam( "boundary", strBoundary.c_str() );
}
