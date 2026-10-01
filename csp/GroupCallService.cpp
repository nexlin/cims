/*
 * Group Call Service Source
 */

#include "GroupCallService.h"

#include <algorithm>
#include <ctime>
#include <set>

#include "CspAddressing.h"
#include "CspServiceMap.h"
#include "DbManager.h"
#include "GroupMap.h"
#include "Log.h"
#include "SipMessageLogger.h"
#include "SipServer.h"

// 문자열 조립 유틸
#include <sstream>

#include "AuthzRevoke.h"
#include "CallDir.h"
#include "CallMap.h"
#include "CmpClient.h"
#include "CspLocalNodeMap.h"
#include "CspPhoneGroup.h"
#include "CspPttGroup.h"
#include "CspRole.h"
#include "CspServiceConfig.h"
#include "FmReporter.h"
#include "McService.h"
#include "McpttInfo.h"
#include "RtpMap.h"
#include "SipCodecTable.h"
#include "SipMessage.h"
#include "SipServerSetup.h"
#include "SipUserAgent.h"
#include "UserMap.h"

// 제어 기능이 자기 Contact 에 싣는 특성 태그 — MCPTT 세션 식별자(URI 의 gr, SessionIdentityToken) +
// g.3gpp.mcptt·g.3gpp.icsi-ref·isfocus
//   (TS 24.379 §6.3.3.1.2 1)·§6.3.3.2.3.1 3)4)·§6.3.3.2.3.2 5)6), RFC 3840 §9 — 확장 태그는 `+`, isfocus 는 기본 태그).
//   멤버 leg INVITE·개시자 18x/200 OK·이후 in-dialog 요청이 같은 값을 쓴다.
static const char *kFocusContactParams =
    "+g.3gpp.mcptt;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";isfocus";

// 제어 기능의 멤버 초대 offer 의 floor fmtp(TS 24.380 §14.2.2 mc_queueing · §14.2.3 mc_priority).
//   이어지는 offer(조건 재광고·세션 갱신)도 같은 값이다(§14.5 — 처음 offer 규칙, mc_granted 없음).
//   mc_priority 는 그룹 문서 <user-priority> 가 아니라 고정값이다(mcptt_standard_conformance.md C4a).
static const char *kMemberFloorOfferFmtp = "mc_queueing;mc_priority=3";

// CspServer.cpp — PTT 세션 참가 leg 의 dialog-event NOTIFY (dispatch_center.md §5.6a)
extern void SendPttDialogEventNotify( const std::string &strWatchedAor, const std::string &strDlgCallId,
                                      const std::string &strState, bool bInitiator, const std::string &strSessionUri,
                                      const std::string &strExtXml );

// Notify subscribers about group changes
extern void SendSipNotify( const std::string &uri, const std::string &etag, const std::string &action );
/** conference 구독자에게 참가자 NOTIFY 푸시 (CspServer.cpp) — 0 이면 구독자 없음(in-dialog 폴백). */
extern int SendConferenceNotifyToSubscribers( const std::string &strGroupId, const std::string &strBody,
                                              std::set<std::string> *psetNotifiedUsers );

// CscfModule.cpp — 제휴 변경 감사(E-AUD-009)·affiliation-info 구독자 NOTIFY (TS 24.379 §9.2.2.3.5)
extern void EmitAffiliationChanged( const std::string &strGroupId, const char *pszAction, const std::string &strUserId,
                                    EMcService eService = EMcService::Mcptt );
extern void SendAffiliationNotify( const std::string &strUserId, const std::string &strPid,
                                   EMcService eService = EMcService::Mcptt );

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

// MCPTT 세션 식별자(TS 24.379 §4.5) — 제어 기능의 세션을 가리키는 GRUU. Contact(멤버 leg INVITE·개시자 응답·이후
// in-dialog)에
//   `<sip:<그룹>@<CSP>;gr=<토큰>>` 로 싣고, 단말은 재합류 INVITE 의 Request-URI 로 쓴다(§10.1.1.2.4·§10.1.1.4.5.1).
//   토큰 = 세션 sesid 의 시각·순번 — 세션마다 새로 나고(같은 그룹의 지난 세션과 구별된다) 세션이 끝나면 사라진다.
std::string CGroupCallService::SessionIdentityToken( const std::string &strGroupId, bool bIssue ) {
    std::string strSesId;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto it = m_mapGroupSesId.find( strGroupId );
        if ( it != m_mapGroupSesId.end() ) strSesId = it->second;
    }
    if ( strSesId.empty() ) {
        if ( !bIssue ) return "";
        strSesId = GetOrIssueGroupSesId( strGroupId );
    }
    const size_t iPos = strSesId.find( "::csp::" );
    std::string strToken = iPos == std::string::npos ? strSesId : strSesId.substr( iPos + 7 );
    for ( size_t p = strToken.find( "::" ); p != std::string::npos; p = strToken.find( "::", p ) )
        strToken.replace( p, 2, "-" );
    return strToken;
}

bool CGroupCallService::IsSessionIdentityActive( const std::string &strGroupId, const std::string &strToken ) {
    if ( strToken.empty() ) return false;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        if ( !HasActiveLeg( strGroupId ) ) return false;
    }
    return SessionIdentityToken( strGroupId, false ) == strToken;
}

void CGroupCallService::RemoveGroupSesId( const std::string &strGroupId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    m_mapGroupSesId.erase( strGroupId );
    m_mapGroupSession.erase( strGroupId );  // 세션 속성(개시자·일제 통화)도 세션과 함께 끝난다
    m_mapGroupTalkers.erase( strGroupId );
    // 세션 정체성이 끝나면 런타임 condition(긴급/임박)·TNG2 도 함께 끝난다 — 잔존 조건이 다음 세션의
    //   fan-out(InviteMember 경로 포함)에 상속되는 것을 막는다.
    if ( m_mapGroupCond.erase( strGroupId ) )
        CLog::Print( LOG_INFO, "RemoveGroupSesId: group(%s) 잔존 condition 정리 (세션 종료)", strGroupId.c_str() );
}

void CGroupCallService::OnGroupAborted( const std::string &strGroupId ) {
    RemoveGroupSesId( strGroupId );
    CLog::Print( LOG_INFO, "GroupCallService: group=%s aborted by CMP (idle) — sesid 캐시 정리, 재사용 시 재수립",
                 strGroupId.c_str() );
}

CGroupCallService::GroupSession CGroupCallService::SessionOf( const std::string &strGroupId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    auto it = m_mapGroupSession.find( strGroupId );
    return it != m_mapGroupSession.end() ? it->second : GroupSession();
}

void CGroupCallService::SettlePendingSession( const std::string &strGroupId, const std::string &strInitiator,
                                              bool bEstablished ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    auto it = m_mapGroupSession.find( strGroupId );
    if ( it == m_mapGroupSession.end() || !it->second.bPending || it->second.strInitiator != strInitiator ) return;
    if ( bEstablished ) {
        it->second.bPending = false;
        return;
    }
    CLog::Print( LOG_INFO, "GroupSession: group=%s initiator=%s 개시 실패 — 세션 속성 폐기%s", strGroupId.c_str(),
                 strInitiator.c_str(), it->second.bBroadcast ? " (broadcast)" : "" );
    m_mapGroupSession.erase( it );
}

bool CGroupCallService::IsBroadcastInProgress( const std::string &strGroupId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    auto it = m_mapGroupSession.find( strGroupId );
    return it != m_mapGroupSession.end() && it->second.bBroadcast && !it->second.bPending;
}

bool CGroupCallService::IsOnDemandGroupCall( const CspPttGroup &clsGroup ) {
    return !clsGroup._isAdhoc && clsGroup._groupType != "chat" && clsGroup._groupType != "private";
}

bool CGroupCallService::IsBroadcastCapable( const CspPttGroup &clsGroup ) {
    return IsOnDemandGroupCall( clsGroup ) || ( clsGroup._isAdhoc && clsGroup._groupType != "private" );
}

bool CGroupCallService::AcceptsImplicitFloorRequest( const McpttFmtp &clsOffer, const CspPttGroup &clsGroup,
                                                     bool bNewSession, bool bListen ) {
    return clsOffer.iImplicit > 0 && bNewSession && !bListen && clsGroup._groupType != "chat";
}

std::string CGroupCallService::AnswerFloorFmtp( const McpttFmtp &clsOffer, bool bImplicitAccepted ) {
    std::string str;
    if ( clsOffer.iQueueing != 0 ) str = "mc_queueing";  // 1 = offer 가 실었다 · -1 = fmtp 없는 구단말(종전 광고 유지)
    if ( bImplicitAccepted ) str += std::string( str.empty() ? "" : ";" ) + "mc_implicit_request";
    return str;
}
void CGroupCallService::StripInitialOnlyFloorFmtp( CSipMessage *pclsOffer ) {
    if ( pclsOffer == NULL ) return;
    std::string &strBody = pclsOffer->m_strBody;
    static const char kPre[] = "a=fmtp:MCPTT";
    size_t iLine = strBody.find( kPre );
    if ( iLine == std::string::npos ) return;
    size_t iEol = strBody.find( "\r\n", iLine );
    const size_t iNext = iEol == std::string::npos ? strBody.size() : iEol + 2;
    if ( iEol == std::string::npos ) iEol = strBody.size();
    std::string strKept;
    const std::string strParams = strBody.substr( iLine + sizeof( kPre ) - 1, iEol - iLine - ( sizeof( kPre ) - 1 ) );
    for ( size_t iPos = 0; iPos <= strParams.size(); ) {
        size_t iEnd = strParams.find( ';', iPos );
        if ( iEnd == std::string::npos ) iEnd = strParams.size();
        std::string strTok = strParams.substr( iPos, iEnd - iPos );
        iPos = iEnd + 1;
        const size_t iB = strTok.find_first_not_of( " \t" );
        if ( iB == std::string::npos ) continue;
        strTok = strTok.substr( iB, strTok.find_last_not_of( " \t" ) - iB + 1 );
        if ( strcasecmp( strTok.c_str(), "mc_granted" ) == 0 ||
             strcasecmp( strTok.c_str(), "mc_implicit_request" ) == 0 )
            continue;
        strKept += ( strKept.empty() ? "" : ";" ) + strTok;
    }
    const std::string strLine = strKept.empty() ? std::string() : std::string( kPre ) + " " + strKept + "\r\n";
    strBody.replace( iLine, iNext - iLine, strLine );
    pclsOffer->m_iContentLength = (int)strBody.size();
}

bool CGroupCallService::RebuildReInviteFloorFmtp( const std::string &strCallId, CSipCallRtp *pclsRemoteRtp,
                                                  CSipCallRtp *pclsLocalRtp ) {
    if ( pclsLocalRtp == NULL || pclsLocalRtp->m_iApplicationPort <= 0 ) return false;
    if ( pclsLocalRtp->m_eMcMediaProfile != E_MC_MEDIA_MCPTT ) return false;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        if ( m_mapCallSession.find( strCallId ) == m_mapCallSession.end() ) return false;
    }
    McpttFmtp clsReOffer;
    ParseMcpttFmtp( pclsRemoteRtp, clsReOffer );
    const std::string strFmtp = AnswerFloorFmtp( clsReOffer, false );
    if ( strFmtp != pclsLocalRtp->m_strApplicationFmtp )
        CLog::Print( LOG_DEBUG, "ReInvite(%s): floor answer fmtp '%s' → '%s' (re-offer 기준, TS 24.380 §14.3.1)",
                     strCallId.c_str(), pclsLocalRtp->m_strApplicationFmtp.c_str(), strFmtp.c_str() );
    pclsLocalRtp->m_strApplicationFmtp = strFmtp;
    return true;
}

CmpGroupSession CGroupCallService::CmpSessionOf( const CspPttGroup &clsGroup ) {
    const GroupSession clsSes = SessionOf( clsGroup._id );
    CmpGroupSession clsCmp;
    clsCmp.strInitiator = clsSes.strInitiator;
    clsCmp.bBroadcast = clsSes.bBroadcast;
    // T4 출처는 호 종류별 하나(TS 24.379 §6.3.8.1 · TS 24.481 §7.2.2 o) — 그룹 호 = 그룹 문서 hang-timer.
    clsCmp.iT4Sec = IsOnDemandGroupCall( clsGroup ) ? std::max( 0, clsGroup._hangTimerSec ) : 0;
    return clsCmp;
}

void CGroupCallService::OnFloorInactivity( const std::string &strGroupId, const std::string &strSesId ) {
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto it = m_mapGroupSesId.find( strGroupId );
        if ( it == m_mapGroupSesId.end() || ( !strSesId.empty() && it->second != strSesId ) ) {
            CLog::Print( LOG_INFO, "OnFloorInactivity: group=%s sesid=%s — 현재 세션 아님(무시)", strGroupId.c_str(),
                         strSesId.c_str() );
            return;
        }
    }
    CspPttGroup clsGroup;
    if ( !gclsGroupMap.Select( strGroupId.c_str(), clsGroup ) || !IsOnDemandGroupCall( clsGroup ) ) return;
    CLog::Print( LOG_INFO, "OnFloorInactivity: group=%s T4(%ds) 만료 — 그룹 호 해제 (TS 24.379 §6.3.8.1)",
                 strGroupId.c_str(), clsGroup._hangTimerSec );
    ReleaseGroupSession( strGroupId, "t4_inactivity" );
}

void CGroupCallService::ReleaseGroupSession( const std::string &strGroupId, const char *pszReason ) {
    std::vector<std::string> vecLegs;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        for ( const auto &kv : m_mapCallSession )
            if ( kv.second.strGroupId == strGroupId ) vecLegs.push_back( kv.first );
    }
    CLog::Print( LOG_INFO, "ReleaseGroupSession: group=%s reason=%s legs=%zu", strGroupId.c_str(), pszReason,
                 vecLegs.size() );
    // leg 마다 BYE(미확립이면 CANCEL) + teardown 재진입 — psip 은 로컬 StopCall 로 끝낸 호에 EventCallEnd 를 올리지
    //   않으므로 OnCallTerminated 를 직접 부른다(private 잔여 leg 종료와 같은 규약). 마지막 확립 leg 의 teardown 이
    //   남은 미확립 초대 취소·CMP REMOVE·세션 정리를 끝내고, 그 뒤의 leg 는 맵에 없어 무동작이다.
    for ( const auto &strLeg : vecLegs ) {
        gclsUserAgent.StopCall( strLeg.c_str() );
        OnCallTerminated( strLeg );
    }
}

void CGroupCallService::CheckSessionLimits() {
    std::vector<std::pair<std::string, time_t>> vecStarts;
    std::vector<std::string> vecTng2;
    const time_t tNow = time( NULL );
    const int iTng2Sec = gclsCspServiceConfig.GetEmergencyGroupTimeLimitSec();
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        for ( const auto &kv : m_mapGroupSession ) {
            if ( kv.second.tStart <= 0 || !HasActiveLeg( kv.first ) ) continue;
            // 긴급 상태 동안은 TNG2 가 TNG3 를 대신한다 (§6.3.3.5.2 — TNG2 를 켜면 TNG3 를 켜지 않는다)
            auto itCond = m_mapGroupCond.find( kv.first );
            if ( itCond != m_mapGroupCond.end() && itCond->second.iCond >= 2 ) continue;
            vecStarts.push_back( { kv.first, kv.second.tStart } );
        }
        if ( iTng2Sec > 0 )
            for ( const auto &kv : m_mapGroupCond )
                if ( kv.second.iCond >= 2 && kv.second.tTng2Start > 0 && tNow - kv.second.tTng2Start >= iTng2Sec )
                    vecTng2.push_back( kv.first );
    }
    for ( const auto &strGroupId : vecTng2 ) {
        // TNG2 만료 (TS 24.379 §6.3.3.1.16) — 긴급 상태 해제, 참여 멤버 re-INVITE(§6.3.3.1.10), 비참여 제휴 멤버 통지
        //   (§6.3.3.1.11 — P-Asserted-Identity = 제어 기능 PSI). 요청자가 없으니 제외 대상도 없다.
        CLog::Print( LOG_INFO, "CheckSessionLimits: group=%s TNG2(%ds) 만료 — 긴급 상태 해제 (TS 24.379 §6.3.3.1.16)",
                     strGroupId.c_str(), iTng2Sec );
        CancelGroupEmergency( strGroupId, "", McpttIndicators(), "", "tng2" );
    }
    for ( const auto &st : vecStarts ) {
        CspPttGroup clsGroup;
        if ( !gclsGroupMap.Select( st.first.c_str(), clsGroup ) || !IsOnDemandGroupCall( clsGroup ) ) continue;
        if ( clsGroup._maxDurationSec <= 0 || tNow - st.second < clsGroup._maxDurationSec ) continue;
        CLog::Print( LOG_INFO, "CheckSessionLimits: group=%s TNG3(%ds) 만료 — 그룹 호 해제 (TS 24.379 §6.3.8.1)",
                     st.first.c_str(), clsGroup._maxDurationSec );
        ReleaseGroupSession( st.first, "max_duration" );
    }
}

// ── 개시자 응답 게이트 (TS 24.379 §6.3.3.3·§10.1.1.4.2·§11.1.1.4.2) ────────────────────────────────────────────
//   경고 문구는 §4.4 Table 4.4.2-2 원문. 거절 코드는 개시자에게 옮길 수 있는 최종 응답으로 맞춘다(3xx·로컬 합성 → 480).
static int _gateFinalCode( int iSipStatus ) {
    if ( iSipStatus == SIP_GONE ) return SIP_REQUEST_TIME_OUT;
    if ( iSipStatus == SIP_UNAUTHORIZED || iSipStatus == SIP_PROXY_AUTHENTICATION_REQUIRED ) return SIP_FORBIDDEN;
    if ( iSipStatus < SIP_BAD_REQUEST || iSipStatus >= 700 ) return SIP_TEMPORARILY_UNAVAILABLE;
    return iSipStatus;
}

// 캐시할 최종 거절 — 6xx 가 먼저(RFC 3261 §16.7 6 — 전역 실패), 그 밖에는 처음 받은 것.
static bool _gateBetterFinal( int iNew, int iOld ) {
    return iOld == 0 || ( iNew >= 600 && iOld < 600 );
}

// 개시자 200 OK 의 Warning 값 — 제어 기능 자신의 경고(111) 뒤에 멤버 응답에서 받은 것(§6.3.3.2.3.2 7)). RFC 3261 §20.43
// —
//   warning-value 를 쉼표로 잇는다.
static std::string _joinWarnings( const std::string &strOwn, const std::vector<std::string> &vecReceived ) {
    std::string strOut = strOwn;
    for ( const auto &w : vecReceived ) {
        if ( w.empty() || w == strOwn ) continue;
        if ( !strOut.empty() ) strOut += ", ";
        strOut += w;
    }
    return strOut;
}

void CGroupCallService::AckGateMemberResult( const std::string &strGroupId, const std::string &strMemberId,
                                             int iSipStatus ) {
    bool bAbandon = false;
    AckGate clsTaken;
    int iForward = 0;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto it = m_mapAckGate.find( strGroupId );
        if ( it == m_mapAckGate.end() ) return;
        AckGate &g = it->second;
        if ( g.setPending.erase( strMemberId ) == 0 ) return;
        const bool bRequired = g.setRequiredPending.erase( strMemberId ) > 0;
        if ( iSipStatus >= 200 && iSipStatus < 300 ) {
            ++g.iOkCount;  // §10.1.1.4.1.1 3) — 멤버 200 누계
            g.setAnswered.insert( strMemberId );
        } else {
            const int iCode = _gateFinalCode( iSipStatus );
            if ( _gateBetterFinal( iCode, g.iBestFinal ) ) g.iBestFinal = iCode;
            // §6.3.3.3 — TNG1 중 필수 멤버의 최종 거절: abandon 정책이면 그 응답을 112 와 함께 개시자에게, proceed 면
            // 계속
            if ( bRequired && g.bTng1 && !g.bTng1Expired ) {
                if ( !g.bProceed ) {
                    bAbandon = true;
                    iForward = iCode;
                } else {
                    g.bRequiredMissed = true;
                }
            }
        }
        CLog::Print( LOG_INFO, "AckGate: group=%s member=%s%s → %d (ok=%d/%d pending=%zu required=%zu)",
                     strGroupId.c_str(), strMemberId.c_str(), bRequired ? " [required]" : "", iSipStatus, g.iOkCount,
                     g.iMinToStart, g.setPending.size(), g.setRequiredPending.size() );
        if ( bAbandon ) {
            clsTaken = g;
            m_mapAckGate.erase( it );
        }
    }
    if ( bAbandon ) {
        AbortAckGate(
            strGroupId, clsTaken, iForward,
            McpttWarning( 112, "group call abandoned due to required group member not part of the group session",
                          gclsServiceMap.GetDomainByKind( "ptt" ) ),
            "ack_required_rejected" );
        return;
    }
    AckGateEvaluate( strGroupId );
}

void CGroupCallService::AckGateEvaluate( const std::string &strGroupId ) {
    enum { ACT_NONE, ACT_ANSWER, ACT_FINAL } eAct = ACT_NONE;
    std::string strWarning;
    const char *pszCause = "";
    int iCode = 0;
    AckGate clsTaken;
    const std::string strAgent = gclsServiceMap.GetDomainByKind( "ptt" );
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto it = m_mapAckGate.find( strGroupId );
        if ( it == m_mapAckGate.end() ) return;
        AckGate &g = it->second;
        const bool bTng1Running = g.bTng1 && !g.bTng1Expired;
        if ( bTng1Running ) {
            if ( g.setRequiredPending.empty() ) {
                if ( g.bRequiredMissed ) {
                    // 필수 멤버 거절을 proceed 정책으로 넘겼다 — 나머지 초대 전원이 200 이면 111 과 함께 응답, 아니면
                    // TNG1 계속
                    if ( g.setPending.empty() && g.iOkCount >= g.iMinToStart ) {
                        eAct = ACT_ANSWER;
                        strWarning =
                            McpttWarning( 111, "group call proceeded without all required group members", strAgent );
                    }
                } else {
                    g.bTng1 = false;  // §6.3.3.3 — 필수 멤버 전원 200: TNG1 정지
                    if ( g.iOkCount >= g.iMinToStart ) eAct = ACT_ANSWER;
                }
            }
        } else if ( g.bTng1Expired ) {
            // §6.3.3.3 — 만료 뒤: 최소 인원에 닿은 때 만료 동작(proceed = 200 + 111 / abandon = 480 + 112)
            if ( g.iOkCount >= g.iMinToStart ) {
                if ( g.bProceed ) {
                    eAct = ACT_ANSWER;
                    strWarning =
                        McpttWarning( 111, "group call proceeded without all required group members", strAgent );
                } else {
                    eAct = ACT_FINAL;
                    iCode = SIP_TEMPORARILY_UNAVAILABLE;
                    strWarning = McpttWarning(
                        112, "group call abandoned due to required group members not part of the group session",
                        strAgent );
                    pszCause = "ack_timeout_abandoned";
                }
            }
        } else if ( g.iOkCount >= g.iMinToStart ) {
            eAct = ACT_ANSWER;  // §10.1.1.4.2 — 멤버 200 누계가 최소 인원에 닿았다
            if ( g.bRequiredMissed )
                strWarning = McpttWarning( 111, "group call proceeded without all required group members", strAgent );
        }
        // 초대 전원이 최종 응답했는데 수락 조건에 못 닿았다 — 캐시한 최종 응답을 개시자에게 (§10.1.1.4.2 1))
        if ( eAct == ACT_NONE && g.setPending.empty() && g.iOkCount < g.iMinToStart ) {
            eAct = ACT_FINAL;
            iCode = g.iBestFinal > 0 ? g.iBestFinal : SIP_TEMPORARILY_UNAVAILABLE;
            if ( g.bRequiredMissed || g.bTng1Expired )
                strWarning = McpttWarning(
                    112, "group call abandoned due to required group members not part of the group session", strAgent );
            pszCause = "no_member_answered";
        }
        if ( eAct != ACT_NONE ) {
            clsTaken = g;
            m_mapAckGate.erase( it );
        }
    }
    if ( eAct == ACT_ANSWER ) {
        CLog::Print( LOG_INFO, "AckGate: group=%s initiator=%s — 수락 (ok=%d min=%d%s)", strGroupId.c_str(),
                     clsTaken.strInitiator.c_str(), clsTaken.iOkCount, clsTaken.iMinToStart,
                     strWarning.empty() ? "" : " Warning 111" );
        if ( !clsTaken.fnAnswer || clsTaken.fnAnswer( _joinWarnings( strWarning, clsTaken.vecWarnings ), false ) < 0 )
            AbortAckGate( strGroupId, clsTaken, SIP_INTERNAL_SERVER_ERROR, "", "accept_failed" );
        else if ( !strWarning
                       .empty() )  // 111 — 필수 멤버 없이 진행했다(§6.3.3.3): 자격 있는 개시자에게 미응답 멤버 INFO
            QueueNonAckInfo( strGroupId, clsTaken );
    } else if ( eAct == ACT_FINAL ) {
        AbortAckGate( strGroupId, clsTaken, iCode, strWarning, pszCause );
    }
}

void CGroupCallService::CheckAckGates() {
    std::vector<std::string> vecExpired;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        const auto tNow = std::chrono::steady_clock::now();
        for ( auto &kv : m_mapAckGate ) {
            AckGate &g = kv.second;
            if ( !g.bTng1 || g.bTng1Expired || tNow < g.tTng1End ) continue;
            g.bTng1Expired = true;  // §6.3.3.3 — TNG1 만료: 응답하지 않은 필수 멤버는 없이 간다(정책대로)
            if ( !g.setRequiredPending.empty() ) g.bRequiredMissed = true;
            vecExpired.push_back( kv.first );
            CLog::Print( LOG_INFO, "AckGate: group=%s TNG1 만료 — 필수 미응답 %zu, ok=%d/%d, action=%s",
                         kv.first.c_str(), g.setRequiredPending.size(), g.iOkCount, g.iMinToStart,
                         g.bProceed ? "proceed" : "abandon" );
        }
    }
    for ( const auto &strGroup : vecExpired ) AckGateEvaluate( strGroup );
    SendDueNonAckInfo();
}

void CGroupCallService::QueueNonAckInfo( const std::string &strGroupId, const AckGate &clsGate ) {
    PendingNonAckInfo clsInfo;
    for ( const auto &strMember : clsGate.setInvited )
        if ( !clsGate.setAnswered.count( strMember ) ) clsInfo.vecNonAck.push_back( strMember );
    if ( clsInfo.vecNonAck.empty() ) return;
    // §6.3.3.3 2) — 개시자 user profile 의 <allow-to-receive-non-acknowledged-users-information> 가 true 일 때만
    CspUserProfile clsProf;
    if ( gclsDbManager.SelectUserProfile( clsGate.strInitiator, clsProf ) <= 0 || !clsProf.m_bAllowNonAckUsersInfo )
        return;
    clsInfo.strCallId = clsGate.strCallId;
    clsInfo.strGroupId = strGroupId;
    clsInfo.tDue = time( NULL ) + 1;  // ACK 뒤에 — "Upon receiving a SIP ACK to the above SIP 200 (OK)"
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    m_vecNonAckInfo.push_back( clsInfo );
}

void CGroupCallService::SendDueNonAckInfo() {
    std::vector<PendingNonAckInfo> vecDue;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        const time_t tNow = time( NULL );
        for ( auto it = m_vecNonAckInfo.begin(); it != m_vecNonAckInfo.end(); ) {
            if ( it->tDue > tNow ) {
                ++it;
                continue;
            }
            // 개시자 leg 이 그 사이 끝났으면 버린다
            if ( m_mapCallSession.count( it->strCallId ) ) vecDue.push_back( *it );
            it = m_vecNonAckInfo.erase( it );
        }
    }
    for ( const auto &clsInfo : vecDue ) {
        // TS 24.379 §6.3.3.3 3) — mcptt-info(Annex F.1) 의 <non-acknowledged-user> = 200 을 보내지 않은 초대 멤버의
        // MCPTT ID.
        //   이 요소는 mcptt-Params 의 <anyExt> 안에 둔다(F.1 스키마 — 전역 요소).
        std::ostringstream oss;
        oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
            << "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\""
            << " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
            << "  <mcptt-Params>\r\n"
            << McpttInfoUri( "mcptt-calling-group-id", "tel:" + clsInfo.strGroupId ) << "    <anyExt>\r\n";
        for ( const auto &strMember : clsInfo.vecNonAck )
            oss << "  " << McpttInfoUri( "non-acknowledged-user", "tel:" + strMember );
        oss << "    </anyExt>\r\n"
            << "  </mcptt-Params>\r\n"
            << "</mcpttinfo>\r\n";
        const bool bSent = gclsUserAgent.SendInfoWithBody( clsInfo.strCallId.c_str(), "g.3gpp.mcptt-info",
                                                           "application", "vnd.3gpp.mcptt-info+xml", oss.str() );
        CLog::Print( LOG_INFO, "AckGate: group=%s — 미응답 멤버 INFO %zu명 %s (§6.3.3.3)", clsInfo.strGroupId.c_str(),
                     clsInfo.vecNonAck.size(), bSent ? "전송" : "실패(다이얼로그 없음)" );
    }
}

