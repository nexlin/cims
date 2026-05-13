#include "CmpClient.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>

#include "Log.h"
#include "SimpleJson.h"
#include "SipMessageLogger.h"

CCmpClient::CCmpClient()
    : m_iCmpPort( 0 ),
      m_hSocket( -1 ),
      m_iNextTransId( 1 ),
      m_bKeepAliveRunning( false ),
      m_bRecvRunning( false ),
      m_bConnected( false ),
      m_ring( 128 ) {
}

CCmpClient::~CCmpClient() {
    m_bKeepAliveRunning = false;
    if ( m_threadKeepAlive.joinable() ) {
        m_threadKeepAlive.join();
    }

    m_bRecvRunning = false;
    if ( m_hSocket != -1 ) {
        shutdown( m_hSocket, SHUT_RDWR );
        close( m_hSocket );
        m_hSocket = -1;
    }

    if ( m_threadRecv.joinable() ) {
        m_threadRecv.join();
    }
}

CCmpClient& CCmpClient::GetInstance() {
    static CCmpClient instance;
    return instance;
}

bool CCmpClient::Init( const std::string& strCmpIp, int iCmpPort, int iLocalPort ) {
    // std::lock_guard<std::mutex> lock(m_mutex); // No global mutex needed for init if called once
    m_strCmpIp = strCmpIp;
    m_iCmpPort = iCmpPort;
    m_iLocalCmpPort = iLocalPort;

    // Phase 1.E — primary endpoint 를 endpoints + ring 에 등록 (단일 endpoint backward compat).
    // 추가 endpoint 는 외부에서 AddEndpoint 호출 (csp.json 의 Cmp.Endpoints, 후속 라운드 활성).
    {
        std::lock_guard<std::mutex> lock( m_mutexEndpoints );
        CmpEndpoint primary( strCmpIp, iCmpPort );
        bool exists = false;
        for ( const auto& ep : m_endpoints ) {
            if ( ep.strKey == primary.strKey ) {
                exists = true;
                break;
            }
        }
        if ( !exists ) {
            m_endpoints.push_back( primary );
            m_ring.AddNode( primary.strKey );
            CLog::Print( LOG_INFO, "CmpClient: primary endpoint registered (key=%s)", primary.strKey.c_str() );
        }
    }

    if ( m_hSocket == -1 ) {
        m_hSocket = socket( AF_INET, SOCK_DGRAM, 0 );
        if ( m_hSocket < 0 ) {
            CLog::Print( LOG_ERROR, "CmpClient::Init socket error" );
            return false;
        }

        struct sockaddr_in localAddr;
        memset( &localAddr, 0, sizeof( localAddr ) );
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = htonl( INADDR_ANY );
        localAddr.sin_port = htons( m_iLocalCmpPort );

        if ( bind( m_hSocket, (struct sockaddr*)&localAddr, sizeof( localAddr ) ) < 0 ) {
            CLog::Print( LOG_ERROR, "CmpClient::Init bind error port=%d", m_iLocalCmpPort );
            close( m_hSocket );
            m_hSocket = -1;
            return false;
        }

        // Start Recv Thread
        m_bRecvRunning = true;
        m_threadRecv = std::thread( &CCmpClient::RecvLoop, this );
    }

    if ( !m_bKeepAliveRunning ) {
        m_bKeepAliveRunning = true;
        m_threadKeepAlive = std::thread( &CCmpClient::KeepAliveLoop, this );
    }

    return true;
}

// Helper to clean up transaction
void CCmpClient::OnTransactionComplete( unsigned int transId, bool success, const std::string& response ) {
    if ( transId == 0 ) return;

    std::shared_ptr<Transaction> pTrans = nullptr;
    {
        std::lock_guard<std::mutex> lock( m_mutexTrans );
        auto it = m_mapTransactions.find( transId );
        if ( it != m_mapTransactions.end() ) {
            pTrans = it->second;
        }
    }

    if ( pTrans ) {
        std::lock_guard<std::mutex> transLock( pTrans->mutex );
        pTrans->strResponse = response;
        pTrans->bSuccess = success;
        pTrans->bCompleted = true;
        pTrans->cv.notify_one();
    }
}

