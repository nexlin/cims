/*
 * Copyright (C) 2012 Yee Young Han <websearch@naver.com> (http://blog.naver.com/websearch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "CspServer.h"

#include <csignal>

#include "CallDir.h"
#include "CspAddressing.h"

// SIGUSR1 reload 플래그 (file-scope). 신호 핸들러는 set-only, 실제 reload 는 메인 루프에서.
static volatile sig_atomic_t g_reloadFlag = 0;
static void _cspReloadHandler( int ) {
    g_reloadFlag = 1;
}

#include "CallMap.h"
#include "SipMessageLogger.h"

CCallDir gclsCallDir;
#include "CmpClient.h"
#include "CscInterface.h"
#include "CspAclPolicyEngine.h"
#include "CspConfigCache.h"
#include "CspListenerManager.h"
#include "CspLocalNodeMap.h"
#include "CspPendingRouteMap.h"
#include "CspRemoteNodeMap.h"
#include "CspRouteMap.h"
#include "CspRouteSetMap.h"
#include "CspRoutingPolicyEngine.h"
#include "CspRuleEvaluator.h"
#include "CspServerDefine.h"
#include "CspServerVersion.h"
#include "CspServiceMap.h"
#include "DbManager.h"
#include "Directory.h"
#include "GroupCallService.h"
#include "GroupMap.h"
#include "McDataMediaService.h"
#include "Log.h"
#include "MemoryDebug.h"
#include "ModuleDispatcher.h"
#include "Monitor.h"
#include "NonceMap.h"
#include "RedisStore.h"
#include "ServerService.h"
#include "ServerUtility.h"
#include "SipServer.h"
#include "SipServerSetup.h"
#include "SipUri.h"
#include "SipUserAgentVersion.h"
#include "SubscriptionManager.h"
#include "UserMap.h"

// Forward Declaration for Notify Helpers
void SendSipNotify( const std::string &uri, const std::string &etag, const std::string &action );
void SendInitialNotify( const SubscriptionInfo &sub );
void SendRegEventNotify( const std::string &strUserId, const char *pszEvent, const CUserInfo *pclsInfo );

bool gbFork = true;
/**
 * @returns 정상 종료하면 0 을 리턴하고 오류가 발생하면 -1 를 리턴한다.
 */
int ServiceMain() {
#ifdef WIN32
    _CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_CHECK_ALWAYS_DF );
