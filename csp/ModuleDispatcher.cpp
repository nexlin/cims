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
extern void SendDialogEventNotify( const std::string &strWatchedAor, const std::string &strDlgCallId,
                                   const std::string &strState, const std::string &strDir,
                                   const std::string &strLocalAor, const std::string &strRemoteAor,
                                   const std::string &strLocalTag, const std::string &strRemoteTag );

// dialog-event(RFC 4235) 상태 통지 — 한 호의 두 당사자(caller/callee) 각각을 감시하는 구독자에게
//   그 당사자의 CSP 측 leg Call-ID 로 partial NOTIFY 를 낸다(당겨받기 BLF, §6.2). B2BUA 양 leg 는
//   From=caller/To=callee 로 동일하므로, 당사자별 leg 는 m_bRecv(caller-facing) 로 가른다.
static void NotifyDialogState( const char *pszCallId, const char *pszState ) {
    CCallInfo clsCi;
    if ( !gclsCallMap.Select( pszCallId, clsCi ) ) return;
    std::string strCaller, strCallee;
    gclsUserAgent.GetFromId( pszCallId, strCaller );  // A (발신)
    gclsUserAgent.GetToId( pszCallId, strCallee );    // B (착신)
    // caller-facing leg = m_bRecv=true 인 leg, callee-facing leg = m_bRecv=false 인 leg
    std::string strAleg, strBleg;
    if ( clsCi.m_bRecv ) {
        strAleg = pszCallId;
        strBleg = clsCi.m_strPeerCallId;
    } else {
        strBleg = pszCallId;
        strAleg = clsCi.m_strPeerCallId;
    }
    // 착신 B 감시자 → B-facing leg Call-ID (picker 가 Replaces 로 이 leg 를 가져간다)
    if ( !strCallee.empty() && !strBleg.empty() ) {
        std::string lt, rt;
        gclsUserAgent.GetDialogTags( strBleg.c_str(), lt, rt );
        SendDialogEventNotify( strCallee, strBleg, pszState, "recipient", strCallee, strCaller, lt, rt );
    }
    // 발신 A 감시자 → A-facing leg Call-ID
    if ( !strCaller.empty() && !strAleg.empty() ) {
        std::string lt, rt;
        gclsUserAgent.GetDialogTags( strAleg.c_str(), lt, rt );
        SendDialogEventNotify( strCaller, strAleg, pszState, "initiator", strCaller, strCallee, lt, rt );
    }
}

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

    // REFER(호 전달) 게이트 — 접속서비스 transfer_allowed=false 가입자의 전달 요청은 403
    //   (volte_supplementary_services.md §6.3). 통과 시 기존 흐름대로 psip 이 REFER 를 종단한다
    //   (B2BUA — EventTransfer/EventBlindTransfer).
    if ( pclsMessage->IsMethod( SIP_METHOD_REFER ) ) {
        const std::string strReferFrom = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
        ServiceInfo clsXferSvc = gclsServiceMap.GetForUser( strReferFrom, "volte" );
        if ( clsXferSvc.id > 0 && clsXferSvc.transfer_allowed == false ) {
            CLog::Print( LOG_INFO, "RecvRequest: REFER from(%s) denied — service '%s' transfer_allowed=false → 403",
                         strReferFrom.c_str(), clsXferSvc.name.c_str() );
            SendResponse( pclsMessage, SIP_FORBIDDEN );
            return true;
        }
    }

    // INVITE → Proxy 가능 여부 판단
    if ( pclsMessage->IsMethod( SIP_METHOD_INVITE ) ) {
        std::string strTo = pclsMessage->m_clsTo.m_clsUri.m_strUser;
        std::string strFrom = pclsMessage->m_clsFrom.m_clsUri.m_strUser;

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

// ═══ VoLTE relay 미디어 SRTP(SDES) — leg 별 종단 (media_security.md §5.2) ═══
//   relay 는 media-list passthrough 라 수신 crypto 를 그대로 흘리면 단말끼리 E2E SRTP 를
//   협상해 CMP 종단(녹취·NAT latch 판정)이 깨진다. 모든 전달 지점에서 crypto 라인을 벗기고
//   그 leg 의 협상 상태(CallMap RelaySdesLeg)로 다시 싣는다.

/** 발신(A) leg offer 의 한 미디어 평가 — 정책×offer 내용 → leg 상태.
 *  반환 1=SRTP(서버 키 생성) / 0=평문 / -1=협상 실패(488). */
static int _evalRelayOfferSdes( const ServiceInfo &clsSvc, const SDP_MEDIA_LIST &clsList, const char *pszMedia,
                                RelaySdesMedia &clsOut ) {
    std::string strTag, strSuite, strInline, strProto;
    int iRet = MediaSdes::ReadOfferCrypto( clsList, pszMedia, strTag, strSuite, strInline, strProto );
    clsOut.strProto = strProto;        // answer protocol echo 근거 (평문 포함)
    if ( strProto.empty() ) return 0;  // 미디어 부재/비활성
    if ( iRet < 0 ) return -1;         // SAVP 인데 유효 crypto 없음 — 폴백 불가
    if ( clsSvc.media_srtp == "off" ) {
        // off = a=crypto 무시(평문). 단 SAVP 단독 offer 는 평문 answer 가 불가(RFC 4568) → 488.
        return iRet == 1 && strncasecmp( strProto.c_str(), "RTP/SAVP", 8 ) == 0 ? -1 : 0;
    }
    if ( iRet == 0 ) {
        // crypto 없는 offer: required 는 488(SAVP 단일 정책), optional 은 평문 leg 허용.
        return clsSvc.media_srtp == "required" ? -1 : 0;
    }
    clsOut.bSrtp = true;
    clsOut.strTag = strTag;
    clsOut.strSuite = strSuite;
    clsOut.strUeKey = strInline;
    clsOut.strSrvKey = MediaSdes::GenerateInlineKeyB64();
    return clsOut.strSrvKey.empty() ? -1 : 1;
}

/** 착신(B) leg offer 의 한 미디어 재작성 — SRTP 면 서버 키 생성+SAVP+a=crypto, 평문이면 RTP/AVP
 *  정규화(A 가 SAVP 로 왔어도 이 leg 는 평문). 반환 false = 키 생성 실패. 미디어 비활성 = true. */
static bool _applyRelayLegOffer( SDP_MEDIA_LIST &clsList, const char *pszMedia, bool bSrtp, RelaySdesMedia &clsOut ) {
    std::string strTag, strSuite, strInline, strProto;
    MediaSdes::ReadOfferCrypto( clsList, pszMedia, strTag, strSuite, strInline, strProto );  // strip 후 — active 판정용
    if ( strProto.empty() ) return true;
    if ( !bSrtp ) {
        MediaSdes::ApplyCrypto( clsList, pszMedia, "RTP/AVP", "", "", "" );
        return true;
    }
    clsOut.bSrtp = true;
    clsOut.strTag = "1";
    clsOut.strSuite = "AES_CM_128_HMAC_SHA1_80";  // 기본 제안 (§2)
    clsOut.strProto = "RTP/SAVP";
    clsOut.strSrvKey = MediaSdes::GenerateInlineKeyB64();
    if ( clsOut.strSrvKey.empty() ) return false;
    MediaSdes::ApplyCrypto( clsList, pszMedia, "RTP/SAVP", clsOut.strTag, clsOut.strSuite, clsOut.strSrvKey );
    return true;
}

/** 착신(B) leg answer 의 한 미디어 검증·UE 키 확정 — SAVP offer 미디어는 같은 suite 의 유효
 *  crypto 가 있어야 한다(평문 폴백 금지). 미디어 거절(port 0)은 그 미디어만 비활성.
 *  반환 false = 협상 실패(호출자가 호 종료). */
static bool _evalRelayAnswerSdes( const SDP_MEDIA_LIST &clsList, const char *pszMedia, RelaySdesMedia &clsLeg,
                                  CmpMediaCrypto &clsOut ) {
    if ( !clsLeg.bSrtp ) return true;
    std::string strTag, strSuite, strInline, strProto;
    int iRet = MediaSdes::ReadOfferCrypto( clsList, pszMedia, strTag, strSuite, strInline, strProto );
    if ( strProto.empty() ) {  // 미디어 거절 — 평문(비활성)화
        clsLeg = RelaySdesMedia();
        return true;
    }
    if ( iRet != 1 || strSuite != clsLeg.strSuite ) return false;
    clsLeg.strUeKey = strInline;
    return MediaSdes::BuildCmpKeys( clsLeg.strSuite, clsLeg.strUeKey, clsLeg.strSrvKey, clsOut );
}

/** 재협상 offer 의 한 미디어 — SRTP leg 는 UE 재키잉만 반영(서버 키 유지: 이 leg 의 200 OK 는
 *  psip 이 기존 local SDP 로 답한다). crypto 소거 offer 는 키 유지 + ERROR(강등 수용 금지 —
 *  이후 unprotect 실패는 srtp_drop 으로 드러남). 동일 선언 재전송 = CMP 세션 유지. */
static void _readReinviteSdes( const SDP_MEDIA_LIST &clsList, const char *pszMedia, int iPeerIdx,
                               RelaySdesMedia &clsLeg, CmpMediaCrypto &clsOut ) {
    if ( !clsLeg.bSrtp ) return;
    std::string strTag, strSuite, strInline, strProto;
    int iRet = MediaSdes::ReadOfferCrypto( clsList, pszMedia, strTag, strSuite, strInline, strProto );
    if ( iRet != 1 || strSuite != clsLeg.strSuite ) {
        CLog::Print( LOG_ERROR, "EventReInvite: peer%d %s SRTP leg re-offer without matching crypto — 기존 키 유지",
                     iPeerIdx, pszMedia );
        return;
    }
    if ( strInline != clsLeg.strUeKey ) {
        clsLeg.strUeKey = strInline;
        CLog::Print( LOG_INFO, "EventReInvite: peer%d %s SRTP UE rekey", iPeerIdx, pszMedia );
    }
    MediaSdes::BuildCmpKeys( clsLeg.strSuite, clsLeg.strUeKey, clsLeg.strSrvKey, clsOut );
}

/** 상대 leg 로 나가는 SDP 를 그 leg 의 SDES 상태로 재작성. bOffer=true 면 protocol 을 상태로
 *  결정(SAVP/AVP), false(answer)면 그 leg offer 의 protocol echo. */
static void _rewriteRelaySdpForLeg( SDP_MEDIA_LIST &clsList, const RelaySdesLeg &clsLeg, bool bOffer ) {
    MediaSdes::StripCrypto( clsList );
    const RelaySdesMedia *arr[2] = { &clsLeg.clsAudio, &clsLeg.clsVideo };
    const char *arrName[2] = { "audio", "video" };
    for ( int i = 0; i < 2; ++i ) {
        const RelaySdesMedia &m = *arr[i];
        std::string strProto = bOffer ? std::string( m.bSrtp ? "RTP/SAVP" : "RTP/AVP" ) : m.strProto;
        MediaSdes::ApplyCrypto( clsList, arrName[i], strProto, m.strTag, m.strSuite,
                                m.bSrtp ? m.strSrvKey : std::string() );
    }
}

void CModuleDispatcher::EventIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                           CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
    CLog::Print( LOG_DEBUG, "EventIncomingCall: CallId=%s From=%s To=%s", pszCallId, pszFrom, pszTo );
    CspUser clsUser;
    CUserInfo clsUserInfo;
    bool bRoutePrefix = false;
    std::string strTo;

    // 수신 INVITE 의 Replaces(RFC 3891) — 관제 BLF 당겨받기·표준 attended 완결. 헤더가 있으면
    //   대상 다이얼로그를 pszCallId 로 교체하고 여기서 종결한다(정상 라우팅 미진입).
    if ( HandleIncomingReplaces( pszCallId, pszFrom, pclsRtp, pclsMessage ) ) return;

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
            CLog::Print( LOG_INFO, "EventIncomingCall: private call target(%s) not registered → 480 [PTT-AS]", pszTo );
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
        {
            std::string strPickupTarget;
            if ( IsPickupDial( pszFrom, pszTo, strPickupTarget ) ) {
                SetCallOwner( pszCallId, &m_clsTas );
                return PickUp( pszCallId, pszFrom, strPickupTarget.empty() ? NULL : strPickupTarget.c_str(), pclsRtp );
            }
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
    RelaySdesLeg clsSdesA, clsSdesB;  // leg 별 SDES 상태 — CallMap 기록용 (블록 밖 scope)
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
            int iSdesAudio = _evalRelayOfferSdes( clsVolteSvc, pclsRtp->m_clsMediaList, "audio", clsSdesA.clsAudio );
            int iSdesVideo = _evalRelayOfferSdes( clsVolteSvc, pclsRtp->m_clsMediaList, "video", clsSdesA.clsVideo );
            if ( iSdesAudio < 0 || iSdesVideo < 0 ) {
                CLog::Print( LOG_INFO,
                             "EventIncomingCall: caller(%s) SDES offer not acceptable (svc=%s media_srtp=%s) → 488",
                             pszFrom, clsVolteSvc.name.c_str(), clsVolteSvc.media_srtp.c_str() );
                return StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
            }
            if ( ( clsSdesA.clsAudio.bSrtp &&
                   !MediaSdes::BuildCmpKeys( clsSdesA.clsAudio.strSuite, clsSdesA.clsAudio.strUeKey,
                                             clsSdesA.clsAudio.strSrvKey, clsCallerAudioCrypto ) ) ||
                 ( clsSdesA.clsVideo.bSrtp &&
                   !MediaSdes::BuildCmpKeys( clsSdesA.clsVideo.strSuite, clsSdesA.clsVideo.strUeKey,
                                             clsSdesA.clsVideo.strSrvKey, clsCallerVideoCrypto ) ) ) {
                CLog::Print( LOG_ERROR, "EventIncomingCall: caller(%s) SRTP key build failed → 488", pszFrom );
                return StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
            }

            bool bCalleeSdes = false;
            if ( clsVolteSvc.media_srtp == "required" ) {
                bCalleeSdes = true;  // 능력 미선언 단말 포함 SAVP 단일 (§4)
            } else if ( clsVolteSvc.media_srtp == "optional" && !bRoutePrefix && pszTo ) {
                CUserInfo clsCalleeInfo;
                if ( gclsUserMap.Select( pszTo, clsCalleeInfo ) ) bCalleeSdes = clsCalleeInfo.m_bMediaSecSdes;
            }
            MediaSdes::StripCrypto( pclsRtp->m_clsMediaList );
            if ( !_applyRelayLegOffer( pclsRtp->m_clsMediaList, "audio", bCalleeSdes, clsSdesB.clsAudio ) ||
                 !_applyRelayLegOffer( pclsRtp->m_clsMediaList, "video", bCalleeSdes, clsSdesB.clsVideo ) ) {
                CLog::Print( LOG_ERROR, "EventIncomingCall: callee(%s) SRTP key build failed → 500", pszTo );
                return StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
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
        // leg 별 SDES 협상 상태 — answer 재작성(offer echo)·re-INVITE 키 유지/갱신의 원천 (§5.2)
        gclsCallMap.SetRelaySdesLeg( pszCallId, 0, clsSdesA );
        gclsCallMap.SetRelaySdesLeg( pszCallId, 1, clsSdesB );
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
            pclsRtp->m_strIp = clsCallInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress()
                                                                     : clsCallInfo.m_strRelayLocalIp;
            // 18x(early media) SDP 도 전달받는 leg 상태로 재작성 — 링잉 leg crypto 투과 차단 (§5.2).
            //   링잉 leg 키의 CMP 반영·검증은 확정 answer(EventCallStart)에서. leg index 는
            //   m_bRecv(=peer0 표식)로 판정 — 전달·픽업 재결합 pair 는 남는 쪽이 peer1 일 수 있다.
            if ( !clsCallInfo.m_strRelaySessionId.empty() )
                _rewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsCallInfo.m_clsSdesLeg[clsCallInfo.m_bRecv ? 1 : 0],
                                        false );
        }
        int iRSeq = gclsUserAgent.GetRSeq( pszCallId );
        if ( iRSeq != -1 ) gclsUserAgent.SetRSeq( clsCallInfo.m_strPeerCallId.c_str(), iRSeq );
        gclsUserAgent.RingCall( clsCallInfo.m_strPeerCallId.c_str(), iSipStatus, pclsRtp );
        // dialog-event: 착신 링잉(early) 통지 — 감시자(BLF)가 당겨받기 대상을 알 수 있게 (§6.2)
        if ( iSipStatus >= 180 && iSipStatus < 200 ) NotifyDialogState( pszCallId, "early" );
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

    // dialog-event: 호 확립(confirmed) 통지 (§6.2). 이 leg 가 CallMap 에 있을 때만.
    if ( gclsCallMap.Select( pszCallId ) ) NotifyDialogState( pszCallId, "confirmed" );

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
            // ── answer leg SDES 검증·UE 키 확정 (media_security.md §5.2) — SAVP offer 에
            //    crypto 없는/불일치 answer 는 종료(평문 폴백 금지). UE 키는 CMP 해당 peer rx 로 내린다.
            RelaySdesLeg clsSdesB = clsCallInfo.m_clsSdesLeg[iAnswerIdx];
            CmpMediaCrypto clsCalleeAudioCrypto, clsCalleeVideoCrypto;
            if ( !clsCallInfo.m_strRelaySessionId.empty() ) {
                if ( !_evalRelayAnswerSdes( pclsRtp->m_clsMediaList, "audio", clsSdesB.clsAudio,
                                            clsCalleeAudioCrypto ) ||
                     !_evalRelayAnswerSdes( pclsRtp->m_clsMediaList, "video", clsSdesB.clsVideo,
                                            clsCalleeVideoCrypto ) ) {
                    CLog::Print( LOG_ERROR,
                                 "EventCallStart: callee(%s) SDES answer missing/mismatched crypto on SAVP offer — "
                                 "평문 폴백 금지, 호 종료 (CallId=%s)",
                                 clsCallInfo.m_strRelayCallee.c_str(), pszCallId );
                    gclsUserAgent.StopCall( pszCallId );
                    return;
                }
                gclsCallMap.SetRelaySdesLeg( pszCallId, iAnswerIdx, clsSdesB );
            }
            // answer leg RTP 주소를 CMP 에 MODIFY (peer_index=iAnswerIdx) — session_id 로 직접 지목.
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
                    CGroupCallService::GetLegPt( pszCallId, true, iCalleePt, iCalleeSrcPt, iCalleeTePt, iCalleeSrcTePt,
                                                 &strCalleeCodec );
                    gclsCmpClient.ModifySession(
                        clsCallInfo.m_strRelaySessionId, pclsRtp->m_strIp, iAudioPort, iVideoPort > 0 ? iVideoPort : 0,
                        iAnswerIdx, clsCallInfo.m_strRelayCaller, clsCallInfo.m_strRelayCallee,
                        clsCallInfo.m_strRelaySesId, iCalleeNat, strCalleeGuardIp, iCalleePt, iCalleeSrcPt, iCalleeTePt,
                        iCalleeSrcTePt, strCalleeCodec, clsCalleeAudioCrypto.bEnabled ? &clsCalleeAudioCrypto : NULL,
                        clsCalleeVideoCrypto.bEnabled ? &clsCalleeVideoCrypto : NULL );
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

            // 상대 leg 로 나가는 SDP 재작성 — answer leg 키 투과 차단 + 상대 leg 상태로 재광고 (§5.2).
            //   AcceptCall(answer)은 상대 offer 의 tag/suite/protocol echo, SendReInvite(offer)는
            //   leg 상태로 protocol 결정.
            _rewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsCallInfo.m_clsSdesLeg[1 - iAnswerIdx],
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
    } else if ( gclsTransCallMap.Select( pszCallId, clsCallInfo ) ) {
        // blind transfer 완결 — 전환 대상 leg 의 answer. 원 통화의 relay 세션을 유지한 채 전환
        //   leg 를 RELAY_MODIFY 로 재고정하고 남는 leg 와 재결합한다 (포트 산술 금지 —
        //   cmp_media_api.md §6.2, media_security.md §5.2. 구 ±4 leg 블록 추정 제거).
        const std::string strTransferorCallId = clsCallInfo.m_strPeerCallId;  // REFER 를 보낸(떠나는) leg
        CCallInfo clsOldInfo, clsStayInfo;
        if ( gclsCallMap.Select( strTransferorCallId.c_str(), clsOldInfo ) == false ||
             gclsCallMap.Select( clsOldInfo.m_strPeerCallId.c_str(), clsStayInfo ) == false ) {
            gclsUserAgent.SendNotify( strTransferorCallId.c_str(), SIP_OK );
            gclsTransCallMap.Delete( pszCallId, false );
            gclsUserAgent.StopCall( pszCallId );
            return;
        }
        const std::string strStayCallId = clsOldInfo.m_strPeerCallId;  // 남는(전환받는) leg
        const bool bRelay = !clsOldInfo.m_strRelaySessionId.empty();
        const int iStayIdx = clsStayInfo.m_bRecv ? 0 : 1;  // 남는 leg 의 relay peer index
        const int iNewIdx = 1 - iStayIdx;                  // 전환 leg 가 승계하는 index

        // 전환 leg answer SDES 검증 — offer 상태는 EventBlindTransfer 가 trans entry 에 저장.
        //   SAVP offer 에 crypto 없는 answer 는 전환만 중단(평문 폴백 금지) — 원 통화는 유지.
        RelaySdesLeg clsNewLeg = clsCallInfo.m_clsSdesLeg[iNewIdx];
        CmpMediaCrypto clsNewAudioCrypto, clsNewVideoCrypto;
        if ( bRelay && pclsRtp &&
             ( !_evalRelayAnswerSdes( pclsRtp->m_clsMediaList, "audio", clsNewLeg.clsAudio, clsNewAudioCrypto ) ||
               !_evalRelayAnswerSdes( pclsRtp->m_clsMediaList, "video", clsNewLeg.clsVideo, clsNewVideoCrypto ) ) ) {
            CLog::Print( LOG_ERROR,
                         "EventCallStart: transfer leg SDES answer missing/mismatched crypto — 전환 중단, 원 통화 유지 "
                         "(CallId=%s)",
                         pszCallId );
            gclsUserAgent.SendNotify( strTransferorCallId.c_str(), SIP_NOT_ACCEPTABLE_HERE );
            gclsTransCallMap.Delete( pszCallId, false );
            gclsUserAgent.StopCall( pszCallId );
            return;
        }
        gclsUserAgent.SendNotify( strTransferorCallId.c_str(), SIP_OK );

        std::string strNewUserId;
        gclsUserAgent.GetToId( pszCallId, strNewUserId );
        const std::string strNewCaller = iNewIdx == 0 ? strNewUserId : clsOldInfo.m_strRelayCaller;
        const std::string strNewCallee = iNewIdx == 1 ? strNewUserId : clsOldInfo.m_strRelayCallee;

        if ( bRelay && pclsRtp ) {
            // 전환 leg 를 기존 relay 의 승계 index 로 재고정 — 주소·NAT·PT·crypto (정상 answer 경로와 동형)
            int iAudioPort = pclsRtp->GetAudioPort();
            if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
            if ( iAudioPort > 0 ) {
                int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;
                int iNewNat = 0;
                std::string strNewGuardIp;
                {
                    ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( strNewUserId, "volte" );
                    std::string strSigIp;
                    CUserInfo clsNewUserInfo;
                    if ( !strNewUserId.empty() && gclsUserMap.Select( strNewUserId.c_str(), clsNewUserInfo ) )
                        strSigIp = clsNewUserInfo.m_strIp;
                    if ( CCspServiceMap::EvalMediaNat( clsNatSvc, pclsRtp->m_strIp, strSigIp, strNewGuardIp ) )
                        iNewNat = 1;
                }
                int iNewPt = 0, iNewSrcPt = 0, iNewTePt = 0, iNewSrcTePt = 0;
                std::string strNewCodec;
                CGroupCallService::GetLegPt( pszCallId, true, iNewPt, iNewSrcPt, iNewTePt, iNewSrcTePt, &strNewCodec );
                gclsCmpClient.ModifySession( clsOldInfo.m_strRelaySessionId, pclsRtp->m_strIp, iAudioPort,
                                             iVideoPort > 0 ? iVideoPort : 0, iNewIdx, strNewCaller, strNewCallee,
                                             clsOldInfo.m_strRelaySesId, iNewNat, strNewGuardIp, iNewPt, iNewSrcPt,
                                             iNewTePt, iNewSrcTePt, strNewCodec,
                                             clsNewAudioCrypto.bEnabled ? &clsNewAudioCrypto : NULL,
                                             clsNewVideoCrypto.bEnabled ? &clsNewVideoCrypto : NULL );
            }
        }

        // 떠나는 leg 종료 + 원 pair 해체 — relay 는 계속 쓰므로 회수 금지(bStopPort=false)
        gclsUserAgent.StopCall( strTransferorCallId.c_str() );
        gclsCallMap.Delete( strTransferorCallId.c_str(), false );

        // 새 pair — entry 포트 = 그 leg 의 peer 에게 광고하는 relay 포트(각 leg 포트 불변),
        //   m_bRecv 는 peer0 표식(EventReInvite/EventCallStart 의 index 판정 근거).
        if ( iStayIdx == 0 )
            gclsCallMap.Insert( strStayCallId.c_str(), pszCallId, clsStayInfo.m_iPeerRtpPort,
                                clsOldInfo.m_iPeerRtpPort );
        else
            gclsCallMap.Insert( pszCallId, strStayCallId.c_str(), clsOldInfo.m_iPeerRtpPort,
                                clsStayInfo.m_iPeerRtpPort );
        if ( bRelay ) {
            gclsCallMap.SetRelayInfo( pszCallId, clsOldInfo.m_strRelaySessionId, clsOldInfo.m_strRelaySesId,
                                      clsOldInfo.m_strRelayLocalIp, strNewCaller, strNewCallee );
            gclsCallMap.SetRelaySdesLeg( pszCallId, iStayIdx, clsOldInfo.m_clsSdesLeg[iStayIdx] );
            gclsCallMap.SetRelaySdesLeg( pszCallId, iNewIdx, clsNewLeg );
        }
        gclsCallMap.SetEstablished( pszCallId );

        // 남는 leg 로 re-INVITE — 전환 leg answer 를 남는 leg 상태로 재작성 + relay 주소·기존 포트 재광고
        if ( bRelay && pclsRtp && clsOldInfo.m_iPeerRtpPort > 0 ) {
            _rewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsOldInfo.m_clsSdesLeg[iStayIdx], true );
            std::string strRelayIp = clsOldInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress()
                                                                          : clsOldInfo.m_strRelayLocalIp;
            pclsRtp->SetIpPort( strRelayIp.c_str(), clsOldInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        }
        gclsUserAgent.SendReInvite( strStayCallId.c_str(), pclsRtp );
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

    // dialog-event: 종료(terminated) 통지 — CallMap 삭제 전에 leg 식별이 살아 있을 때 낸다 (§6.2).
    if ( bSelHit ) NotifyDialogState( pszCallId, "terminated" );

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
                // 재협상 leg SDES — UE 재키잉만 반영(서버 키 유지: psip 자동 200 이 기존 local SDP
                //   로 답한다). 동일 선언 재전송 = CMP 세션 유지 (§5.2·§6.3).
                RelaySdesLeg clsSdesLeg = clsCallInfo.m_clsSdesLeg[iPeerIdx];
                CmpMediaCrypto clsLegAudioCrypto, clsLegVideoCrypto;
                _readReinviteSdes( pclsRemoteRtp->m_clsMediaList, "audio", iPeerIdx, clsSdesLeg.clsAudio,
                                   clsLegAudioCrypto );
                _readReinviteSdes( pclsRemoteRtp->m_clsMediaList, "video", iPeerIdx, clsSdesLeg.clsVideo,
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
                _rewriteRelaySdpForLeg( pclsRemoteRtp->m_clsMediaList, clsCallInfo.m_clsSdesLeg[iTargetLeg], true );
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
    _readReinviteSdes( pclsRemoteRtp->m_clsMediaList, "audio", iPeerIdx, clsSdesLeg.clsAudio, clsAudioCrypto );
    _readReinviteSdes( pclsRemoteRtp->m_clsMediaList, "video", iPeerIdx, clsSdesLeg.clsVideo, clsVideoCrypto );
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
                _rewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsCallInfo.m_clsSdesLeg[iTargetLeg], false );
            }
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

    // pszCallId/pszReferToCallId = 전환 지시자(REFER 발신 단말)의 두 다이얼로그 leg. 남는 당사자는
    //   각 pair 의 peer leg — 유지(stay) = 원 통화의 peer, 합류(join) = 상담 통화의 peer.
    //   원 통화의 relay 세션을 유지해 stay leg 의 미디어 앵커(광고된 relay 포트)를 불변으로 두고,
    //   전환 지시자가 쓰던 peer index 로 join 단말을 RELAY_MODIFY 재고정한다 (cmp_media_api.md §6.2).
    //   상담 통화의 relay 는 pair 해체 시 회수한다. (구 ±4 leg 블록 포트 산술 제거.)
    const std::string strStayCallId = clsCallInfo.m_strPeerCallId;
    const std::string strJoinCallId = clsReferToCallInfo.m_strPeerCallId;
    CCallInfo clsStayInfo, clsJoinInfo;
    if ( gclsCallMap.Select( strStayCallId.c_str(), clsStayInfo ) == false ) return false;
    if ( gclsCallMap.Select( strJoinCallId.c_str(), clsJoinInfo ) == false ) return false;

    if ( gclsUserAgent.GetRemoteCallRtp( strStayCallId.c_str(), &clsRtp ) == false ) return false;
    clsRtp.SetDirection( E_RTP_SEND_RECV );  // join 단말로 보낼 재-offer 본문 (stay 원격 SDP 기반)

    const bool bRelay = !clsCallInfo.m_strRelaySessionId.empty();
    const int iStayIdx = clsStayInfo.m_bRecv ? 0 : 1;  // stay leg 의 relay peer index
    const int iNewIdx = 1 - iStayIdx;                  // join 단말이 승계하는 index
    const int iStayPort = clsCallInfo.m_iPeerRtpPort;  // stay 에게 광고된 relay 포트 (불변)
    const int iJoinPort = clsStayInfo.m_iPeerRtpPort;  // 전환 지시자 leg 포트 → join 이 승계
    const std::string strRelayIp =
        clsCallInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : clsCallInfo.m_strRelayLocalIp;
    std::string strStayUserId, strJoinUserId;
    gclsUserAgent.GetToId( strStayCallId.c_str(), strStayUserId );
    gclsUserAgent.GetToId( strJoinCallId.c_str(), strJoinUserId );
    const std::string strNewCaller = iStayIdx == 0 ? strStayUserId : strJoinUserId;
    const std::string strNewCallee = iStayIdx == 0 ? strJoinUserId : strStayUserId;

    if ( bScreenedTransfer ) {
        CSipCallRtp clsReferToRtp;
        if ( gclsUserAgent.GetRemoteCallRtp( strJoinCallId.c_str(), &clsReferToRtp ) == false ) return false;
        clsReferToRtp.SetDirection( E_RTP_SEND_RECV );  // stay 로 보낼 재-offer 본문 (join 원격 SDP 기반)

        // join 단말이 상담 통화에서 확립한 SDES leg 상태를 그대로 이관 — 같은 서버 키를 재광고하고
        //   기존 UE 선언 키를 유지 relay 의 승계 index 로 내린다 (재키잉 없음, media_security.md §5.2).
        const int iJoinOldIdx = clsJoinInfo.m_bRecv ? 0 : 1;
        RelaySdesLeg clsJoinLeg = clsJoinInfo.m_clsSdesLeg[iJoinOldIdx];
        CmpMediaCrypto clsJoinAudioCrypto, clsJoinVideoCrypto;
        if ( bRelay && ( ( clsJoinLeg.clsAudio.bSrtp &&
                           !MediaSdes::BuildCmpKeys( clsJoinLeg.clsAudio.strSuite, clsJoinLeg.clsAudio.strUeKey,
                                                     clsJoinLeg.clsAudio.strSrvKey, clsJoinAudioCrypto ) ) ||
                         ( clsJoinLeg.clsVideo.bSrtp &&
                           !MediaSdes::BuildCmpKeys( clsJoinLeg.clsVideo.strSuite, clsJoinLeg.clsVideo.strUeKey,
                                                     clsJoinLeg.clsVideo.strSrvKey, clsJoinVideoCrypto ) ) ) ) {
            CLog::Print( LOG_ERROR, "EventTransfer: SRTP key carry-over failed — 전환 거부 (CallId=%s)", pszCallId );
            return false;
        }

        // pair 해체 — 유지 relay(원 통화)는 회수 금지(bStopPort=false), 상담 통화 relay 는 여기서 회수
        gclsCallMap.Delete( pszCallId, false );
        gclsCallMap.Delete( pszReferToCallId, true );

        if ( bRelay ) {
            // join 단말의 미디어를 유지 relay 의 승계 index 로 재고정 — 주소는 join 의 UE 선언 주소
            int iAudioPort = clsReferToRtp.GetAudioPort();
            if ( iAudioPort <= 0 && clsReferToRtp.m_iPort > 0 ) iAudioPort = clsReferToRtp.m_iPort;
            if ( iAudioPort > 0 ) {
                int iVideoPort = ( clsReferToRtp.GetMediaCount() >= 2 ) ? clsReferToRtp.GetVideoPort() : 0;
                int iJoinNat = 0;
                std::string strJoinGuardIp;
                {
                    ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( strJoinUserId, "volte" );
                    std::string strSigIp;
                    CUserInfo clsJoinUserInfo;
                    if ( !strJoinUserId.empty() && gclsUserMap.Select( strJoinUserId.c_str(), clsJoinUserInfo ) )
                        strSigIp = clsJoinUserInfo.m_strIp;
                    if ( CCspServiceMap::EvalMediaNat( clsNatSvc, clsReferToRtp.m_strIp, strSigIp, strJoinGuardIp ) )
                        iJoinNat = 1;
                }
                int iJoinPt = 0, iJoinSrcPt = 0, iJoinTePt = 0, iJoinSrcTePt = 0;
                std::string strJoinCodec;
                CGroupCallService::GetLegPt( strJoinCallId, clsJoinInfo.m_bRecv ? false : true, iJoinPt, iJoinSrcPt,
                                             iJoinTePt, iJoinSrcTePt, &strJoinCodec );
                gclsCmpClient.ModifySession( clsCallInfo.m_strRelaySessionId, clsReferToRtp.m_strIp, iAudioPort,
                                             iVideoPort > 0 ? iVideoPort : 0, iNewIdx, strNewCaller, strNewCallee,
                                             clsCallInfo.m_strRelaySesId, iJoinNat, strJoinGuardIp, iJoinPt, iJoinSrcPt,
                                             iJoinTePt, iJoinSrcTePt, strJoinCodec,
                                             clsJoinAudioCrypto.bEnabled ? &clsJoinAudioCrypto : NULL,
                                             clsJoinVideoCrypto.bEnabled ? &clsJoinVideoCrypto : NULL );
            }
        }

        // 새 pair + relay descriptor — entry 포트 = 그 leg 의 peer 에게 광고하는 relay 포트
        if ( iStayIdx == 0 )
            gclsCallMap.Insert( strStayCallId.c_str(), strJoinCallId.c_str(), iJoinPort, iStayPort );
        else
            gclsCallMap.Insert( strJoinCallId.c_str(), strStayCallId.c_str(), iStayPort, iJoinPort );
        if ( bRelay ) {
            gclsCallMap.SetRelayInfo( strStayCallId.c_str(), clsCallInfo.m_strRelaySessionId,
                                      clsCallInfo.m_strRelaySesId, clsCallInfo.m_strRelayLocalIp, strNewCaller,
                                      strNewCallee );
            gclsCallMap.SetRelaySdesLeg( strStayCallId.c_str(), iStayIdx, clsCallInfo.m_clsSdesLeg[iStayIdx] );
            gclsCallMap.SetRelaySdesLeg( strStayCallId.c_str(), iNewIdx, clsJoinLeg );
        }
        gclsCallMap.SetEstablished( strStayCallId.c_str() );

        // 양 leg 재-offer — 각 수신 leg 의 SDES 상태로 재작성 + relay 주소·기존 leg 포트 재광고
        if ( bRelay ) {
            _rewriteRelaySdpForLeg( clsReferToRtp.m_clsMediaList, clsCallInfo.m_clsSdesLeg[iStayIdx], true );
            clsReferToRtp.SetIpPort( strRelayIp.c_str(), iStayPort, SOCKET_COUNT_PER_MEDIA );
            _rewriteRelaySdpForLeg( clsRtp.m_clsMediaList, clsJoinLeg, true );
            clsRtp.SetIpPort( strRelayIp.c_str(), iJoinPort, SOCKET_COUNT_PER_MEDIA );
        } else {
            // 직결(비 relay) — crypto 투과만 차단 (기존 동작 보존)
            MediaSdes::StripCrypto( clsRtp.m_clsMediaList );
            MediaSdes::StripCrypto( clsReferToRtp.m_clsMediaList );
        }
        gclsUserAgent.SendReInvite( strStayCallId.c_str(), &clsReferToRtp );
        gclsUserAgent.SendReInvite( strJoinCallId.c_str(), &clsRtp );

        gclsUserAgent.StopCall( pszCallId );
        gclsUserAgent.StopCall( pszReferToCallId, SIP_REQUEST_TERMINATED );
        return true;
    }

    // unscreened — 상담 통화 미확립: 상담 pair 전체 해체(relay 회수) 후, 유지 relay 의 승계 index
    //   포트로 join 대상에게 신규 INVITE. answer 재고정은 EventCallStart 정상 경로가 수행한다.
    gclsCallMap.Delete( pszCallId, false );
    gclsCallMap.Delete( pszReferToCallId, true );
    gclsUserAgent.StopCall( pszCallId );
    gclsUserAgent.StopCall( pszReferToCallId, SIP_REQUEST_TERMINATED );

    std::string strNewCallId;
    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    if ( gclsUserMap.Select( strJoinUserId.c_str(), clsUserInfo ) == false ) {
        gclsUserAgent.StopCall( strStayCallId.c_str() );
        gclsUserAgent.StopCall( strJoinCallId.c_str() );
        return true;
    }
    gclsUserAgent.StopCall( strJoinCallId.c_str() );  // 미확립 상담 leg 정리(CANCEL)

    RelaySdesLeg clsNewLeg;
    MediaSdes::StripCrypto( clsRtp.m_clsMediaList );  // stay leg 키 누출 차단
    if ( bRelay ) {
        // 신규 leg offer — 정책 × join 단말의 mediasec 능력으로 형태 결정, 서버 키는 새로 생성
        //   (떠나는 전환 지시자에게 알려진 키를 재사용하지 않는다).
        bool bNewSdes = false;
        ServiceInfo clsJoinSvc = gclsServiceMap.GetForUser( strJoinUserId, "volte" );
        if ( clsJoinSvc.media_srtp == "required" )
            bNewSdes = true;
        else if ( clsJoinSvc.media_srtp == "optional" )
            bNewSdes = clsUserInfo.m_bMediaSecSdes;
        if ( !_applyRelayLegOffer( clsRtp.m_clsMediaList, "audio", bNewSdes, clsNewLeg.clsAudio ) ||
             !_applyRelayLegOffer( clsRtp.m_clsMediaList, "video", bNewSdes, clsNewLeg.clsVideo ) ) {
            CLog::Print( LOG_ERROR, "EventTransfer: transfer leg SRTP key build failed (CallId=%s)", pszCallId );
            gclsUserAgent.StopCall( strStayCallId.c_str() );
            return false;
        }
        clsRtp.SetIpPort( strRelayIp.c_str(), iJoinPort, SOCKET_COUNT_PER_MEDIA );
    }

    clsUserInfo.GetCallRoute( clsRoute );
    if ( gclsUserAgent.StartCall( strStayUserId.c_str(), strJoinUserId.c_str(), &clsRtp, &clsRoute, strNewCallId ) ==
         false ) {
        gclsUserAgent.StopCall( strStayCallId.c_str() );
        return false;
    }
    if ( iStayIdx == 0 )
        gclsCallMap.Insert( strStayCallId.c_str(), strNewCallId.c_str(), iJoinPort, iStayPort );
    else
        gclsCallMap.Insert( strNewCallId.c_str(), strStayCallId.c_str(), iStayPort, iJoinPort );
    if ( bRelay ) {
        gclsCallMap.SetRelayInfo( strStayCallId.c_str(), clsCallInfo.m_strRelaySessionId, clsCallInfo.m_strRelaySesId,
                                  clsCallInfo.m_strRelayLocalIp, strNewCaller, strNewCallee );
        gclsCallMap.SetRelaySdesLeg( strStayCallId.c_str(), iStayIdx, clsCallInfo.m_clsSdesLeg[iStayIdx] );
        gclsCallMap.SetRelaySdesLeg( strStayCallId.c_str(), iNewIdx, clsNewLeg );
    }
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

    // 원 통화의 relay 세션을 유지 — 전환 대상은 REFER 지시자 leg 의 peer index·포트를 승계한다
    //   (신규 AddSession 없음: 실패·거절 시 원 통화가 그대로 남고, relay 누수 경로도 사라진다).
    //   완결(answer 재고정·pair 재결합)은 EventCallStart 의 trans 분기가 RELAY_MODIFY 로 수행.
    CCallInfo clsOldInfo, clsStayInfo;
    const bool bRelay = gclsCallMap.Select( pszCallId, clsOldInfo ) && !clsOldInfo.m_strRelaySessionId.empty() &&
                        gclsCallMap.Select( strCallId.c_str(), clsStayInfo );
    RelaySdesLeg clsNewLeg;
    int iNewIdx = 1;
    MediaSdes::StripCrypto( clsRtp.m_clsMediaList );  // 남는 leg 키 누출 차단
    if ( bRelay ) {
        iNewIdx = clsOldInfo.m_bRecv ? 0 : 1;     // 지시자 leg 의 index = 전환 대상이 승계
        iStartPort = clsStayInfo.m_iPeerRtpPort;  // 지시자 leg 의 relay 포트 → 전환 대상에게 광고
        // 신규 leg offer — 정책 × 전환 대상의 mediasec 능력, 서버 키는 새로 생성 (떠나는 단말에
        //   알려진 키 재사용 금지 — media_security.md §5.2)
        bool bNewSdes = false;
        ServiceInfo clsNewSvc = gclsServiceMap.GetForUser( pszReferToId ? pszReferToId : "", "volte" );
        if ( clsNewSvc.media_srtp == "required" )
            bNewSdes = true;
        else if ( clsNewSvc.media_srtp == "optional" )
            bNewSdes = clsUserInfo.m_bMediaSecSdes;
        if ( !_applyRelayLegOffer( clsRtp.m_clsMediaList, "audio", bNewSdes, clsNewLeg.clsAudio ) ||
             !_applyRelayLegOffer( clsRtp.m_clsMediaList, "video", bNewSdes, clsNewLeg.clsVideo ) ) {
            CLog::Print( LOG_ERROR, "EventBlindTransfer: transfer leg SRTP key build failed (CallId=%s)", pszCallId );
            return false;
        }
        std::string strRelayIp =
            clsOldInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : clsOldInfo.m_strRelayLocalIp;
        clsRtp.SetIpPort( strRelayIp.c_str(), iStartPort, SOCKET_COUNT_PER_MEDIA );
    }

    clsUserInfo.GetCallRoute( clsRoute );
    if ( gclsUserAgent.StartCall( strToId.c_str(), pszReferToId, &clsRtp, &clsRoute, strInviteCallId ) == false )
        return false;
    gclsTransCallMap.Insert( pszCallId, strInviteCallId.c_str(), iStartPort );
    // 전환 leg 의 SDES offer 상태만 trans entry 에 보관 — relay descriptor 는 보관하지 않는다
    //   (trans Delete 가 계속 쓰는 원 통화의 relay 를 회수하지 않도록).
    if ( bRelay ) gclsTransCallMap.SetRelaySdesLeg( pszCallId, iNewIdx, clsNewLeg );
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
//  Call Pickup (당겨받기 — volte_supplementary_services.md §5)
// ──────────────────────────────────────────────────────────────