// Protocol: TRANS_ID CSP_ID CSP_SESS_ID CMP_ID CMP_SESS_ID CMD PAYLOAD...
// Example: 1001 CSP_MAIN sess_1 CMP_MAIN 0 add 1.2.3.4 1000 0 0
// Response: 1001 CSP_MAIN sess_1 CMP_MAIN 0 OK ...
// Phase 1.E-2 wrapper — session sticky → endpoint 선택 후 _SendOnEndpoint 호출.
bool CCmpClient::SendRequestAndWait( const std::string& strSessionKey,
                                      const SimpleJson::JsonNode& payload, std::string& strResponse ) {
    CmpEndpoint ep = _ResolveEndpoint( strSessionKey );
    return _SendOnEndpoint( ep, payload, strResponse );
}

// Legacy / heartbeat — primary endpoint
bool CCmpClient::SendRequestAndWait( const SimpleJson::JsonNode& payload, std::string& strResponse ) {
    CmpEndpoint ep = _ResolveEndpoint( "" );
    return _SendOnEndpoint( ep, payload, strResponse );
}

void CCmpClient::ReleaseEndpointForKey( const std::string& strSessionKey ) {
    if ( strSessionKey.empty() ) return;
    std::lock_guard<std::mutex> lock( m_mutexEndpoints );
    m_mapSessionToEndpointKey.erase( strSessionKey );
}

CmpEndpoint CCmpClient::_ResolveEndpoint( const std::string& strSessionKey ) {
    std::lock_guard<std::mutex> lock( m_mutexEndpoints );
    if ( m_endpoints.empty() ) {
        return CmpEndpoint( m_strCmpIp, m_iCmpPort );  // bootstrap fallback
    }
    if ( !strSessionKey.empty() ) {
        // 1) 캐시 hit
        auto it = m_mapSessionToEndpointKey.find( strSessionKey );
        if ( it != m_mapSessionToEndpointKey.end() ) {
            for ( const auto& ep : m_endpoints ) {
                if ( ep.strKey == it->second ) return ep;
            }
            // stale cache — fall through to ring
        }
        // 2) Ring select
        std::string selectedKey;
        if ( m_ring.Select( strSessionKey, selectedKey ) ) {
            for ( const auto& ep : m_endpoints ) {
                if ( ep.strKey == selectedKey ) {
                    m_mapSessionToEndpointKey[strSessionKey] = ep.strKey;
                    return ep;
                }
            }
        }
    }
    return m_endpoints.front();  // primary
}

