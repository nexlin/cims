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
#include "Log.h"
#include "McDataMediaService.h"
#include "McpttInfo.h"
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
            pclsResp->AddHeader( "Allow", SIP_ALLOW_METHODS );
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
                bool bLocalTarget = gclsGroupMap.Contains( strTo.c_str() ) ||
                                    gclsCspUserMap.isAlive( strTo.c_str(), clsToProv ) ||
                                    gclsDbManager.SelectUser( strTo, clsToProv );
                if ( !bLocalTarget ) {
                    CLog::SuppressNetworkSource( pclsMessage->m_strClientIp.c_str(), 300 );
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
    // v3 (2026-04-22): OPTIONS 헬스체크는 RouteSet 의 health_check 가 담당하도록 이관 예정.
    //   현 스테이지는 헬스체크 송신/수신 자체를 아직 구현 안함.
    (void)iThreadId;
    if ( pclsMessage == NULL ) return false;

    ReapSubscriptionOnNotifyFailure( pclsMessage, pclsMessage->m_iStatusCode );

    // 응답 소비는 하지 않는다 — 뒤따르는 콜백(CSipUserAgent)의 처리를 막으면 안 된다.
    return false;
}

bool CModuleDispatcher::SendTimeout( int iThreadId, CSipMessage *pclsMessage ) {
    (void)iThreadId;
    if ( pclsMessage == NULL ) return false;

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

    // G10 (2026-05-11): 외부 peer inbound auth-skip.
    //   RecvRequest 의 AclPolicy 가 inbound trust 를 이미 검증한 뒤
    //   routing_policies 가 outbound peer 를 결정하여 PendingRouteMap 에 저장한 콜은,
    //   From-user 가 로컬 user map 에 없는 외부 peer 발신이므로 401 challenge 우회.
    {
        std::string strCallId;
        pclsMessage->GetCallId( strCallId );
        if ( gclsPendingRouteMap.Has( strCallId ) ) return true;
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

    // tel: URI 착신 정규화 — HM-TRCP 등 MMTEL 단말은 착신을 tel:+82..(userinfo 없음)로
    //   보낸다. psip 는 '@' 부재로 번호를 host 에 파싱 → To user 가 비어 pszTo 가 빈 문자열.
    //   이 경우 To/Request-URI 가 tel: 이면 host(전화번호)를 착신으로 채택한다. (sip: 는 무변)
    std::string strTelCallee;
    if ( ( pszTo == NULL || pszTo[0] == '\0' ) && pclsMessage ) {
        if ( strcasecmp( pclsMessage->m_clsTo.m_clsUri.m_strProtocol.c_str(), "tel" ) == 0 &&
             !pclsMessage->m_clsTo.m_clsUri.m_strHost.empty() )
            strTelCallee = pclsMessage->m_clsTo.m_clsUri.m_strHost;
        else if ( strcasecmp( pclsMessage->m_clsReqUri.m_strProtocol.c_str(), "tel" ) == 0 &&
                  !pclsMessage->m_clsReqUri.m_strHost.empty() )
            strTelCallee = pclsMessage->m_clsReqUri.m_strHost;
        if ( !strTelCallee.empty() ) {
            CLog::Print( LOG_INFO, "EventIncomingCall: tel: URI 착신 정규화 → %s", strTelCallee.c_str() );
            pszTo = strTelCallee.c_str();  // 이후 라우팅(가입자/그룹 조회)이 sip: 와 동일하게 동작
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
            CLog::Print( LOG_INFO, "EventIncomingCall: private call target(%s) not registered → 480 [PTT-AS]",
                         pszTo );
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
            if ( gclsGroupMap.Select( strPrivId.c_str(), clsPrivOld ) &&
                 clsPrivOld._floorControl != pszWantFloorCtl ) {
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
    if ( m_clsPttAs.IsEnabled() && !gclsGroupMap.Contains( pszTo ) && pclsMessage ) {
        std::vector<std::string> vecAdhoc = ParseResourceListUsers( pclsMessage->m_strBody );
        if ( !vecAdhoc.empty() ) {
            CspPttGroup clsAdhoc;
            clsAdhoc.Clear();
            clsAdhoc._id = pszTo;
            clsAdhoc._name = std::string( "adhoc:" ) + pszTo;
            clsAdhoc._groupType = "prearranged";  // on-demand 수명(마지막 이탈 시 teardown)
            clsAdhoc._requireAffiliation = false;
            clsAdhoc._isAdhoc = true;  // 통화 종료 시 GroupMap 에서 제거(ephemeral)
            bool bHasInit = false;
            for ( const auto &m : vecAdhoc ) {
                clsAdhoc._pusers.push_back( std::make_shared<CspPttUser>( m, 5, "participant", "" ) );
                if ( m == pszFrom ) bHasInit = true;
            }
            if ( !bHasInit )
                clsAdhoc._pusers.push_back( std::make_shared<CspPttUser>( pszFrom, 5, "participant", "" ) );
            gclsGroupMap.Insert( clsAdhoc );
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
            if ( gclsGroupCallService.ProcessGroupCall( pszTo, pszFrom, pszCallId, pclsRtp, &clsRouteTemp,
                                                        iMcpttCond ) ) {
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
        {
            ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( pszFrom ? pszFrom : "", "volte" );
            std::string strSigIp;
            int iSigPort = 0;
            if ( pclsMessage ) pclsMessage->GetTopViaIpPort( strSigIp, iSigPort );
            if ( strSigIp.empty() ) {
                CUserInfo clsFromInfo;
                if ( pszFrom && gclsUserMap.Select( pszFrom, clsFromInfo ) ) strSigIp = clsFromInfo.m_strIp;
            }
            if ( CCspServiceMap::EvalMediaNat( clsNatSvc, pclsRtp->m_strIp, strSigIp, strCallerGuardIp ) ) {
                iCallerNat = 1;
                CLog::Print( LOG_INFO, "EventIncomingCall: caller leg NAT (svc=%s sdp=%s sig=%s guard=%s)",
                             clsNatSvc.name.c_str(), pclsRtp->m_strIp.c_str(), strSigIp.c_str(),
                             strCallerGuardIp.c_str() );
            }
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
                                        strCallerGuardIp,
                                        iCallerPt, iCallerSrcPt, iCallerTePt, iCallerSrcTePt, strCallerCodec ) ) {
            return StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
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

    // P-Asserted-Identity 는 psip(CSipDialog::CreateMessage)가 발신 leg 도메인 기준으로
    //   이미 1개 삽입한다(동일 값). 여기서 재삽입하면 동일 PAID 가 2개 되어 RFC 3325
    //   위반(scheme 당 1개) → 중복 삽입 제거.

    // leg 별 포트: 각 entry 의 m_iPeerRtpPort = "그 leg 의 peer 에게 광고하는 relay 포트".
    //   A(수신) entry = peer1 포트(B leg SDP 용), B(발신) entry = peer0 포트(A leg SDP 용).
    gclsCallMap.Insert( pszCallId, strCallId.c_str(), iStartPortB, iStartPort );
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

    // MCData media plane 레그 — CallMap 밖에서 자체 수명 관리 (미선점 시 아래 else 가 StopCall)
    if ( gclsMcDataMediaService.OnCallStarted( pszCallId, pclsRtp ) ) return;

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
            // 착신(callee) RTP 주소를 CMP 에 MODIFY (peer_index=1) — session_id 로 직접 지목.
            if ( !clsCallInfo.m_strRelaySessionId.empty() ) {
                int iAudioPort = pclsRtp->GetAudioPort();
                if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
                int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;
                if ( iAudioPort > 0 ) {
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
                            CLog::Print( LOG_INFO, "EventCallStart: callee leg NAT (svc=%s sdp=%s sig=%s guard=%s)",
                                         clsNatSvc.name.c_str(), pclsRtp->m_strIp.c_str(), strSigIp.c_str(),
                                         strCalleeGuardIp.c_str() );
                        }
                    }
                    // 착신(B) leg PT/코덱 — 서버 offer(코덱 테이블) vs 착신 answer wire PT
                    //   (bServerOffered=true). 녹취 세그먼트 메타(audio_pt_b/audio_codec_b) 근거.
                    int iCalleePt = 0, iCalleeSrcPt = 0, iCalleeTePt = 0, iCalleeSrcTePt = 0;
                    std::string strCalleeCodec;
                    CGroupCallService::GetLegPt( pszCallId, true, iCalleePt, iCalleeSrcPt, iCalleeTePt,
                                                 iCalleeSrcTePt, &strCalleeCodec );
                    gclsCmpClient.ModifySession( clsCallInfo.m_strRelaySessionId, pclsRtp->m_strIp, iAudioPort,
                                                 iVideoPort > 0 ? iVideoPort : 0, 1, clsCallInfo.m_strRelayCaller,
                                                 clsCallInfo.m_strRelayCallee, clsCallInfo.m_strRelaySesId, iCalleeNat,
                                                 strCalleeGuardIp,
                                                 iCalleePt, iCalleeSrcPt, iCalleeTePt, iCalleeSrcTePt, strCalleeCodec );
                }
            }

            int iRemoteAudio = pclsRtp->GetAudioPort();
            if ( iRemoteAudio <= 0 && pclsRtp->m_iPort > 0 ) iRemoteAudio = pclsRtp->m_iPort;
            if ( iRemoteAudio > 0 ) {
                int iRemoteVideo = pclsRtp->GetVideoPort();
                // SDP m=application floor control 포트 파싱 (≤0 이면 OnCallStarted 내부 fallback)
                int iRemoteFloor = pclsRtp->GetApplicationPort();
                gclsGroupCallService.OnCallStarted( pszCallId, pclsRtp->m_strIp, iRemoteAudio,
                                                    iRemoteFloor > 0 ? iRemoteFloor : 0, iRemoteVideo, pclsRtp );
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
            // m_iPeerRtpPort = 전환 leg(peer1, base+4) 포트 → 반대 leg(peer0) = -4 (leg 블록 배치)
            iStartPort = clsCallInfo.m_iPeerRtpPort - 4;
            pclsRtp->SetIpPort( CspAddressing::GetLocalRtpAddress().c_str(), clsCallInfo.m_iPeerRtpPort,
                                SOCKET_COUNT_PER_MEDIA );
        }
        gclsUserAgent.SendReInvite( strReferToCallId.c_str(), pclsRtp );
        gclsCallMap.Insert( strReferToCallId.c_str(), pszCallId, iStartPort, clsCallInfo.m_iPeerRtpPort );
        gclsTransCallMap.Delete( pszCallId, false );
    } else {
        gclsUserAgent.StopCall( pszCallId );
    }
}

void CModuleDispatcher::EventCallEnd( const char *pszCallId, int iSipStatus ) {
    CCallInfo clsCallInfo;
    CLog::Print( LOG_DEBUG, "EventCallEnd(%s:%d)", pszCallId, iSipStatus );

    // MCData media plane 레그 — cmdp 세션 정리 (UE 발 BYE·실패 응답 포함)
    if ( gclsMcDataMediaService.OnCallTerminated( pszCallId ) ) return;

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
        // PTT 개시자(originator) BYE: AcceptCall 경로라 gclsCallMap 미등록.
        // OnCallTerminated 미호출 시 m_mapCallSession에 1001 엔트리가 잔존 →
        // 마지막 fan-out BYE 처리 시 bStillActive=true → PTT_GROUP_REMOVE 누락 →
        // CheckGroupIntegrity 재-INVITE 폭주. 여기서 처리해야 정상 종료.
        gclsGroupCallService.OnCallTerminated( pszCallId );

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
                                     iPeerIdx, clsNatSvc.name.c_str(), pclsRemoteRtp->m_strIp.c_str(),
                                     strSigIp.c_str(), strLegGuardIp.c_str() );
                    }
                }
                // 재협상 leg PT/코덱 — UE 가 offer, 서버 answer 는 echo(bServerOffered=false).
                int iLegPt = 0, iLegSrcPt = 0, iLegTePt = 0, iLegSrcTePt = 0;
                std::string strLegCodec;
                CGroupCallService::GetLegPt( pszCallId, false, iLegPt, iLegSrcPt, iLegTePt, iLegSrcTePt,
                                             &strLegCodec );
                gclsCmpClient.ModifySession( clsCallInfo.m_strRelaySessionId, pclsRemoteRtp->m_strIp, iAudioPort,
                                             iVideoPort, iPeerIdx, clsCallInfo.m_strRelayCaller,
                                             clsCallInfo.m_strRelayCallee, clsCallInfo.m_strRelaySesId, iLegNat,
                                             strLegGuardIp,
                                             iLegPt, iLegSrcPt, iLegTePt, iLegSrcTePt, strLegCodec );
            }
        }
        if ( pclsRemoteRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
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

void CModuleDispatcher::EventPrack( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    CCallInfo clsCallInfo;
    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            std::string strRelayIp = clsCallInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress()
                                                                          : clsCallInfo.m_strRelayLocalIp;
            pclsRtp->SetIpPort( strRelayIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
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
        // 전환 시 relay 반대 leg 참조는 leg 블록 간격(+4) — legacy best-effort (구 +2 는 old 배치)
        const std::string strRtpAddr = CspAddressing::GetLocalRtpAddress();
        if ( bScreenedTransfer )
            clsRtp.SetIpPort( strRtpAddr.c_str(), clsReferToCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        else
            clsRtp.SetIpPort( strRtpAddr.c_str(), clsReferToCallInfo.m_iPeerRtpPort + 4, SOCKET_COUNT_PER_MEDIA );
    }

    if ( bScreenedTransfer ) {
        CSipCallRtp clsReferToRtp;
        if ( gclsUserAgent.GetRemoteCallRtp( clsReferToCallInfo.m_strPeerCallId.c_str(), &clsReferToRtp ) == false )
            return false;
        clsReferToRtp.SetDirection( E_RTP_SEND_RECV );
        if ( gclsSetup.m_bUseRtpRelay )
            clsReferToRtp.SetIpPort( CspAddressing::GetLocalRtpAddress().c_str(), clsReferToCallInfo.m_iPeerRtpPort + 4,
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
        int iLocalPort = 0, iLocalVideoPort = 0, iLocalPortB = 0, iLocalVideoPortB = 0;
        int iVideoPort = ( clsRtp.GetMediaCount() >= 2 ) ? clsRtp.GetVideoPort() : 0;
        if ( !gclsCmpClient.AddSession( strRelaySessionId, strRelayLocalIp, iLocalPort, iLocalVideoPort, iLocalPortB,
                                        iLocalVideoPortB, "", "", pszReferToId ? pszReferToId : "", clsRtp.m_strIp,
                                        clsRtp.GetAudioPort(), iVideoPort, strRelaySesId ) ) {
            return false;
        }
        // 전환 대상(신규 leg = peer1)에게는 peer1 전용 포트를 광고
        iStartPort = iLocalPortB;
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
        const char *pszEvt = bActivate ? "alert_sent" : "alert_cancelled";
        int iFanout = 0;
        if ( bAllowed && bHaveGroup ) {
            if ( gclsCallDir.IsEnabled() )
                gclsCallDir.PttLogEvent(
                    pszTo, pszEvt, std::string( "{\"actor\":\"" ) + pszFrom + "\",\"target\":\"" + pszTo + "\"}" );
            // Phase 3b — 그룹 등록 멤버에게 alert MESSAGE fan-out (발신자 제외). affiliation 요구 그룹은
            //   affiliate 된 멤버만. 취소(alert-ind=false)도 동일 본문 전파로 멤버에 반영.
            {
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
                                                    &clsMemRoute ) )
                            iFanout++;
                    }
                }
            }
        }
        CLog::Print( LOG_INFO, "EventMessage: MCPTT emergency %s from(%s) to(%s) group=%d fanout=%d", pszEvt, pszFrom,
                     pszTo, bGroupTarget, iFanout );
        // 200 OK ack (경보 수신 확인).
        SendResponse( pclsMessage, SIP_OK );
        return true;
    }

    // MCData 그룹 SDS (TS 24.282) — 그룹 대상 MESSAGE 는 MCDATA-AS 가 게이트+fan-out.
    if ( m_clsMcDataAs.IsEnabled() && m_clsMcDataAs.OnMessage( pszFrom, pszTo, pclsMessage ) ) return true;

    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    if ( gclsUserMap.Select( pszTo, clsUserInfo ) == false ) return false;
    clsUserInfo.GetCallRoute( clsRoute );
    // 1:1 전달 — Content-Type 보존 (MCData disposition 통지 등 text/plain 이외 본문 대응)
    char szContentType[512];
    szContentType[0] = '\0';
    pclsMessage->m_clsContentType.ToString( szContentType, sizeof( szContentType ) );
    return gclsUserAgent.SendSms( pszFrom, pszTo, pclsMessage->m_strBody.c_str(), &clsRoute,
                                  szContentType[0] ? szContentType : NULL );
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