#endif
    if ( gclsSetup.Read( GetConfigFileName() ) == false && gclsSetup.Read( CONFIG_FILENAME ) == false ) {
        CLog::Print( LOG_ERROR, "config filename(%s) read error", GetConfigFileName() );
        return -1;
    }
    CLog::SetPrefix( "csp" );
    CLog::SetDirectory( gclsSetup.m_strLogFolder.c_str() );
    gclsCallDir.Init( gclsSetup.m_strServiceLogDir, "csp" );
    std::string sysId = gclsSetup.m_strSystemId.empty() ? "csp_01" : gclsSetup.m_strSystemId;
    gclsSipLogger.Init( gclsSetup.m_strServiceLogDir, gclsSetup.m_strMsgLogDir, sysId );
    // v3 (2026-04-22): domain→kind 매핑은 AccessServiceMap 이 SOT.
    //   Sync 는 아래 clsSetup 설정 블록에서 수행.
    //   초기 SipLogger 는 빈 맵으로 시작 — AccessServiceMap.Sync() 후 재설정됨.
    CLog::SetCallBack( &gclsSipLogger );
    CLog::Print( LOG_SYSTEM, "CspServer is started ( version-%s %s %s )", CSP_SERVER_VERSION, __DATE__, __TIME__ );
    if ( !gclsSetup.m_strOverlayPath.empty() ) {
        CLog::Print( LOG_SYSTEM, "SipServerSetup: overlay %s applied (%d keys)", gclsSetup.m_strOverlayPath.c_str(),
                     gclsSetup.m_iOverlayKeys );
    }
    CLog::Print( LOG_DEBUG, "CspServer[%s]", CDirectory::GetProgramDirectory() );
    // G10+ (2026-04-23): CDR CSV 폴더 생성 제거 — service_log 로 대체됨.
    CSipStackSetup clsSetup;

    // v3 (2026-04-22): VoIP 도메인은 AccessServiceMap 이 SOT. 이 시점엔 Sync 전이므로
    //   ConfigCache 선로드 + LocalNodeMap/AccessServiceMap 선 Sync 후 조회.
    // R1 (2026-04-23): primary local_node 에서 Setup.Sip.LocalIp/UdpPort 를 유도하기 위해
    //   LocalNodeMap 도 clsSetup 복사 전에 Sync 한다. (기존 line 224 호출은 제거)
    {
        std::string strJsonlDir = gclsSetup.m_strConfigJsonlDir;
        if ( !strJsonlDir.empty() && strJsonlDir[0] != '/' ) {
            strJsonlDir = std::string( CDirectory::GetProgramDirectory() ) + "/" + strJsonlDir;
        }
        gclsCspConfigCache.Init( strJsonlDir );
        gclsCspConfigCache.LoadInitial();
        gclsLocalNodeMap.Sync();
        gclsAccessServiceMap_Sync_compat();
        gclsSipLogger.SetDomainServiceMap( gclsServiceMap.BuildDomainToKindMap() );
    }

    // primary local_node → gclsSetup.m_strLocalIp/m_iUdpPort.
    //   T2~T4 이후 SIP 송신 자기 주소(Via/Contact)는 SipDialog hint (route 결정/access_service binding)
    //   가 우선이지만, hint 가 없는 경로 (PTT 그룹 InviteMember, RecvOptions, 기타 fallback) 에서는
    //   여전히 gclsSetup.m_strLocalIp 가 SIP UA identity 의 fallback 자리.
    //   따라서 primary 부재 시 fail-fast 유지 — render.py 가 항상 primary 1개 보장하는 체계 신뢰.
    //   (옛 csp.json Setup.Sip.LocalIp/UdpPort fallback 은 c2c8911 에서 제거됨.)
    {
        LocalNodeInfo primary = gclsLocalNodeMap.GetPrimary();
        if ( !primary.IsValid() ) {
            CLog::Print( LOG_ERROR,
                         "no primary local_node — start aborted. Set is_primary=true on a "
                         "local_nodes.jsonl record (or use UI 'local_nodes' collection)." );
            return -1;
        }
        std::string ip = primary.bind_ip;
        if ( ip.empty() || ip == "0.0.0.0" || ip == "::" ) {
            std::string auto_ip;
            GetLocalIp( auto_ip );
            if ( !auto_ip.empty() ) ip = auto_ip;
        }
        if ( !ip.empty() ) gclsSetup.m_strLocalIp = ip;
        if ( primary.bind_port > 0 ) gclsSetup.m_iUdpPort = primary.bind_port;
        CLog::Print( LOG_SYSTEM, "primary local_node '%s' → LocalIp=%s UdpPort=%d", primary.name.c_str(),
                     gclsSetup.m_strLocalIp.c_str(), gclsSetup.m_iUdpPort );
        if ( !primary.protocol.empty() && primary.protocol != "UDP" ) {
            CLog::Print( LOG_INFO, "primary local_node protocol=%s (not UDP) — using for identity only",
                         primary.protocol.c_str() );
        }
    }

    if ( gclsSetup.m_strLocalIp.empty() ) {
        // N개의 IP주소를 사용하는 호스트에서는 SIP 프로토콜로 사용할 IP주소를 직접 입력해 주세요.
        // Vmware 등을 사용하는 경우 N개의 IP주소가 호스트에 존재합니다.
        GetLocalIp( clsSetup.m_strLocalIp );
        gclsSetup.m_strLocalIp = clsSetup.m_strLocalIp;
    } else {
        clsSetup.m_strLocalIp = gclsSetup.m_strLocalIp;
    }
    // R6 (2026-06-08): 부트스트랩 UDP 리스너를 만들지 않는다.
    //   기존엔 primary local_node 포트를 여기서 stack 부트스트랩 소켓(id=0)으로 바인딩했는데,
    //   그 소켓은 ListenerManager 가 소유하지 않아 SIGUSR1 reload 로 포트 변경/제거가 불가능했다
    //   (= 포트 바꾸려면 프로세스 재기동 필수). 모든 SIP 리스너를 local_nodes.jsonl 로부터
    //   ListenerManager 가 동적 add/remove 관리하도록 일원화하여 무중단 포트 변경을 가능케 한다.
    //   m_iLocalUdpPort==0 && m_iUdpThreadCount==0 → CSipStack::_Start 가 UDP 리스너 생성을 건너뜀
    //   (CSipStackSetup::Check 가 LocalIp 만 있으면 통과). identity(Via/Contact) 송신 fallback 포트는
    //   gclsSetup.m_iUdpPort 로 유지하고 Start 직후 스택 식별값에 보정한다(아래 ListenerManager.Sync 뒤).
    clsSetup.m_iLocalUdpPort = 0;
    clsSetup.m_iUdpThreadCount = 0;

    // G9 (2026-04-23): TCP/TLS primary 도 local_nodes 에서 protocol 별 자동 주입.
    //   UDP primary 와 대칭. 조회 실패 시 _infra Setup.Sip.TcpPort/TlsPort/CertFile 값 유지.
    //   local_nodes 에 동일 TCP/TLS 리스너가 추가되면 ListenerManager 가 "already bound by
    //   bootstrap — skip" 로 중복 회피하므로 기존 보조 listener 설정도 안전하게 동작.
    {
        LocalNodeInfo tcpPrimary = gclsLocalNodeMap.GetPrimaryByProtocol( "TCP" );
        if ( tcpPrimary.IsValid() && tcpPrimary.bind_port > 0 ) {
            gclsSetup.m_iTcpPort = tcpPrimary.bind_port;
            CLog::Print( LOG_SYSTEM, "primary local_node '%s' (TCP) → TcpPort=%d", tcpPrimary.name.c_str(),
                         gclsSetup.m_iTcpPort );
        }
        LocalNodeInfo tlsPrimary = gclsLocalNodeMap.GetPrimaryByProtocol( "TLS" );
        if ( tlsPrimary.IsValid() && tlsPrimary.bind_port > 0 ) {
            gclsSetup.m_iTlsPort = tlsPrimary.bind_port;
            // 인증서 경로는 local_nodes 에 명시되어 있을 때만 override.
            if ( !tlsPrimary.tls_cert_path.empty() ) {
                gclsSetup.m_strCertFile = tlsPrimary.tls_cert_path;
            }
            if ( !tlsPrimary.tls_ca_path.empty() ) {
                gclsSetup.m_strCaCertFile = tlsPrimary.tls_ca_path;
            }
            CLog::Print( LOG_SYSTEM, "primary local_node '%s' (TLS) → TlsPort=%d cert=%s", tlsPrimary.name.c_str(),
                         gclsSetup.m_iTlsPort, gclsSetup.m_strCertFile.c_str() );
        }
    }

    clsSetup.m_iLocalTcpPort = gclsSetup.m_iTcpPort;
    clsSetup.m_iTcpThreadCount = gclsSetup.m_iTcpThreadCount;
    clsSetup.m_iTcpCallBackThreadCount = gclsSetup.m_iTcpCallBackThreadCount;
    clsSetup.m_iLocalTlsPort = gclsSetup.m_iTlsPort;
    clsSetup.m_iTlsAcceptTimeout = gclsSetup.m_iTlsAcceptTimeout;
    clsSetup.m_strCertFile = gclsSetup.m_strCertFile;
    clsSetup.m_strCaCertFile = gclsSetup.m_strCaCertFile;

    clsSetup.m_strUserAgent = "csp_";
    clsSetup.m_strUserAgent.append( CSP_SERVER_VERSION );
    clsSetup.m_strDomain = gclsServiceMap.GetDomainByKind( "volte" );
    clsSetup.m_iStackExecutePeriod = gclsSetup.m_iStackExecutePeriod;
    clsSetup.m_iTimerD = gclsSetup.m_iTimerD;
    clsSetup.m_iTimerJ = gclsSetup.m_iTimerJ;
    clsSetup.m_bIpv6 = gclsSetup.m_bIpv6;
    clsSetup.m_bUseRegisterSession = gclsSetup.m_bUseRegisterSession;
    Fork( gbFork );
    SetCoreDumpEnable();
    ServerSignal();
    // G10 (2026-04-23): SipServerMap (legacy IBCF XML) 제거. routes/remote_nodes 체계가 SOT.

    // CSP 런타임 설정 캐시 — jsonl 전용 (Phase C 이후).
    //   agent 가 관리하는 install_path/config/*.jsonl 을 SIGUSR1 수신 시마다 재로드.
    //   v3: 이미 위 clsSetup.m_strDomain 설정 블록에서 Init+LoadInitial 완료. 여기선 로그만.
    CLog::Print( LOG_SYSTEM, "ConfigCache initialized (jsonlDir=%s)",
                 gclsSetup.m_strConfigJsonlDir.empty() ? "(none)" : gclsSetup.m_strConfigJsonlDir.c_str() );

    // [FIX] Init CMP Client before loading groups (which triggers AddGroup)
    if ( !gclsCmpClient.Init( gclsSetup.m_strCmpIp, gclsSetup.m_iCmpPort, gclsSetup.m_iLocalCmpPort ) ) {
        CLog::Print( LOG_ERROR, "CmpClient Init failed" );
        CLog::Print( LOG_ERROR, "CmpClient Init failed" );
    }
    // audit 수준2 — CSP↔CMP 세션 재조정 설정 주입 (ha_design.md 수준2 / cmp_media_api.md)
    gclsCmpClient.SetAuditConfig( gclsSetup.m_bAuditEnable, gclsSetup.m_iAuditGraceSec,
                                  gclsSetup.m_iAuditMaxPerCycle, gclsSetup.m_bAuditZombieTeardown,
                                  gclsSetup.m_strHaRole );

    // Phase 1.D-2 — Redis register state replication (optional, cold-mode if not configured)
    if ( !gclsSetup.m_strRedisHost.empty() && gclsSetup.m_iRedisPort > 0 ) {
        gclsRedisStore.Init( gclsSetup.m_strRedisHost, gclsSetup.m_iRedisPort, gclsSetup.m_strRedisPassword );
    }

    // [FIX] Wire Connection Callback and Start Monitor
    gclsCmpClient.SetConnectionCallback(
        []( bool bConnected ) { gclsGroupCallService.OnCmpStatusChanged( bConnected ); } );
    gclsGroupCallService.StartMonitor();

    // MCData media plane(cmdp, MSRP) — Setup.McDataMedia.Enable 시에만 기동
    gclsMcDataMediaService.Init();

    // DB 연결 (DbHost 가 설정된 경우 항상 연결)
    bool bNeedDb = !gclsSetup.m_strDbHost.empty();
    if ( bNeedDb ) {
        CLog::Print( LOG_SYSTEM, "Connecting to DB %s:%d/%s ...", gclsSetup.m_strDbHost.c_str(), gclsSetup.m_iDbPort,
                     gclsSetup.m_strDbName.c_str() );
        if ( !gclsDbManager.Connect( gclsSetup.m_strDbHost, gclsSetup.m_strDbUser, gclsSetup.m_strDbPasswd,
                                     gclsSetup.m_strDbName, gclsSetup.m_iDbPort ) ) {
            CLog::Print( LOG_ERROR, "DB Connect failed — check csp.json Database section" );
        }
    }

    // Load groups: DB primary, file fallback
    if ( gclsDbManager.IsConnected() ) {
        CLog::Print( LOG_SYSTEM, "Loading GroupMap from DB (primary)..." );
        gclsGroupMap.LoadFromDb();
    } else if ( gclsSetup.m_strGroupDataFolder.length() > 0 ) {
        CLog::Print( LOG_SYSTEM, "Loading GroupMap from files (DB unavailable): %s",
                     gclsSetup.m_strGroupDataFolder.c_str() );
        gclsGroupMap.Load( gclsSetup.m_strGroupDataFolder.c_str() );
    }

    // Load users: DB primary, file fallback
    if ( gclsDbManager.IsConnected() ) {
        CLog::Print( LOG_SYSTEM, "Loading CspUserMap from DB (primary)..." );
        gclsCspUserMap.LoadFromDb();
    } else if ( gclsSetup.m_strUserDataFolder.length() > 0 ) {
        CLog::Print( LOG_SYSTEM, "Loading CspUserMap from files (DB unavailable): %s",
                     gclsSetup.m_strUserDataFolder.c_str() );
        gclsCspUserMap.Load( gclsSetup.m_strUserDataFolder.c_str() );
    }

    {
        USER_ID_LIST clsRegList;
        gclsUserMap.GetRegisteredUsers( clsRegList );
        std::string strRegUsers;
        for ( auto const &strId : clsRegList ) {
            if ( !strRegUsers.empty() ) strRegUsers += ", ";
            strRegUsers += strId;
        }
        CLog::Print( LOG_INFO, "Total Registered Users[%s]", strRegUsers.c_str() );
    }
    CLog::Print( LOG_SYSTEM, "Starting csp..." );
    if ( gclsCscInterface.Start( 4421 ) == false ) {
        CLog::Print( LOG_ERROR, "CscInterface start error (Port 4421)" );
    }
    if ( gclsDispatcher.Start( clsSetup ) == false ) {
        CLog::Print( LOG_ERROR, "SipServer start error" );
        CLog::Print( LOG_ERROR, "SipServer start error (check logs/permissions/ports)" );
        return -1;
    }
    CLog::Print( LOG_SYSTEM, "SipServer started successfully." );

    // SIGUSR1: agent 가 jsonl 쓰기 직후 보내는 reload 시그널.
    //   핸들러에서는 플래그만 세팅, 실제 reload 는 메인 루프에서 수행.
    signal( SIGUSR1, _cspReloadHandler );

    // v3 (2026-04-22): 9-collection 로드 순서 (sip_runtime_config.md §4.3)
    //   1) LocalNode / RemoteNode   (의존성 없음)
    //   2) Route                    (LN, RN 참조)
    //   3) RouteSet                 (Route 참조)
    //   4) Rule                     (의존성 없음)
    //   5) RuleSet                  (Rule 참조) — RuleEvaluator 가 둘 다 로드
    //   6/7) RoutingPolicy / AclPolicy (RuleSet + Route/Access 참조) — 후속 스테이지
    //   8) AccessService            (LocalNode 참조)
    // R1 (2026-04-23): LocalNodeMap 은 기동 초기 (Setup 결정 시) 이미 Sync 됨 → 여기서는 생략.
    gclsRemoteNodeMap.Sync();

    // 미디어(CMP) 풀 활성 (2026-06-01): 전용 MediaServer.Endpoints 에서 추가 CMP endpoint 를
    // CmpClient.AddEndpoint 에 등록. CmpClient::Init 의 primary 와 함께 consistent hash ring
    // 으로 Session-ID 분배 → multi-cmp (AA) relay 분배. (이전엔 SIP remote_nodes 의 tags=["cmp"]
    // 를 재활용했으나, remote_nodes 는 SIP 연동 포인트 전용이라 미디어 평면과 분리함.)
    {
        int added = 0;
        for ( const auto &ep : gclsSetup.m_vecCmpEndpoints ) {
            if ( ep.first.empty() || ep.second <= 0 ) continue;
            // primary(MediaServer.Host) 와 동일 endpoint 면 AddEndpoint 가 internal dedup.
            gclsCmpClient.AddEndpoint( ep.first, ep.second );
            ++added;
        }
        if ( added > 0 ) {
            CLog::Print( LOG_INFO, "CmpClient: registered %d media endpoints from MediaServer.Endpoints", added );
        }
    }

    gclsRouteMap.Sync();
    gclsRouteMap.ValidateRefs();  // LocalNode/RemoteNode 존재 확인
    gclsRouteSetMap.Sync();
    gclsRouteSetMap.ValidateRefs();      // Route 존재 확인
    gclsRuleEvaluator.LoadAll();         // rules + rule_sets
    gclsRoutingPolicyEngine.Sync();      // routing_policies
    gclsAclPolicyEngine.Sync();          // acl_policies
    gclsAccessServiceMap_Sync_compat();  // (임시) 기존 gclsServiceMap.Sync() 호출

    // psip 실제 UDP 리스너 bind (동일 local_nodes.jsonl 을 다른 용도로 소비)
    //   R6 (2026-06-08): 부트스트랩 UDP 바인딩을 제거했으므로 이 Sync 가 primary 포함 모든
    //   SIP 리스너를 올린다. 이후 SIGUSR1 reload 시 Sync 가 포트 변경분을 remove+add 로 재바인딩.
    gclsListenerManager.Sync();
    // identity(Via/Contact) 송신 fallback 포트를 primary 포트로 보정 (스택 m_clsSetup 은 복사본이라
    //   bind 와 무관하게 식별값만 갱신). + UDP 리스너 미바인딩 시 fail-fast.
    {
        LocalNodeInfo pri = gclsLocalNodeMap.GetPrimary();
        if ( pri.IsValid() && pri.bind_port > 0 ) {
            gclsSetup.m_iUdpPort = pri.bind_port;
            gclsUserAgent.m_clsSipStack.m_clsSetup.m_iLocalUdpPort = pri.bind_port;
        }
        std::vector<CSipStackUdpListener *> vUdp;
        gclsUserAgent.m_clsSipStack.GetUdpListenerInfo( vUdp );
        if ( vUdp.empty() ) {
            CLog::Print( LOG_ERROR, "no UDP SIP listener bound after ListenerManager.Sync() — "
                                    "check local_nodes.jsonl primary record / port availability. aborting." );
            return -1;
        }
        CLog::Print( LOG_SYSTEM, "ListenerManager: %zu UDP SIP listener(s) active (identity port=%d)",
                     vUdp.size(), gclsSetup.m_iUdpPort );
    }
    if ( gclsSetup.m_iMonitorPort > 0 ) {
        gclsMonitor.m_iMonitorPort = gclsSetup.m_iMonitorPort;
        StartMonitorServerThread( &gclsMonitor );
    }
    int iSecond = 0;
    sleep( 1 );
    while ( gbStop == false ) {
        sleep( 1 );
        ++iSecond;

        // SIGUSR1 수신 → scalar csp.json + jsonl 재로드 + 관리자 Sync()
        if ( g_reloadFlag ) {
            g_reloadFlag = 0;
            CLog::Print( LOG_SYSTEM, "SIGUSR1: reloading scalar config + jsonl (v3 9-collection)" );
            // scalar csp.json 재파싱 → gclsSetup 의 단순 값(CallPickupId/Timeout 류) 즉시 반영.
            //   bootstrap 성 필드(UdpThreadCount, DB 연결 등)는 재기동이 필요 — 여기서 반영해도 기존 객체엔 미적용.
            gclsSetup.Read();
            gclsCspConfigCache.ReloadFromJsonl();
            gclsLocalNodeMap.Sync();
            gclsRemoteNodeMap.Sync();
            gclsRouteMap.Sync();
            gclsRouteMap.ValidateRefs();
            gclsRouteSetMap.Sync();
            gclsRouteSetMap.ValidateRefs();
            gclsRuleEvaluator.LoadAll();
            gclsRoutingPolicyEngine.Sync();
            gclsAclPolicyEngine.Sync();
            gclsAccessServiceMap_Sync_compat();
            gclsListenerManager.Sync();
            // R6 (2026-06-08): 무중단 포트 변경 — primary 포트가 바뀌었으면 identity fallback 도 추종.
            {
                LocalNodeInfo pri = gclsLocalNodeMap.GetPrimary();
                if ( pri.IsValid() && pri.bind_port > 0 ) {
                    gclsSetup.m_iUdpPort = pri.bind_port;
                    gclsUserAgent.m_clsSipStack.m_clsSetup.m_iLocalUdpPort = pri.bind_port;
                }
            }
        }

        if ( iSecond % 10 == 0 ) {
            gclsNonceMap.DeleteTimeout( 1000 );

            // 등록 만료 사용자 삭제 → DB logout_time 동기화 + PTT 세션 정리
            USER_INFO_LIST clsExpiredUsers;
            gclsUserMap.DeleteTimeout( 1000, clsExpiredUsers );
            for ( const auto &clsExpired : clsExpiredUsers ) {
                const std::string &strUserId = clsExpired.first;
                CLog::Print( LOG_INFO, "Registration expired: user(%s) — syncing DB and cleaning resources",
                             strUserId.c_str() );
                gclsCspUserMap.unregisterUser( strUserId );
                gclsGroupCallService.ClearUserCall( strUserId );
                // reg-event 구독자에게 만료 통지 (partial, 삭제 직전 바인딩)
                SendRegEventNotify( strUserId, "expired", &clsExpired.second );
            }

            gclsUserMap.SendOptions();
            gclsSubscriptionManager.CheckExpired();
        }
        // Stale Call 타이머 (30초마다 체크)
        if ( iSecond % 30 == 0 && gclsSetup.m_iStaleCallTimeout > 0 ) {
            gclsCallMap.DeleteTimeout( gclsSetup.m_iStaleCallTimeout );
            gclsTransCallMap.DeleteTimeout( gclsSetup.m_iStaleCallTimeout );
        }
        // PendingRouteMap — RecvRequest 저장분 중 EventIncomingCall 까지 도달 못한 고아 항목 정리 (30s)
        if ( iSecond % 30 == 0 ) {
            size_t nRemoved = gclsPendingRouteMap.CleanupExpired( std::chrono::seconds( 30 ) );
            if ( nRemoved > 0 ) {
                CLog::Print( LOG_INFO, "PendingRouteMap: cleaned %zu expired entries", nRemoved );
            }
        }
        if ( iSecond % 60 == 0 ) {
            // G10 (2026-04-23): SipServerMap periodic reload 제거.
            if ( gclsDbManager.IsConnected() ) {
                gclsGroupMap.LoadFromDb();
            } else if ( gclsSetup.m_strGroupDataFolder.length() > 0 ) {
                gclsGroupMap.Load( gclsSetup.m_strGroupDataFolder.c_str() );
            }
        }
        if ( iSecond == 3600 ) {
            iSecond = 0;
        }
        if ( gclsSetup.IsChange() ) {
            gclsSetup.Read();
        }
    }
    gclsCallMap.StopCallAll();
    gclsTransCallMap.StopCallAll();
    gclsGroupCallService.StopMonitor();
    gclsCscInterface.Stop();
    for ( int i = 0; i < 20; ++i ) {
        if ( gclsUserAgent.GetCallCount() == 0 ) {
            break;
        }
        sleep( 1 );
    }
    gclsUserAgent.Stop();
    gclsUserAgent.Final();
    CLog::Print( LOG_SYSTEM, "CspServer is terminated" );
    CLog::Release();
    return 0;
}

