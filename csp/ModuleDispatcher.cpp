/**
 * CModuleDispatcher — CSipServer 를 완전히 대체하는 중앙 디스패처
 *
 * ISipStackCallBack: REGISTER, SUBSCRIBE, Proxy INVITE
 * ISipUserAgentCallBack: B2BUA 호 이벤트 (모듈별 분배)
 *
 * RecvRequest 콜백 순서: [ModuleDispatcher, CSipUserAgent]
 *  → Proxy 대상 INVITE: ModuleDispatcher 직접 처리 (return true)
 *  → B2BUA 대상 INVITE: return false → CSipUserAgent 처리
 */

#include "ModuleDispatcher.h"

#include <cctype>
#include <cstdio>

#include "CallDir.h"
#include "CallMap.h"
#include "CmpClient.h"
#include "CspAclPolicyEngine.h"
#include "CspAddressing.h"
#include "CspLocalNodeMap.h"
#include "CspPendingRouteMap.h"
#include "CspPttGroup.h"
#include "CspRemoteNodeMap.h"
#include "CspRouteMap.h"
#include "CspRoutingPolicyEngine.h"
#include "CspServer.h"
#include "CspServiceMap.h"
#include "CspUser.h"
#include "DbManager.h"
#include "Directory.h"
#include "GroupCallService.h"
#include "GroupMap.h"
#include "McpttInfo.h"
#include "Log.h"
#include "MemoryDebug.h"
#include "NonceMap.h"
#include "RtpMap.h"
#include "SipMd5.h"
#include "SipMessageLogger.h"
#include "SipServerSetup.h"
#include "SipStackThread.h"  // GetCurrentInboundListenerId()
#include "SipUserAgent.h"
#include "SipUtility.h"
#include "SubscriptionManager.h"
#include "TimeString.h"
#include "UserMap.h"

CModuleDispatcher gclsDispatcher;

extern void SendSipNotify( const std::string &uri, const std::string &etag, const std::string &action );
extern void SendInitialNotify( const SubscriptionInfo &sub );

// ── 비정상(스캔/사기) 수신 분류 헬퍼 ───────────────────────────────
// 공개 SIP 포트로 유입되는 인터넷발 스캔/사기 INVITE 를 수신 시점에 탐지·기록하기 위함.
namespace {
// 공인(글로벌) IPv4 여부 — 사설/루프백/링크로컬은 내부로 간주(false).
bool SecIsPublicIp( const std::string &ip ) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if ( sscanf( ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d ) != 4 ) return false;
    if ( a == 10 ) return false;
    if ( a == 172 && b >= 16 && b <= 31 ) return false;
    if ( a == 192 && b == 168 ) return false;
    if ( a == 127 || a == 0 || a >= 224 ) return false;
    if ( a == 169 && b == 254 ) return false;
    return true;
}
// 알려진 SIP 스캐너/공격툴 UA (부분일치, 소문자). pplsip(psip 기본 UA)은 미등록 발신일 때만 의미.
bool SecIsScannerUa( const std::string &ua ) {
    std::string l;
    l.reserve( ua.size() );
    for ( char ch : ua ) l += (char)tolower( (unsigned char)ch );
    static const char *kUa[] = { "pplsip", "friendly-scanner", "sipvicious", "sipcli", "sundayddr",
                                 "vaxsipuseragent", "sip-scan", "sipsak", "sippts", "sundayddr", 0 };
    for ( int i = 0; kUa[i]; ++i )
        if ( l.find( kUa[i] ) != std::string::npos ) return true;
    return false;
}
// 사기성 번호: E.164 최대(15) 초과, 또는 0/9 의 9자리+ 비정상 반복(스캐너 enumeration).
bool SecIsFraudNumber( const std::string &num ) {
    std::string digits;
    for ( char ch : num )
        if ( isdigit( (unsigned char)ch ) ) digits += ch;
    if ( digits.size() > 15 ) return true;
    int run = 1;
    for ( size_t i = 1; i < digits.size(); ++i ) {
        if ( digits[i] == digits[i - 1] && ( digits[i] == '0' || digits[i] == '9' ) ) {
            if ( ++run >= 9 ) return true;
        } else {
            run = 1;
        }
    }
    return false;
}
}  // namespace

// ──────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ──────────────────────────────────────────────────────────────

CModuleDispatcher::CModuleDispatcher() {
}
CModuleDispatcher::~CModuleDispatcher() {
}

// ──────────────────────────────────────────────────────────────
//  Start — 콜백 순서: [ModuleDispatcher, CSipUserAgent]
// ──────────────────────────────────────────────────────────────

void CModuleDispatcher::InitModules() {
    CLog::Print( LOG_SYSTEM, "ModuleDispatcher: Roles CSCF=%s TAS=%s PTT-AS=%s IBCF=%s",
                 m_clsCscf.IsEnabled() ? "ON" : "OFF", m_clsTas.IsEnabled() ? "ON" : "OFF",
                 m_clsPttAs.IsEnabled() ? "ON" : "OFF", m_clsIbcf.IsEnabled() ? "ON" : "OFF" );
}

bool CModuleDispatcher::Start( CSipStackSetup &clsSetup ) {
    // G10 (2026-04-23): SipServerMap (legacy IBCF XML) 제거. routing_policies/routes/
    //   remote_nodes 체계가 SOT. REGISTER_TO_REMOTE 는 별도 워커로 이관 예정.

    // UserAgent 시작 (내부적으로 CSipStack 시작 + UserAgent 를 콜백 등록)
    if ( gclsUserAgent.Start( clsSetup, this, this ) == false ) return false;

    // 콜백 순서를 [ModuleDispatcher, CSipUserAgent] 로 재배치
    // → Proxy INVITE 를 ModuleDispatcher 가 먼저 가로챌 수 있음
    gclsUserAgent.m_clsSipStack.DeleteCallBack( &gclsUserAgent );
    gclsUserAgent.m_clsSipStack.AddCallBack( this );
    gclsUserAgent.m_clsSipStack.AddCallBack( &gclsUserAgent );

    InitModules();
    return true;
}

// ──────────────────────────────────────────────────────────────
//  Call ownership tracking
// ──────────────────────────────────────────────────────────────

void CModuleDispatcher::SetCallOwner( const char *pszCallId, IModule *pModule ) {
    m_clsOwnerMutex.acquire();
    m_mapCallOwner[pszCallId] = pModule;
    m_clsOwnerMutex.release();
}

IModule *CModuleDispatcher::GetCallOwner( const char *pszCallId ) {
    IModule *pOwner = NULL;
    m_clsOwnerMutex.acquire();
    auto it = m_mapCallOwner.find( pszCallId );
    if ( it != m_mapCallOwner.end() ) pOwner = it->second;
    m_clsOwnerMutex.release();
    return pOwner;
}

void CModuleDispatcher::RemoveCallOwner( const char *pszCallId ) {
    m_clsOwnerMutex.acquire();
    m_mapCallOwner.erase( pszCallId );
    m_clsOwnerMutex.release();
}

// ──────────────────────────────────────────────────────────────
//  Proxy call tracking
// ──────────────────────────────────────────────────────────────

void CModuleDispatcher::SetProxyCall( const std::string &strCallId, const ProxyCallInfo &info ) {
    m_clsProxyMutex.acquire();
    m_mapProxyCall[strCallId] = info;
    m_clsProxyMutex.release();
}

bool CModuleDispatcher::GetProxyCall( const std::string &strCallId, ProxyCallInfo &info ) {
    bool bFound = false;
    m_clsProxyMutex.acquire();
    auto it = m_mapProxyCall.find( strCallId );
    if ( it != m_mapProxyCall.end() ) {
        info = it->second;
        bFound = true;
    }
    m_clsProxyMutex.release();
    return bFound;
}

void CModuleDispatcher::RemoveProxyCall( const std::string &strCallId ) {
    m_clsProxyMutex.acquire();
    m_mapProxyCall.erase( strCallId );
    m_clsProxyMutex.release();
}

