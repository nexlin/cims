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
#include <set>

#include "CallDir.h"
#include "CallMap.h"
#include "CmpClient.h"
#include "CspAclPolicyEngine.h"
#include "CspAddressing.h"
#include "CspDialPlan.h"
#include "CspLocalNodeMap.h"
#include "CspPendingRouteMap.h"
#include "CspPttGroup.h"
#include "CspRemoteNodeMap.h"
#include "CspRouteHealth.h"
#include "CspRouteMap.h"
#include "CspRouteSetMap.h"
#include "CspRoutingPolicyEngine.h"
#include "CspServer.h"
#include "CspServiceMap.h"
#include "CspTrunkRegistrar.h"
#include "CspUser.h"
#include "DbManager.h"
#include "Directory.h"
#include "GroupCallService.h"
#include "GroupMap.h"
#include "Log.h"
#include "McDataCodec.h"
#include "McDataMediaService.h"
#include "McpttInfo.h"
#include "MemoryDebug.h"
#include "NonceMap.h"
#include "RelayCodec.h"
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
    CLog::Print( LOG_SYSTEM, "ModuleDispatcher: Roles CSCF=%s TAS=%s PTT-AS=%s IBCF=%s MCDATA-AS=%s",
                 m_clsCscf.IsEnabled() ? "ON" : "OFF", m_clsTas.IsEnabled() ? "ON" : "OFF",
                 m_clsPttAs.IsEnabled() ? "ON" : "OFF", m_clsIbcf.IsEnabled() ? "ON" : "OFF",
                 m_clsMcDataAs.IsEnabled() ? "ON" : "OFF" );
}

bool CModuleDispatcher::Start( CSipStackSetup &clsSetup ) {
    // G10 (2026-04-23): SipServerMap (legacy IBCF XML) 제거. routing_policies/routes/
    //   remote_nodes 체계가 SOT. REGISTER_TO_REMOTE 는 별도 워커로 이관 예정.

    // UserAgent 시작 (내부적으로 CSipStack 시작 + UserAgent 를 콜백 등록)
    if ( gclsUserAgent.Start( clsSetup, this, this ) == false ) return false;

    // 세션 타이머 (RFC 4028) — BYE 없이 사라진 leg 의 시한 회수.
    //   docs/design/features/leg_liveness.md. 점검 tick 은 CspServer 주기 루프가 돌린다.
    gclsUserAgent.SetSessionTimer(
        gclsSetup.m_bSessionTimer, gclsSetup.m_iSessionExpires, gclsSetup.m_iSessionMinSE,
        gclsSetup.m_strSessionRefresher == "ue" ? E_SESSION_REFRESHER_REMOTE : E_SESSION_REFRESHER_LOCAL );

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
        // 이 호출은 멱등 가드에 걸려 보통 no-op 이다(EventCallEnd 가 이미 마감했다) — 그래도
        // 값을 온전히 넘긴다. 가드가 바뀌면 조용히 0 이 들어가는 쪽으로 퇴화하지 않게.
        gclsCallDir.VoipCallEnd( clsCdr.m_strCallId, "normal", dur > 0 ? dur : 0, iSipStatus );
    }
}

// (Proxy 모드 제거됨 — 모든 VoIP INVITE는 B2BUA + CMP 경유)

// ──────────────────────────────────────────────────────────────
//  ISipStackCallBack — RecvRequest
//  순서: ModuleDispatcher (1st) → CSipUserAgent (2nd)
// ──────────────────────────────────────────────────────────────

/**
 * @brief 수신 요청이 어느 Route 로 들어왔는가 — (수신 LocalNode, 소스 IP[:UDP 소스 포트], transport) 로 식별.
 *
 * Route 는 (LocalNode, RemoteNode) 연결 정보라 양방향으로 쓴다. 발신은 RoutingPolicy 가 고르고, 인바운드는 여기서
 * 역으로 찾는다. 결과가 있으면 그 요청은 "설정된 피어" 의 것이다 — 피어 신뢰(Digest 생략)와 ACL scope=route/route_set
 * 이 이 식별에서 나온다(sip_service_model.md §4). 접속점 edge 는 신뢰 근거가 아니다(주소 선택 분류일 뿐).
 * TCP/TLS 는 소스 포트가 임시 포트라 IP 로만 맞춘다(FindInbound 가 처리).
 */
static RouteConfig InboundRouteOf( const CSipMessage *pclsMessage, const std::string &strLocalNodeName ) {
    if ( pclsMessage == NULL || pclsMessage->m_strClientIp.empty() ) return RouteConfig();
    int iSrcPort = ( pclsMessage->m_eTransport == E_SIP_UDP ) ? pclsMessage->m_iClientPort : 0;
    RouteConfig rc = gclsRouteMap.FindInbound( strLocalNodeName, pclsMessage->m_strClientIp, iSrcPort,
                                               SipGetTransport( pclsMessage->m_eTransport ) );
    if ( rc.IsValid() ) return rc;
    // 등록형 트렁크(CCspTrunkRegistrar) — RemoteNode.ip 가 동적이거나 NAT 뒤라 소스가 설정 주소와 다르면 등록 바인딩의
    // 소스로 식별
    return gclsTrunkRegistrar.FindBySource( strLocalNodeName, pclsMessage->m_strClientIp, iSrcPort,
                                            SipGetTransport( pclsMessage->m_eTransport ) );
}

/** 등록형 트렁크의 요청 신뢰 — 그 Route 의 바인딩(REGISTER 소스)에서 온 요청은 등록으로 인증된 것이다(UE 등록 바인딩과
 * 같은 규칙). */
static bool TrunkRegisteredSource( const RouteConfig &rc, const CSipMessage *pclsMessage ) {
    if ( !rc.IsTrunkAccount() ) return false;
    int iSrcPort = ( pclsMessage->m_eTransport == E_SIP_UDP ) ? pclsMessage->m_iClientPort : 0;
    return gclsTrunkRegistrar.IsRegisteredSource( rc.name, pclsMessage->m_strClientIp, iSrcPort,
                                                  SipGetTransport( pclsMessage->m_eTransport ) );
}

// ──────────────────────────────────────────────────────────────
//  착신 번호 번역(다이얼 플랜) — CspDialPlan, sip_service_model.md §2-10
//  TS 24.229 §5.4.3.2: 국제형이 아닌 착신(국내형·phone-context)은 홈 망이 E.164 로 번역하고, 못 하면 484.
// ──────────────────────────────────────────────────────────────

/** 요청의 인바운드 Route(피어 식별) — 수신 listener id → LocalNode 이름 → InboundRouteOf. */
static RouteConfig InboundRouteFor( const CSipMessage *pclsMessage ) {
    std::string strLn;
    if ( pclsMessage && pclsMessage->m_iListenerId > 0 ) {
        LocalNodeInfo ln = gclsLocalNodeMap.GetByIntId( pclsMessage->m_iListenerId );
        if ( ln.IsValid() ) strLn = ln.name;
    }
    return InboundRouteOf( pclsMessage, strLn );
}

/** 요청에 적용할 다이얼 플랜 — ① 인바운드 Route(피어가 보낸 요청은 그 Route 의 플랜) ② `phone-context` 가 접속서비스
 *  도메인이면 그 서비스 ③ 발신 가입자의 접속서비스(service_ref → kind 대표 폴백). strSource 는 로그용 출처. */
static DialPlan DialPlanFor( const RouteConfig &clsInRoute, const std::string &strFrom,
                             const std::string &strPhoneContext, std::string &strSource ) {
    if ( clsInRoute.IsValid() ) {
        strSource = "route:" + clsInRoute.name;
        return clsInRoute.dial_plan;
    }
    if ( !strPhoneContext.empty() && strPhoneContext[0] != '+' ) {
        ServiceInfo svc = gclsServiceMap.GetByDomain( strPhoneContext );
        if ( svc.id > 0 ) {
            strSource = "phone-context:" + svc.name;
            return svc.dial_plan;
        }
    }
    ServiceInfo svc = gclsServiceMap.GetForUser( strFrom, "volte" );
    strSource = ( svc.id > 0 ) ? "service:" + svc.name : "none";
    return svc.dial_plan;
}

/** 착신 해석 + 번역. 착신 = **Request-URI**(sip user / tel host — 라우팅 키, RFC 3261 §16.6), 번호가 없으면 To user
 * 폴백. 그룹 id(메모리·DB 단건)는 번호가 아니므로 그대로. TRANSLATED 면 Request-URI 를 재작성한다 — To 는 손대지 않는다
 *  (§8.2.6.2 응답의 To 는 요청 그대로). 반환 뒤 strCallee 가 이후 조회·라우팅·B-leg 가 쓸 착신이다. 멱등(재호출 무해).
 */
static EDialPlanResult ResolveCallee( CSipMessage *pclsMessage, const RouteConfig &clsInRoute,
                                      const std::string &strFrom, std::string &strCallee, std::string &strSource ) {
    strCallee.clear();
    strSource.clear();
    std::string strNum, strCtx;
    const bool bFromReqUri = CspDialPlan::ExtractNumber( pclsMessage->m_clsReqUri, strNum, strCtx );
    if ( !bFromReqUri && !CspDialPlan::ExtractNumber( pclsMessage->m_clsTo.m_clsUri, strNum, strCtx ) )
        return DIAL_PLAN_UNCHANGED;
    if ( gclsGroupMap.Contains( strNum.c_str() ) ) {
        strCallee = strNum;
        strSource = "group";
        return DIAL_PLAN_UNCHANGED;
    }
    DialPlan clsPlan = DialPlanFor( clsInRoute, strFrom, strCtx, strSource );
    std::string strOut;
    EDialPlanResult eRes = CspDialPlan::Normalize( strNum, strCtx, clsPlan, strOut );
    if ( eRes == DIAL_PLAN_INCOMPLETE && gclsDbManager.IsConnected() && gclsGroupMap.LoadOneFromDb( strNum.c_str() ) ) {
        // 숫자열 그룹 id 가 아직 메모리에 없었다 — 단건 DB 조회(EventIncomingCall 의 lazy-load 와 같은 안전망)
        strCallee = strNum;
        strSource = "group";
        return DIAL_PLAN_UNCHANGED;
    }
    strCallee = ( eRes == DIAL_PLAN_TRANSLATED ) ? strOut : strNum;
    if ( eRes == DIAL_PLAN_TRANSLATED && bFromReqUri ) CspDialPlan::RewriteNumber( pclsMessage->m_clsReqUri, strOut );
    return eRes;
}