// Logic to send SIP NOTIFY
#include "CspPttGroup.h"
#include "SipMessage.h"
#include "SipUtility.h"
extern CSipUserAgent gclsUserAgent;

/**
 * @brief Build xcap-diff XML body for NOTIFY
 *   - GMS (group_change): sel points to group document for this subscriber
 *   - CMS (user_change):  sel points to user-profile and service-config documents
 */
static std::string BuildXcapDiffBody( const SubscriptionInfo &sub, const std::string &etag,
                                      const std::string &strChangedId ) {
    // Phase 3: xcap-root = CSC XCAP(MCPTT) 서버 (Setup.Xcap.Scheme://Host:Port, 기본 https:4430).
    //   기존 하드코딩 http://{CSP}:4420 은 CSC Admin 서버(라우트 없음)를 가리키던 오류였음.
    const std::string strXcapRoot = CspAddressing::GetXcapScheme() + "://" + CspAddressing::GetLocalXcapAddress() +
                                    ":" + std::to_string( CspAddressing::GetXcapPort() ) + "/";
    std::string strBody;
    strBody = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    strBody += "<xcap-diff xmlns=\"urn:ietf:params:xml:ns:xcap-diff\" xcap-root=\"";
    strBody += strXcapRoot + "\">\r\n";

    if ( sub.strEventType == "gms" ) {
        // sel = org.openmobilealliance.groups/users/tel:{user}/tel:{group}
        //   strChangedId 비면(특정 그룹 없음) document 생략 — 빈 xcap-diff(구독 active, 변경문서 없음).
        //   초기 동기화 시 사용자 그룹 enumerate 는 SendInitialNotify 가 그룹별로 호출.
        if ( !strChangedId.empty() ) {
            strBody += "  <document new-etag=\"" + etag + "\" sel=\"org.openmobilealliance.groups/users/" +
                       "tel:" + sub.strUserId + "/tel:" + strChangedId + "\"/>\r\n";
        }
    } else {
        // cms: user-profile
        strBody += "  <document new-etag=\"" + etag + "\" sel=\"org.3gpp.mcptt.user-profile/users/" +
                   "tel:" + sub.strUserId + "/user-profile\"/>\r\n";
        // cms: service-config
        strBody += "  <document new-etag=\"" + etag + "\" sel=\"org.3gpp.mcptt.service-config/users/" +
                   "tel:" + sub.strUserId + "/service-config\"/>\r\n";
    }

    strBody += "</xcap-diff>\r\n";
    return strBody;
}