// ──────────────────────────────────────────────────────────────
//  Shared helpers
// ──────────────────────────────────────────────────────────────

bool CModuleDispatcher::SendResponse( CSipMessage *pclsMessage, int iStatusCode ) {
    CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( iStatusCode );
    if ( pclsResponse == NULL ) return false;

    gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
    return true;
}

void CModuleDispatcher::StopCall( const char *pszCallId, int iResponseCode ) {
    CLog::Print( LOG_DEBUG, "StopCall: CallId=%s Code=%d", pszCallId, iResponseCode );
    OnCallEnded( pszCallId, iResponseCode );
    gclsUserAgent.StopCall( pszCallId, iResponseCode );
}

void CModuleDispatcher::OnCallEnded( const char *pszCallId, int iSipStatus ) {
    // 기존 CDR CSV 파일은 제거됨 (service_log 로 대체). DB + service_log 종료 기록만 남김.
    CSipCdr clsCdr;
    if ( !gclsUserAgent.GetCdr( pszCallId, &clsCdr ) ) return;

    if ( gclsDbManager.IsConnected() ) {
        time_t tAnswer = clsCdr.m_sttStartTime.tv_sec;
        time_t tEnd = clsCdr.m_sttEndTime.tv_sec ? clsCdr.m_sttEndTime.tv_sec : time( nullptr );
        gclsDbManager.UpdateCallLogEnded( clsCdr.m_strCallId, tAnswer, tEnd, iSipStatus );
    }
    if ( gclsCallDir.IsEnabled() ) {
        int dur = (int)( clsCdr.m_sttEndTime.tv_sec - clsCdr.m_sttStartTime.tv_sec );
        gclsCallDir.VoipCallEnd( clsCdr.m_strCallId, "normal", dur > 0 ? dur : 0 );
    }
}

// (Proxy 모드 제거됨 — 모든 VoIP INVITE는 B2BUA + CMP 경유)

// ──────────────────────────────────────────────────────────────
//  ISipStackCallBack — RecvRequest
//  순서: ModuleDispatcher (1st) → CSipUserAgent (2nd)
// ──────────────────────────────────────────────────────────────

bool CModuleDispatcher::RecvRequest( int iThreadId, CSipMessage *pclsMessage ) {
    std::string strCallId;
    pclsMessage->GetCallId( strCallId );

    // v3 (2026-04-22): 접근제어 — AclPolicyEngine (rule_set 기반).
    //   psip v3 확장으로 수신 listener 식별 가능 → scope=local_node 동작.
    //   scope=route/route_set 는 inbound 시점 unknown (outbound 결정 이후에 적용 가능).
    std::string strLocalNodeName;
    if ( pclsMessage->m_iListenerId > 0 ) {
        LocalNodeInfo ln = gclsLocalNodeMap.GetByIntId( pclsMessage->m_iListenerId );
        if ( ln.IsValid() ) strLocalNodeName = ln.name;
    }
    {
        MessageCtx mctx;
        mctx.from_uri_host = pclsMessage->m_clsFrom.m_clsUri.m_strHost;
        mctx.from_uri_user = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
        mctx.to_uri_host = pclsMessage->m_clsTo.m_clsUri.m_strHost;
        mctx.to_uri_user = pclsMessage->m_clsTo.m_clsUri.m_strUser;
        mctx.req_uri_host = pclsMessage->m_clsReqUri.m_strHost;
        mctx.req_uri_user = pclsMessage->m_clsReqUri.m_strUser;
        mctx.src_ip = pclsMessage->m_strClientIp;
        mctx.user_agent = pclsMessage->m_strUserAgent;
        mctx.method = pclsMessage->m_strSipMethod;
        AclDecision d = gclsAclPolicyEngine.Check( mctx, strLocalNodeName, "", "" );
        if ( !d.allowed ) {
            CLog::Print( LOG_INFO, "AclPolicy: denied src=%s local_node=%s policy=%s",
                         pclsMessage->m_strClientIp.c_str(), strLocalNodeName.c_str(), d.matched_policy.c_str() );
            SendResponse( pclsMessage, 403 );
            return true;
        }
    }

    // OPTIONS → 표준 200 OK 자동 응답 (RFC 3261 §11.2).
    //   트렁크 헬스체크(상대 CSP/Kamailio 등)용. 본 프로세스의 capability 를 간소히 알림.
    if ( pclsMessage->IsMethod( SIP_METHOD_OPTIONS ) ) {
        CSipMessage *pclsResp = pclsMessage->CreateResponse( SIP_OK );
        if ( pclsResp ) {
            pclsResp->AddHeader( "Allow",
                                 "INVITE, ACK, CANCEL, BYE, OPTIONS, REGISTER, SUBSCRIBE, NOTIFY, MESSAGE, REFER" );
            gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResp );
        }
        return true;
    }

    // REGISTER, SUBSCRIBE → CSCF 모듈
    if ( m_clsCscf.IsEnabled() && m_clsCscf.OnSipRequest( iThreadId, pclsMessage ) ) {
        return true;
    }

    // INVITE → Proxy 가능 여부 판단
    if ( pclsMessage->IsMethod( SIP_METHOD_INVITE ) ) {
        std::string strTo = pclsMessage->m_clsTo.m_clsUri.m_strUser;
        std::string strFrom = pclsMessage->m_clsFrom.m_clsUri.m_strUser;

        // MCPTT 진행 중 호의 condition 변경(re-INVITE 업그레이드/취소, TS 24.379) 엿보기 — 순수 side-effect.
        //   초기 INVITE 는 아직 세션맵 미등록 → 미발동(초기 긴급은 EventIncomingCall 경로가 처리).
        //   재-INVITE(in-dialog, 동일 Call-ID)만 활성 그룹콜로 매칭되어 floor tier 갱신. 흐름은 그대로 진행.
        {
            std::string strGid, strMid;
            if ( gclsGroupCallService.GetGroupCallSession( strCallId, strGid, strMid ) ) {
                CMcpttInfo clsMi = ParseMcpttInfo( pclsMessage->m_strBody );
                gclsGroupCallService.ApplyInCallCondition( strGid, strMid, clsMi.Condition() );
            }
        }

        // 비정상(스캔/사기) INVITE 탐지·기록 — 미등록 발신 + (공인IP|사기번호|스캐너UA).
        //   정상 가입자(REGISTER 인증완료) 발신은 등록캐시에 있어 제외 → 오탐 없음.
        //   기록만 수행(차단/응답은 기존 ACL/Routing/CSCF 로직에 위임).
        {
            CspUser clsSecUser;
            if ( !gclsCspUserMap.isAlive( strFrom, clsSecUser ) ) {
                bool bPub = SecIsPublicIp( pclsMessage->m_strClientIp );
                bool bFraud = SecIsFraudNumber( strFrom ) || SecIsFraudNumber( strTo );
                bool bScan = SecIsScannerUa( pclsMessage->m_strUserAgent );
                if ( bPub || bFraud || bScan ) {
                    std::string strReasons;
                    if ( bPub ) strReasons += "external_ip,";
                    if ( bScan ) strReasons += "scanner_ua,";
                    if ( bFraud ) strReasons += "fraud_number,";
                    if ( !strReasons.empty() ) strReasons.pop_back();
                    char szPeer[80];
                    snprintf( szPeer, sizeof( szPeer ), "%s:%d",
                              pclsMessage->m_strClientIp.c_str(), pclsMessage->m_iClientPort );
                    gclsSipLogger.LogSecurity( szPeer, "INVITE", strFrom.c_str(), strTo.c_str(),
                                               pclsMessage->m_strUserAgent.c_str(), strCallId.c_str(),
                                               strReasons.c_str(), false );
                    CLog::Print( LOG_INFO,
                                 "SECURITY abnormal INVITE src=%s from=%s to=%s ua=%s reasons=%s",
                                 pclsMessage->m_strClientIp.c_str(), strFrom.c_str(), strTo.c_str(),
                                 pclsMessage->m_strUserAgent.c_str(), strReasons.c_str() );
                }
            }
        }

        // PTT 그룹 → B2BUA (return false → UserAgent 처리)
        if ( gclsGroupMap.Contains( strTo.c_str() ) ) {
            return false;
        }

        // v3 (2026-04-22): routing_policies 평가 — REJECT 즉시 반영.
        //   route_set/access_service 분기는 후속 스테이지에서 배선 (현재는 로그만).
        {
            MessageCtx mctx;
            mctx.from_uri_host = pclsMessage->m_clsFrom.m_clsUri.m_strHost;
            mctx.from_uri_user = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
            mctx.to_uri_host = pclsMessage->m_clsTo.m_clsUri.m_strHost;
            mctx.to_uri_user = pclsMessage->m_clsTo.m_clsUri.m_strUser;
            mctx.req_uri_host = pclsMessage->m_clsReqUri.m_strHost;
            mctx.req_uri_user = pclsMessage->m_clsReqUri.m_strUser;
            mctx.src_ip = pclsMessage->m_strClientIp;
            mctx.user_agent = pclsMessage->m_strUserAgent;
            mctx.method = pclsMessage->m_strSipMethod;
            std::string hashKey = mctx.from_uri_user + "@" + mctx.from_uri_host;
            RoutingDecision rd = gclsRoutingPolicyEngine.Decide( mctx, hashKey );
            if ( rd.type == ROUTING_REJECT ) {
                CLog::Print( LOG_INFO, "RoutingPolicyEngine: reject policy='%s' reason='%s'", rd.matched_policy.c_str(),
                             rd.reason.c_str() );
                SendResponse( pclsMessage, 403 );
                return true;
            }
            if ( rd.type == ROUTING_ROUTE_SET ) {
                // G1 (2026-04-23): picked_route → RouteConfig → RemoteNode 정보를 PendingRouteMap 에
                //   Call-ID 로 저장. CSipUserAgent 가 dialog 를 만들어 EventIncomingCall 을 호출하면
                //   거기서 Take() 로 꺼내 B2BUA B-leg peer 로 사용한다.
                //   (직전 구현의 AddRoute()→return false 경로는 B-leg 메시지에 carry-over 되지 않아 무효였음.)
                RouteConfig rc = gclsRouteMap.GetByName( rd.picked_route );
                if ( rc.IsValid() ) {
                    RemoteNodeInfo rn = gclsRemoteNodeMap.GetByName( rc.remote_node_ref );
                    if ( rn.IsValid() && !rn.ip.empty() && rn.port > 0 ) {
                        PendingRouteEntry pe;
                        pe.remote_ip = rn.ip;
                        pe.remote_port = rn.port;
                        pe.protocol = rn.protocol;
                        pe.route_name = rd.picked_route;
                        pe.route_set = rd.target_name;
                        pe.policy_name = rd.matched_policy;
                        pe.local_node_ref = rc.local_node_ref;  // outbound leg 자기 주소 결정용
                        gclsPendingRouteMap.Insert( strCallId, pe );
                        CLog::Print( LOG_SYSTEM,
                                     "RoutingPolicyEngine: policy='%s' route_set='%s' picked_route='%s' → RemoteNode "
                                     "%s (%s:%d %s) [pending callId=%s]",
                                     rd.matched_policy.c_str(), rd.target_name.c_str(), rd.picked_route.c_str(),
                                     rn.name.c_str(), rn.ip.c_str(), rn.port, rn.protocol.c_str(), strCallId.c_str() );
                    } else {
                        CLog::Print(
                            LOG_ERROR,
                            "RoutingPolicyEngine: picked_route='%s' remote_node_ref='%s' 조회 실패 — legacy fallback",
                            rd.picked_route.c_str(), rc.remote_node_ref.c_str() );
                    }
                } else {
                    CLog::Print( LOG_ERROR, "RoutingPolicyEngine: picked_route='%s' Route 조회 실패 — legacy fallback",
                                 rd.picked_route.c_str() );
                }
            } else if ( rd.type == ROUTING_ACCESS_SERVICE ) {
                // ACCESS_SERVICE target 은 UE 에게 라우팅 (TAS/B2BUA 레거시 경로가 처리).
                //   명시적 분기 없이 legacy TAS 판단 로직(DND/reject)으로 진행 → 로그만.
                CLog::Print( LOG_INFO, "RoutingPolicyEngine: match policy='%s' access_service='%s' (legacy TAS path)",
                             rd.matched_policy.c_str(), rd.target_name.c_str() );
            }
        }

        // G1/G8/G10 (2026-04-23): 외부 peer routing 은 routing_policies 매칭 시 PendingRouteMap
        //   경유로 결정. 여기까지 도달한 INVITE 는 내부 B2BUA 처리 대상 (CSipUserAgent 위임).

        // TAS 서비스 판단: DND, 착신거부
        CspUser clsToUser;
        if ( gclsCspUserMap.isAlive( strTo.c_str(), clsToUser ) ) {
            if ( clsToUser.isDnd() || clsToUser.isReject( strFrom.c_str() ) ) {
                CLog::Print( LOG_INFO, "CSCF: Rejected by TAS (DND/Reject) From=%s To=%s", strFrom.c_str(),
                             strTo.c_str() );
                SendResponse( pclsMessage, SIP_DECLINE );
                return true;
            }

            // 서비스 모드 체크
            const std::string &mode = gclsSetup.m_strServiceMode;
            if ( mode == "ptt" ) {
                SendResponse( pclsMessage, SIP_FORBIDDEN );
                return true;
            }
        }

        // 모든 VoIP INVITE → B2BUA (CMP 경유)
        return false;
    }

    return false;
}