bool CCmpClient::_SendOnEndpoint( const CmpEndpoint& ep, const SimpleJson::JsonNode& payload,
                                   std::string& strResponse ) {
    if ( m_hSocket == -1 ) return false;

    unsigned int transId;
    // Use shared_ptr to prevent Use-After-Free if timeout occurs while RecvLoop is active
    std::shared_ptr<Transaction> pTrans = std::make_shared<Transaction>();
    {
        std::lock_guard<std::mutex> lock( m_mutexTrans );
        transId = m_iNextTransId++;
        pTrans->id = transId;
        // flow 기록용 sesid/service/caller/callee 저장 (응답 수신 시 사용)
        pTrans->strSesId = payload.GetString( "sesid" );
        if ( pTrans->strSesId.empty() ) pTrans->strSesId = payload.GetString( "session_id" );
        if ( pTrans->strSesId.empty() ) pTrans->strSesId = payload.GetString( "group_id" );
        pTrans->strCaller = payload.GetString( "caller" );
        pTrans->strCallee = payload.GetString( "callee" );
        // payload 에 service 필드가 있으면 그대로 사용, 없으면 cmd 기반 자동 결정
        std::string strSvc = payload.GetString( "service" );
        if ( strSvc.empty() ) {
            std::string cmd = payload.GetString( "cmd" );
            if ( cmd.find( "PTT" ) != std::string::npos )
                strSvc = "mcptt";
            else if ( cmd.find( "SESSION" ) != std::string::npos )
                strSvc = "volte";
            else
                strSvc = "system";
        }
        pTrans->strService = strSvc;
        m_mapTransactions[transId] = pTrans;
    }

    // payload 에 service 필드가 없으면 Transaction 계산 결과로 주입
    if ( payload.GetString( "service" ).empty() && !pTrans->strService.empty() ) {
        const_cast<SimpleJson::JsonNode&>( payload ).Set( "service", pTrans->strService );
    }

    // Construct Packet (JSON wrapper)
    SimpleJson::JsonNode packet;
    packet.Set( "trans_id", (int)transId );
    packet.Set( "payload", payload );

    std::string strPacket = packet.ToString();
    CLog::Print( LOG_DEBUG, "CmpClient TX → %s:%d : %s", ep.strIp.c_str(), ep.iPort, strPacket.c_str() );
    if ( gclsSipLogger.IsEnabled() ) {
        std::string strCmd = payload.GetString( "cmd" );
        // service: Transaction 에 저장된 값 (payload.service 기반)
        const char* pszSvc = pTrans->strService.empty() ? "system" : pTrans->strService.c_str();
        std::string strTxId = std::to_string( transId );
        std::string strSesId = payload.GetString( "sesid" );
        if ( strSesId.empty() ) strSesId = payload.GetString( "session_id" );
        if ( strSesId.empty() ) strSesId = payload.GetString( "group_id" );

        // detail: method별 결정
        std::string strDetail;
        std::string caller = payload.GetString( "caller" );
        std::string callee = payload.GetString( "callee" );
        int iPeerIdx = (int)payload.GetInt( "peer_index", -1 );
        if ( strCmd == "ADD_PTT_GROUP" || strCmd == "REMOVE_PTT_GROUP" ) {
            strDetail = payload.GetString( "group_id" );
        } else if ( strCmd == "JOIN_PTT_GROUP" || strCmd == "LEAVE_PTT_GROUP" ) {
            strDetail = payload.GetString( "session_id" );
        } else if ( strCmd == "ADD_SESSION" ) {
            if ( !caller.empty() && !callee.empty() )
                strDetail = caller + "\xe2\x86\x92" + callee;
            else if ( !caller.empty() )
                strDetail = caller;
            else
                strDetail = payload.GetString( "session_id" );
        } else if ( strCmd == "MODIFY_SESSION" ) {
            if ( iPeerIdx == 1 && !callee.empty() )
                strDetail = callee;
            else if ( iPeerIdx == 0 && !caller.empty() )
                strDetail = caller;
            else if ( !caller.empty() && !callee.empty() )
                strDetail = caller + "\xe2\x86\x92" + callee;
        } else if ( strCmd == "REMOVE_SESSION" ) {
            if ( !caller.empty() && !callee.empty() )
                strDetail = caller + "\xe2\x86\x92" + callee;
            else
                strDetail = payload.GetString( "session_id" );
        }

        gclsSipLogger.LogMessage( "csp", "cmp", "JSON", strCmd.c_str(),
                                  ( ep.strIp + ":" + std::to_string( ep.iPort ) ).c_str(), strPacket.c_str(),
                                  pszSvc, strTxId.c_str(), strSesId.c_str(), strDetail.c_str(), caller.c_str(),
                                  callee.c_str() );
    }

    struct sockaddr_in servaddr;
    memset( &servaddr, 0, sizeof( servaddr ) );
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons( ep.iPort );
    servaddr.sin_addr.s_addr = inet_addr( ep.strIp.c_str() );

    if ( sendto( m_hSocket, strPacket.c_str(), strPacket.length(), 0, (struct sockaddr*)&servaddr,
                 sizeof( servaddr ) ) < 0 ) {
        CLog::Print( LOG_ERROR, "SendRequestAndWait sendto error" );
        // Remove from map
        {
            std::lock_guard<std::mutex> mapLock( m_mutexTrans );
            m_mapTransactions.erase( transId );
        }
        return false;
    }

    // Wait
    bool bResult = false;
    // Scope for unique_lock
    {
        std::unique_lock<std::mutex> lock( pTrans->mutex );
        if ( pTrans->cv.wait_for( lock, std::chrono::milliseconds( 2000 ) ) == std::cv_status::timeout ) {
            CLog::Print( LOG_ERROR, "SendRequestAndWait Timeout (TransId=%d)", transId );
            bResult = false;
        } else {
            strResponse = pTrans->strResponse;
            bResult = pTrans->bSuccess;
            CLog::Print( LOG_DEBUG, "CmpClient RX ← %s", strResponse.c_str() );
        }
    }

    // Safe Cleanup: Remove from map.
    // If RecvLoop holds a shared_ptr copy, the object won't be deleted yet.
    {
        std::lock_guard<std::mutex> mapLock( m_mutexTrans );
        m_mapTransactions.erase( transId );
    }

    return bResult;
}