void CGroupCallService::AbortAckGate( const std::string &strGroupId, const AckGate &clsGate, int iSipStatus,
                                      const std::string &strWarning, const char *pszReason ) {
    CLog::Print( LOG_INFO, "AckGate: group=%s initiator=%s — 개시 중단 %d (%s)", strGroupId.c_str(),
                 clsGate.strInitiator.c_str(), iSipStatus, pszReason );
    if ( iSipStatus > 0 ) {  // 개시자에게 최종 응답 (CANCEL 로 끝난 것이면 psip 이 이미 487 을 줬다)
        std::vector<std::pair<std::string, std::string>> vecHdr;
        if ( !strWarning.empty() ) vecHdr.push_back( { "Warning", strWarning } );
        gclsUserAgent.StopCall( clsGate.strCallId.c_str(), iSipStatus, NULL, vecHdr );
    }
    const std::string strSesId = GetOrIssueGroupSesId( strGroupId );
    // 개시자 몫으로 선할당한 멤버 포트(GetOrAllocMemberPort 의 CMP 선할당 JOIN)를 거둔다
    LeaveGroupOrQueue( strGroupId, clsGate.strInitiator, strSesId, "개시 중단" );
    InvalidateMemberPort( strGroupId, clsGate.strInitiator );
    SettlePendingSession( strGroupId, clsGate.strInitiator, false );
    CspPttGroup clsGroup;
    const bool bGroup = gclsGroupMap.Select( strGroupId.c_str(), clsGroup );
    if ( gclsCallDir.IsEnabled() )
        gclsCallDir.PttAttempt( strGroupId, bGroup ? std::to_string( clsGroup._dbId ) : "", clsGate.strInitiator,
                                "failed", iSipStatus > 0 ? "no_answer" : "canceled", pszReason, iSipStatus, strSesId );
    // 초대 leg BYE/CANCEL — 마지막 leg 의 teardown 이 CMP REMOVE·세션 정리를 끝낸다(게이트를 먼저 지웠다)
    ReleaseGroupSession( strGroupId, pszReason );
    bool bLeftover;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        bLeftover = m_mapGroupRtp.count( strGroupId ) > 0;
        for ( const auto &kv : m_mapCallSession )
            if ( kv.second.strGroupId == strGroupId ) bLeftover = false;  // 아직 leg 이 있다 — 그 teardown 이 거둔다
    }
    if ( bLeftover ) {  // 초대한 leg 이 하나도 없었다 — 세션 자원을 직접 거둔다
        gclsCmpClient.RemoveGroup( strGroupId, strSesId );
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            m_mapGroupRtp.erase( strGroupId );
        }
        RemoveGroupSesId( strGroupId );
        if ( gclsCallDir.IsEnabled() ) gclsCallDir.PttSessionEnd( strGroupId, "error" );
        if ( gclsDbManager.IsConnected() ) gclsDbManager.EndGroupCallLog( strGroupId );
        if ( bGroup && clsGroup._isAdhoc ) gclsGroupMap.Remove( strGroupId.c_str() );
    }
}

bool CGroupCallService::OnAckGateRinging( const std::string &strCallId, int iSipStatus ) {
    std::string strInitiatorCallId;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto itS = m_mapCallSession.find( strCallId );
        if ( itS == m_mapCallSession.end() || itS->second.bEstablished ) return false;
        auto itG = m_mapAckGate.find( itS->second.strGroupId );
        if ( itG == m_mapAckGate.end() ) return false;
        if ( itG->second.bPrivate && iSipStatus == SIP_RINGING && !itG->second.bRingSent ) {
            itG->second.bRingSent = true;
            strInitiatorCallId = itG->second.strCallId;
        }
    }
    if ( !strInitiatorCallId.empty() ) gclsUserAgent.RingCall( strInitiatorCallId.c_str(), SIP_RINGING, NULL );
    return true;
}

void CGroupCallService::OnMemberInviteResponse( const std::string &strCallId, CSipMessage *pclsResponse ) {
    if ( !pclsResponse ) return;
    const int iStatus = pclsResponse->m_iStatusCode;
    bool bMember = false, bPrack = false, bAnswer = false;
    std::string strGroupId;
    AckGate clsTaken;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto itS = m_mapCallSession.find( strCallId );
        if ( itS == m_mapCallSession.end() || itS->second.bEstablished || itS->second.bInitiator ) return;
        bMember = true;
        strGroupId = itS->second.strGroupId;
        // RFC 3262 §4 — 신뢰성 임시 응답(Require: 100rel + RSeq)은 UAC 가 PRACK 으로 받는다
        if ( iStatus > SIP_TRYING && iStatus < SIP_OK ) {
            CSipHeader *pclsRequire = pclsResponse->GetHeader( "Require" );
            bPrack = pclsRequire && pclsRequire->m_strValue.find( "100rel" ) != std::string::npos &&
                     pclsResponse->GetHeader( "RSeq" ) != NULL;
        }
        auto itG = m_mapAckGate.find( strGroupId );
        if ( itG != m_mapAckGate.end() ) {
            AckGate &g = itG->second;
            // §6.3.3.2.3.2 7) — 멤버 응답의 Warning 을 개시자 200 OK 에 옮긴다
            for ( auto &h : pclsResponse->m_clsHeaderList ) {
                if ( strcasecmp( h.m_strName.c_str(), "Warning" ) != 0 || h.m_strValue.empty() ) continue;
                if ( std::find( g.vecWarnings.begin(), g.vecWarnings.end(), h.m_strValue ) == g.vecWarnings.end() )
                    g.vecWarnings.push_back( h.m_strValue );
            }
            // §10.1.1.4.2 · §11.1.1.4.2 — 183 + P-Answer-State: Unconfirmed, TNG1 이 돌지 않음(없었거나 필수 멤버 전원
            // 응답으로
            //   멈춤), 미디어 버퍼링 지원, 개시자 최종 응답 전 → 개시자에게 200 OK(P-Answer-State: Unconfirmed)
            if ( iStatus == SIP_SESSION_PROGRESS && !g.bTng1 && !g.bTng1Expired && !g.bRequiredMissed &&
                 gclsCmpClient.SupportsMediaBuffer() ) {
                CSipHeader *pclsState = pclsResponse->GetHeader( "P-Answer-State" );
                if ( pclsState && strncasecmp( pclsState->m_strValue.c_str(), "Unconfirmed", 11 ) == 0 ) {
                    bAnswer = true;
                    clsTaken = g;
                    m_mapAckGate.erase( itG );
                }
            }
        }
    }
    if ( !bMember ) return;
    if ( bPrack ) gclsUserAgent.SendPrack( strCallId.c_str(), NULL );
    if ( bAnswer ) {
        CLog::Print( LOG_INFO,
                     "AckGate: group=%s initiator=%s — 멤버 183 Unconfirmed → 200 OK (P-Answer-State: Unconfirmed)",
                     strGroupId.c_str(), clsTaken.strInitiator.c_str() );
        if ( !clsTaken.fnAnswer || clsTaken.fnAnswer( _joinWarnings( "", clsTaken.vecWarnings ), true ) < 0 )
            AbortAckGate( strGroupId, clsTaken, SIP_INTERNAL_SERVER_ERROR, "", "accept_failed" );
    }
}

void CGroupCallService::OnAckGateLegEnd( const std::string &strCallId, int iSipStatus ) {
    std::string strGroupId, strMemberId;
    bool bInitiator = false;
    AckGate clsTaken;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        for ( auto it = m_mapAckGate.begin(); it != m_mapAckGate.end(); ++it ) {
            if ( it->second.strCallId == strCallId ) {  // 개시자가 개시를 거뒀다(CANCEL) — 세션 개시 중단
                bInitiator = true;
                strGroupId = it->first;
                clsTaken = it->second;
                m_mapAckGate.erase( it );
                break;
            }
        }
        if ( !bInitiator ) {
            auto itS = m_mapCallSession.find( strCallId );
            if ( itS == m_mapCallSession.end() || itS->second.bEstablished ) return;
            if ( m_mapAckGate.find( itS->second.strGroupId ) == m_mapAckGate.end() ) return;
            strGroupId = itS->second.strGroupId;
            strMemberId = itS->second.strMemberId;
        }
    }
    if ( bInitiator ) {
        AbortAckGate( strGroupId, clsTaken, 0, "", "initiator_canceled" );
        return;
    }
    AckGateMemberResult( strGroupId, strMemberId, iSipStatus > 0 ? iSipStatus : SIP_REQUEST_TIME_OUT );
}

bool CGroupCallService::GetOrAllocMemberPort( const std::string &strGroupId, const std::string &strMemberId,
                                              int &iAudioPort ) {
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto itRtp = m_mapGroupRtp.find( strGroupId );
        if ( itRtp != m_mapGroupRtp.end() ) {
            auto itM = itRtp->second.memberPorts.find( strMemberId );
            if ( itM != itRtp->second.memberPorts.end() && itM->second > 0 ) {
                iAudioPort = itM->second;
                return true;
            }
        }
    }
    // 캐시에 없음(늦은 참가자/로스터 외) — PTT_JOIN ①(선할당, user_ip 없이)로 멤버 전용 포트 확보 (멱등)
    int iLocalAudio = 0;
    PurgePendingLeave( strGroupId, strMemberId );  // JOIN 이 «지금 있다» 는 권위 — 밀린 LEAVE 를 버린다
    if ( !gclsCmpClient.JoinGroup( strGroupId, strMemberId, "", 0, 0, GetOrIssueGroupSesId( strGroupId ), "",
                                   &iLocalAudio ) ||
         iLocalAudio <= 0 ) {
        CLog::Print( LOG_ERROR, "GetOrAllocMemberPort: PTT_JOIN prealloc failed group=%s member=%s", strGroupId.c_str(),
                     strMemberId.c_str() );
        return false;
    }
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto itRtp = m_mapGroupRtp.find( strGroupId );
        if ( itRtp != m_mapGroupRtp.end() ) itRtp->second.memberPorts[strMemberId] = iLocalAudio;
    }
    iAudioPort = iLocalAudio;
    return true;
}

void CGroupCallService::InvalidateMemberPort( const std::string &strGroupId, const std::string &strMemberId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    auto itRtp = m_mapGroupRtp.find( strGroupId );
    if ( itRtp != m_mapGroupRtp.end() ) itRtp->second.memberPorts.erase( strMemberId );
}

CGroupCallService::~CGroupCallService() {
    StopMonitor();
}

/** leg 별 PT 재작성 파라미터 산출 (docs/api/cmp_media_api.md §7.4).
 *  user_pt/user_te_pt = 이 leg 의 원격 SDP(개시자=offer, 수신자=answer)가 수신 선언한
 *  audio/TE wire PT — CMP 가 이 leg 로 송신 시 스탬프. user_src_pt/user_src_te_pt =
 *  서버가 그 leg 쪽으로 낸 SDP 의 PT(= UE 송신 PT, RFC 3264) — CMP ingress 분류 기준.
 *  - 수신자 leg(bServerOffered): 서버 offer 는 코덱 테이블 PT.
 *  - 개시자 leg: 서버 answer 는 오퍼 echo(psip AddSdp 규칙) → src = user 와 동일.
 *  pstrCodec: 협상 오디오 코덱 문자열(코덱 테이블 top, 예 "AMR-WB/16000") — 녹취 메타용. */
void CGroupCallService::GetLegPt( const std::string &strCallId, bool bServerOffered, int &iUserPt, int &iUserSrcPt,
                                  int &iUserTePt, int &iUserSrcTePt, std::string *pstrCodec ) {
    const CSipCodecEntry &clsTop = CSipCodecTable::GetTop();
    const CSipCodecEntry &clsTe = CSipCodecTable::GetTelephoneEvent();
    int iPt = -1, iTePt = -1;
    gclsUserAgent.GetRemotePayloadTypes( strCallId.c_str(), clsTop.GetMatchPrefix().c_str(), iPt, iTePt );
    iUserPt = iPt > 0 ? iPt : clsTop.m_iPt;
    iUserTePt = iTePt > 0 ? iTePt : clsTe.m_iPt;
    if ( bServerOffered ) {
        iUserSrcPt = clsTop.m_iPt;
        iUserSrcTePt = clsTe.m_iPt;
    } else {
        iUserSrcPt = iUserPt;
        iUserSrcTePt = iUserTePt;
    }
    if ( pstrCodec ) *pstrCodec = clsTop.GetMatchPrefix();
}

void CGroupCallService::ParseMcpttFmtp( CSipCallRtp *pclsRtp, McpttFmtp &clsFmtp ) {
    if ( pclsRtp == NULL ) return;
    for ( const auto &clsMedia : pclsRtp->m_clsMediaList ) {
        if ( strcasecmp( clsMedia.m_strMedia.c_str(), "application" ) != 0 ) continue;
        for ( const auto &clsAttr : clsMedia.m_clsAttributeList ) {
            // a=fmtp:MCPTT mc_queueing;mc_priority=4[;mc_implicit_request][;mc_granted] → name="fmtp", value="MCPTT
            // mc_..."
            if ( strcasecmp( clsAttr.m_strName.c_str(), "fmtp" ) != 0 ) continue;
            if ( strncasecmp( clsAttr.m_strValue.c_str(), "MCPTT", 5 ) != 0 ) continue;
            clsFmtp.iQueueing = 0;  // fmtp:MCPTT 존재 — 이제부터 미포함 파라미터는 "미협상"
            std::string strParams = clsAttr.m_strValue.substr( 5 );
            size_t iPos = 0;
            while ( iPos < strParams.size() ) {
                size_t iEnd = strParams.find( ';', iPos );
                if ( iEnd == std::string::npos ) iEnd = strParams.size();
                std::string strTok = strParams.substr( iPos, iEnd - iPos );
                iPos = iEnd + 1;
                // 공백 trim — CR/LF 포함 (SDP 마지막 라인의 잔존 \r 이 마지막 토큰 매칭을
                //   깨뜨린다: "mc_no_floor_ctrl\r" != "mc_no_floor_ctrl")
                size_t iB = strTok.find_first_not_of( " \t\r\n" );
                if ( iB == std::string::npos ) continue;
                strTok = strTok.substr( iB, strTok.find_last_not_of( " \t\r\n" ) - iB + 1 );
                if ( strcasecmp( strTok.c_str(), "mc_queueing" ) == 0 ) {
                    clsFmtp.iQueueing = 1;
                } else if ( strncasecmp( strTok.c_str(), "mc_priority=", 12 ) == 0 ) {
                    int iPrio = atoi( strTok.c_str() + 12 );
                    if ( iPrio > 0 ) clsFmtp.iMaxPriority = iPrio;
                } else if ( strcasecmp( strTok.c_str(), "mc_implicit_request" ) == 0 ) {
                    clsFmtp.iImplicit = 1;  // 발언 요청(§14.2.5) — 받아들일지는 호출자가 세션 상태로 정한다
                } else if ( strcasecmp( strTok.c_str(), "mc_granted" ) == 0 ) {
                    clsFmtp.iGrantedCap = 1;  // 능력 표시(§14.2.4) — 요청으로 읽지 않는다(§12.1.2.2 NOTE 2)
                } else if ( strcasecmp( strTok.c_str(), "mc_no_floor_ctrl" ) == 0 ) {
                    clsFmtp.iNoFloorCtrl = 1;
                }
            }
            return;
        }
    }
}

// 미디어 SRTP answer 협상 (SDES — media_security.md §4·§5.2). 정책 × 수신 offer crypto.
//   수용 관대화(§4): AVP + a=crypto 병기(best-effort) offer 도 SRTP 로 수락한다.
//   반환 1=SRTP 수락(strSuite/strUeInline 채움), 0=평문 진행, -1=협상 실패(호출자가 488).
static int _evalAnswerSdes( const ServiceInfo &svc, CSipCallRtp *pclsRtp, std::string &strSuite,
                            std::string &strUeInline ) {
    const bool bValid = !pclsRtp->m_strRemoteCryptoSuite.empty() &&
                        MediaSdes::IsSupportedSuite( pclsRtp->m_strRemoteCryptoSuite ) &&
                        MediaSdes::ValidInlineKeyB64( pclsRtp->m_strRemoteCryptoKey );
    if ( svc.media_srtp == "required" ) {
        if ( !bValid ) return -1;  // crypto 없는/불량 offer — 488 (§4 표)
    } else if ( svc.media_srtp == "optional" ) {
        // SAVP 인데 유효 crypto 가 없으면 협상 불가 (RFC 4568 — SAVP answer 는 crypto 필수)
        if ( !bValid ) return pclsRtp->m_bRemoteSavp ? -1 : 0;
    } else {
        // off — a=crypto 무시(평문). SAVP offer 는 평문 answer 가 성립하지 않으므로 488.
        return pclsRtp->m_bRemoteSavp ? -1 : 0;
    }
    strSuite = pclsRtp->m_strRemoteCryptoSuite;
    strUeInline = pclsRtp->m_strRemoteCryptoKey;
    return 1;
}

/**
 * @brief Process Incoming Group Call (A calling Group)
 */