bool CModuleDispatcher::RecvResponse( int iThreadId, CSipMessage *pclsMessage ) {
    // v3 (2026-04-22): OPTIONS 헬스체크는 RouteSet 의 health_check 가 담당하도록 이관 예정.
    //   현 스테이지는 헬스체크 송신/수신 자체를 아직 구현 안함.
    (void)iThreadId;
    (void)pclsMessage;
    return false;
}

bool CModuleDispatcher::SendTimeout( int iThreadId, CSipMessage *pclsMessage ) {
    return false;
}

// ──────────────────────────────────────────────────────────────
//  ISipStackSecurityCallBack
// ──────────────────────────────────────────────────────────────

bool CModuleDispatcher::IsAllowUserAgent( const char *pszSipUserAgent ) {
    return gclsSetup.IsAllowUserAgent( pszSipUserAgent );
}

bool CModuleDispatcher::IsDenyUserAgent( const char *pszSipUserAgent ) {
    return gclsSetup.IsDenyUserAgent( pszSipUserAgent );
}

bool CModuleDispatcher::IsAllowIp( const char *pszIp ) {
    return true;
}
bool CModuleDispatcher::IsDenyIp( const char *pszIp ) {
    return false;
}

// ──────────────────────────────────────────────────────────────
//  ISipUserAgentCallBack — B2BUA 이벤트
//  (CSipUserAgent 가 return false 된 INVITE 를 B2BUA 처리 후 호출)
// ──────────────────────────────────────────────────────────────

void CModuleDispatcher::EventRegister( CSipServerInfo *pclsInfo, int iStatus ) {
    // G10 (2026-04-23): IBCF XML 기반 outbound REGISTER 상태 업데이트 제거.
    //   routes.register_to_remote 워커가 이관 예정 (현재 미구현).
    (void)pclsInfo;
    (void)iStatus;
}