bool CModuleDispatcher::IsPickupDial( const char *pszFrom, const char *pszTo, std::string &strTarget ) {
    strTarget.clear();
    if ( pszTo == NULL || pszTo[0] == '\0' ) return false;

    // 코드 결정 — 발신 가입자의 접속서비스 pickup_feature_code. 필드 미지정(레거시 레코드)이면
    //   전역 Setup.Sip.CallPickupId 폴백(전환기 호환), 빈 값이면 그 서비스에서 픽업 비활성 (§5.2).
    ServiceInfo clsSvc = gclsServiceMap.GetForUser( pszFrom ? pszFrom : "", "volte" );
    std::string strCode;
    if ( clsSvc.id > 0 && clsSvc.pickup_code_set )
        strCode = clsSvc.pickup_feature_code;
    else
        strCode = gclsSetup.m_strCallPickupId;
    if ( strCode.empty() ) return false;

    const size_t iLen = strCode.size();
    if ( strncmp( pszTo, strCode.c_str(), iLen ) != 0 ) return false;
    if ( pszTo[iLen] == '\0' ) return true;  // 그룹 픽업
    strTarget = pszTo + iLen;                // 지정 픽업 — "<code><내선>"
    return true;
}

void CModuleDispatcher::PickUp( const char *pszCallId, const char *pszFrom, const char *pszTarget,
                                CSipCallRtp *pclsRtp ) {
    CspUser xmlFrom;
    USER_ID_LIST clsUserIdList;
    bool bCallPickup = false;
    bool bCommitted = false;  // 재키잉(원 착신 leg 해체) 이후에는 다음 후보로 넘어갈 수 없다

    // 후보 결정 — 축은 픽업 그룹(pickup_group, 미지정 시 org 폴백 — §5.1).
    //   그룹 픽업: 그룹 인덱스의 등록 그룹원 전체. 지정 픽업: 대상 내선 하나 — 같은 그룹일 때만(403).
    if ( gclsCspUserMap.Select( pszFrom, xmlFrom ) ) {
        if ( pszTarget != NULL && pszTarget[0] != '\0' ) {
            CUserInfo clsTargetInfo;
            if ( gclsUserMap.Select( pszTarget, clsTargetInfo ) == false ) return StopCall( pszCallId, SIP_NOT_FOUND );
            if ( clsTargetInfo.m_strGroupId != xmlFrom.EffectivePickupGroup() ) {
                CLog::Print( LOG_INFO, "PickUp: directed target(%s) not in picker(%s) group(%s) → 403", pszTarget,
                             pszFrom, xmlFrom.EffectivePickupGroup().c_str() );
                return StopCall( pszCallId, SIP_FORBIDDEN );
            }
            clsUserIdList.push_back( pszTarget );
        } else {
            gclsUserMap.SelectGroup( xmlFrom.EffectivePickupGroup().c_str(), clsUserIdList );
        }
    }

    for ( USER_ID_LIST::iterator itUIL = clsUserIdList.begin();
          itUIL != clsUserIdList.end() && bCallPickup == false && bCommitted == false; ++itUIL ) {
        std::string strOldCallId;
        if ( gclsCallMap.SelectToRing( itUIL->c_str(), strOldCallId ) == false ) continue;
        // SDES 협상 불가(488)는 원 호를 건드리지 않고 즉시 종료; 재키잉 이후 실패는 후보 순회 중단.
        int iRc = PickUpLeg( pszCallId, pszFrom, pclsRtp, strOldCallId );
        if ( iRc == 0 ) {
            bCallPickup = true;
        } else if ( iRc == SIP_NOT_ACCEPTABLE_HERE ) {
            return StopCall( pszCallId, iRc );
        } else {
            bCommitted = true;  // 재키잉 후 실패 — 다른 후보로 넘어가지 않는다
        }
    }

    if ( bCallPickup == false ) StopCall( pszCallId, SIP_NOT_FOUND );
}

