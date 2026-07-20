#include "CmdpClient.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>

#include "Log.h"
#include "SipMessageLogger.h"

// MSRP 세션 식별자 발행 (재시작 경계 포함 전역 유일 — CMDP 잔존 고아와 충돌 불가).
std::string CCmdpClient::IssueSessionId() {
    return CSipMessageLogger::IssueUniqueId( "csp" );
}

// trans_id 초기값 — 부팅 시각(ms) 하위 비트 시드 (CmpClient::SeedTransId 와 동일 근거).
static unsigned int SeedTransId() {
    struct timeval tv;
    gettimeofday( &tv, NULL );
    return (unsigned int)( ( (unsigned long long)tv.tv_sec * 1000ULL + tv.tv_usec / 1000 ) & 0x3FFFFFFF ) | 1;
}

CCmdpClient::CCmdpClient()
    : m_iCmdpPort( 0 ),
      m_iLocalPort( 0 ),
      m_hSocket( -1 ),
      m_iNextTransId( SeedTransId() ),
      m_bKeepAliveRunning( false ),
      m_bRecvRunning( false ),
      m_bEventRunning( false ),
      m_bConnected( false ),
      m_iAliveFailCount( 0 ) {
}

CCmdpClient::~CCmdpClient() {
    m_bKeepAliveRunning = false;
    if ( m_threadKeepAlive.joinable() ) {
        m_threadKeepAlive.join();
    }

    m_bEventRunning = false;
    m_cvEvents.notify_all();
    if ( m_threadEventDispatch.joinable() ) {
        m_threadEventDispatch.join();
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

CCmdpClient &CCmdpClient::GetInstance() {
    static CCmdpClient instance;
    return instance;
}

bool CCmdpClient::Init( const std::string &strCmdpIp, int iCmdpPort, int iLocalPort ) {
    m_strCmdpIp = strCmdpIp;
    m_iCmdpPort = iCmdpPort;
    m_iLocalPort = iLocalPort;

    if ( m_hSocket == -1 ) {
        m_hSocket = socket( AF_INET, SOCK_DGRAM, 0 );
        if ( m_hSocket < 0 ) {
            CLog::Print( LOG_ERROR, "CmdpClient::Init socket error" );
            return false;
        }

        struct sockaddr_in localAddr;
        memset( &localAddr, 0, sizeof( localAddr ) );
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = htonl( INADDR_ANY );
        localAddr.sin_port = htons( m_iLocalPort );

        if ( bind( m_hSocket, (struct sockaddr *)&localAddr, sizeof( localAddr ) ) < 0 ) {
            CLog::Print( LOG_ERROR, "CmdpClient::Init bind error port=%d", m_iLocalPort );
            close( m_hSocket );
            m_hSocket = -1;
            return false;
        }

        m_bRecvRunning = true;
        m_threadRecv = std::thread( &CCmdpClient::RecvLoop, this );
    }

    if ( !m_bKeepAliveRunning ) {
        m_bKeepAliveRunning = true;
        m_threadKeepAlive = std::thread( &CCmdpClient::KeepAliveLoop, this );
    }

    if ( !m_bEventRunning ) {
        m_bEventRunning = true;
        m_threadEventDispatch = std::thread( &CCmdpClient::EventDispatchLoop, this );
    }

    CLog::Print( LOG_INFO, "CmdpClient: init %s:%d (local=%d)", strCmdpIp.c_str(), iCmdpPort, iLocalPort );
    return true;
}

// 이벤트 전용 dispatch 스레드 — RecvLoop 가 큐에 넣고 여기서 콜백을 부른다.
//   콜백(McDataMediaService fan-out)이 AddSendSession/RemoveSession 등 동기 요청을 수행해도
//   응답은 RecvLoop 가 정상 수신하므로 데드락이 없다.
void CCmdpClient::EventDispatchLoop() {
    while ( m_bEventRunning ) {
        SimpleJson::JsonNode clsEvent;
        {
            std::unique_lock<std::mutex> lock( m_mutexEvents );
            m_cvEvents.wait( lock, [this] { return !m_eventQueue.empty() || !m_bEventRunning; } );
            if ( !m_bEventRunning && m_eventQueue.empty() ) break;
            clsEvent = m_eventQueue.front();
            m_eventQueue.pop_front();
        }
        if ( m_fnEventCallback ) {
            m_fnEventCallback( clsEvent );
        }
    }
}

void CCmdpClient::OnTransactionComplete( unsigned int transId, bool success, const std::string &response ) {
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

bool CCmdpClient::SendRequestAndWait( const SimpleJson::JsonNode &payload, std::string &strResponse ) {
    if ( m_hSocket == -1 ) return false;

    unsigned int transId;
    std::shared_ptr<Transaction> pTrans = std::make_shared<Transaction>();
    {
        std::lock_guard<std::mutex> lock( m_mutexTrans );
        transId = m_iNextTransId++;
        pTrans->id = transId;
        m_mapTransactions[transId] = pTrans;
    }

    // envelope v2 {hdr, payload} — CmpClient 와 동일 (cmp_media_api.md §2).
    //   상관 메타(sesid)와 cmd 는 hdr 로, payload 에는 업무 필드만 남긴다.
    //   CORE 명령(HEARTBEAT/STATS)은 sesid/service 를 싣지 않는다.
    std::string strCmdName = payload.GetString( "cmd" );
    bool bCore = ( strCmdName == "HEARTBEAT" || strCmdName == "STATS" );
    SimpleJson::JsonNode hdr;
    hdr.Set( "ver", 2 );
    hdr.Set( "trans_id", (int)transId );
    hdr.Set( "node", gclsSipLogger.GetSystemId().empty() ? "csp_01" : gclsSipLogger.GetSystemId() );
    hdr.Set( "cmd", strCmdName );
    hdr.Set( "type", "request" );
    if ( !bCore ) {
        std::string strHdrSesId = payload.GetString( "sesid" );
        if ( !strHdrSesId.empty() ) hdr.Set( "sesid", strHdrSesId );
        hdr.Set( "service", "mcdata" );
    }

    SimpleJson::JsonNode body = payload;
    body.objects.erase( "cmd" );
    body.objects.erase( "sesid" );
    body.objects.erase( "service" );

    SimpleJson::JsonNode packet;
    packet.Set( "hdr", hdr );
    if ( !body.objects.empty() ) packet.Set( "payload", body );
    std::string strPacket = packet.ToString();

    if ( gclsSipLogger.IsEnabled() ) {
        std::string strTxId = std::to_string( transId );
        std::string strSesId = payload.GetString( "sesid" );
        if ( strSesId.empty() ) strSesId = payload.GetString( "session_id" );
        gclsSipLogger.LogMessage( "csp", "cmdp", "JSON", strCmdName.c_str(),
                                  ( m_strCmdpIp + ":" + std::to_string( m_iCmdpPort ) ).c_str(), strPacket.c_str(),
                                  "mcdata", strTxId.c_str(), strSesId.c_str(),
                                  payload.GetString( "session_id" ).c_str(), payload.GetString( "caller" ).c_str(),
                                  payload.GetString( "callee" ).c_str() );
    }

    struct sockaddr_in servaddr;
    memset( &servaddr, 0, sizeof( servaddr ) );
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons( m_iCmdpPort );
    servaddr.sin_addr.s_addr = inet_addr( m_strCmdpIp.c_str() );

    // CmpClient 와 동일한 UDP 견고화 — cmdp 명령은 session_id 기준 멱등이라 재전송 안전.
    const int kMaxAttempts = 3;
    const int kAttemptMs = 100;
    bool bResult = false;
    for ( int attempt = 0; attempt < kMaxAttempts && !bResult; ++attempt ) {
        if ( sendto( m_hSocket, strPacket.c_str(), strPacket.length(), 0, (struct sockaddr *)&servaddr,
                     sizeof( servaddr ) ) < 0 ) {
            CLog::Print( LOG_ERROR, "CmdpClient sendto error (TransId=%d attempt=%d)", transId, attempt );
            break;
        }

        std::unique_lock<std::mutex> lock( pTrans->mutex );
        if ( pTrans->cv.wait_for( lock, std::chrono::milliseconds( kAttemptMs ),
                                  [&pTrans] { return pTrans->bCompleted; } ) ) {
            strResponse = pTrans->strResponse;
            bResult = pTrans->bSuccess;
        }
    }

    {
        std::lock_guard<std::mutex> mapLock( m_mutexTrans );
        m_mapTransactions.erase( transId );
    }

    if ( !bResult ) {
        CLog::Print( LOG_ERROR, "CmdpClient request timeout (TransId=%d, cmd=%s)", transId,
                     payload.GetString( "cmd" ).c_str() );
    }
    return bResult;
}

bool CCmdpClient::AddRecvSession( const std::string &strSessionId, const std::string &strCaller,
                                  const std::string &strGroupId, const std::string &strRemotePath,
                                  long long llMaxSize, std::string &strMsrpPath, const std::string &strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "MSRP_ADD" );
    req.Set( "mode", "recv" );
    req.Set( "session_id", strSessionId );
    req.Set( "caller", strCaller );
    req.Set( "group_id", strGroupId );
    req.Set( "remote_path", strRemotePath );
    req.Set( "max_size", (long long)llMaxSize );
    std::string strFinalSesId = strSesId.empty() ? CSipMessageLogger::IssueSesId( strCaller, "csp" ) : strSesId;
    req.Set( "sesid", strFinalSesId );

    std::string strResp;
    if ( !SendRequestAndWait( req, strResp ) ) return false;

    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( strResp );
    if ( respNode.type == SimpleJson::JSON_OBJECT && respNode.GetString( "status" ) == "OK" ) {
        strMsrpPath = respNode.GetString( "msrp_path" );
        return !strMsrpPath.empty();
    }
    CLog::Print( LOG_ERROR, "CmdpClient::AddRecvSession failed: %s", strResp.c_str() );
    return false;
}

bool CCmdpClient::AddSendSession( const std::string &strSessionId, const std::string &strFileId,
                                  const std::string &strCaller, const std::string &strCallee,
                                  const std::string &strContentType, std::string &strMsrpPath,
                                  const std::string &strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "MSRP_ADD" );
    req.Set( "mode", "send" );
    req.Set( "session_id", strSessionId );
    req.Set( "file_id", strFileId );
    req.Set( "caller", strCaller );
    req.Set( "callee", strCallee );
    if ( !strContentType.empty() ) req.Set( "content_type", strContentType );
    std::string strFinalSesId = strSesId.empty() ? CSipMessageLogger::IssueSesId( strCaller, "csp" ) : strSesId;
    req.Set( "sesid", strFinalSesId );

    std::string strResp;
    if ( !SendRequestAndWait( req, strResp ) ) return false;

    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( strResp );
    if ( respNode.type == SimpleJson::JSON_OBJECT && respNode.GetString( "status" ) == "OK" ) {
        strMsrpPath = respNode.GetString( "msrp_path" );
        return !strMsrpPath.empty();
    }
    CLog::Print( LOG_ERROR, "CmdpClient::AddSendSession failed: %s", strResp.c_str() );
    return false;
}

bool CCmdpClient::SetRemotePath( const std::string &strSessionId, const std::string &strRemotePath ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "MSRP_MODIFY" );
    req.Set( "session_id", strSessionId );
    req.Set( "remote_path", strRemotePath );

    // 소실 세션은 NOT_FOUND — 소비자(McDataMediaService)가 실패 시 레그 정리 판단.
    std::string strResp;
    if ( !SendRequestAndWait( req, strResp ) ) return false;
    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( strResp );
    return respNode.type == SimpleJson::JSON_OBJECT && respNode.GetString( "status" ) == "OK";
}