bool CModuleDispatcher::EventIncomingRequestAuth( CSipMessage *pclsMessage ) {
    std::string strIp;
    int iPort;
    CUserInfo clsUserInfo;

    if ( pclsMessage->GetTopViaIpPort( strIp, iPort ) == false ) {
        CLog::Print( LOG_ERROR, "EventIncomingRequestAuth - GetTopViaIpPort error" );
        SendResponse( pclsMessage, SIP_BAD_REQUEST );
        return false;
    }

    // G10 (2026-05-11): 외부 peer inbound auth-skip.
    //   RecvRequest 의 AclPolicy 가 inbound trust 를 이미 검증한 뒤
    //   routing_policies 가 outbound peer 를 결정하여 PendingRouteMap 에 저장한 콜은,
    //   From-user 가 로컬 user map 에 없는 외부 peer 발신이므로 401 challenge 우회.
    {
        std::string strCallId;
        pclsMessage->GetCallId( strCallId );
        if ( gclsPendingRouteMap.Has( strCallId ) ) return true;
    }

    CspUser clsCspUser;
    bool bCspUserFound = gclsCspUserMap.Select( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsCspUser );

    if ( gclsUserMap.Select( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsUserInfo ) == false &&
         !bCspUserFound ) {
        if ( pclsMessage->IsMethod( SIP_METHOD_BYE ) ) {
            std::string strCallId;
            pclsMessage->GetCallId( strCallId );
            if ( gclsCallMap.Select( strCallId.c_str() ) ) return true;
        }

        if ( CCscfModule::CheckAuthrization( pclsMessage ) == false ) return false;

        if ( gclsUserMap.Select( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsUserInfo ) == false &&
             !gclsCspUserMap.Select( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsCspUser ) ) {
            return false;
        }
    }

    if ( strcmp( clsUserInfo.m_strIp.c_str(), strIp.c_str() ) || clsUserInfo.m_iPort != iPort ) {
        if ( CCscfModule::CheckAuthrization( pclsMessage ) == false ) return false;
        gclsUserMap.SetIpPort( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), strIp.c_str(), iPort );
    }

    return true;
}

