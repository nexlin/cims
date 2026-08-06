#include "CscInterface.h"

#include <sstream>

#include "CallDir.h"
#include "CallMap.h"
#include "CspConfigCache.h"
#include "CspListenerManager.h"
#include "CspLocalNodeMap.h"
#include "CspRouteMap.h"
#include "CspServiceMap.h"
#include "CspUser.h"
#include "DbManager.h"
#include "GroupCallService.h"
#include "Log.h"
#include "ModuleDispatcher.h"
#include "SipMessageLogger.h"
#include "SipServer.h"
#include "SipServerSetup.h"
#include "UserMap.h"

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <iostream>
#include <vector>

CCscInterface gclsCscInterface;

CCscInterface::CCscInterface() : m_iPort( 0 ), m_iServerSock( -1 ), m_bRunning( false ) {
}

CCscInterface::~CCscInterface() {
    Stop();
}

bool CCscInterface::Start( int iPort ) {
    m_iPort = iPort;
    m_bRunning = true;
    m_threadListener = std::thread( &CCscInterface::ListenerLoop, this );
    CLog::Print( LOG_INFO, "CscInterface Started on Port %d", m_iPort );
    return true;
}

void CCscInterface::Stop() {
    m_bRunning = false;
    if ( m_iServerSock != -1 ) {
#ifdef WIN32
        closesocket( m_iServerSock );
#else
        close( m_iServerSock );
#endif
        m_iServerSock = -1;
    }
    if ( m_threadListener.joinable() ) {
        m_threadListener.join();
    }
}

void CCscInterface::ListenerLoop() {
    struct sockaddr_in serverAddr;

#ifdef WIN32
    WSADATA wsaData;
    WSAStartup( MAKEWORD( 2, 2 ), &wsaData );
#endif

    // UDP Socket
    m_iServerSock = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( m_iServerSock == -1 ) {
        CLog::Print( LOG_ERROR, "CscInterface: Socket creation failed" );
        return;
    }

    // Reuse Address
    int opt = 1;
    setsockopt( m_iServerSock, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof( opt ) );

    serverAddr.sin_family = AF_INET;
    // bind IP 는 primary local_node 의 bind_ip "원문" 기준. gclsSetup.m_strLocalIp 는
    // 기동 시 0.0.0.0 이 실IP 로 치환된 값 (CspServer R1 주입) 이라 그걸로 판단하면
    // HA 운용 (bind_ip=0.0.0.0 + keepalived VIP) 에서 실IP 에만 bind 되어 VIP:4421
    // 수신이 안 된다 (oam-svc STATS probe 오탐).
    //  - 빈값/0.0.0.0/:: → INADDR_ANY: 실IP·VIP 모든 destination 수신 (SIP 리스너와 동일 규칙)
    //  - 명시 IP (예: PSP=127.0.0.3) → 그 IP: 같은 host 의 여러 csp 인스턴스 (CSP/PSP/ISP)
    //    가 공유하는 4421 을 destination IP 매칭으로 정확히 라우팅
    // primary 미존재 시 (CspServer 가 기동 시 fail-fast 라 정상 경로에선 도달 불가)
    // 기존 동작대로 gclsSetup.m_strLocalIp 를 사용.
    std::string strBindIp = gclsSetup.m_strLocalIp;
    LocalNodeInfo clsPrimary = gclsLocalNodeMap.GetPrimary();
    if ( clsPrimary.IsValid() ) strBindIp = clsPrimary.bind_ip;

    if ( !strBindIp.empty() && strBindIp != "0.0.0.0" && strBindIp != "::" ) {
        serverAddr.sin_addr.s_addr = inet_addr( strBindIp.c_str() );
        if ( serverAddr.sin_addr.s_addr == INADDR_NONE ) {
            CLog::Print( LOG_ERROR, "CscInterface: invalid bind ip [%s] — fallback INADDR_ANY", strBindIp.c_str() );
            serverAddr.sin_addr.s_addr = INADDR_ANY;
        }
    } else {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    }
    serverAddr.sin_port = htons( m_iPort );

    if ( bind( m_iServerSock, (struct sockaddr *)&serverAddr, sizeof( serverAddr ) ) < 0 ) {
        CLog::Print( LOG_ERROR, "CscInterface: Bind failed port %d", m_iPort );
        return;
    }

    // No Listen for UDP

    CLog::Print( LOG_INFO, "CscInterface: UDP Listener Started (bind=%s:%d)", inet_ntoa( serverAddr.sin_addr ),
                 m_iPort );

    char buffer[4096];
    struct sockaddr_in clientAddr;
#ifdef WIN32
    int clientLen = sizeof( clientAddr );
#else
    socklen_t clientLen = sizeof( clientAddr );
#endif

    while ( m_bRunning ) {
        int bytesRead =
            recvfrom( m_iServerSock, buffer, sizeof( buffer ) - 1, 0, (struct sockaddr *)&clientAddr, &clientLen );

        if ( bytesRead > 0 ) {
            buffer[bytesRead] = '\0';
            std::string strMsg( buffer );
            ProcessMessage( strMsg, clientAddr );
        } else if ( bytesRead < 0 ) {
            // Error or Timeout
            // CLog::Print(LOG_ERROR, "CscInterface: Recvfrom failed");
            // Break if socket closed?
            if ( !m_bRunning ) break;
        }
    }
}