bool CGroupCallService::ProcessGroupCall( const char *pszGroupId, const char *pszCallerInfo, const char *pszCallId,
                                          CSipCallRtp *pclsRtp, CSipCallRoute *pclsRoute, int iCondition,
                                          bool bBroadcastInd ) {
    CspPttGroup clsGroup;

    if ( gclsGroupMap.Select( pszGroupId, clsGroup ) == false ) {
        // 시도 장부(sip_statistics.md §2.3) — 여기부터 아래 일곱 갈래는 세션 디렉터리를
        //   만들기 **전에** 반환한다. 장부에 남기지 않으면 실패한 시도가 원천에 없어
        //   성공률의 분모가 서지 않는다(§8 Y6). 청취 leg 은 시도가 아니므로 제외한다.
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.PttAttempt( pszGroupId, "", pszCallerInfo, "failed", "denied", "group_not_found", 404 );
        return false;
    }

    // 개시자 멤버십·청취 leg 판정.
    //   청취 leg = offer 가 a=recvonly (RFC 3264) — 관제사가 진행 중 그룹콜을 듣기만 하는 합류
    //   (dispatch_center.md §5.6, TS 24.379 ambient listening 자격 재사용). 인가 2단 = 자격
    //   ptt_user_profile.allow_ambient_listening(TS 24.484) + 범위 = 관제 그룹 ptt_listen. 실패·DB 불가는
    //   403(fail-closed — 당사자 모르게 미디어를 인도하는 동작). 비멤버의 일반(sendrecv) INVITE 는 403
    //   (TS 24.379 §10.1.1 — 그룹 멤버가 아닌 사용자의 개시/합류 거절).
    bool bIsMember = false;
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( pUser && pUser->_id == pszCallerInfo ) {
            bIsMember = true;
            break;
        }
    }
    const bool bListen = ( pclsRtp != NULL && pclsRtp->m_eDirection == E_RTP_RECV );
    std::string strListenGroup;
    bool bListenHidden = true;
    // 청취 인가 판정 시점의 정책 세대 — 아래 CMP 왕복 동안 자격을 거두면 스윕은 아직 등록되지 않은 이 leg 을
    //   지나친다. 200 OK 직전에 세대가 달라졌으면 같은 판정을 한 번 더 한다(dispatch_center.md §5.10).
    unsigned uListenAuthzGen = 0;
    if ( bListen ) {
        // 2단 인가 (§5.6): 자격 = TS 24.484 프로파일 allow_ambient_listening(역할 배정의 결과로 CSC 가 동기),
        //   범위 = 청취자 회선의 역할 ptt_listen. strListenGroup 은 역할 id (감사 E-AUD-016 `role`).
        strListenGroup = gclsRoleMap.RoleIdForLine( pszCallerInfo );
        uListenAuthzGen = CspAuthz::PolicyGeneration();
        const std::string strDeny = ListenDenyReason( clsGroup, pszCallerInfo );
        if ( !strDeny.empty() ) {
            CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) listener(%s) role(%s) denied (%s) → 403", pszGroupId,
                         pszCallerInfo, strListenGroup.c_str(), strDeny.c_str() );
            gclsUserAgent.StopCall( pszCallId, SIP_FORBIDDEN );
            EmitPttListenAudit( "denied", pszCallerInfo, strListenGroup, pszGroupId, "", -1 );
            return true;
        }
        bListenHidden = gclsRoleMap.ListenHidden( pszCallerInfo );
        // 청취는 진행 중 세션에 합류하는 것이다 — 청취자가 세션을 개시(멤버 fan-out)하지 않는다. 상시 세션(chat)만
        // 예외.
        bool bHasSession;
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            bHasSession = HasActiveLeg( pszGroupId );
        }
        if ( !bHasSession && clsGroup._groupType != "chat" ) {
            CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) listener(%s) — no active session → 480", pszGroupId,
                         pszCallerInfo );
            gclsUserAgent.StopCall( pszCallId, SIP_TEMPORARILY_UNAVAILABLE );
            return true;
        }
        iCondition = 0;  // 청취자는 세션 조건(긴급/임박)을 개시·상향하지 않는다
    } else if ( !bIsMember ) {
        CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) Caller(%s) not a member → 403", pszGroupId, pszCallerInfo );
        gclsUserAgent.StopCall( pszCallId, SIP_FORBIDDEN );
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.PttAttempt( pszGroupId, std::to_string( clsGroup._dbId ), pszCallerInfo, "failed", "denied",
                                    "not_member", 403 );
        return true;
    }

    // 협상 게이트 (RFC 3264): 개시자 오퍼에 서비스 코덱(코덱 테이블 최우선, 기본 AMR-WB)이 없으면
    //   488 로 거부한다. 없는데 수락하면 answer 가 오퍼에 없는 코덱을 선언(비규격)하게 되고 미디어도
    //   성립하지 않는다. (m_clsCodecList 는 GetSipCallRtp 가 rtpmap 이름 매칭으로 테이블 PT 정규화)
    if ( pclsRtp ) {
        const CSipCodecEntry &clsSvcCodec = CSipCodecTable::GetTop();
        bool bHasSvcCodec = false;
        for ( CODEC_LIST::iterator it = pclsRtp->m_clsCodecList.begin(); it != pclsRtp->m_clsCodecList.end(); ++it ) {
            if ( *it == clsSvcCodec.m_iPt ) {
                bHasSvcCodec = true;
                break;
            }
        }
        if ( !bHasSvcCodec ) {
            CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) Caller(%s) offer has no service codec(%s) → 488",
                         pszGroupId, pszCallerInfo, clsSvcCodec.m_strName.c_str() );
            gclsUserAgent.StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
            if ( gclsCallDir.IsEnabled() && !bListen )
                gclsCallDir.PttAttempt( pszGroupId, std::to_string( clsGroup._dbId ), pszCallerInfo, "failed", "error",
                                        "codec_mismatch", 488 );
            return true;  // 488 응답 완료 — 호출측(dispatcher) 이 실패로 보고 403 을 덧보내지 않게 한다
        }
    }

    // 미디어 SRTP 협상 (SDES — media_security.md §4·§5): 접속서비스 정책 × 개시자 offer crypto.
    //   실패는 488 — required 인데 crypto 부재, off/optional 인데 성립 불가한 SAVP offer.
    std::string strCallerSuite, strCallerUeKey, strCallerSrvKey;
    if ( pclsRtp ) {
        ServiceInfo clsSrtpSvc = gclsServiceMap.GetForUser( pszCallerInfo, "ptt" );
        int iSdes = _evalAnswerSdes( clsSrtpSvc, pclsRtp, strCallerSuite, strCallerUeKey );
        if ( iSdes > 0 ) {
            strCallerSrvKey = MediaSdes::GenerateInlineKeyB64();
            if ( strCallerSrvKey.empty() ) iSdes = -1;  // 키 생성 실패 — 평문 조용 폴백 금지
        }
        if ( iSdes < 0 ) {
            CLog::Print( LOG_INFO,
                         "ProcessGroupCall: Group(%s) Caller(%s) SRTP negotiation failed (policy=%s savp=%d) → 488",
                         pszGroupId, pszCallerInfo, clsSrtpSvc.media_srtp.c_str(), pclsRtp->m_bRemoteSavp ? 1 : 0 );
            gclsUserAgent.StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
            if ( gclsCallDir.IsEnabled() && !bListen )
                gclsCallDir.PttAttempt( pszGroupId, std::to_string( clsGroup._dbId ), pszCallerInfo, "failed", "error",
                                        "srtp_failed", 488 );
            return true;
        }
    }

    // condition(긴급·임박) 개시 인가 (TS 24.379 §6.3.3.1.13.2) — 그룹 capability(emergency_call,
    //   두 tier 공통) + 사용자 프로파일(개시 인가·DedicatedGroup 대상 일치). 미인가는 403 거절
    //   (§6.3.3.1.14) — 단말이 normal 재발신으로 폴백한다.
    int iCond = iCondition;
    if ( iCond >= 1 ) {
        std::string strReason;
        if ( !IsConditionInitAuthorized( clsGroup, pszCallerInfo, strReason ) ) {
            CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) Caller(%s) condition(%d) denied (%s) → 403", pszGroupId,
                         pszCallerInfo, iCond, strReason.c_str() );
            gclsUserAgent.StopCall( pszCallId, SIP_FORBIDDEN );
            if ( gclsCallDir.IsEnabled() && !bListen )
                gclsCallDir.PttAttempt( pszGroupId, std::to_string( clsGroup._dbId ), pszCallerInfo, "failed", "denied",
                                        "policy_denied", 403 );
            return true;  // 403 응답 완료 — 호출측(dispatcher)이 중복 응답하지 않게 한다
        }
    }
    // affiliation 검사 (TS 24.379 §10.1.1.4.2 14)a)·15)a) 개시·합류, §10.1.1.4.5.1 8) 재합류, §10.1.2.4.1.1 6)·13) chat
    // —
    //   §6.3.6). affiliation 을 쓰는 그룹(require_affiliation — fan-out 이 affiliated 멤버만 초대하는 그룹)에서만
    //   판정한다: 그 밖의 그룹은 멤버 전원을 초대하므로 멤버십이 곧 affiliation 이다. DB 단절이면 fan-out 과 같이
    //   건너뛴다. 청취 leg(비멤버 관제사 — 2단 인가가 따로)·사설 호는 대상이 아니다. · 긴급·임박 위험(인가된 요청,
    //   위에서 판정) 또는 chat = 암묵적 affiliation(§9.2.2.3.7) — 자격(§9.2.2.3.6·9.2.2.3.8)은
    //     그룹 존재 + 멤버십이라 여기까지 온 멤버는 자격이 있다.
    //   · 그 밖(편성 그룹 호의 일반 개시·합류·재합류) = 403 + Warning "120 user is not affiliated to this group".
    if ( !bListen && clsGroup._requireAffiliation && clsGroup._groupType != "private" && gclsDbManager.IsConnected() &&
         !gclsDbManager.IsAffiliated( pszGroupId, pszCallerInfo ) ) {
        if ( iCond >= 1 || clsGroup._groupType == "chat" ) {
            CUserInfo clsAffUser;
            const std::string strClientId =
                gclsUserMap.Select( pszCallerInfo, clsAffUser ) ? clsAffUser.m_strContactUri : std::string();
            if ( gclsDbManager.InsertAffiliation( pszGroupId, pszCallerInfo, strClientId, kImplicitAffiliationSec ) ) {
                EmitAffiliationChanged( pszGroupId, "affiliate", pszCallerInfo );
                // §9.2.2.3.7 5) → §9.2.2.3.5 — 암묵적 제휴라 되돌릴 PUBLISH p-id 가 없다.
                SendAffiliationNotify( pszCallerInfo, "" );
                CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) Caller(%s) implicit affiliation (%s)", pszGroupId,
                             pszCallerInfo, iCond >= 1 ? "emergency/imminent peril" : "chat" );
            } else {
                CLog::Print( LOG_ERROR, "ProcessGroupCall: Group(%s) Caller(%s) implicit affiliation 미기록 — 계속",
                             pszGroupId, pszCallerInfo );
            }
        } else {
            CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) Caller(%s) not affiliated → 403 (120)", pszGroupId,
                         pszCallerInfo );
            std::vector<std::pair<std::string, std::string>> vecHdr = {
                { "Warning", McpttWarning( 120, "user is not affiliated to this group",
                                           gclsServiceMap.GetDomainByKind( "ptt" ) ) } };
            gclsUserAgent.StopCall( pszCallId, SIP_FORBIDDEN, NULL, vecHdr );
            if ( gclsCallDir.IsEnabled() )
                gclsCallDir.PttAttempt( pszGroupId, std::to_string( clsGroup._dbId ), pszCallerInfo, "failed", "denied",
                                        "not_affiliated", SIP_FORBIDDEN );
            return true;
        }
    }
    int iPrevCond = 0;
    bool bActiveSession = false;
    bool bNewSession = false;
    // 이 INVITE 가 세션 속성을 선점했는가 — 개시자 leg 확립 전에 어느 경로로 끝나든(세션 창·AcceptCall 실패·
    //   CMP 포트 부족) 선점을 지운다. 남기면 그 그룹의 conference 구독이 다음 세션까지 480/105 를 받는다
    //   (TS 24.379 §10.1.3.4.1 — 일제 통화 진행 중에만).
    bool bClaimedSession = false;
    struct PendingGuard {
        CGroupCallService *pSvc;
        const char *pszGroup;
        const char *pszCaller;
        bool &bArmed;
        ~PendingGuard() {
            if ( bArmed ) pSvc->SettlePendingSession( pszGroup, pszCaller, false );
        }
    } clsPendingGuard{ this, pszGroupId, pszCallerInfo, bClaimedSession };
    bool bNewEmergencyUser = false;  // 긴급 진행 중 그룹에 다른 사용자가 긴급으로 합류 (§10.1.1.4.7 6)c) 와 같은 뜻)
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto itPrev = m_mapGroupCond.find( pszGroupId );
        if ( itPrev != m_mapGroupCond.end() ) iPrevCond = itPrev->second.iCond;
        // 세션 활성 판정 = 이 그룹의 기존 호 존재 (이 INVITE 의 leg 등록은 아래에서 — 미포함).
        //   InviteMember 의 stale-cache 가드와 동일 기준. 청취 leg 는 세션을 구성하지 않는다.
        bActiveSession = HasActiveLeg( pszGroupId );
        // 세션 속성(개시자·일제 통화)은 세션을 개시하는 INVITE 한 번에서 정한다 (mcptt_broadcast_group_call.md §3.1).
        //   진행 중 세션에 들어오는 INVITE(늦은 합류·재참여)는 <broadcast-ind> 와 무관하게 참가자 합류다 —
        //   일반 통화 중인 그룹을 일제 통화로 바꾸지 않고(Release 18 에 상향 절차 없음), 개시자도 바꾸지 않는다(R7).
        //   일제 통화는 편성 그룹 호(prearranged)의 속성이다 — chat·즉석 세션의 <broadcast-ind> 는 무시한다.
        //   «개시» = 이 그룹에 참가 leg(확립·미확립 초대)이 하나도 없다 — 확립 leg 만 보면 개시자의 200 OK 전에
        //   들어온 두 번째 INVITE 가 개시자를 덮는다.
        //   개시자 leg 은 200 OK 뒤에야 m_mapCallSession 에 오르므로, 그 사이 들어온 INVITE 는 개시 선점(bPending)을
        //   보고 합류로 처리한다 — 빈 그룹에 거의 동시에 온 두 INVITE 가 서로 개시자로 캐시를 덮어 CMP·CSP 판단이
        //   갈리지 않게(TS 24.380 §6.3.5.3.4). 시한을 넘긴 선점은 버려진 것으로 본다.
        bNewSession = !bActiveSession;
        for ( const auto &kv : m_mapCallSession )
            if ( kv.second.strGroupId == pszGroupId && !kv.second.bListenOnly ) {
                bNewSession = false;
                break;
            }
        if ( bNewSession ) {
            auto itSes = m_mapGroupSession.find( pszGroupId );
            if ( itSes != m_mapGroupSession.end() && itSes->second.bPending &&
                 itSes->second.strInitiator != pszCallerInfo &&
                 time( NULL ) - itSes->second.tStart < kPendingSessionSec ) {
                CLog::Print( LOG_INFO,
                             "ProcessGroupCall: Group(%s) Caller(%s) — 개시 진행 중(initiator=%s), 합류로 처리",
                             pszGroupId, pszCallerInfo, itSes->second.strInitiator.c_str() );
                bNewSession = false;
            }
        }
        if ( bNewSession && !bListen ) {
            GroupSession clsSes;
            clsSes.strInitiator = pszCallerInfo;
            clsSes.bBroadcast =
                bBroadcastInd && IsBroadcastCapable( clsGroup );  // 편성 on-demand · ad hoc(§17.2.2.1.1 9))
            clsSes.tStart = time( NULL );
            clsSes.bPending = true;
            bClaimedSession = true;
            if ( bBroadcastInd && !clsSes.bBroadcast )
                CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) type=%s — broadcast-ind 무시(chat·개별 호)",
                             pszGroupId, clsGroup._groupType.c_str() );
            m_mapGroupSession[pszGroupId] = clsSes;
        } else if ( bBroadcastInd ) {
            CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) Caller(%s) broadcast-ind on join — 진행 중 세션 합류",
                         pszGroupId, pszCallerInfo );
        }
        if ( !bActiveSession ) {
            // 새 세션 개시 — 직전 세션의 잔존 조건을 이번 개시 조건으로 리셋
            m_mapGroupCond.erase( pszGroupId );
            if ( iCond > 0 ) SetGroupConditionLocked( pszGroupId, iCond, pszCallerInfo );
        } else if ( iCond > iPrevCond ) {
            // 활성 세션 조인 상향(예: normal 세션에 긴급 조인) — 조건 격상 + 개시자 교체.
            //   기존 확립 멤버 재광고는 발신자 leg 확립 후 수행(아래 PropagateConditionToMembers).
            SetGroupConditionLocked( pszGroupId, iCond, pszCallerInfo );
        } else if ( iCond >= 2 && iPrevCond >= 2 ) {
            // 긴급 진행 중 그룹에 다른 사용자의 새 긴급 표시 — 긴급 사용자 캐시에 더하고 제휴 멤버에 알린다(아래).
            bNewEmergencyUser = m_mapGroupCond[pszGroupId].setEmergencyUsers.insert( pszCallerInfo ).second;
        }
        // else: 활성 세션에 같거나 낮은 조건의 조인 → 세션 조건 유지. 조인은 하향이 아니다 —
        //   하향(해제)은 인가된 사용자의 re-INVITE·MESSAGE 로만(OnInCallConditionRequest·CancelGroupEmergency).
    }
    // 이 호가 참여하는 세션의 유효 조건 — 200 OK 동봉·전파 판단용
    int iCondEff = ( bActiveSession && iCond <= iPrevCond ) ? iPrevCond : iCond;

    CLog::Print( LOG_INFO, "Processing Group Call GroupId(%s) Name(%s) Caller(%s) Priority(%d)", pszGroupId,
                 clsGroup._name.c_str(), pszCallerInfo, clsGroup._priority );

    // 세션 시간 확인: 현재시간이 session_start~session_end 범위 내인지
    time_t tNow = time( NULL );
    if ( clsGroup._sessionStart > 0 && tNow < clsGroup._sessionStart ) {
        CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) session not started yet → 403", pszGroupId );
        // 응답코드를 **실제로 나가는 값**으로 적는다. 이 경로는 `false` 를 돌려주고 호출측
        //   (ModuleDispatcher)이 403 으로 끝내는데, 장부에 0 을 적어 두면 응답코드 축에서
        //   이 실패가 통째로 빠진다 — 코드로 원인을 좁히는 경로에서 안 보인다.
        if ( gclsCallDir.IsEnabled() && !bListen )
            gclsCallDir.PttAttempt( pszGroupId, std::to_string( clsGroup._dbId ), pszCallerInfo, "failed", "denied",
                                    "session_not_started", SIP_FORBIDDEN );
        return false;
    }
    if ( clsGroup._sessionEnd > 0 && tNow > clsGroup._sessionEnd ) {
        CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) session expired → 403", pszGroupId );
        if ( gclsCallDir.IsEnabled() && !bListen )
            gclsCallDir.PttAttempt( pszGroupId, std::to_string( clsGroup._dbId ), pszCallerInfo, "failed", "denied",
                                    "session_expired", SIP_FORBIDDEN );
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
    //   sesid 를 먼저 확보한다 — 녹취 세션 디렉터리 이름의 근거이자, 같은 세션의 두 번째
    //   INVITE 가 같은 디렉터리를 이어 쓰게 하는 키다 (세션 종료 시 RemoveGroupSesId).
    std::string strGroupSesId = GetOrIssueGroupSesId( pszGroupId );
    std::string strSessionDir;
    if ( gclsCallDir.IsEnabled() && !bListen ) {  // 청취 leg 는 세션 이력·녹취 디스크립터에 관여하지 않는다
        strRecordDir = gclsCallDir.GetPttSessionDir( pszGroupId, strGroupSesId, std::to_string( clsGroup._dbId ) );
        // 자기완결 그룹 디스크립터 (계획서 §5) — group.json
        std::string strDescriptor = BuildGroupDescriptor( clsGroup, SessionOf( pszGroupId ).bBroadcast );
        gclsCallDir.PttSessionStart( pszGroupId, pszCallId, pszCallerInfo, strDescriptor );
        strSessionDir = gclsCallDir.GetPttSessionName( pszGroupId );
    }

    if ( iSharedFloorPort <= 0 ) {
        // session_seq 증가 (그룹 세션 시작)
        int iSessionSeq = gclsDbManager.IncrementSessionSeq( pszGroupId );
        clsGroup._sessionSeq = iSessionSeq;
        CLog::Print( LOG_INFO, "GroupCall: session_seq=%d for group %s", iSessionSeq, pszGroupId );
        std::map<std::string, int> mapMemberPorts;
        if ( gclsCmpClient.AddGroup( pszGroupId, clsGroup._pusers, strSharedIp, iSharedFloorPort, mapMemberPorts,
                                     strRecordDir, iSessionSeq, strGroupSesId, clsGroup._groupType,
                                     CmpSessionOf( clsGroup ), clsGroup._floorPolicy, clsGroup._maxTalkers,
                                     clsGroup._floorControl, strSessionDir ) ) {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            // nConfigHash 실제값 (0 이면 다음 SyncGroupsState 오탐 → NOTIFY storm → drop).
            m_mapGroupRtp[pszGroupId] = { iSharedFloorPort, strSharedIp, ComputeGroupConfigHash( clsGroup ), "", 0,
                                          mapMemberPorts };
        }
    } else if ( !strRecordDir.empty() || ( bNewSession && !bListen ) ) {
        // 그룹이 이미 CMP 에 있다 — record_dir 전달이 필요하거나(미녹취 그룹이면 이 record_dir 로 녹취 개시),
        //   남은 컨텍스트 위에 새 세션을 여는 경우(새 sesid — CMP 가 세션 속성을 이 ADD 에서 정한다) addgroup 재호출.
        //   세션 속성은 세션 캐시 값이다 — 합류자를 개시자로 싣지 않는다(CMP 도 같은 세션 재ADD 는 무시한다).
        std::string tmpIp;
        int tmpFPort = 0;
        std::map<std::string, int> tmpMemberPorts;
        gclsCmpClient.AddGroup( pszGroupId, clsGroup._pusers, tmpIp, tmpFPort, tmpMemberPorts, strRecordDir, 0,
                                strGroupSesId, clsGroup._groupType, CmpSessionOf( clsGroup ), clsGroup._floorPolicy,
                                clsGroup._maxTalkers, clsGroup._floorControl, strSessionDir );
    }

    // 개시자 offer 의 floor 협상(fmtp:MCPTT)과 암묵적 발언 요청 판정(TS 24.380 §14.3.5) — 200 OK answer 의 fmtp 와
    //   PTT_JOIN 초기 발언권(granted)이 같은 판정을 쓴다. 진행 중 세션 합류·chat·청취는 받지 않는다(단말은 명시 Floor
    //   Request).
    McpttFmtp clsCallerOffer;
    ParseMcpttFmtp( pclsRtp, clsCallerOffer );
    const bool bImplicitAccepted = AcceptsImplicitFloorRequest( clsCallerOffer, clsGroup, bNewSession, bListen );
    if ( clsCallerOffer.iImplicit > 0 )
        CLog::Print(
            LOG_INFO, "ProcessGroupCall: Group(%s) Caller(%s) implicit floor request %s", pszGroupId, pszCallerInfo,
            bImplicitAccepted ? "accepted" : ( bNewSession ? "not accepted (chat/listen)" : "not accepted (join)" ) );

    // 2. 발신자(Caller)에게 caller 전용 CMP 포트로 200 OK 응답 (leg 별 포트셋)
    //   ⚠ floor 없는 세션(private 멀티, floor_control=off)은 floor_port 가 0 이다 — 종전
    //   `iSharedFloorPort > 0` 게이트는 이 경우 200 OK 응답 블록 전체를 건너뛰어 발신자가
    //   서버 미디어 주소를 받지 못했다(peer 없음 → 무음, 08-04 실측). 멤버 포트 확보 성공을
    //   기준으로 응답한다 (floor 라인은 포트 0 이면 SDP 에서 자연 생략).
    int iCallerLocalAudio = 0;
    if ( GetOrAllocMemberPort( pszGroupId, pszCallerInfo, iCallerLocalAudio ) ) {
        // PTT 발신 Dialog 도 mcptt realm 사용 (200 OK 의 From/To/Contact 도메인)
        {
            std::string strMcpttDomain = gclsServiceMap.GetDomainByKind( "ptt" );
            if ( !strMcpttDomain.empty() ) gclsUserAgent.SetCallDomain( pszCallId, strMcpttDomain.c_str() );
        }
        // 제어 기능의 200 OK (TS 24.379 §6.3.3.2.3.2) — Contact 특성 태그(5)·6)), refresher = uac(2)) + Require:
        // timer(3)).
        //   refresher 는 개시자가 지정하지 않았을 때만 정한다 — 지정했거나 timer 미지원이면 RFC 4028 §9 Table 2 가
        //   정한 값이다(미지원 UAC 는 갱신할 수 없어 uas). 단말이 갱신하고 CSP 는 만료를 감시한다(leg_liveness.md
        //   §5.3).
        gclsUserAgent.SetContactParams( pszCallId, kFocusContactParams );
        gclsUserAgent.SetContactUriParams( pszCallId, ( "gr=" + SessionIdentityToken( pszGroupId ) ).c_str() );
        gclsUserAgent.SetSessionRefresher( pszCallId, E_SESSION_REFRESHER_REMOTE );
        CSipCallRtp clsCallerRtp;
        clsCallerRtp.SetIpPort( strSharedIp.c_str(), iCallerLocalAudio, SOCKET_COUNT_PER_MEDIA );
        // 서비스 코덱 (Setup.Media.Codecs 최우선 — 기본 AMR-WB). answer 의 실 wire PT 는
        // psip AddSdp 가 개시자 오퍼의 rtpmap 에서 echo 한다 (여기 값은 코덱 선택자).
        const CSipCodecEntry &clsSvcCodec = CSipCodecTable::GetTop();
        clsCallerRtp.m_iCodec = clsSvcCodec.m_iPt;
        clsCallerRtp.m_clsCodecList.push_back( clsSvcCodec.m_iPt );
        // MCPTT floor (TS 24.379/24.380): 200 OK 에 m=application(SharedFloorPort) 광고 →
        //   개시자가 floor dest 를 학습해 floor REQUEST 를 올바른 포트로 송신(명시적 GRANT).
        clsCallerRtp.m_iApplicationPort = iSharedFloorPort;
        //   answer fmtp — offer 에 있던 파라미터만, 암묵적 발언 요청을 받아들였으면 mc_implicit_request 를
        //   되돌린다(§14.3.1·§14.3.5)
        clsCallerRtp.m_strApplicationFmtp = AnswerFloorFmtp( clsCallerOffer, bImplicitAccepted );
        // MCPTT 는 음성만이다 — 개시자가 m=video 를 오퍼하면 psip 이 port 0 으로 거절한다(m_iVideoPort 미지정, RFC 3264
        // §6 —
        //   라인 생략은 규격 위반: answer 의 m= 수·순서 = offer). 그룹 영상 = MCVideo 호(mcvideo.md §8).
        if ( bListen ) clsCallerRtp.SetDirection( E_RTP_SEND );  // recvonly offer 의 answer 는 sendonly (RFC 3264 §6.1)
        // 미디어 SRTP answer — offer 의 tag/suite echo + 서버측 키 선언 (media_security.md §5.1)
        if ( !strCallerSrvKey.empty() && pclsRtp ) {
            clsCallerRtp.m_strLocalCryptoTag = pclsRtp->m_strRemoteCryptoTag;
            clsCallerRtp.m_strLocalCryptoSuite = strCallerSuite;
            clsCallerRtp.m_strLocalCryptoKey = strCallerSrvKey;
        }
        // 합류 중 자격 회수 재확인 — 아직 200 OK 전이라 403 으로 끝낼 수 있다.
        if ( bListen && CspAuthz::PolicyGeneration() != uListenAuthzGen ) {
            const std::string strLost = ListenDenyReason( clsGroup, pszCallerInfo );
            if ( !strLost.empty() ) {
                CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) listener(%s) lost scope while joining (%s) → 403",
                             pszGroupId, pszCallerInfo, strLost.c_str() );
                // 여기 오기 전에 `GetOrAllocMemberPort` 가 CMP 에 선할당 PTT_JOIN 을 이미 보냈다 — 거절하고
                //   끝내면 **주인 없는 멤버·포트가 남는다.** 같이 걷는다(§5.10).
                LeaveGroupOrQueue( pszGroupId, pszCallerInfo, strGroupSesId, "합류 중 인가 상실" );
                InvalidateMemberPort( pszGroupId, pszCallerInfo );
                gclsUserAgent.StopCall( pszCallId, SIP_FORBIDDEN );
                EmitPttListenAudit( "denied", pszCallerInfo, strListenGroup, pszGroupId, "", -1 );
                return true;
            }
        }
        // ── 개시자 수락 (§6.3.3.2.3.2) — 새 세션 개시가 개시자 응답 게이트(AckGate)에 걸리면 멤버 응답 뒤에 부른다 ──
        //   값으로 붙잡는다: 미룬 실행은 이 함수가 돌아간 뒤라 인자 포인터(psip 소유)·지역 변수가 없다.
        const std::string strAnsGroup( pszGroupId ), strAnsCaller( pszCallerInfo ), strAnsCallId( pszCallId );
        const bool bHaveOffer = ( pclsRtp != NULL );
        CSipCallRtp clsOfferCopy;
        if ( pclsRtp ) clsOfferCopy = *pclsRtp;
        const bool bClaimedAtAnswer = bClaimedSession;
        auto fnAnswer = [=]( const std::string &strWarning, bool bUnconfirmed ) mutable -> int {
            const char *pszGroupId = strAnsGroup.c_str();
            const char *pszCallerInfo = strAnsCaller.c_str();
            const char *pszCallId = strAnsCallId.c_str();
            CSipCallRtp *pclsRtp = bHaveOffer ? &clsOfferCopy : NULL;
            bool bClaimedSession = bClaimedAtAnswer;
            bool bAccepted;
            {
                CSipMessage *pclsOk = NULL;
                bAccepted = gclsUserAgent.AcceptCall( pszCallId, &clsCallerRtp, &pclsOk );
                if ( bAccepted ) {
                    // 확인 통화 설정의 경고 (§6.3.3.3 — 111 필수 멤버 없이 진행) — §6.3.3.2.3.2 7) 받은 Warning 도
                    // 여기로
                    if ( !strWarning.empty() ) pclsOk->AddHeader( "Warning", strWarning.c_str() );
                    // 멤버 확인 전 수락 — 멤버 쪽 참여 기능이 자동 응답(automatic commencement)으로 대신 받았고
                    // 미디어는
                    //   CMP 가 첫 수신자까지 버퍼링한다(TS 24.379 §10.1.1.4.2 · §6.3.2.2.5.2 · RFC 4964).
                    if ( bUnconfirmed ) pclsOk->AddHeader( "P-Answer-State", "Unconfirmed" );
                    // §6.3.3.2.3.2 4) P-Asserted-Identity = 제어 기능 PSI(그룹 URI — 멤버 leg INVITE 의 PAI 와 같다),
                    //   8) Supported: tdialog (RFC 4538)
                    const std::string strPsi =
                        "<sip:" + std::string( pszGroupId ) + "@" + gclsServiceMap.GetDomainByKind( "ptt" ) + ">";
                    pclsOk->AddHeader( "P-Asserted-Identity", strPsi.c_str() );
                    pclsOk->AddHeader( "Supported", "tdialog" );
                    // 진행 중 조건(긴급/임박) 세션이면 200 OK 에 mcptt-info 로 현재 상태를 동봉 — 조인/재조인
                    //   단말이 개시자의 다음 발언(floor TAKEN)을 기다리지 않고 즉시 세션 긴급 표시를 갖는다
                    //   (TS 24.379, §9-5 멤버 전파). normal 세션은 단일 SDP 200 OK 그대로.
                    if ( iCondEff > 0 ) {
                        std::string strCondActor = pszCallerInfo;
                        {
                            std::unique_lock<std::recursive_mutex> lock( m_mutex );
                            auto ita = m_mapGroupCond.find( pszGroupId );
                            if ( ita != m_mapGroupCond.end() && !ita->second.strInitiator.empty() )
                                strCondActor = ita->second.strInitiator;
                        }
                        WrapInfoMultipart( pclsOk, BuildGroupInfoXml( clsGroup, pszCallerInfo, strCondActor, iCondEff,
                                                                      NULL, SessionOf( pszGroupId ).bBroadcast ) );
                    }
                    bAccepted = gclsUserAgent.m_clsSipStack.SendSipMessage( pclsOk );
                }
            }
            if ( !bAccepted ) {
                CLog::Print( LOG_ERROR, "ProcessGroupCall: AcceptCall failed for Caller(%s)", pszCallerInfo );
                if ( gclsCallDir.IsEnabled() && !bListen )
                    gclsCallDir.PttAttempt( pszGroupId, std::to_string( clsGroup._dbId ), pszCallerInfo, "failed",
                                            "error", "accept_failed", SIP_FORBIDDEN, strGroupSesId );
                return -1;
            }
            // 시도 장부 — **성립**. 개시자 leg 이 실제로 확립된 이 지점이 성립의 정의다(§2.1).
            //   세션 디렉터리 생성(PttSessionStart)만으로 세면 자원 확보·200 OK 실패가 성공으로
            //   잡힌다. sesid 를 함께 남겨 세션 기록과 대조한다(§3).
            //   재조인(같은 발신자가 BYE 없이 새 INVITE)은 아래에서 옛 leg 을 정리하는 경로라
            //   여기까지 오면 새 시도 1건이 맞다.
            if ( gclsCallDir.IsEnabled() && !bListen )
                gclsCallDir.PttAttempt( pszGroupId, std::to_string( clsGroup._dbId ), pszCallerInfo, "established", "",
                                        "", 0, strGroupSesId );
            // 발신자 호출 추적. 같은 (발신자,그룹) 의 옛 레그가 남아 있으면(재조인 — 앱이 BYE 없이
            // 새 INVITE 로 재참여) 고아가 되어 참가자 명단에 중복 표기되고 NOTIFY 가 낭비된다 →
            // 옛 레그를 정리한다. ⚠️CMP LEAVE 는 보내지 않는다 — 멤버 키가 (group, user) 라 같은
            // 사용자의 방금 JOIN 한 멤버십·포트까지 회수되어 미디어가 끊긴다(SIP 다이얼로그만 종료).
            std::string strPrevCallId;
            {
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                auto itPrev = m_mapUserCall.find( { pszCallerInfo, pszGroupId } );
                if ( itPrev != m_mapUserCall.end() && itPrev->second != pszCallId ) {
                    strPrevCallId = itPrev->second;
                    m_mapCallSession.erase( strPrevCallId );
                }
                m_mapUserCall[{ pszCallerInfo, pszGroupId }] = pszCallId;
                CallSessionInfo clsSess;  // 발신자 = 확립
                clsSess.strGroupId = pszGroupId;
                clsSess.strMemberId = pszCallerInfo;
                clsSess.strSessionId = pszCallerInfo;
                clsSess.bEstablished = true;
                clsSess.bListenOnly = bListen;
                clsSess.bListenHidden = bListenHidden;
                clsSess.strListenGroup = strListenGroup;
                clsSess.tListenStart = time( NULL );
                clsSess.bInitiator = true;
                m_mapCallSession[pszCallId] = clsSess;
            }
            if ( bClaimedSession ) {  // 개시자 leg 확립 — 세션 속성 확정 (이제 합류는 m_mapCallSession 으로 판정된다)
                SettlePendingSession( pszGroupId, pszCallerInfo, true );
                bClaimedSession = false;
            }
            // dialog-event(§5.6a): 개시자 leg 확립 — 개시자 회선 감시자에게 confirmed (remote = 세션 URI)
            {
                PttDialogLeg leg;
                leg.strCallId = pszCallId;
                leg.strUser = pszCallerInfo;
                leg.strGroupId = pszGroupId;
                leg.bInitiator = true;
                leg.bListen = bListen;
                EmitPttDialog( leg, "confirmed" );
            }
            if ( !strPrevCallId.empty() ) {
                CLog::Print( LOG_INFO, "ProcessGroupCall: Caller(%s) rejoined Group(%s) — clearing stale leg(%s)",
                             pszCallerInfo, pszGroupId, strPrevCallId.c_str() );
                gclsUserAgent.StopCall( strPrevCallId.c_str() );
                gclsCallMap.Delete( strPrevCallId.c_str(), false );
            }
            CLog::Print( LOG_INFO, "ProcessGroupCall: AcceptCall OK → Caller(%s) MemberPort(%d)", pszCallerInfo,
                         iCallerLocalAudio );

            // 활성 세션 조인이 조건을 상향시켰으면(normal 세션에 긴급 조인) 기존 확립 멤버 leg 에
            //   re-INVITE 재광고 — fan-out INVITE 는 미참여 멤버만 커버한다(참여 중 멤버는
            //   InviteMember 가 조기 반환). (TS 24.379 §6.3.3.1.6 긴급·§6.3.3.1.15 임박 위험, §9-5 멤버 전파)
            //   참여하지 않은 제휴 멤버는 상태 통지 MESSAGE 로 안다(§10.1.1.4.7 6)d)v)·§10.1.1.4.8 1)b)iv) —
            //   §6.3.3.1.11).
            if ( bActiveSession && iCond > iPrevCond ) {
                McpttIndicators clsInd;
                if ( iCond >= 2 ) {
                    clsInd.iEmergency = 1;
                    clsInd.iAlert = HasOutstandingAlert( pszGroupId, pszCallerInfo ) ? 1 : 0;  // §6.3.3.1.6 3)c)
                    if ( iPrevCond == 1 ) clsInd.iImminent = 0;                                // §6.3.3.1.6 3)d)
                } else {
                    clsInd.iImminent = 1;
                }
                PropagateConditionToMembers( pszGroupId, iCond, pszCallerInfo, clsInd );
                McpttIndicators clsNotify;
                if ( iCond >= 2 )
                    clsNotify.iEmergency = 1;
                else
                    clsNotify.iImminent = 1;
                NotifyConditionToAffiliated( pszGroupId, pszCallerInfo, clsNotify, true, pszCallerInfo );
            } else if ( bNewEmergencyUser ) {
                // 다른 사용자의 새 긴급 표시 — 나머지 제휴 멤버 전원에 통지(§10.1.1.4.7 6)c)i) — 참여 여부 무관)
                gclsCmpClient.SetFloorTier( pszGroupId, pszCallerInfo, 2, GetOrIssueGroupSesId( pszGroupId ) );
                McpttIndicators clsNotify;
                clsNotify.iEmergency = 1;
                if ( HasOutstandingAlert( pszGroupId, pszCallerInfo ) ) clsNotify.iAlert = 1;
                NotifyConditionToAffiliated( pszGroupId, pszCallerInfo, clsNotify, false, pszCallerInfo );
            }

            // 개시자(caller)를 CMP floor/RTP 멤버로 등록.
            //   AcceptCall 만으로는 caller 가 CMP _members 에 없어 onRtpPacket 이 caller RTP 를
            //   drop(미릴레이)하고 floor REQUEST 도 미매칭(GRANT 안 됨)이었다. caller 의 INVITE SDP
            //   audio 포트 + 관례(floor=audio+1; cspsim RtpThread 와 동일)로 JoinGroup.
            //   (caller INVITE 에 m=application 이 있으면 GetApplicationPort 우선.)
            if ( pclsRtp ) {
                int iCallerAudio = pclsRtp->GetAudioPort();
                if ( iCallerAudio <= 0 ) iCallerAudio = pclsRtp->m_iPort;
                if ( iCallerAudio > 0 ) {
                    int iCallerFloor = pclsRtp->GetApplicationPort();
                    if ( iCallerFloor <= 0 ) iCallerFloor = iCallerAudio + 1;
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
                        // NAT 판정인데 guard IP 가 비면(UserMap 미조회 — 등록 만료/ID 불일치) CMP 의
                        //   latch IP guard 가 이 leg 에 한해 무력화된다 — 조용한 약화 방지용 경고.
                        if ( iCallerNat && strCallerGuardIp.empty() && clsNatSvc.latch_ip_guard != "off" )
                            CLog::Print( LOG_ERROR,
                                         "ProcessGroupCall: caller leg NAT without sig-guard ip"
                                         " (member=%s sdp=%s) — UserMap miss, latch guard disabled",
                                         pszCallerInfo, pclsRtp->m_strIp.c_str() );
                    }
                    // 개시자 leg PT — 오퍼가 비 96 PT 여도 CMP 가 leg 별 재작성으로 그룹 정합.
                    int iCallerPt = 0, iCallerSrcPt = 0, iCallerTePt = 0, iCallerSrcTePt = 0;
                    std::string strCallerCodec;
                    GetLegPt( pszCallId, false, iCallerPt, iCallerSrcPt, iCallerTePt, iCallerSrcTePt, &strCallerCodec );
                    // 개시자 offer 의 fmtp:MCPTT 협상 결과 (queueing/max_priority) + 초기 발언권 = 암묵적 발언 요청을
                    // 받아들였는가
                    //   (TS 24.380 §14.3.5 — offer 의 mc_granted 는 능력 표시라 요청이 아니다, §12.1.2.2 NOTE 2)
                    McpttFmtp clsCallerFmtp = clsCallerOffer;
                    clsCallerFmtp.iGranted = bImplicitAccepted ? 1 : 0;
                    // 미디어 SRTP 키 (media_security.md §6.3) — rx=개시자 a=crypto, tx=서버 생성
                    CmpMediaCrypto clsCallerCrypto;
                    if ( !strCallerSrvKey.empty() &&
                         !MediaSdes::BuildCmpKeys( strCallerSuite, strCallerUeKey, strCallerSrvKey, clsCallerCrypto ) )
                        CLog::Print( LOG_ERROR,
                                     "ProcessGroupCall: Caller(%s) SRTP key build failed — leg will not decrypt",
                                     pszCallerInfo );
                    // 청취 leg: recv_only=1 (상향 미중계·floor 요청 거절). floor_suppress 는 쓰지 않는다 — 청취자는
                    //   Floor Taken(Permission=0 변형, 단말 U6)으로 현재 발언자를 알아야 하고, 유니캐스트 floor
                    //   메시지는 다른 참가자에게 드러나지 않는다.
                    PurgePendingLeave( pszGroupId, pszCallerInfo );
                    gclsCmpClient.JoinGroup( pszGroupId, pszCallerInfo, pclsRtp->m_strIp, iCallerAudio, iCallerFloor,
                                             GetOrIssueGroupSesId( pszGroupId ), strCallerRole, NULL, iCallerNat,
                                             strCallerGuardIp, iCallerPt, iCallerSrcPt, iCallerTePt, iCallerSrcTePt,
                                             strCallerCodec, clsCallerFmtp,
                                             clsCallerCrypto.bEnabled ? &clsCallerCrypto : NULL, bListen ? 1 : 0, 0 );
                    CLog::Print( LOG_INFO, "ProcessGroupCall: Caller(%s) joined CMP group audio=%d floor=%d role=%s%s",
                                 pszCallerInfo, iCallerAudio, iCallerFloor, strCallerRole.c_str(),
                                 bListen ? ( bListenHidden ? " [listen hidden]" : " [listen visible]" ) : "" );
                    // 긴급/임박 개시: 개시자에 floor tier 부여 → 하위 tier 발언자 선점 (TS 24.380, Phase 1 엔진).
                    if ( iCond > 0 ) {
                        gclsCmpClient.SetFloorTier( pszGroupId, pszCallerInfo, iCond,
                                                    GetOrIssueGroupSesId( pszGroupId ) );
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

            // [CALL LOG] PTT 그룹 세션 기록 — 청취 leg 는 참가자 기록에 남기지 않는다(감사 E-AUD-016 으로만)
            if ( gclsDbManager.IsConnected() && !bListen ) {
                gclsDbManager.InsertCallLog( pszCallId, true, pszGroupId, pszCallerInfo, pszGroupId );
                gclsDbManager.InsertGroupParticipant( pszGroupId, pszCallerInfo );
                gclsDbManager.UpdateParticipantJoined( pszGroupId, pszCallerInfo );
                // 녹취 DB 레코드 삽입
                if ( gclsSetup.m_bRecordEnable && !strRecordDir.empty() ) {
                    gclsDbManager.InsertRecording( pszCallId, "ptt", pszGroupId, pszCallerInfo, pszGroupId,
                                                   strRecordDir, false );
                }
            }

            // RFC 4575: 발신 조인(개시/늦은 재참여)도 참가자 변경 통지 — 콜리 경로(OnCallStarted)만
            // 통지하면, 세션이 이미 있는 그룹에 INVITE 로 늦게 참여한 멤버가 기존 단말 화면에
            // 반영되지 않는다 (음성은 CMP JoinGroup 으로 정상 — 증상=화면 참가자 목록만 미갱신).
            // full 스냅샷이므로 늦은 참여자 본인도 이 NOTIFY 로 현재 로스터를 받는다.
            //   은닉 청취 leg 는 로스터 변경이 아니다(참가자에게 통지 없음 — §5.6 hidden).
            if ( !bListen || !bListenHidden ) SendConferenceNotify( pszGroupId, pszCallerInfo, "connected", "full" );
            if ( bListen ) {
                // 감사 `started` 는 **재검사보다 앞**이다 — 여기까지 왔다면 CMP `JoinGroup` 이 이미 끝나 청취
                //   미디어가 실제로 흘렀다. 재검사가 곧바로 회수하더라도 `OnCallTerminated` 가 `ended` 를 올리므로,
                //   뒤에 두면 **짝 없는 `ended`** 가 남는다(§9 M7 은 시작/종료 한 쌍을 요구한다).
                EmitPttListenAudit( "started", pszCallerInfo, strListenGroup, pszGroupId, strGroupSesId, -1 );
                // **등록 뒤 한 번 더 본다 — 창을 닫는 마지막 조각**(dispatch_center.md §5.10). 세션 맵 등록(위)과
                //   CMP `JoinGroup`(위) 사이에 회수 스윕이 지나갔을 수 있다. 그러면 스윕은 맵에서 지우고 LEAVE 를
                //   보냈는데 이 경로가 이어서 JoinGroup 을 해 **주인 없는 CMP 청취 멤버**가 남는다. 맵에 아직
                //   있는지와(스윕이 지나갔나) 자격이 그대로인지(세대가 올랐으면 재판정)를 확인하고, 아니면
                //   여기서 걷는다 — 스윕이 했을 일과 같다.
                bool bStillRegistered;
                {
                    std::lock_guard<std::recursive_mutex> lock( m_mutex );
                    bStillRegistered = m_mapCallSession.find( pszCallId ) != m_mapCallSession.end();
                }
                std::string strLate;
                if ( CspAuthz::PolicyGeneration() != uListenAuthzGen )
                    strLate = ListenDenyReason( clsGroup, pszCallerInfo );
                if ( !bStillRegistered || !strLate.empty() ) {
                    CLog::Print( LOG_INFO,
                                 "ProcessGroupCall: Group(%s) listener(%s) 회수와 경쟁 — 합류 취소 (registered=%d, %s)",
                                 pszGroupId, pszCallerInfo, (int)bStillRegistered,
                                 strLate.empty() ? "스윕이 먼저 지나감" : strLate.c_str() );
                    LeaveGroupOrQueue( pszGroupId, pszCallerInfo, strGroupSesId, "합류과 회수의 경쟁" );
                    InvalidateMemberPort( pszGroupId, pszCallerInfo );
                    gclsUserAgent.StopCall( pszCallId );
                    OnCallTerminated( pszCallId );  // 맵에 남아 있으면 정리, 이미 지워졌으면 무동작
                    return 1;
                }
                CLog::Print( LOG_INFO, "ProcessGroupCall: Group(%s) listener(%s) role(%s) joined recv_only (%s)",
                             pszGroupId, pszCallerInfo, strListenGroup.c_str(), bListenHidden ? "hidden" : "visible" );
                return 1;  // 청취 합류는 fan-out 을 일으키지 않는다
            }
            return 0;
        };

        // 개시자 응답 게이트 (TS 24.379 §6.3.3.3·§10.1.1.4.2·§11.1.1.4.2) — 새 세션 개시만. 진행 중 세션 합류·청취·chat
        // 은
        //   곧바로 수락한다. 초대 대상은 아래 fan-out 과 같은 규칙(개시자 제외·affiliation 요구 그룹은 affiliate 된
        //   멤버).
        const bool bPrivateCall = clsGroup._groupType == "private";
        std::vector<std::string> vecGateInvite;
        std::set<std::string> setGateRequired;
        if ( bNewSession && !bListen && clsGroup._groupType != "chat" ) {
            for ( const auto &pUser : clsGroup._pusers ) {
                if ( !pUser || pUser->_id == pszCallerInfo ) continue;
                if ( clsGroup._requireAffiliation && gclsDbManager.IsConnected() &&
                     !gclsDbManager.IsAffiliated( pszGroupId, pUser->_id ) )
                    continue;
                vecGateInvite.push_back( pUser->_id );
                if ( pUser->_onNetworkRequired ) setGateRequired.insert( pUser->_id );  // 필수 = affiliated·초대 대상
            }
        }
        int iGateMin = bPrivateCall ? 1 : std::max( 0, clsGroup._minNumberToStart );
        // 멤버 확인 전 수락(최소 0) = 멤버 쪽 참여 기능의 자동 응답(183 Unconfirmed, §6.3.2.2.5.2)을 받아 미디어
        // 버퍼링으로
        //   개시자에게 200 OK(P-Answer-State: Unconfirmed)를 주는 것이다(§10.1.1.4.2). CMP 가 미디어 버퍼링을 하지
        //   않으면 (resource.media_buffer 미광고) 첫 멤버의 200 을 기다린다 — 버퍼 없이 먼저 수락하면 첫 발언이
        //   유실된다.
        const bool bMediaBuffer = gclsCmpClient.SupportsMediaBuffer();
        if ( !bMediaBuffer && iGateMin == 0 && !vecGateInvite.empty() ) iGateMin = 1;
        if ( bNewSession && !bListen && clsGroup._groupType != "chat" &&
             ( !setGateRequired.empty() || iGateMin > 0 ) ) {
            AckGate clsGate;
            clsGate.strCallId = pszCallId;
            clsGate.strInitiator = pszCallerInfo;
            clsGate.setPending.insert( vecGateInvite.begin(), vecGateInvite.end() );
            clsGate.setInvited = clsGate.setPending;
            clsGate.setRequiredPending = setGateRequired;
            clsGate.iMinToStart = iGateMin;
            clsGate.bProceed = clsGroup._ackAction == "proceed";
            clsGate.bPrivate = bPrivateCall;
            clsGate.fnAnswer = fnAnswer;
            if ( !setGateRequired.empty() ) {  // §6.3.3.3 — TNG1 은 초대를 내보내기 전에 켠다
                clsGate.bTng1 = true;
                clsGate.tTng1End =
                    std::chrono::steady_clock::now() + std::chrono::seconds( std::max( 1, clsGroup._ackTimeoutSec ) );
            }
            {
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                m_mapAckGate[pszGroupId] = clsGate;
            }
            bClaimedSession = false;  // 개시 선점은 게이트가 넘겨받는다 — 수락(확정)·중단(삭제) 때 푼다
            CLog::Print( LOG_INFO,
                         "ProcessGroupCall: Group(%s) Caller(%s) — 개시자 응답 대기 invite=%zu required=%zu min=%d "
                         "TNG1=%ds action=%s%s",
                         pszGroupId, pszCallerInfo, vecGateInvite.size(), setGateRequired.size(), iGateMin,
                         setGateRequired.empty() ? 0 : clsGroup._ackTimeoutSec, clsGroup._ackAction.c_str(),
                         bPrivateCall ? " [private]" : "" );
            std::vector<std::string> vecNotInvited;
            for ( const auto &strMember : vecGateInvite )
                if ( !InviteMember( strMember.c_str(), pszGroupId ) ) vecNotInvited.push_back( strMember );
            // 초대하지 못한 멤버(미등록 등)는 최종 거절(480)로 센다 — 필수 멤버면 §6.3.3.3 의 거절 규칙을 탄다.
            //   이미 이 그룹 leg 이 확립돼 있어 새 초대를 내지 않은 멤버는 200 으로 센다(응답이 다시 오지 않는다).
            std::vector<std::string> vecAlready;
            {
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                for ( const auto &strMember : vecGateInvite ) {
                    auto itUC = m_mapUserCall.find( { strMember, pszGroupId } );
                    if ( itUC == m_mapUserCall.end() ) continue;
                    auto itCS = m_mapCallSession.find( itUC->second );
                    if ( itCS != m_mapCallSession.end() && itCS->second.bEstablished )
                        vecAlready.push_back( strMember );
                }
            }
            for ( const auto &strMember : vecAlready ) AckGateMemberResult( pszGroupId, strMember, SIP_OK );
            for ( const auto &strMember : vecNotInvited )
                AckGateMemberResult( pszGroupId, strMember, SIP_TEMPORARILY_UNAVAILABLE );
            AckGateEvaluate( pszGroupId );
            return true;
        }

        // 새 세션 개시인데 게이트가 없다 = 초대할 멤버가 없거나 최소 0 + 미디어 버퍼링 — 후자는 멤버 확인 전 수락이다
        const int iAnswered = fnAnswer( "", !vecGateInvite.empty() );
        if ( iAnswered < 0 ) return false;
        bClaimedSession = false;  // 개시자 leg 확립으로 선점을 확정했다(fnAnswer)
        if ( iAnswered > 0 ) return true;
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

bool CGroupCallService::IsConditionInitAuthorized( const CspPttGroup &clsGroup, const std::string &strUserId,
                                                   std::string &strReason ) {
    // 긴급 사설콜 (TS 24.379 §11): 합성 priv- 그룹은 그룹문서가 없어 capability·Dedicated 축이
    //   공허 — 사용자 축(allow-emergency-private-call + MCPTTPrivateRecipient)이 유일 게이트.
    //   UsePreConfigured 모드는 사전 지정 수신자와 착신자 일치까지 판정한다(그룹 긴급콜의
    //   DedicatedGroup 대상 일치와 대칭).
    if ( clsGroup._groupType == "private" ) {
        CspUserProfile clsProf;
        if ( gclsDbManager.SelectUserProfile( strUserId, clsProf ) < 0 ) return true;  // DB 불가 — fail-open
        if ( !clsProf.m_bAllowEmergencyPrivateCall ) {
            strReason = "user not authorised (private)";
            return false;
        }
        if ( clsProf.m_strPrivateEmergencyMode == "UsePreConfigured" ) {
            if ( clsProf.m_strEmergencyPrivateRecipient.empty() ) {
                strReason = "preconfigured recipient not provisioned";
                return false;
            }
            std::string strPeer;
            for ( const auto &p : clsGroup._pusers )
                if ( p && p->_id != strUserId ) {
                    strPeer = p->_id;
                    break;
                }
            if ( clsProf.m_strEmergencyPrivateRecipient != strPeer ) {
                strReason = "preconfigured-recipient mismatch";
                return false;
            }
        }
        return true;
    }
    if ( !clsGroup._emergencyCall ) {
        strReason = "group capability";
        return false;
    }
    CspUserProfile clsProf;
    int iRes = gclsDbManager.SelectUserProfile( strUserId, clsProf );
    if ( iRes < 0 ) return true;  // DB 불가/마이그레이션 전 — fail-open (그룹 capability 만 판정)
    if ( !clsProf.m_bAllowEmergencyCall ) {
        strReason = "user not authorised";
        return false;
    }
    if ( clsProf.m_strEmergencyGroupMode == "DedicatedGroup" && clsProf.m_strEmergencyGroupId != clsGroup._id ) {
        strReason =
            clsProf.m_strEmergencyGroupId.empty() ? "dedicated group not provisioned" : "dedicated-group mismatch";
        return false;
    }
    return true;
}

// ── 진행 중 조건(긴급·임박 위험) — 인가·전이·재광고·통지 (TS 24.379 §6.3.3.1.6·§6.3.3.1.10·§6.3.3.1.11·
//    §6.3.3.1.13·§6.3.3.1.15·§6.3.3.1.16·§10.1.1.4.7·§10.1.1.4.8) ──────────────────────────────────────────────

void CGroupCallService::SetGroupConditionLocked( const std::string &strGroupId, int iCond,
                                                 const std::string &strUser ) {
    GroupCondition &c = m_mapGroupCond[strGroupId];
    const bool bEmergencyStart = ( iCond >= 2 && c.iCond < 2 );
    c.iCond = iCond;
    c.strInitiator = strUser;
    if ( iCond >= 2 ) {
        c.setEmergencyUsers.insert( strUser );               // §10.1.1.4.7 6)a) — 긴급 개시 사용자 캐시
        if ( bEmergencyStart ) c.tTng2Start = time( NULL );  // TNG2 시작 (§10.1.1.4.7 6)d)ii))
    } else {
        c.setEmergencyUsers.clear();
        c.tTng2Start = 0;
    }
}

int CGroupCallService::GroupConditionOf( const std::string &strGroupId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    auto it = m_mapGroupCond.find( strGroupId );
    return it != m_mapGroupCond.end() ? it->second.iCond : 0;
}

bool CGroupCallService::HasGroupCallInProgress( const std::string &strGroupId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    return HasActiveLeg( strGroupId );
}

bool CGroupCallService::IsEmergencyCancelAuthorized( const std::string &strGroupId, const std::string &strUserId,
                                                     std::string &strReason ) {
    std::string strInitiator;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto it = m_mapGroupCond.find( strGroupId );
        if ( it != m_mapGroupCond.end() ) strInitiator = it->second.strInitiator;
    }
    if ( !strInitiator.empty() && strInitiator == strUserId ) return true;  // 개시자 (§6.3.3.1.13.4 예시)
    // 사설 호(§11)의 해제 인가는 <allow-cancel-private-emergency-call>(§6.3.3.1.13.4 둘째 문단 — 규격도 FFS)인데
    //   CIMS 는 그 요소를 제공하지 않는다 — 개시자만.
    if ( strGroupId.rfind( "priv-", 0 ) == 0 ) {
        strReason = "private call — initiator only";
        return false;
    }
    CspUserProfile clsProf;  // DB 불가·행 없음 = 기본값(false) — 해제는 fail-closed(긴급을 지킨다)
    gclsDbManager.SelectUserProfile( strUserId, clsProf );
    if ( clsProf.m_bAllowCancelGroupEmergency ) return true;  // TS 24.484 allow-cancel-group-emergency (관제사 등)
    strReason = "not initiator and allow-cancel-group-emergency=false";
    return false;
}

bool CGroupCallService::IsImminentCancelAuthorized( const std::string &strUserId ) {
    CspUserProfile clsProf;
    gclsDbManager.SelectUserProfile( strUserId, clsProf );
    return clsProf.m_bAllowCancelImminentPeril;
}

std::string CGroupCallService::EmergencyTalkerOtherThan( const std::string &strGroupId, const std::string &strUserId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    auto itc = m_mapGroupCond.find( strGroupId );
    auto itt = m_mapGroupTalkers.find( strGroupId );
    if ( itc == m_mapGroupCond.end() || itt == m_mapGroupTalkers.end() ) return std::string();
    for ( const auto &strTalker : itt->second )
        if ( strTalker != strUserId && itc->second.setEmergencyUsers.count( strTalker ) ) return strTalker;
    return std::string();
}

void CGroupCallService::OnFloorTalkers( const std::string &strGroupId, const std::vector<std::string> &vecTalkers ) {
    if ( strGroupId.empty() ) return;
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    if ( vecTalkers.empty() )
        m_mapGroupTalkers.erase( strGroupId );
    else
        m_mapGroupTalkers[strGroupId] = std::set<std::string>( vecTalkers.begin(), vecTalkers.end() );
}

void CGroupCallService::SetAlertOutstanding( const std::string &strGroupId, const std::string &strUserId, bool bOn ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    if ( bOn ) {
        m_mapGroupAlerts[strGroupId].insert( strUserId );
        return;
    }
    auto it = m_mapGroupAlerts.find( strGroupId );
    if ( it == m_mapGroupAlerts.end() ) return;
    it->second.erase( strUserId );
    if ( it->second.empty() ) m_mapGroupAlerts.erase( it );
}

bool CGroupCallService::HasOutstandingAlert( const std::string &strGroupId, const std::string &strUserId ) {
    std::unique_lock<std::recursive_mutex> lock( m_mutex );
    auto it = m_mapGroupAlerts.find( strGroupId );
    return it != m_mapGroupAlerts.end() && it->second.count( strUserId ) > 0;
}

bool CGroupCallService::IsAlertActivateAuthorized( const CspPttGroup &clsGroup, const std::string &strUserId ) {
    if ( !clsGroup._emergencyAlert ) return false;  // TS 24.481 allow-MCPTT-emergency-alert
    CspUserProfile clsProf;
    // DB 불가(-1)면 그룹 축만(개시 인가 IsConditionInitAuthorized 와 같은 fail-open)
    if ( gclsDbManager.SelectUserProfile( strUserId, clsProf ) < 0 ) return true;
    return clsProf.m_bAllowEmergencyAlert;  // TS 24.484 allow-activate-emergency-alert
}

bool CGroupCallService::IsAlertCancelAuthorized( const std::string &strUserId ) {
    CspUserProfile clsProf;
    gclsDbManager.SelectUserProfile( strUserId, clsProf );
    return clsProf.m_bAllowCancelEmergencyAlert;  // TS 24.484 allow-cancel-emergency-alert
}

int CGroupCallService::NotifyConditionToAffiliated( const std::string &strGroupId, const std::string &strCallingUser,
                                                    const McpttIndicators &clsInd, bool bNonParticipantsOnly,
                                                    const std::string &strExclude ) {
    CspPttGroup clsGroup;
    if ( !gclsGroupMap.Select( strGroupId.c_str(), clsGroup ) ) return 0;
    std::set<std::string> setParticipants;
    if ( bNonParticipantsOnly ) {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        for ( const auto &kv : m_mapCallSession )
            if ( kv.second.strGroupId == strGroupId ) setParticipants.insert( kv.second.strMemberId );
    }
    const std::string strDomain = gclsServiceMap.GetDomainByKind( "ptt" );
    // §6.3.3.1.11 2)·3) Accept-Contact(g.3gpp.mcptt·ICSI mcptt), 5) P-Asserted-Identity = 제어 기능 PSI(그룹 URI — 멤버
    //   leg INVITE 의 PAI 와 같다), 6) P-Asserted-Service(RFC 6050 헤더 이름)
    const std::vector<std::pair<std::string, std::string>> vecHeaders = {
        { "Accept-Contact", "*;+g.3gpp.mcptt;require;explicit" },
        { "Accept-Contact", "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";require;explicit" },
        { "P-Asserted-Identity", "<sip:" + strGroupId + "@" + strDomain + ">" },
        { "P-Asserted-Service", "urn:urn-7:3gpp-service.ims.icsi.mcptt" } };
    int iSent = 0;
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( !pUser || pUser->_id == strExclude || setParticipants.count( pUser->_id ) ) continue;
        if ( clsGroup._requireAffiliation && gclsDbManager.IsConnected() &&
             !gclsDbManager.IsAffiliated( strGroupId, pUser->_id ) )
            continue;  // 제휴 멤버만
        CUserInfo clsMemInfo;
        if ( !gclsUserMap.Select( pUser->_id.c_str(), clsMemInfo ) ) continue;  // 등록(온라인) 멤버만
        CSipCallRoute clsRoute;
        clsMemInfo.GetCallRoute( clsRoute );
        std::string strParams = McpttInfoUri( "mcptt-request-uri", "tel:" + pUser->_id );  // 7) 대상 MCPTT ID
        if ( !strCallingUser.empty() ) strParams += McpttInfoUri( "mcptt-calling-user-id", "tel:" + strCallingUser );
        strParams += McpttInfoUri( "mcptt-calling-group-id", "tel:" + strGroupId );  // 8)
        strParams += McpttIndicatorElems( clsInd ) + McpttIndicatorOriginatedBy( clsInd );
        const std::string strBody = McpttInfoDocument( strParams );
        if ( gclsUserAgent.SendSms( strGroupId.c_str(), pUser->_id.c_str(), strBody.c_str(), &clsRoute,
                                    "application/vnd.3gpp.mcptt-info+xml", &vecHeaders ) )
            iSent++;
    }
    CLog::Print( LOG_INFO, "NotifyConditionToAffiliated: group(%s) calling(%s) E=%d A=%d I=%d %s → %d MESSAGE",
                 strGroupId.c_str(), strCallingUser.c_str(), clsInd.iEmergency, clsInd.iAlert, clsInd.iImminent,
                 bNonParticipantsOnly ? "non-participants" : "affiliated", iSent );
    return iSent;
}

bool CGroupCallService::CancelGroupEmergency( const std::string &strGroupId, const std::string &strCanceller,
                                              const McpttIndicators &clsAlert, const std::string &strExclude,
                                              const char *pszBy ) {
    std::set<std::string> setUsers;
    std::string strInitiator;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto it = m_mapGroupCond.find( strGroupId );
        if ( it == m_mapGroupCond.end() || it->second.iCond < 2 ) return false;
        setUsers = it->second.setEmergencyUsers;
        strInitiator = it->second.strInitiator;
        m_mapGroupCond.erase( it );  // §10.1.1.4.7 8)a)·b) — 상태 false, 긴급 사용자 캐시 비움, TNG2 정지(8)e))
        // TNG3 재시작 (§6.3.3.5.2 — 긴급이 끝나면 on-network-maximum-duration 을 새로 센다)
        auto itSes = m_mapGroupSession.find( strGroupId );
        if ( itSes != m_mapGroupSession.end() ) itSes->second.tStart = time( NULL );
    }
    const std::string strSesId = GetOrIssueGroupSesId( strGroupId );
    for ( const auto &strUser : setUsers ) gclsCmpClient.SetFloorTier( strGroupId, strUser, 0, strSesId );
    const std::string strActor = strCanceller.empty() ? strInitiator : strCanceller;
    if ( gclsCallDir.IsEnabled() )
        gclsCallDir.PttLogEvent(
            strGroupId, "emergency_cancelled",
            std::string( "{\"actor\":\"" ) + CCallDir::JsonEsc( strActor ) + "\",\"by\":\"" + pszBy + "\"}" );
    EmitEmergencyModeEvent( "cancelled", 2, strGroupId, strActor, strSesId );
    CLog::Print( LOG_INFO, "CancelGroupEmergency: group(%s) by(%s) via %s — users=%zu", strGroupId.c_str(),
                 strActor.c_str(), pszBy, setUsers.size() );

    // 참여 leg 재광고 — §6.3.3.1.6 4)(요청이 있을 때)·§6.3.3.1.10(TNG2): emergency-ind false, 요청이 인가된 경보 취소를
    //   실었으면 alert-ind false + originated-by(§6.3.3.1.6 4)b)ii)).
    McpttIndicators clsInd = clsAlert;
    clsInd.iEmergency = 0;
    clsInd.iImminent = -1;
    PropagateConditionToMembers( strGroupId, 0, strExclude, clsInd );
    // 참여하지 않은 제휴 멤버 — 상태 통지 MESSAGE (§10.1.1.4.7 8)f)·§6.3.3.1.16 3)). mcptt-calling-user-id = 해제한
    // 사용자.
    NotifyConditionToAffiliated( strGroupId, strCanceller, clsInd, true, strExclude );
    return true;
}