void CModuleDispatcher::EventIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                           CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
    CLog::Print( LOG_DEBUG, "EventIncomingCall: CallId=%s From=%s To=%s", pszCallId, pszFrom, pszTo );
    CspUser clsUser;
    CUserInfo clsUserInfo;
    bool bRoutePrefix = false;
    std::string strTo;

    if ( strlen( pszTo ) == 0 ) return StopCall( pszCallId, SIP_DECLINE );

    // MCPTT condition(emergency/imminent) 파싱 — INVITE 의 mcptt-info+xml 지시자 (TS 24.379).
    //   session-type(그룹유형)과 직교. ProcessGroupCall 로 전달해 floor tier·fan-out 광고에 반영.
    int iMcpttCond = 0;
    if ( pclsMessage ) {
        CMcpttInfo clsMi = ParseMcpttInfo( pclsMessage->m_strBody );
        iMcpttCond = clsMi.Condition();
    }

    // 1. PTT-AS: 그룹콜 (MCPTT 규격 on-demand) — UE 발신 그룹 INVITE 를 받아 fan-out.
    //   구 always-on 모델은 여기서 403 거부했으나(발신 안 한다는 전제), 규격 모델에선
    //   발신 UE 의 키업(그룹 INVITE)이 세션 개시 트리거다 → ProcessGroupCall 로 라우팅.
    if ( m_clsPttAs.IsEnabled() ) {
        // 신규 그룹(GROUP_CHANGED notify 미수신/지연)으로 in-memory 캐시 미스 시 DB lazy-load 후
        //   재확인 — 그룹 생성 직후 재기동 없이 즉시 발신 가능. (notify 경로와 독립적 안전망.)
        //
        // ⚠️ 부하시험(2026-06-06)서 발견: VoLTE 1:1 호의 착신(일반 가입자 MSISDN)은 그룹이 아니라
        //   항상 cache miss → 그대로 두면 매 INVITE 마다 LoadAllGroups(전체 그룹 DB 재로드, SelectGroup×N
        //   + 맵 Clear/재구축)가 일어나 SIP 수신스레드를 블록 → 소켓 버퍼 overflow·호 실패(408). 그래서:
        //   (1) 착신이 '등록된 가입자' 면 1:1 호이므로 DB 조회 자체를 생략(그룹 아님 — 폭풍 원천 차단).
        //   (2) 미등록 타겟(신규 그룹일 수 있음)만 전체가 아니라 '해당 id 단건' 만 DB 조회·로드.
        if ( !gclsGroupMap.Contains( pszTo ) && gclsDbManager.IsConnected() ) {
            CspUser clsToUser;
            bool bToIsRegisteredUser = gclsCspUserMap.isAlive( pszTo, clsToUser );
            if ( !bToIsRegisteredUser ) {
                if ( gclsGroupMap.LoadOneFromDb( pszTo ) ) {
                    CLog::Print( LOG_INFO, "EventIncomingCall: group(%s) lazy-loaded from DB (single)", pszTo );
                }
            }
        }
    }
    if ( m_clsPttAs.IsEnabled() && gclsGroupMap.Contains( pszTo ) ) {
        SetCallOwner( pszCallId, &m_clsPttAs );
        CSipCallRoute clsGroupRoute;
        clsUserInfo.GetCallRoute( clsGroupRoute );
        if ( gclsGroupCallService.ProcessGroupCall( pszTo, pszFrom, pszCallId, pclsRtp, &clsGroupRoute, iMcpttCond ) ) {
            return;
        }
        CLog::Print( LOG_INFO, "EventIncomingCall: ProcessGroupCall(%s) failed for caller(%s) → 403 [PTT-AS]", pszTo,
                     pszFrom );
        return StopCall( pszCallId, SIP_FORBIDDEN );
    }

    // 서비스 모드 체크
    {
        CspUser clsFromUser;
        bool bFromKnown = gclsCspUserMap.isAlive( pszFrom, clsFromUser );
        const std::string &mode = gclsSetup.m_strServiceMode;
        if ( mode == "ptt" ) return StopCall( pszCallId, SIP_FORBIDDEN );
        if ( bFromKnown && !clsFromUser.m_strServiceType.empty() && clsFromUser.m_strServiceType == "ptt" )
            return StopCall( pszCallId, SIP_FORBIDDEN );
    }

    // G1 (2026-04-23): Routing policy 결정 (RecvRequest 에서 PendingRouteMap 에 넣어둔 것) 을 먼저 소비.
    //   있으면 callee 가 내부 가입자여도 외부 peer 로 B2BUA forward (Routing policy 가 우선).
    //   없으면 아래 내부 가입자 경로 (PTT 그룹 / legacy IBCF / TAS) 로 진행.
    //   Route 의 auth_user/password 는 Route map 재조회로 보강 (RemoteNode 에는 auth 정보 없음).
    bool v3Routed = false;
    // T3: route 결정으로 결정된 outbound leg 의 자기 주소 (Via/Contact 자기 IP/Port) hint.
    //     EventIncomingCall 이 CreateCall 호출 전에 clsRoute 에 채워서 dialog 까지 전달.
    std::string strOutboundLocalIp;
    int iOutboundLocalPort = -1;
    {
        PendingRouteEntry pe;
        std::string strCallIdKey = pszCallId ? pszCallId : "";
        if ( !strCallIdKey.empty() && gclsPendingRouteMap.Take( strCallIdKey, pe ) ) {
            RouteConfig rc = gclsRouteMap.GetByName( pe.route_name );
            if ( !rc.auth_user.empty() ) {
                clsUser.m_strId = rc.auth_user;
                clsUser.m_strPassWord = rc.auth_password;
                pszFrom = clsUser.m_strId.c_str();
            }
            clsUserInfo.m_strIp = pe.remote_ip;
            clsUserInfo.m_iPort = pe.remote_port;
            clsUserInfo.m_eTransport = ( pe.protocol == "TCP" )   ? E_SIP_TCP
                                       : ( pe.protocol == "TLS" ) ? E_SIP_TLS
                                                                  : E_SIP_UDP;
            bRoutePrefix = true;
            SetCallOwner( pszCallId, &m_clsIbcf );
            v3Routed = true;
            // T3: local_node_ref → bind_ip/bind_port 추출. 미정 또는 dangling 시 fallback 으로 진행.
            if ( !pe.local_node_ref.empty() ) {
                LocalNodeInfo ln = gclsLocalNodeMap.GetByName( pe.local_node_ref );
                if ( ln.IsValid() ) {
                    strOutboundLocalIp =
                        ( ln.bind_ip.empty() || ln.bind_ip == "0.0.0.0" ) ? gclsSetup.m_strLocalIp : ln.bind_ip;
                    iOutboundLocalPort = ln.bind_port;
                } else {
                    CLog::Print( LOG_INFO,
                                 "RoutingPolicyEngine: local_node_ref='%s' 조회 실패 — primary fallback [callId=%s]",
                                 pe.local_node_ref.c_str(), pszCallId ? pszCallId : "" );
                }
            }
            CLog::Print( LOG_SYSTEM,
                         "RoutingPolicyEngine: outbound via route_set='%s' route='%s' policy='%s' → %s:%d/%s "
                         "src=%s:%d [callId=%s]",
                         pe.route_set.c_str(), pe.route_name.c_str(), pe.policy_name.c_str(), pe.remote_ip.c_str(),
                         pe.remote_port, pe.protocol.c_str(),
                         strOutboundLocalIp.empty() ? gclsSetup.m_strLocalIp.c_str() : strOutboundLocalIp.c_str(),
                         iOutboundLocalPort > 0 ? iOutboundLocalPort : gclsSetup.m_iUdpPort,
                         pszCallId ? pszCallId : "" );
        }
    }

    if ( !v3Routed && gclsCspUserMap.isAlive( pszTo, clsUser ) == false ) {
        CspPttGroup clsGroup;
        if ( m_clsPttAs.IsEnabled() && gclsGroupMap.Select( pszTo, clsGroup ) ) {
            CSipCallRoute clsRouteTemp;
            clsUserInfo.GetCallRoute( clsRouteTemp );
            if ( gclsGroupCallService.ProcessGroupCall( pszTo, pszFrom, pszCallId, pclsRtp, &clsRouteTemp, iMcpttCond ) ) {
                SetCallOwner( pszCallId, &m_clsPttAs );
                return;
            }
        }

        // G10 (2026-04-23): 레거시 IBCF XML trunk (SipServerMap) 경로 제거.
        //   외부 peer 라우팅은 routing_policies + PendingRouteMap (G1) 으로 결정.
        //   여기까지 도달한 "내부에 없는 callee" 는 CallPickup 외에는 NOT_FOUND.
        if ( gclsSetup.IsCallPickupId( pszTo ) ) {
            SetCallOwner( pszCallId, &m_clsTas );
            return PickUp( pszCallId, pszFrom, pszTo, pclsRtp );
        }
        return StopCall( pszCallId, SIP_NOT_FOUND );
    }

    if ( GetCallOwner( pszCallId ) == NULL ) SetCallOwner( pszCallId, &m_clsTas );

    // TAS: DND/착신전환/착신거부
    if ( clsUser.isDnd() || clsUser.isReject( pszFrom ) ) return StopCall( pszCallId, SIP_DECLINE );

    if ( clsUser.isCallForward() ) {
        CSipMessage *pclsInvite = gclsUserAgent.DeleteIncomingCall( pszCallId );
        if ( pclsInvite ) {
            CSipMessage *pclsResponse = pclsInvite->CreateResponseWithToTag( SIP_MOVED_TEMPORARILY );
            if ( pclsResponse ) {
                CSipFrom clsContact;
                clsContact.m_clsUri.m_strProtocol = SIP_PROTOCOL;
                clsContact.m_clsUri.m_strUser = clsUser.m_strForward;
                // T4: 302 Moved Temporarily 는 수신 listener 의 bind_ip:bind_port 로 Contact 생성.
                const int iListenerId = GetCurrentInboundListenerId();
                clsContact.m_clsUri.m_strHost = CspAddressing::GetLocalSipAddress( iListenerId );
                clsContact.m_clsUri.m_iPort = CspAddressing::GetLocalSipPort( iListenerId, gclsSetup.m_iUdpPort );
                pclsResponse->m_clsContactList.push_back( clsContact );
                gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
                return;
            }
        }
        return StopCall( pszCallId, SIP_MOVED_TEMPORARILY );
    }

    // B2BUA 호 설정
    if ( bRoutePrefix == false ) {
        if ( gclsUserMap.Select( pszTo, clsUserInfo ) == false ) return StopCall( pszCallId, SIP_NOT_FOUND );
    }

    int iStartPort = -1;
    std::string strMediaNode;  // 이 호를 처리하는 미디어(CMP) 노드 relay IP — state 기록용
    std::string strCallId;
    CSipCallRoute clsRoute;

    // B2BUA: Session-ID 생성 + 발신 leg 매핑
    std::string strSessionId;
    if ( gclsCallDir.IsEnabled() ) {
        strSessionId = CCallDir::GenerateSessionId();
        gclsCallDir.MapCallToSession( pszCallId, strSessionId );
        // Session-ID로 디렉터리 생성
        gclsCallDir.GetVoipDir( pszCallId, pszFrom, pszTo );
    }

    // CMP relay descriptor — CreateCall 실패 시 회수 + CallMap.SetRelayInfo 에 사용 (블록 밖 scope).
    std::string strRelaySessionId, strRelaySesId, strRelayLocalIp;
    if ( gclsSetup.m_bUseRtpRelay ) {
        // 녹취 경로: Recording 활성화 시 세션 디렉터리 사용
        std::string strRecordDir, strLogDir;
        if ( gclsSetup.m_bRecordEnable && gclsCallDir.IsEnabled() ) {
            strRecordDir = gclsCallDir.GetVoipDir( pszCallId, pszFrom, pszTo );
            strLogDir = strRecordDir;
        }
        // 발신측 RTP 주소를 ADD_SESSION에 포함 (생성 + peer[0] 한번에)
        int iAudioPort = pclsRtp->GetAudioPort();
        if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
        int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;

        // sesid: 수신 INVITE의 Call-ID로 이미 발행되어 있으면 재사용, 없으면 발행
        strRelaySesId = gclsSipLogger.GetOrIssueSesId( pszCallId, pszFrom ? pszFrom : "" );

        // CMP relay 생성: session_id(전역 유일) 발행 후 ADD_SESSION 직접 전송.
        //   (구 gclsRtpMap.CreatePort 대체 — 포트단독키 bookkeeping 제거. 멀티 미디어노드에서 포트가
        //    노드별 비유일이라 포트키 충돌로 teardown 이 엉뚱한 세션을 회수→relay 누수하던 근본버그 제거.)
        strRelaySessionId = CCmpClient::IssueSessionId();
        std::string strAllocatedIp;
        int iLocalPort = 0, iLocalVideoPort = 0;
        if ( !gclsCmpClient.AddSession( strRelaySessionId, strAllocatedIp, iLocalPort, iLocalVideoPort, strRecordDir,
                                        strLogDir, pszFrom ? pszFrom : "", pszTo ? pszTo : "", pclsRtp->m_strIp,
                                        iAudioPort, iVideoPort, strRelaySesId ) ) {
            return StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
        }
        iStartPort = iLocalPort;
        strRelayLocalIp = strAllocatedIp;

        std::string strRelayIp = CspAddressing::GetLocalRtpAddress();
        if ( !strAllocatedIp.empty() ) {
            strRelayIp = strAllocatedIp;
            strMediaNode = strAllocatedIp;  // CMP 노드 relay IP = 처리 미디어 노드
        }
        pclsRtp->SetIpPort( strRelayIp.c_str(), iStartPort, SOCKET_COUNT_PER_MEDIA );
    }

    clsUserInfo.GetCallRoute( clsRoute );
    clsRoute.m_b100rel = gclsUserAgent.Is100rel( pszCallId );
    // T3: route 결정으로 추출한 outbound local identity hint 를 dialog 까지 전달.
    //     hint 미설정 시 stack primary fallback (NO-OP regression).
    if ( !strOutboundLocalIp.empty() ) clsRoute.m_strOutboundLocalIp = strOutboundLocalIp;
    if ( iOutboundLocalPort > 0 ) clsRoute.m_iOutboundLocalPort = iOutboundLocalPort;

    CSipMessage *pclsInvite;
    if ( gclsUserAgent.CreateCall( pszFrom, pszTo, pclsRtp, &clsRoute, strCallId, &pclsInvite ) == false ) {
        // [LEAK-FIX] B-leg INVITE 생성 실패 — 직전 AddSession 으로 만든 CMP relay 가 CallMap 등록
        //   전이라 추적 불가(고아) 상태로 누수된다. 여기서 즉시 RemoveSession 으로 회수한다.
        //   (호 실패 시 주요 RTP 누수 경로 — session_id 로 직접 회수.)
        if ( !strRelaySessionId.empty() ) {
            CLog::Print( LOG_INFO, "CreateCall failed — freeing orphan CMP relay session=%s callid=%s",
                         strRelaySessionId.c_str(), pszCallId );
            gclsCmpClient.RemoveSession( strRelaySessionId, pszFrom ? pszFrom : "", pszTo ? pszTo : "", strRelaySesId );
        }
        return StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
    }

    // P-Asserted-Identity: B2BUA 발신 leg에 인증된 발신자 신원 전달 (3GPP TS 24.229)
    if ( pclsInvite ) {
        std::string strDomain = gclsServiceMap.GetDomainByKind( "volte" );
        char szPAI[512];
        snprintf( szPAI, sizeof( szPAI ), "<sip:%s@%s>", pszFrom ? pszFrom : "", strDomain.c_str() );
        pclsInvite->AddHeader( "P-Asserted-Identity", szPAI );
    }

    gclsCallMap.Insert( pszCallId, strCallId.c_str(), iStartPort );
    SetCallOwner( strCallId.c_str(), GetCallOwner( pszCallId ) );

    // CMP relay descriptor 를 양 leg(수신/발신 Call-ID)에 기록 → teardown(BYE)·answer MODIFY 가
    //   포트가 아닌 session_id 로 CMP 세션을 직접 지목 (포트충돌 오지목/누수 차단).
    if ( !strRelaySessionId.empty() ) {
        gclsCallMap.SetRelayInfo( pszCallId, strRelaySessionId, strRelaySesId, strRelayLocalIp, pszFrom ? pszFrom : "",
                                  pszTo ? pszTo : "" );
    }

    // B2BUA: 착신 leg Call-ID에도 발신 leg의 sesid 계승 등록
    std::string strLegASesId = gclsSipLogger.GetSesIdByCallId( pszCallId );
    if ( !strLegASesId.empty() && !strCallId.empty() ) {
        gclsSipLogger.SetCallSesId( strCallId, strLegASesId );
    }

    // B2BUA: 착신 leg도 같은 Session-ID에 매핑 + session.json에 sesid 기록
    if ( gclsCallDir.IsEnabled() && !strSessionId.empty() ) {
        gclsCallDir.MapCallToSession( strCallId, strSessionId );
        gclsCallDir.WriteSessionMapping( strSessionId, pszCallId, strCallId, strLegASesId );
    }

    if ( gclsUserAgent.StartCall( strCallId.c_str(), pclsInvite ) == false ) {
        gclsCallMap.Delete( pszCallId );
        return StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
    }

    if ( gclsDbManager.IsConnected() ) {
        gclsDbManager.InsertCallLog( pszCallId, false, "", pszFrom, pszTo );
        gclsDbManager.InsertParticipant( pszCallId, pszFrom, "caller", true );
        gclsDbManager.InsertParticipant( pszCallId, pszTo, "callee", false );
    }
    if ( gclsCallDir.IsEnabled() ) {
        bool bVideo = ( pclsRtp->GetMediaCount() >= 2 && pclsRtp->GetVideoPort() > 0 );
        gclsCallDir.VoipCallStart( pszCallId, pszFrom, pszTo, bVideo, strMediaNode );
        gclsCallDir.VoipAddParticipant( pszCallId, pszFrom, "caller" );
        gclsCallDir.VoipAddParticipant( pszCallId, pszTo, "callee" );
    }
}