// Overload for legacy string if needed, or remove.
// Refactoring commands to call the new one.
// The original SendRequestAndWait is removed as per instruction to modify it.
// If a string-based SendRequestAndWait is still needed, it would be an overload.

std::string CCmpClient::GetSesIdByKey( const std::string& strKey ) {
    std::lock_guard<std::mutex> lock( m_mutexSesid );
    auto it = m_mapKeyToSesid.find( strKey );
    if ( it != m_mapKeyToSesid.end() ) return it->second;
    return "";
}

// ── Phase 1.E — multi-endpoint dispatch ────────────────────────────────────
void CCmpClient::AddEndpoint( const std::string& strIp, int iPort ) {
    CmpEndpoint ep( strIp, iPort );
    std::lock_guard<std::mutex> lock( m_mutexEndpoints );
    for ( const auto& existing : m_endpoints ) {
        if ( existing.strKey == ep.strKey ) return;  // 중복 무시
    }
    m_endpoints.push_back( ep );
    m_ring.AddNode( ep.strKey );
    CLog::Print( LOG_INFO, "CmpClient::AddEndpoint registered (key=%s, total=%zu)", ep.strKey.c_str(),
                 m_endpoints.size() );
}

CmpEndpoint CCmpClient::SelectEndpointForSession( const std::string& strSessionId ) {
    std::lock_guard<std::mutex> lock( m_mutexEndpoints );

    // 기존 세션 → 캐시된 endpoint 우선 (Modify/Remove 가 같은 CMP 로 가야)
    auto it = m_mapSessionToEndpointKey.find( strSessionId );
    if ( it != m_mapSessionToEndpointKey.end() ) {
        for ( const auto& ep : m_endpoints ) {
            if ( ep.strKey == it->second ) return ep;
        }
        // 캐시된 endpoint 가 사라진 경우 → 다시 선택 (아래로 fall-through)
        m_mapSessionToEndpointKey.erase( it );
    }

    // 신규 세션 → ring 으로 선택
    std::string selectedKey;
    if ( !strSessionId.empty() && m_ring.Select( strSessionId, selectedKey ) ) {
        for ( const auto& ep : m_endpoints ) {
            if ( ep.strKey == selectedKey ) {
                m_mapSessionToEndpointKey[strSessionId] = selectedKey;
                return ep;
            }
        }
    }

    // fallback — primary 반환 (sessionId 비어있거나 ring 빈 경우)
    if ( !m_endpoints.empty() ) return m_endpoints.front();
    return CmpEndpoint( m_strCmpIp, m_iCmpPort );
}

bool CCmpClient::AddSession( const std::string& strSessionId, std::string& strLocalIp, int& iLocalPort,
                             int& iLocalVideoPort, const std::string& strRecordDir, const std::string& strLogDir,
                             const std::string& strCaller, const std::string& strCallee, const std::string& strRmtIp,
                             int iRmtPort, int iRmtVideoPort, const std::string& strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "ADD_SESSION" );
    req.Set( "session_id", strSessionId );
    req.Set( "remote_ip", strRmtIp.empty() ? "0.0.0.0" : strRmtIp );
    req.Set( "remote_port", iRmtPort );
    req.Set( "remote_video_port", iRmtVideoPort );
    req.Set( "peer_index", 0 );
    if ( !strRecordDir.empty() ) req.Set( "record_dir", strRecordDir );
    if ( !strLogDir.empty() ) req.Set( "log_dir", strLogDir );
    if ( !strCaller.empty() ) req.Set( "caller", strCaller );
    if ( !strCallee.empty() ) req.Set( "callee", strCallee );

    // sesid: 호출자 제공 > 발행
    std::string strFinalSesId = strSesId.empty() ? CSipMessageLogger::IssueSesId( strCaller, "csp" ) : strSesId;
    req.Set( "sesid", strFinalSesId );
    {
        std::lock_guard<std::mutex> lock( m_mutexSesid );
        m_mapKeyToSesid[strSessionId] = strFinalSesId;
    }

    // Header info (legacy args)
    req.Set( "csp_id", "CSP_MAIN" );
    req.Set( "csp_sess_id", strSessionId );
    req.Set( "cmp_id", "CMP_MAIN" );
    req.Set( "cmp_sess_id", "0" );

    std::string strResp;

    CLog::Print( LOG_DEBUG, "CmpClient::AddSession: %s", req.ToString().c_str() );

    std::string strReqBody = req.ToString();

    if ( !SendRequestAndWait( strSessionId, req, strResp ) ) return false;

    // Response Body: CSP_MAIN <sessId> CMP_MAIN <cmpSess> OK <ip> <port> <vport>
    // The response is now expected to be JSON.
    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( strResp );
    if ( respNode.type != SimpleJson::JSON_OBJECT ) {
        CLog::Print( LOG_ERROR, "CmpClient::AddSession: Failed to parse JSON response: %s", strResp.c_str() );
        return false;
    }

    if ( respNode.Has( "status" ) && respNode.Get( "status" ).AsString() == "OK" ) {
        strLocalIp = respNode.Get( "local_ip" ).AsString();
        iLocalPort = respNode.Get( "local_port" ).AsInt();
        iLocalVideoPort = respNode.Get( "local_video_port" ).AsInt();
        return true;
    }

    return false;
}