bool CModuleDispatcher::RecvRequest( int iThreadId, CSipMessage *pclsMessage ) {
    std::string strCallId;
    pclsMessage->GetCallId( strCallId );

    // v3 (2026-04-22): 접근제어 — AclPolicyEngine (rule_set 기반).
    //   psip v3 확장으로 수신 listener 식별 가능 → scope=local_node 동작.
    //   인바운드 Route 식별(InboundRouteOf) 로 scope=route/route_set 도 인바운드에서 동작한다.
    std::string strLocalNodeName;
    LocalNodeInfo clsInLn;
    if ( pclsMessage->m_iListenerId > 0 ) {
        clsInLn = gclsLocalNodeMap.GetByIntId( pclsMessage->m_iListenerId );
        if ( clsInLn.IsValid() ) strLocalNodeName = clsInLn.name;
    }
    const RouteConfig clsInRoute = InboundRouteOf( pclsMessage, strLocalNodeName );
    const std::string strInRouteSet = clsInRoute.IsValid() ? gclsRouteSetMap.SetOfRoute( clsInRoute.name ) : "";

    // 피어링 접속점(edge=peering) 은 설정된 피어(Route 가 있는 RemoteNode)만 받는다 — NNI 는 알려진 상대와만 맺는다
    //   (TS 29.165 II-NNI, TS 33.210 NDS/IP 전제). 접속(access) 접속점은 가입자용이라 낯선 소스도 인증 흐름으로 들인다.
    if ( clsInLn.IsValid() && clsInLn.edge == "peering" && !clsInRoute.IsValid() ) {
        CLog::Print( LOG_INFO, "InboundRoute: peering local_node=%s src=%s:%d/%s 에 맞는 Route 없음 → 403",
                     strLocalNodeName.c_str(), pclsMessage->m_strClientIp.c_str(), pclsMessage->m_iClientPort,
                     SipGetTransport( pclsMessage->m_eTransport ) );
        SendResponse( pclsMessage, 403 );
        return true;
    }
    if ( clsInRoute.IsValid() && pclsMessage->IsMethod( SIP_METHOD_INVITE ) &&
         !pclsMessage->m_clsTo.SelectParam( SIP_TAG ) ) {
        CLog::Print( LOG_INFO, "InboundRoute: route=%s remote_node=%s route_set=%s local_node=%s src=%s:%d/%s auth=%s",
                     clsInRoute.name.c_str(), clsInRoute.remote_node_ref.c_str(), strInRouteSet.c_str(),
                     strLocalNodeName.c_str(), pclsMessage->m_strClientIp.c_str(), pclsMessage->m_iClientPort,
                     SipGetTransport( pclsMessage->m_eTransport ), clsInRoute.inbound_auth.c_str() );
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
        AclDecision d = gclsAclPolicyEngine.Check( mctx, strLocalNodeName, clsInRoute.IsValid() ? clsInRoute.name : "",
                                                   strInRouteSet );
        if ( !d.allowed ) {
            CLog::Print( LOG_INFO, "AclPolicy: denied src=%s local_node=%s route=%s policy=%s",
                         pclsMessage->m_strClientIp.c_str(), strLocalNodeName.c_str(),
                         clsInRoute.IsValid() ? clsInRoute.name.c_str() : "-", d.matched_policy.c_str() );
            SendResponse( pclsMessage, 403 );
            return true;
        }
    }

    // OPTIONS → 표준 200 OK 자동 응답 (RFC 3261 §11.2).
    //   트렁크 헬스체크(상대 CSP/Kamailio 등)용. 본 프로세스의 capability 를 간소히 알림.
    if ( pclsMessage->IsMethod( SIP_METHOD_OPTIONS ) ) {
        CSipMessage *pclsResp = pclsMessage->CreateResponse( SIP_OK );
        if ( pclsResp ) {
            pclsResp->AddHeader( "Allow", SIP_ALLOW_METHODS );
            gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResp );
        }
        return true;
    }

    // REGISTER, SUBSCRIBE → CSCF 모듈
    if ( m_clsCscf.IsEnabled() && m_clsCscf.OnSipRequest( iThreadId, pclsMessage ) ) {
        return true;
    }

    // REFER(호 전달) 게이트 → TAS 모듈 (transfer_allowed=false 403 — volte_supplementary_services.md §6.3)
    if ( m_clsTas.IsEnabled() && m_clsTas.OnSipRequest( iThreadId, pclsMessage ) ) {
        return true;
    }

    // INVITE → Proxy 가능 여부 판단
    if ( pclsMessage->IsMethod( SIP_METHOD_INVITE ) ) {
        std::string strTo = pclsMessage->m_clsTo.m_clsUri.m_strUser;
        std::string strFrom = pclsMessage->m_clsFrom.m_clsUri.m_strUser;

        // 착신 번호 번역(다이얼 플랜, §2-10) — 초기 INVITE 만(To tag 없음). 착신은 Request-URI 에서 읽어 +E.164 로
        // 번역하고
        //   Request-URI 를 재작성한다. 이후 그룹 조회·라우팅 규칙(req_uri_user)·TAS 스크린·EventIncomingCall 이 번역된
        //   번호를 본다. 번역 불가(접두 없는 숫자열)는 TS 24.229 §5.4.3.2 대로 484 Address Incomplete.
        if ( !pclsMessage->m_clsTo.SelectParam( SIP_TAG ) ) {
            std::string strCallee, strPlanSource;
            EDialPlanResult eDial = ResolveCallee( pclsMessage, clsInRoute, strFrom, strCallee, strPlanSource );
            if ( eDial == DIAL_PLAN_INCOMPLETE ) {
                CLog::Print( LOG_INFO, "DialPlan: INVITE from(%s) callee(%s) 번역 불가(plan=%s) → 484 [callId=%s]",
                             strFrom.c_str(), strCallee.c_str(), strPlanSource.c_str(), strCallId.c_str() );
                // 시도 장부 — 다이얼로그 생성 전 거절도 시도다(TAS ScreenInvite 의 603 과 같은 이유). 응답은 484.
                if ( gclsCallDir.IsEnabled() )
                    gclsCallDir.VoipCallRejected( strCallId, strFrom, strCallee, SIP_ADDRESS_INCOMPLETE );
                SendResponse( pclsMessage, SIP_ADDRESS_INCOMPLETE );
                return true;
            }
            if ( eDial == DIAL_PLAN_TRANSLATED )
                CLog::Print( LOG_INFO, "DialPlan: INVITE from(%s) callee %s → %s (plan=%s) [callId=%s]",
                             strFrom.c_str(), strTo.c_str(), strCallee.c_str(), strPlanSource.c_str(),
                             strCallId.c_str() );
            if ( !strCallee.empty() ) strTo = strCallee;  // Request-URI 기준 착신(번역 뒤) — To user 는 표시용
        }

        // MCPTT 진행 중 호의 condition 변경(re-INVITE 업그레이드/취소, TS 24.379) 엿보기.
        //   초기 INVITE 는 아직 세션맵 미등록 → 미발동(초기 긴급은 EventIncomingCall 경로가 처리).
        //   재-INVITE(in-dialog, 동일 Call-ID)만 활성 그룹콜로 매칭되어 floor tier 갱신. 흐름은 그대로 진행.
        //   단, capability 불허 그룹으로의 상향은 403 + mcptt-info(emergency-ind=false)로 거절한다
        //   (TS 24.379 §6.3.3.1.14) — 재-INVITE 거절은 다이얼로그를 깨지 않아 호는 normal 유지.
        {
            std::string strGid, strMid;
            if ( gclsGroupCallService.GetGroupCallSession( strCallId, strGid, strMid ) ) {
                CMcpttInfo clsMi = ParseMcpttInfo( pclsMessage->m_strBody );
                int iCond = clsMi.Condition();
                if ( !gclsGroupCallService.IsInCallUpgradeAllowed( strGid, strMid, iCond ) ) {
                    CSipMessage *pclsResp = pclsMessage->CreateResponseWithToTag( SIP_FORBIDDEN );
                    if ( pclsResp ) {
                        pclsResp->m_strBody =
                            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                            "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\">\r\n"
                            "  <mcptt-Params>\r\n"
                            "    <emergency-ind>false</emergency-ind>\r\n"
                            "    <alert-ind>false</alert-ind>\r\n"
                            "  </mcptt-Params>\r\n"
                            "</mcpttinfo>\r\n";
                        pclsResp->m_iContentLength = (int)pclsResp->m_strBody.size();
                        pclsResp->m_clsContentType.Set( "application", "vnd.3gpp.mcptt-info+xml" );
                        gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResp );
                    }
                    CLog::Print( LOG_INFO, "RecvRequest: in-call upgrade denied group(%s) member(%s) cond(%d) → 403",
                                 strGid.c_str(), strMid.c_str(), iCond );
                    return true;
                }
                gclsGroupCallService.ApplyInCallCondition( strGid, strMid, iCond );
            }
        }

        // ⚠️ 테스트 환경 전용 (Setup.TestEnvOpenTermination=true) — 상용 원복 대상.
        //   미등록 발신 INVITE 종단 정책 — 착신(To) 기준.
        //   ① 착신이 로컬 가입자/그룹 → 통과(하단 라우팅/B2BUA). 발신자 401 챌린지도
        //      생략한다(EventIncomingRequestAuth). NAT 뒤 정상 단말·협력업체·외부 수신
        //      통화 허용.
        //   ② 착신이 비가입자(toll-fraud 스캐너의 외부 PSTN 번호 등) → 603 Decline 응답
        //      후 종료. 로그는 남기지 않으며, 소스 IP 를 억제 등록해 원본 패킷 덤프
        //      (psip 수신/송신 게이트)도 생략 → 로그 무발생.
        //   정상 등록 발신자(isAlive)는 이 분기에 들어오지 않아 기존 흐름 그대로.
        //   상용(내부망)은 이 문제가 없어 플래그 off → 표준 인증 흐름 사용.
        if ( gclsSetup.m_bTestEnvOpenTermination ) {
            CspUser clsSecUser;
            if ( !gclsCspUserMap.isAlive( strFrom, clsSecUser ) ) {
                CspUser clsToProv;
                // adhoc-* 는 PTT-AS 의 로컬 서비스 주소공간(EventIncomingCall 에서 ephemeral 합성,
                // CSC 도 접두사 예약) — GroupMap 에는 합성 후에야 실리므로 접두사로 로컬 판정한다.
                bool bLocalTarget =
                    gclsGroupMap.Contains( strTo.c_str() ) || strncmp( strTo.c_str(), "adhoc-", 6 ) == 0 ||
                    gclsCspUserMap.isAlive( strTo.c_str(), clsToProv ) || gclsDbManager.SelectUser( strTo, clsToProv );
                if ( !bLocalTarget ) {
                    CLog::SuppressNetworkSource( pclsMessage->m_strClientIp.c_str(), SIP_SCAN_SUPPRESS_TTL_SEC );
                    SendResponse( pclsMessage, SIP_DECLINE );  // 603
                    return true;
                }
                // 착신 로컬 → 통과 (401 skip 은 EventIncomingRequestAuth 에서)
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
            // 라우팅 규칙의 착신 번호 = 번역된 착신(tel: URI 는 user 가 비어 있어 종전엔 prefix 규칙에 걸리지 않았다)
            mctx.req_uri_user = strTo.empty() ? pclsMessage->m_clsReqUri.m_strUser : strTo;
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
            if ( rd.type == ROUTING_ROUTE_SET && !m_clsIbcf.IsEnabled() ) {
                // 역할 격리(Setup.Roles.IBCF=false) — 피어(트렁크) 라우팅은 IBCF 역할의 것이다. 정책이 RouteSet 을
                // 골라도 이 노드는 피어로
                //   내보내지 않는다(403). 종전엔 가드가 없어 역할을 끈 노드가 트렁크 발신을 했다(test_instrument.md
                //   §12).
                CLog::Print(
                    LOG_INFO,
                    "RoutingPolicyEngine: policy='%s' route_set='%s' picked but Roles.IBCF=false → 403 [callId=%s]",
                    rd.matched_policy.c_str(), rd.target_name.c_str(), strCallId.c_str() );
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
                    // 등록형 트렁크 — 다음 홉은 RemoteNode 설정 주소가 아니라 등록 바인딩(REGISTER 소스, SIPconnect 2.0
                    // §8)
                    TrunkBinding tb;
                    const bool bTrunk = rc.IsTrunkAccount() && gclsTrunkRegistrar.Get( rc.name, tb );
                    if ( bTrunk ) {
                        rn.name = rc.remote_node_ref;
                        rn.ip = tb.ip;
                        rn.port = tb.port;
                        rn.protocol = tb.transport;
                    }
                    if ( rn.IsValid() && !rn.ip.empty() && rn.port > 0 ) {
                        PendingRouteEntry pe;
                        pe.remote_ip = rn.ip;
                        pe.remote_port = rn.port;
                        pe.protocol = rn.protocol;
                        pe.route_name = rd.picked_route;
                        pe.route_set = rd.target_name;
                        pe.policy_name = rd.matched_policy;
                        pe.local_node_ref = rc.local_node_ref;  // outbound leg 자기 주소 결정용
                        pe.hash_key = hashKey;                  // 재라우팅 재선택(hash_by_caller)에 같은 키
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

        // TAS 조기 스크린 — 착신 가입자 DND/착신거부 603, ptt 전용 모드 403 (다이얼로그 생성 전)
        if ( m_clsTas.IsEnabled() && m_clsTas.ScreenInvite( pclsMessage, strFrom.c_str(), strTo.c_str() ) ) {
            return true;
        }

        // 모든 VoIP INVITE → B2BUA (CMP 경유)
        return false;
    }

    return false;
}

/**
 * @brief NOTIFY 가 최종 실패하면 해당 구독을 즉시 회수한다 (RFC 6665 §4.2.2).
 *
 * 481/404/410 은 구독자 dialog 가 사라졌다는 **확정 신호**이고, 트랜잭션 타임아웃은 단말이
 * 응답 자체를 못 하는 상태다. 어느 쪽이든 구독을 남겨두면 만료(최대 Expires=3600)까지
 * 로스터 이벤트마다 죽은 dialog 로 NOTIFY 가 계속 나간다. 실측(2026-07-31): 앱을 force-stop
 * 하면 구 인스턴스 구독이 살아남아 신 소켓으로 중복 NOTIFY 가 가고 앱이 481 을 주는데도
 * 1시간을 버텼다.
 *
 * 5xx 는 회수하지 않는다 — 구 APK 호환용 in-dialog 폴백 NOTIFY 가 정상적으로 500 을 주고,
 * 실제 구독자의 5xx 는 일시적 오류일 수 있다. 폴백 NOTIFY 는 애초에 Call-ID 가 구독 맵에
 * 없어 무시되지만, 조건을 좁혀 의도를 분명히 한다. (죽은 **leg** 회수는 별건 — P1-①)
 *
 * @param pclsMessage NOTIFY 응답(또는 타임아웃된 NOTIFY 요청)
 * @param iStatusCode 응답 코드. 타임아웃은 0 을 넘긴다.
 */
static void ReapSubscriptionOnNotifyFailure( CSipMessage *pclsMessage, int iStatusCode ) {
    if ( pclsMessage->m_clsCSeq.m_strMethod != SIP_METHOD_NOTIFY ) return;

    const bool bTimeout = ( iStatusCode == 0 );
    if ( !bTimeout && iStatusCode != SIP_CALL_TRANSACTION_DOES_NOT_EXIST && iStatusCode != SIP_NOT_FOUND &&
         iStatusCode != SIP_GONE )
        return;

    std::string strCallId;
    if ( !pclsMessage->GetCallId( strCallId ) ) return;

    SubscriptionInfo clsSub;
    if ( !gclsSubscriptionManager.GetSubscriptionByCallId( strCallId, clsSub ) ) return;

    CLog::Print( LOG_INFO, "Subscription Reaped: User=%s Type=%s CallId=%s Cause=%s", clsSub.strUserId.c_str(),
                 clsSub.strEventType.c_str(), strCallId.c_str(), bTimeout ? "notify-timeout" : "notify-failure" );
    gclsSubscriptionManager.RemoveSubscription( strCallId );
}

bool CModuleDispatcher::RecvResponse( int iThreadId, CSipMessage *pclsMessage ) {
    (void)iThreadId;
    if ( pclsMessage == NULL ) return false;

    // RouteSet 헬스체크(OPTIONS 프로브) 응답 — 프로브였으면 여기서 소비한다 (CspRouteHealth).
    if ( gclsRouteHealth.OnResponse( pclsMessage ) ) return true;

    ReapSubscriptionOnNotifyFailure( pclsMessage, pclsMessage->m_iStatusCode );

    // 응답 소비는 하지 않는다 — 뒤따르는 콜백(CSipUserAgent)의 처리를 막으면 안 된다.
    return false;
}

void CModuleDispatcher::EventKeepAlive( const char *pszIp, int iPort, ESipTransport eTransport ) {
    // keepalive 에는 신원이 없다 — 주소가 일치하는 바인딩의 생존 기록에만 쓴다.
    //   포트가 바뀐 단말은 여기서 살릴 수 없고, 재등록(인증)만이 복구할 수 있다.
    gclsUserMap.TouchKeepAlive( pszIp, iPort, eTransport );
}

bool CModuleDispatcher::SendTimeout( int iThreadId, CSipMessage *pclsMessage ) {
    (void)iThreadId;
    if ( pclsMessage == NULL ) return false;

    if ( gclsRouteHealth.OnSendTimeout( pclsMessage ) ) return true;
    ReapSubscriptionOnNotifyFailure( pclsMessage, 0 );
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

    // 설정된 피어에서 온 요청 — 인바운드 Route(InboundRouteOf: 수신 LocalNode + 소스 주소 → Route) 가 식별되고
    //   그 Route 의 inbound_auth 가 none 이면 상대는 가입자가 아니라 신뢰 피어 망이다(TS 24.229 §5.10 IBCF ·
    //   TS 29.165 II-NNI). Digest 챌린지를 하지 않는다 — 신뢰는 RemoteNode 설정과 RecvRequest 의 ACL(scope=route
    //   포함)이 세운다. 접속점 edge 는 보지 않는다(피어가 access 접속점으로 와도 Route 가 있으면 피어, 피어링 접속점에
    //   Route 없는 소스는 RecvRequest 가 이미 403). inbound_auth=digest(등록형 트렁크)는 아래 가입자 인증 흐름 그대로.
    //   라우팅 정책이 트렁크를 고른 호(PendingRouteMap)라고 인증을 건너뛰지는 않는다 — 미등록 발신자가 트렁크
    //   번호만 부르면 무인증으로 나가던 구멍(toll fraud)이었다. 발신 UE 는 등록 바인딩으로 인증된다.
    {
        std::string strLocalNodeName;
        if ( pclsMessage->m_iListenerId > 0 ) {
            LocalNodeInfo clsLn = gclsLocalNodeMap.GetByIntId( pclsMessage->m_iListenerId );
            if ( clsLn.IsValid() ) strLocalNodeName = clsLn.name;
        }
        RouteConfig clsInRoute = InboundRouteOf( pclsMessage, strLocalNodeName );
        if ( clsInRoute.IsValid() &&
             ( clsInRoute.TrustsInbound() || TrunkRegisteredSource( clsInRoute, pclsMessage ) ) )
            return true;
    }

    // ⚠️ 테스트 환경 전용 (Setup.TestEnvOpenTermination=true) — 상용 원복 대상.
    //   착신(To)이 로컬 가입자/그룹인 INVITE 는 발신자 401 챌린지를 생략하고 통과시킨다
    //   (수신 통화 허용). 비가입자 착신 INVITE 는 RecvRequest 에서 이미 603 처리되어
    //   여기 도달하지 않는다. 상용(내부망)은 플래그 off → 표준 챌린지 흐름 사용.
    if ( gclsSetup.m_bTestEnvOpenTermination && pclsMessage->IsMethod( SIP_METHOD_INVITE ) ) {
        const std::string &strToUser = pclsMessage->m_clsTo.m_clsUri.m_strUser;
        CspUser clsToProv;
        if ( gclsGroupMap.Contains( strToUser.c_str() ) || gclsCspUserMap.isAlive( strToUser.c_str(), clsToProv ) ||
             gclsDbManager.SelectUser( strToUser, clsToProv ) ) {
            return true;
        }
    }

    // 채널 정책 게이트 (sip_access_security.md §3.2) — 인증·주소 변경 판정보다 앞.
    //   TLS 정책 가입자의 신원으로 평문 채널에서 온 요청은 유효한 Digest 가 있어도 403.
    //   TLS 채널이면 아래 기존 판정으로 계속: 등록된 TLS 바인딩과 일치 → TouchFlow,
    //   새 TLS 연결 → Digest 재인증 후 SetIpPort (TLS→TLS 이동만 성립 — 다른 transport
    //   바인딩은 없으므로 SetIpPort 가 자연히 무시한다).
    if ( CCscfModule::CheckChannelPolicy( pclsMessage ) == false ) return false;

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
        // 비REGISTER 요청의 주소 변경 감지 — 그 transport 의 **기존 바인딩만** 옮긴다.
        //   바인딩을 만들 권한은 등록에만 있으므로(RFC 3261 §10), 승격 TCP 로 온 요청은 해당
        //   transport 의 바인딩이 없어 자연히 무시된다 — 종전의 transport 일치 가드는
        //   SetIpPort 안의 판정과 중복이라 제거했다(registration_binding_set.md §3).
        gclsUserMap.SetIpPort( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), strIp.c_str(), iPort,
                               pclsMessage->m_eTransport );
    } else {
        // 저장된 도달 경로 그대로 도착한 요청 = 그 latch 가 아직 살아 있다는 근거 (진단용).
        gclsUserMap.TouchFlow( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), pclsMessage->m_eTransport );
    }

    return true;
}

// VoLTE relay 미디어 SRTP(SDES) leg 별 종단 헬퍼는 MediaSdes 네임스페이스에 있다
//   (EvalRelayOfferSdes/ApplyRelayLegOffer/EvalRelayAnswerSdes/ReadReinviteSdes/RewriteRelaySdpForLeg
//    — media_security.md §5.2. TasModule 의 픽업·전달 재고정과 공용).