void CModuleDispatcher::EventCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp ) {
    CCallInfo clsCallInfo;
    CLog::Print( LOG_DEBUG, "EventCallRing(%s,%d)", pszCallId, iSipStatus );

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            pclsRtp->m_iPort = clsCallInfo.m_iPeerRtpPort;
            pclsRtp->m_strIp = CspAddressing::GetLocalRtpAddress();
        }
        int iRSeq = gclsUserAgent.GetRSeq( pszCallId );
        if ( iRSeq != -1 ) gclsUserAgent.SetRSeq( clsCallInfo.m_strPeerCallId.c_str(), iRSeq );
        gclsUserAgent.RingCall( clsCallInfo.m_strPeerCallId.c_str(), iSipStatus, pclsRtp );
    } else if ( gclsTransCallMap.Select( pszCallId, clsCallInfo ) ) {
        gclsUserAgent.SendNotify( clsCallInfo.m_strPeerCallId.c_str(), iSipStatus );
    }
}

void CModuleDispatcher::EventCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    CCallInfo clsCallInfo;
    CLog::Print( LOG_DEBUG, "EventCallStart(%s)", pszCallId );

    // 확립(answer) 표시 — sweeper 가 미확립(pending) 호만 빠르게 회수하도록.
    gclsCallMap.SetEstablished( pszCallId );

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        // Service log: VoipCallAnswer
        if ( gclsCallDir.IsEnabled() ) {
            std::string strOrigCallId = pszCallId;
            if ( !gclsCallDir.GetSessionId( pszCallId ).empty() )
                strOrigCallId = pszCallId;
            else if ( !clsCallInfo.m_strPeerCallId.empty() &&
                      !gclsCallDir.GetSessionId( clsCallInfo.m_strPeerCallId ).empty() )
                strOrigCallId = clsCallInfo.m_strPeerCallId;
            else if ( !clsCallInfo.m_strPeerCallId.empty() )
                strOrigCallId = clsCallInfo.m_strPeerCallId;
            std::string gid = gclsGroupCallService.GetGroupIdByCallId( pszCallId );
            if ( gid.empty() ) {
                gclsCallDir.VoipCallAnswer( strOrigCallId );
            }
        }
        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            std::string strAllocatedIp = clsCallInfo.m_strRelayLocalIp;  // 구 gclsRtpMap.GetLocalIp 대체
            // 착신(callee) RTP 주소를 CMP 에 MODIFY (peer_index=1). 구 RtpMap.SetIpPort→CRtpInfo→UpdateSession
            //   경로를 session_id 직접 호출로 대체 (포트키 제거).
            if ( !clsCallInfo.m_strRelaySessionId.empty() ) {
                int iAudioPort = pclsRtp->GetAudioPort();
                if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
                int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;
                if ( iAudioPort > 0 ) {
                    gclsCmpClient.ModifySession( clsCallInfo.m_strRelaySessionId, pclsRtp->m_strIp, iAudioPort,
                                                 iVideoPort > 0 ? iVideoPort : 0, 1, clsCallInfo.m_strRelayCaller,
                                                 clsCallInfo.m_strRelayCallee, clsCallInfo.m_strRelaySesId );
                }
            }

            int iRemoteAudio = pclsRtp->GetAudioPort();
            if ( iRemoteAudio <= 0 && pclsRtp->m_iPort > 0 ) iRemoteAudio = pclsRtp->m_iPort;
            if ( iRemoteAudio > 0 ) {
                int iRemoteVideo = pclsRtp->GetVideoPort();
                // SDP m=application floor control 포트 파싱 (≤0 이면 OnCallStarted 내부 fallback)
                int iRemoteFloor = pclsRtp->GetApplicationPort();
                gclsGroupCallService.OnCallStarted( pszCallId, pclsRtp->m_strIp, iRemoteAudio,
                                                    iRemoteFloor > 0 ? iRemoteFloor : 0, iRemoteVideo );
            }

            std::string strRelayIp = CspAddressing::GetLocalRtpAddress();
            if ( !strAllocatedIp.empty() ) strRelayIp = strAllocatedIp;
            pclsRtp->SetIpPort( strRelayIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        } else if ( clsCallInfo.m_iPeerRtpPort > 0 ) {
            gclsGroupCallService.OnCallStarted( pszCallId, CspAddressing::GetLocalRtpAddress(),
                                                clsCallInfo.m_iPeerRtpPort );
        }

        if ( gclsUserAgent.IsConnected( clsCallInfo.m_strPeerCallId.c_str() ) ) {
            gclsUserAgent.SendReInvite( clsCallInfo.m_strPeerCallId.c_str(), pclsRtp );
        } else {
            gclsUserAgent.AcceptCall( clsCallInfo.m_strPeerCallId.c_str(), pclsRtp );
        }
    } else if ( gclsTransCallMap.Select( pszCallId, clsCallInfo ) ) {
        gclsUserAgent.SendNotify( clsCallInfo.m_strPeerCallId.c_str(), SIP_OK );
        std::string strReferToCallId;
        int iStartPort = -1;
        if ( gclsCallMap.Select( clsCallInfo.m_strPeerCallId.c_str(), strReferToCallId ) ) {
            gclsUserAgent.StopCall( clsCallInfo.m_strPeerCallId.c_str() );
            gclsCallMap.Delete( clsCallInfo.m_strPeerCallId.c_str() );
        }
        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            iStartPort = clsCallInfo.m_iPeerRtpPort - 2;
            pclsRtp->SetIpPort( CspAddressing::GetLocalRtpAddress().c_str(), clsCallInfo.m_iPeerRtpPort,
                                SOCKET_COUNT_PER_MEDIA );
        }
        gclsUserAgent.SendReInvite( strReferToCallId.c_str(), pclsRtp );
        gclsCallMap.Insert( strReferToCallId.c_str(), pszCallId, iStartPort );
        gclsTransCallMap.Delete( pszCallId, false );
    } else {
        gclsUserAgent.StopCall( pszCallId );
    }
}