/**
 * @brief SIP 파라미터 값의 %XX escape 해제 — reginfo <unknown-param> 은 디코딩된 값으로 실린다
 *        (예: Contact 의 urn%3Aurn-7%3A... → "urn:urn-7:...")
 */
static std::string UnescapeParamValue( const std::string &strIn ) {
    std::string strOut;
    strOut.reserve( strIn.size() );
    for ( size_t i = 0; i < strIn.size(); ++i ) {
        if ( strIn[i] == '%' && i + 2 < strIn.size() && isxdigit( (unsigned char)strIn[i + 1] ) &&
             isxdigit( (unsigned char)strIn[i + 2] ) ) {
            strOut += (char)strtol( strIn.substr( i + 1, 2 ).c_str(), NULL, 16 );
            i += 2;
        } else {
            strOut += strIn[i];
        }
    }
    return strOut;
}

/**
 * @brief Build reginfo+xml body for reg-event NOTIFY (RFC 3680, 실망 패킷 형태)
 * @param pszEvent NULL/"" = 구독 직후 initial 문서 (state="full", event="registered").
 *        "refreshed"|"created"|"unregistered"|"expired" = 등록 상태 변경 통지 —
 *        state="partial" 로 바뀐 바인딩만 싣는다 (RFC 3680 §5.2).
 */