void CModuleDispatcher::EventIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                           CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
    CLog::Print( LOG_DEBUG, "EventIncomingCall: CallId=%s From=%s To=%s", pszCallId, pszFrom, pszTo );
    CspUser clsUser;
    CUserInfo clsUserInfo;
    bool bRoutePrefix = false;
    std::string strBRouteName;  // B-leg 이 피어(RoutingPolicy 가 고른 Route)면 그 Route 이름 — RemoteNode
                                // transcode_codecs(cmp.md §11.2)
    std::string strTo;

    // 수신 INVITE 의 Replaces(RFC 3891) → TAS — 관제 BLF 당겨받기·표준 attended 완결. 헤더가 있으면
    //   대상 다이얼로그를 pszCallId 로 교체하고 여기서 종결한다(정상 라우팅 미진입).
    if ( m_clsTas.IsEnabled() &&
         m_clsTas.OnIncomingCall( pszCallId, pszFrom, pszTo, pclsRtp, pclsMessage ) == E_ROUTE_HANDLED )
        return;

    // 착신 해석 — Request-URI(sip user / tel host)가 라우팅 키(RFC 3261 §16.6)다. RecvRequest 가 다이얼 플랜(§2-10)으로
    //   +E.164 로 번역해 두었고, 여기서 같은 해석을 한 번 더 거친다(멱등 — tel: URI·To 폴백 경로까지 한 함수로).
    //   psip 는 To user 를 pszTo 로 넘기는데 tel:+82..(userinfo 없음) 단말은 그것이 비어 있었다(HM-TRCP 등 MMTEL).
    std::string strCalleeResolved;
    if ( pclsMessage ) {
        std::string strPlanSource;
        EDialPlanResult eDial = ResolveCallee( pclsMessage, InboundRouteFor( pclsMessage ), pszFrom ? pszFrom : "",
                                               strCalleeResolved, strPlanSource );
        if ( eDial == DIAL_PLAN_INCOMPLETE ) {
            CLog::Print( LOG_INFO, "EventIncomingCall: callee(%s) 번역 불가(plan=%s) → 484", strCalleeResolved.c_str(),
                         strPlanSource.c_str() );
            return StopCall( pszCallId, SIP_ADDRESS_INCOMPLETE );
        }
        if ( !strCalleeResolved.empty() && strcmp( strCalleeResolved.c_str(), pszTo ? pszTo : "" ) != 0 ) {
            CLog::Print( LOG_INFO, "EventIncomingCall: 착신 To(%s) → Request-URI %s (%s)", pszTo ? pszTo : "",
                         strCalleeResolved.c_str(), CspDialPlan::ResultName( eDial ) );
            pszTo = strCalleeResolved.c_str();
        }
    }

    if ( strlen( pszTo ) == 0 ) return StopCall( pszCallId, SIP_DECLINE );

    // MCPTT condition(emergency/imminent)·session-type 파싱 — INVITE 의 mcptt-info+xml (TS 24.379).
    //   condition 은 session-type 과 직교. ProcessGroupCall 로 전달해 floor tier·fan-out 광고에 반영.
    int iMcpttCond = 0;
    std::string strMcpttSessionType;
    if ( pclsMessage ) {
        CMcpttInfo clsMi = ParseMcpttInfo( pclsMessage->m_strBody );
        iMcpttCond = clsMi.Condition();
        strMcpttSessionType = clsMi.strSessionType;
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
    // MCPTT private call (1:1, TS 24.379 §11.1 on-demand): mcptt-info session-type=private.
    //   합성 2인 ephemeral 그룹(priv-<caller>-<callee>)을 만들어 기존 ProcessGroupCall 경로
    //   (fan-out·CMP 세션·teardown)를 그대로 재사용한다 — 별도 CMP 명령 없음, 계약 §A.1
    //   (mcptt_csp_cmp_roadmap_contract.md). affiliation 불요(멤버십 게이트 우회).
    //   floor 유무는 발신 offer 의 fmtp mc_no_floor_ctrl(G17)로 정한다 — off=full-duplex.
    if ( m_clsPttAs.IsEnabled() && strMcpttSessionType == "private" && !gclsGroupMap.Contains( pszTo ) ) {
        CspUser clsCallee;
        if ( !gclsCspUserMap.isAlive( pszTo, clsCallee ) ) {
            CLog::Print( LOG_INFO, "EventIncomingCall: private call target(%s) not registered → 480 [PTT-AS]", pszTo );
            // 시도 장부 — 사설콜도 PTT 시도다(임시 그룹을 만들어 ProcessGroupCall 로 가므로).
            //   이 경로는 그 함수 앞에서 끝나 기록이 없었다.
            //   사유는 **VoLTE 와 같은 `no_answer`** 다(480·408 → CallDir::_ReasonOfStatus).
            //   상대 단말이 꺼진 것은 우리 구성 문제가 아니라 상대 사정이므로 NER 이 면제한다
            //   (_NER_USER_REASONS) — `denied`(정책 거부)나 `error`(자원 실패)에 넣으면 실패
            //   사유 분포가 왜곡되고, 우리 결함과 상대 사정이 한 칸에 섞인다.
            //   응답은 그대로 480 이다.
            if ( gclsCallDir.IsEnabled() )
                gclsCallDir.PttAttempt( pszTo, "", pszFrom, "failed", "no_answer", "private_callee_offline",
                                        SIP_TEMPORARILY_UNAVAILABLE );
            return StopCall( pszCallId, SIP_TEMPORARILY_UNAVAILABLE );
        }
        std::string strPrivId = std::string( "priv-" ) + pszFrom + "-" + pszTo;
        // 새 발신의 floor 모드 — 싱글(floor on, 기본) vs 멀티(mc_no_floor_ctrl → off).
        McpttFmtp clsPrivFmtpChk;
        CGroupCallService::ParseMcpttFmtp( pclsRtp, clsPrivFmtpChk );
        const char *pszWantFloorCtl = clsPrivFmtpChk.iNoFloorCtrl ? "off" : "";
        // 잔존 ephemeral 그룹의 모드가 이번 발신과 다르면 재사용하지 않는다 — 이전 호의
        //   그룹이 남아(경합·앱 강제종료 등) 이후 모든 1:1 이 그 모드로 고정되는 오염 방지.
        //   CMP 그룹도 REMOVE 로 확실히 재생성한다 (ADD 멱등 경로는 floor_control 을 갱신하지 않음).
        {
            CspPttGroup clsPrivOld;
            if ( gclsGroupMap.Select( strPrivId.c_str(), clsPrivOld ) && clsPrivOld._floorControl != pszWantFloorCtl ) {
                CLog::Print( LOG_INFO,
                             "EventIncomingCall: private(%s) 잔존 그룹 모드 불일치(%s→%s) — 제거 후 재생성 [PTT-AS]",
                             strPrivId.c_str(), clsPrivOld._floorControl.empty() ? "on" : "off",
                             clsPrivFmtpChk.iNoFloorCtrl ? "off" : "on" );
                gclsCmpClient.RemoveGroup( strPrivId );
                gclsGroupMap.Remove( strPrivId.c_str() );
            }
        }
        if ( !gclsGroupMap.Contains( strPrivId.c_str() ) ) {
            CspPttGroup clsPriv;
            clsPriv.Clear();
            clsPriv._id = strPrivId;
            clsPriv._name = std::string( "private:" ) + pszFrom + "-" + pszTo;
            clsPriv._groupType = "private";
            clsPriv._requireAffiliation = false;  // 계약 §A.1 — 상대 MCPTT ID 직접 지정, 사전 편성 없음
            clsPriv._isAdhoc = true;              // 통화 종료 시 GroupMap 에서 제거(ephemeral)
            clsPriv._emergencyCall = true;        // 그룹문서 없음 — capability 축 공허, 긴급은 사용자 축
                                                  // 게이트(IsConditionInitAuthorized private 분기)
            if ( clsPrivFmtpChk.iNoFloorCtrl ) clsPriv._floorControl = "off";
            clsPriv._pusers.push_back( std::make_shared<CspPttUser>( pszFrom, 5, "participant", "" ) );
            clsPriv._pusers.push_back( std::make_shared<CspPttUser>( pszTo, 5, "participant", "" ) );
            gclsGroupMap.Insert( clsPriv );
            CLog::Print( LOG_INFO, "EventIncomingCall: private call session(%s) created floor_control=%s [PTT-AS]",
                         strPrivId.c_str(), clsPriv._floorControl.empty() ? "on" : clsPriv._floorControl.c_str() );
        }
        SetCallOwner( pszCallId, &m_clsPttAs );
        CSipCallRoute clsPrivRoute;
        clsUserInfo.GetCallRoute( clsPrivRoute );
        if ( gclsGroupCallService.ProcessGroupCall( strPrivId.c_str(), pszFrom, pszCallId, pclsRtp, &clsPrivRoute,
                                                    iMcpttCond ) )
            return;
        CLog::Print( LOG_INFO, "EventIncomingCall: private call(%s) failed → 403 [PTT-AS]", strPrivId.c_str() );
        return StopCall( pszCallId, SIP_FORBIDDEN );
    }

    // MCPTT ad hoc 그룹콜 (TS 22.179 Rel-18): 미프로비저닝 타겟 + INVITE resource-lists 멤버 →
    //   임시 그룹을 동적 생성(in-memory, 비영속 ephemeral). 이후 기존 ProcessGroupCall(on-demand)
    //   경로가 fan-out·teardown 까지 처리. requireAffiliation=false(사전 가입 없음).
    //   게이트: Setup.PttAdhocEnabled (시스템 정책 — 사용자 단위 인가는 MCPTT 프로파일 트랙에서).
    if ( m_clsPttAs.IsEnabled() && gclsSetup.m_bPttAdhocEnabled && !gclsGroupMap.Contains( pszTo ) && pclsMessage ) {
        std::vector<std::string> vecAdhoc = ParseResourceListUsers( pclsMessage->m_strBody );
        if ( !vecAdhoc.empty() ) {
            // 사용자 단위 ad hoc 개시 인가 (프로파일 allow_adhoc_call — 시스템 정책과 AND)
            CspUserProfile clsAdhocProf;
            if ( gclsDbManager.SelectUserProfile( pszFrom, clsAdhocProf ) >= 0 && !clsAdhocProf.m_bAllowAdhocCall ) {
                CLog::Print( LOG_INFO, "EventIncomingCall: ad-hoc by(%s) not authorised (user profile) → 403 [PTT-AS]",
                             pszFrom );
                // 시도 장부 — 이 경로도 `ProcessGroupCall` 앞에서 끝나므로 여기서 남긴다.
                if ( gclsCallDir.IsEnabled() )
                    gclsCallDir.PttAttempt( pszTo, "", pszFrom, "failed", "denied", "adhoc_not_authorised",
                                            SIP_FORBIDDEN );
                return StopCall( pszCallId, SIP_FORBIDDEN );
            }
            CspPttGroup clsAdhoc;
            clsAdhoc.Clear();
            clsAdhoc._id = pszTo;
            clsAdhoc._name = std::string( "adhoc:" ) + pszTo;
            clsAdhoc._groupType = "prearranged";  // on-demand 수명(마지막 이탈 시 teardown)
            clsAdhoc._requireAffiliation = false;
            clsAdhoc._isAdhoc = true;        // 통화 종료 시 GroupMap 에서 제거(ephemeral)
            clsAdhoc._emergencyCall = true;  // 그룹문서 없음 — capability 축 공허, 긴급 조건은 ad-hoc 위에 얹힘(§6)
            bool bHasInit = false;
            for ( const auto &m : vecAdhoc ) {
                clsAdhoc._pusers.push_back( std::make_shared<CspPttUser>( m, 5, "participant", "" ) );
                if ( m == pszFrom ) bHasInit = true;
            }
            if ( !bHasInit )
                clsAdhoc._pusers.push_back( std::make_shared<CspPttUser>( pszFrom, 5, "participant", "" ) );
            gclsGroupMap.Insert( clsAdhoc );
            CGroupCallService::EmitRegroupEvent( "created", pszTo, "ad-hoc" );
            CLog::Print( LOG_INFO, "EventIncomingCall: ad-hoc group(%s) created %zu members init(%s) [PTT-AS]", pszTo,
                         clsAdhoc._pusers.size(), pszFrom );
        }
    }

    // MCData media plane — SDP 에 m=message TCP/MSRP 가 있으면 그룹콜이 아닌 대용량 SDS
    //   INVITE (TS 24.282 §9.2.3). PTT-AS 그룹 분기보다 먼저 선점해야 한다.
    if ( m_clsMcDataAs.IsEnabled() && gclsMcDataMediaService.IsMsrpInvite( pclsRtp ) ) {
        gclsMcDataMediaService.OnIncomingMsrpInvite( pszCallId, pszFrom, pszTo, pclsRtp, pclsMessage );
        return;
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
        // 시도 장부 — 여기까지 온 PTT 발신은 **대상이 그룹도 등록 가입자도 아니다**(위 그룹·
        //   private·ad-hoc 분기를 전부 지나왔다). `ProcessGroupCall` 의 group_not_found 기록은
        //   이 경로에 닿지 않는다: 그룹이 맵에 없으면 그 함수를 아예 부르지 않기 때문이다.
        //   그래서 실패한 개시가 원천에 안 남아 성공률이 실제보다 높게 나온다(§8 Y6 잔여 —
        //   실측 2026-09-16: 4건 중 1건 성공인데 화면은 3건 중 1건으로 33.3%).
        //   현장에서 가장 흔한 고장이 이 경로다(단말 그룹 오설정·그룹 삭제 뒤 잔존 발신).
        //   **응답은 그대로 403 이다** — 와이어 동작은 바꾸지 않고 기록만 더한다.
        //   장부의 group 칸에 실제 발신 대상이 그대로 들어가므로 무엇을 눌렀는지 보인다.
        auto RejectPtt = [&]() {
            if ( gclsCallDir.IsEnabled() )
                gclsCallDir.PttAttempt( pszTo, "", pszFrom, "failed", "denied", "group_not_found", SIP_FORBIDDEN );
            return StopCall( pszCallId, SIP_FORBIDDEN );
        };
        if ( mode == "ptt" ) return RejectPtt();
        if ( bFromKnown && !clsFromUser.m_strServiceType.empty() && clsFromUser.m_strServiceType == "ptt" )
            return RejectPtt();
    }

    // 여기서부터는 **1:1 VoLTE 경로**다(그룹·private 분기는 위에서 끝났다). 이 구간의 거절은
    //   시도로 남긴다 — 기록하지 않으면 성공률·NER 의 분모에서 빠진다(F-54).
    //   그룹·private 거절에는 쓰지 않는다: volte 트리에 쓰면 집계가 svc='volte' 로 못 박아
    //   세므로(build_minutes) PTT 실패가 VoLTE 통계를 오염시킨다.
    auto RejectVoice = [&]( int iCode ) {
        if ( gclsCallDir.IsEnabled() ) gclsCallDir.VoipCallRejected( pszCallId, pszFrom, pszTo, iCode );
        return StopCall( pszCallId, iCode );
    };

    // G1 (2026-04-23): Routing policy 결정 (RecvRequest 에서 PendingRouteMap 에 넣어둔 것) 을 먼저 소비.
    //   있으면 callee 가 내부 가입자여도 외부 peer 로 B2BUA forward (Routing policy 가 우선).
    //   없으면 아래 내부 가입자 경로 (PTT 그룹 / legacy IBCF / TAS) 로 진행.
    //   Route 의 auth_user/password 는 Route map 재조회로 보강 (RemoteNode 에는 auth 정보 없음).
    bool v3Routed = false;
    PendingRouteEntry
        clsRoutePending;  // B 가 피어면 그 결정(RouteSet·Route·정책·해시키) — CallMap 라우팅 상태(재라우팅 근거)
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
            strBRouteName = pe.route_name;
            clsRoutePending = pe;
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
            if ( gclsGroupCallService.ProcessGroupCall( pszTo, pszFrom, pszCallId, pclsRtp, &clsRouteTemp,
                                                        iMcpttCond ) ) {
                SetCallOwner( pszCallId, &m_clsPttAs );
                return;
            }
        }

        // G10 (2026-04-23): 레거시 IBCF XML trunk (SipServerMap) 경로 제거.
        //   외부 peer 라우팅은 routing_policies + PendingRouteMap (G1) 으로 결정.
        //   여기까지 도달한 "내부에 없는 callee" 는 CallPickup(TAS) 외에는 NOT_FOUND.
        // 관제 그룹 대표번호(pilot) — 등록 그룹원 전원에게 병렬 포크 (dispatch_center.md §4.2, TryPickupDial 앞)
        if ( m_clsTas.IsEnabled() && m_clsTas.TryDispatchPilot( pszCallId, pszFrom, pszTo, pclsRtp, pclsMessage ) )
            return;
        if ( m_clsTas.IsEnabled() && m_clsTas.TryPickupDial( pszCallId, pszFrom, pszTo, pclsRtp ) ) return;
        return RejectVoice( SIP_NOT_FOUND );
    }

    if ( GetCallOwner( pszCallId ) == NULL ) SetCallOwner( pszCallId, &m_clsTas );

    // TAS: DND/착신거부 603, 착신전환 302
    //   거절은 모듈 안에서 응답하므로 시도 기록도 **모듈 안에서** 남긴다(여기서 `RejectVoice`
    //   를 쓰면 응답을 두 번 보낸다). 착신 식별자를 넘기는 이유가 그것이다 — 장부의 callee 가
    //   표·이력의 다른 자리와 같은 문자열이어야 한다. 착신전환 302 는 남기지 않는다(TasModule).
    if ( m_clsTas.IsEnabled() && m_clsTas.ApplyTerminationServices( pszCallId, pszFrom, pszTo, clsUser ) ) return;

    // B2BUA 호 설정
    if ( bRoutePrefix == false ) {
        if ( gclsUserMap.Select( pszTo, clsUserInfo ) == false ) return RejectVoice( SIP_NOT_FOUND );
    }

    int iStartPort = -1;
    int iStartPortB = -1;
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
    RelaySdesLeg clsSdesA, clsSdesB;             // leg 별 SDES 상태 — CallMap 기록용 (블록 밖 scope)
    RelayCodec::LegCodecs clsCodecA, clsCodecB;  // leg 별 오퍼 코덱(코덱 삽입 뒤) — CallMap 기록용
    if ( gclsSetup.m_bUseRtpRelay ) {
        // 녹취 경로: Recording 활성화 시 세션 디렉터리 사용
        std::string strRecordDir;
        if ( gclsSetup.m_bRecordEnable && gclsCallDir.IsEnabled() ) {
            strRecordDir = gclsCallDir.GetVoipDir( pszCallId, pszFrom, pszTo );
        }
        // 발신측 RTP 주소를 RELAY_ADD에 포함 (생성 + peer[0] 한번에)
        int iAudioPort = pclsRtp->GetAudioPort();
        if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
        int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;

        // sesid: 수신 INVITE의 Call-ID로 이미 발행되어 있으면 재사용, 없으면 발행
        strRelaySesId = gclsSipLogger.GetOrIssueSesId( pszCallId, pszFrom ? pszFrom : "" );

        // 발신(caller) leg NAT 판정 — SDP 선언 미디어 IP vs INVITE 실소스(received/rport).
        //   nat 면 CMP 가 peer0 전용 포트에서 목적지 latch 를 허용한다 (ue_nat_traversal.md §4-5).
        int iCallerNat = 0;
        std::string strCallerGuardIp;
        ServiceInfo clsVolteSvc = gclsServiceMap.GetForUser( pszFrom ? pszFrom : "", "volte" );
        {
            std::string strSigIp;
            int iSigPort = 0;
            if ( pclsMessage ) pclsMessage->GetTopViaIpPort( strSigIp, iSigPort );
            if ( strSigIp.empty() ) {
                CUserInfo clsFromInfo;
                if ( pszFrom && gclsUserMap.Select( pszFrom, clsFromInfo ) ) strSigIp = clsFromInfo.m_strIp;
            }
            if ( CCspServiceMap::EvalMediaNat( clsVolteSvc, pclsRtp->m_strIp, strSigIp, strCallerGuardIp ) ) {
                iCallerNat = 1;
                CLog::Print( LOG_INFO, "EventIncomingCall: caller leg NAT (svc=%s sdp=%s sig=%s guard=%s)",
                             clsVolteSvc.name.c_str(), pclsRtp->m_strIp.c_str(), strSigIp.c_str(),
                             strCallerGuardIp.c_str() );
            }
        }

        // ── 미디어 SRTP(SDES e2ae) — relay 는 crypto 를 leg 별로 종단한다 (media_security.md §5.2).
        //    A(발신) leg: offer 의 crypto ×정책 평가(키는 RELAY_ADD 로), B(착신) leg: 정책×착신
        //    바인딩 mediasec 능력으로 offer 형태 결정. 수신 crypto 라인은 정책 무관 strip (E2E 차단).
        CmpMediaCrypto clsCallerAudioCrypto, clsCallerVideoCrypto;
        {
            int iSdesAudio = MediaSdes::EvalRelayOfferSdes( clsVolteSvc.media_srtp, pclsRtp->m_clsMediaList, "audio",
                                                            clsSdesA.clsAudio );
            int iSdesVideo = MediaSdes::EvalRelayOfferSdes( clsVolteSvc.media_srtp, pclsRtp->m_clsMediaList, "video",
                                                            clsSdesA.clsVideo );
            if ( iSdesAudio < 0 || iSdesVideo < 0 ) {
                CLog::Print( LOG_INFO,
                             "EventIncomingCall: caller(%s) SDES offer not acceptable (svc=%s media_srtp=%s) → 488",
                             pszFrom, clsVolteSvc.name.c_str(), clsVolteSvc.media_srtp.c_str() );
                return RejectVoice( SIP_NOT_ACCEPTABLE_HERE );
            }
            if ( ( clsSdesA.clsAudio.bSrtp &&
                   !MediaSdes::BuildCmpKeys( clsSdesA.clsAudio.strSuite, clsSdesA.clsAudio.strUeKey,
                                             clsSdesA.clsAudio.strSrvKey, clsCallerAudioCrypto ) ) ||
                 ( clsSdesA.clsVideo.bSrtp &&
                   !MediaSdes::BuildCmpKeys( clsSdesA.clsVideo.strSuite, clsSdesA.clsVideo.strUeKey,
                                             clsSdesA.clsVideo.strSrvKey, clsCallerVideoCrypto ) ) ) {
                CLog::Print( LOG_ERROR, "EventIncomingCall: caller(%s) SRTP key build failed → 488", pszFrom );
                return RejectVoice( SIP_NOT_ACCEPTABLE_HERE );
            }

            bool bCalleeSdes = false;
            if ( clsVolteSvc.media_srtp == "required" ) {
                bCalleeSdes = true;  // 능력 미선언 단말 포함 SAVP 단일 (§4)
            } else if ( clsVolteSvc.media_srtp == "optional" && !bRoutePrefix && pszTo ) {
                CUserInfo clsCalleeInfo;
                if ( gclsUserMap.Select( pszTo, clsCalleeInfo ) ) bCalleeSdes = clsCalleeInfo.m_bMediaSecSdes;
            }
            MediaSdes::StripCrypto( pclsRtp->m_clsMediaList );
            if ( !MediaSdes::ApplyRelayLegOffer( pclsRtp->m_clsMediaList, "audio", bCalleeSdes, clsSdesB.clsAudio ) ||
                 !MediaSdes::ApplyRelayLegOffer( pclsRtp->m_clsMediaList, "video", bCalleeSdes, clsSdesB.clsVideo ) ) {
                CLog::Print( LOG_ERROR, "EventIncomingCall: callee(%s) SRTP key build failed → 500", pszTo );
                return RejectVoice( SIP_INTERNAL_SERVER_ERROR );
            }
            if ( clsSdesA.clsAudio.bSrtp || clsSdesB.clsAudio.bSrtp )
                CLog::Print(
                    LOG_INFO,
                    "EventIncomingCall: relay SDES caller[a=%d v=%d] callee[a=%d v=%d] suite=%s "
                    "(svc=%s media_srtp=%s)",
                    clsSdesA.clsAudio.bSrtp, clsSdesA.clsVideo.bSrtp, clsSdesB.clsAudio.bSrtp, clsSdesB.clsVideo.bSrtp,
                    clsSdesA.clsAudio.bSrtp ? clsSdesA.clsAudio.strSuite.c_str() : clsSdesB.clsAudio.strSuite.c_str(),
                    clsVolteSvc.name.c_str(), clsVolteSvc.media_srtp.c_str() );
        }

        // ── 피어 leg 코덱 삽입(cmp.md §11.2, TS 29.162 트랜스코딩 제어 모델) — 피어→가입자 오퍼에는 서비스 코덱(코덱
        // 테이블 top, AMR-WB)을,
        //    가입자→피어 오퍼에는 RemoteNode.transcode_codecs 를 더한다(있는 코덱은 그대로). leg 별 최종 코덱은 answer
        //    때 정하고(ApplyRelayAnswerLeg) 다르면 CMP 가 변환한다. 정책이 없으면 종전 동작(삽입 없음).
        {
            clsCodecA.offered = RelayCodec::AudioCodecs( pclsRtp->m_clsMediaList, &clsCodecA.tePt, &clsCodecA.teRtpmap,
                                                         &clsCodecA.teFmtp );
            std::vector<RelayCodec::CodecDesc> vecInsert;
            std::string strInLn;
            if ( pclsMessage && pclsMessage->m_iListenerId > 0 ) {
                LocalNodeInfo clsInLn = gclsLocalNodeMap.GetByIntId( pclsMessage->m_iListenerId );
                if ( clsInLn.IsValid() ) strInLn = clsInLn.name;
            }
            const RouteConfig clsARoute = pclsMessage ? InboundRouteOf( pclsMessage, strInLn ) : RouteConfig();
            if ( clsARoute.IsValid() && !bRoutePrefix ) {
                // A 가 피어(트렁크), B 가 가입자 — 가입자 leg 오퍼에 서비스 코덱
                RelayCodec::CodecDesc svc = RelayCodec::ServiceCodec();
                if ( svc.Valid() && !RelayCodec::Find( clsCodecA.offered, svc ) &&
                     RelayCodec::HasTranscodableSource( clsCodecA.offered, svc ) )
                    vecInsert.push_back( svc );
            }
            if ( bRoutePrefix && !strBRouteName.empty() ) {
                // B 가 피어 — 그 RemoteNode 정책 코덱
                RouteConfig clsBRoute = gclsRouteMap.GetByName( strBRouteName );
                RemoteNodeInfo clsBNode = gclsRemoteNodeMap.GetByName( clsBRoute.remote_node_ref );
                for ( const std::string &strName : clsBNode.transcode_codecs ) {
                    RelayCodec::CodecDesc d = RelayCodec::ByName( strName );
                    if ( d.name.empty() ) {
                        CLog::Print( LOG_ERROR, "EventIncomingCall: remote_node %s transcode_codecs '%s' 모름 — 건너뜀",
                                     clsBNode.name.c_str(), strName.c_str() );
                        continue;
                    }
                    // A 가 이미 낸 코덱은 그대로, A 의 어떤 코덱과도 변환 쌍이 안 되는 코덱은 끼우지 않는다 — 예: A 가
                    // PCMU 를
                    //   냈는데 PCMA 를 끼우면 PBX 가 PCMA 를 골라도 PCMU↔PCMA 변환이 없어 488 이 났다(tb48
                    //   TRUNK-PBX-OUTBOUND 실측). G.711 을 낸 A 는 삽입 없이 PBX 와 직접 맞고, AMR-WB 만 낸 A 에만
                    //   G.711 이 끼워진다.
                    if ( !RelayCodec::Find( clsCodecA.offered, d ) &&
                         RelayCodec::HasTranscodableSource( clsCodecA.offered, d ) )
                        vecInsert.push_back( d );
                }
            }
            if ( !vecInsert.empty() ) {
                int n = RelayCodec::InsertCodecs( pclsRtp->m_clsMediaList, vecInsert );
                std::string strList;
                for ( const RelayCodec::CodecDesc &d : vecInsert ) strList += ( strList.empty() ? "" : "," ) + d.name;
                CLog::Print( LOG_INFO,
                             "EventIncomingCall: codec insertion %s → %d codec(s) [%s] (A %s, B %s) CallId=%s",
                             clsARoute.IsValid() ? "peer→ue" : "ue→peer", n, strList.c_str(),
                             clsARoute.IsValid() ? clsARoute.remote_node_ref.c_str() : "ue",
                             bRoutePrefix ? "peer" : "ue", pszCallId );
            }
            clsCodecB.offered = RelayCodec::AudioCodecs( pclsRtp->m_clsMediaList, &clsCodecB.tePt, &clsCodecB.teRtpmap,
                                                         &clsCodecB.teFmtp );
        }

        // CMP relay 생성: session_id(전역 유일) 발행 후 RELAY_ADD 직접 전송.
        //   (구 gclsRtpMap.CreatePort 대체 — 포트단독키 bookkeeping 제거. 멀티 미디어노드에서 포트가
        //    노드별 비유일이라 포트키 충돌로 teardown 이 엉뚱한 세션을 회수→relay 누수하던 근본버그 제거.)
        strRelaySessionId = CCmpClient::IssueSessionId();
        std::string strAllocatedIp;
        int iLocalPort = 0, iLocalVideoPort = 0, iLocalPortB = 0, iLocalVideoPortB = 0;
        // 발신(A) leg PT/코덱 — 서버 answer 는 오퍼 echo(bServerOffered=false). CMP leg 별
        //   PT 재작성 + 녹취 세그먼트 메타(audio_pt_a/audio_codec_a) 근거 (cmp_media_api.md §6.1).
        int iCallerPt = 0, iCallerSrcPt = 0, iCallerTePt = 0, iCallerSrcTePt = 0;
        std::string strCallerCodec;
        CGroupCallService::GetLegPt( pszCallId, false, iCallerPt, iCallerSrcPt, iCallerTePt, iCallerSrcTePt,
                                     &strCallerCodec );
        if ( !gclsCmpClient.AddSession( strRelaySessionId, strAllocatedIp, iLocalPort, iLocalVideoPort, iLocalPortB,
                                        iLocalVideoPortB, strRecordDir, pszFrom ? pszFrom : "", pszTo ? pszTo : "",
                                        pclsRtp->m_strIp, iAudioPort, iVideoPort, strRelaySesId, iCallerNat,
                                        strCallerGuardIp, iCallerPt, iCallerSrcPt, iCallerTePt, iCallerSrcTePt,
                                        strCallerCodec, clsCallerAudioCrypto.bEnabled ? &clsCallerAudioCrypto : NULL,
                                        clsCallerVideoCrypto.bEnabled ? &clsCallerVideoCrypto : NULL ) ) {
            return RejectVoice( SIP_INTERNAL_SERVER_ERROR );
        }
        // leg 별 전용 포트: A(발신) leg SDP = iLocalPort(peer0), B(착신) leg SDP = iLocalPortB(peer1)
        iStartPort = iLocalPort;
        iStartPortB = iLocalPortB;
        strRelayLocalIp = strAllocatedIp;

        std::string strRelayIp = CspAddressing::GetLocalRtpAddress();
        if ( !strAllocatedIp.empty() ) {
            strRelayIp = strAllocatedIp;
            strMediaNode = strAllocatedIp;  // CMP 노드 relay IP = 처리 미디어 노드
        }
        pclsRtp->SetIpPort( strRelayIp.c_str(), iStartPortB, SOCKET_COUNT_PER_MEDIA );
    }

    clsUserInfo.GetCallRoute( clsRoute );
    clsRoute.m_b100rel = gclsUserAgent.Is100rel( pszCallId );
    // T3: route 결정으로 추출한 outbound local identity hint 를 dialog 까지 전달.
    //     hint 미설정 시 stack primary fallback (NO-OP regression).
    if ( !strOutboundLocalIp.empty() ) clsRoute.m_strOutboundLocalIp = strOutboundLocalIp;
    if ( iOutboundLocalPort > 0 ) clsRoute.m_iOutboundLocalPort = iOutboundLocalPort;

    // 발신 신원 표시 — 관제 그룹원이 P-Preferred-Identity 로 자기 그룹 대표번호를 제시하면 B-leg From(= psip 가 같은
    //   값으로 넣는 P-Asserted-Identity)을 대표번호로 낸다(dispatch_center.md §4.7). CallMap·CDR·dialog 당사자는
    //   실제 발신자(pszFrom) 그대로 — 아래 모든 기록은 pszFrom 을 쓴다.
    const std::string strPresentFrom = m_clsTas.IsEnabled()
                                           ? m_clsTas.ResolveOriginatingIdentity( pszFrom, pclsMessage )
                                           : std::string( pszFrom ? pszFrom : "" );
    CSipMessage *pclsInvite;
    if ( gclsUserAgent.CreateCall( strPresentFrom.c_str(), pszTo, pclsRtp, &clsRoute, strCallId, &pclsInvite ) ==
         false ) {
        // [LEAK-FIX] B-leg INVITE 생성 실패 — 직전 AddSession 으로 만든 CMP relay 가 CallMap 등록
        //   전이라 추적 불가(고아) 상태로 누수된다. 여기서 즉시 RemoveSession 으로 회수한다.
        //   (호 실패 시 주요 RTP 누수 경로 — session_id 로 직접 회수.)
        if ( !strRelaySessionId.empty() ) {
            CLog::Print( LOG_INFO, "CreateCall failed — freeing orphan CMP relay session=%s callid=%s",
                         strRelaySessionId.c_str(), pszCallId );
            gclsCmpClient.RemoveSession( strRelaySessionId, pszFrom ? pszFrom : "", pszTo ? pszTo : "", strRelaySesId );
        }
        return RejectVoice( SIP_INTERNAL_SERVER_ERROR );
    }

    // P-Asserted-Identity 는 psip(CSipDialog::CreateMessage)가 발신 leg 도메인 기준으로
    //   이미 1개 삽입한다(동일 값). 여기서 재삽입하면 동일 PAID 가 2개 되어 RFC 3325
    //   위반(scheme 당 1개) → 중복 삽입 제거.

    // leg 별 포트: 각 entry 의 m_iPeerRtpPort = "그 leg 의 peer 에게 광고하는 relay 포트".
    //   A(수신) entry = peer1 포트(B leg SDP 용), B(발신) entry = peer0 포트(A leg SDP 용).
    gclsCallMap.Insert( pszCallId, strCallId.c_str(), iStartPortB, iStartPort );
    SetCallOwner( strCallId.c_str(), GetCallOwner( pszCallId ) );
    // 피어 B-leg 의 라우팅 상태 — 5xx·타임아웃이면 같은 RouteSet 의 다음 멤버로 재라우팅(TryRerouteLeg, §2-4)
    if ( v3Routed && !clsRoutePending.route_set.empty() )
        gclsCallMap.SetRouteInfo( pszCallId, clsRoutePending.route_set, clsRoutePending.route_name,
                                  clsRoutePending.policy_name, clsRoutePending.hash_key, std::vector<std::string>() );

    // CMP relay descriptor 를 양 leg(수신/발신 Call-ID)에 기록 → teardown(BYE)·answer MODIFY 가
    //   포트가 아닌 session_id 로 CMP 세션을 직접 지목 (포트충돌 오지목/누수 차단).
    if ( !strRelaySessionId.empty() ) {
        gclsCallMap.SetRelayInfo( pszCallId, strRelaySessionId, strRelaySesId, strRelayLocalIp, pszFrom ? pszFrom : "",
                                  pszTo ? pszTo : "" );
        // leg 별 SDES 협상 상태 — answer 재작성(offer echo)·re-INVITE 키 유지/갱신의 원천 (§5.2)
        gclsCallMap.SetRelaySdesLeg( pszCallId, 0, clsSdesA );
        gclsCallMap.SetRelaySdesLeg( pszCallId, 1, clsSdesB );
        // leg 별 오퍼 코덱(코덱 삽입 뒤) — answer 때 협상 코덱 판정·변환 결정의 원천 (cmp.md §11.2)
        gclsCallMap.SetRelayCodecLeg( pszCallId, 0, clsCodecA );
        gclsCallMap.SetRelayCodecLeg( pszCallId, 1, clsCodecB );
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
        return RejectVoice( SIP_INTERNAL_SERVER_ERROR );
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

/** answer leg 의 SDP 를 relay(CMP)에 반영한다 — SDES 검증·UE 키 확정, NAT 판정, PT, RELAY_MODIFY(peer_index=answer
 * leg). 확정 answer(200, EventCallStart)와 **18x 의 SDP**(early media, EventCallRing)가 같은 절차를 쓴다: 18x+SDP 는 그
 * 다이얼로그의 answer 이고(RFC 3264 §5·RFC 3262 §5 — 뒤의 200 은 같은 SDP), 미디어 앵커가 그때 B-leg 주소를 알아야
 * early media(링백·안내음, RFC 3960)가 relay 를 지난다. 18x 에서 이미 반영한 키가 200 에서 그대로면 CMP SRTP 컨텍스트를
 * 다시 만들지 않는다 (media_crypto 생략 — 재생성은 replay 창·ROC 를 버린다). 주소가 같은 재-MODIFY 는 CMP 가 latch 를
 * 유지한다. 반환 false = SAVP offer 에 crypto 없는/불일치 answer (호출자가 평문 폴백 없이 처리). */
/** ApplyRelayAnswerLeg 의 코덱 판정 결과 — 변환 호면 상대 leg(A)로 나가는 answer 를 A 코덱으로 재작성해야
 * 한다(RelayCodec::RewriteAudio). */
struct RelayAnswerCodec {
    bool bTranscode = false;
    bool bReject = false;  // 성립 불가(공통 코덱도 변환 쌍도 없음) 또는 CMP 변환 자원 없음 → 488
    RelayCodec::CodecDesc clsCodecA;
    int iTePtA = -1;
    std::string strTeRtpmapA, strTeFmtpA;
};

static bool ApplyRelayAnswerLeg( const char *pszCallId, const CCallInfo &clsCallInfo, CSipCallRtp *pclsRtp,
                                 const char *pszWhere, RelayAnswerCodec *pclsOut = NULL ) {
    if ( clsCallInfo.m_strRelaySessionId.empty() ) return true;
    // answer leg 의 relay peer index — 통상 착신 leg=peer1 이지만, 전달로 재구성된 pair 는
    //   answer leg 가 peer0 을 승계할 수 있다. m_bRecv(=peer0 표식)로 일반화.
    const int iAnswerIdx = clsCallInfo.m_bRecv ? 0 : 1;
    // ── answer leg SDES 검증·UE 키 확정 (media_security.md §5.2) — UE 키는 CMP 해당 peer rx 로 내린다.
    RelaySdesLeg clsSdesB = clsCallInfo.m_clsSdesLeg[iAnswerIdx];
    const std::string strPrevAudioKey = clsSdesB.clsAudio.strUeKey, strPrevVideoKey = clsSdesB.clsVideo.strUeKey;
    CmpMediaCrypto clsCalleeAudioCrypto, clsCalleeVideoCrypto;
    if ( !MediaSdes::EvalRelayAnswerSdes( pclsRtp->m_clsMediaList, "audio", clsSdesB.clsAudio, clsCalleeAudioCrypto ) ||
         !MediaSdes::EvalRelayAnswerSdes( pclsRtp->m_clsMediaList, "video", clsSdesB.clsVideo,
                                          clsCalleeVideoCrypto ) ) {
        CLog::Print( LOG_ERROR, "%s: callee(%s) SDES answer missing/mismatched crypto on SAVP offer (CallId=%s)",
                     pszWhere, clsCallInfo.m_strRelayCallee.c_str(), pszCallId );
        return false;
    }
    gclsCallMap.SetRelaySdesLeg( pszCallId, iAnswerIdx, clsSdesB );
    const bool bAudioKeyKept = !strPrevAudioKey.empty() && strPrevAudioKey == clsSdesB.clsAudio.strUeKey;
    const bool bVideoKeyKept = !strPrevVideoKey.empty() && strPrevVideoKey == clsSdesB.clsVideo.strUeKey;

    // answer leg RTP 주소를 CMP 에 MODIFY (peer_index=iAnswerIdx) — session_id 로 직접 지목.
    int iAudioPort = pclsRtp->GetAudioPort();
    if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
    int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;
    if ( iAudioPort <= 0 ) return true;
    // 착신(callee) leg NAT 판정 — answer SDP IP vs 착신 등록 바인딩(received/rport latch).
    int iCalleeNat = 0;
    std::string strCalleeGuardIp;
    {
        std::string strCalleeId;
        gclsUserAgent.GetToId( pszCallId, strCalleeId );
        ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( strCalleeId, "volte" );
        std::string strSigIp;
        CUserInfo clsToInfo;
        if ( !strCalleeId.empty() && gclsUserMap.Select( strCalleeId.c_str(), clsToInfo ) )
            strSigIp = clsToInfo.m_strIp;
        if ( CCspServiceMap::EvalMediaNat( clsNatSvc, pclsRtp->m_strIp, strSigIp, strCalleeGuardIp ) ) {
            iCalleeNat = 1;
            CLog::Print( LOG_INFO, "%s: callee leg NAT (svc=%s sdp=%s sig=%s guard=%s)", pszWhere,
                         clsNatSvc.name.c_str(), pclsRtp->m_strIp.c_str(), strSigIp.c_str(), strCalleeGuardIp.c_str() );
        }
    }
    // 착신(B) leg PT/코덱 — 서버 offer(코덱 테이블) vs 착신 answer wire PT
    //   (bServerOffered=true). 녹취 세그먼트 메타(audio_pt_b/audio_codec_b) 근거.
    int iCalleePt = 0, iCalleeSrcPt = 0, iCalleeTePt = 0, iCalleeSrcTePt = 0;
    std::string strCalleeCodec;
    CGroupCallService::GetLegPt( pszCallId, true, iCalleePt, iCalleeSrcPt, iCalleeTePt, iCalleeSrcTePt,
                                 &strCalleeCodec );

    // ── leg 별 협상 코덱(cmp.md §11.2) — answer leg(B)의 첫 오디오 코덱 vs 상대 leg(A) 오퍼. A 오퍼에 있으면 relay(PT
    // 만 leg 별),
    //    없으면 변환 가능 쌍(AMR-WB↔G.711)이어야 한다 — 아니면 488. 변환이면 양 leg 에 media_codec 을 실어 CMP 변환
    //    유닛을 붙인다.
    RelayCodec::LegCodecs clsLegB = clsCallInfo.m_clsCodecLeg[iAnswerIdx];
    RelayCodec::LegCodecs clsLegA = clsCallInfo.m_clsCodecLeg[1 - iAnswerIdx];
    int iAnsTePt = -1;
    std::vector<RelayCodec::CodecDesc> vecAns = RelayCodec::AudioCodecs( pclsRtp->m_clsMediaList, &iAnsTePt );
    CmpMediaCodec clsCodecB, clsCodecA;
    bool bTranscode = false;
    if ( !vecAns.empty() && !clsLegA.offered.empty() ) {
        RelayCodec::CodecDesc negB = vecAns.front(), negA;
        const int iDecide = RelayCodec::DecideLeg( clsLegA.offered, negB, negA );
        if ( iDecide < 0 ) {
            std::string strA;
            for ( const RelayCodec::CodecDesc &c : clsLegA.offered ) strA += ( strA.empty() ? "" : "," ) + c.Label();
            CLog::Print( LOG_ERROR, "%s: no common/transcodable codec — answer %s vs offer [%s] → 488 (CallId=%s)",
                         pszWhere, negB.Label().c_str(), strA.c_str(), pszCallId );
            if ( pclsOut ) pclsOut->bReject = true;
            return false;
        }
        bTranscode = ( iDecide == 1 );
        clsLegB.negotiated = negB;
        clsLegA.negotiated = negA;
        clsLegA.transcode = clsLegB.transcode = bTranscode;
        gclsCallMap.SetRelayCodecLeg( pszCallId, iAnswerIdx, clsLegB );
        gclsCallMap.SetRelayCodecLeg( pszCallId, 1 - iAnswerIdx, clsLegA );
        // B leg 의 wire PT 는 answer 가 말한 값(코덱 테이블 top 이 아니어도 — G.711 피어)
        iCalleePt = iCalleeSrcPt = negB.pt;
        if ( iAnsTePt >= 0 ) iCalleeTePt = iCalleeSrcTePt = iAnsTePt;
        strCalleeCodec = negB.Label();
        if ( bTranscode ) {
            clsCodecB = CmpMediaCodec::From( negB );
            clsCodecA = CmpMediaCodec::From( negA );
            CLog::Print( LOG_SYSTEM, "%s: transcode %s(peer%d) <-> %s(peer%d) (CallId=%s)", pszWhere,
                         negB.Label().c_str(), iAnswerIdx, negA.Label().c_str(), 1 - iAnswerIdx, pszCallId );
        }
        if ( pclsOut ) {
            pclsOut->bTranscode = bTranscode;
            pclsOut->clsCodecA = negA;
            pclsOut->iTePtA = clsLegA.tePt;
            pclsOut->strTeRtpmapA = clsLegA.teRtpmap;
            pclsOut->strTeFmtpA = clsLegA.teFmtp;
        }
    }
    const bool bOkB = gclsCmpClient.ModifySession(
        clsCallInfo.m_strRelaySessionId, pclsRtp->m_strIp, iAudioPort, iVideoPort > 0 ? iVideoPort : 0, iAnswerIdx,
        clsCallInfo.m_strRelayCaller, clsCallInfo.m_strRelayCallee, clsCallInfo.m_strRelaySesId, iCalleeNat,
        strCalleeGuardIp, iCalleePt, iCalleeSrcPt, iCalleeTePt, iCalleeSrcTePt, strCalleeCodec,
        ( clsCalleeAudioCrypto.bEnabled && !bAudioKeyKept ) ? &clsCalleeAudioCrypto : NULL,
        ( clsCalleeVideoCrypto.bEnabled && !bVideoKeyKept ) ? &clsCalleeVideoCrypto : NULL,
        bTranscode ? &clsCodecB : NULL );
    if ( !bTranscode && clsLegA.negotiated.Valid() && !clsLegA.negotiated.Same( RelayCodec::ServiceCodec() ) ) {
        // relay 인데 A 의 코덱이 서비스 코덱(코덱 테이블 top)이 아니다(예 UE 가 PCMU 오퍼 → G.711 relay) — RELAY_ADD 는
        // top PT 로 갔으니 A leg PT 를
        //   협상 값으로 바로잡는다(주소 미변경). 종전엔 egress 에 top PT(96) 가 스탬프되어 규격 준수 단말이 버릴 수
        //   있었다.
        gclsCmpClient.ModifySession( clsCallInfo.m_strRelaySessionId, "", 0, 0, 1 - iAnswerIdx,
                                     clsCallInfo.m_strRelayCaller, clsCallInfo.m_strRelayCallee,
                                     clsCallInfo.m_strRelaySesId, 0, "", clsLegA.negotiated.pt, clsLegA.negotiated.pt,
                                     clsLegA.tePt > 0 ? clsLegA.tePt : 0, clsLegA.tePt > 0 ? clsLegA.tePt : 0,
                                     clsLegA.negotiated.Label() );
    }
    if ( bTranscode ) {
        // A leg — 주소는 그대로(remote_port 0 = 미변경), PT·코덱 선언만. 둘 중 하나라도 CMP 가
        // 거절(TRANSCODE_CAPACITY)하면 488 — 무음 relay 를 만들지 않는다
        const bool bOkA =
            bOkB && gclsCmpClient.ModifySession(
                        clsCallInfo.m_strRelaySessionId, "", 0, 0, 1 - iAnswerIdx, clsCallInfo.m_strRelayCaller,
                        clsCallInfo.m_strRelayCallee, clsCallInfo.m_strRelaySesId, 0, "", clsLegA.negotiated.pt,
                        clsLegA.negotiated.pt, clsLegA.tePt > 0 ? clsLegA.tePt : 0, clsLegA.tePt > 0 ? clsLegA.tePt : 0,
                        clsLegA.negotiated.Label(), NULL, NULL, &clsCodecA );
        if ( !bOkA ) {
            CLog::Print( LOG_ERROR, "%s: CMP transcode setup failed (capacity?) → 488 (CallId=%s)", pszWhere,
                         pszCallId );
            if ( pclsOut ) pclsOut->bReject = true;
            return false;
        }
    }
    return true;
}

void CModuleDispatcher::EventCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp ) {
    CCallInfo clsCallInfo;
    CLog::Print( LOG_DEBUG, "EventCallRing(%s,%d)", pszCallId, iSipStatus );

    // TAS — 대표번호 포크 대기 leg 18x(소비: 첫 180 만 A 에 전달) / dialog-event 링잉(early) 통지(BLF §6.2,
    //   CallMap leg — 통과) / blind transfer 진행 NOTIFY (trans leg — 소비)
    if ( m_clsTas.IsEnabled() && m_clsTas.OnCallRing( pszCallId, iSipStatus, pclsRtp ) ) return;

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            // 18x 의 SDP(early media) = 미디어 앵커링 대상 — 확정 answer 와 같은 절차로 answer leg 를 relay 에 반영한
            // 뒤
            //   (B-leg 주소·키가 CMP 에 있어야 링백이 relay 를 지난다), 상대 leg 로 나가는 SDP 를 relay 주소로
            //   재작성한다. (포크 대기 leg 의 18x 는 위 TAS 가 소비한다 — 여기 오는 것은 CallMap pair 의 단일 B-leg.)
            //   SAVP offer 에 crypto 가 어긋난 18x 는 SDP 를 떼고 전달한다(early media 없음 — 호 종료 판정은 200 에서).
            RelayAnswerCodec clsAns;
            if ( !ApplyRelayAnswerLeg( pszCallId, clsCallInfo, pclsRtp, "EventCallRing", &clsAns ) ) {
                pclsRtp = NULL;
            } else {
                // 변환 호 — 상대 leg(A)로 가는 early answer 는 A 의 코덱으로(B 의 G.711 SDP 를 그대로 넘기면 A 가
                // 488/무음)
                if ( clsAns.bTranscode )
                    RelayCodec::RewriteAudio( pclsRtp->m_clsMediaList, clsAns.clsCodecA, clsAns.iTePtA,
                                              clsAns.strTeRtpmapA, clsAns.strTeFmtpA );
                // 링잉 leg crypto 투과 차단 + 전달받는 leg 상태로 재광고 (§5.2). leg index 는 m_bRecv(=peer0 표식)로
                //   판정 — 전달·픽업 재결합 pair 는 남는 쪽이 peer1 일 수 있다.
                if ( !clsCallInfo.m_strRelaySessionId.empty() )
                    MediaSdes::RewriteRelaySdpForLeg( pclsRtp->m_clsMediaList,
                                                      clsCallInfo.m_clsSdesLeg[clsCallInfo.m_bRecv ? 1 : 0], false );
                // m= 포트·미디어 레벨 c= 까지 relay 로 — m_iPort/m_strIp 만 바꾸면 미디어 목록이 있는 SDP 는 원래
                // 포트로 나간다
                pclsRtp->SetIpPort( clsCallInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress().c_str()
                                                                          : clsCallInfo.m_strRelayLocalIp.c_str(),
                                    clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
            }
        }
        int iRSeq = gclsUserAgent.GetRSeq( pszCallId );
        if ( iRSeq != -1 ) gclsUserAgent.SetRSeq( clsCallInfo.m_strPeerCallId.c_str(), iRSeq );
        gclsUserAgent.RingCall( clsCallInfo.m_strPeerCallId.c_str(), iSipStatus, pclsRtp );
    }
}