bool CCmpClient::ModifySession( const std::string& strSessionId, const std::string& strRmtIp, int iRmtPort,
                                int iRmtVideoPort, int iPeerIdx, const std::string& strCaller,
                                const std::string& strCallee, const std::string& strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "MODIFY_SESSION" );
    req.Set( "session_id", strSessionId );
    req.Set( "remote_ip", strRmtIp );
    req.Set( "remote_port", iRmtPort );
    req.Set( "remote_video_port", iRmtVideoPort );
    req.Set( "peer_index", iPeerIdx );
    if ( !strCaller.empty() ) req.Set( "caller", strCaller );
    if ( !strCallee.empty() ) req.Set( "callee", strCallee );

    // sesid: 파라미터 > 캐시 > 발행
    std::string strFinalSesId = strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = GetSesIdByKey( strSessionId );
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( strCaller, "csp" );
    req.Set( "sesid", strFinalSesId );
    {
        std::lock_guard<std::mutex> lock( m_mutexSesid );
        m_mapKeyToSesid[strSessionId] = strFinalSesId;
    }

    req.Set( "csp_id", "CSP_MAIN" );
    req.Set( "csp_sess_id", strSessionId );
    req.Set( "cmp_id", "CMP_MAIN" );
    req.Set( "cmp_sess_id", "0" );

    std::string strResp;
    std::string strReqBody = req.ToString();

    if ( !SendRequestAndWait( strSessionId, req, strResp ) ) return false;

    return true;
}

bool CCmpClient::UpdateSession( const std::string& strSessionId, const std::string& strRmtIp, int iRmtPort,
                                int iRmtVideoPort, int iPeerIdx, const std::string& strCaller,
                                const std::string& strCallee, std::string& strLocalIp, int& iLocalPort,
                                const std::string& strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "MODIFY_SESSION" );
    req.Set( "session_id", strSessionId );
    req.Set( "remote_ip", strRmtIp );
    req.Set( "remote_port", iRmtPort );
    req.Set( "remote_video_port", iRmtVideoPort );
    req.Set( "peer_index", iPeerIdx );
    if ( !strCaller.empty() ) req.Set( "caller", strCaller );
    if ( !strCallee.empty() ) req.Set( "callee", strCallee );

    // sesid: 파라미터 > 캐시 > 발행
    std::string strFinalSesId = strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = GetSesIdByKey( strSessionId );
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( strCaller, "csp" );
    req.Set( "sesid", strFinalSesId );
    {
        std::lock_guard<std::mutex> lock( m_mutexSesid );
        m_mapKeyToSesid[strSessionId] = strFinalSesId;
    }

    // Header info (legacy args)
    req.Set( "csp_id", "CSP_MAIN" );
    req.Set( "csp_sess_id", strSessionId );
    req.Set( "cmp_id", "CMP_MAIN" );
    req.Set( "cmp_sess_id", "0" );

    std::string strResp;

    CLog::Print( LOG_DEBUG, "CmpClient::UpdateSession: %s", req.ToString().c_str() );

    if ( !SendRequestAndWait( strSessionId, req, strResp ) ) return false;

    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( strResp );
    if ( respNode.type != SimpleJson::JSON_OBJECT ) {
        CLog::Print( LOG_ERROR, "CmpClient::UpdateSession: Failed to parse JSON response: %s", strResp.c_str() );
        return false;
    }

    if ( respNode.Has( "status" ) && respNode.Get( "status" ).AsString() == "OK" ) {
        strLocalIp = respNode.Get( "local_ip" ).AsString();
        iLocalPort = respNode.Get( "local_port" ).AsInt();
        // iLocalVideoPort is not returned by UpdateSession in the original code, but it's in AddSession.
        // Assuming it's not needed here or would be part of the response if the protocol changed.
        return true;
    }
    return false;
}