// 당겨받기 재고정 코어 — 링잉/대상 leg(strOldCallId)를 pszCallId(신규 단말)로 승계·재고정.
//   PickUp(피처코드)·HandleIncomingReplaces(RFC 3891) 공용. 반환 0=성공, >0=실패 SIP 코드.
int CModuleDispatcher::PickUpLeg( const char *pszCallId, const char *pszFrom, CSipCallRtp *pclsRtp,
                                  const std::string &strOldCallId ) {
    // 링잉 호의 양 leg — 발신자에게 광고할 relay 포트(peer0)는 링잉(착신) leg entry 에,
    //   신규(픽업) 단말에게 광고할 relay 포트(peer1)는 발신 leg entry 에 있다.
    CCallInfo clsOldCallInfo, clsPeerCallInfo;
    if ( gclsCallMap.Select( strOldCallId.c_str(), clsOldCallInfo ) == false ) return SIP_NOT_FOUND;
    if ( gclsCallMap.Select( clsOldCallInfo.m_strPeerCallId.c_str(), clsPeerCallInfo ) == false ) return SIP_NOT_FOUND;

    const bool bRelay = !clsOldCallInfo.m_strRelaySessionId.empty();
    RelaySdesLeg clsNewLeg;
    CmpMediaCrypto clsNewAudioCrypto, clsNewVideoCrypto;
    if ( bRelay && pclsRtp ) {
        // 신규 단말 offer 의 SDES 평가(정책 = 신규 단말의 접속서비스). 협상 불가는 원 호를
        //   건드리지 않고 488 (평문 폴백 금지) — 아직 재키잉 전이라 원 호가 그대로 유지된다.
        ServiceInfo clsPickSvc = gclsServiceMap.GetForUser( pszFrom ? pszFrom : "", "volte" );
        if ( _evalRelayOfferSdes( clsPickSvc, pclsRtp->m_clsMediaList, "audio", clsNewLeg.clsAudio ) < 0 ||
             _evalRelayOfferSdes( clsPickSvc, pclsRtp->m_clsMediaList, "video", clsNewLeg.clsVideo ) < 0 ||
             ( clsNewLeg.clsAudio.bSrtp &&
               !MediaSdes::BuildCmpKeys( clsNewLeg.clsAudio.strSuite, clsNewLeg.clsAudio.strUeKey,
                                         clsNewLeg.clsAudio.strSrvKey, clsNewAudioCrypto ) ) ||
             ( clsNewLeg.clsVideo.bSrtp &&
               !MediaSdes::BuildCmpKeys( clsNewLeg.clsVideo.strSuite, clsNewLeg.clsVideo.strUeKey,
                                         clsNewLeg.clsVideo.strSrvKey, clsNewVideoCrypto ) ) ) {
            CLog::Print( LOG_INFO, "PickUpLeg: picker(%s) SDES offer not acceptable → 488", pszFrom );
            return SIP_NOT_ACCEPTABLE_HERE;
        }
    }

    // 재키잉 — 신규 단말 leg 가 링잉 착신 leg 의 자리를(relay peer index·포트 포함) 승계한다.
    if ( gclsCallMap.Insert( pszCallId, clsOldCallInfo ) == false ) return SIP_INTERNAL_SERVER_ERROR;
    gclsCallMap.Update( clsOldCallInfo.m_strPeerCallId.c_str(), pszCallId );
    gclsCallMap.DeleteOne( strOldCallId.c_str() );
    gclsUserAgent.StopCall( strOldCallId.c_str() );

    if ( bRelay && pclsRtp ) {
        // 신규 단말을 relay 착신(peer1) index 로 재고정 — 주소·NAT·PT·crypto (RELAY_MODIFY,
        //   cmp_media_api.md §6.2). 대상은 항상 발신(peer0)이 건 링잉 호이므로 승계 index 는 1.
        int iAudioPort = pclsRtp->GetAudioPort();
        if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
        if ( iAudioPort > 0 ) {
            int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;
            int iPickNat = 0;
            std::string strPickGuardIp;
            {
                ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( pszFrom ? pszFrom : "", "volte" );
                std::string strSigIp;
                CUserInfo clsPickUserInfo;
                if ( gclsUserMap.Select( pszFrom, clsPickUserInfo ) ) strSigIp = clsPickUserInfo.m_strIp;
                if ( CCspServiceMap::EvalMediaNat( clsNatSvc, pclsRtp->m_strIp, strSigIp, strPickGuardIp ) )
                    iPickNat = 1;
            }
            int iPickPt = 0, iPickSrcPt = 0, iPickTePt = 0, iPickSrcTePt = 0;
            std::string strPickCodec;
            CGroupCallService::GetLegPt( pszCallId, false, iPickPt, iPickSrcPt, iPickTePt, iPickSrcTePt,
                                         &strPickCodec );
            gclsCmpClient.ModifySession( clsOldCallInfo.m_strRelaySessionId, pclsRtp->m_strIp, iAudioPort,
                                         iVideoPort > 0 ? iVideoPort : 0, 1, clsOldCallInfo.m_strRelayCaller,
                                         pszFrom ? pszFrom : "", clsOldCallInfo.m_strRelaySesId, iPickNat,
                                         strPickGuardIp, iPickPt, iPickSrcPt, iPickTePt, iPickSrcTePt, strPickCodec,
                                         clsNewAudioCrypto.bEnabled ? &clsNewAudioCrypto : NULL,
                                         clsNewVideoCrypto.bEnabled ? &clsNewVideoCrypto : NULL );
        }
        gclsCallMap.SetRelayInfo( pszCallId, clsOldCallInfo.m_strRelaySessionId, clsOldCallInfo.m_strRelaySesId,
                                  clsOldCallInfo.m_strRelayLocalIp, clsOldCallInfo.m_strRelayCaller,
                                  pszFrom ? pszFrom : "" );
        gclsCallMap.SetRelaySdesLeg( pszCallId, 1, clsNewLeg );

        const std::string strRelayIp = clsOldCallInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress()
                                                                                : clsOldCallInfo.m_strRelayLocalIp;
        // 발신자에게 200 answer — 신규 offer 를 발신 leg 상태(offer echo)로 재작성 + peer0 포트
        CSipCallRtp clsAnswerRtp = *pclsRtp;
        _rewriteRelaySdpForLeg( clsAnswerRtp.m_clsMediaList, clsOldCallInfo.m_clsSdesLeg[0], false );
        clsAnswerRtp.SetIpPort( strRelayIp.c_str(), clsOldCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        if ( gclsUserAgent.AcceptCall( clsOldCallInfo.m_strPeerCallId.c_str(), &clsAnswerRtp ) == false ) {
            gclsUserAgent.StopCall( clsOldCallInfo.m_strPeerCallId.c_str() );
            return SIP_INTERNAL_SERVER_ERROR;
        }
        // 신규 단말에게 200 answer — 자기 offer echo(신규 leg 상태) + peer1 포트
        _rewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsNewLeg, false );
        pclsRtp->SetIpPort( strRelayIp.c_str(), clsPeerCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        gclsUserAgent.AcceptCall( pszCallId, pclsRtp );
        gclsCallMap.SetEstablished( pszCallId );
        return 0;
    }

    // 직결(비 relay) — 기존 포트 재사용 동작 보존
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
            if ( pclsRtp ) {
                if ( clsOldCallInfo.m_iPeerRtpPort > 0 )
                    pclsRtp->m_iPort = clsPeerCallInfo.m_iPeerRtpPort;
                else
                    pclsRtp = &clsRemoteRtp;
            }
            gclsUserAgent.AcceptCall( pszCallId, pclsRtp );
            gclsCallMap.SetEstablished( pszCallId );
            return 0;
        }
        gclsUserAgent.StopCall( clsOldCallInfo.m_strPeerCallId.c_str() );
    }
    return SIP_INTERNAL_SERVER_ERROR;
}