static std::string BuildRegInfoBody( const SubscriptionInfo &sub, const CUserInfo &clsUserInfo, bool bRegistered,
                                     int iVersion, const char *pszEvent = NULL ) {
    const bool bPartial = ( pszEvent != NULL && pszEvent[0] != '\0' );
    const char *pszContactEvent = bPartial ? pszEvent : "registered";

    time_t tNow = time( NULL );
    time_t tRemaining = 0, tDuration = 0;
    if ( bRegistered ) {
        tRemaining = ( clsUserInfo.m_iLoginTime + clsUserInfo.m_iLoginTimeout ) - tNow;
        if ( tRemaining < 0 ) tRemaining = 0;
    }
    if ( clsUserInfo.m_iLoginTime > 0 ) {
        // 종료 통지에서도 삭제 직전 바인딩 기준 등록 지속시간을 알린다
        tDuration = tNow - clsUserInfo.m_iLoginTime;
        if ( tDuration < 0 ) tDuration = 0;
    }

    std::string strBody = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    strBody += "<reginfo xmlns=\"urn:ietf:params:xml:ns:reginfo\" "
               "xmlns:cp=\"urn:ietf:params:xml:ns:common-policy\" "
               "xmlns:eri=\"urn:3gpp:ns:extRegInfo:1.0\" version=\"" +
               std::to_string( iVersion ) + "\" state=\"" + ( bPartial ? "partial" : "full" ) + "\">\r\n";
    strBody += "<registration aor=\"" + sub.strSubscriberUri + "\" id=\"" + sub.strUserId + "\" state=\"" +
               ( bRegistered ? "active" : "terminated" ) + "\">\r\n";

    // 바인딩이 있으면 <contact> 를 싣는다 — 종료 통지(partial unregistered/expired)도
    //   삭제 직전 바인딩을 expires=0 으로 실어 어떤 바인딩이 사라졌는지 알린다.
    if ( clsUserInfo.m_strContactUri.empty() == false || clsUserInfo.m_strIp.empty() == false ) {
        // <uri> = as-registered Contact (RFC 3680 — 단말이 등록한 URI 그대로),
        //   feature 파라미터는 <unknown-param> 으로 나열 (실망 형태)
        std::string strContactUri = clsUserInfo.m_strContactUri;
        if ( strContactUri.empty() ) {
            strContactUri = "sip:" + sub.strUserId + "@" + clsUserInfo.m_strIp + ":" +
                            std::to_string( clsUserInfo.m_iPort );
        }
        strBody += "<contact id=\"1\" state=\"" + std::string( bRegistered ? "active" : "terminated" ) +
                   "\" event=\"" + pszContactEvent + "\" duration-registered=\"" +
                   std::to_string( (int)tDuration ) + "\" expires=\"" + std::to_string( (int)tRemaining ) +
                   "\" cseq=\"" + std::to_string( clsUserInfo.m_iRegisterCSeq ) + "\">\r\n";
        strBody += "<uri>" + strContactUri + "</uri>\r\n";
        for ( SIP_PARAMETER_LIST::const_iterator itParam = clsUserInfo.m_clsContactParamList.begin();
              itParam != clsUserInfo.m_clsContactParamList.end(); ++itParam ) {
            if ( itParam->m_strValue.empty() ) {
                strBody += "<unknown-param name=\"" + itParam->m_strName + "\"/>\r\n";
            } else {
                strBody += "<unknown-param name=\"" + itParam->m_strName + "\">" +
                           UnescapeParamValue( itParam->m_strValue ) + "</unknown-param>\r\n";
            }
        }
        strBody += "</contact>\r\n";
    }
    strBody += "</registration>\r\n";
    strBody += "</reginfo>\r\n";
    return strBody;
}