void CModuleDispatcher::EventCallEnd( const char *pszCallId, int iSipStatus ) {
    CCallInfo clsCallInfo;
    CLog::Print( LOG_DEBUG, "EventCallEnd(%s:%d)", pszCallId, iSipStatus );

    bool bSelHit = gclsCallMap.Select( pszCallId, clsCallInfo );
    CLog::Print( LOG_DEBUG, "EventCallEnd callid=%s sip=%d selHit=%d peer=%s peerRtpPort=%d", pszCallId, iSipStatus,
                 bSelHit ? 1 : 0, bSelHit ? clsCallInfo.m_strPeerCallId.c_str() : "-",
                 bSelHit ? clsCallInfo.m_iPeerRtpPort : -1 );

    if ( bSelHit ) {
        // Service log: VoipCallEnd
        if ( gclsCallDir.IsEnabled() ) {
            std::string strOrigCallId = pszCallId;
            if ( !gclsCallDir.GetSessionId( pszCallId ).empty() )
                strOrigCallId = pszCallId;
            else if ( !clsCallInfo.m_strPeerCallId.empty() &&
                      !gclsCallDir.GetSessionId( clsCallInfo.m_strPeerCallId ).empty() )
                strOrigCallId = clsCallInfo.m_strPeerCallId;
            else if ( !clsCallInfo.m_strPeerCallId.empty() )
                strOrigCallId = clsCallInfo.m_strPeerCallId;
            std::string gid = gclsGroupCallService.GetGroupIdByCallId( pszCallId );
            if ( gid.empty() ) {
                gclsCallDir.VoipCallEnd( strOrigCallId, iSipStatus == 200 ? "normal" : "error", 0 );
            }
        }
        if ( clsCallInfo.m_bRecv )
            OnCallEnded( pszCallId, iSipStatus );
        else
            OnCallEnded( clsCallInfo.m_strPeerCallId.c_str(), iSipStatus );

        // CMP 리소스 해제 → BYE 순서 (리소스 먼저 해제 후 SIP 종료)
        bool bIsGroup = gclsGroupCallService.OnCallTerminated( pszCallId );
        gclsCallMap.Delete( pszCallId, !bIsGroup );
        gclsUserAgent.StopCall( clsCallInfo.m_strPeerCallId.c_str() );

        RemoveCallOwner( pszCallId );
        RemoveCallOwner( clsCallInfo.m_strPeerCallId.c_str() );
    } else {
        std::string strCallId;
        if ( gclsTransCallMap.Select( pszCallId, strCallId ) ) {
            gclsUserAgent.SendNotify( strCallId.c_str(), iSipStatus );
            gclsTransCallMap.Delete( pszCallId );
        }
        RemoveCallOwner( pszCallId );
    }
}

void CModuleDispatcher::EventReInvite( const char *pszCallId, CSipCallRtp *pclsRemoteRtp, CSipCallRtp *pclsLocalRtp ) {
    CCallInfo clsCallInfo;
    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( pclsRemoteRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            pclsRemoteRtp->SetIpPort( CspAddressing::GetLocalRtpAddress().c_str(), clsCallInfo.m_iPeerRtpPort,
                                      SOCKET_COUNT_PER_MEDIA );
        }
        gclsUserAgent.SendReInvite( clsCallInfo.m_strPeerCallId.c_str(), pclsRemoteRtp );
    }
}

void CModuleDispatcher::EventPrack( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    CCallInfo clsCallInfo;
    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            pclsRtp->SetIpPort( CspAddressing::GetLocalRtpAddress().c_str(), clsCallInfo.m_iPeerRtpPort,
                                SOCKET_COUNT_PER_MEDIA );
        }
        gclsUserAgent.SendPrack( clsCallInfo.m_strPeerCallId.c_str(), pclsRtp );
    }
}

bool CModuleDispatcher::EventTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreenedTransfer ) {
    CCallInfo clsCallInfo, clsReferToCallInfo;
    CSipCallRtp clsRtp;

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) == false ) return false;
    if ( gclsCallMap.Select( pszReferToCallId, clsReferToCallInfo ) == false ) return false;

    gclsCallMap.Delete( pszCallId );
    gclsCallMap.Delete( pszReferToCallId, false );

    if ( gclsUserAgent.GetRemoteCallRtp( clsCallInfo.m_strPeerCallId.c_str(), &clsRtp ) == false ) return false;
    clsRtp.SetDirection( E_RTP_SEND_RECV );

    if ( gclsSetup.m_bUseRtpRelay ) {
        const std::string strRtpAddr = CspAddressing::GetLocalRtpAddress();
        if ( bScreenedTransfer )
            clsRtp.SetIpPort( strRtpAddr.c_str(), clsReferToCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        else
            clsRtp.SetIpPort( strRtpAddr.c_str(), clsReferToCallInfo.m_iPeerRtpPort + 2, SOCKET_COUNT_PER_MEDIA );
    }

    if ( bScreenedTransfer ) {
        CSipCallRtp clsReferToRtp;
        if ( gclsUserAgent.GetRemoteCallRtp( clsReferToCallInfo.m_strPeerCallId.c_str(), &clsReferToRtp ) == false )
            return false;
        clsReferToRtp.SetDirection( E_RTP_SEND_RECV );
        if ( gclsSetup.m_bUseRtpRelay )
            clsReferToRtp.SetIpPort( CspAddressing::GetLocalRtpAddress().c_str(), clsReferToCallInfo.m_iPeerRtpPort + 2,
                                     SOCKET_COUNT_PER_MEDIA );
        gclsCallMap.Insert( clsCallInfo.m_strPeerCallId.c_str(), clsReferToCallInfo.m_strPeerCallId.c_str(),
                            clsReferToCallInfo.m_iPeerRtpPort );
        gclsUserAgent.SendReInvite( clsCallInfo.m_strPeerCallId.c_str(), &clsReferToRtp );
        gclsUserAgent.SendReInvite( clsReferToCallInfo.m_strPeerCallId.c_str(), &clsRtp );
    }

    gclsUserAgent.StopCall( pszCallId );
    gclsUserAgent.StopCall( pszReferToCallId, SIP_REQUEST_TERMINATED );

    if ( bScreenedTransfer == false ) {
        std::string strNewCallId, strFromId, strToId;
        CUserInfo clsUserInfo;
        CSipCallRoute clsRoute;
        gclsUserAgent.GetToId( clsCallInfo.m_strPeerCallId.c_str(), strFromId );
        gclsUserAgent.GetToId( clsReferToCallInfo.m_strPeerCallId.c_str(), strToId );
        if ( gclsUserMap.Select( strToId.c_str(), clsUserInfo ) ) {
            clsUserInfo.GetCallRoute( clsRoute );
            gclsUserAgent.StopCall( clsReferToCallInfo.m_strPeerCallId.c_str() );
            gclsUserAgent.StartCall( strFromId.c_str(), strToId.c_str(), &clsRtp, &clsRoute, strNewCallId );
            gclsCallMap.Insert( strNewCallId.c_str(), clsCallInfo.m_strPeerCallId.c_str(),
                                clsReferToCallInfo.m_iPeerRtpPort );
        } else {
            gclsUserAgent.StopCall( clsCallInfo.m_strPeerCallId.c_str() );
            gclsUserAgent.StopCall( clsReferToCallInfo.m_strPeerCallId.c_str() );
        }
    }

    // (구 gclsRtpMap.ReSetIpPort 제거 — CRtpInfo 로컬 소켓배열 리셋이라 CMP relay 분리 후 no-op 이었음.)
    return true;
}