void CModuleDispatcher::EventCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    CCallInfo clsCallInfo;
    CLog::Print( LOG_DEBUG, "EventCallStart(%s)", pszCallId );

    // MCData media plane 레그 — CallMap 밖에서 자체 수명 관리 (미선점 시 아래 else 가 StopCall)
    if ( gclsMcDataMediaService.OnCallStarted( pszCallId, pclsRtp ) ) return;

    // 확립(answer) 표시 — sweeper 가 미확립(pending) 호만 빠르게 회수하도록.
    gclsCallMap.SetEstablished( pszCallId );

    // TAS — dialog-event 확립(confirmed) 통지(§6.2, CallMap leg — 통과) / blind transfer 완결
    //   (trans leg 재고정·재결합 — 소비, 여기서 종결)
    if ( m_clsTas.IsEnabled() && m_clsTas.OnCallStart( pszCallId, pclsRtp ) ) return;

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
            // answer leg 의 relay peer index — 통상 착신 leg=peer1 이지만, 전달로 재구성된 pair 는
            //   answer leg 가 peer0 을 승계할 수 있다. m_bRecv(=peer0 표식)로 일반화.
            const int iAnswerIdx = clsCallInfo.m_bRecv ? 0 : 1;
            // ── answer leg 를 relay 에 반영 — SDES 검증·UE 키 확정·NAT·PT·RELAY_MODIFY (media_security.md §5.2).
            //    SAVP offer 에 crypto 없는/불일치 answer 는 종료(평문 폴백 금지). 18x 에서 이미 반영했으면 같은 값의
            //    재확인이다.
            RelayAnswerCodec clsAns;
            if ( !ApplyRelayAnswerLeg( pszCallId, clsCallInfo, pclsRtp, "EventCallStart", &clsAns ) ) {
                // SAVP 불일치(평문 폴백 금지) 또는 코덱 성립 불가/CMP 변환 자원 없음 — 양 leg 종료. 미응답 A 에는 488
                // Not Acceptable Here
                CLog::Print( LOG_ERROR, "EventCallStart: %s, 호 종료 (CallId=%s)",
                             clsAns.bReject ? "코덱 성립 불가/변환 자원 없음 → 488" : "평문 폴백 금지", pszCallId );
                gclsUserAgent.StopCall( pszCallId );
                if ( clsAns.bReject && !clsCallInfo.m_strPeerCallId.empty() &&
                     !gclsUserAgent.IsConnected( clsCallInfo.m_strPeerCallId.c_str() ) )
                    gclsUserAgent.StopCall( clsCallInfo.m_strPeerCallId.c_str(), SIP_NOT_ACCEPTABLE_HERE );
                return;
            }
            // 변환 호 — 상대 leg(A)로 가는 answer 는 A 의 협상 코덱(+A 의 telephone-event)으로 재작성 (cmp.md §11.2)
            if ( clsAns.bTranscode )
                RelayCodec::RewriteAudio( pclsRtp->m_clsMediaList, clsAns.clsCodecA, clsAns.iTePtA, clsAns.strTeRtpmapA,
                                          clsAns.strTeFmtpA );

            int iRemoteAudio = pclsRtp->GetAudioPort();
            if ( iRemoteAudio <= 0 && pclsRtp->m_iPort > 0 ) iRemoteAudio = pclsRtp->m_iPort;
            if ( iRemoteAudio > 0 ) {
                int iRemoteVideo = pclsRtp->GetVideoPort();
                // SDP m=application floor control 포트 파싱 (≤0 이면 OnCallStarted 내부 fallback)
                int iRemoteFloor = pclsRtp->GetApplicationPort();
                gclsGroupCallService.OnCallStarted( pszCallId, pclsRtp->m_strIp, iRemoteAudio,
                                                    iRemoteFloor > 0 ? iRemoteFloor : 0, iRemoteVideo, pclsRtp );
            }

            // 상대 leg 로 나가는 SDP 재작성 — answer leg 키 투과 차단 + 상대 leg 상태로 재광고 (§5.2).
            //   AcceptCall(answer)은 상대 offer 의 tag/suite/protocol echo, SendReInvite(offer)는
            //   leg 상태로 protocol 결정.
            MediaSdes::RewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsCallInfo.m_clsSdesLeg[1 - iAnswerIdx],
                                              gclsUserAgent.IsConnected( clsCallInfo.m_strPeerCallId.c_str() ) );

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
    } else {
        gclsUserAgent.StopCall( pszCallId );
    }
}