/**
 * @brief C2: affiliation-info NOTIFY 본문 (TS 24.379 §9.3/F.4)
 */
static std::string BuildAffiliationInfoBody( const std::string &strUserId ) {
    std::string strDomain = gclsServiceMap.GetDomainByKind( "ptt" );
    std::string strBody = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    strBody += "<mcptt-affiliation-info xmlns=\"urn:3gpp:ns:mcpttAffiliation:1.0\">\r\n";
    bool bDb = gclsDbManager.IsConnected();
    gclsGroupMap.IterateInternal( [&]( const CspPttGroup &clsGroup ) {
        bool bMember = false;
        for ( const auto &pUser : clsGroup._pusers ) {
            if ( pUser && ( pUser->_id == strUserId || pUser->_mcpttId == strUserId ) ) { bMember = true; break; }
        }
        if ( !bMember ) return;
        bool bAff = bDb ? gclsDbManager.IsAffiliated( clsGroup._id, strUserId ) : true;
        if ( !bAff ) return;
        strBody += "  <affiliation group=\"sip:" + clsGroup._id + "@" + strDomain + "\">\r\n";
        strBody += "    <status>affiliated</status>\r\n";
        strBody += "  </affiliation>\r\n";
    } );
    strBody += "</mcptt-affiliation-info>\r\n";
    return strBody;
}

/**
 * @brief Send a proper in-dialog NOTIFY to a single subscriber
 */
/**
 * @param pszRegEvent  reg-event 전용: NULL = initial full 문서,
 *                     "refreshed"|"created"|"unregistered"|"expired" = partial 변경 통지
 * @param pclsRegInfo  reg-event 전용: 등록 삭제 후 통지(unregistered/expired)처럼 UserMap 에서
 *                     더 이상 조회할 수 없는 경우 삭제 직전 바인딩을 넘긴다 (body·전송 목적지에 사용)
 */
static void SendNotifyToSubscriber( const SubscriptionInfo &sub, const std::string &etag,
                                    const std::string &strChangedId, const char *pszRegEvent = NULL,
                                    const CUserInfo *pclsRegInfo = NULL ) {
    // SUBSCRIBE 수신 listener 의 bind_ip:bind_port 를 Via/From 자기 주소로 사용.
    // listener id 가 0 (옛 dialog) 이거나 매칭 실패 시 stack primary 로 fallback.
    const int iListenerId = sub.iInboundListenerId;
    const int iFallbackPort = gclsUserAgent.m_clsSipStack.m_clsSetup.m_iLocalUdpPort;
    const std::string strLocalIp = CspAddressing::GetLocalSipAddress( iListenerId );
    const int iLocalPort = CspAddressing::GetLocalSipPort( iListenerId, iFallbackPort );

    // Get NOTIFY CSeq (increment in manager)
    int iSeq = gclsSubscriptionManager.IncrementNotifySeq( sub.strCallId );

    // Request-URI = subscriber's Contact URI
    std::string strTarget = sub.strContact.empty() ? sub.strSubscriberUri : sub.strContact;

    CSipMessage *pMsg = new CSipMessage();
    pMsg->m_strSipMethod = "NOTIFY";
    pMsg->m_clsReqUri.Parse( strTarget.c_str(), (int)strTarget.size() );

    // Via
    char szBranch[SIP_BRANCH_MAX_SIZE];
    SipMakeBranch( szBranch, sizeof( szBranch ) );
    pMsg->AddVia( strLocalIp.c_str(), iLocalPort, szBranch );

    if ( sub.strEventType == "reg" ) {
        // reg-event: From = 가입자 자신의 AoR (RFC 3680)
        pMsg->m_clsFrom.m_clsUri.Parse( sub.strSubscriberUri.c_str(), (int)sub.strSubscriberUri.size() );
    } else {
        std::string strServerPsi = ( sub.strEventType == "gms" )         ? "gms_psi"
                                 : ( sub.strEventType == "affiliation" ) ? "mcptt_psi"
                                                                         : "cms_psi";
        pMsg->m_clsFrom.m_clsUri.Set( "sip", strServerPsi.c_str(), strLocalIp.c_str(), iLocalPort );
    }
    if ( !sub.strToTag.empty() ) {
        pMsg->m_clsFrom.InsertParam( SIP_TAG, sub.strToTag.c_str() );
    }

    // To: subscriber AoR, tag = sub.strFromTag (the tag from SUBSCRIBE From)
    pMsg->m_clsTo.m_clsUri.Parse( sub.strSubscriberUri.c_str(), (int)sub.strSubscriberUri.size() );
    if ( !sub.strFromTag.empty() ) {
        pMsg->m_clsTo.InsertParam( SIP_TAG, sub.strFromTag.c_str() );
    }

    pMsg->m_clsCallId.Parse( sub.strCallId.c_str(), (int)sub.strCallId.size() );
    pMsg->m_clsCSeq.Set( iSeq, "NOTIFY" );

    // Max-Forwards
    pMsg->m_iMaxForwards = 70;

    // Contact = 서버 자기 주소 (user 없음 — 실망 형태, 예: <sip:scscf11.ims...>)
    {
        CSipFrom clsSelfContact;
        clsSelfContact.m_clsUri.m_strProtocol = "sip";
        clsSelfContact.m_clsUri.m_strHost = strLocalIp;
        clsSelfContact.m_clsUri.m_iPort = iLocalPort;
        pMsg->m_clsContactList.push_back( clsSelfContact );
    }

    // 전송 목적지 = 등록 바인딩(received/rport latch). Route 헤더는 싣지 않는다 —
    //   실망 NOTIFY 에는 Route 가 없으며, NAT 도달은 dest 오버라이드로 처리.
    //   등록 삭제 후 통지(unregistered/expired)는 UserMap 조회가 실패하므로
    //   호출자가 넘긴 삭제 직전 바인딩(pclsRegInfo)을 사용한다.
    CUserInfo clsUserInfo;
    bool bRegistered;
    if ( pclsRegInfo != NULL ) {
        clsUserInfo = *pclsRegInfo;
        bRegistered = false;  // 삭제 직전 바인딩 전달 = 이미 등록 해제된 상태
    } else {
        bRegistered = gclsUserMap.Select( sub.strUserId.c_str(), clsUserInfo );
    }
    if ( clsUserInfo.m_strIp.empty() == false ) {
        pMsg->m_strSendDestIp = clsUserInfo.m_strIp;
        pMsg->m_iSendDestPort = clsUserInfo.m_iPort;
    }

    time_t tRemaining = ( sub.tStartTime + sub.iExpires ) - time( NULL );
    if ( tRemaining < 0 ) tRemaining = 0;
    pMsg->AddHeader( "Subscription-State", ( "active;expires=" + std::to_string( (int)tRemaining ) ).c_str() );

    std::string strBody;
    if ( sub.strEventType == "reg" ) {
        pMsg->AddHeader( "Event", "reg" );
        // reginfo version 은 구독 내 0 부터 시작 (RFC 3680) — 첫 NOTIFY 의 iSeq 가 2 이므로 -2
        strBody = BuildRegInfoBody( sub, clsUserInfo, bRegistered, iSeq - 2, pszRegEvent );
        pMsg->m_clsContentType.Set( "application", "reginfo+xml" );
    } else if ( sub.strEventType == "affiliation" ) {
        pMsg->AddHeader( "Event", "presence" );
        strBody = BuildAffiliationInfoBody( sub.strUserId );
        pMsg->m_clsContentType.Set( "application", "vnd.3gpp.mcptt-affiliation-info+xml" );
    } else {
        pMsg->AddHeader( "Event", "xcap-diff" );
        strBody = BuildXcapDiffBody( sub, etag, strChangedId );
        pMsg->m_clsContentType.Set( "application", "xcap-diff+xml" );
    }
    pMsg->m_strBody = strBody;
    pMsg->m_iContentLength = (int)strBody.size();

    CLog::Print( LOG_INFO, "SendNotifyToSubscriber: User=%s Type=%s Target=%s CSeq=%d", sub.strUserId.c_str(),
                 sub.strEventType.c_str(), strTarget.c_str(), iSeq );

    gclsUserAgent.m_clsSipStack.SendSipMessage( pMsg );
}

