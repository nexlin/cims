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

#include "CallDir.h"
#include "CallMap.h"
#include "SipMessageLogger.h"

CCallDir gclsCallDir;
#include "CmpClient.h"
#include "CspConfigCache.h"
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
    gclsSipLogger.SetDomainServiceMap( gclsSetup.m_mapDomainToService );
    CLog::SetCallBack( &gclsSipLogger );
    CLog::Print( LOG_SYSTEM, "CspServer is started ( version-%s %s %s )", CSP_SERVER_VERSION, __DATE__, __TIME__ );
    CLog::Print( LOG_DEBUG, "CspServer[%s]", CDirectory::GetProgramDirectory() );
    if ( gclsSetup.m_strCdrFolder.empty() == false ) {
        CDirectory::Create( gclsSetup.m_strCdrFolder.c_str() );
    }
    CSipStackSetup clsSetup;
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
    // VoIP SIP domain for From/To/P-Asserted-Identity (Realm 설정의 volte 도메인)
    clsSetup.m_strDomain = gclsSetup.GetDomainForService("volte");
    if (clsSetup.m_strDomain.empty()) {
        // volte 미설정이면 첫 도메인 사용 (단일 service 배포)
        if (!gclsSetup.m_mapDomainToService.empty())
            clsSetup.m_strDomain = gclsSetup.m_mapDomainToService.begin()->first;
    }
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

    // CSP 런타임 설정 캐시 (로컬 파일 우선 + CSC HTTP pull 비동기 새로고침)
    {
        std::string strCacheDir = gclsSetup.m_strConfigCacheDir;
        if (!strCacheDir.empty() && strCacheDir[0] != '/') {
            // 상대경로 → 프로그램 기동 디렉토리 기준으로 절대화
            strCacheDir = std::string(CDirectory::GetProgramDirectory()) + "/" + strCacheDir;
        }
        gclsCspConfigCache.Init(strCacheDir,
                                 gclsSetup.m_strCscInternalIp,
                                 gclsSetup.m_iCscInternalPort,
                                 gclsSetup.m_strCscInternalToken);
        gclsCspConfigCache.LoadInitial();
        CLog::Print(LOG_SYSTEM,
                    "ConfigCache initialized (csc_reachable=%s cacheDir=%s)",
                    gclsCspConfigCache.IsCscReachable() ? "true" : "false",
                    strCacheDir.c_str());
    }

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
    if ( gclsSetup.m_iMonitorPort > 0 ) {
        gclsMonitor.m_iMonitorPort = gclsSetup.m_iMonitorPort;
        StartMonitorServerThread( &gclsMonitor );
    }
    int iSecond = 0;
    sleep( 1 );
    while ( gbStop == false ) {
        sleep( 1 );
        ++iSecond;
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
