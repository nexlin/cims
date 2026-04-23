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

// SIGUSR1 reload 플래그 (file-scope). 신호 핸들러는 set-only, 실제 reload 는 메인 루프에서.
static volatile sig_atomic_t g_reloadFlag = 0;
static void _cspReloadHandler(int) { g_reloadFlag = 1; }

#include "CallMap.h"
#include "SipMessageLogger.h"

CCallDir gclsCallDir;
#include "CmpClient.h"
#include "CspAclPolicyEngine.h"
#include "CspConfigCache.h"
#include "CspListenerManager.h"
#include "CspLocalNodeMap.h"
#include "CspRemoteNodeMap.h"
#include "CspRouteMap.h"
#include "CspRouteSetMap.h"
#include "CspRoutingPolicyEngine.h"
#include "CspRuleEvaluator.h"
#include "CspServiceMap.h"
#include "DbManager.h"
#include "CspServerDefine.h"
#include "CspServerVersion.h"
#include "Directory.h"
#include "Log.h"
#include "MemoryDebug.h"
#include "Monitor.h"
#include "NonceMap.h"
#include "ServerService.h"
#include "ServerUtility.h"
#include "SipServer.h"
#include "SipServerMap.h"
#include "SipServerSetup.h"
#include "SipUserAgentVersion.h"
#include "UserMap.h"
#include "GroupMap.h"
#include "GroupCallService.h"
#include "CscInterface.h"
#include "SubscriptionManager.h"
#include "UserMap.h"
#include "SipUri.h"
#include "ModuleDispatcher.h"