/** 호의 통화시간(초) — CDR 의 응답 시각 ~ 종료 시각. 응답 전에 끝난 호는 0.
 *
 *  CallDir 의 마감은 **첫 종료 호출 한 번뿐**이다(같은 호가 인덱스에 두 줄로 남지 않게 걸어둔
 *  멱등 가드). 그래서 마감하는 그 자리에서 통화시간을 알아야 한다 — 뒤따르는 OnCallEnded 가
 *  실제 값을 알고 있어도 가드에 걸려 반영되지 못한다(그 탓에 duration 이 늘 0 이었다).
 *
 *  종료 시각이 아직 안 찍힌 시점에도 불릴 수 있어 그때는 현재 시각으로 본다.
 */
static int _CallDurationSec( const char *pszCallId ) {
    if ( !pszCallId || !*pszCallId ) return 0;
    CSipCdr clsCdr;
    if ( !gclsUserAgent.GetCdr( pszCallId, &clsCdr ) ) return 0;
    if ( clsCdr.m_sttStartTime.tv_sec == 0 ) return 0;
    time_t tEnd = clsCdr.m_sttEndTime.tv_sec ? clsCdr.m_sttEndTime.tv_sec : time( nullptr );
    int iDur = (int)( tEnd - clsCdr.m_sttStartTime.tv_sec );
    return iDur > 0 ? iDur : 0;
}