/**
 * @brief Send final NOTIFY with Subscription-State: terminated (RFC 3265 §3.1.4)
 *        Called when SUBSCRIBE Expires=0 is received.
 */
void SendTerminatedNotify( const SubscriptionInfo &sub ) {
    const int iListenerId = sub.iInboundListenerId;
    const int iFallbackPort = gclsUserAgent.m_clsSipStack.m_clsSetup.m_iLocalUdpPort;
    const std::string strLocalIp = CspAddressing::GetLocalSipAddress( iListenerId );
    const int iLocalPort = CspAddressing::GetLocalSipPort( iListenerId, iFallbackPort );

    int iSeq = gclsSubscriptionManager.IncrementNotifySeq( sub.strCallId );

    std::string strTarget = sub.strContact.empty() ? sub.strSubscriberUri : sub.strContact;

    CSipMessage *pMsg = new CSipMessage();
    pMsg->m_strSipMethod = "NOTIFY";
    pMsg->m_clsReqUri.Parse( strTarget.c_str(), (int)strTarget.size() );

    char szBranch[SIP_BRANCH_MAX_SIZE];
    SipMakeBranch( szBranch, sizeof( szBranch ) );
    pMsg->AddVia( strLocalIp.c_str(), iLocalPort, szBranch );

    if ( sub.strEventType == "reg" ) {
        pMsg->m_clsFrom.m_clsUri.Parse( sub.strSubscriberUri.c_str(), (int)sub.strSubscriberUri.size() );
    } else {
        std::string strServerPsi = ( sub.strEventType == "gms" ) ? "gms_psi" : "cms_psi";
        pMsg->m_clsFrom.m_clsUri.Set( "sip", strServerPsi.c_str(), strLocalIp.c_str(), iLocalPort );
    }
    if ( !sub.strToTag.empty() ) {
        pMsg->m_clsFrom.InsertParam( SIP_TAG, sub.strToTag.c_str() );
    }

    pMsg->m_clsTo.m_clsUri.Parse( sub.strSubscriberUri.c_str(), (int)sub.strSubscriberUri.size() );
    if ( !sub.strFromTag.empty() ) {
        pMsg->m_clsTo.InsertParam( SIP_TAG, sub.strFromTag.c_str() );
    }

    pMsg->m_clsCallId.Parse( sub.strCallId.c_str(), (int)sub.strCallId.size() );
    pMsg->m_clsCSeq.Set( iSeq, "NOTIFY" );
    pMsg->m_iMaxForwards = 70;

    // Contact = 서버 자기 주소 (user 없음 — SendNotifyToSubscriber 와 동일)
    {
        CSipFrom clsSelfContact;
        clsSelfContact.m_clsUri.m_strProtocol = "sip";
        clsSelfContact.m_clsUri.m_strHost = strLocalIp;
        clsSelfContact.m_clsUri.m_iPort = iLocalPort;
        pMsg->m_clsContactList.push_back( clsSelfContact );
    }

    CUserInfo clsUserInfo;
    if ( gclsUserMap.Select( sub.strUserId.c_str(), clsUserInfo ) ) {
        // Route 헤더 없이 등록 바인딩으로 직접 전송 (SendNotifyToSubscriber 와 동일)
        pMsg->m_strSendDestIp = clsUserInfo.m_strIp;
        pMsg->m_iSendDestPort = clsUserInfo.m_iPort;
    }

    pMsg->AddHeader( "Event", sub.strEventType == "reg"         ? "reg"
                           : sub.strEventType == "affiliation" ? "presence"
                           :                                     "xcap-diff" );
    pMsg->AddHeader( "Subscription-State", "terminated;reason=timeout" );
    pMsg->m_iContentLength = 0;

    CLog::Print( LOG_INFO, "SendTerminatedNotify: User=%s Type=%s Target=%s CSeq=%d",
                 sub.strUserId.c_str(), sub.strEventType.c_str(), strTarget.c_str(), iSeq );

    gclsUserAgent.m_clsSipStack.SendSipMessage( pMsg );
}

