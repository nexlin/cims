#include "CscInterface.h"

#include <sstream>

#include "AuthzRevoke.h"
#include "CallDir.h"
#include "CallMap.h"
#include "CscEndpointCache.h"
#include "CspConfigCache.h"
#include "CspListenerManager.h"
#include "CspLocalNodeMap.h"
#include "CspPhoneGroup.h"
#include "CspRole.h"
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
        // active_calls: CallMap 실측 (call log 가 파일 기반으로 바뀐 뒤
        //   GetActiveVoipCallCount 는 상시 0 인 레거시 no-op — DB 연결 여부와 무관하게
        //   메모리 다이얼로그 수가 유일한 실측이다. 세션 사용률 임계(A-QOS-008)의 분자.
        bool dbConnected = gclsDbManager.IsConnected();
        int activeCalls = gclsCallMap.GetCount();

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

        // CSC 재기동 = 설정 재로드 계기 — 단말용 MCPTT 서비스 주소(xcap-root) 재취득.
        gclsCscEndpointCache.Refresh();

        // Resync user map from DB — 사용자 캐시도 판정 근거다(EffectiveGroupOf 의 폴백이 pickup_group).
        //   실패를 흘려보내면 낡은 소속으로 스윕이 돌아 이미 그룹을 옮긴 사람을 «그대로» 로 본다.
        bool bUsersUnavail = false;
        gclsCspUserMap.LoadFromDb( &bUsersUnavail );
        bool bMapsFresh = !bUsersUnavail;
        // 전화 그룹·역할도 재동기 (대표번호·감청 범위 — dispatch_center.md §3.5)
        if ( gclsDbManager.HasPhoneGroupTables() ) bMapsFresh = gclsPhoneGroupMap.LoadFromDb() && bMapsFresh;
        if ( gclsDbManager.HasRoleTables() ) bMapsFresh = gclsRoleMap.LoadFromDb() && bMapsFresh;
        // 재기동 중에 바뀐 배정·멤버십이 있을 수 있다 — 재적재한 값으로 성립물을 다시 판정한다(§5.10).
        //   **적재가 실패했으면 스윕하지 않는다** — 옛 맵으로 판정하면 «바뀐 것이 없는데 전부 철회» 가 된다.
        if ( bMapsFresh )
            CspAuthz::RevokeUnauthorized( "CSC_RESTART" );
        else {
            CLog::Print( LOG_ERROR, "CscInterface: CSC_RESTART — 맵 적재 실패로 인가 회수 생략(기존 성립물 유지)" );
            CspAuthz::NotePolicyReloadOwed( "CSC_RESTART" );
        }

        // Trigger full group resync (SyncGroupsState)
        gclsGroupCallService.OnGroupConfigChanged();

        // 동적 설정(listener/trunk/route/acl/service) 는 jsonl + SIGUSR1 경로로 반영됨.
        // CSC_RESTART 는 이제 사용자/그룹 상태만 담당.
    } else if ( strEvent == "LISTENER_CHANGED" || strEvent == "TRUNK_CHANGED" || strEvent == "ROUTE_RULE_CHANGED" ||
                strEvent == "ACCESS_LIST_CHANGED" || strEvent == "SERVICE_CHANGED" ) {
        // Phase C 이후: 동적 설정은 agent → jsonl → SIGUSR1 경로로만 반영.
        // 이 이벤트는 Phase B 이전의 HTTP pull 경로용으로 더 이상 수신하지 않음.
        CLog::Print( LOG_DEBUG, "CscInterface: ignoring deprecated event %s (use SIGUSR1 path)", strEvent.c_str() );
    } else if ( strEvent == "SERVICE_CONFIG_CHANGED" ) {
        // 시스템 전역 정책(service-config) 변경 — cms 구독자 전원에게 재조회 통지.
        //   CSP 는 이 문서를 소비하지 않으므로(서빙은 CSC XCAP) 캐시 갱신 없이 중계만 한다.
        extern void SendServiceConfigNotify( const std::string &etag );
        SendServiceConfigNotify( strEtag );
    } else if ( strEvent == "PHONE_GROUP_CHANGED" || strEvent == "DISPATCH_GROUP_CHANGED" ) {
        // 전화 그룹 변경 (dispatch_center.md §3.5) — uri = 그룹 id. DELETE 는 맵에서 제거, 그 외(POST/PUT/멤버 변경)는
        //   DB 단건 재적재. uri 가 비면 전량 재적재. 가입자 pickup_group 파생 갱신은 CSC 가 USER_CHANGED 를 따로
        //   보낸다. DISPATCH_GROUP_CHANGED 는 전환 전 CSC 의 이름 — 같은 처리 + 역할도 전량 재적재(한 엔티티였던 범위
        //   열 대비).
        std::string strGroupId = strUri;
        if ( strGroupId.substr( 0, 4 ) == "tel:" ) strGroupId = strGroupId.substr( 4 );
        bool bGroupsFresh = true;
        if ( strEvent == "DISPATCH_GROUP_CHANGED" ) {
            CLog::Print( LOG_INFO,
                         "CscInterface: DISPATCH_GROUP_CHANGED (legacy name) — reloading phone groups + roles" );
            // 역할 적재 결과도 센다 — 실패한 채 아래 스윕이 돌면 옛 역할로 «자격 없음» 을 판정한다.
            if ( gclsDbManager.HasRoleTables() && !gclsRoleMap.LoadFromDb() ) {
                bGroupsFresh = false;
                CLog::Print( LOG_ERROR, "CscInterface: DISPATCH_GROUP_CHANGED — 역할 적재 실패" );
                CspAuthz::NotePolicyReloadOwed( "PHONE_GROUP_CHANGED" );
            }
        }
        if ( !gclsDbManager.HasPhoneGroupTables() ) {
            CLog::Print( LOG_INFO, "CscInterface: %s ignored — phone_groups table absent", strEvent.c_str() );
        } else if ( strGroupId.empty() ) {
            if ( !gclsPhoneGroupMap.LoadFromDb() ) {
                bGroupsFresh = false;
                CspAuthz::NotePolicyReloadOwed( "PHONE_GROUP_CHANGED" );
            }
            CLog::Print( LOG_INFO, "CscInterface: PhoneGroupMap reloaded (%d groups, ok=%d)",
                         gclsPhoneGroupMap.GetCount(), (int)bGroupsFresh );
        } else if ( strAction == "DELETE" ) {
            gclsPhoneGroupMap.Remove( strGroupId.c_str() );
            CLog::Print( LOG_INFO, "CscInterface: Phone group removed [%s]", strGroupId.c_str() );
        } else {
            // **«없음» 과 «조회 불능» 을 가른다.** 둘을 섞으면 DB 일시 장애가 «그룹 삭제» 로 읽혀, 그 그룹을
            //   근거로 서 있던 감시·픽업이 통째로 끊긴다(§5.10).
            bool bUnavail = false;
            if ( gclsPhoneGroupMap.LoadOneFromDb( strGroupId.c_str(), &bUnavail ) ) {
                CLog::Print( LOG_INFO, "CscInterface: Phone group updated [%s]", strGroupId.c_str() );
            } else if ( bUnavail ) {
                bGroupsFresh = false;  // 판정 근거가 낡았다 — 아래 스윕을 건너뛴다
                CLog::Print( LOG_ERROR, "CscInterface: Phone group [%s] 조회 불능 — 맵 유지·인가 회수 생략",
                             strGroupId.c_str() );
                CspAuthz::NotePolicyReloadOwed( "PHONE_GROUP_CHANGED" );
            } else {
                // 질의는 됐고 행이 없다 = 삭제 (통지 순서 역전 방어)
                gclsPhoneGroupMap.Remove( strGroupId.c_str() );
                CLog::Print( LOG_INFO, "CscInterface: Phone group not in DB — removed [%s]", strGroupId.c_str() );
            }
        }
        // 전화 그룹은 감시 인가의 한 축이다 — 규칙 1(같은 그룹)과 monitor_call=own 이 그룹 멤버십으로 답한다.
        //   그룹에서 빠지면 그 그룹원을 보던 BLF 구독도 근거를 잃으므로 같이 걷는다(§5.10).
        if ( gclsDbManager.HasPhoneGroupTables() && bGroupsFresh )
            CspAuthz::RevokeUnauthorized( "PHONE_GROUP_CHANGED" );
    } else if ( strEvent == "ROLE_CHANGED" ) {
        // 역할·배정·대상 변경 (mcptt_authorization.md §2, dispatch_center.md §3.5) — 배정은 person 단위라 회선 펼침이
        //   바뀌므로 전량 재적재한다(역할 수는 작다). uri 는 역할 id 또는 person id — 로그용.
        if ( !gclsDbManager.HasRoleTables() ) {
            CLog::Print( LOG_INFO, "CscInterface: ROLE_CHANGED ignored — role tables absent" );
        } else {
            const bool bRolesFresh = gclsRoleMap.LoadFromDb();
            CLog::Print( LOG_INFO, "CscInterface: RoleMap reloaded (%d roles, ok=%d) [%s]", gclsRoleMap.GetCount(),
                         (int)bRolesFresh, strUri.c_str() );
            // 재적재만으로는 **이미 성립한 것**이 걷히지 않는다 — 구독은 갱신으로, 감청·청취 leg 은 통화가
            //   끝날 때까지 산다. 잃은 자격을 즉시 회수한다(dispatch_center.md §5.10).
            //   적재가 실패하면 판정 근거가 낡았으므로 회수하지 않는다 — DB 일시 장애를 서비스 정지로 바꾸지 않는다.
            if ( bRolesFresh )
                CspAuthz::RevokeUnauthorized( "ROLE_CHANGED" );
            else {
                CLog::Print( LOG_ERROR, "CscInterface: ROLE_CHANGED — 역할 적재 실패로 인가 회수 생략" );
                CspAuthz::NotePolicyReloadOwed( "ROLE_CHANGED" );
            }
        }
    } else if ( strEvent == "USER_CHANGED" ) {
        extern void SendSipNotify( const std::string &uri, const std::string &etag, const std::string &action );
        SendSipNotify( strUri, strEtag, strAction );

        // 가입자 캐시 즉시 갱신.
        // 가입 테이블(voip·volte·ptt _subscriptions).id 는 E.164 `+` prefix 포함.
        // tel:+821001 → +821001 (scheme 만 제거, MSISDN 의 `+` 는 유지).
        std::string strUserId = strUri;
        if ( strUserId.substr( 0, 4 ) == "tel:" ) {
            strUserId = strUserId.substr( 4 );
        }

        // 갱신 **전** 파생 픽업 그룹 — 감시 인가의 한 축이다(§5.2 규칙 1·monitor_call=own). CSC 는 멤버 이동을
        //   PHONE_GROUP_CHANGED 와 USER_CHANGED(PUT) 두 통지로 보내는데, 앞 통지의 스윕 시점에는 멤버 색인에서
        //   빠졌어도 `EffectiveGroupOf` 가 **아직 낡은 이 캐시값으로 폴백**해 «여전히 같은 그룹» 으로 판정한다
        //   (`CspPhoneGroup.cpp` `EffectiveGroupOf`). 그래서 캐시가 실제로 바뀐 이 시점에 한 번 더 걷는다.
        std::string strPrevPickup;
        {
            CspUser clsPrev;
            if ( gclsCspUserMap.Select( strUserId.c_str(), clsPrev ) ) strPrevPickup = clsPrev.EffectivePickupGroup();
        }

        if ( strAction == "DELETE" ) {
            gclsCspUserMap.Remove( strUserId );
            CLog::Print( LOG_INFO, "CscInterface: User cache removed [%s]", strUserId.c_str() );
        } else {
            // POST (신규) 또는 PUT (수정) — DB에서 다시 읽어 캐시 갱신
            if ( gclsCspUserMap.ReloadFromDb( strUserId ) ) {
                CLog::Print( LOG_INFO, "CscInterface: User cache updated [%s]", strUserId.c_str() );
            } else {
                // 단건 조회는 «없음» 과 «조회 불능» 을 구분하지 못한다 — 어느 쪽이든 캐시가 통지와 어긋난
                //   상태이므로 빚으로 남긴다(§5.10). 자료가 살아나면 전량 재적재 후 다시 판정한다.
                CLog::Print( LOG_ERROR, "CscInterface: User 재적재 실패(없음 또는 조회 불능) [%s] — 재판정 예약",
                             strUserId.c_str() );
                CspAuthz::NotePolicyReloadOwed( "USER_CHANGED" );
            }
        }

        std::string strNowPickup;
        if ( strAction != "DELETE" ) {
            CspUser clsNow;
            if ( gclsCspUserMap.Select( strUserId.c_str(), clsNow ) ) strNowPickup = clsNow.EffectivePickupGroup();
        }
        const bool bPickupChanged = ( strNowPickup != strPrevPickup );

        // 회선 개설/삭제는 person 의 회선 집합을 바꾼다 — 역할 맵의 회선 펼침을 다시 만든다(§3.5).
        //   PUT(회선 속성 변경)은 회선 집합을 바꾸지 않으므로 재적재하지 않는다 — 역할 배정 자체가 바뀌면
        //   ROLE_CHANGED 가 따로 온다.
        if ( gclsDbManager.HasRoleTables() && ( strAction == "POST" || strAction == "DELETE" ) ) {
            // 회선이 사라지면 그 회선의 역할 펼침도 사라진다 — 그 회선으로 선 구독·leg 을 같이 걷는다(§5.10).
            //   적재 실패 시에는 걷지 않는다(위와 같은 이유).
            if ( gclsRoleMap.LoadFromDb() )
                CspAuthz::RevokeUnauthorized( "USER_CHANGED" );
            else {
                CLog::Print( LOG_ERROR, "CscInterface: USER_CHANGED — 역할 적재 실패로 인가 회수 생략" );
                CspAuthz::NotePolicyReloadOwed( "USER_CHANGED" );
            }
        } else if ( bPickupChanged && gclsDbManager.HasPhoneGroupTables() ) {
            // 회선 집합은 그대로다 — 역할 맵은 재적재하지 않고 그룹 축만 다시 판정한다(§5.10).
            CLog::Print( LOG_INFO, "CscInterface: USER_CHANGED pickup_group %s → %s [%s] — 인가 재판정",
                         strPrevPickup.empty() ? "(none)" : strPrevPickup.c_str(),
                         strNowPickup.empty() ? "(none)" : strNowPickup.c_str(), strUserId.c_str() );
            CspAuthz::RevokeUnauthorized( "USER_CHANGED" );
        }
    }
}