/**
 * @brief 피어 B-leg 실패 시 같은 RouteSet 의 다음 멤버로 재라우팅 — RFC 3261 §16.7 순차 forking(실패 응답 뒤 다음 대상
 * 시도), TS 24.229 §5.10 IBCF alternative routing, sip_service_model.md §2-4.
 *
 * 대상 = RoutingPolicy 가 고른 피어 B-leg(CallMap 라우팅 상태 있음)가 **확립 전** 5xx / 408 / 410(전송 타임아웃 — psip
 * SendTimeout) 으로 끝났을 때. 4xx(486·404 등)·6xx 는 상대가 판단을 내린 것이라 재라우팅하지 않는다(RFC 3261 §16.7 —
 * 6xx 는 즉시 종결). 헬스체크가 아직 dead 로 표시하지 않은 피어의 첫 실패도 여기서 받는다 — 헬스체크(§2-4)와 재라우팅은
 * 서로 보완한다.
 *
 * 절차: 이미 실패한 Route 를 제외해 RouteSet 재선택 → 실패 B-leg 가 냈던 오퍼(relay 재작성·코덱 삽입 포함,
 * GetLocalCallRtp) 그대로 새 B-leg INVITE(From/To 동일, 다음 홉 = 새 RemoteNode 또는 트렁크 바인딩) → CallMap 은 A↔새 B
 * 로 교체(relay·SDES·코덱 상태 복사, 실패 Route 를 tried 에 누적) → 세션 로그·sesid 승계 → 전송. 후보가 없으면 false 를
 * 돌려 종전 종료 경로로.
 */
bool CModuleDispatcher::TryRerouteLeg( const char *pszCallId, const CCallInfo &clsB, int iSipStatus ) {
    if ( clsB.m_bRecv || clsB.m_bEstablished || clsB.m_strRouteSet.empty() ) return false;
    const bool bRetriable = ( iSipStatus == SIP_REQUEST_TIME_OUT || iSipStatus == SIP_GONE ||
                              ( iSipStatus >= SIP_INTERNAL_SERVER_ERROR && iSipStatus < 600 ) );
    if ( !bRetriable ) return false;
    if ( !m_clsIbcf.IsEnabled() ) return false;

    const std::string strACallId = clsB.m_strPeerCallId;
    std::set<std::string> setExclude( clsB.m_vecRoutesTried.begin(), clsB.m_vecRoutesTried.end() );
    setExclude.insert( clsB.m_strRouteName );
    std::string strReason;
    const std::string strNext =
        gclsRouteSetMap.SelectRoute( clsB.m_strRouteSet, clsB.m_strRouteHashKey, strReason, setExclude );
    if ( strNext.empty() ) {
        CLog::Print( LOG_INFO,
                     "RouteSet failover: route='%s' set='%s' ended %d — 다음 멤버 없음(%s, tried=%zu) → 발신자에게 "
                     "전달 [callId=%s]",
                     clsB.m_strRouteName.c_str(), clsB.m_strRouteSet.c_str(), iSipStatus, strReason.c_str(),
                     setExclude.size(), pszCallId );
        return false;
    }
    RouteConfig rc = gclsRouteMap.GetByName( strNext );
    RemoteNodeInfo rn = gclsRemoteNodeMap.GetByName( rc.remote_node_ref );
    TrunkBinding tb;
    if ( rc.IsTrunkAccount() && gclsTrunkRegistrar.Get( rc.name, tb ) ) {
        rn.ip = tb.ip;
        rn.port = tb.port;
        rn.protocol = tb.transport;
    }
    if ( !rc.IsValid() || rn.ip.empty() || rn.port <= 0 ) {
        CLog::Print( LOG_ERROR, "RouteSet failover: next route='%s' remote_node='%s' 조회 실패 [callId=%s]",
                     strNext.c_str(), rc.remote_node_ref.c_str(), pszCallId );
        return false;
    }

    // 실패 B-leg 가 냈던 오퍼·신원 그대로(다이얼로그는 이 콜백 뒤에 psip 이 지운다 — 지금 읽어야 한다)
    CSipCallRtp clsRtp;
    std::string strFrom, strTo;
    if ( !gclsUserAgent.GetLocalCallRtp( pszCallId, &clsRtp ) || !gclsUserAgent.GetFromId( pszCallId, strFrom ) ||
         !gclsUserAgent.GetToId( pszCallId, strTo ) ) {
        CLog::Print( LOG_ERROR, "RouteSet failover: 실패 leg 의 오퍼/신원을 읽지 못함 [callId=%s]", pszCallId );
        return false;
    }
    CSipCallRoute clsRoute;
    clsRoute.m_strDestIp = rn.ip;
    clsRoute.m_iDestPort = rn.port;
    clsRoute.m_eTransport = ( rn.protocol == "TCP" ) ? E_SIP_TCP : ( rn.protocol == "TLS" ) ? E_SIP_TLS : E_SIP_UDP;
    clsRoute.m_b100rel = gclsUserAgent.Is100rel( strACallId.c_str() );
    if ( !rc.local_node_ref.empty() ) {
        LocalNodeInfo ln = gclsLocalNodeMap.GetByName( rc.local_node_ref );
        if ( ln.IsValid() ) {
            clsRoute.m_strOutboundLocalIp =
                ( ln.bind_ip.empty() || ln.bind_ip == "0.0.0.0" ) ? gclsSetup.m_strLocalIp : ln.bind_ip;
            clsRoute.m_iOutboundLocalPort = ln.bind_port;
        }
    }
    std::string strNewCallId;
    CSipMessage *pclsInvite = NULL;
    if ( !gclsUserAgent.CreateCall( strFrom.c_str(), strTo.c_str(), &clsRtp, &clsRoute, strNewCallId, &pclsInvite ) ) {
        CLog::Print( LOG_ERROR, "RouteSet failover: CreateCall 실패 route='%s' [callId=%s]", strNext.c_str(),
                     pszCallId );
        return false;
    }

    // CallMap: A ↔ 새 B. relay 세션·SDES·코덱 상태는 실패 leg 의 것을 그대로(같은 CMP 세션·peer1 포트), 라우팅 상태만
    // 갱신
    CCallInfo clsNew = clsB;
    clsNew.m_vecRoutesTried.push_back( clsB.m_strRouteName );
    clsNew.m_strRouteName = strNext;
    time( &clsNew.m_iLastActivityTime );
    gclsCallMap.Insert( strNewCallId.c_str(), clsNew );
    gclsCallMap.Update( strACallId.c_str(), strNewCallId.c_str() );
    gclsCallMap.DeleteOne( pszCallId );  // 실패 leg 만 — relay 세션은 유지(bStopPort 경로 아님)
    gclsCallMap.SetRouteInfo( strNewCallId.c_str(), clsNew.m_strRouteSet, clsNew.m_strRouteName,
                              clsNew.m_strRoutePolicy, clsNew.m_strRouteHashKey, clsNew.m_vecRoutesTried );
    SetCallOwner( strNewCallId.c_str(), GetCallOwner( pszCallId ) ? GetCallOwner( pszCallId ) : &m_clsIbcf );

    // 세션 로그·sesid 승계 — 새 B-leg 도 같은 세션의 착신 leg
    std::string strLegASesId = gclsSipLogger.GetSesIdByCallId( strACallId );
    if ( !strLegASesId.empty() ) gclsSipLogger.SetCallSesId( strNewCallId, strLegASesId );
    if ( gclsCallDir.IsEnabled() ) {
        std::string strSessionId = gclsCallDir.GetSessionId( strACallId );
        if ( !strSessionId.empty() ) {
            gclsCallDir.MapCallToSession( strNewCallId, strSessionId );
            gclsCallDir.WriteSessionMapping( strSessionId, strACallId, strNewCallId, strLegASesId );
        }
    }

    CLog::Print(
        LOG_SYSTEM,
        "RouteSet failover: route='%s' ended %d → re-route via route='%s' (%s:%d %s) set='%s' policy='%s' attempt=%zu "
        "[A=%s old=%s new=%s]",
        clsB.m_strRouteName.c_str(), iSipStatus, strNext.c_str(), rn.ip.c_str(), rn.port, rn.protocol.c_str(),
        clsB.m_strRouteSet.c_str(), clsB.m_strRoutePolicy.c_str(), clsNew.m_vecRoutesTried.size() + 1,
        strACallId.c_str(), pszCallId, strNewCallId.c_str() );

    if ( !gclsUserAgent.StartCall( strNewCallId.c_str(), pclsInvite ) ) {
        // 전송 실패 — 새 B 를 걷고 A 를 종전 실패 코드로 끝낸다(relay 는 Delete 가 회수)
        CLog::Print( LOG_ERROR, "RouteSet failover: StartCall 실패 route='%s' → A 종료 %d [callId=%s]", strNext.c_str(),
                     RelayEndStatus( iSipStatus ), strNewCallId.c_str() );
        gclsCallMap.Delete( strNewCallId.c_str() );
        gclsUserAgent.StopCall( strACallId.c_str(), RelayEndStatus( iSipStatus ) );
        RemoveCallOwner( strNewCallId.c_str() );
        RemoveCallOwner( strACallId.c_str() );
    }
    return true;
}