/**
 * @brief Send initial NOTIFY immediately after 200 OK to SUBSCRIBE
 *        (Active state, no specific document change)
 */
void SendInitialNotify( const SubscriptionInfo &sub ) {
    if ( sub.strEventType == "reg" ) {
        // reg-event: 등록 상태 reginfo 문서 1건
        SendNotifyToSubscriber( sub, "", "" );
        return;
    }
    if ( sub.strEventType == "affiliation" ) {
        // C2: 제휴상태 초기 NOTIFY (현재 affiliated 그룹 목록).
        SendNotifyToSubscriber( sub, "init", "" );
        return;
    }
    if ( sub.strEventType == "gms" ) {
        // GMS 초기 동기화: 가입자가 속한 그룹별로 group document NOTIFY 발송.
        //   (기존엔 빈 group sel `tel:` 하나만 보내 UE GET 이 404 였음.)
        std::vector<std::string> vecGroupIds;
        gclsGroupMap.IterateInternal( [&]( const CspPttGroup &clsGroup ) {
            for ( const auto &pUser : clsGroup._pusers ) {
                if ( pUser && pUser->_id == sub.strUserId ) {
                    vecGroupIds.push_back( clsGroup._id );
                    break;
                }
            }
        } );
        if ( vecGroupIds.empty() ) {
            SendNotifyToSubscriber( sub, "init", "" );  // 빈 xcap-diff (active, 변경문서 없음)
        } else {
            for ( const auto &strGid : vecGroupIds ) SendNotifyToSubscriber( sub, "init", strGid );
        }
    } else {
        // CMS: user-profile + service-config 문서 (BuildXcapDiffBody cms 분기).
        SendNotifyToSubscriber( sub, "init", "" );
    }
}

/**
 * @brief 등록 상태 변경을 reg-event 구독자에게 통지 (RFC 3680 — state="partial")
 * @param strUserId  등록 상태가 바뀐 가입자
 * @param pszEvent   "refreshed" | "created" | "unregistered" | "expired"
 * @param pclsInfo   등록 삭제 후 통지(unregistered/expired)는 삭제 직전 바인딩을 넘긴다.
 *                   갱신(refreshed/created)은 NULL — UserMap 에서 현재 바인딩 조회.
 */
void SendRegEventNotify( const std::string &strUserId, const char *pszEvent, const CUserInfo *pclsInfo ) {
    std::list<SubscriptionInfo> clsSubList;
    gclsSubscriptionManager.GetSubscriptionsByUser( strUserId, "reg", clsSubList );

    for ( std::list<SubscriptionInfo>::iterator itSub = clsSubList.begin(); itSub != clsSubList.end(); ++itSub ) {
        SendNotifyToSubscriber( *itSub, "", "", pszEvent, pclsInfo );
    }
}

/**
 * @brief Send NOTIFY on group_change or user_change event
 *   - group_change: uri = group ID, notify all GMS subscribers that are group members
 *   - user_change:  uri = user ID, notify that user's CMS subscribers
 */
void SendSipNotify( const std::string &uri, const std::string &etag, const std::string &action ) {
    CLog::Print( LOG_INFO, "SendSipNotify: Uri=%s ETag=%s Action=%s", uri.c_str(), etag.c_str(), action.c_str() );

    // Strip uri prefix
    std::string strId = uri;
    if ( strId.rfind( "tel:", 0 ) == 0 )
        strId = strId.substr( 4 );
    else if ( strId.rfind( "sip:", 0 ) == 0 )
        strId = strId.substr( 4 );

    // Determine if group or user change
    CspPttGroup clsGroup;
    bool bIsGroup = gclsGroupMap.Select( strId.c_str(), clsGroup );

    if ( bIsGroup ) {
        // GMS: find each group member's GMS subscription and notify
        CLog::Print( LOG_INFO, "SendSipNotify: group_change Group=%s Members=%d", strId.c_str(),
                     (int)clsGroup._pusers.size() );
        for ( const auto &pUser : clsGroup._pusers ) {
            if ( !pUser ) continue;
            std::list<SubscriptionInfo> subList;
            gclsSubscriptionManager.GetSubscriptionsByUser( pUser->_id, "gms", subList );
            for ( auto &sub : subList ) {
                SendNotifyToSubscriber( sub, etag, strId );
            }
        }
    } else {
        // CMS: find this user's CMS subscriptions and notify
        CLog::Print( LOG_INFO, "SendSipNotify: user_change User=%s", strId.c_str() );
        std::list<SubscriptionInfo> subList;
        gclsSubscriptionManager.GetSubscriptionsByUser( strId, "cms", subList );
        for ( auto &sub : subList ) {
            SendNotifyToSubscriber( sub, etag, strId );
        }
    }
}

/**
 * @brief C2: 가입자의 affiliation 상태 변경 시 그 가입자의 "affiliation"(presence) 구독자에게
 *   affiliation-info NOTIFY 를 푸시한다. RecvRequestPublish(affiliate/de-affiliate) 에서 호출.
 */
void SendAffiliationNotify( const std::string &strUserId ) {
    std::list<SubscriptionInfo> subList;
    gclsSubscriptionManager.GetSubscriptionsByUser( strUserId, "affiliation", subList );
    CLog::Print( LOG_INFO, "SendAffiliationNotify: User=%s subs=%d", strUserId.c_str(), (int)subList.size() );
    for ( auto &sub : subList ) {
        SendNotifyToSubscriber( sub, "aff", "" );
    }
}

/**
 * @ingroup CspServer
 * @brief C++ SIP stack 을 이용한 한국형 IP-PBX
 * @param argc
 * @param argv
 * @returns 정상 종료하면 0 을 리턴하고 오류가 발생하면 -1 를 리턴한다.
 */
int main( int argc, char *argv[] ) {
    CServerService clsService;
    clsService.m_strName = SERVICE_NAME;
    clsService.m_strDisplayName = SERVICE_DISPLAY_NAME;
    clsService.m_strDescription = SERVICE_DESCRIPTION_STRING;
    clsService.m_strConfigFileName = CONFIG_FILENAME;
    clsService.m_strVersion = CSP_SERVER_VERSION;
    clsService.SetBuildDate( __DATE__, __TIME__ );
    if ( argc == 3 && !strcmp( argv[2], "-n" ) ) {
        gbFork = false;
    }
    ServerMain( argc, argv, clsService, ServiceMain );
    return 0;
}