bool CCmpClient::RemoveSession( const std::string& strSessionId, const std::string& strCaller,
                                const std::string& strCallee, const std::string& strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "REMOVE_SESSION" );
    req.Set( "session_id", strSessionId );
    if ( !strCaller.empty() ) req.Set( "caller", strCaller );
    if ( !strCallee.empty() ) req.Set( "callee", strCallee );

    // sesid: 파라미터 > 캐시 > 발행
    std::string strFinalSesId = strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = GetSesIdByKey( strSessionId );
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( strCaller, "csp" );
    req.Set( "sesid", strFinalSesId );
    // Remove 후 캐시 정리
    {
        std::lock_guard<std::mutex> lock( m_mutexSesid );
        m_mapKeyToSesid.erase( strSessionId );
    }

    // Header info (legacy args)
    req.Set( "csp_id", "CSP_MAIN" );
    req.Set( "csp_sess_id", strSessionId );
    req.Set( "cmp_id", "CMP_MAIN" );
    req.Set( "cmp_sess_id", "0" );

    std::string strResp;
    CLog::Print( LOG_DEBUG, "CmpClient::RemoveSession: %s", req.ToString().c_str() );
    std::string strReqBody = req.ToString();
    bool bRet = SendRequestAndWait( strSessionId, req, strResp );
    ReleaseEndpointForKey( strSessionId );  // remove 후 캐시 해제
    return bRet;
}

bool CCmpClient::AddGroup( const std::string& strGroupId, const std::vector<std::shared_ptr<CspPttUser>>& vecMembers,
                           std::string& strIp, int& iPort, int& iFloorPort, int& iVideoPort,
                           const std::string& strRecordDir, const std::string& strLogDir, bool bVideoEnabled,
                           int iSessionSeq, const std::string& strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "ADD_PTT_GROUP" );
    req.Set( "group_id", strGroupId );

    // sesid: 파라미터 > 캐시 > 발행 (caller=group_id의 세션 시작 UE 정보가 없으면 빈값)
    std::string strFinalSesId = strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = GetSesIdByKey( strGroupId );
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );
    {
        std::lock_guard<std::mutex> lock( m_mutexSesid );
        m_mapKeyToSesid[strGroupId] = strFinalSesId;
    }

    req.Set( "subid", std::to_string( iSessionSeq > 0 ? iSessionSeq : 1 ) );
    req.Set( "count", (int)vecMembers.size() );
    if ( !strRecordDir.empty() ) req.Set( "record_dir", strRecordDir );
    if ( !strLogDir.empty() ) req.Set( "log_dir", strLogDir );
    if ( bVideoEnabled ) req.Set( "video_enabled", 1 );

    std::stringstream ssMembers;
    for ( size_t i = 0; i < vecMembers.size(); ++i ) {
        if ( !vecMembers[i] ) continue;
        if ( i > 0 ) ssMembers << ",";
        ssMembers << vecMembers[i]->_id << ":" << vecMembers[i]->_priority;
    }
    req.Set( "members", ssMembers.str() );

    // Header info (legacy args)
    req.Set( "csp_id", "CSP_MAIN" );
    req.Set( "csp_sess_id", "0" );
    req.Set( "cmp_id", "CMP_MAIN" );
    req.Set( "cmp_sess_id", "0" );

    std::string strResp;
    std::string strReqBody = req.ToString();

    if ( SendRequestAndWait( strGroupId, req, strResp ) ) {
        SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( strResp );
        if ( respNode.type != SimpleJson::JSON_OBJECT ) {
            CLog::Print( LOG_ERROR, "CmpClient::AddGroup: Failed to parse JSON response: %s", strResp.c_str() );
            return false;
        }

        if ( respNode.Has( "status" ) && respNode.Get( "status" ).AsString() == "OK" ) {
            strIp = respNode.Get( "ip" ).AsString();
            iPort = respNode.Get( "port" ).AsInt();
            iFloorPort = respNode.Has( "floor_port" ) ? respNode.Get( "floor_port" ).AsInt() : 0;
            iVideoPort = respNode.Has( "video_port" ) ? respNode.Get( "video_port" ).AsInt() : 0;
            CLog::Print( LOG_INFO, "CmpClient::AddGroup Success: %s:%d floor=%d video=%d Members: %d", strIp.c_str(),
                         iPort, iFloorPort, iVideoPort, (int)vecMembers.size() );
            return true;
        }
        CLog::Print( LOG_ERROR, "CmpClient::AddGroup Fail: Status not OK. Resp: %s", strResp.c_str() );
    } else {
        CLog::Print( LOG_ERROR, "CmpClient::AddGroup SendRequest Failed" );
    }
    return false;
}