void CModuleDispatcher::EventCallEnd( const char *pszCallId, int iSipStatus ) {
    EventCallEnd( pszCallId, iSipStatus, NULL );
}

int CModuleDispatcher::RelayEndStatus( int iSipStatus ) {
    if ( iSipStatus < SIP_MULTIPLE_CHOICES ) return 0;
    if ( iSipStatus < SIP_BAD_REQUEST ) return SIP_TEMPORARILY_UNAVAILABLE;  // 3xx
    if ( iSipStatus == SIP_UNAUTHORIZED || iSipStatus == SIP_PROXY_AUTHENTICATION_REQUIRED ) return SIP_FORBIDDEN;
    if ( iSipStatus == SIP_GONE ) return SIP_REQUEST_TIME_OUT;
    return iSipStatus;
}

void CModuleDispatcher::EventCallEnd( const char *pszCallId, int iSipStatus, const char *pszReason ) {
    CCallInfo clsCallInfo;
    CLog::Print( LOG_DEBUG, "EventCallEnd(%s:%d) reason=%s", pszCallId, iSipStatus, pszReason ? pszReason : "-" );

    // MCData media plane 레그 — cmdp 세션 정리 (UE 발 BYE·실패 응답 포함)
    if ( gclsMcDataMediaService.OnCallTerminated( pszCallId ) ) return;

    bool bSelHit = gclsCallMap.Select( pszCallId, clsCallInfo );
    CLog::Print( LOG_DEBUG, "EventCallEnd callid=%s sip=%d selHit=%d peer=%s peerRtpPort=%d", pszCallId, iSipStatus,
                 bSelHit ? 1 : 0, bSelHit ? clsCallInfo.m_strPeerCallId.c_str() : "-",
                 bSelHit ? clsCallInfo.m_iPeerRtpPort : -1 );

    // TAS — dialog-event 종료(terminated) 통지(CallMap 삭제 전, §6.2 — 통과) / blind transfer 전환 leg
    //   실패 NOTIFY + trans entry 정리 (CallMap 밖 trans leg — 소비) / 대표번호 포크 leg·A 취소 (소비)
    if ( m_clsTas.IsEnabled() && m_clsTas.OnCallEnd( pszCallId, iSipStatus ) ) {
        RemoveCallOwner( pszCallId );
        return;
    }

    // 피어 B-leg 의 5xx·타임아웃 — 같은 RouteSet 의 다음 멤버로 새 B-leg 를 낸다(A-leg·relay 세션 유지). 성공하면 이
    // leg 의
    //   종료는 여기서 끝난다(A 에는 아무 것도 보내지 않는다).
    if ( bSelHit && TryRerouteLeg( pszCallId, clsCallInfo, iSipStatus ) ) {
        RemoveCallOwner( pszCallId );
        return;
    }

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
                // B2BUA 는 어느 leg 이 먼저 끝날지 정해져 있지 않다 — 끝난 leg 의 CDR 에
                // 응답 시각이 없으면 상대 leg 에서 찾는다.
                int iDur = _CallDurationSec( pszCallId );
                if ( iDur == 0 && !clsCallInfo.m_strPeerCallId.empty() )
                    iDur = _CallDurationSec( clsCallInfo.m_strPeerCallId.c_str() );
                gclsCallDir.VoipCallEnd( strOrigCallId, iSipStatus == 200 ? "normal" : "error", iDur, iSipStatus );
            }
        }
        if ( clsCallInfo.m_bRecv )
            OnCallEnded( pszCallId, iSipStatus );
        else
            OnCallEnded( clsCallInfo.m_strPeerCallId.c_str(), iSipStatus );

        // CMP 리소스 해제 → BYE 순서 (리소스 먼저 해제 후 SIP 종료)
        bool bIsGroup = gclsGroupCallService.OnCallTerminated( pszCallId );
        gclsCallMap.Delete( pszCallId, !bIsGroup );
        // 상대 leg 종료 — 종료 사유(Reason, RFC 3326 §2)와 최종 응답 코드를 그대로 옮긴다
        //   (TS 24.229 §5.4.3.2 — 미응답 착신 leg 는 발신 leg 의 실패 코드로 끝나야 통계·사용자 표시가 맞는다).
        gclsUserAgent.StopCall( clsCallInfo.m_strPeerCallId.c_str(), RelayEndStatus( iSipStatus ), pszReason );

        RemoveCallOwner( pszCallId );
        RemoveCallOwner( clsCallInfo.m_strPeerCallId.c_str() );
    } else {
        // PTT 개시자(originator) BYE: AcceptCall 경로라 gclsCallMap 미등록.
        // OnCallTerminated 미호출 시 m_mapCallSession에 1001 엔트리가 잔존 →
        // 마지막 fan-out BYE 처리 시 bStillActive=true → PTT_GROUP_REMOVE 누락 →
        // CheckGroupIntegrity 재-INVITE 폭주. 여기서 처리해야 정상 종료.
        gclsGroupCallService.OnCallTerminated( pszCallId );
        RemoveCallOwner( pszCallId );
    }
}

bool CModuleDispatcher::EventGetLegDest( const char *pszCallId, const char *pszPeerId, std::string &strIp, int &iPort,
                                         ESipTransport &eTransport ) {
    (void)pszCallId;
    if ( pszPeerId == NULL || *pszPeerId == '\0' ) return false;

    // 등록 단말이면 latch 된 실제 도달 주소를 준다. 미등록(제휴 노드 등)이면 false —
    //   psip 이 다이얼로그가 기억한 주소를 그대로 쓴다(기존 동작 보존).
    CUserInfo clsUserInfo;
    if ( !gclsUserMap.Select( pszPeerId, clsUserInfo ) ) return false;
    if ( clsUserInfo.m_strIp.empty() || clsUserInfo.m_iPort <= 0 ) return false;

    strIp = clsUserInfo.m_strIp;
    iPort = clsUserInfo.GetSendPort();  // IPsec 바인딩은 port_us
    eTransport = clsUserInfo.m_eTransport;
    return true;
}

void CModuleDispatcher::EventReInvite( const char *pszCallId, CSipCallRtp *pclsRemoteRtp, CSipCallRtp *pclsLocalRtp ) {
    // 세션 갱신 re-INVITE (RFC 4028) — 선언 미디어가 직전과 동일하면 미디어 재협상이 아니다.
    //   CMP 재호출·NAT 재평가를 생략한다 (leg_liveness.md §6.3). psip 이 200 OK 를 이미
    //   같은 SDP 로 응답하므로 여기서 할 일이 없다.
    if ( gclsUserAgent.IsSessionRefreshReInvite( pszCallId ) ) {
        CLog::Print( LOG_DEBUG, "EventReInvite: session refresh (media unchanged) — CallId(%s)", pszCallId );
        return;
    }

    CCallInfo clsCallInfo;
    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        // 재협상 leg 의 새 원격 RTP 주소를 CMP 에 MODIFY — 수신(A) leg=peer0, 발신(B) leg=peer1.
        //   미갱신 시 CMP 는 초기 ADD 주소로 계속 송신: no-NAT leg 의 포트 변경은 rtp_src_drop
        //   전량 드롭, NAT leg 의 망 전환(re-INVITE)은 구 sig_ip guard 에 막혀 재-latch 불가였다.
        //   반드시 아래 SetIpPort(relay 주소 덮어쓰기) 전에 UE 선언 주소를 읽어 보낸다.
        if ( pclsRemoteRtp && !clsCallInfo.m_strRelaySessionId.empty() ) {
            int iAudioPort = pclsRemoteRtp->GetAudioPort();
            if ( iAudioPort <= 0 && pclsRemoteRtp->m_iPort > 0 ) iAudioPort = pclsRemoteRtp->m_iPort;
            if ( iAudioPort > 0 ) {
                int iVideoPort = ( pclsRemoteRtp->GetMediaCount() >= 2 ) ? pclsRemoteRtp->GetVideoPort() : 0;
                int iPeerIdx = clsCallInfo.m_bRecv ? 0 : 1;
                const std::string &strUserId =
                    clsCallInfo.m_bRecv ? clsCallInfo.m_strRelayCaller : clsCallInfo.m_strRelayCallee;
                // 재협상 leg NAT 판정 — re-INVITE SDP IP vs 등록 바인딩(received/rport latch)
                int iLegNat = 0;
                std::string strLegGuardIp;
                {
                    ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( strUserId, "volte" );
                    std::string strSigIp;
                    CUserInfo clsLegUserInfo;
                    if ( !strUserId.empty() && gclsUserMap.Select( strUserId.c_str(), clsLegUserInfo ) )
                        strSigIp = clsLegUserInfo.m_strIp;
                    if ( CCspServiceMap::EvalMediaNat( clsNatSvc, pclsRemoteRtp->m_strIp, strSigIp, strLegGuardIp ) ) {
                        iLegNat = 1;
                        CLog::Print( LOG_INFO, "EventReInvite: peer%d leg NAT (svc=%s sdp=%s sig=%s guard=%s)",
                                     iPeerIdx, clsNatSvc.name.c_str(), pclsRemoteRtp->m_strIp.c_str(), strSigIp.c_str(),
                                     strLegGuardIp.c_str() );
                    }
                }
                // 재협상 leg PT/코덱 — UE 가 offer, 서버 answer 는 echo(bServerOffered=false).
                int iLegPt = 0, iLegSrcPt = 0, iLegTePt = 0, iLegSrcTePt = 0;
                std::string strLegCodec;
                CGroupCallService::GetLegPt( pszCallId, false, iLegPt, iLegSrcPt, iLegTePt, iLegSrcTePt, &strLegCodec );
                // 협상 코덱이 확정된 leg(G.711 피어 등 코덱 테이블 top 이 아닐 수 있다)는 그 PT 를 쓴다 (cmp.md §11)
                if ( clsCallInfo.m_clsCodecLeg[iPeerIdx].negotiated.Valid() ) {
                    iLegPt = iLegSrcPt = clsCallInfo.m_clsCodecLeg[iPeerIdx].negotiated.pt;
                    strLegCodec = clsCallInfo.m_clsCodecLeg[iPeerIdx].negotiated.Label();
                    if ( clsCallInfo.m_clsCodecLeg[iPeerIdx].tePt > 0 )
                        iLegTePt = iLegSrcTePt = clsCallInfo.m_clsCodecLeg[iPeerIdx].tePt;
                }
                // 재협상 leg SDES — UE 재키잉만 반영(서버 키 유지: psip 자동 200 이 기존 local SDP
                //   로 답한다). 동일 선언 재전송 = CMP 세션 유지 (§5.2·§6.3).
                RelaySdesLeg clsSdesLeg = clsCallInfo.m_clsSdesLeg[iPeerIdx];
                CmpMediaCrypto clsLegAudioCrypto, clsLegVideoCrypto;
                MediaSdes::ReadReinviteSdes( pclsRemoteRtp->m_clsMediaList, "audio", iPeerIdx, clsSdesLeg.clsAudio,
                                             clsLegAudioCrypto );
                MediaSdes::ReadReinviteSdes( pclsRemoteRtp->m_clsMediaList, "video", iPeerIdx, clsSdesLeg.clsVideo,
                                             clsLegVideoCrypto );
                gclsCallMap.SetRelaySdesLeg( pszCallId, iPeerIdx, clsSdesLeg );
                gclsCmpClient.ModifySession( clsCallInfo.m_strRelaySessionId, pclsRemoteRtp->m_strIp, iAudioPort,
                                             iVideoPort, iPeerIdx, clsCallInfo.m_strRelayCaller,
                                             clsCallInfo.m_strRelayCallee, clsCallInfo.m_strRelaySesId, iLegNat,
                                             strLegGuardIp, iLegPt, iLegSrcPt, iLegTePt, iLegSrcTePt, strLegCodec,
                                             clsLegAudioCrypto.bEnabled ? &clsLegAudioCrypto : NULL,
                                             clsLegVideoCrypto.bEnabled ? &clsLegVideoCrypto : NULL );
            }
        }
        if ( pclsRemoteRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            // 상대 leg 로 전달할 re-offer 재작성 — 재협상 leg 의 crypto 투과 차단, 상대 leg 는 자기
            //   기존 키 그대로 재광고(키 불변 = CMP 세션 유지 — PropagateConditionToMembers 와 동형).
            if ( !clsCallInfo.m_strRelaySessionId.empty() ) {
                int iTargetLeg = clsCallInfo.m_bRecv ? 1 : 0;
                MediaSdes::RewriteRelaySdpForLeg( pclsRemoteRtp->m_clsMediaList, clsCallInfo.m_clsSdesLeg[iTargetLeg],
                                                  true );
                // 변환 호 — 대상 leg 로 가는 re-offer 는 그 leg 의 협상 코덱으로(hold/resume 의 AMR-WB re-offer 가
                // G.711 피어에 그대로 가면 488)
                const RelayCodec::LegCodecs &clsTgt = clsCallInfo.m_clsCodecLeg[iTargetLeg];
                if ( clsTgt.transcode && clsTgt.negotiated.Valid() )
                    RelayCodec::RewriteAudio( pclsRemoteRtp->m_clsMediaList, clsTgt.negotiated, clsTgt.tePt,
                                              clsTgt.teRtpmap, clsTgt.teFmtp );
            }
            // 재협상 SDP 에도 CMP relay IP 를 광고 (멀티 미디어노드에서 CSP 로컬 주소 오광고 방지)
            std::string strRelayIp = clsCallInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress()
                                                                           : clsCallInfo.m_strRelayLocalIp;
            pclsRemoteRtp->SetIpPort( strRelayIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        }
        gclsUserAgent.SendReInvite( clsCallInfo.m_strPeerCallId.c_str(), pclsRemoteRtp );
    } else if ( pclsRemoteRtp ) {
        // PTT 멤버 leg (CallMap 밖 — CSP 가 종단, 스택이 기존 로컬 SDP 로 자동 200 OK) — 재협상된
        //   멤버 주소를 JOIN ②(멱등)로 CMP 에 재전달 + NAT 재판정. PTT 세션이 아니면 내부에서
        //   조기 return 이라 안전.
        int iAudioPort = pclsRemoteRtp->GetAudioPort();
        if ( iAudioPort <= 0 && pclsRemoteRtp->m_iPort > 0 ) iAudioPort = pclsRemoteRtp->m_iPort;
        if ( iAudioPort > 0 ) {
            int iRemoteVideo = pclsRemoteRtp->GetVideoPort();
            int iRemoteFloor = pclsRemoteRtp->GetApplicationPort();
            gclsGroupCallService.OnCallStarted( pszCallId, pclsRemoteRtp->m_strIp, iAudioPort,
                                                iRemoteFloor > 0 ? iRemoteFloor : 0, iRemoteVideo, pclsRemoteRtp );
        }
    }
}