CGroupCallService::InCallConditionVerdict CGroupCallService::OnInCallConditionRequest( const std::string &strCallId,
                                                                                       const std::string &strGroupId,
                                                                                       const std::string &strMemberId,
                                                                                       const CMcpttInfo &clsMi ) {
    InCallConditionVerdict v = EvaluateInCallCondition( strGroupId, strMemberId, clsMi );
    if ( v.iStatus == 0 && !v.strWarning.empty() ) {
        // 받아들이는 re-INVITE 의 200 OK(스택이 만든다)에 Warning 149 를 싣고, ACK 뒤 INFO 를 보낸다(§6.3.3.1.18)
        gclsUserAgent.AddReInviteAnswerHeader( strCallId.c_str(), "Warning", v.strWarning.c_str() );
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        m_mapPendingInfo[strCallId] = v.strInfoBody;
    }
    return v;
}

CGroupCallService::InCallConditionVerdict CGroupCallService::EvaluateInCallCondition( const std::string &strGroupId,
                                                                                      const std::string &strMemberId,
                                                                                      const CMcpttInfo &clsMi ) {
    InCallConditionVerdict v;
    const bool bEmgTrue = clsMi.bHasEmergencyInd && clsMi.bEmergency;
    const bool bEmgFalse = clsMi.bHasEmergencyInd && !clsMi.bEmergency;
    const bool bImmTrue = clsMi.bHasImminentInd && clsMi.bImminent;
    const bool bImmFalse = clsMi.bHasImminentInd && !clsMi.bImminent;
    const bool bAlertTrue = clsMi.bHasAlertInd && clsMi.bAlert;
    const bool bAlertFalse = clsMi.bHasAlertInd && !clsMi.bAlert;
    if ( !bEmgTrue && !bEmgFalse && !bImmTrue && !bImmFalse && !bAlertTrue && !bAlertFalse )
        return v;  // 조건 요청 아님

    CspPttGroup clsGroup;
    if ( !gclsGroupMap.Select( strGroupId.c_str(), clsGroup ) ) return v;
    const int iCur = GroupConditionOf( strGroupId );
    const std::string strSesId = GetOrIssueGroupSesId( strGroupId );
    // 경보 취소의 대상 = <originated-by>(제3자 취소) 또는 요청자 (§10.1.1.4.7 8)c)i)·ii))
    const std::string strAlertOwner =
        clsMi.strOriginatedBy.empty() ? strMemberId : McpttBareId( clsMi.strOriginatedBy );
    // Warning 149 + INFO (§10.1.1.4.7 200 OK 5)·6)·7) · §6.3.3.1.18 3)) — 호는 받아들이고 받아들이지 않은 부분을 알린다
    auto infoPending = [&]( const McpttIndicators &clsInfo ) {
        v.strWarning = McpttWarning( 149, "SIP INFO request pending", gclsServiceMap.GetDomainByKind( "ptt" ) );
        v.strInfoBody = McpttInfoDocument( McpttIndicatorElems( clsInfo ) );
    };
    auto reject = [&]( const McpttIndicators &clsBody, const char *pszWhy ) {
        v.iStatus = SIP_FORBIDDEN;
        v.strBody = McpttInfoDocument( McpttIndicatorElems( clsBody ) );
        CLog::Print( LOG_INFO, "InCallCondition: group(%s) member(%s) → 403 (%s)", strGroupId.c_str(),
                     strMemberId.c_str(), pszWhy );
    };

    // ── 긴급 상향 (§10.1.1.4.7 3)·6)) ──
    if ( bEmgTrue ) {
        std::string strReason;
        if ( !IsConditionInitAuthorized( clsGroup, strMemberId, strReason ) ) {
            McpttIndicators b;  // §6.3.3.1.14 — emergency-ind false · alert-ind false
            b.iEmergency = 0;
            b.iAlert = 0;
            reject( b, ( "emergency not authorised: " + strReason ).c_str() );
            return v;
        }
        bool bAlertOk = false;
        if ( bAlertTrue ) {
            bAlertOk = IsAlertActivateAuthorized( clsGroup, strMemberId );
            if ( bAlertOk ) {
                SetAlertOutstanding( strGroupId, strMemberId, true );  // 6)b)
            } else {
                McpttIndicators i;  // §6.3.3.1.18 3)a)
                i.iEmergency = 1;
                i.iAlert = 0;
                infoPending( i );
            }
        }
        if ( iCur >= 2 ) {
            // 6)c) 다른 사용자의 새 긴급 표시 — 캐시에 더하고 나머지 제휴 멤버에 통지(참여 여부 무관)
            bool bNew;
            {
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                bNew = m_mapGroupCond[strGroupId].setEmergencyUsers.insert( strMemberId ).second;
            }
            if ( bNew || bAlertOk ) {
                gclsCmpClient.SetFloorTier( strGroupId, strMemberId, 2, strSesId );
                McpttIndicators n;
                n.iEmergency = 1;
                if ( bAlertOk ) n.iAlert = 1;
                NotifyConditionToAffiliated( strGroupId, strMemberId, n, false, strMemberId );
            }
            return v;
        }
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            SetGroupConditionLocked( strGroupId, 2, strMemberId );  // 6)a)·d)i)·ii)
        }
        gclsCmpClient.SetFloorTier( strGroupId, strMemberId, 2, strSesId );
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.PttLogEvent( strGroupId, "emergency_activated",
                                     std::string( "{\"actor\":\"" ) + strMemberId + "\",\"by\":\"reinvite\"}" );
        EmitEmergencyModeEvent( "activated", 2, strGroupId, strMemberId, strSesId );
        CLog::Print( LOG_INFO, "InCallCondition: emergency_activated group(%s) by(%s)", strGroupId.c_str(),
                     strMemberId.c_str() );
        McpttIndicators r;  // §6.3.3.1.6 3)
        r.iEmergency = 1;
        r.iAlert = bAlertOk ? 1 : 0;
        if ( iCur == 1 ) r.iImminent = 0;
        PropagateConditionToMembers( strGroupId, 2, strMemberId, r );  // 6)d)iii)·iv)
        McpttIndicators n;                                             // 6)d)v)
        n.iEmergency = 1;
        if ( bAlertOk ) n.iAlert = 1;
        NotifyConditionToAffiliated( strGroupId, strMemberId, n, true, strMemberId );
        return v;
    }

    // ── 임박 위험 상향 (§10.1.1.4.7 4)·9) → §10.1.1.4.8 1)) ──
    if ( bImmTrue ) {
        if ( iCur >= 2 ) {
            // 200 OK 7) — 긴급이 이미 진행 중: 임박 요청은 받아들이지 않고 긴급 수준으로 받는다(NOTE 5)
            McpttIndicators i;  // §6.3.3.1.18 3)c)
            i.iEmergency = 1;
            i.iImminent = 0;
            infoPending( i );
            return v;
        }
        std::string strReason;
        if ( !IsConditionInitAuthorized( clsGroup, strMemberId, strReason ) ) {
            McpttIndicators b;  // 4)a) imminentperil-ind false
            b.iImminent = 0;
            reject( b, ( "imminent peril not authorised: " + strReason ).c_str() );
            return v;
        }
        if ( iCur == 1 ) {
            // §10.1.1.4.8 1)a) 다른 사용자의 새 임박 표시 — 나머지 제휴 멤버에 통지
            McpttIndicators n;
            n.iImminent = 1;
            NotifyConditionToAffiliated( strGroupId, strMemberId, n, false, strMemberId );
            return v;
        }
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            SetGroupConditionLocked( strGroupId, 1, strMemberId );
        }
        gclsCmpClient.SetFloorTier( strGroupId, strMemberId, 1, strSesId );
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.PttLogEvent( strGroupId, "imminent_activated",
                                     std::string( "{\"actor\":\"" ) + strMemberId + "\",\"by\":\"reinvite\"}" );
        EmitEmergencyModeEvent( "activated", 1, strGroupId, strMemberId, strSesId );
        CLog::Print( LOG_INFO, "InCallCondition: imminent_activated group(%s) by(%s)", strGroupId.c_str(),
                     strMemberId.c_str() );
        McpttIndicators r;  // §6.3.3.1.15
        r.iImminent = 1;
        PropagateConditionToMembers( strGroupId, 1, strMemberId, r );                  // 1)b)ii)·iii)
        NotifyConditionToAffiliated( strGroupId, strMemberId, r, true, strMemberId );  // 1)b)iv)
        return v;
    }

    // 경보 취소 인가 — 긴급 해제에 실린 것이든 단독이든 (§6.3.3.1.13.3)
    const bool bAlertCancelOk = bAlertFalse && IsAlertCancelAuthorized( strMemberId );

    // ── 긴급 해제 (§10.1.1.4.7 7)·7a)·8)) ──
    if ( bEmgFalse && iCur >= 2 ) {
        std::string strReason;
        const bool bAuth = IsEmergencyCancelAuthorized( strGroupId, strMemberId, strReason );
        const std::string strTalker = bAuth ? EmergencyTalkerOtherThan( strGroupId, strMemberId ) : std::string();
        if ( !bAuth || !strTalker.empty() ) {
            McpttIndicators b;  // 7)b)·7a)b) emergency-ind true, 7)c) 경보가 남아 있으면 alert-ind true
            b.iEmergency = 1;
            if ( bAlertFalse && HasOutstandingAlert( strGroupId, strAlertOwner ) ) b.iAlert = 1;
            reject( b, bAuth ? ( "emergency user transmitting: " + strTalker ).c_str()
                             : ( "cancel not authorised: " + strReason ).c_str() );
            return v;
        }
        McpttIndicators clsAlert;
        if ( bAlertFalse ) {
            if ( bAlertCancelOk ) {
                // 8)c) — 경보 캐시 정리, 재광고·통지에 alert-ind false (+ originated-by)
                SetAlertOutstanding( strGroupId, strAlertOwner, false );
                clsAlert.iAlert = 0;
                clsAlert.strOriginatedBy = clsMi.strOriginatedBy;
            } else {
                McpttIndicators i;  // 200 OK 6) · §6.3.3.1.18 3)b)
                i.iAlert = 1;
                infoPending( i );
            }
        }
        CancelGroupEmergency( strGroupId, strMemberId, clsAlert, strMemberId, "reinvite" );  // 8)a)~f)
        return v;
    }

    // ── 임박 위험 해제 (§10.1.1.4.8 2)·3)) ──
    if ( bImmFalse && iCur == 1 ) {
        if ( !IsImminentCancelAuthorized( strMemberId ) ) {
            // 2)b) 원문은 imminentperil-ind "false" 이나, 단말 절차(§10.1.2.1.5 — 4xx 에 imminentperil-ind true 또는
            //   요소 없음 = 상태 유지)와 긴급 해제 거절(7)b) true)에 맞춰 현재 상태 true 를 싣는다 (편차 표).
            McpttIndicators b;
            b.iImminent = 1;
            reject( b, "imminent peril cancel not authorised (allow-cancel-imminent-peril=false)" );
            return v;
        }
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            m_mapGroupCond.erase( strGroupId );  // 3)a)·b)
        }
        gclsCmpClient.SetFloorTier( strGroupId, strMemberId, 0, strSesId );
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.PttLogEvent( strGroupId, "imminent_cancelled",
                                     std::string( "{\"actor\":\"" ) + strMemberId + "\",\"by\":\"reinvite\"}" );
        EmitEmergencyModeEvent( "cancelled", 1, strGroupId, strMemberId, strSesId );
        CLog::Print( LOG_INFO, "InCallCondition: imminent_cancelled group(%s) by(%s)", strGroupId.c_str(),
                     strMemberId.c_str() );
        McpttIndicators r;  // §6.3.3.1.15 — imminentperil-ind false
        r.iImminent = 0;
        PropagateConditionToMembers( strGroupId, 0, strMemberId, r );                  // 3)c)
        NotifyConditionToAffiliated( strGroupId, strMemberId, r, true, strMemberId );  // 3)d)
        return v;
    }

    // ── 긴급 요소 없이 경보 취소만 실린 re-INVITE (200 OK 6)) ──
    if ( bAlertFalse ) {
        if ( bAlertCancelOk ) {
            SetAlertOutstanding( strGroupId, strAlertOwner, false );
        } else {
            McpttIndicators i;
            i.iAlert = 1;
            infoPending( i );
        }
    }
    // 경보 발령만 실린 re-INVITE — 미인가면 200 OK 5) Warning 149, 인가면 캐시(§6.3.3.1.6 3)c) 의 전제)
    if ( bAlertTrue ) {
        if ( IsAlertActivateAuthorized( clsGroup, strMemberId ) ) {
            SetAlertOutstanding( strGroupId, strMemberId, true );
        } else {
            McpttIndicators i;  // §6.3.3.1.18 3)a)
            i.iEmergency = iCur >= 2 ? 1 : 0;
            i.iAlert = 0;
            infoPending( i );
        }
    }
    return v;
}