bool CCmpClient::ModifyGroup( const std::string& strGroupId, const std::vector<std::shared_ptr<CspPttUser>>& vecMembers,
                              const std::string& strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "MODIFY_GROUP" );
    req.Set( "group_id", strGroupId );

    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strGroupId ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );

    std::stringstream ssMembers;
    for ( size_t i = 0; i < vecMembers.size(); ++i ) {
        if ( !vecMembers[i] ) continue;
        if ( i > 0 ) ssMembers << ",";
        ssMembers << vecMembers[i]->_id << ":" << vecMembers[i]->_priority;
    }
    req.Set( "members", ssMembers.str() );

    // Header info (legacy args)
    req.Set( "csp_id", "CSP_MAIN" );
    req.Set( "csp_sess_id", "0" );
    req.Set( "cmp_id", "CMP_MAIN" );
    req.Set( "cmp_sess_id", "0" );

    std::string strResp;
    return SendRequestAndWait( strGroupId, req, strResp );
}

bool CCmpClient::JoinGroup( const std::string& strGroupId, const std::string& strSessionId,
                            const std::string& strUserIp, int iUserPort, int iFloorPort, int iVideoPort,
                            const std::string& strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "JOIN_PTT_GROUP" );
    req.Set( "group_id", strGroupId );

    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strGroupId ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );

    req.Set( "session_id", strSessionId );
    req.Set( "user_ip", strUserIp );
    req.Set( "user_port", iUserPort );
    if ( iFloorPort > 0 ) req.Set( "user_floor_port", iFloorPort );
    if ( iVideoPort > 0 ) req.Set( "user_video_port", iVideoPort );

    req.Set( "csp_id", "CSP_MAIN" );
    req.Set( "csp_sess_id", strSessionId );
    req.Set( "cmp_id", "CMP_MAIN" );
    req.Set( "cmp_sess_id", "0" );

    std::string resp;
    std::string strReqBody = req.ToString();
    bool bRet = SendRequestAndWait( strGroupId, req, resp );
    return bRet;
}

bool CCmpClient::LeaveGroup( const std::string& strGroupId, const std::string& strSessionId,
                             const std::string& strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "LEAVE_PTT_GROUP" );
    req.Set( "group_id", strGroupId );

    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strGroupId ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );

    req.Set( "session_id", strSessionId );

    req.Set( "csp_id", "CSP_MAIN" );
    req.Set( "csp_sess_id", strSessionId );
    req.Set( "cmp_id", "CMP_MAIN" );
    req.Set( "cmp_sess_id", "0" );

    std::string resp;
    std::string strReqBody = req.ToString();
    bool bRet = SendRequestAndWait( strGroupId, req, resp );
    return bRet;
}

bool CCmpClient::RemoveGroup( const std::string& strGroupId, const std::string& strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "REMOVE_PTT_GROUP" );
    req.Set( "group_id", strGroupId );

    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strGroupId ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );
    {
        std::lock_guard<std::mutex> lock( m_mutexSesid );
        m_mapKeyToSesid.erase( strGroupId );
    }

    // Header info (legacy args)
    req.Set( "csp_id", "CSP_MAIN" );
    req.Set( "csp_sess_id", "0" );
    req.Set( "cmp_id", "CMP_MAIN" );
    req.Set( "cmp_sess_id", "0" );

    std::string strResp;
    bool bRet = SendRequestAndWait( strGroupId, req, strResp );
    ReleaseEndpointForKey( strGroupId );  // remove 후 캐시 해제
    return bRet;
}