void CModuleDispatcher::EventReInviteResponse( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRemoteRtp ) {
    // 서버가 전달한 re-INVITE 의 재-answer — SRTP leg 의 UE 재키잉만 CMP 에 반영한다 (§5.2).
    //   키 불변(통상)이면 아무 것도 하지 않는다 — MODIFY 재전송은 NAT 플래그 재평가를 요구하므로
    //   재키잉이 실제 감지될 때만 주소·NAT 포함 전체 MODIFY 를 낸다.
    if ( iSipStatus < SIP_OK || iSipStatus >= SIP_MULTIPLE_CHOICES || pclsRemoteRtp == NULL ) return;
    CCallInfo clsCallInfo;
    if ( !gclsCallMap.Select( pszCallId, clsCallInfo ) || clsCallInfo.m_strRelaySessionId.empty() ) return;
    const int iPeerIdx = clsCallInfo.m_bRecv ? 0 : 1;
    RelaySdesLeg clsSdesLeg = clsCallInfo.m_clsSdesLeg[iPeerIdx];
    if ( !clsSdesLeg.clsAudio.bSrtp && !clsSdesLeg.clsVideo.bSrtp ) return;
    const std::string strOldAudioKey = clsSdesLeg.clsAudio.strUeKey;
    const std::string strOldVideoKey = clsSdesLeg.clsVideo.strUeKey;
    CmpMediaCrypto clsAudioCrypto, clsVideoCrypto;
    MediaSdes::ReadReinviteSdes( pclsRemoteRtp->m_clsMediaList, "audio", iPeerIdx, clsSdesLeg.clsAudio,
                                 clsAudioCrypto );
    MediaSdes::ReadReinviteSdes( pclsRemoteRtp->m_clsMediaList, "video", iPeerIdx, clsSdesLeg.clsVideo,
                                 clsVideoCrypto );
    if ( clsSdesLeg.clsAudio.strUeKey == strOldAudioKey && clsSdesLeg.clsVideo.strUeKey == strOldVideoKey ) return;
    gclsCallMap.SetRelaySdesLeg( pszCallId, iPeerIdx, clsSdesLeg );

    int iAudioPort = pclsRemoteRtp->GetAudioPort();
    if ( iAudioPort <= 0 && pclsRemoteRtp->m_iPort > 0 ) iAudioPort = pclsRemoteRtp->m_iPort;
    if ( iAudioPort <= 0 ) return;
    int iVideoPort = ( pclsRemoteRtp->GetMediaCount() >= 2 ) ? pclsRemoteRtp->GetVideoPort() : 0;
    const std::string &strUserId = iPeerIdx == 0 ? clsCallInfo.m_strRelayCaller : clsCallInfo.m_strRelayCallee;
    int iLegNat = 0;
    std::string strLegGuardIp;
    {
        ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( strUserId, "volte" );
        std::string strSigIp;
        CUserInfo clsLegUserInfo;
        if ( !strUserId.empty() && gclsUserMap.Select( strUserId.c_str(), clsLegUserInfo ) )
            strSigIp = clsLegUserInfo.m_strIp;
        if ( CCspServiceMap::EvalMediaNat( clsNatSvc, pclsRemoteRtp->m_strIp, strSigIp, strLegGuardIp ) ) iLegNat = 1;
    }
    CLog::Print( LOG_INFO, "EventReInviteResponse: peer%d SRTP UE rekey — CMP MODIFY (CallId=%s)", iPeerIdx,
                 pszCallId );
    gclsCmpClient.ModifySession( clsCallInfo.m_strRelaySessionId, pclsRemoteRtp->m_strIp, iAudioPort, iVideoPort,
                                 iPeerIdx, clsCallInfo.m_strRelayCaller, clsCallInfo.m_strRelayCallee,
                                 clsCallInfo.m_strRelaySesId, iLegNat, strLegGuardIp, 0, 0, 0, 0, "",
                                 clsAudioCrypto.bEnabled ? &clsAudioCrypto : NULL,
                                 clsVideoCrypto.bEnabled ? &clsVideoCrypto : NULL );
}

void CModuleDispatcher::EventPrack( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    CCallInfo clsCallInfo;
    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            // PRACK SDP 도 대상 leg 상태로 재작성 (answer 시맨틱 — crypto 투과 차단, §5.2)
            if ( !clsCallInfo.m_strRelaySessionId.empty() ) {
                int iTargetLeg = clsCallInfo.m_bRecv ? 1 : 0;
                MediaSdes::RewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsCallInfo.m_clsSdesLeg[iTargetLeg],
                                                  false );
            }
            std::string strRelayIp = clsCallInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress()
                                                                           : clsCallInfo.m_strRelayLocalIp;
            pclsRtp->SetIpPort( strRelayIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        }
        gclsUserAgent.SendPrack( clsCallInfo.m_strPeerCallId.c_str(), pclsRtp );
    }
}

bool CModuleDispatcher::EventTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreenedTransfer ) {
    // 호 전달(attended) → TAS 모듈 (volte_supplementary_services.md §6.2)
    return m_clsTas.IsEnabled() && m_clsTas.OnTransfer( pszCallId, pszReferToCallId, bScreenedTransfer );
}

bool CModuleDispatcher::EventBlindTransfer( const char *pszCallId, const char *pszReferToId ) {
    // 호 전달(blind) → TAS 모듈 (volte_supplementary_services.md §6.1)
    //   Refer-To 의 전달 대상도 다이얼 플랜(§2-10)을 거친다 — 전달자(REFER 지시자 leg 의 가입자 쪽 신원)의 접속서비스
    //   플랜
    std::string strReferTo = pszReferToId ? pszReferToId : "";
    {
        std::string strA, strB, strOut;
        gclsUserAgent.GetFromId( pszCallId, strA );
        gclsUserAgent.GetToId( pszCallId, strB );
        CspUser clsTmp;
        const std::string &strReferrer = gclsCspUserMap.Select( strA.c_str(), clsTmp ) ? strA : strB;
        ServiceInfo svc = gclsServiceMap.GetForUser( strReferrer, "volte" );
        if ( CspDialPlan::Normalize( strReferTo, "", svc.dial_plan, strOut ) == DIAL_PLAN_TRANSLATED ) {
            CLog::Print( LOG_INFO, "DialPlan: REFER referrer(%s) Refer-To %s → %s (service=%s)", strReferrer.c_str(),
                         strReferTo.c_str(), strOut.c_str(), svc.name.c_str() );
            strReferTo = strOut;
        }
    }
    return m_clsTas.IsEnabled() && m_clsTas.OnBlindTransfer( pszCallId, strReferTo.c_str() );
}

int CModuleDispatcher::EventMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) {
    // 착신 번역(다이얼 플랜, §2-10) — MESSAGE 도 Request-URI 기준 착신. 국내형 → +E.164, 번역 불가 → 484
    std::string strMsgCallee;
    if ( pclsMessage ) {
        std::string strPlanSource;
        EDialPlanResult eDial = ResolveCallee( pclsMessage, InboundRouteFor( pclsMessage ), pszFrom ? pszFrom : "",
                                               strMsgCallee, strPlanSource );
        if ( eDial == DIAL_PLAN_INCOMPLETE ) {
            CLog::Print( LOG_INFO, "DialPlan: MESSAGE from(%s) callee(%s) 번역 불가(plan=%s) → 484",
                         pszFrom ? pszFrom : "", strMsgCallee.c_str(), strPlanSource.c_str() );
            return SIP_ADDRESS_INCOMPLETE;
        }
        if ( !strMsgCallee.empty() ) {
            if ( eDial == DIAL_PLAN_TRANSLATED )
                CLog::Print( LOG_INFO, "DialPlan: MESSAGE from(%s) callee %s → %s (plan=%s)", pszFrom ? pszFrom : "",
                             pszTo ? pszTo : "", strMsgCallee.c_str(), strPlanSource.c_str() );
            pszTo = strMsgCallee.c_str();
        }
    }
    // MCPTT emergency alert (TS 24.379): mcptt-info alert-ind 판별 → SMS 와 분기.
    //   Phase 3a 탐지/로깅/ack + Phase 3b 그룹 멤버 fan-out(같은 alert MESSAGE 전파, 취소도 동일).
    if ( pclsMessage && pclsMessage->m_strBody.find( "alert-ind" ) != std::string::npos ) {
        CMcpttInfo clsMi = ParseMcpttInfo( pclsMessage->m_strBody );
        bool bActivate = clsMi.bAlert;  // true=경보 발신, false=경보 취소
        bool bGroupTarget = gclsGroupMap.Contains( pszTo );
        // 능력 게이트 (TS 24.481): 그룹의 allow-MCPTT-emergency-alert 허용 시에만 전파.
        CspPttGroup clsGroup;
        bool bHaveGroup = bGroupTarget && gclsGroupMap.Select( pszTo, clsGroup );
        bool bAllowed = bHaveGroup ? clsGroup._emergencyAlert : true;
        // 사용자 단위 개시 인가 (TS 24.484 allow-activate-emergency-alert) — 미인가 경보는 전파하지
        //   않는다 (규격: 콜과 달리 거절이 아닌 스트립). 취소는 항상 통과 — 잔존 경보 정리 경로 보존.
        if ( bAllowed && bActivate ) {
            CspUserProfile clsProf;
            if ( gclsDbManager.SelectUserProfile( pszFrom, clsProf ) >= 0 && !clsProf.m_bAllowEmergencyAlert ) {
                bAllowed = false;
                CLog::Print( LOG_INFO, "EventMessage: alert by(%s) not authorised (user profile) → drop", pszFrom );
            }
        }
        const char *pszEvt = bActivate ? "alert_sent" : "alert_cancelled";
        int iFanout = 0;
        if ( bAllowed && bHaveGroup ) {
            if ( gclsCallDir.IsEnabled() )
                gclsCallDir.PttLogEvent(
                    pszTo, pszEvt, std::string( "{\"actor\":\"" ) + pszFrom + "\",\"target\":\"" + pszTo + "\"}" );
            // Phase 3b — 그룹 등록 멤버에게 alert MESSAGE fan-out (발신자 제외). affiliation 요구 그룹은
            //   affiliate 된 멤버만. 취소(alert-ind=false)도 동일 본문 전파로 멤버에 반영.
            //   Content-Type 보존 (mcptt-info+xml — text/plain 강등 시 단말이 SMS 로 오인해 경보 분기 미동작).
            {
                char szContentType[512];
                szContentType[0] = '\0';
                pclsMessage->m_clsContentType.ToString( szContentType, sizeof( szContentType ) );
                for ( const auto &pUser : clsGroup._pusers ) {
                    if ( !pUser || pUser->_id == pszFrom ) continue;
                    if ( clsGroup._requireAffiliation && gclsDbManager.IsConnected() &&
                         !gclsDbManager.IsAffiliated( pszTo, pUser->_id ) )
                        continue;
                    CUserInfo clsMemInfo;
                    if ( gclsUserMap.Select( pUser->_id.c_str(), clsMemInfo ) ) {
                        CSipCallRoute clsMemRoute;
                        clsMemInfo.GetCallRoute( clsMemRoute );
                        if ( gclsUserAgent.SendSms( pszFrom, pUser->_id.c_str(), pclsMessage->m_strBody.c_str(),
                                                    &clsMemRoute, szContentType[0] ? szContentType : NULL ) )
                            iFanout++;
                    }
                }
            }
        }
        CLog::Print( LOG_INFO, "EventMessage: MCPTT emergency %s from(%s) to(%s) group=%d fanout=%d", pszEvt, pszFrom,
                     pszTo, bGroupTarget, iFanout );
        // 200 OK ack (경보 수신 확인) — 응답은 psip 가 이 반환값으로 보낸다.
        return SIP_OK;
    }

    // MCData 그룹 SDS (TS 24.282) — 그룹 대상 MESSAGE 는 MCDATA-AS 가 게이트+fan-out.
    int iMcStatus = SIP_OK;
    if ( m_clsMcDataAs.IsEnabled() && m_clsMcDataAs.OnMessage( pszFrom, pszTo, pclsMessage, iMcStatus ) )
        return iMcStatus;

    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    if ( gclsUserMap.Select( pszTo, clsUserInfo ) == false ) {
        // 착신자에게 보낼 등록 바인딩이 없다.
        //   RFC 3261 §21.4.18 — 가입자는 알지만 유효한 도달 경로가 없으면 480 Temporarily Unavailable.
        //   가입자 자체를 모르면 404 Not Found (TS 24.229 의 미등록 처리와 같은 구분).
        //   603 Decline 은 "착신자가 거부했다"는 전역 실패라 이 상황과 의미가 다르다 - 포크·재시도까지 막는다.
        CspUser clsTarget;
        int iStatus = gclsCspUserMap.Select( pszTo, clsTarget ) ? SIP_TEMPORARILY_UNAVAILABLE : SIP_NOT_FOUND;
        CLog::Print( LOG_INFO, "EventMessage: 1:1 from(%s) to(%s) no binding -> %d", pszFrom, pszTo, iStatus );
        return iStatus;
    }
    clsUserInfo.GetCallRoute( clsRoute );
    // 1:1 전달 — Content-Type 보존 (MCData disposition 통지 등 text/plain 이외 본문 대응)
    char szContentType[512];
    szContentType[0] = '\0';
    pclsMessage->m_clsContentType.ToString( szContentType, sizeof( szContentType ) );

    // 관제 데스크 통합 이력용 1:1 SDS/SMS 보관 (dispatch_center.md §5.6, mcdata_messaging.md §4.3).
    //   Setup.McData.StoreOneToOneSds 가 켜졌을 때만. 전량 보관 — 열람 범위는 CSC 조회 시점 게이트.
    //   disposition 통지(수신확인)는 이력이 아니므로 제외한다(사람 메시지·파일만).
    if ( gclsSetup.m_bStoreOneToOneSds && gclsCallDir.IsEnabled() ) {
        CMcDataSdsInfo clsInfo;
        bool bMc = McDataIsMultipartMixed( szContentType ) &&
                   McDataParseBody( szContentType, pclsMessage->m_strBody, clsInfo );
        bool bDisposition = bMc && ( clsInfo.m_iMsgType == MCDATA_MSG_SDS_NOTIFICATION );
        if ( !bDisposition ) {
            std::string strText = bMc ? clsInfo.m_strText : pclsMessage->m_strBody;
            const char *pszType = bMc ? ( clsInfo.m_iMsgType == MCDATA_MSG_FD_SIGNALLING ? "fd" : "sds" ) : "text";
            int iSize = bMc ? clsInfo.m_iPayloadSize : (int)pclsMessage->m_strBody.size();
            std::string strRec = std::string( "{\"from\":\"" ) + CCallDir::JsonEsc( pszFrom ) + "\",\"to\":\"" +
                                 CCallDir::JsonEsc( pszTo ) + "\",\"msg_type\":\"" + pszType + "\",\"conv_id\":\"" +
                                 CCallDir::JsonEsc( clsInfo.m_strConvId ) + "\",\"msg_id\":\"" +
                                 CCallDir::JsonEsc( clsInfo.m_strMsgId ) + "\",\"text\":\"" +
                                 CCallDir::JsonEsc( strText ) + "\",\"size\":" + std::to_string( iSize ) +
                                 ",\"disposition_req\":" + std::to_string( clsInfo.m_iDispositionReq ) + "}";
            gclsCallDir.McData1to1Log( strRec );
        }
    }

    if ( gclsUserAgent.SendSms( pszFrom, pszTo, pclsMessage->m_strBody.c_str(), &clsRoute,
                                szContentType[0] ? szContentType : NULL ) == false ) {
        CLog::Print( LOG_ERROR, "EventMessage: 1:1 from(%s) to(%s) send failed", pszFrom, pszTo );
        return SIP_INTERNAL_SERVER_ERROR;
    }
    return SIP_OK;
}