// Simple JSON Parser
// Expected: {"event": "group_change", "uri": "tel:+...", "action": "PUT", "etag": "..."}
void CCscInterface::ProcessMessage( const std::string &strMsg, const struct sockaddr_in &clientAddr ) {
    // Helper lambda to get value by key
    auto getVal = [&]( const std::string &key ) -> std::string {
        std::string searchKey = "\"" + key + "\"";
        size_t pos = strMsg.find( searchKey );
        if ( pos == std::string::npos ) return "";

        pos = strMsg.find( ":", pos );
        if ( pos == std::string::npos ) return "";

        size_t startQuote = strMsg.find( "\"", pos );
        if ( startQuote == std::string::npos ) return "";

        size_t endQuote = strMsg.find( "\"", startQuote + 1 );
        if ( endQuote == std::string::npos ) return "";

        return strMsg.substr( startQuote + 1, endQuote - startQuote - 1 );
    };

    std::string strEvent = getVal( "event" );
    std::string strUri = getVal( "uri" );
    std::string strAction = getVal( "action" );
    std::string strEtag = getVal( "etag" );
    std::string strTransId = getVal( "trans_id" );
    std::string strSesId = getVal( "sesid" );
    std::string strService = getVal( "service" );
    // CSC가 sesid/service를 안 보낸 경우 보수적 기본값 적용
    if ( strSesId.empty() ) {
        strSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    }
    if ( strService.empty() ) strService = "system";

    // caller 파생: uri 에서 추출 (tel:+82... 또는 sip:user@domain)
    std::string strCaller;
    if ( !strUri.empty() ) {
        if ( strUri.compare( 0, 4, "tel:" ) == 0 )
            strCaller = strUri.substr( 4 );
        else if ( strUri.compare( 0, 4, "sip:" ) == 0 ) {
            std::string tail = strUri.substr( 4 );
            size_t at = tail.find( '@' );
            strCaller = ( at != std::string::npos ) ? tail.substr( 0, at ) : tail;
        }
    }

    CLog::Print( LOG_INFO, "CscInterface Event: %s, URI: %s, Action: %s, TransId: %s, SesId: %s, Service: %s",
                 strEvent.c_str(), strUri.c_str(), strAction.c_str(), strTransId.c_str(), strSesId.c_str(),
                 strService.c_str() );

    // CSC admin 메시지를 SIP 로그에 기록 (sesid/service/caller 포함)
    {
        char peerBuf[64];
        snprintf( peerBuf, sizeof( peerBuf ), "%s:%d", inet_ntoa( clientAddr.sin_addr ), ntohs( clientAddr.sin_port ) );
        std::string strLabel = strEvent + "(" + strAction + ")";
        gclsSipLogger.LogMessage( "csc", "csp", "CSC", strLabel.c_str(), peerBuf, strMsg.c_str(), strService.c_str(),
                                  strTransId.c_str(), strSesId.c_str(), "", strCaller.c_str(), "" );
    }

    if ( strEvent == "GROUP_CHANGED" ) {
        extern void SendSipNotify( const std::string &uri, const std::string &etag, const std::string &action );
        SendSipNotify( strUri, strEtag, strAction );
        // Log config_change event to active PTT session history
        {
            // Extract group ID from URI (strip "tel:" prefix if present)
            std::string strGroupId = strUri;
            if ( strGroupId.substr( 0, 4 ) == "tel:" ) strGroupId = strGroupId.substr( 4 );
            if ( gclsCallDir.IsEnabled() ) {
                gclsCallDir.PttLogEvent( strGroupId, "config_change", "{\"action\":\"" + strAction + "\"}" );
            }
        }
        // Reload group config and re-sync CMP sessions / re-invite members
        gclsGroupCallService.OnGroupConfigChanged();
    } else if ( strEvent == "STATS_REQUEST" ) {
        // stats 요청 → 현재 CSP 상태를 JSON으로 응답
        USER_ID_LIST regList;
        gclsUserMap.GetRegisteredUsers( regList );
        int regUsers = (int)regList.size();
        // active_calls: DB 기반 정확한 수 (B2BUA + Proxy 모두 포함)
        int activeCalls = 0;
        bool dbConnected = gclsDbManager.IsConnected();
        if ( dbConnected ) {
            activeCalls = gclsDbManager.GetActiveVoipCallCount();
        } else {
            activeCalls = gclsCallMap.GetCount();
        }

        std::ostringstream oss;
        oss << "{\"status\":\"OK\"" << ",\"registered_users\":" << regUsers << ",\"active_calls\":" << activeCalls
            << ",\"db_connected\":" << ( dbConnected ? "true" : "false" )
            << ",\"roles\":{\"CSCF\":" << ( gclsSetup.m_bRoleCscf ? "true" : "false" )
            << ",\"TAS\":" << ( gclsSetup.m_bRoleTas ? "true" : "false" )
            << ",\"PTT_AS\":" << ( gclsSetup.m_bRolePttAs ? "true" : "false" )
            << ",\"IBCF\":" << ( gclsSetup.m_bRoleIbcf ? "true" : "false" ) << "}"
            << ",\"timeouts\":{\"user_timeout\":" << gclsSetup.m_iUserTimeout
            << ",\"stale_call_timeout\":" << gclsSetup.m_iStaleCallTimeout
            << ",\"send_options_period\":" << gclsSetup.m_iSendOptionsPeriod << "}"
            << ",\"record_enable\":" << ( gclsSetup.m_bRecordEnable ? "true" : "false" );

        // v3 (2026-04-22): 트렁크 상태 — RouteMap/RouteSetMap 기반으로 재표현.
        //   Route 의 런타임 alive/RTT 를 리스트로 출력.
        {
            auto routes = gclsRouteMap.GetAll();
            oss << ",\"routes\":[";
            for ( size_t i = 0; i < routes.size(); ++i ) {
                const auto &r = routes[i];
                if ( i ) oss << ",";
                bool alive = gclsRouteMap.IsAlive( r.name );
                oss << "{\"name\":\"" << r.name << "\"" << ",\"local_node_ref\":\"" << r.local_node_ref << "\""
                    << ",\"remote_node_ref\":\"" << r.remote_node_ref << "\""
                    << ",\"enabled\":" << ( r.enabled ? "true" : "false" )
                    << ",\"alive\":" << ( alive ? "true" : "false" ) << "}";
            }
            oss << "]";
        }
        oss << "}";

        std::string resp = oss.str();

        // TX 로그: CSC에 응답 전송 기록 (요청의 sesid/service 계승)
        {
            char peerBuf[64];
            snprintf( peerBuf, sizeof( peerBuf ), "%s:%d", inet_ntoa( clientAddr.sin_addr ),
                      ntohs( clientAddr.sin_port ) );
            gclsSipLogger.LogMessage( "csp", "csc", "CSC", "STATS_RESPONSE", peerBuf, resp.c_str(), strService.c_str(),
                                      strTransId.c_str(), strSesId.c_str() );
        }

        sendto( m_iServerSock, resp.c_str(), resp.size(), 0, (const struct sockaddr *)&clientAddr,
                sizeof( clientAddr ) );

        CLog::Print( LOG_INFO, "CscInterface: Stats response sent (reg=%d calls=%d)", regUsers, activeCalls );
    } else if ( strEvent == "CSC_RESTART" ) {
        CLog::Print( LOG_INFO, "CscInterface: CSC_RESTART received — resyncing group/user state from DB" );

        // Resync user map from DB
        gclsCspUserMap.LoadFromDb();

        // Trigger full group resync (SyncGroupsState)
        gclsGroupCallService.OnGroupConfigChanged();

        // 동적 설정(listener/trunk/route/acl/service) 는 jsonl + SIGUSR1 경로로 반영됨.
        // CSC_RESTART 는 이제 사용자/그룹 상태만 담당.
    } else if ( strEvent == "LISTENER_CHANGED" || strEvent == "TRUNK_CHANGED" || strEvent == "ROUTE_RULE_CHANGED" ||
                strEvent == "ACCESS_LIST_CHANGED" || strEvent == "SERVICE_CHANGED" ) {
        // Phase C 이후: 동적 설정은 agent → jsonl → SIGUSR1 경로로만 반영.
        // 이 이벤트는 Phase B 이전의 HTTP pull 경로용으로 더 이상 수신하지 않음.
        CLog::Print( LOG_DEBUG, "CscInterface: ignoring deprecated event %s (use SIGUSR1 path)", strEvent.c_str() );
    } else if ( strEvent == "USER_CHANGED" ) {
        extern void SendSipNotify( const std::string &uri, const std::string &etag, const std::string &action );
        SendSipNotify( strUri, strEtag, strAction );

        // 가입자 캐시 즉시 갱신.
        // volte_subscriptions.id / ptt_subscriptions.id 는 E.164 `+` prefix 포함.
        // tel:+821001 → +821001 (scheme 만 제거, MSISDN 의 `+` 는 유지).
        std::string strUserId = strUri;
        if ( strUserId.substr( 0, 4 ) == "tel:" ) {
            strUserId = strUserId.substr( 4 );
        }

        if ( strAction == "DELETE" ) {
            gclsCspUserMap.Remove( strUserId );
            CLog::Print( LOG_INFO, "CscInterface: User cache removed [%s]", strUserId.c_str() );
        } else {
            // POST (신규) 또는 PUT (수정) — DB에서 다시 읽어 캐시 갱신
            if ( gclsCspUserMap.ReloadFromDb( strUserId ) ) {
                CLog::Print( LOG_INFO, "CscInterface: User cache updated [%s]", strUserId.c_str() );
            } else {
                CLog::Print( LOG_ERROR, "CscInterface: User not found in DB [%s]", strUserId.c_str() );
            }
        }
    }
}
