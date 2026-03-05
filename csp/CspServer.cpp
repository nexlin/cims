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

#include "CallMap.h"
#include "CmpClient.h"
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

// Forward Declaration for Notify Helper
void SendSipNotify(const std::string& uri, const std::string& etag, const std::string& action);

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
    CLog::SetDirectory( gclsSetup.m_strLogFolder.c_str() );
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

    if ( gclsSetup.m_strGroupDataFolder.length() > 0 ) {
        CLog::Print( LOG_SYSTEM, "Loading GroupMap from %s...", gclsSetup.m_strGroupDataFolder.c_str() );
        gclsGroupMap.Load( gclsSetup.m_strGroupDataFolder.c_str() );
    }
    if ( gclsSetup.m_strUserDataFolder.length() > 0 ) {
        CLog::Print( LOG_SYSTEM, "Loading CspUserMap from %s...", gclsSetup.m_strUserDataFolder.c_str() );
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
    if ( gclsSipServer.Start( clsSetup ) == false ) {
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
            gclsUserMap.DeleteTimeout( 1000 );
            gclsUserMap.SendOptions();
            //gclsCspUserMap.DeleteTimeout( 1000 );
        }
        if ( iSecond % 60 == 0 ) {
            gclsSipServerMap.Load();
            gclsSipServerMap.SetSipUserAgentRegisterInfo();
            if ( gclsSetup.m_strGroupDataFolder.length() > 0 ) {
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
extern CSipUserAgent gclsUserAgent;

void SendSipNotify(const std::string& uri, const std::string& etag, const std::string& action) {
    CLog::Print(LOG_INFO, "SendSipNotify: Processing Event Uri=%s ETag=%s Action=%s", uri.c_str(), etag.c_str(), action.c_str());

    std::list<SubscriptionInfo> subList;
    
    // [USER REQ] Skip subscription requirement, target registered group members
    
    gclsSubscriptionManager.GetSubscribers(uri, subList);

    if (subList.empty()) {
        CLog::Print(LOG_INFO, "SendSipNotify: No subscribers for %s", uri.c_str());
        return;
    }
    



    for (const auto& sub : subList) {
        CLog::Print(LOG_INFO, "SendSipNotify: Sending to %s", sub.strSubscriberUri.c_str());
        
        CSipMessage *pMsg = new CSipMessage();
        pMsg->m_strSipMethod = "NOTIFY";
        pMsg->m_clsReqUri.Parse( sub.strSubscriberUri.c_str(), sub.strSubscriberUri.length() );
        pMsg->m_strSipVersion = "SIP/2.0";

        // Set Headers (Standardize using structures)
        pMsg->m_clsFrom.m_clsUri.Set("sip", "csc", "mcptt.com");
        pMsg->m_clsFrom.InsertParam("tag", "serverTag");

        pMsg->m_clsTo.m_clsUri.Parse( sub.strSubscriberUri.c_str(), sub.strSubscriberUri.length() );
        if (!sub.strFromTag.empty()) {
             pMsg->m_clsTo.InsertParam("tag", sub.strFromTag.c_str());
        }
        
        pMsg->m_clsCallId.Parse( sub.strCallId.c_str(), sub.strCallId.length() );
        pMsg->m_clsCSeq.Set( 1, "NOTIFY" );

        pMsg->AddHeader("Event", "xcap-diff");
        pMsg->AddHeader("Subscription-State", ( "active;expires=" + std::to_string(sub.iExpires) ).c_str());
        pMsg->AddHeader("Content-Type", "application/xcap-diff+xml");

        // Body
        std::string strBody = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        strBody += "<xcap-diff xmlns=\"urn:ietf:params:xml:ns:xcap-diff\" xcap-root=\"http://csc.mcptt.com/\">\n";
        strBody += " <document-new-etag>" + etag + "</document-new-etag>\n";
        strBody += "</xcap-diff>";

        pMsg->m_strBody = strBody;
        pMsg->m_iContentLength = strBody.length();

        // Ensure raw packet is generated so stack can log it correctly
        if (pMsg->MakePacket() == false) {
            CLog::Print(LOG_ERROR, "SendSipNotify: MakePacket failed for %s", sub.strSubscriberUri.c_str());
            delete pMsg;
            continue;
        }

        CLog::Print(LOG_INFO, "SendSipNotify: Sending NOTIFY to %s", sub.strSubscriberUri.c_str());
        
        // Send (Stack will log UdpSend now because m_strPacket is filled)
        gclsUserAgent.m_clsSipStack.SendSipMessage(pMsg);
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