// 수신 INVITE 의 Replaces(RFC 3891) 처리 — 관제 BLF 당겨받기·표준 attended 완결.
bool CModuleDispatcher::HandleIncomingReplaces( const char *pszCallId, const char *pszFrom, CSipCallRtp *pclsRtp,
                                                CSipMessage *pclsMessage ) {
    if ( pclsMessage == NULL ) return false;
    CSipHeader *pclsRep = pclsMessage->GetHeader( "Replaces" );
    if ( pclsRep == NULL || pclsRep->m_strValue.empty() ) return false;

    // Replaces: <call-id>;to-tag=<t>;from-tag=<f>  (RFC 3891, 미이스케이프 표준형)
    std::string strVal = pclsRep->m_strValue, strTargetCallId, strToTag, strFromTag;
    {
        size_t p = strVal.find( ';' );
        strTargetCallId = ( p == std::string::npos ) ? strVal : strVal.substr( 0, p );
        // 앞뒤 공백 제거
        while ( !strTargetCallId.empty() && ( strTargetCallId.back() == ' ' ) ) strTargetCallId.pop_back();
        auto param = [&]( const char *pszKey ) -> std::string {
            std::string strKey = pszKey;
            size_t q = strVal.find( strKey );
            if ( q == std::string::npos ) return "";
            q += strKey.size();
            size_t e = strVal.find( ';', q );
            std::string v = strVal.substr( q, e == std::string::npos ? std::string::npos : e - q );
            while ( !v.empty() && ( v.back() == ' ' ) ) v.pop_back();
            return v;
        };
        strToTag = param( "to-tag=" );
        strFromTag = param( "from-tag=" );
    }
    if ( strTargetCallId.empty() ) return false;

    // 대상 다이얼로그 확인 — CallMap 에 존재하고 psip 다이얼로그 태그가 일치해야 한다.
    CCallInfo clsTarget;
    if ( gclsCallMap.Select( strTargetCallId.c_str(), clsTarget ) == false ) {
        CLog::Print( LOG_INFO, "Replaces target %s not found → 481 (from %s)", strTargetCallId.c_str(), pszFrom );
        StopCall( pszCallId, SIP_CALL_TRANSACTION_DOES_NOT_EXIST );
        return true;
    }
    if ( gclsUserAgent.MatchReplacesDialog( strTargetCallId.c_str(), strToTag.c_str(), strFromTag.c_str() ) == false ) {
        CLog::Print( LOG_INFO, "Replaces tag mismatch for %s → 481 (from %s)", strTargetCallId.c_str(), pszFrom );
        StopCall( pszCallId, SIP_CALL_TRANSACTION_DOES_NOT_EXIST );
        return true;
    }

    // 인가 — Replaces 발신자와 대상 호 당사자는 같은 픽업 그룹이어야 한다(§6.2, 무단 가로채기 방지).
    //   대상 호의 당사자 = 대상 leg 의 상대(원 발신자) + 대상 leg 자신(원 착신자) 중 하나.
    {
        std::string strCallee, strCaller;
        gclsUserAgent.GetToId( strTargetCallId.c_str(), strCallee );
        gclsUserAgent.GetFromId( strTargetCallId.c_str(), strCaller );
        CspUser clsPicker, clsCallee;
        std::string strGP, strGC;
        if ( gclsCspUserMap.Select( pszFrom, clsPicker ) ) strGP = clsPicker.EffectivePickupGroup();
        if ( gclsCspUserMap.Select( strCallee.c_str(), clsCallee ) ) strGC = clsCallee.EffectivePickupGroup();
        const bool bSelf = ( pszFrom && ( strCallee == pszFrom || strCaller == pszFrom ) );
        if ( !bSelf && ( strGP.empty() || strGP != strGC ) ) {
            CLog::Print( LOG_INFO, "Replaces denied — %s not same pickup group as %s (%s vs %s) → 403", pszFrom,
                         strCallee.c_str(), strGP.c_str(), strGC.c_str() );
            StopCall( pszCallId, SIP_FORBIDDEN );
            return true;
        }
    }

    CLog::Print( LOG_INFO, "Replaces: %s replaces dialog %s (picker %s)", pszCallId, strTargetCallId.c_str(), pszFrom );
    int iRc = PickUpLeg( pszCallId, pszFrom, pclsRtp, strTargetCallId );
    if ( iRc != 0 ) StopCall( pszCallId, iRc );
    return true;
}