void CGroupCallService::OnInDialogAck( const std::string &strCallId ) {
    std::string strBody;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto it = m_mapPendingInfo.find( strCallId );
        if ( it == m_mapPendingInfo.end() ) return;
        strBody = it->second;
        m_mapPendingInfo.erase( it );
    }
    // §6.3.3.1.18 — Info-Package g.3gpp.mcptt-info (RFC 6086), 같은 다이얼로그
    const bool bSent = gclsUserAgent.SendInfoWithBody( strCallId.c_str(), "g.3gpp.mcptt-info", "application",
                                                       "vnd.3gpp.mcptt-info+xml", strBody );
    CLog::Print( LOG_INFO, "InCallCondition: Warning 149 뒤 INFO %s [callId=%s]", bSent ? "sent" : "FAILED",
                 strCallId.c_str() );
}

void CGroupCallService::ClearUserCall( const std::string &strUserId ) {
    // 멀티그룹: 사용자의 모든 그룹 콜을 정리한다 (그룹별 독립 다이얼로그).
    struct ClearItem {
        std::string strCallId;
        std::string strGroupId;
        std::string strSessionId;
        bool bStillActive = false;
        PttDialogLeg clsDlg;  ///< dialog-event terminated 통지용(§5.6a)
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
                clsItem.clsDlg = _pttLegOf( clsItem.strCallId, itSess->second );
                m_mapCallSession.erase( itSess );
            }
            it = m_mapUserCall.erase( it );
            vecItems.push_back( clsItem );
        }
        if ( vecItems.empty() ) return;

        // 그룹별로 아직 다른 멤버가 남아있는지 확인 (확립 leg 만 — pending 초대는 세션을 못 붙듦)
        for ( auto &clsItem : vecItems ) {
            if ( clsItem.strGroupId.empty() ) continue;
            for ( const auto &kv : m_mapCallSession ) {
                if ( kv.second.strGroupId == clsItem.strGroupId && kv.second.bEstablished ) {
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
        EmitPttDialog( clsItem.clsDlg, "terminated" );  // dialog-event(§5.6a) — 맵에서 이미 뺐으므로 여기서 낸다
        // 기존 SIP 다이얼로그 정상 종료(BYE) — 고아 다이얼로그 누수 방지 (1E)
        if ( !clsItem.strCallId.empty() ) {
            gclsUserAgent.StopCall( clsItem.strCallId.c_str() );
            gclsCallMap.Delete( clsItem.strCallId.c_str(), false );
        }
        // lock 해제 후 CMP/DB 호출
        const std::string &strGroupId = clsItem.strGroupId;
        if ( strGroupId.empty() ) continue;
        LeaveGroupOrQueue( strGroupId, clsItem.strSessionId, GetOrIssueGroupSesId( strGroupId ), "세션 정리" );
        InvalidateMemberPort( strGroupId, clsItem.strSessionId );

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

        // RFC 4575: 이탈 통지 — OnCallTerminated(BYE) 와 동일 계약. de-register/로그아웃/
        //   force-stop 으로 들어오는 이 경로도 conference 구독자에게 알려야 남은 단말의
        //   접속 명단에서 이 사용자가 사라진다 (teardown 앞에서 호출 — 버전 단조성).
        SendConferenceNotify( strGroupId, strUserId, "disconnected", "deleted" );

        // on-demand 그룹 호(편성·ad hoc): 잔여 leg 1개 → 세션 해제 — OnCallTerminated(BYE) 경로와 동일 계약
        //   (TS 24.379 §6.3.8.1 참가자 1명 이하. pending 초대가 있으면 count>1 로 유지).
        if ( clsItem.bStillActive ) {
            CspPttGroup clsAdhocChk;
            if ( gclsGroupMap.Select( strGroupId.c_str(), clsAdhocChk ) &&
                 ( ( clsAdhocChk._isAdhoc && clsAdhocChk._groupType != "private" ) ||
                   IsOnDemandGroupCall( clsAdhocChk ) ) ) {
                std::vector<std::string> vecRemainLegs;
                bool bGateOpen;  // 개시자 응답 대기 중 — 개시자가 아직 맵에 없어 참가자 수를 세지 않는다
                {
                    std::unique_lock<std::recursive_mutex> lock( m_mutex );
                    bGateOpen = m_mapAckGate.count( strGroupId ) > 0;
                    for ( const auto &kv : m_mapCallSession )
                        if ( kv.second.strGroupId == strGroupId && !kv.second.bListenOnly )
                            vecRemainLegs.push_back( kv.first );
                }
                if ( !bGateOpen && vecRemainLegs.size() == 1 ) {
                    CLog::Print( LOG_INFO, "GroupCall: group(%s) — 잔여 1 leg 종료(BYE, min-participants)",
                                 strGroupId.c_str() );
                    gclsUserAgent.StopCall( vecRemainLegs[0].c_str() );
                    OnCallTerminated( vecRemainLegs[0] );
                }
            }
        }

        // on-demand 그룹(편성·ad hoc): 마지막 멤버 이탈 시 세션 즉시 해제 (chat 은 상시 유지).
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
                    // 사설콜(priv-)은 제외 — 정의(E-AUD-010)는 ad-hoc/regroup 이다. _isAdhoc 은
                    //   ephemeral 수명 표시일 뿐 "ad-hoc 그룹"이 아니다.
                    if ( clsGrp._groupType != "private" ) EmitRegroupEvent( "released", strGroupId, "ad-hoc" );
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
            LeaveGroupOrQueue( strStaleGroup, strStaleSession, GetOrIssueGroupSesId( strStaleGroup ), "고아 leg" );
            InvalidateMemberPort( strStaleGroup, strStaleSession );
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
    bool bGroupHasActiveCall = false;
    for ( const auto &kv : m_mapCallSession ) {
        if ( kv.second.strGroupId == pszGroupId && !kv.second.bListenOnly ) {
            bGroupHasActiveCall = true;
            break;
        }
    }
    bool bInCache = ( m_mapGroupRtp.find( pszGroupId ) != m_mapGroupRtp.end() );
    if ( bInCache && bGroupHasActiveCall ) {
        iSharedFloorPortIM = m_mapGroupRtp[pszGroupId].iFloorPort;
        strSharedIp = m_mapGroupRtp[pszGroupId].strIp;
    } else {
        if ( bInCache )
            CLog::Print( LOG_INFO,
                         "InviteMember(%s): Group(%s) session (re)start — refreshing CMP group (stale-cache guard)",
                         pszUserId, pszGroupId );
        // Try to allocate now ((재)확보)
        CspPttGroup clsGroup;
        if ( gclsGroupMap.Select( pszGroupId, clsGroup ) ) {
            std::string strGroupSesId = GetOrIssueGroupSesId( pszGroupId );
            std::string strRecordDir, strSessionDir;
            if ( gclsCallDir.IsEnabled() ) {
                strRecordDir =
                    gclsCallDir.GetPttSessionDir( pszGroupId, strGroupSesId, std::to_string( clsGroup._dbId ) );
                // 자기완결 그룹 디스크립터 (계획서 §5) — group.json (autojoin 경로)
                std::string strDescriptor = BuildGroupDescriptor( clsGroup, SessionOf( pszGroupId ).bBroadcast );
                gclsCallDir.PttSessionStart( pszGroupId, "autojoin", pszUserId, strDescriptor );
                strSessionDir = gclsCallDir.GetPttSessionName( pszGroupId );
            }
            // 재생성도 세션 속성(개시자·일제 통화)은 세션 캐시 값 그대로
            std::map<std::string, int> mapMemberPorts;
            int iNewFloorPort = 0;
            if ( gclsCmpClient.AddGroup( pszGroupId, clsGroup._pusers, strSharedIp, iNewFloorPort, mapMemberPorts,
                                         strRecordDir, 0, strGroupSesId, clsGroup._groupType, CmpSessionOf( clsGroup ),
                                         clsGroup._floorPolicy, clsGroup._maxTalkers, clsGroup._floorControl,
                                         strSessionDir ) ) {
                iSharedFloorPortIM = iNewFloorPort;
                // nConfigHash 는 반드시 실제 설정해시로 설정 — 0 으로 두면 다음 SyncGroupsState 가
                // 변경으로 오인해 스퓨리어스 ModifyGroup+group_change NOTIFY storm → 멤버 drop.
                m_mapGroupRtp[pszGroupId] = { iNewFloorPort, strSharedIp, ComputeGroupConfigHash( clsGroup ), "", 0,
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
    int iMemberAudioPort = 0;
    if ( !GetOrAllocMemberPort( pszGroupId, pszUserId, iMemberAudioPort ) ) {
        CLog::Print( LOG_ERROR, "InviteMember(%s) Failed to alloc member port for Group %s", pszUserId, pszGroupId );
        return false;
    }
    CSipCallRtp clsRtp;
    clsRtp.SetIpPort( strSharedIp.c_str(), iMemberAudioPort, SOCKET_COUNT_PER_MEDIA );
    //   fan-out 오퍼는 음성 + floor 만(m=video 없음 — MCPTT 는 speech, 그룹 영상 = MCVideo 호, mcvideo.md §8)

    // 서비스 코덱 (Setup.Media.Codecs 최우선 — 기본 AMR-WB PT=96). fan-out 오퍼는 CSP 가
    // 오퍼러라 이 PT 가 그룹 wire PT 가 된다 — CMP 는 relay 시 PT 를 재작성하지 않으므로 그룹
    // 전 leg 의 PT 가 이 값으로 통일되어야 한다 (pjsua UE 로컬 PT 96 정렬 — 협상 PT 불일치
    // 크래시 예방 실증값. dynamic PT 는 rtpmap 으로 식별되므로 번호 자체는 정책, RFC 3264).
    const CSipCodecEntry &clsSvcCodec = CSipCodecTable::GetTop();
    clsRtp.m_clsCodecList.push_back( clsSvcCodec.m_iPt );
    clsRtp.m_iCodec = clsSvcCodec.m_iPt;

    // 미디어 SRTP offer (media_security.md §4 표·§4.1) — required=SAVP 단일(능력 미선언 포함),
    //   optional=이 바인딩이 등록 시 mediasec(sdes-srtp) 능력을 선언한 경우만 SAVP.
    //   per-call 폴백(488 후 재-offer)은 두지 않는다 — 능력을 등록에서 이미 안다.
    {
        ServiceInfo clsSrtpSvc = gclsServiceMap.GetForUser( pszUserId, "ptt" );
        if ( clsSrtpSvc.media_srtp == "required" ||
             ( clsSrtpSvc.media_srtp == "optional" && clsUserInfo.m_bMediaSecSdes ) ) {
            std::string strSrvKey = MediaSdes::GenerateInlineKeyB64();
            if ( strSrvKey.empty() ) {
                CLog::Print( LOG_ERROR, "InviteMember(%s) SRTP key generation failed — abort invite", pszUserId );
                return false;
            }
            clsRtp.m_strLocalCryptoTag = "1";
            clsRtp.m_strLocalCryptoSuite = "AES_CM_128_HMAC_SHA1_80";
            clsRtp.m_strLocalCryptoKey = strSrvKey;
        }
    }

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
            // 도달 주소는 (IP, 포트, transport) 한 세트 — transport 누락 시 스택 기본값(UDP)으로
            //   나가 TCP 바인딩 주소에 UDP 를 쏘게 되고 NAT 이 폐기한다(fan-out 전량 408).
            pclsInvite->m_strSendDestIp = clsRoute.m_strDestIp;
            pclsInvite->m_iSendDestPort = clsRoute.m_iDestPort;
            pclsInvite->m_eTransport = clsRoute.m_eTransport;

            // To: 는 개인 AOR 유지 (cwrtc가 WS 클라이언트를 찾는 데 필요)
            // 그룹 식별은 Contact(isfocus), P-Called-Party-ID, XML body로 전달

            // 발신자 ID (mcptt-calling-user-id) = 세션 개시자 — 늦은 합류자가 아니다
            const GroupSession clsSes = SessionOf( pszGroupId );
            const std::string strCallerId =
                clsSes.strInitiator.empty() ? std::string( pszGroupId ) : clsSes.strInitiator;

            int iGroupCond = 0;
            {
                auto itCond = m_mapGroupCond.find( pszGroupId );
                if ( itCond != m_mapGroupCond.end() ) iGroupCond = itCond->second.iCond;
            }
            std::string strGroupXml =
                BuildGroupInfoXml( clsGroup, pszUserId, strCallerId, iGroupCond, NULL, clsSes.bBroadcast );
            // CMP floor port 사용 (m_mapGroupRtp에서 조회)
            int iFloorPort = iSharedFloorPortIM > 0 ? iSharedFloorPortIM : iMemberAudioPort + 1;  // fallback
            {
                auto itRtp2 = m_mapGroupRtp.find( pszGroupId );
                if ( itRtp2 != m_mapGroupRtp.end() && itRtp2->second.iFloorPort > 0 )
                    iFloorPort = itRtp2->second.iFloorPort;
            }
            // floor 없는 세션(floor_control=off)은 floor 포트가 없다 — 관례 fallback(audio+1)을
            //   그대로 두면 멤버 RTCP 포트가 floor 로 오광고되어 단말이 거기로 floor 연결을
            //   시도한다(08-05 실측 52199). m=application 은 mc_no_floor_ctrl 에코를 실어야
            //   하므로 라인은 유지하되 포트 0(미사용)으로 내린다 (RFC 3264 §6).
            if ( clsGroup._floorControl == "off" ) iFloorPort = 0;
            std::string strGroupUri = "sip:" + std::string( pszGroupId ) + "@" + strMcpttDomain;
            WrapMultipartBody( pclsInvite, strGroupXml, strSharedIp, iFloorPort, strGroupUri,
                               clsGroup._floorControl == "off" );
            // floor 줄은 본문에 덧붙인 것이라 다이얼로그 상태에 없다 — 스택이 만드는 세션 갱신 offer·멤버 re-INVITE
            //   answer 가 floor 를 m=application 0 으로 끄지 않게 같은 선언을 다이얼로그에 둔다(RFC 3264 §8)
            if ( iFloorPort > 0 )
                gclsUserAgent.SetLocalApplicationMedia( strCallId.c_str(), iFloorPort, kMemberFloorOfferFmtp );

            // MCPTT capability required (3GPP TS 24.379 §6.3.1)
            pclsInvite->AddHeader(
                "Accept-Contact",
                "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";+g.3gpp.mcptt;require;explicit" );
            // P-Asserted-Service: MCPTT ICSI — 제어 기능은 신뢰 영역 안이라 단언한다 (TS 24.379 §6.3.3.1.2 3)).
            //   헤더 이름은 RFC 6050 §4.1 의 P-Asserted-Service — 본문의 "-Id" 는 표기, 부록 A.1.3-7 예시도 이 이름.
            pclsInvite->AddHeader( "P-Asserted-Service", "urn:urn-7:3gpp-service.ims.icsi.mcptt" );
            // 단말 자동 응답 요구 (3GPP TS 24.379 §6.3.3.1)
            pclsInvite->AddHeader( "Answer-Mode", "Auto" );
            // Resource-Priority (RFC 4412/8101) — 값은 service-config 의
            // emergency-/imminent-peril-/normal-resource-priority
            //   (TS 24.379 §6.3.3.1.19 — 단말과 같은 문서, 없으면 mcpttp.15/8/0).
            pclsInvite->AddHeader( "Resource-Priority", gclsCspServiceConfig.ResourcePriorityOf( iGroupCond ).c_str() );
            // Callee identity (MCPTT 도메인 사용)
            std::string strMcpttDomain = gclsServiceMap.GetDomainByKind( "ptt" );
            char szPCalledParty[256];
            snprintf( szPCalledParty, sizeof( szPCalledParty ), "<sip:%s@%s>", pszUserId, strMcpttDomain.c_str() );
            pclsInvite->AddHeader( "P-Called-Party-ID", szPCalledParty );
            // Contact 특성 태그 = kFocusContactParams (TS 24.379 §6.3.3.1.2 1)) — 이후 in-dialog 요청(조건 재광고
            //   re-INVITE·BYE)도 같은 태그를 싣도록 다이얼로그에도 둔다.
            gclsUserAgent.SetContactParams( strCallId.c_str(), kFocusContactParams );
            const std::string strGr = SessionIdentityToken( pszGroupId );
            gclsUserAgent.SetContactUriParams( strCallId.c_str(), ( "gr=" + strGr ).c_str() );
            // INVITE 의 Contact 는 정확히 1개여야 한다(RFC 3261 §8.1.1.8). 스택은 전송 직전
            // m_clsContactList 가 비어 있을 때만 자동 Contact 를 넣으므로(SipStackComm),
            // 라우팅 가능한 자기 주소 Contact 를 구조화 리스트에 직접 1개 채운다.
            // (기존: AddHeader 원문 헤더로 도메인형 Contact 를 추가 → 리스트는 비어 있어
            //  스택 자동 Contact 와 중복 2개가 되고, 실단말이 INVITE 를 폐기하는 원인)
            {
                // Contact 는 멤버가 BYE·re-INVITE 를 보낼 목적지다 — 이 leg 의 transport 를
                //   포트와 함께 실어야 한다(CspAddressing::FillSelfContact 주석 참조).
                CSipFrom clsContact;
                CspAddressing::FillSelfContact( clsContact, clsRoute.m_eTransport, pszGroupId );
                if ( !clsRoute.m_strOutboundLocalIp.empty() )
                    clsContact.m_clsUri.m_strHost = clsRoute.m_strOutboundLocalIp;
                clsContact.m_clsUri.InsertParam( "gr", strGr.c_str() );  // 세션 식별자 GRUU (§4.5)
                clsContact.HeaderListParamParse( kFocusContactParams, (int)strlen( kFocusContactParams ) );
                pclsInvite->m_clsContactList.clear();
                pclsInvite->m_clsContactList.push_back( clsContact );
            }
            // 세션 타이머(RFC 4028) 헤더는 psip UA 가 협상값으로 싣는다 (leg_liveness.md §5.2) — 여기서 따로
            //   광고하면 갱신을 이행하지 않는 값이 그대로 나간다. refresher 는 생략한다(TS 24.379 §6.3.3.1.2 6)
            //   «The refresher parameter shall be omitted») — 규격 단말은 200 OK 에서 uas 로 정해 스스로
            //   갱신하고(§6.2.3.1.1 5)) CSP 는 만료를 감시한다. refresher 를 정하지 않는 단말(pjsip 기본 =
            //   uac)이면 CSP 가 갱신한다(leg_liveness.md §5.3).
            McStripSessionRefresher( pclsInvite->m_clsHeaderList );
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
        // Use UserId as SessionId. 미확립(pending) — 200 OK(OnCallStarted)에서 확립 표기
        m_mapCallSession[strCallId] = { pszGroupId, pszUserId, pszUserId, false };
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

        RetryPendingLeaves();  // 실패한 PTT_LEAVE 재시도 — 대기열이 비면 즉시 반환한다
        CheckSessionLimits();  // TNG3(그룹 호 최대 시간) — 세션이 없으면 즉시 반환한다
        CheckAckGates();       // TNG1(확인 통화 설정) — 게이트가 없으면 즉시 반환한다

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

// CMP 에 재전달이 필요한 그룹 설정의 지문 — 값이 바뀌면 SyncGroupsState 가 MODIFY 를 보낸다.
//   멤버(로스터·우선순위)에 더해 floor 정책을 포함한다: 정책만 바꾼 경우에도 CMP 에 도달해야
//   운영 중 정원 조정(예: single ↔ multi)이 실제로 반영된다.
size_t CGroupCallService::ComputeGroupConfigHash( const CspPttGroup &group ) {
    std::string strHashInput;
    for ( const auto &pUser : group._pusers ) {
        if ( !pUser ) continue;
        strHashInput += pUser->_id + ":" + std::to_string( pUser->_priority ) + ";";
    }
    strHashInput += "|floor=" + group._floorPolicy + ":" + std::to_string( group._maxTalkers );
    strHashInput += "|t4=" + std::to_string( group._hangTimerSec );  // hang-timer 변경도 MODIFY 로 CMP 에 도달
    return std::hash<std::string>{}( strHashInput );
}

void CGroupCallService::SyncGroupsState() {
    // A. Add New Groups
    gclsGroupMap.IterateInternal( [this]( const CspPttGroup &group ) {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );

        // Calculate Hash
        size_t nHash = ComputeGroupConfigHash( group );

        auto itRtp = m_mapGroupRtp.find( group._id );
        if ( itRtp == m_mapGroupRtp.end() ) {
            // 규격 모델: CMP 그룹 컨텍스트를 proactive 하게 만들지 않는다.
            //   세션 생성은 발신 INVITE(on-demand: ProcessGroupCall) 또는
            //   affiliation 기반 합류(chat: CheckGroupIntegrity)가 담당. 여기선 무동작.
            return;
        } else {
            // EXISTING GROUP - Check for Diff
            if ( itRtp->second.nConfigHash != nHash ) {
                // CHANGED
                lock.unlock();
                CLog::Print( LOG_INFO, "SyncGroupsState: Group(%s) Config Changed. Sending ModifyGroup.",
                             group._id.c_str() );
                if ( gclsCmpClient.ModifyGroup( group._id, group._pusers, GetOrIssueGroupSesId( group._id ),
                                                group._floorPolicy, group._maxTalkers,
                                                CmpSessionOf( group ).iT4Sec ) ) {
                    std::unique_lock<std::recursive_mutex> lock2( m_mutex );
                    m_mapGroupRtp[group._id].nConfigHash = nHash;
                } else {
                    // MODIFY 실패 (NOT_FOUND: CMP 그룹 소실 등) — AddGroup 멱등 재수립.
                    //   재생성이면 floor/멤버 포트가 새로 할당되므로 캐시를 응답값으로 갱신한다.
                    std::string strIp, strRecordDir, strSessionDir;
                    int iFloorPort = 0;
                    std::map<std::string, int> mapMemberPorts;
                    // 재수립도 같은 세션의 산출물 자리를 가리켜야 한다 — sesid 를 근거로 record_dir 과
                    //   session_dir 을 함께 넘긴다. session_dir 없이 재수립하면 CMP 가 시간버킷 직행으로
                    //   녹취를 열고, 뒤에 세션 ADD 가 와도 그 자리에 머물러 세그먼트가 이력에서 사라진다.
                    std::string strGroupSesId = GetOrIssueGroupSesId( group._id );
                    if ( gclsCallDir.IsEnabled() ) {
                        strRecordDir =
                            gclsCallDir.GetPttSessionDir( group._id, strGroupSesId, std::to_string( group._dbId ) );
                        strSessionDir = gclsCallDir.GetPttSessionName( group._id );
                    }
                    if ( gclsCmpClient.AddGroup( group._id, group._pusers, strIp, iFloorPort, mapMemberPorts,
                                                 strRecordDir, group._sessionSeq, strGroupSesId, group._groupType,
                                                 CmpSessionOf( group ), group._floorPolicy, group._maxTalkers,
                                                 group._floorControl, strSessionDir ) ) {
                        std::unique_lock<std::recursive_mutex> lock2( m_mutex );
                        auto it2 = m_mapGroupRtp.find( group._id );
                        if ( it2 != m_mapGroupRtp.end() ) {
                            it2->second.iFloorPort = iFloorPort;
                            it2->second.strIp = strIp;
                            it2->second.memberPorts = mapMemberPorts;
                            it2->second.nConfigHash = nHash;
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
                } else if ( itSess->second.bListenOnly ) {
                    // 청취 leg(dispatch_center.md §5.6)는 그룹 멤버가 아닌 것이 정상 — 멤버십 검사 대상이 아니다.
                    //   수명은 세션(마지막 멤버 이탈 teardown)과 자기 BYE 가 정한다.
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
    //  - prearranged                 : on-demand — active 세션의 컨텍스트/콜로그 보장만. 서버 주도
    //                                  재초대 없음(late entry/복구 = UE 주도 재조인·사용자 재참여).
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

        // 2) 세션 존재 판정 (확립 leg 만 — pending 초대가 '활성'을 자가 재생산하는 루프 방지)
        bool bActive = false, bHasContext = false;
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            bHasContext = ( m_mapGroupRtp.find( group._id ) != m_mapGroupRtp.end() );
            bActive = HasActiveLeg( group._id );
            if ( m_mapAckGate.count( group._id ) ) return;  // 개시자 응답 대기 — 게이트가 끝난 뒤에 본다
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
            std::map<std::string, int> mapMemberPorts;
            std::string strRecordDir, strSessionDir;
            std::string strGroupSesId = GetOrIssueGroupSesId( group._id );
            if ( gclsCallDir.IsEnabled() ) {
                strRecordDir = gclsCallDir.GetPttSessionDir( group._id, strGroupSesId, std::to_string( group._dbId ) );
                strSessionDir = gclsCallDir.GetPttSessionName( group._id );
            }
            if ( !gclsCmpClient.AddGroup( group._id, group._pusers, ip, floorPort, mapMemberPorts, strRecordDir,
                                          group._sessionSeq, strGroupSesId, group._groupType, CmpSessionOf( group ),
                                          group._floorPolicy, group._maxTalkers, group._floorControl, strSessionDir ) )
                return;
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            m_mapGroupRtp[group._id] = { floorPort, ip, ComputeGroupConfigHash( group ), "", 0, mapMemberPorts };
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

        // 5) 누락 멤버 초대 — chat 전용(상시 채널 유지). prearranged 의 서버 주도
        //    주기 재초대는 폐지: TS 24.379 의 late entry 는 UE 주도 재조인 모델이고, 백오프 없는
        //    재초대는 미응답 멤버에게 무한 INVITE 루프가 된다(개시 시 fan-out 은 ProcessGroupCall 유지).
        if ( !bPersistent ) return;
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
                                       int iRemoteFloorPort, CSipCallRtp *pclsRtp ) {
    std::string strGroupId, strSessionId, strMemberId;
    int iCmpFloorPort = 0;
    PttDialogLeg clsDlgLeg;

    // 1. lock 보유 중 맵 조회만 수행
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto it = m_mapCallSession.find( strCallId );
        if ( it == m_mapCallSession.end() ) return;

        it->second.bEstablished = true;  // 200 OK 수신 = leg 확립 (세션 활성 집계 대상)
        strGroupId = it->second.strGroupId;
        strSessionId = it->second.strSessionId;
        strMemberId = it->second.strMemberId;
        clsDlgLeg = _pttLegOf( strCallId, it->second );

        // CMP에서 할당한 floor_port 조회 (멤버 SDP에서 파싱 불필요)
        auto itRtp = m_mapGroupRtp.find( strGroupId );
        if ( itRtp != m_mapGroupRtp.end() ) {
            iCmpFloorPort = itRtp->second.iFloorPort;
        }
    }
    // dialog-event(§5.6a): 멤버 leg 확립 — 멤버 회선 감시자에게 confirmed
    EmitPttDialog( clsDlgLeg, "confirmed" );
    // 2. lock 해제 후 외부 호출 (CMP, DB)
    int iFloorPort = iRemoteFloorPort > 0 ? iRemoteFloorPort : ( iRemotePort + 1 );
    // 멤버 role 조회 (chair/participant) — CMP floor 선점 판정에 사용
    std::string strRole = "participant";
    CspPttGroup clsGroup;
    bool bHaveGroup = gclsGroupMap.Select( strGroupId.c_str(), clsGroup );
    if ( bHaveGroup ) {
        for ( const auto &pUser : clsGroup._pusers ) {
            if ( pUser && pUser->_id == strMemberId ) {
                strRole = pUser->_role;
                break;
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
            // NAT 판정인데 guard IP 가 비면(UserMap 미조회 — 등록 만료/ID 불일치) CMP 의
            //   latch IP guard 가 이 leg 에 한해 무력화된다 — 조용한 약화 방지용 경고.
            if ( strMemberGuardIp.empty() && clsNatSvc.latch_ip_guard != "off" )
                CLog::Print( LOG_ERROR,
                             "OnCallStarted: member leg NAT without sig-guard ip"
                             " (member=%s sdp=%s) — UserMap miss, latch guard disabled",
                             strMemberId.c_str(), strRemoteIp.c_str() );
        }
    }
    // 멤버 leg PT — 서버 offer(코덱 테이블) vs 멤버 answer wire PT. answer 가 비 96 이어도
    //   CMP leg 별 재작성으로 그룹 정합 (타사 단말 interop).
    int iMemberPt = 0, iMemberSrcPt = 0, iMemberTePt = 0, iMemberSrcTePt = 0;
    std::string strMemberCodec;
    GetLegPt( strCallId, true, iMemberPt, iMemberSrcPt, iMemberTePt, iMemberSrcTePt, &strMemberCodec );
    // 멤버 answer 의 fmtp:MCPTT 협상 결과 (queueing/max_priority). 초기 발언권은 주지 않는다 — 암묵적 발언 요청은
    // 클라이언트가
    //   낸 SIP 요청(개시 INVITE)만 뜻한다(TS 24.380 §14.2.5). 서버 offer 에 대한 멤버 answer 의 mc_granted 는 요청이
    //   아니다
    McpttFmtp clsMemberFmtp;
    ParseMcpttFmtp( pclsRtp, clsMemberFmtp );
    clsMemberFmtp.iGranted = 0;
    // 미디어 SRTP (media_security.md §5.2) — 서버 offer 에 crypto 를 실었는지는 다이얼로그
    //   local RTP 가 기억한다 (재협상 re-INVITE 합류 경로 포함 — 키 불변이면 CMP 가 세션 유지).
    CmpMediaCrypto clsMemberCrypto;
    {
        CSipCallRtp clsLocalRtp;
        if ( gclsUserAgent.GetLocalCallRtp( strCallId.c_str(), &clsLocalRtp ) &&
             !clsLocalRtp.m_strLocalCryptoKey.empty() ) {
            // SAVP offer 의 answer 는 유효한 crypto(동일 suite) 필수 (RFC 4568 §5.1.2) —
            //   부재/불량 = 협상 실패. 평문으로 조용히 폴백하지 않고 leg 를 종료한다.
            const bool bOk = pclsRtp && !pclsRtp->m_strRemoteCryptoKey.empty() &&
                             pclsRtp->m_strRemoteCryptoSuite == clsLocalRtp.m_strLocalCryptoSuite &&
                             MediaSdes::BuildCmpKeys( clsLocalRtp.m_strLocalCryptoSuite, pclsRtp->m_strRemoteCryptoKey,
                                                      clsLocalRtp.m_strLocalCryptoKey, clsMemberCrypto );
            if ( !bOk ) {
                CLog::Print( LOG_ERROR,
                             "OnCallStarted: member(%s) SRTP answer missing/invalid (group=%s suite=%s) — drop leg",
                             strMemberId.c_str(), strGroupId.c_str(), clsLocalRtp.m_strLocalCryptoSuite.c_str() );
                gclsUserAgent.StopCall( strCallId.c_str() );
                AckGateMemberResult( strGroupId, strMemberId, SIP_NOT_ACCEPTABLE_HERE );  // 협상 실패 = 최종 거절
                return;
            }
        }
    }
    const CmpMediaCrypto *pclsMemberCrypto = clsMemberCrypto.bEnabled ? &clsMemberCrypto : NULL;
    int iJoinLocalAudio = 0;
    PurgePendingLeave( strGroupId, strSessionId );  // JOIN 이 «지금 있다» 는 권위 — 밀린 LEAVE 를 버린다
    bool bJoined = gclsCmpClient.JoinGroup( strGroupId, strSessionId, strRemoteIp, iRemotePort, iFloorPort,
                                            GetOrIssueGroupSesId( strGroupId ), strRole, &iJoinLocalAudio, iMemberNat,
                                            strMemberGuardIp, iMemberPt, iMemberSrcPt, iMemberTePt, iMemberSrcTePt,
                                            strMemberCodec, clsMemberFmtp, pclsMemberCrypto, 0, 0 );
    // 방어: JOIN 응답의 멤버 포트가 offer 에 쓴 캐시와 다르면(유닛 재배정) 캐시를 교정한다.
    //   이 호 자체는 이미 옛 포트로 SDP 를 받아 상향이 성립하지 않으므로 발생 = 버그 신호(ERROR).
    //   정상 경로에서는 LeaveGroup 시 InvalidateMemberPort 로 캐시가 비워져 여기 오지 않는다.
    if ( bJoined && iJoinLocalAudio > 0 ) {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto itRtp = m_mapGroupRtp.find( strGroupId );
        if ( itRtp != m_mapGroupRtp.end() ) {
            auto itM = itRtp->second.memberPorts.find( strSessionId );
            if ( itM != itRtp->second.memberPorts.end() && itM->second != iJoinLocalAudio ) {
                CLog::Print( LOG_ERROR,
                             "OnCallStarted: member port drift group=%s member=%s offer=%d join=%d (cache corrected)",
                             strGroupId.c_str(), strSessionId.c_str(), itM->second, iJoinLocalAudio );
            }
            itRtp->second.memberPorts[strSessionId] = iJoinLocalAudio;
        }
    }
    if ( !bJoined && bHaveGroup ) {
        // JoinGroup 실패의 주요 원인은 CMP 그룹 소실(NOT_FOUND) — CMP 재시작/orphan 정리 후 CSP 세션만
        //   남은 상태. JoinGroup 경로엔 self-heal 이 없어 영구 무음이 되므로, SyncGroupsState 의 MODIFY
        //   실패 self-heal 과 대칭으로 AddGroup(멱등) 재수립 후 1회 재시도한다.
        std::string strReAddIp, strReAddRecDir, strReAddSesDir;
        int iReAddFloor = 0;
        std::map<std::string, int> mapReAddPorts;
        std::string strReAddSesId = GetOrIssueGroupSesId( strGroupId );
        if ( gclsCallDir.IsEnabled() ) {
            strReAddRecDir =
                gclsCallDir.GetPttSessionDir( strGroupId, strReAddSesId, std::to_string( clsGroup._dbId ) );
            strReAddSesDir = gclsCallDir.GetPttSessionName( strGroupId );
        }
        if ( gclsCmpClient.AddGroup( strGroupId, clsGroup._pusers, strReAddIp, iReAddFloor, mapReAddPorts,
                                     strReAddRecDir, clsGroup._sessionSeq, strReAddSesId, clsGroup._groupType,
                                     CmpSessionOf( clsGroup ), clsGroup._floorPolicy, clsGroup._maxTalkers,
                                     clsGroup._floorControl, strReAddSesDir ) ) {
            {
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                auto itRe = m_mapGroupRtp.find( strGroupId );
                if ( itRe != m_mapGroupRtp.end() ) {
                    itRe->second.iFloorPort = iReAddFloor;
                    itRe->second.strIp = strReAddIp;
                    itRe->second.memberPorts = mapReAddPorts;
                    itRe->second.nConfigHash = ComputeGroupConfigHash( clsGroup );
                }
            }
            CLog::Print( LOG_INFO,
                         "OnCallStarted: Group(%s) NOT_FOUND → AddGroup re-established (floor=%d), retry JoinGroup",
                         strGroupId.c_str(), iReAddFloor );
            PurgePendingLeave( strGroupId, strSessionId );
            bJoined = gclsCmpClient.JoinGroup( strGroupId, strSessionId, strRemoteIp, iRemotePort, iFloorPort,
                                               GetOrIssueGroupSesId( strGroupId ), strRole, NULL, iMemberNat,
                                               strMemberGuardIp, iMemberPt, iMemberSrcPt, iMemberTePt, iMemberSrcTePt,
                                               strMemberCodec, clsMemberFmtp, pclsMemberCrypto, 0, 0 );
        }
    }
    if ( bJoined ) {
        CLog::Print( LOG_INFO, "OnCallStarted: Joined Group(%s) Peer(%s:%d floor=%d)", strGroupId.c_str(),
                     strRemoteIp.c_str(), iRemotePort, iFloorPort );
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

    // 개시자 응답 게이트 — 멤버 200 누계·필수 멤버 응답 (TS 24.379 §10.1.1.4.1.1 3)·§6.3.3.3)
    AckGateMemberResult( strGroupId, strMemberId, SIP_OK );
}

// BYE/Error -> Leave Group
int CGroupCallService::TerminateGroupLocal( const std::string &strGroupId ) {
    if ( strGroupId.empty() ) return 0;

    std::vector<std::string> vecCallIds;
    std::vector<PttDialogLeg> vecDlg;
    // 1) lock 안 — 이 그룹의 활성 멤버 호를 수집하고 로컬 맵에서 제거 (CMP/네트워크 호출 금지)
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        for ( auto it = m_mapCallSession.begin(); it != m_mapCallSession.end(); ) {
            if ( it->second.strGroupId == strGroupId ) {
                vecCallIds.push_back( it->first );
                vecDlg.push_back( _pttLegOf( it->first, it->second ) );
                for ( auto uIt = m_mapUserCall.begin(); uIt != m_mapUserCall.end(); ++uIt ) {
                    if ( uIt->second == it->first ) {
                        m_mapUserCall.erase( uIt );
                        break;
                    }
                }
                it = m_mapCallSession.erase( it );
            } else {
                ++it;
            }
        }
        m_mapGroupRtp.erase( strGroupId );
        RemoveGroupSesId( strGroupId );
        // CheckGroupIntegrity race 방지 grace 기록 (OnCallTerminated 와 동일)
        m_mapGroupLastTerminate[strGroupId] = std::chrono::steady_clock::now();
    }

    if ( vecCallIds.empty() ) return 0;

    // 2) lock 해제 후 BYE + B2BUA 레코드 정리. dead node → LeaveGroup/RemoveGroup(blocking) 생략.
    CLog::Print( LOG_INFO, "TerminateGroupLocal: Group(%s) media node down — %zu member call(s) BYE (local only)",
                 strGroupId.c_str(), vecCallIds.size() );
    for ( const auto &clsDlg : vecDlg ) EmitPttDialog( clsDlg, "terminated" );  // dialog-event(§5.6a)
    for ( const auto &strCallId : vecCallIds ) {
        gclsUserAgent.StopCall( strCallId.c_str() );
        gclsCallMap.Delete( strCallId.c_str(), false );  // 그룹호: RemoveSession 미사용(dead node)
    }

    // best-effort 이력/DB 마감 (dead node 무관 — 로컬/DB 만)
    //   강제 회수 경로다 — 마지막 멤버가 나가서 끝난 것이 아니므로 완료율의 분자에 넣지
    //   않는다(§8 Y4). 정상 종료로 세면 노드 소실·그룹 삭제가 "정상" 으로 보인다.
    if ( gclsCallDir.IsEnabled() ) gclsCallDir.PttSessionEnd( strGroupId, "error" );
    if ( gclsDbManager.IsConnected() ) gclsDbManager.EndGroupCallLog( strGroupId );

    return (int)vecCallIds.size();
}

bool CGroupCallService::OnCallTerminated( const std::string &strCallId ) {
    std::string strGroupId, strMemberId, strSessionId, strListenGroup;
    bool bStillActive = false;
    bool bFound = false;
    bool bListen = false, bListenHidden = true;
    time_t tListenStart = 0;
    PttDialogLeg clsDlgLeg;
    std::vector<PttDialogLeg> vecPendingDlg;  // 세션 해제로 함께 걷는 미확립 초대 leg 의 terminated

    // 1. lock 보유 중 맵 조회/수정만 수행 (외부 호출 금지)
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        CLog::Print( LOG_DEBUG, "OnCallTerminated: Enter CallId=%s", strCallId.c_str() );
        m_mapPendingInfo.erase( strCallId );  // ACK 전에 끝난 호의 Warning 149 INFO

        auto it = m_mapCallSession.find( strCallId );
        if ( it == m_mapCallSession.end() ) return false;

        strGroupId = it->second.strGroupId;
        strMemberId = it->second.strMemberId;
        strSessionId = it->second.strSessionId;
        bListen = it->second.bListenOnly;
        bListenHidden = it->second.bListenHidden;
        strListenGroup = it->second.strListenGroup;
        tListenStart = it->second.tListenStart;
        clsDlgLeg = _pttLegOf( strCallId, it->second );

        m_mapCallSession.erase( it );

        for ( auto uIt = m_mapUserCall.begin(); uIt != m_mapUserCall.end(); ++uIt ) {
            if ( uIt->second == strCallId ) {
                m_mapUserCall.erase( uIt );
                break;
            }
        }

        bStillActive = HasActiveLeg( strGroupId );  // 확립 leg 만 집계 — pending 초대·청취 leg 가 세션을 붙들지 못한다
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
    EmitPttDialog( clsDlgLeg, "terminated" );  // dialog-event(§5.6a) — 그룹 컨텍스트가 아직 살아 있을 때
    LeaveGroupOrQueue( strGroupId, strSessionId, GetOrIssueGroupSesId( strGroupId ), "leg 종료" );
    InvalidateMemberPort( strGroupId, strSessionId );
    if ( bListen ) {
        EmitPttListenAudit( "ended", strMemberId, strListenGroup, strGroupId, GetOrIssueGroupSesId( strGroupId ),
                            tListenStart > 0 ? (int)( time( NULL ) - tListenStart ) * 1000 : -1 );
    }

    // private call(1:1, TS 24.379 §11.1): 한쪽이 끊으면 세션 전체가 끝난다 — 그룹 시맨틱
    //   (한 멤버 이탈해도 세션 유지)을 적용하지 않고 잔여 leg 를 종료한다. 실행은 함수 끝에서
    //   BYE 발신 + 본 함수 재진입으로 한다 — psip 은 로컬 StopCall 로 끝낸 호에 EventCallEnd
    //   를 올리지 않으므로, BYE 만 보내면 잔여 leg 의 마지막-멤버 teardown(그룹 해제·adhoc
    //   제거·CMP REMOVE)이 실행되지 않는다. BYE 응답 유무와도 무관해야 한다 — 미응답 단말이
    //   그룹을 붙들면 안 된다.
    //   **청취 leg 이탈은 이 규칙을 발동시키지 않는다**(dispatch_center.md §5.6·§5.10). 청취자는 참가자가
    //   아니라 관측자다 — 감청자가 빠졌다고 사설콜 당사자를 끊으면 «자격 회수» 가 «업무 통화 차단» 이 된다.
    //   (ad hoc 의 «잔여 1명» 도 같다 — 참가자 수는 청취자 이탈로 변하지 않는다.)
    std::vector<std::string> vecPrivPeerLegs;
    if ( bStillActive && !bListen ) {
        CspPttGroup clsPrivChk;
        bool bGateOpen;  // 개시자 응답 대기 중 — 참가 leg 수·개시자 이탈 판정은 게이트가 끝난 뒤의 일이다
        {
            std::unique_lock<std::recursive_mutex> lock( m_mutex );
            bGateOpen = m_mapAckGate.count( strGroupId ) > 0;
        }
        if ( !bGateOpen && gclsGroupMap.Select( strGroupId.c_str(), clsPrivChk ) ) {
            std::vector<std::string> vecRemainLegs;
            {
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                for ( const auto &kv : m_mapCallSession )
                    if ( kv.second.strGroupId == strGroupId && !kv.second.bListenOnly )
                        vecRemainLegs.push_back( kv.first );
            }
            const GroupSession clsSesNow = SessionOf( strGroupId );
            if ( clsPrivChk._groupType == "private" ) {
                vecPrivPeerLegs = vecRemainLegs;
                CLog::Print( LOG_INFO, "OnCallTerminated: private(%s) — 상대 leg %zu 개 종료(BYE)", strGroupId.c_str(),
                             vecPrivPeerLegs.size() );
            } else if ( clsSesNow.bBroadcast && clsSesNow.strInitiator == strMemberId ) {
                // 일제 통화 개시자 이탈 = 호 해제 (TS 24.379 §6.3.8.1 3) "the initiator of the group call leaves" —
                // 로컬 정책).
                //   일제 통화는 개시자 송출이 끝나면 호도 끝난다(§4.12) — 편성·ad hoc 모두, 수신자는 T4·TNG3 를
                //   기다리지 않는다
                vecPrivPeerLegs = vecRemainLegs;
                CLog::Print( LOG_INFO,
                             "OnCallTerminated: broadcast group(%s) — 개시자 %s 이탈, 잔여 leg %zu 개 종료(BYE)",
                             strGroupId.c_str(), strMemberId.c_str(), vecPrivPeerLegs.size() );
            } else if ( ( clsPrivChk._isAdhoc || IsOnDemandGroupCall( clsPrivChk ) ) && vecRemainLegs.size() == 1 ) {
                // 그룹 호 해제 정책 (TS 24.379 §6.3.8.1): 참가자 1명 이하 = 대화 상대가 없는 호 — 해제한다.
                //   on-demand 그룹 호(편성·ad hoc)만 — chat 은 상시 채널이라 잔류를 허용한다. 미확립 fan-out
                //   초대가 남아 있으면 맵에 함께 잡혀 여기 오지 않는다(합류 대기 유지). 청취 leg 는 참가자가 아니다.
                vecPrivPeerLegs = vecRemainLegs;
                CLog::Print( LOG_INFO, "OnCallTerminated: group(%s) — 잔여 1 leg 종료(BYE, min-participants)",
                             strGroupId.c_str() );
            }
        }
    }

    // PTT history: member leave event (청취 leg 는 참가자 이력·DB 에 없다)
    if ( gclsCallDir.IsEnabled() && !bListen ) {
        gclsCallDir.PttMemberLeave( strGroupId, strMemberId );
        if ( !bStillActive ) {
            gclsCallDir.PttSessionEnd( strGroupId );
        }
    }

    if ( gclsDbManager.IsConnected() && !bListen ) {
        gclsDbManager.UpdateParticipantLeft( strGroupId, strMemberId );
        if ( !bStillActive ) {
            gclsDbManager.EndGroupCallLog( strGroupId );
        }
    }

    // RFC 4575: 이탈을 conference 구독자 + 잔여 참가자에게 통지.
    //   구독은 참여보다 오래 산다 — 단말은 이탈 후에도 conference 구독을 유지하고 미조인
    //   채널까지 구독한다. 따라서 "잔여 확립 leg 없음"이 "통지 대상 없음"을 뜻하지 않으며,
    //   마지막 멤버 이탈도 반드시 통지해야 구독자의 로스터가 빈 상태로 수렴한다. (통지를
    //   in-dialog 로만 보내던 시절엔 leg=0 이면 실을 다이얼로그가 없어 생략이 맞았다.)
    //   ⚠ teardown 앞에서 호출한다 — BuildConferenceInfoBody 가 m_mapGroupRtp 의
    //   iConfVersion 을 증가시키므로 erase 뒤에 부르면 version 이 0 으로 되돌아가고
    //   수신측이 stale 로 버릴 수 있다.
    if ( !strGroupId.empty() && !( bListen && bListenHidden ) ) {
        SendConferenceNotify( strGroupId, strMemberId, "disconnected", "deleted" );
    }
    if ( !bStillActive && !strGroupId.empty() ) {
        // on-demand 그룹(편성·ad hoc): 마지막 확립 멤버 이탈 시 세션 즉시 해제 (chat 은 상시 유지).
        CspPttGroup clsGrp;
        bool bSelected = gclsGroupMap.Select( strGroupId.c_str(), clsGrp );
        bool bChat = bSelected && clsGrp._groupType == "chat";
        if ( !bChat ) {
            // 미확립(pending) fan-out INVITE 잔존분 취소 — 세션 해제 후 뒤늦게 200 OK 가 와서
            // 없는 그룹에 JOIN 하는 고아 leg 방지 (StopCall 재진입은 맵 선삭제로 no-op).
            std::vector<std::string> vecPending;
            {
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                for ( auto itP = m_mapCallSession.begin(); itP != m_mapCallSession.end(); ) {
                    if ( itP->second.strGroupId == strGroupId ) {
                        vecPending.push_back( itP->first );
                        vecPendingDlg.push_back( _pttLegOf( itP->first, itP->second ) );
                        itP = m_mapCallSession.erase( itP );
                    } else {
                        ++itP;
                    }
                }
                for ( auto uIt = m_mapUserCall.begin(); uIt != m_mapUserCall.end(); ) {
                    if ( uIt->first.second == strGroupId ) {
                        uIt = m_mapUserCall.erase( uIt );
                    } else {
                        ++uIt;
                    }
                }
            }
            for ( const auto &clsPendingDlg : vecPendingDlg ) EmitPttDialog( clsPendingDlg, "terminated" );
            for ( const auto &strPending : vecPending ) {
                CLog::Print( LOG_INFO, "OnCallTerminated: cancel pending invite Call(%s) — Group(%s) session end",
                             strPending.c_str(), strGroupId.c_str() );
                gclsUserAgent.StopCall( strPending.c_str() );
                gclsCallMap.Delete( strPending.c_str(), false );
            }
            gclsCmpClient.RemoveGroup( strGroupId, GetOrIssueGroupSesId( strGroupId ) );
            {
                std::unique_lock<std::recursive_mutex> lock( m_mutex );
                m_mapGroupRtp.erase( strGroupId );
                RemoveGroupSesId( strGroupId );
            }
            // ad hoc/private 임시 그룹: 세션 종료 시 GroupMap 에서도 제거 (ephemeral —
            //   다음 개시 시 발신 SDP 기준 새 모드로 재생성. de-register 경로와 동일 계약)
            if ( bSelected && clsGrp._isAdhoc ) {
                gclsGroupMap.Remove( strGroupId.c_str() );
                if ( clsGrp._groupType != "private" ) EmitRegroupEvent( "released", strGroupId, "ad-hoc" );
                CLog::Print( LOG_INFO, "OnCallTerminated: ad-hoc group(%s) removed from map (session ended)",
                             strGroupId.c_str() );
            }
        }
    }

    // private 잔여 leg 종료 실행 — BYE 발신 + teardown 재진입. 마지막 leg 가 위의
    //   !bStillActive 경로를 밟아 그룹 해제까지 완결한다 (동시 BYE glare 는 맵 선삭제로 no-op).
    for ( const auto &strPeerLeg : vecPrivPeerLegs ) {
        gclsUserAgent.StopCall( strPeerLeg.c_str() );
        OnCallTerminated( strPeerLeg );
    }

    return bFound;
}

// ─────────────────────────────────────────────────────────
// Conference Event Package (RFC 4575) — in-dialog NOTIFY
// ─────────────────────────────────────────────────────────

bool CGroupCallService::HasActiveLeg( const std::string &strGroupId ) const {
    // 개시자 응답 대기 중인 세션은 살아 있다 — 개시자 leg 이 아직 맵에 없어도 멤버 이탈이 세션을 해제하지 않게
    if ( m_mapAckGate.count( strGroupId ) ) return true;
    for ( const auto &kv : m_mapCallSession )
        if ( kv.second.strGroupId == strGroupId && kv.second.bEstablished && !kv.second.bListenOnly ) return true;
    return false;
}

// TS 24.379 §10.1.3.4.1 — conference 이벤트 패키지 구독 인가 (dispatch_center.md §5.6).
//   규격: controlling function 이 구독자(<mcptt-calling-user-id> ≒ From)를 그룹 문서(TS 24.481)의
//   <on-network-allow-conference-state> 로 판정, 불허 시 403 + Warning "138 subscription of conference events not
//   allowed". 일제 통화로 개시된 호는 480 + Warning 105. CIMS 해석: 그룹 멤버 = 그룹 속성값(기본 허용), 비멤버
//   관제사 = 청취 leg 와 같은 2단 인가(프로파일 allow_ambient_listening + 관제 그룹 ptt_listen 범위) — 합류 전
//   사전 모니터링 구독(진행 중·참가자 수)을 같은 축으로 허용한다. 프로파일 부재·DB 불가는 불허(fail-closed).
//   즉석 세션(adhoc-/priv-)은 그룹 문서가 없고 참가자 = fan-out 대상이라 통과, 미지 자원은 기존 처리에 맡긴다.
int CGroupCallService::CheckConferenceSubscribe( const std::string &strGroupId, const std::string &strUserId,
                                                 std::string &strWarning, std::string &strReason, bool *pbUnavailable,
                                                 bool bAuthzOnly ) {
    if ( pbUnavailable ) *pbUnavailable = false;
    CspPttGroup clsGroup;
    if ( !gclsGroupMap.Select( strGroupId.c_str(), clsGroup ) ) return 0;
    // 일제 통화로 개시된 호의 conference 구독 = 480 + Warning 105 (TS 24.379 §10.1.3.4.1) — 판정은 그룹 종류가
    //   아니라 진행 중 세션의 속성이다(같은 그룹의 일반 그룹 통화는 구독 가능). ad hoc 일제 통화(§17.2.2.1.1 9))도
    //   같다 — 즉석 세션 인가보다 먼저 본다(참가자도 일제 통화 중에는 구독할 수 없다).
    if ( !bAuthzOnly && gclsGroupCallService.IsBroadcastInProgress( strGroupId ) ) {
        strWarning = McpttWarning( 105, "subscription not allowed in a broadcast group call",
                                   gclsServiceMap.GetDomainByKind( "ptt" ) );
        strReason = "broadcast group call";
        return SIP_TEMPORARILY_UNAVAILABLE;
    }
    if ( clsGroup._isAdhoc ) {
        // 즉석 세션(priv-/adhoc-) — 그룹 문서가 없다. 참가자(fan-out 대상)는 허용, 그 외(관제사)는 청취 leg 와 같은
        //   즉석 세션 관측 인가(자격 + 참가자 monitor_scope). 세션 id 를 아는 것만으로 로스터가 열리지 않게
        //   한다(§5.6a).
        std::string strWhy;
        if ( CanObserveEphemeral( clsGroup, strUserId, strWhy, pbUnavailable ) ) return 0;
        strWarning = McpttWarning( 138, "subscription of conference events not allowed",
                                   gclsServiceMap.GetDomainByKind( "ptt" ) );
        strReason = "ephemeral session, " + strWhy;
        return SIP_FORBIDDEN;
    }
    bool bMember = false;
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( pUser && ( pUser->_id == strUserId || pUser->_mcpttId == strUserId ) ) {
            bMember = true;
            break;
        }
    }
    if ( bMember && clsGroup._allowConferenceState ) return 0;
    CspUserProfile clsProf;
    const int iProf = gclsDbManager.SelectUserProfile( strUserId.c_str(), clsProf );
    if ( iProf < 0 && pbUnavailable ) *pbUnavailable = true;
    const std::string strDg = gclsRoleMap.RoleIdForLine( strUserId.c_str() );
    if ( iProf == 1 && clsProf.m_bAllowAmbientListening &&
         gclsRoleMap.CanListenPtt( strUserId.c_str(), strGroupId.c_str() ) ) {
        CLog::Print( LOG_INFO, "SUBSCRIBE conference: %s on group %s allowed by role listen scope (%s)",
                     strUserId.c_str(), strGroupId.c_str(), strDg.c_str() );
        return 0;
    }
    strWarning =
        McpttWarning( 138, "subscription of conference events not allowed", gclsServiceMap.GetDomainByKind( "ptt" ) );
    if ( bMember )
        strReason = "on-network-allow-conference-state=false";
    else if ( iProf != 1 )
        strReason = ( iProf < 0 ) ? "non-member, profile unavailable" : "non-member, no profile";
    else if ( !clsProf.m_bAllowAmbientListening )
        strReason = "non-member, allow_ambient_listening=0";
    else
        strReason = "non-member, ptt_listen scope (" + strDg + ")";
    return SIP_FORBIDDEN;
}

// 즉석 세션 관측 인가 (dispatch_center.md §5.6a) — 사설콜·애드혹은 PTT 그룹이 아니라 사람 사이의 세션이라 범위 축은
//   ptt_listen(그룹 목록)이 아닌 "참가자 중 한 명의 전화 그룹이 관측자 역할 monitor_call 안"(VoLTE 통화 Join·dialog
//   감시와 같은 CanWatch)다. 자격은 그룹콜 청취와 같은 allow_ambient_listening(TS 24.484). 참가자 자신은 항상 허용.
bool CGroupCallService::CanObserveEphemeral( const CspPttGroup &clsGroup, const std::string &strUserId,
                                             std::string &strReason, bool *pbUnavailable ) {
    if ( pbUnavailable ) *pbUnavailable = false;
    for ( const auto &pUser : clsGroup._pusers )
        if ( pUser && ( pUser->_id == strUserId || pUser->_mcpttId == strUserId ) ) return true;
    CspUserProfile clsProf;
    const int iProf = gclsDbManager.SelectUserProfile( strUserId.c_str(), clsProf );
    if ( iProf < 0 && pbUnavailable ) *pbUnavailable = true;  // 조회 불능 — 자격 없음과 다르다
    if ( iProf != 1 || !clsProf.m_bAllowAmbientListening ) {
        strReason = ( iProf < 0 ) ? "profile unavailable" : ( iProf != 1 ) ? "no profile" : "allow_ambient_listening=0";
        return false;
    }
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( !pUser ) continue;
        if ( gclsRoleMap.CanWatch( strUserId.c_str(), gclsPhoneGroupMap.EffectiveGroupOf( pUser->_id.c_str() ) ) )
            return true;
    }
    strReason = "monitor_call (role " + gclsRoleMap.RoleIdForLine( strUserId.c_str() ) + ")";
    return false;
}

std::string CGroupCallService::ListenDenyReason( const CspPttGroup &clsGroup, const std::string &strListener,
                                                 bool *pbUnavailable ) {
    if ( pbUnavailable ) *pbUnavailable = false;
    CspUserProfile clsProf;
    const int iProf = gclsDbManager.SelectUserProfile( strListener.c_str(), clsProf );
    if ( iProf < 0 && pbUnavailable ) *pbUnavailable = true;  // 조회 불능 — 자격 없음과 다르다
    if ( iProf != 1 || !clsProf.m_bAllowAmbientListening )
        return ( iProf < 0 ) ? "profile unavailable" : "allow_ambient_listening=0";
    if ( clsGroup._isAdhoc ) {
        // 즉석 세션(사설콜·애드혹)은 PTT 그룹이 아니라 사람 사이의 세션 — 범위 축은 ptt_listen 이 아닌 참가자
        //   전화 그룹에 대한 관측자 역할 monitor_call(VoLTE 통화 Join 과 같은 규칙, §5.6a).
        std::string strReason;
        if ( !CanObserveEphemeral( clsGroup, strListener, strReason, pbUnavailable ) ) return "ephemeral " + strReason;
        return "";
    }
    if ( !gclsRoleMap.CanListenPtt( strListener.c_str(), clsGroup._id.c_str() ) ) return "ptt_listen scope";
    return "";
}

void CGroupCallService::LeaveGroupOrQueue( const std::string &strGroupId, const std::string &strSessionId,
                                           const std::string &strSesId, const char *pszWhy ) {
    if ( gclsCmpClient.LeaveGroup( strGroupId, strSessionId, strSesId ) ) return;
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    // CMP 멤버 키가 `(group, user)` 하나뿐이라 같은 짝은 한 건으로 접는다 — 두 건이 있어도 걷는 대상은 같다.
    for ( const auto &clsOld : m_vecPendingLeave )
        if ( clsOld.strGroupId == strGroupId && clsOld.strSessionId == strSessionId ) return;
    if ( m_vecPendingLeave.size() >= PENDING_LEAVE_MAX ) {
        CLog::Print( LOG_ERROR, "PTT_LEAVE 대기열 상한(%zu) 초과 — **회수 유실**(group=%s member=%s)",
                     (size_t)PENDING_LEAVE_MAX, strGroupId.c_str(), strSessionId.c_str() );
        return;
    }
    m_vecPendingLeave.push_back( { strGroupId, strSessionId, strSesId, 0, time( NULL ) + 1 } );
    CLog::Print( LOG_ERROR, "PTT_LEAVE 실패(%s, group=%s member=%s) — 재시도 대기 %zu 건", pszWhy ? pszWhy : "?",
                 strGroupId.c_str(), strSessionId.c_str(), m_vecPendingLeave.size() );
}

void CGroupCallService::PurgePendingLeave( const std::string &strGroupId, const std::string &strMemberId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( m_vecPendingLeave.empty() ) return;
    const size_t uBefore = m_vecPendingLeave.size();
    m_vecPendingLeave.erase( std::remove_if( m_vecPendingLeave.begin(), m_vecPendingLeave.end(),
                                             [&]( const PendingLeave &p ) {
                                                 return p.strGroupId == strGroupId && p.strSessionId == strMemberId;
                                             } ),
                             m_vecPendingLeave.end() );
    if ( m_vecPendingLeave.size() != uBefore )
        CLog::Print( LOG_INFO, "PTT_LEAVE 대기 취소 — %s 가 group(%s) 에 다시 합류했다", strMemberId.c_str(),
                     strGroupId.c_str() );
}

void CGroupCallService::RetryPendingLeaves() {
    // **포기하지 않는다.** 여기서 포기하면 자격을 잃은 청취자가 그룹이 해제될 때까지 계속 듣는다. PTT_LEAVE 는
    //   없는 그룹·멤버에도 `OK`(cmp_media_api.md §7.5 — 자연 멱등)라 무한 재시도가 스스로 끝난다 — 그룹이
    //   사라졌으면 CMP 가 OK 로 답하고 대기열에서 빠진다. CMP 불통일 때만 남고, 그때는 간격 상한이 비용을 묶는다.
    static const int SLOW_AFTER = 6;  // 1+2+4+8+16+32초 ≈ 1분 — 이후는 간격 고정 + 희소 로그
    static const int MAX_WAIT = 30;
    static const int MAX_PER_TICK = 4;  // CMP 왕복은 블로킹 — 1초 틱을 넘기지 않게
    std::vector<PendingLeave> vecDue;
    {
        std::lock_guard<std::recursive_mutex> lock( m_mutex );
        if ( m_vecPendingLeave.empty() ) return;
        const time_t tNow = time( NULL );
        for ( auto it = m_vecPendingLeave.begin();
              it != m_vecPendingLeave.end() && (int)vecDue.size() < MAX_PER_TICK; ) {
            if ( it->tNextTry > tNow ) {
                ++it;
                continue;
            }
            vecDue.push_back( *it );
            it = m_vecPendingLeave.erase( it );
        }
    }
    for ( auto &clsItem : vecDue ) {
        // **보내기 직전에 한 번 더 본다.** 대기 중에 그 멤버가 다시 합류했으면 이 LEAVE 는 새 멤버를 걷는다.
        //   Purge 가 주 방어이고 이것은 그 사이에 들어온 합류를 잡는 그물이다.
        {
            std::lock_guard<std::recursive_mutex> lock( m_mutex );
            bool bRejoined = false;
            for ( const auto &kv : m_mapCallSession )
                if ( kv.second.strGroupId == clsItem.strGroupId && kv.second.strMemberId == clsItem.strSessionId ) {
                    bRejoined = true;
                    break;
                }
            if ( bRejoined ) {
                CLog::Print( LOG_INFO, "PTT_LEAVE 재시도 취소 — %s 가 group(%s) 에 다시 합류했다",
                             clsItem.strSessionId.c_str(), clsItem.strGroupId.c_str() );
                continue;
            }
        }
        if ( gclsCmpClient.LeaveGroup( clsItem.strGroupId, clsItem.strSessionId, clsItem.strSesId ) ) {
            CLog::Print( LOG_INFO, "PTT_LEAVE 재시도 성공 (%d회차, group=%s member=%s)", clsItem.iTries + 1,
                         clsItem.strGroupId.c_str(), clsItem.strSessionId.c_str() );
            continue;
        }
        ++clsItem.iTries;
        if ( clsItem.iTries == SLOW_AFTER || ( clsItem.iTries > SLOW_AFTER && clsItem.iTries % 60 == 0 ) )
            CLog::Print( LOG_ERROR, "PTT_LEAVE %d회 실패(group=%s member=%s) — %d초 간격으로 계속 재시도",
                         clsItem.iTries, clsItem.strGroupId.c_str(), clsItem.strSessionId.c_str(), MAX_WAIT );
        const int iWait = clsItem.iTries >= SLOW_AFTER ? MAX_WAIT : ( 1 << clsItem.iTries );
        clsItem.tNextTry = time( NULL ) + iWait;
        std::lock_guard<std::recursive_mutex> lock( m_mutex );
        m_vecPendingLeave.push_back( clsItem );
    }
}

int CGroupCallService::RevokeUnauthorizedListeners( const char *pszWhy ) {
    // 락 안에서는 **스냅샷만** 뜬다 — 판정이 그룹 맵과 DB(프로파일)를 읽고, 집행(BYE)이 OnCallTerminated 로
    //   같은 락에 재진입하기 때문이다.
    struct Snap {
        std::string strCallId, strGroupId, strMember;
    };
    std::vector<Snap> vecSnap;
    {
        std::lock_guard<std::recursive_mutex> lock( m_mutex );
        for ( const auto &kv : m_mapCallSession ) {
            if ( !kv.second.bListenOnly ) continue;
            vecSnap.push_back( { kv.first, kv.second.strGroupId, kv.second.strMemberId } );
        }
    }

    // 판정 — 합류 시(§5.6 ProcessGroupCall)와 같은 2단: 자격 allow_ambient_listening + 범위(즉석 세션은
    //   CanObserveEphemeral, 그 외는 CanListenPtt).
    std::vector<Snap> vecRevoke;
    for ( const auto &clsSnap : vecSnap ) {
        CspPttGroup clsGroup;
        if ( gclsGroupMap.Select( clsSnap.strGroupId.c_str(), clsGroup ) == false )
            continue;  // 그룹이 없으면 별 경로(CheckMemberState)로 정리된다
        bool bUnavail = false;
        const std::string strDeny = ListenDenyReason( clsGroup, clsSnap.strMember, &bUnavail );
        if ( strDeny.empty() ) continue;
        if ( bUnavail ) {
            // **조회 불능은 권한 상실이 아니다.** 새 합류는 막되(fail closed) 이미 선 것은 걷지 않는다
            //   (fail open) — DB 일시 장애로 멀쩡한 청취를 끊으면 장애가 서비스 정지가 된다(§5.10).
            //   «걷지 않는다» 와 «잊는다» 는 다르다 — 빚으로 남겨야 DB 가 복구된 뒤 다시 판정한다.
            CLog::Print( LOG_ERROR, "AuthzRevoke(%s): ptt listen leg(%s) 판정 불능(%s) — 회수 보류",
                         pszWhy ? pszWhy : "authz", clsSnap.strCallId.c_str(), strDeny.c_str() );
            CspAuthz::NotePolicyReloadOwed( pszWhy ? pszWhy : "authz" );
            continue;
        }
        CLog::Print( LOG_INFO, "AuthzRevoke(%s): ptt listen leg(%s) revoked — %s on group %s (%s, role %s)",
                     pszWhy ? pszWhy : "authz", clsSnap.strCallId.c_str(), clsSnap.strMember.c_str(),
                     clsSnap.strGroupId.c_str(), strDeny.c_str(),
                     gclsRoleMap.RoleIdForLine( clsSnap.strMember.c_str() ).c_str() );
        vecRevoke.push_back( clsSnap );
    }

    // 집행 — 판정 사이에 그 leg 이 끝나고 같은 Call-ID 로 다른 leg 이 섰을 수 있다(단말이 주는 값이다).
    //   끊기 직전에 스냅샷과 같은 청취 leg 인지 확인한다.
    int iRevoked = 0;
    for ( const auto &clsSnap : vecRevoke ) {
        {
            std::lock_guard<std::recursive_mutex> lock( m_mutex );
            auto it = m_mapCallSession.find( clsSnap.strCallId );
            if ( it == m_mapCallSession.end() || !it->second.bListenOnly ) continue;  // 그새 끝났다
            if ( it->second.strMemberId != clsSnap.strMember || it->second.strGroupId != clsSnap.strGroupId ) {
                CLog::Print( LOG_INFO, "AuthzRevoke(%s): ptt listen leg(%s) 교체됨 — 회수 건너뜀",
                             pszWhy ? pszWhy : "authz", clsSnap.strCallId.c_str() );
                continue;
            }
        }
        // BYE — 청취자 leg 만. **로컬 StopCall 은 EventCallEnd 를 올리지 않으므로**(psip
        //   `SipUserAgentCall.hpp` — dialog 를 지우고 BYE 만 보낸다) 뒷정리를 직접 태운다. 이것을 빠뜨리면
        //   세션 맵·CMP 청취 멤버(`LeaveGroup`)·감사 `ended` 가 모두 남아 **단말만 끊기고 미디어는 계속
        //   복사된다** — 회수가 성립하지 않는다. `CheckMemberState` 의 강제 종료와 같은 순서다.
        gclsUserAgent.StopCall( clsSnap.strCallId.c_str() );
        if ( OnCallTerminated( clsSnap.strCallId ) ) ++iRevoked;
    }
    return iRevoked;
}

// ── PTT 세션 dialog 이벤트 (RFC 4235 dialog-info, dispatch_center.md §5.6a) ─────────────────────────────
//   관제 앱은 범위 안 사람의 PTT 회선에 Event: dialog 를 구독한다(VoLTE 회선과 같은 패키지·같은 인가 CanWatch).
//   참가 leg 마다 dialog 1건: local = 참가자, remote = 세션 URI(UE 의 대화 상대는 focus), 확장 <mcptt> 로 세션
//   종류(사설콜·애드혹·그룹)·개시자·긴급/임박을 싣는다. 앱은 같은 remote 를 가진 dialog 를 한 세션으로 묶고,
//   참가자 명단이 더 필요하면 그 세션 URI 에 conference 를 구독한다(CheckConferenceSubscribe 즉석 게이트).
std::string CGroupCallService::PttSessionUri( const std::string &strGroupId ) {
    const std::string strDom = gclsServiceMap.GetDomainByKind( "ptt" );
    return "sip:" + strGroupId + ( strDom.empty() ? std::string() : "@" + strDom );
}

std::string CGroupCallService::BuildPttDialogExt( const std::string &strGroupId ) {
    std::string strType;
    CspPttGroup clsGroup;
    if ( gclsGroupMap.Select( strGroupId.c_str(), clsGroup ) ) {
        if ( clsGroup._groupType == "private" )
            strType = "private";
        else if ( clsGroup._isAdhoc )
            strType = "adhoc";
        else
            strType = clsGroup._groupType.empty() ? "prearranged" : clsGroup._groupType;
    } else if ( strGroupId.rfind( "priv-", 0 ) == 0 ) {
        strType = "private";
    } else if ( strGroupId.rfind( "adhoc-", 0 ) == 0 ) {
        strType = "adhoc";
    } else {
        strType = "prearranged";
    }
    const int iCond = GroupConditionOf( strGroupId );
    const GroupSession clsSes = SessionOf( strGroupId );
    std::string s = "<mcptt xmlns=\"urn:cims:xml:ns:dialog-info:mcptt\" session-type=\"" + strType +
                    "\" session-id=\"" + strGroupId + "\"";
    if ( !clsSes.strInitiator.empty() ) s += " initiator=\"" + clsSes.strInitiator + "\"";
    if ( clsSes.bBroadcast ) s += " broadcast=\"true\"";
    s += std::string( " emergency=\"" ) + ( iCond == 2 ? "true" : "false" ) + "\" imminent-peril=\"" +
         ( iCond == 1 ? "true" : "false" ) + "\"/>";
    return s;
}

void CGroupCallService::EmitPttDialog( const PttDialogLeg &leg, const char *pszState ) {
    if ( leg.bListen || leg.strUser.empty() || leg.strCallId.empty() || leg.strGroupId.empty() ) return;
    SendPttDialogEventNotify( leg.strUser, leg.strCallId, pszState ? pszState : "confirmed", leg.bInitiator,
                              PttSessionUri( leg.strGroupId ), BuildPttDialogExt( leg.strGroupId ) );
}

void CGroupCallService::NotifyPttDialog( const std::string &strCallId, const char *pszState ) {
    PttDialogLeg leg;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto it = m_mapCallSession.find( strCallId );
        if ( it == m_mapCallSession.end() ) return;
        leg = _pttLegOf( strCallId, it->second );
    }
    EmitPttDialog( leg, pszState );
}

void CGroupCallService::CollectPttDialogs( const std::string &strAor, std::vector<PttDialogSnapshot> &vecOut ) {
    std::vector<std::pair<PttDialogLeg, bool>> vecLegs;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        for ( const auto &kv : m_mapCallSession ) {
            if ( kv.second.strMemberId != strAor || kv.second.bListenOnly ) continue;
            vecLegs.emplace_back( _pttLegOf( kv.first, kv.second ), kv.second.bEstablished );
        }
    }
    for ( const auto &pr : vecLegs ) {
        PttDialogSnapshot p;
        p.strCallId = pr.first.strCallId;
        p.strSessionUri = PttSessionUri( pr.first.strGroupId );
        p.strExtXml = BuildPttDialogExt( pr.first.strGroupId );
        p.bEstablished = pr.second;
        p.bInitiator = pr.first.bInitiator;
        vecOut.push_back( p );
    }
}

// dispatch_center.md §5.7 — PTT 그룹콜 청취 감사(E-AUD-016 call_monitored, tap_mode=ptt_listen). 시작/종료/거절 각 1건.
//   target_a = PTT 그룹 id, target_b 없음(그룹 세션). 통화 감청(TAS Join)과 같은 이벤트 코드·필드 체계. role = 청취자
//   역할.
void CGroupCallService::EmitPttListenAudit( const char *pszPhase, const std::string &strMonitor,
                                            const std::string &strRole, const std::string &strPttGroup,
                                            const std::string &strSesId, int iDurMs ) {
    if ( !gclsFmReporter.IsEnabled() ) return;
    SimpleJson::JsonNode p;
    p.Set( "phase", pszPhase );
    p.Set( "monitor", strMonitor );
    p.Set( "role", strRole );
    p.Set( "session", strPttGroup );
    if ( !strSesId.empty() ) p.Set( "sesid", strSesId );
    p.Set( "target_a", strPttGroup );
    p.Set( "target_b", "" );
    p.Set( "tap_mode", "ptt_listen" );
    if ( iDurMs >= 0 ) p.Set( "dur_ms", iDurMs );
    gclsFmReporter.SendEvent( "call_monitored", "audit", gclsFmReporter.Node() + "/csp", p );
}

// TS 23.379 — ad-hoc 그룹 생성·해제 감사(E-AUD-010 regroup_changed).
//   ad-hoc 그룹은 ephemeral 이라 통화가 끝나면 GroupMap 에서 사라진다 — 지금까지는
//   **존재했다는 사실 자체가 남지 않아** 사후에 "그때 누구를 묶어 통신했나" 를 답할 수 없었다.
void CGroupCallService::EmitRegroupEvent( const char *pszAction, const std::string &strGroupId, const char *pszScope ) {
    if ( !gclsFmReporter.IsEnabled() ) return;
    SimpleJson::JsonNode p;
    p.Set( "action", pszAction );
    p.Set( "gid", strGroupId );
    p.Set( "scope", pszScope );
    gclsFmReporter.SendEvent( "regroup_changed", "audit", gclsFmReporter.Node() + "/csp", p );
}

// mcptt_emergency_modes.md §4 — 긴급/임박 모드 전이 감사(E-STC-007 emergency_mode_changed).
//   전이 자체는 PttLogEvent 가 세션 이력에 남기지만, 그것은 세션을 열어야 보인다. 운용은
//   "어제 긴급이 몇 번 걸렸나" 를 기간으로 묻는다 — 그래서 이벤트 스트림에도 같이 올린다.
//   등급(iTier)은 **전이 대상이 아니라 그 전이가 말하는 상태** 다: 상향은 새 등급, 취소는 직전 등급.
void CGroupCallService::EmitEmergencyModeEvent( const char *pszAction, int iTier, const std::string &strGroupId,
                                                const std::string &strActor, const std::string &strSesId ) {
    if ( !gclsFmReporter.IsEnabled() ) return;
    SimpleJson::JsonNode p;
    p.Set( "action", pszAction );
    p.Set( "condition", iTier >= 2 ? "emergency" : "imminent-peril" );
    p.Set( "gid", strGroupId );
    p.Set( "uri", strActor );
    p.Set( "tier", iTier );
    if ( !strSesId.empty() ) p.Set( "sesid", strSesId );
    gclsFmReporter.SendEvent( "emergency_mode_changed", "stateChange", gclsFmReporter.Node() + "/csp", p );
}

std::string CGroupCallService::BuildConferenceInfoBody(
    const std::string &strGroupId, const std::string &strChangedUser, const std::string &strStatus,
    const std::string &strJoining, std::vector<std::pair<std::string, std::string>> *pvecLegsOut ) {
    // 1. Collect established legs for this group + bump version
    //    확립 leg(200 OK 수신)만 대상 — 미확립(pending) fan-out 초대는 ①다이얼로그가 없어 NOTIFY 가
    //    성립하지 않고 ②참가자 명단에 실리면 '아직 참여하지 않은 초대 대상'이 참여자로 표시된다.
    std::vector<std::pair<std::string, std::string>> vecLegs;
    // 로스터는 **사용자 단위**다 — RFC 4575 는 참가자당 <user> 하나이고, 그 사람의 단말들은
    //   그 안의 <endpoint> 로 표현한다. leg 단위로 만들면 재조인 과도기에 같은 사용자가 여러 번
    //   실려(실측: 로스터[3] = 001·002·001) 수신측에서 상태가 뒤집힐 수 있다.
    //   vecUsers 는 등장 순서를 보존하고, mapUserLegs 는 그 사용자의 확립 leg 수다 — 2 이상은
    //   재조인 잔존 leg(정리되지 않은 세션)의 징후이므로 경고로 드러낸다.
    std::vector<std::string> vecUsers;
    std::map<std::string, int> mapUserLegs;
    std::set<std::string> setListeners;  // listen_visibility=visible 청취자 — <roles> listener 로 표기
    int iVersion = 0;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto itRtp = m_mapGroupRtp.find( strGroupId );
        if ( itRtp != m_mapGroupRtp.end() ) {
            itRtp->second.iConfVersion++;
            iVersion = itRtp->second.iConfVersion;
        }

        for ( const auto &kv : m_mapCallSession ) {
            if ( kv.second.strGroupId == strGroupId && kv.second.bEstablished ) {
                // leg 목록은 leg 단위 유지 — NOTIFY 는 다이얼로그(leg)마다 보내야 한다.
                vecLegs.push_back( std::make_pair( kv.first, kv.second.strMemberId ) );
                // 은닉 청취 leg 는 로스터에 실리지 않는다(통지는 받는다 — dispatch_center.md §5.6 hidden).
                if ( kv.second.bListenOnly && kv.second.bListenHidden ) continue;
                if ( kv.second.bListenOnly ) setListeners.insert( kv.second.strMemberId );
                if ( mapUserLegs.find( kv.second.strMemberId ) == mapUserLegs.end() )
                    vecUsers.push_back( kv.second.strMemberId );
                mapUserLegs[kv.second.strMemberId]++;
            }
        }
    }
    if ( pvecLegsOut ) *pvecLegsOut = vecLegs;

    // 같은 사용자에 확립 leg 가 둘 이상 = 재조인 과정에서 이전 leg 가 정리되지 않은 상태.
    //   로스터는 사용자 단위로 합쳐 내보내지만, 원인은 세션 정리 쪽이므로 관측 가능하게 남긴다.
    for ( const auto &kv : mapUserLegs ) {
        if ( kv.second > 1 )
            CLog::Print( LOG_INFO,
                         "BuildConferenceInfoBody: Group(%s) user(%s) has %d established legs — "
                         "재조인 잔존 leg 의심",
                         strGroupId.c_str(), kv.first.c_str(), kv.second );
    }

    // 2. Build conference-info+xml body (RFC 4575)
    //    F-09: 참가자 NOTIFY 는 항상 state="full"(변경 반영 후 현재 로스터 스냅샷) — partial 증분은
    //          UDP NOTIFY 유실/늦은 발신 조인 시 수신측 목록이 어긋난 채 남는다. full 은 매 통지가
    //          자가치유이고, 늦은 참여자 본인도 같은 NOTIFY 로 기존 로스터를 얻는다.
    //          이탈자는 로스터에서 이미 빠져 있으므로 deleted 엔트리를 명시 부가한다.
    //    F-10: entity는 sip: URI (tel: → RFC 4575 §5.3 위반)
    std::string strMcpttDomain = gclsServiceMap.GetDomainByKind( "ptt" );
    std::ostringstream oss;

    bool bChangedInRoster = false;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        << "<conference-info xmlns=\"urn:ietf:params:xml:ns:conference-info\"\r\n"
        << "  entity=\"sip:" << strGroupId << "@" << strMcpttDomain << "\"\r\n"
        << "  state=\"full\" version=\"" << iVersion << "\">\r\n"
        << "  <users>\r\n";
    for ( const auto &strMember : vecUsers ) {
        bool bChanged = ( strMember == strChangedUser );
        if ( bChanged ) bChangedInRoster = true;
        // 같은 사용자의 확립 leg 가 둘 이상이면(재조인 과도기의 잔존 leg) 하나로 합친다 —
        //   참가자 상태는 사용자 단위 하나여야 한다. 진짜 멀티 디바이스를 지원하게 되면
        //   여기서 단말별 <endpoint> 로 확장한다.
        oss << "    <user entity=\"sip:" << strMember << "@" << strMcpttDomain << "\" state=\""
            << ( bChanged ? strJoining : "full" ) << "\">\r\n";
        // 공개 청취자(listen_visibility=visible) — RFC 4575 §5.6.3 <roles> 로 "청취 중" 표기 (발언 자격 없음)
        if ( setListeners.count( strMember ) > 0 ) oss << "      <roles><entry>listener</entry></roles>\r\n";
        oss << "      <endpoint entity=\"sip:" << strMember << "@" << strMcpttDomain << "\">\r\n"
            << "        <status>" << ( bChanged ? strStatus : "connected" ) << "</status>\r\n"
            << "      </endpoint>\r\n"
            << "    </user>\r\n";
    }
    // 변경 사용자가 로스터에 없으면 이탈(deleted)이므로 명시 엔트리로 알린다.
    //   단 변경 인자 없는 순수 스냅샷(구독 수락 직후 초기 NOTIFY)에서는 대상이 없으므로
    //   생략한다 — 안 그러면 entity="sip:@domain" 인 빈 참가자가 실려 단말 명단에 유령이 뜬다.
    if ( !bChangedInRoster && !strChangedUser.empty() ) {
        oss << "    <user entity=\"sip:" << strChangedUser << "@" << strMcpttDomain << "\" state=\"" << strJoining
            << "\">\r\n"
            << "      <endpoint entity=\"sip:" << strChangedUser << "@" << strMcpttDomain << "\">\r\n"
            << "        <status>" << strStatus << "</status>\r\n"
            << "      </endpoint>\r\n"
            << "    </user>\r\n";
    }
    oss << "  </users>\r\n"
        << "</conference-info>\r\n";
    return oss.str();
}

void CGroupCallService::SendConferenceNotify( const std::string &strGroupId, const std::string &strChangedUser,
                                              const std::string &strStatus, const std::string &strJoining ) {
    std::vector<std::pair<std::string, std::string>> vecLegs;
    std::string strBody = BuildConferenceInfoBody( strGroupId, strChangedUser, strStatus, strJoining, &vecLegs );

    // 전송 경로는 **멤버 단위**로 갈린다.
    //   ① conference 구독자 → 구독 경로(RFC 4575/6665 정합, 단말이 200 OK 로 응답).
    //   ② 구독 없는 멤버 → 통화 dialog in-dialog NOTIFY 폴백. 구독 미구현 단말(구 APK)은 이 경로로만
    //      참가자 화면이 갱신되며, 그 단말 스택은 usage 없음으로 500 을 응답한다(무해·재전송 중단).
    //   구독자가 하나라도 있으면 폴백 전체를 생략하던 종전 방식은 구·신 APK 혼재 시 구 APK 단말의
    //   명단을 멈추게 한다 — 그래서 구독자 집합을 받아 그 멤버만 폴백에서 제외한다.
    std::set<std::string> setNotified;
    int iSubs = SendConferenceNotifyToSubscribers( strGroupId, strBody, &setNotified );

    int iFallback = 0;
    for ( const auto &leg : vecLegs ) {
        if ( setNotified.count( leg.second ) > 0 ) continue;  // 구독 경로로 이미 통지됨
        gclsUserAgent.SendNotifyWithBody( leg.first.c_str(), "conference", "application", "conference-info+xml",
                                          strBody );
        ++iFallback;
    }

    if ( iSubs == 0 && iFallback == 0 ) return;
    CLog::Print( LOG_INFO,
                 "SendConferenceNotify: Group(%s) User(%s) Status(%s) Joining(%s) → %d subscribers + %d in-dialog",
                 strGroupId.c_str(), strChangedUser.c_str(), strStatus.c_str(), strJoining.c_str(), iSubs, iFallback );
}

/**
 * @brief Build PTT group info XML body per 3GPP TS 24.379 MCPTT spec
 *        Content-Type: application/vnd.3gpp.mcptt-info+xml
 */
std::string CGroupCallService::BuildGroupInfoXml( const CspPttGroup &clsGroup, const std::string &strUserId,
                                                  const std::string &strCallerId, int iCondition,
                                                  const McpttIndicators *pInd, bool bBroadcast ) {
    std::ostringstream oss;

    // session-type = 그룹 종류(prearranged/chat, 즉석 1:1 은 private — TS 24.379 Annex F.1). 일제 통화는 session-type
    // 이
    //   아니라 <broadcast-ind> 로 싣는다(§6.3.3.1 — 수신 단말이 수신 전용·호 종료 규칙을 이 표식으로 안다).
    std::string strSessionType = clsGroup._groupType.empty() ? "prearranged" : clsGroup._groupType;

    // 요소 순서 = mcptt-ParamsType sequence, contentType 요소는 type="Normal" + 자식(TS 24.379 Annex F.1).
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        << "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\""
        << " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
        << "  <mcptt-Params>\r\n"
        << McpttInfoValue( "session-type", strSessionType ) << McpttInfoUri( "mcptt-request-uri", "tel:" + strUserId )
        << McpttInfoUri( "mcptt-calling-user-id", "tel:" + strCallerId )
        << McpttInfoUri( "mcptt-calling-group-id", "tel:" + clsGroup._id );
    // condition 지시자 (TS 24.379) — session-type 과 직교. fan-out·합류 200 OK 는 활성 지시자만, 재광고는 절이 정한
    //   요소(pInd — false 값 포함)를 싣는다.
    if ( pInd ) {
        oss << McpttIndicatorElems( *pInd );
    } else {
        if ( iCondition >= 2 ) oss << McpttInfoBool( "emergency-ind", true );
        if ( iCondition == 1 ) oss << McpttInfoBool( "imminentperil-ind", true );
    }
    if ( bBroadcast ) oss << McpttInfoValue( "broadcast-ind", "true" );
    if ( pInd ) oss << McpttIndicatorOriginatedBy( *pInd );
    oss << "  </mcptt-Params>\r\n"
        << "</mcpttinfo>\r\n";

    return oss.str();
}

std::string CGroupCallService::BuildGroupDescriptor( const CspPttGroup &clsGroup, bool bBroadcast ) {
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
    // 일제 통화 = 이 세션의 속성(그룹 종류와 직교 — mcptt_broadcast_group_call.md §3.1)
    oss << ",\"broadcast\":" << ( bBroadcast ? "true" : "false" );
    // floor 축 (docs/api/cmp_media_api.md §7.7) — 세션 이력이 반이중/전이중·동시 발언 정원을
    //   표시하는 근거. floor_control 은 발신 SDP 협상 결과(private call)라 그룹 컬럼이 아니다.
    oss << ",\"floor_control\":\""
        << ( clsGroup._floorControl.empty() ? "on" : CCallDir::JsonEsc( clsGroup._floorControl ) ) << "\"";
    if ( clsGroup._floorPolicy.empty() )
        oss << ",\"floor_policy\":\"single\"";
    else
        oss << ",\"floor_policy\":\"" << CCallDir::JsonEsc( clsGroup._floorPolicy ) << "\"";
    oss << ",\"max_talkers\":"
        << ( clsGroup._floorPolicy == "multi" ? clsGroup._maxTalkers : ( clsGroup._floorPolicy == "dual" ? 2 : 1 ) );
    oss << ",\"priority\":" << clsGroup._priority;
    oss << ",\"encryption\":" << jbool( clsGroup._encryption );
    oss << ",\"emergency_call\":" << jbool( clsGroup._emergencyCall );
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
 *        멤버 명단(resource-lists)은 싣지 않는다 — 제어 기능이 멤버에게 보내는 INVITE 는 mcptt-info·SDP 만
 *        담고(TS 24.379 §6.3.3.1.2, 부록 A.1.3-7), 명단은 conference 이벤트 패키지(§10.1.3)·GMS 그룹 문서로 준다.
 *        명단을 실으면 본문이 멤버 수에 비례해 UDP 경로 MTU(RFC 3261 §18.1.1)를 넘는다.
 */
void CGroupCallService::WrapMultipartBody( CSipMessage *pclsInvite, const std::string &strGroupXml,
                                           const std::string &strFloorIp, int iFloorPort,
                                           const std::string &strGroupUri, bool bNoFloorCtrl ) {
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
             << "a=floorid:0 mstrm:audio\r\n";
    // floor 없는 세션(private full-duplex)은 fan-out 에도 mc_no_floor_ctrl 을 광고해야
    //   수신 단말이 전이중(마이크 상시)으로 수락한다 (G17 — 협상 결과의 양방향 정합).
    if ( bNoFloorCtrl )
        sdpFloor << "a=fmtp:MCPTT mc_queueing;mc_no_floor_ctrl\r\n";
    else
        sdpFloor << "a=fmtp:MCPTT " << kMemberFloorOfferFmtp << "\r\n";
    if ( !strGroupUri.empty() ) sdpFloor << "a=mcptt-floor-request-uri:" << strGroupUri << "\r\n";  // TS 24.379 §C.3
    strSdp += sdpFloor.str();

    std::ostringstream oss;
    // Part 1: mcptt-info XML (3GPP MCPTT call control)
    oss << "--" << strBoundary << "\r\n"
        << "Content-Type: application/vnd.3gpp.mcptt-info+xml\r\n"
        << "Content-Length: " << strGroupXml.size() << "\r\n"
        << "\r\n"
        << strGroupXml << "\r\n";
    // Part 2: SDP with floor control
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

/**
 * @brief 기존 바디(psip AddSdp 산출 SDP)를 유지한 채 mcptt-info part 를 앞세운 multipart/mixed 로
 *        감싼다 (TS 24.379) — in-call 조건 재광고 re-INVITE·조인 200 OK 동봉용.
 *        WrapMultipartBody 와 달리 SDP 에 floor 라인을 덧붙이지 않는다 (m= 구성은 psip AddSdp 가
 *        dialog local RTP 로 이미 완결 — m= 라인 수가 바뀌면 단말 pjsua 가 미디어 수 불일치로 크래시).
 */
void CGroupCallService::WrapInfoMultipart( CSipMessage *pclsMessage, const std::string &strInfoXml ) {
    if ( pclsMessage == NULL || pclsMessage->m_strBody.empty() || strInfoXml.empty() ) return;

    // boundary 는 랜덤 hex — body 내 "mcptt" 등장과 충돌 방지 (RFC 2046 §5.1.1, WrapMultipartBody 동일)
    struct timespec _ts;
    clock_gettime( CLOCK_REALTIME, &_ts );
    unsigned _uRnd = (unsigned)( _ts.tv_nsec ^ (uintptr_t)pclsMessage );
    char _szBoundary[32];
    snprintf( _szBoundary, sizeof( _szBoundary ), "mcptt_%08x%08x", (unsigned)_ts.tv_sec, _uRnd );
    const std::string strBoundary = _szBoundary;
    std::string strSdp = pclsMessage->m_strBody;

    std::ostringstream oss;
    oss << "--" << strBoundary << "\r\n"
        << "Content-Type: application/vnd.3gpp.mcptt-info+xml\r\n"
        << "Content-Length: " << strInfoXml.size() << "\r\n"
        << "\r\n"
        << strInfoXml << "\r\n";
    oss << "--" << strBoundary << "\r\n"
        << "Content-Type: application/sdp\r\n"
        << "Content-Disposition: render\r\n"
        << "Content-Length: " << strSdp.size() << "\r\n"
        << "\r\n"
        << strSdp << "\r\n";
    oss << "--" << strBoundary << "--\r\n";

    pclsMessage->m_strBody = oss.str();
    pclsMessage->m_iContentLength = (int)pclsMessage->m_strBody.size();
    pclsMessage->m_clsContentType.Set( "multipart", "mixed" );
    pclsMessage->m_clsContentType.InsertParam( "boundary", strBoundary.c_str() );
}

/**
 * @brief 진행 중 세션의 조건 변경을 확립 참여 leg 에 re-INVITE 로 재광고한다 (TS 24.379 §6.3.3.1.6 긴급·§6.3.3.1.10
 * 긴급 해제·§6.3.3.1.15 임박 위험). "each of the other participants"(§10.1.1.4.7 8)d)) — 청취 leg(관제사의 recvonly
 *        합류, dispatch_center.md §5.6)도 참여자다. 은닉 청취도 보낸다 — 은닉은 로스터 노출 규칙이지 청취자 자신에게
 *        알리지 않는 규칙이 아니다.
 *        SDP = 그 leg 에 성립한 미디어 그대로(§6.3.3.1.6 1)·§6.3.3.1.15 2) "as currently established"):
 *          · 제어 기능이 **응답한** leg(개시자·합류·청취 — ProcessGroupCall 의 AcceptCall) = 다이얼로그의 현재 local
 * SDP (포트·코덱·floor m=application·fmtp·SRTP 키·방향 — 청취 leg 는 sendonly)로 offer 를 만든다. · 제어 기능이
 * **오퍼한** 멤버 leg(fan-out InviteMember) = 초기 오퍼와 같은 구성(멤버 CMP 포트·서비스 코덱·그룹 floor 포트) —
 * floor m=application 은 초기 오퍼에서 WrapMultipartBody 가 SDP 에 덧붙여 다이얼로그 상태에
 * 없기 때문이다. SRTP 는 기존 키 그대로 — 키가 같으니 단말·CMP 세션이 유지된다(media_security.md §5.2). 단말 pjsua 의
 * 자동 200 OK 는 psip EventReInviteResponse(CSP 기본 no-op)로 격리된다.
 */
int CGroupCallService::PropagateConditionToMembers( const std::string &strGroupId, int iCond,
                                                    const std::string &strExcludeMemberId,
                                                    const McpttIndicators &clsInd ) {
    CspPttGroup clsGroup;
    if ( gclsGroupMap.Select( strGroupId.c_str(), clsGroup ) == false ) return 0;

    struct Leg {
        std::string strCallId, strMemberId;
        bool bAnswered;  // 제어 기능이 응답한 leg(개시자·합류·청취)
    };
    std::string strActor, strSharedIp;
    int iFloorPort = 0;
    std::vector<Leg> vecLegs;
    {
        std::unique_lock<std::recursive_mutex> lock( m_mutex );
        auto ita = m_mapGroupCond.find( strGroupId );
        if ( ita != m_mapGroupCond.end() ) strActor = ita->second.strInitiator;
        auto itRtp = m_mapGroupRtp.find( strGroupId );
        if ( itRtp != m_mapGroupRtp.end() ) {
            strSharedIp = itRtp->second.strIp;
            iFloorPort = itRtp->second.iFloorPort;
        }
        for ( const auto &kv : m_mapCallSession ) {
            if ( kv.second.strGroupId != strGroupId || !kv.second.bEstablished ) continue;
            if ( kv.second.strMemberId == strExcludeMemberId ) continue;
            vecLegs.push_back( { kv.first, kv.second.strMemberId, kv.second.bInitiator } );
        }
    }
    if ( vecLegs.empty() ) return 0;
    // mcptt-calling-user-id = 상태를 세운 사용자(§6.3.3.1.6 2)·§6.3.3.1.15 3)). 해제로 상태가 지워졌으면 변경을 일으킨
    // 사용자.
    if ( strActor.empty() ) strActor = strExcludeMemberId.empty() ? strGroupId : strExcludeMemberId;
    const bool bBroadcast = SessionOf( strGroupId ).bBroadcast;
    const std::string strRp = gclsCspServiceConfig.ResourcePriorityOf( iCond );  // §6.3.3.1.19

    const CSipCodecEntry &clsSvcCodec = CSipCodecTable::GetTop();
    int iSent = 0;
    for ( const auto &leg : vecLegs ) {
        CSipMessage *pclsReq = NULL;
        if ( leg.bAnswered ) {
            if ( !gclsUserAgent.CreateReInvite( leg.strCallId.c_str(), NULL, &pclsReq ) ) continue;  // 성립 SDP 그대로
            StripInitialOnlyFloorFmtp( pclsReq );  // 로컬 선언 = 개시 answer — 이어지는 offer 규칙(§14.5)
        } else {
            // 멤버 leg — InviteMember 의 초기 오퍼와 같은 구성
            int iAudioPort = 0;
            if ( strSharedIp.empty() || !GetOrAllocMemberPort( strGroupId, leg.strMemberId, iAudioPort ) ) continue;
            CSipCallRtp clsRtp;
            clsRtp.SetIpPort( strSharedIp.c_str(), iAudioPort, SOCKET_COUNT_PER_MEDIA );
            clsRtp.m_clsCodecList.push_back( clsSvcCodec.m_iPt );
            clsRtp.m_iCodec = clsSvcCodec.m_iPt;
            // floor 없는 세션(floor_control=off)은 application 포트 미설정 — AddSdp 가 상대 응답 미러(port 0)로 m= 수를
            //   보존한다. 있으면 초기 오퍼의 fmtp 그대로(WrapMultipartBody).
            if ( clsGroup._floorControl != "off" && iFloorPort > 0 ) {
                clsRtp.m_iApplicationPort = iFloorPort;
                clsRtp.m_strApplicationFmtp = kMemberFloorOfferFmtp;
            }
            CSipCallRtp clsLocalRtp;  // SRTP leg — 기존 키 그대로 (재협상 아님)
            if ( gclsUserAgent.GetLocalCallRtp( leg.strCallId.c_str(), &clsLocalRtp ) &&
                 !clsLocalRtp.m_strLocalCryptoKey.empty() ) {
                clsRtp.m_strLocalCryptoTag = clsLocalRtp.m_strLocalCryptoTag;
                clsRtp.m_strLocalCryptoSuite = clsLocalRtp.m_strLocalCryptoSuite;
                clsRtp.m_strLocalCryptoKey = clsLocalRtp.m_strLocalCryptoKey;
            }
            if ( !gclsUserAgent.CreateReInvite( leg.strCallId.c_str(), &clsRtp, &pclsReq ) ) continue;
        }
        WrapInfoMultipart( pclsReq,
                           BuildGroupInfoXml( clsGroup, leg.strMemberId, strActor, iCond, &clsInd, bBroadcast ) );
        pclsReq->AddHeader( "Resource-Priority", strRp.c_str() );
        if ( gclsUserAgent.m_clsSipStack.SendSipMessage( pclsReq ) ) iSent++;
    }
    CLog::Print( LOG_INFO,
                 "PropagateConditionToMembers: group(%s) cond=%d actor(%s) E=%d A=%d I=%d → %d/%zu legs re-INVITE",
                 strGroupId.c_str(), iCond, strActor.c_str(), clsInd.iEmergency, clsInd.iAlert, clsInd.iImminent, iSent,
                 vecLegs.size() );
    return iSent;
}