bool CCmpClient::Alive() {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "HEARTBEAT" );
    // HEARTBEAT는 매 번 신규 sesid 발행 (요청/응답 쌍은 같은 sesid)
    req.Set( "sesid", CSipMessageLogger::IssueSesId( "", "csp" ) );
    req.Set( "csp_id", "CSP_MAIN" );
    req.Set( "csp_sess_id", "0" );
    req.Set( "cmp_id", "CMP_MAIN" );
    req.Set( "cmp_sess_id", "0" );

    std::string resp;
    return SendRequestAndWait( req, resp );
}

void CCmpClient::RecvLoop() {
    char buffer[4096];
    struct sockaddr_in cliaddr;
    socklen_t len;

    while ( m_bRecvRunning ) {
        len = sizeof( cliaddr );
        int n = recvfrom( m_hSocket, buffer, sizeof( buffer ) - 1, 0, (struct sockaddr*)&cliaddr, &len );
        if ( n > 0 ) {
            buffer[n] = '\0';
            std::string strPacket = buffer;
            // printf("RX: %s\n", strPacket.c_str());

            // New JSON Parsing
            SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse( strPacket );
            if ( root.type == SimpleJson::JSON_OBJECT ) {
                int transId = (int)root.GetInt( "trans_id", 0 );
                std::string respBody;
                if ( root.Has( "response" ) ) {
                    SimpleJson::JsonNode r = root.Get( "response" );
                    if ( r.type == SimpleJson::JSON_STRING )
                        respBody = r.strValue;
                    else
                        respBody = r.ToString();
                }

                if ( gclsSipLogger.IsEnabled() ) {
                    // 응답에서 원래 명령 추출 (trans_id로 매칭)
                    std::string strCmd;
                    {
                        std::lock_guard<std::mutex> lk( m_mutexTrans );
                        auto it = m_mapTransactions.find( transId );
                        if ( it != m_mapTransactions.end() ) {
                            // 요청 payload에서 cmd 추출
                            // trans_id → 원래 요청 매핑은 없으므로, 응답 body에서 status 확인
                        }
                    }
                    // 응답 body에서 상태 추출
                    bool bOk = ( respBody.find( "OK" ) != std::string::npos );
                    std::string strStatus = bOk ? "OK" : "ERROR";

                    // Transaction에서 sesid/service/caller/callee 조회
                    std::string strRxSesId, strRxSvc, strRxCaller, strRxCallee;
                    {
                        std::lock_guard<std::mutex> lk2( m_mutexTrans );
                        auto itT = m_mapTransactions.find( transId );
                        if ( itT != m_mapTransactions.end() ) {
                            strRxSesId = itT->second->strSesId;
                            strRxSvc = itT->second->strService;
                            strRxCaller = itT->second->strCaller;
                            strRxCallee = itT->second->strCallee;
                        }
                    }
                    const char* pszSvc = strRxSvc.empty() ? "system" : strRxSvc.c_str();
                    std::string strRxTxId = std::to_string( transId );
                    gclsSipLogger.LogMessage( "cmp", "csp", "JSON", strStatus.c_str(),
                                              ( m_strCmpIp + ":" + std::to_string( m_iCmpPort ) ).c_str(),
                                              strPacket.c_str(), pszSvc, strRxTxId.c_str(), strRxSesId.c_str(), "",
                                              strRxCaller.c_str(), strRxCallee.c_str() );
                }
                // Notify Transaction matches
                OnTransactionComplete( transId, true, respBody );
            } else {
                CLog::Print( LOG_INFO, "CmpClient Non-JSON RX: %s", strPacket.c_str() );
            }
        }
    }
}

// OnPacketReceived is deprecated/unused with new RecvLoop logic.

void CCmpClient::KeepAliveLoop() {
    while ( m_bKeepAliveRunning ) {
        // Use JSON Alive()
        bool bSuccess = Alive();

        if ( bSuccess ) {
            if ( !m_bConnected ) {
                m_bConnected = true;
                if ( m_fnConnectionCallback ) {
                    m_fnConnectionCallback( true );
                }
                CLog::Print( LOG_INFO, "CMP Connected" );
            }
        } else {
            if ( m_bConnected ) {
                m_bConnected = false;
                if ( m_fnConnectionCallback ) {
                    m_fnConnectionCallback( false );
                }
                CLog::Print( LOG_INFO, "CMP Disconnected" );
            }
        }

        std::this_thread::sleep_for( std::chrono::seconds( 3 ) );
    }
}