bool CModuleDispatcher::EventBlindTransfer( const char *pszCallId, const char *pszReferToId ) {
    std::string strCallId, strInviteCallId, strToId;
    CSipCallRtp clsRtp;
    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    int iStartPort = -1;

    if ( gclsCallMap.Select( pszCallId, strCallId ) == false ) return false;
    if ( gclsUserAgent.GetToId( strCallId.c_str(), strToId ) == false ) return false;
    if ( gclsUserMap.Select( pszReferToId, clsUserInfo ) == false ) return false;
    if ( gclsUserAgent.GetRemoteCallRtp( strCallId.c_str(), &clsRtp ) == false ) return false;
    clsRtp.SetDirection( E_RTP_SEND_RECV );

    std::string strRelaySessionId, strRelaySesId, strRelayLocalIp;
    if ( gclsSetup.m_bUseRtpRelay ) {
        // 전환(blind transfer) relay 생성 — 구 gclsRtpMap.CreatePort 대체 (session_id 직접 AddSession).
        strRelaySesId = gclsSipLogger.GetOrIssueSesId( pszCallId, "" );
        strRelaySessionId = CCmpClient::IssueSessionId();
        int iLocalPort = 0, iLocalVideoPort = 0;
        int iVideoPort = ( clsRtp.GetMediaCount() >= 2 ) ? clsRtp.GetVideoPort() : 0;
        if ( !gclsCmpClient.AddSession( strRelaySessionId, strRelayLocalIp, iLocalPort, iLocalVideoPort, "", "", "",
                                        pszReferToId ? pszReferToId : "", clsRtp.m_strIp, clsRtp.GetAudioPort(),
                                        iVideoPort, strRelaySesId ) ) {
            return false;
        }
        iStartPort = iLocalPort;
        std::string strRelayIp = strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : strRelayLocalIp;
        clsRtp.SetIpPort( strRelayIp.c_str(), iStartPort, SOCKET_COUNT_PER_MEDIA );
    }

    clsUserInfo.GetCallRoute( clsRoute );
    if ( gclsUserAgent.StartCall( strToId.c_str(), pszReferToId, &clsRtp, &clsRoute, strInviteCallId ) == false ) {
        if ( !strRelaySessionId.empty() )
            gclsCmpClient.RemoveSession( strRelaySessionId, "", pszReferToId ? pszReferToId : "", strRelaySesId );
        return false;
    }
    gclsTransCallMap.Insert( pszCallId, strInviteCallId.c_str(), iStartPort );
    if ( !strRelaySessionId.empty() )
        gclsTransCallMap.SetRelayInfo( pszCallId, strRelaySessionId, strRelaySesId, strRelayLocalIp, "",
                                       pszReferToId ? pszReferToId : "" );
    return true;
}

bool CModuleDispatcher::EventMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) {
    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    if ( gclsUserMap.Select( pszTo, clsUserInfo ) == false ) return false;
    clsUserInfo.GetCallRoute( clsRoute );
    return gclsUserAgent.SendSms( pszFrom, pszTo, pclsMessage->m_strBody.c_str(), &clsRoute );
}

// ──────────────────────────────────────────────────────────────
//  PickUp (from SipServerPickUp.hpp)
// ──────────────────────────────────────────────────────────────

void CModuleDispatcher::PickUp( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp ) {
    CspUser xmlFrom;
    USER_ID_LIST clsUserIdList;
    bool bCallPickup = false;

    if ( gclsCspUserMap.Select( pszFrom, xmlFrom ) &&
         gclsUserMap.SelectGroup( xmlFrom.m_strOrganizationId.c_str(), clsUserIdList ) ) {
        USER_ID_LIST::iterator itUIL;
        std::string strOldCallId;

        for ( itUIL = clsUserIdList.begin(); itUIL != clsUserIdList.end(); ++itUIL ) {
            if ( gclsCallMap.SelectToRing( itUIL->c_str(), strOldCallId ) == false ) continue;

            CCallInfo clsOldCallInfo;
            if ( gclsCallMap.Select( strOldCallId.c_str(), clsOldCallInfo ) &&
                 gclsCallMap.Insert( pszCallId, clsOldCallInfo ) ) {
                gclsCallMap.DeleteOne( strOldCallId.c_str() );
                gclsUserAgent.StopCall( strOldCallId.c_str() );

                CSipCallRtp clsRemoteRtp;
                if ( gclsUserAgent.GetRemoteCallRtp( clsOldCallInfo.m_strPeerCallId.c_str(), &clsRemoteRtp ) ) {
                    if ( pclsRtp ) {
                        if ( clsOldCallInfo.m_iPeerRtpPort > 0 ) {
                            pclsRtp->m_iPort = clsOldCallInfo.m_iPeerRtpPort;
                            pclsRtp->m_strIp = CspAddressing::GetLocalRtpAddress();
                        }
                        pclsRtp->m_iCodec = clsRemoteRtp.m_iCodec;
                    }
                    if ( gclsUserAgent.AcceptCall( clsOldCallInfo.m_strPeerCallId.c_str(), pclsRtp ) ) {
                        CCallInfo clsPeerCallInfo;
                        if ( gclsCallMap.Select( clsOldCallInfo.m_strPeerCallId.c_str(), clsPeerCallInfo ) ) {
                            gclsCallMap.Update( clsOldCallInfo.m_strPeerCallId.c_str(), pszCallId );
                            if ( pclsRtp ) {
                                if ( clsOldCallInfo.m_iPeerRtpPort > 0 )
                                    pclsRtp->m_iPort = clsPeerCallInfo.m_iPeerRtpPort;
                                else
                                    pclsRtp = &clsRemoteRtp;
                            }
                            gclsUserAgent.AcceptCall( pszCallId, pclsRtp );
                            bCallPickup = true;
                        }
                        if ( bCallPickup == false ) gclsUserAgent.StopCall( clsOldCallInfo.m_strPeerCallId.c_str() );
                    }
                }
            }
            break;
        }
    }

    if ( bCallPickup == false ) StopCall( pszCallId, SIP_NOT_FOUND );
}