bool CCmdpClient::RemoveSession( const std::string &strSessionId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "MSRP_REMOVE" );
    req.Set( "session_id", strSessionId );

    std::string strResp;
    if ( !SendRequestAndWait( req, strResp ) ) return false;
    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( strResp );
    return respNode.type == SimpleJson::JSON_OBJECT && respNode.GetString( "status" ) == "OK";
}

bool CCmdpClient::Alive() {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "HEARTBEAT" );

    std::string resp;
    return SendRequestAndWait( req, resp );
}

void CCmdpClient::RecvLoop() {
    char buffer[8192];
    struct sockaddr_in cliaddr;
    socklen_t len;

    while ( m_bRecvRunning ) {
        len = sizeof( cliaddr );
        int n = recvfrom( m_hSocket, buffer, sizeof( buffer ) - 1, 0, (struct sockaddr *)&cliaddr, &len );
        if ( n <= 0 ) continue;
        buffer[n] = '\0';
        std::string strPacket = buffer;

        // envelope v2 — {hdr:{trans_id,cmd,type,...}[,payload]} (응답/이벤트 공통)
        SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse( strPacket );
        if ( root.type != SimpleJson::JSON_OBJECT || root.Get( "hdr" ).type != SimpleJson::JSON_OBJECT ) {
            CLog::Print( LOG_INFO, "CmdpClient non-v2 RX: %s", strPacket.c_str() );
            continue;
        }
        SimpleJson::JsonNode hdr = root.Get( "hdr" );
        int transId = (int)hdr.GetInt( "trans_id", 0 );

        // cmdp 비동기 이벤트 — 동일 trans_id 의 v2 response 로 ack 후 콜백 dispatch
        if ( hdr.GetString( "type" ) == "event" ) {
            SimpleJson::JsonNode ackHdr;
            ackHdr.Set( "ver", 2 );
            ackHdr.Set( "trans_id", transId );
            ackHdr.Set( "node", gclsSipLogger.GetSystemId().empty() ? "csp_01" : gclsSipLogger.GetSystemId() );
            ackHdr.Set( "cmd", hdr.GetString( "cmd" ) );
            ackHdr.Set( "type", "response" );
            ackHdr.Set( "status", "OK" );
            SimpleJson::JsonNode ack;
            ack.Set( "hdr", ackHdr );
            std::string strAck = ack.ToString();
            sendto( m_hSocket, strAck.c_str(), strAck.length(), 0, (struct sockaddr *)&cliaddr, len );

            if ( gclsSipLogger.IsEnabled() ) {
                gclsSipLogger.LogMessage( "cmdp", "csp", "JSON", hdr.GetString( "cmd" ).c_str(),
                                          ( m_strCmdpIp + ":" + std::to_string( m_iCmdpPort ) ).c_str(),
                                          strPacket.c_str(), "mcdata", std::to_string( transId ).c_str(),
                                          hdr.GetString( "sesid" ).c_str(), "", "", "" );
            }
            {
                std::lock_guard<std::mutex> lock( m_mutexEvents );
                m_eventQueue.push_back( root );
            }
            m_cvEvents.notify_one();
            continue;
        }

        // 응답 — 소비자 파싱 호환을 위해 hdr 의 status/code/reason 을 payload 에 병합 (CmpClient 와 동일)
        SimpleJson::JsonNode body = root.Get( "payload" );
        if ( body.type != SimpleJson::JSON_OBJECT ) {
            body = SimpleJson::JsonNode();
            body.type = SimpleJson::JSON_OBJECT;
        }
        body.Set( "status", hdr.GetString( "status", "ERROR" ) );
        if ( hdr.Has( "code" ) ) body.Set( "code", hdr.GetString( "code" ) );
        if ( hdr.Has( "reason" ) ) body.Set( "reason", hdr.GetString( "reason" ) );
        OnTransactionComplete( transId, true, body.ToString() );
    }
}

void CCmdpClient::KeepAliveLoop() {
    while ( m_bKeepAliveRunning ) {
        bool bSuccess = Alive();

        // CmpClient 와 동일: 연속 임계 실패에서만 disconnect (일시적 UDP 유실 과민 방지)
        const int kMaxAliveFail = 3;
        if ( bSuccess ) {
            m_iAliveFailCount = 0;
            if ( !m_bConnected ) {
                m_bConnected = true;
                if ( m_fnConnectionCallback ) m_fnConnectionCallback( true );
                CLog::Print( LOG_INFO, "CMDP Connected" );
            }
        } else {
            if ( m_bConnected ) {
                ++m_iAliveFailCount;
                if ( m_iAliveFailCount >= kMaxAliveFail ) {
                    m_bConnected = false;
                    m_iAliveFailCount = 0;
                    if ( m_fnConnectionCallback ) m_fnConnectionCallback( false );
                    CLog::Print( LOG_INFO, "CMDP Disconnected (연속 %d회 HEARTBEAT 실패)", kMaxAliveFail );
                }
            }
        }

        std::this_thread::sleep_for( std::chrono::seconds( 3 ) );
    }
}