// Forward Declaration for Notify Helpers
void SendSipNotify(const std::string& uri, const std::string& etag, const std::string& action);
void SendInitialNotify(const SubscriptionInfo& sub);

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
        CLog::Print( LOG_SYSTEM, "SipServerSetup: overlay %s applied (%d keys)",
                     gclsSetup.m_strOverlayPath.c_str(), gclsSetup.m_iOverlayKeys );
    }
    CLog::Print( LOG_DEBUG, "CspServer[%s]", CDirectory::GetProgramDirectory() );
    if ( gclsSetup.m_strCdrFolder.empty() == false ) {
        CDirectory::Create( gclsSetup.m_strCdrFolder.c_str() );
    }
    CSipStackSetup clsSetup;

    // v3 (2026-04-22): VoIP 도메인은 AccessServiceMap 이 SOT. 이 시점엔 Sync 전이므로
    //   ConfigCache 선로드 + LocalNodeMap/AccessServiceMap 선 Sync 후 조회.
    // R1 (2026-04-23): primary local_node 에서 Setup.Sip.LocalIp/UdpPort 를 유도하기 위해
    //   LocalNodeMap 도 clsSetup 복사 전에 Sync 한다. (기존 line 224 호출은 제거)
    {
        std::string strJsonlDir = gclsSetup.m_strConfigJsonlDir;
        if (!strJsonlDir.empty() && strJsonlDir[0] != '/') {
            strJsonlDir = std::string(CDirectory::GetProgramDirectory()) + "/" + strJsonlDir;
        }
        gclsCspConfigCache.Init(strJsonlDir);
        gclsCspConfigCache.LoadInitial();
        gclsLocalNodeMap.Sync();
        gclsAccessServiceMap_Sync_compat();
        gclsSipLogger.SetDomainServiceMap(gclsServiceMap.BuildDomainToKindMap());
    }

    // R1: primary local_node → gclsSetup.m_strLocalIp/m_iUdpPort override.
    //   local_nodes 가 CSP identity 의 SOT. bind_ip=0.0.0.0 은 GetLocalIp() 자동탐지로 치환.
    //   primary 가 없으면 _infra 의 Setup.Sip.LocalIp/UdpPort 를 fallback 으로 사용.
    {
        LocalNodeInfo primary = gclsLocalNodeMap.GetPrimary();
        if ( primary.IsValid() ) {
            std::string ip = primary.bind_ip;
            if ( ip.empty() || ip == "0.0.0.0" || ip == "::" ) {
                std::string auto_ip;
                GetLocalIp( auto_ip );
                if ( !auto_ip.empty() ) ip = auto_ip;
            }
            if ( !ip.empty() )            gclsSetup.m_strLocalIp = ip;
            if ( primary.bind_port > 0 )  gclsSetup.m_iUdpPort   = primary.bind_port;
            CLog::Print( LOG_SYSTEM, "primary local_node '%s' → LocalIp=%s UdpPort=%d",
                         primary.name.c_str(), gclsSetup.m_strLocalIp.c_str(), gclsSetup.m_iUdpPort );
            if ( !primary.protocol.empty() && primary.protocol != "UDP" ) {
                CLog::Print( LOG_INFO, "primary local_node protocol=%s (not UDP) — using for identity only",
                             primary.protocol.c_str() );
            }
        } else {
            CLog::Print( LOG_INFO, "no primary local_node — using _infra Setup.Sip.LocalIp=%s UdpPort=%d",
                         gclsSetup.m_strLocalIp.c_str(), gclsSetup.m_iUdpPort );
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
    clsSetup.m_iLocalUdpPort = gclsSetup.m_iUdpPort;
    clsSetup.m_iUdpThreadCount = gclsSetup.m_iUdpThreadCount;
    clsSetup.m_iLocalTcpPort = gclsSetup.m_iTcpPort;
    clsSetup.m_iTcpThreadCount = gclsSetup.m_iTcpThreadCount;
    clsSetup.m_iTcpCallBackThreadCount = gclsSetup.m_iTcpCallBackThreadCount;
    clsSetup.m_iLocalTlsPort = gclsSetup.m_iTlsPort;
    clsSetup.m_iTlsAcceptTimeout = gclsSetup.m_iTlsAcceptTimeout;
    clsSetup.m_strCertFile = gclsSetup.m_strCertFile;
    clsSetup.m_strCaCertFile = gclsSetup.m_strCaCertFile;

    clsSetup.m_strUserAgent = "csp_";
    clsSetup.m_strUserAgent.append( CSP_SERVER_VERSION );
    clsSetup.m_strDomain = gclsServiceMap.GetDomainByKind("volte");
    clsSetup.m_iStackExecutePeriod = gclsSetup.m_iStackExecutePeriod;
    clsSetup.m_iTimerD = gclsSetup.m_iTimerD;
    clsSetup.m_iTimerJ = gclsSetup.m_iTimerJ;
    clsSetup.m_bIpv6 = gclsSetup.m_bIpv6;
    clsSetup.m_bUseRegisterSession = gclsSetup.m_bUseRegisterSession;
    Fork( gbFork );
    SetCoreDumpEnable();
    ServerSignal();
    CLog::Print( LOG_SYSTEM, "Loading SipServerMap..." );
    gclsSipServerMap.Load();

    // CSP 런타임 설정 캐시 — jsonl 전용 (Phase C 이후).
    //   agent 가 관리하는 install_path/config/*.jsonl 을 SIGUSR1 수신 시마다 재로드.
    //   v3: 이미 위 clsSetup.m_strDomain 설정 블록에서 Init+LoadInitial 완료. 여기선 로그만.
    CLog::Print(LOG_SYSTEM, "ConfigCache initialized (jsonlDir=%s)",
                gclsSetup.m_strConfigJsonlDir.empty() ? "(none)" : gclsSetup.m_strConfigJsonlDir.c_str());

    // [FIX] Init CMP Client before loading groups (which triggers AddGroup)
    if ( !gclsCmpClient.Init( gclsSetup.m_strCmpIp, gclsSetup.m_iCmpPort, gclsSetup.m_iLocalCmpPort ) ) {
        CLog::Print( LOG_ERROR, "CmpClient Init failed" );
        CLog::Print( LOG_ERROR, "CmpClient Init failed" );
    }

    // [FIX] Wire Connection Callback and Start Monitor
    gclsCmpClient.SetConnectionCallback([](bool bConnected) {
        gclsGroupCallService.OnCmpStatusChanged(bConnected);
    });
    gclsGroupCallService.StartMonitor();

    // DB 연결 (DbHost 가 설정된 경우 항상 연결)
    bool bNeedDb = !gclsSetup.m_strDbHost.empty();
    if ( bNeedDb ) {
        CLog::Print( LOG_SYSTEM, "Connecting to DB %s:%d/%s ...",
                     gclsSetup.m_strDbHost.c_str(), gclsSetup.m_iDbPort, gclsSetup.m_strDbName.c_str() );
        if ( !gclsDbManager.Connect( gclsSetup.m_strDbHost, gclsSetup.m_strDbUser,
                                     gclsSetup.m_strDbPasswd, gclsSetup.m_strDbName, gclsSetup.m_iDbPort ) ) {
            CLog::Print( LOG_ERROR, "DB Connect failed — check csp.json Database section" );
        }
    }

    // Load groups: DB primary, file fallback
    if ( gclsDbManager.IsConnected() ) {
        CLog::Print( LOG_SYSTEM, "Loading GroupMap from DB (primary)..." );
        gclsGroupMap.LoadFromDb();
    } else if ( gclsSetup.m_strGroupDataFolder.length() > 0 ) {
        CLog::Print( LOG_SYSTEM, "Loading GroupMap from files (DB unavailable): %s", gclsSetup.m_strGroupDataFolder.c_str() );
        gclsGroupMap.Load( gclsSetup.m_strGroupDataFolder.c_str() );
    }

    // Load users: DB primary, file fallback
    if ( gclsDbManager.IsConnected() ) {
        CLog::Print( LOG_SYSTEM, "Loading CspUserMap from DB (primary)..." );
        gclsCspUserMap.LoadFromDb();
    } else if ( gclsSetup.m_strUserDataFolder.length() > 0 ) {
        CLog::Print( LOG_SYSTEM, "Loading CspUserMap from files (DB unavailable): %s", gclsSetup.m_strUserDataFolder.c_str() );
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
    gclsRouteMap.Sync();
    gclsRouteMap.ValidateRefs();        // LocalNode/RemoteNode 존재 확인
    gclsRouteSetMap.Sync();
    gclsRouteSetMap.ValidateRefs();     // Route 존재 확인
    gclsRuleEvaluator.LoadAll();        // rules + rule_sets
    gclsRoutingPolicyEngine.Sync();     // routing_policies
    gclsAclPolicyEngine.Sync();         // acl_policies
    gclsAccessServiceMap_Sync_compat(); // (임시) 기존 gclsServiceMap.Sync() 호출

    // psip 실제 UDP 리스너 bind (동일 local_nodes.jsonl 을 다른 용도로 소비)
    gclsListenerManager.Sync();
    if ( gclsSetup.m_iMonitorPort > 0 ) {
        gclsMonitor.m_iMonitorPort = gclsSetup.m_iMonitorPort;
        StartMonitorServerThread( &gclsMonitor );
    }
    int iSecond = 0;
    sleep( 1 );
    while ( gbStop == false ) {
        sleep( 1 );
        ++iSecond;

        // SIGUSR1 수신 → jsonl 재로드 + 관리자 Sync()
        if ( g_reloadFlag ) {
            g_reloadFlag = 0;
            CLog::Print( LOG_SYSTEM, "SIGUSR1: reloading jsonl config (v3 9-collection)" );
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
        }

        if ( iSecond % 10 == 0 ) {
            gclsNonceMap.DeleteTimeout( 1000 );

            // 등록 만료 사용자 삭제 → DB logout_time 동기화 + PTT 세션 정리
            USER_ID_LIST clsExpiredUsers;
            gclsUserMap.DeleteTimeout( 1000, clsExpiredUsers );
            for ( const auto &strUserId : clsExpiredUsers ) {
                CLog::Print( LOG_INFO, "Registration expired: user(%s) — syncing DB and cleaning resources",
                             strUserId.c_str() );
                gclsCspUserMap.unregisterUser( strUserId );
                gclsGroupCallService.ClearUserCall( strUserId );
            }

            gclsUserMap.SendOptions();
            gclsSubscriptionManager.CheckExpired();
        }
        // Stale Call 타이머 (30초마다 체크)
        if ( iSecond % 30 == 0 && gclsSetup.m_iStaleCallTimeout > 0 ) {
            gclsCallMap.DeleteTimeout( gclsSetup.m_iStaleCallTimeout );
            gclsTransCallMap.DeleteTimeout( gclsSetup.m_iStaleCallTimeout );
        }
        if ( iSecond % 60 == 0 ) {
            gclsSipServerMap.Load();
            gclsSipServerMap.SetSipUserAgentRegisterInfo();
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
#include "SipMessage.h"
#include "SipUtility.h"
#include "CspPttGroup.h"
extern CSipUserAgent gclsUserAgent;

/**
 * @brief Build xcap-diff XML body for NOTIFY
 *   - GMS (group_change): sel points to group document for this subscriber
 *   - CMS (user_change):  sel points to user-profile and service-config documents
 */
static std::string BuildXcapDiffBody(const SubscriptionInfo& sub, const std::string& etag,
                                     const std::string& strChangedId) {
    const std::string strXcapRoot = "http://" + gclsSetup.m_strLocalIp + ":4420/";
    std::string strBody;
    strBody  = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    strBody += "<xcap-diff xmlns=\"urn:ietf:params:xml:ns:xcap-diff\" xcap-root=\"";
    strBody += strXcapRoot + "\">\r\n";

    if (sub.strEventType == "gms") {
        // sel = org.openmobilealliance.groups/users/tel:{user}/tel:{group}
        strBody += "  <document new-etag=\"" + etag + "\" sel=\"org.openmobilealliance.groups/users/"
               +  "tel:" + sub.strUserId + "/tel:" + strChangedId + "\"/>\r\n";
    } else {
        // cms: user-profile
        strBody += "  <document new-etag=\"" + etag + "\" sel=\"org.3gpp.mcptt.user-profile/users/"
               +  "tel:" + sub.strUserId + "/user-profile\"/>\r\n";
        // cms: service-config
        strBody += "  <document new-etag=\"" + etag + "\" sel=\"org.3gpp.mcptt.service-config/users/"
               +  "tel:" + sub.strUserId + "/service-config\"/>\r\n";
    }

    strBody += "</xcap-diff>\r\n";
    return strBody;
}

/**
 * @brief Send a proper in-dialog NOTIFY to a single subscriber
 */
static void SendNotifyToSubscriber(const SubscriptionInfo& sub, const std::string& etag,
                                   const std::string& strChangedId) {
    const std::string& strLocalIp = gclsUserAgent.m_clsSipStack.m_clsSetup.m_strLocalIp;
    int iLocalPort = gclsUserAgent.m_clsSipStack.m_clsSetup.m_iLocalUdpPort;

    // Get NOTIFY CSeq (increment in manager)
    int iSeq = gclsSubscriptionManager.IncrementNotifySeq(sub.strCallId);

    // Request-URI = subscriber's Contact URI
    std::string strTarget = sub.strContact.empty() ? sub.strSubscriberUri : sub.strContact;

    CSipMessage *pMsg = new CSipMessage();
    pMsg->m_strSipMethod = "NOTIFY";
    pMsg->m_clsReqUri.Parse(strTarget.c_str(), (int)strTarget.size());

    // Via
    char szBranch[SIP_BRANCH_MAX_SIZE];
    SipMakeBranch(szBranch, sizeof(szBranch));
    pMsg->AddVia(strLocalIp.c_str(), iLocalPort, szBranch);

    // From: server (our PSI), tag = sub.strToTag (the tag we sent in 200 OK)
    std::string strServerPsi = (sub.strEventType == "gms") ? "gms_psi" : "cms_psi";
    pMsg->m_clsFrom.m_clsUri.Set("sip", strServerPsi.c_str(), strLocalIp.c_str(), iLocalPort);
    if (!sub.strToTag.empty()) {
        pMsg->m_clsFrom.InsertParam(SIP_TAG, sub.strToTag.c_str());
    }

    // To: subscriber AoR, tag = sub.strFromTag (the tag from SUBSCRIBE From)
    pMsg->m_clsTo.m_clsUri.Parse(sub.strSubscriberUri.c_str(), (int)sub.strSubscriberUri.size());
    if (!sub.strFromTag.empty()) {
        pMsg->m_clsTo.InsertParam(SIP_TAG, sub.strFromTag.c_str());
    }

    pMsg->m_clsCallId.Parse(sub.strCallId.c_str(), (int)sub.strCallId.size());
    pMsg->m_clsCSeq.Set(iSeq, "NOTIFY");

    // Max-Forwards
    pMsg->m_iMaxForwards = 70;

    // Route to subscriber's registered address
    CUserInfo clsUserInfo;
    if (gclsUserMap.Select(sub.strUserId.c_str(), clsUserInfo)) {
        CSipCallRoute clsRoute;
        clsUserInfo.GetCallRoute(clsRoute);
        pMsg->AddRoute(clsRoute.m_strDestIp.c_str(), clsRoute.m_iDestPort, E_SIP_UDP);
    }

    pMsg->AddHeader("Event", "xcap-diff");
    time_t tRemaining = (sub.tStartTime + sub.iExpires) - time(NULL);
    if (tRemaining < 0) tRemaining = 0;
    pMsg->AddHeader("Subscription-State", ("active;expires=" + std::to_string((int)tRemaining)).c_str());

    std::string strBody = BuildXcapDiffBody(sub, etag, strChangedId);
    pMsg->m_clsContentType.Set("application", "xcap-diff+xml");
    pMsg->m_strBody = strBody;
    pMsg->m_iContentLength = (int)strBody.size();

    CLog::Print(LOG_INFO, "SendNotifyToSubscriber: User=%s Type=%s Target=%s CSeq=%d",
        sub.strUserId.c_str(), sub.strEventType.c_str(), strTarget.c_str(), iSeq);

    gclsUserAgent.m_clsSipStack.SendSipMessage(pMsg);
}

/**
 * @brief Send initial NOTIFY immediately after 200 OK to SUBSCRIBE
 *        (Active state, no specific document change)
 */
void SendInitialNotify(const SubscriptionInfo& sub) {
    SendNotifyToSubscriber(sub, "init", "");
}

/**
 * @brief Send NOTIFY on group_change or user_change event
 *   - group_change: uri = group ID, notify all GMS subscribers that are group members
 *   - user_change:  uri = user ID, notify that user's CMS subscribers
 */
void SendSipNotify(const std::string& uri, const std::string& etag, const std::string& action) {
    CLog::Print(LOG_INFO, "SendSipNotify: Uri=%s ETag=%s Action=%s", uri.c_str(), etag.c_str(), action.c_str());

    // Strip uri prefix
    std::string strId = uri;
    if (strId.rfind("tel:", 0) == 0) strId = strId.substr(4);
    else if (strId.rfind("sip:", 0) == 0) strId = strId.substr(4);

    // Determine if group or user change
    CspPttGroup clsGroup;
    bool bIsGroup = gclsGroupMap.Select(strId.c_str(), clsGroup);

    if (bIsGroup) {
        // GMS: find each group member's GMS subscription and notify
        CLog::Print(LOG_INFO, "SendSipNotify: group_change Group=%s Members=%d", strId.c_str(), (int)clsGroup._pusers.size());
        for (const auto& pUser : clsGroup._pusers) {
            if (!pUser) continue;
            std::list<SubscriptionInfo> subList;
            gclsSubscriptionManager.GetSubscriptionsByUser(pUser->_id, "gms", subList);
            for (auto& sub : subList) {
                SendNotifyToSubscriber(sub, etag, strId);
            }
        }
    } else {
        // CMS: find this user's CMS subscriptions and notify
        CLog::Print(LOG_INFO, "SendSipNotify: user_change User=%s", strId.c_str());
        std::list<SubscriptionInfo> subList;
        gclsSubscriptionManager.GetSubscriptionsByUser(strId, "cms", subList);
        for (auto& sub : subList) {
            SendNotifyToSubscriber(sub, etag, strId);
        }
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
