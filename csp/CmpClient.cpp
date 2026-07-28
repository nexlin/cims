#include "CmpClient.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "CallMap.h"
#include "GroupCallService.h"
#include "Log.h"
#include "SimpleJson.h"
#include "SipMessageLogger.h"

// VoIP relay 세션 식별자 발행 (재시작 경계 포함 전역 유일 — CMP 잔존 고아와 충돌 불가).
std::string CCmpClient::IssueSessionId() {
    return CSipMessageLogger::IssueUniqueId( "csp" );
}

// trans_id 초기값 — 부팅 시각(ms) 하위 비트 시드. 고정 로컬포트 bind 라 재기동 직후
//   구 프로세스 앞으로 온 지연 응답 datagram 이 새 트랜잭션과 오매칭되는 창을 제거 (TCP ISN 관례).
static unsigned int SeedTransId() {
    struct timeval tv;
    gettimeofday( &tv, NULL );
    return (unsigned int)( ( (unsigned long long)tv.tv_sec * 1000ULL + tv.tv_usec / 1000 ) & 0x3FFFFFFF ) | 1;
}

CCmpClient::CCmpClient()
    : m_iCmpPort( 0 ),
      m_hSocket( -1 ),
      m_iNextTransId( SeedTransId() ),
      m_bKeepAliveRunning( false ),
      m_bRecvRunning( false ),
      m_bEventRunning( false ),
      m_bConnected( false ),
      m_iAliveFailCount( 0 ),
      m_ring( 128 ) {
}

CCmpClient::~CCmpClient() {
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

CCmpClient &CCmpClient::GetInstance() {
    static CCmpClient instance;
    return instance;
}

bool CCmpClient::Init( const std::string &strCmpIp, int iCmpPort, int iLocalPort ) {
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
        for ( const auto &ep : m_endpoints ) {
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

        // 단일 control 소켓의 송수신 버퍼 상향 — 4cps+ 버스트에서 CMP 응답/요청 datagram 이
        //   기본 버퍼(~212KB) overflow 로 드롭되어 SendRequestAndWait 가 거짓 타임아웃나던 문제 완화.
        //   값은 net.core.{r,w}mem_max(여기 4MB) 로 캡됨. 재전송과 함께 UDP 유실 견고화.
        {
            int iBuf = 4 * 1024 * 1024;
            if ( setsockopt( m_hSocket, SOL_SOCKET, SO_RCVBUF, &iBuf, sizeof( iBuf ) ) < 0 )
                CLog::Print( LOG_ERROR, "CmpClient: SO_RCVBUF set failed" );
            if ( setsockopt( m_hSocket, SOL_SOCKET, SO_SNDBUF, &iBuf, sizeof( iBuf ) ) < 0 )
                CLog::Print( LOG_ERROR, "CmpClient: SO_SNDBUF set failed" );
            int iActual = 0;
            socklen_t optlen = sizeof( iActual );
            if ( getsockopt( m_hSocket, SOL_SOCKET, SO_RCVBUF, &iActual, &optlen ) == 0 )
                CLog::Print( LOG_INFO, "CmpClient: SO_RCVBUF=%d (req=%d)", iActual, iBuf );
        }

        struct sockaddr_in localAddr;
        memset( &localAddr, 0, sizeof( localAddr ) );
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = htonl( INADDR_ANY );
        localAddr.sin_port = htons( m_iLocalCmpPort );

        if ( bind( m_hSocket, (struct sockaddr *)&localAddr, sizeof( localAddr ) ) < 0 ) {
            CLog::Print( LOG_ERROR, "CmpClient::Init bind error port=%d", m_iLocalCmpPort );
            close( m_hSocket );
            m_hSocket = -1;
            return false;
        }

        // Start Recv Thread — 단일 수신 스레드(trans_id demux). 비동기 로깅 후 µs 급 처리라 충분.
        m_bRecvRunning = true;
        m_threadRecv = std::thread( &CCmpClient::RecvLoop, this );

        // 이벤트 dispatch 스레드 — RELAY_ABORTED/PTT_GROUP_ABORTED 핸들러가 RemoveSession 을
        //   부를 수 있으므로 RecvLoop 와 분리 (데드락 회피).
        m_bEventRunning = true;
        m_threadEventDispatch = std::thread( &CCmpClient::EventDispatchLoop, this );
    }

    if ( !m_bKeepAliveRunning ) {
        m_bKeepAliveRunning = true;
        m_threadKeepAlive = std::thread( &CCmpClient::KeepAliveLoop, this );
    }

    return true;
}

// Helper to clean up transaction
void CCmpClient::OnTransactionComplete( unsigned int transId, bool success, const std::string &response ) {
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

// Phase 1.E-2 wrapper — session sticky → endpoint 선택 후 _SendOnEndpoint 호출.
bool CCmpClient::SendRequestAndWait( const std::string &strSessionKey, const SimpleJson::JsonNode &payload,
                                     std::string &strResponse ) {
    CmpEndpoint ep = _ResolveEndpoint( strSessionKey );
    return _SendOnEndpoint( ep, payload, strResponse );
}

// Legacy / heartbeat — primary endpoint
bool CCmpClient::SendRequestAndWait( const SimpleJson::JsonNode &payload, std::string &strResponse ) {
    CmpEndpoint ep = _ResolveEndpoint( "" );
    return _SendOnEndpoint( ep, payload, strResponse );
}

void CCmpClient::ReleaseEndpointForKey( const std::string &strSessionKey ) {
    if ( strSessionKey.empty() ) return;
    std::lock_guard<std::mutex> lock( m_mutexEndpoints );
    m_mapSessionToEndpointKey.erase( strSessionKey );
}

CmpEndpoint CCmpClient::_ResolveEndpoint( const std::string &strSessionKey ) {
    std::lock_guard<std::mutex> lock( m_mutexEndpoints );
    if ( m_endpoints.empty() ) {
        return CmpEndpoint( m_strCmpIp, m_iCmpPort );  // bootstrap fallback
    }
    if ( !strSessionKey.empty() ) {
        // 1) 캐시 hit
        auto it = m_mapSessionToEndpointKey.find( strSessionKey );
        if ( it != m_mapSessionToEndpointKey.end() ) {
            for ( const auto &ep : m_endpoints ) {
                if ( ep.strKey == it->second ) return ep;
            }
            // stale cache — fall through to ring
        }
        // 2) Ring select
        std::string selectedKey;
        if ( m_ring.Select( strSessionKey, selectedKey ) ) {
            for ( const auto &ep : m_endpoints ) {
                if ( ep.strKey == selectedKey ) {
                    m_mapSessionToEndpointKey[strSessionKey] = ep.strKey;
                    return ep;
                }
            }
        }
    }
    return m_endpoints.front();  // primary
}

bool CCmpClient::_SendOnEndpoint( const CmpEndpoint &ep, const SimpleJson::JsonNode &payload,
                                  std::string &strResponse ) {
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
            else if ( cmd.find( "RELAY" ) != std::string::npos )
                strSvc = "volte";
            else
                strSvc = "system";
        }
        pTrans->strService = strSvc;
        m_mapTransactions[transId] = pTrans;
    }

    // Construct Packet — envelope v2 {hdr, payload} (docs/api/cmp_media_api.md).
    //   상관 메타(sesid/service)와 cmd 는 hdr 로, payload 에는 업무 필드만 남긴다.
    //   CORE 명령(HEARTBEAT/STATS)은 sesid/service 를 싣지 않는다.
    std::string strCmdName = payload.GetString( "cmd" );
    bool bCore = ( strCmdName == "HEARTBEAT" || strCmdName == "STATS" );
    // HEARTBEAT 로그 샘플링 — N 회당 1회만 msg/flow 기록 (응답은 Transaction.bLogMsg 로 동기화).
    //   KeepAliveLoop 단일 스레드가 유일한 HEARTBEAT 송신자라 카운터 lock 불필요.
    if ( strCmdName == "HEARTBEAT" ) pTrans->bLogMsg = ( m_iHbLogCount++ % kHbLogSampleN ) == 0;
    SimpleJson::JsonNode hdr;
    hdr.Set( "ver", 2 );
    hdr.Set( "trans_id", (int)transId );
    hdr.Set( "node", gclsSipLogger.GetSystemId().empty() ? "csp_01" : gclsSipLogger.GetSystemId() );
    hdr.Set( "cmd", strCmdName );
    hdr.Set( "type", "request" );
    if ( !bCore ) {
        std::string strHdrSesId = payload.GetString( "sesid" );
        if ( !strHdrSesId.empty() ) hdr.Set( "sesid", strHdrSesId );
        if ( !pTrans->strService.empty() ) hdr.Set( "service", pTrans->strService );
    }

    SimpleJson::JsonNode body = payload;
    body.objects.erase( "cmd" );
    body.objects.erase( "sesid" );
    body.objects.erase( "service" );

    SimpleJson::JsonNode packet;
    packet.Set( "hdr", hdr );
    if ( !body.objects.empty() ) packet.Set( "payload", body );

    std::string strPacket = packet.ToString();
    // UDP JSON 계약 최대 4KB (cmp_media_api.md §1.2) — 초과 datagram 은 CMP 수신 버퍼(4096)에서
    //   절단되어 파싱 실패가 확정이므로 조기 실패한다 (재전송 낭비 방지). 주 원인은 대형 PTT
    //   로스터 — 초기 로스터를 줄이고 나머지는 PTT_JOIN 2단으로 합류시킨다 (§7.4).
    if ( strPacket.length() > 4096 ) {
        CLog::Print( LOG_ERROR, "CmpClient: packet too large (%zu > 4096) cmd=%s — 송신 중단 (로스터 축소 필요)",
                     strPacket.length(), strCmdName.c_str() );
        std::lock_guard<std::mutex> mapLock( m_mutexTrans );
        m_mapTransactions.erase( transId );
        return false;
    }
    CLog::Print( LOG_DEBUG, "CmpClient TX → %s:%d : %s", ep.strIp.c_str(), ep.iPort, strPacket.c_str() );
    if ( gclsSipLogger.IsEnabled() && pTrans->bLogMsg ) {
        std::string strCmd = payload.GetString( "cmd" );
        // service: Transaction 에 저장된 값 (payload.service 기반)
        const char *pszSvc = pTrans->strService.empty() ? "system" : pTrans->strService.c_str();
        std::string strTxId = std::to_string( transId );
        std::string strSesId = payload.GetString( "sesid" );
        if ( strSesId.empty() ) strSesId = payload.GetString( "session_id" );
        if ( strSesId.empty() ) strSesId = payload.GetString( "group_id" );

        // detail: method별 결정
        std::string strDetail;
        std::string caller = payload.GetString( "caller" );
        std::string callee = payload.GetString( "callee" );
        int iPeerIdx = (int)payload.GetInt( "peer_index", -1 );
        if ( strCmd == "PTT_GROUP_ADD" || strCmd == "PTT_GROUP_REMOVE" ) {
            strDetail = payload.GetString( "group_id" );
        } else if ( strCmd == "PTT_JOIN" || strCmd == "PTT_LEAVE" ) {
            strDetail = payload.GetString( "session_id" );
        } else if ( strCmd == "RELAY_ADD" ) {
            if ( !caller.empty() && !callee.empty() )
                strDetail = caller + "\xe2\x86\x92" + callee;
            else if ( !caller.empty() )
                strDetail = caller;
            else
                strDetail = payload.GetString( "session_id" );
        } else if ( strCmd == "RELAY_MODIFY" ) {
            if ( iPeerIdx == 1 && !callee.empty() )
                strDetail = callee;
            else if ( iPeerIdx == 0 && !caller.empty() )
                strDetail = caller;
            else if ( !caller.empty() && !callee.empty() )
                strDetail = caller + "\xe2\x86\x92" + callee;
        } else if ( strCmd == "RELAY_REMOVE" ) {
            if ( !caller.empty() && !callee.empty() )
                strDetail = caller + "\xe2\x86\x92" + callee;
            else
                strDetail = payload.GetString( "session_id" );
        }

        gclsSipLogger.LogMessage( "csp", "cmp", "JSON", strCmd.c_str(),
                                  ( ep.strIp + ":" + std::to_string( ep.iPort ) ).c_str(), strPacket.c_str(), pszSvc,
                                  strTxId.c_str(), strSesId.c_str(), strDetail.c_str(), caller.c_str(),
                                  callee.c_str() );
    }

    struct sockaddr_in servaddr;
    memset( &servaddr, 0, sizeof( servaddr ) );
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons( ep.iPort );
    servaddr.sin_addr.s_addr = inet_addr( ep.strIp.c_str() );

    // UDP control plane 견고화 — 재전송. CMP 요청/응답 datagram 이 부하 시 드롭되면(단일 소켓)
    //   재전송 없이는 단발 유실이 곧 2초 타임아웃→호 실패였다. CMP 명령은 session_id 기준 멱등
    //   (processAdd: 기존 세션이면 재할당 없이 동일 포트 반환; MODIFY/REMOVE 동일) 이므로 같은
    //   trans_id+payload 재전송이 안전(중복 relay/누수 없음). 총 대기 ceiling(2s)은 기존과 동일.
    // CMP 정상 응답은 5~25ms 관측 → 100ms 는 4~20배 마진(정상 응답엔 헛발동 없음).
    //   유실 시 100ms 내 재시도, 3회=총 300ms ceiling 로 빠르게 복구 or 실패 판정.
    const int kMaxAttempts = 3;
    const int kAttemptMs = 100;
    bool bResult = false;
    bool bSendFail = false;
    for ( int attempt = 0; attempt < kMaxAttempts && !bResult; ++attempt ) {
        if ( sendto( m_hSocket, strPacket.c_str(), strPacket.length(), 0, (struct sockaddr *)&servaddr,
                     sizeof( servaddr ) ) < 0 ) {
            CLog::Print( LOG_ERROR, "SendRequestAndWait sendto error (TransId=%d attempt=%d)", transId, attempt );
            bSendFail = true;
            break;
        }
        if ( attempt > 0 ) CLog::Print( LOG_INFO, "CmpClient retransmit (TransId=%d attempt=%d)", transId, attempt );

        std::unique_lock<std::mutex> lock( pTrans->mutex );
        // ★ 술어(predicate) 있는 wait_for 필수 — CMP 응답(~25ms)이 sendto 직후 sender 가
        //   wait_for 에 진입하기 '전에' 도착하면 RecvLoop 의 notify_one 이 대기자 없이 유실되어,
        //   술어 없는 wait 는 이미 완료된 트랜잭션을 끝까지 기다린다(lost-wakeup → 거짓 타임아웃).
        //   bCompleted 를 술어로 검사하면 notify 선행/spurious wakeup 모두 안전.
        if ( pTrans->cv.wait_for( lock, std::chrono::milliseconds( kAttemptMs ),
                                  [&pTrans] { return pTrans->bCompleted; } ) ) {
            strResponse = pTrans->strResponse;
            bResult = pTrans->bSuccess;
            CLog::Print( LOG_DEBUG, "CmpClient RX ← %s", strResponse.c_str() );
        }
    }
    if ( !bResult && !bSendFail ) {
        CLog::Print( LOG_ERROR, "SendRequestAndWait Timeout (TransId=%d, %d attempts)", transId, kMaxAttempts );
    }

    // Safe Cleanup: Remove from map.
    // If RecvLoop holds a shared_ptr copy, the object won't be deleted yet.
    {
        std::lock_guard<std::mutex> mapLock( m_mutexTrans );
        m_mapTransactions.erase( transId );
    }

    return bResult;
}

std::string CCmpClient::GetSesIdByKey( const std::string &strKey ) {
    std::lock_guard<std::mutex> lock( m_mutexSesid );
    auto it = m_mapKeyToSesid.find( strKey );
    if ( it != m_mapKeyToSesid.end() ) return it->second;
    return "";
}

// ── Phase 1.E — multi-endpoint dispatch ────────────────────────────────────
void CCmpClient::AddEndpoint( const std::string &strIp, int iPort ) {
    CmpEndpoint ep( strIp, iPort );
    std::lock_guard<std::mutex> lock( m_mutexEndpoints );
    for ( const auto &existing : m_endpoints ) {
        if ( existing.strKey == ep.strKey ) return;  // 중복 무시
    }
    m_endpoints.push_back( ep );
    m_ring.AddNode( ep.strKey );
    CLog::Print( LOG_INFO, "CmpClient::AddEndpoint registered (key=%s, total=%zu)", ep.strKey.c_str(),
                 m_endpoints.size() );
}

CmpEndpoint CCmpClient::SelectEndpointForSession( const std::string &strSessionId ) {
    std::lock_guard<std::mutex> lock( m_mutexEndpoints );

    // 기존 세션 → 캐시된 endpoint 우선 (Modify/Remove 가 같은 CMP 로 가야)
    auto it = m_mapSessionToEndpointKey.find( strSessionId );
    if ( it != m_mapSessionToEndpointKey.end() ) {
        for ( const auto &ep : m_endpoints ) {
            if ( ep.strKey == it->second ) return ep;
        }
        // 캐시된 endpoint 가 사라진 경우 → 다시 선택 (아래로 fall-through)
        m_mapSessionToEndpointKey.erase( it );
    }

    // 신규 세션 → ring 으로 선택
    std::string selectedKey;
    if ( !strSessionId.empty() && m_ring.Select( strSessionId, selectedKey ) ) {
        for ( const auto &ep : m_endpoints ) {
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

bool CCmpClient::AddSession( const std::string &strSessionId, std::string &strLocalIp, int &iLocalPort,
                             int &iLocalVideoPort, int &iLocalPortB, int &iLocalVideoPortB,
                             const std::string &strRecordDir, const std::string &strCaller,
                             const std::string &strCallee, const std::string &strRmtIp, int iRmtPort,
                             int iRmtVideoPort, const std::string &strSesId, int iRemoteNat,
                             const std::string &strRemoteSigIp ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "RELAY_ADD" );
    req.Set( "session_id", strSessionId );
    req.Set( "remote_ip", strRmtIp.empty() ? "0.0.0.0" : strRmtIp );
    req.Set( "remote_port", iRmtPort );
    req.Set( "remote_video_port", iRmtVideoPort );
    req.Set( "peer_index", 0 );
    if ( iRemoteNat ) {
        req.Set( "remote_nat", 1 );
        if ( !strRemoteSigIp.empty() ) req.Set( "remote_sig_ip", strRemoteSigIp );
    }
    if ( !strRecordDir.empty() ) req.Set( "record_dir", strRecordDir );
    if ( !strCaller.empty() ) req.Set( "caller", strCaller );
    if ( !strCallee.empty() ) req.Set( "callee", strCallee );

    // sesid: 호출자 제공 > 발행
    std::string strFinalSesId = strSesId.empty() ? CSipMessageLogger::IssueSesId( strCaller, "csp" ) : strSesId;
    req.Set( "sesid", strFinalSesId );
    {
        std::lock_guard<std::mutex> lock( m_mutexSesid );
        m_mapKeyToSesid[strSessionId] = strFinalSesId;
    }

    std::string strResp;

    CLog::Print( LOG_DEBUG, "CmpClient::AddSession: %s", req.ToString().c_str() );

    std::string strReqBody = req.ToString();

    if ( !SendRequestAndWait( strSessionId, req, strResp ) ) return false;

    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( strResp );
    if ( respNode.type != SimpleJson::JSON_OBJECT ) {
        CLog::Print( LOG_ERROR, "CmpClient::AddSession: Failed to parse JSON response: %s", strResp.c_str() );
        return false;
    }

    if ( respNode.Has( "status" ) && respNode.Get( "status" ).AsString() == "OK" ) {
        strLocalIp = respNode.Get( "local_ip" ).AsString();
        iLocalPort = respNode.Get( "local_port" ).AsInt();
        iLocalVideoPort = respNode.Get( "local_video_port" ).AsInt();
        iLocalPortB = respNode.Get( "local_port_b" ).AsInt();
        iLocalVideoPortB = respNode.Get( "local_video_port_b" ).AsInt();
        return true;
    }

    return false;
}

bool CCmpClient::ModifySession( const std::string &strSessionId, const std::string &strRmtIp, int iRmtPort,
                                int iRmtVideoPort, int iPeerIdx, const std::string &strCaller,
                                const std::string &strCallee, const std::string &strSesId, int iRemoteNat,
                                const std::string &strRemoteSigIp ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "RELAY_MODIFY" );
    req.Set( "session_id", strSessionId );
    req.Set( "remote_ip", strRmtIp );
    req.Set( "remote_port", iRmtPort );
    req.Set( "remote_video_port", iRmtVideoPort );
    req.Set( "peer_index", iPeerIdx );
    if ( iRemoteNat ) {
        req.Set( "remote_nat", 1 );
        if ( !strRemoteSigIp.empty() ) req.Set( "remote_sig_ip", strRemoteSigIp );
    }
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

    std::string strResp;
    std::string strReqBody = req.ToString();

    if ( !SendRequestAndWait( strSessionId, req, strResp ) ) return false;

    // 응답 도착 != 성공 (RecvLoop 는 status 를 payload 에 병합만 한다) — ERROR(NOT_FOUND 등)를
    //   성공으로 오인하면 CMP 원격 주소가 갱신되지 않은 채 호가 진행된다.
    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( strResp );
    if ( respNode.type != SimpleJson::JSON_OBJECT ) {
        CLog::Print( LOG_ERROR, "CmpClient::ModifySession: Failed to parse JSON response: %s", strResp.c_str() );
        return false;
    }
    if ( !respNode.Has( "status" ) || respNode.Get( "status" ).AsString() != "OK" ) {
        CLog::Print( LOG_ERROR, "CmpClient::ModifySession: session=%s peer=%d ERROR: %s", strSessionId.c_str(),
                     iPeerIdx, strResp.c_str() );
        return false;
    }
    return true;
}

bool CCmpClient::RemoveSession( const std::string &strSessionId, const std::string &strCaller,
                                const std::string &strCallee, const std::string &strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "RELAY_REMOVE" );
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

    std::string strResp;
    CLog::Print( LOG_DEBUG, "CmpClient::RemoveSession: %s", req.ToString().c_str() );
    std::string strReqBody = req.ToString();
    bool bRet = SendRequestAndWait( strSessionId, req, strResp );
    ReleaseEndpointForKey( strSessionId );  // remove 후 캐시 해제
    return bRet;
}

bool CCmpClient::AddGroup( const std::string &strGroupId, const std::vector<std::shared_ptr<CspPttUser>> &vecMembers,
                           std::string &strIp, int &iFloorPort,
                           std::map<std::string, std::pair<int, int>> &mapMemberPorts, const std::string &strRecordDir,
                           bool bVideoEnabled, int iSessionSeq, const std::string &strSesId,
                           const std::string &strGroupType, const std::string &strInitiator ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "PTT_GROUP_ADD" );
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
    if ( !strRecordDir.empty() ) req.Set( "record_dir", strRecordDir );
    if ( bVideoEnabled ) req.Set( "video_enabled", 1 );
    // group_type / initiator — broadcast 그룹 floor 독점(TS 24.380 §10.3) 판정용.
    //   broadcast: 개시자(initiator)만 floor 보유, 타 멤버 REQUEST 는 CMP 가 REJECT.
    if ( !strGroupType.empty() ) req.Set( "group_type", strGroupType );
    if ( !strInitiator.empty() ) req.Set( "initiator_id", strInitiator );

    std::stringstream ssMembers;
    for ( size_t i = 0; i < vecMembers.size(); ++i ) {
        if ( !vecMembers[i] ) continue;
        if ( i > 0 ) ssMembers << ",";
        // 형식: id:priority:role (role 은 chair/participant — CMP floor 선점 판정용)
        ssMembers << vecMembers[i]->_id << ":" << vecMembers[i]->_priority << ":" << vecMembers[i]->_role;
    }
    req.Set( "members", ssMembers.str() );

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
            iFloorPort = respNode.Has( "floor_port" ) ? respNode.Get( "floor_port" ).AsInt() : 0;
            // member_ports: { "<sid>": { "port": N, "video_port": N }, ... } — 멤버별 전용 포트
            mapMemberPorts.clear();
            if ( respNode.Has( "member_ports" ) ) {
                SimpleJson::JsonNode mp = respNode.Get( "member_ports" );
                for ( const auto &kv : mp.objects ) {
                    int iAudio = (int)kv.second.GetInt( "port" );
                    int iVideo = (int)kv.second.GetInt( "video_port" );
                    if ( iAudio > 0 ) mapMemberPorts[kv.first] = { iAudio, iVideo };
                }
            }
            CLog::Print( LOG_INFO, "CmpClient::AddGroup Success: %s floor=%d member_ports=%d Members: %d",
                         strIp.c_str(), iFloorPort, (int)mapMemberPorts.size(), (int)vecMembers.size() );
            return true;
        }
        CLog::Print( LOG_ERROR, "CmpClient::AddGroup Fail: Status not OK. Resp: %s", strResp.c_str() );
    } else {
        CLog::Print( LOG_ERROR, "CmpClient::AddGroup SendRequest Failed" );
    }
    return false;
}

bool CCmpClient::ModifyGroup( const std::string &strGroupId, const std::vector<std::shared_ptr<CspPttUser>> &vecMembers,
                              const std::string &strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "PTT_GROUP_MODIFY" );
    req.Set( "group_id", strGroupId );

    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strGroupId ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );

    std::stringstream ssMembers;
    for ( size_t i = 0; i < vecMembers.size(); ++i ) {
        if ( !vecMembers[i] ) continue;
        if ( i > 0 ) ssMembers << ",";
        ssMembers << vecMembers[i]->_id << ":" << vecMembers[i]->_priority << ":" << vecMembers[i]->_role;
    }
    req.Set( "members", ssMembers.str() );

    std::string strResp;
    if ( !SendRequestAndWait( strGroupId, req, strResp ) ) return false;

    // NOT_FOUND(그룹 소실 — CMP 는 MODIFY 로 그룹을 재생성하지 않음)·NO_RESOURCE(멤버 pool 고갈)를
    //   성공으로 오인하지 않는다. 호출자(SyncGroupsState)는 실패 시 AddGroup 으로 재수립한다.
    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( strResp );
    if ( respNode.type != SimpleJson::JSON_OBJECT ) {
        CLog::Print( LOG_ERROR, "CmpClient::ModifyGroup: Failed to parse JSON response: %s", strResp.c_str() );
        return false;
    }
    if ( !respNode.Has( "status" ) || respNode.Get( "status" ).AsString() != "OK" ) {
        CLog::Print( LOG_ERROR, "CmpClient::ModifyGroup: group=%s ERROR: %s", strGroupId.c_str(), strResp.c_str() );
        return false;
    }
    return true;
}

bool CCmpClient::SetFloorTier( const std::string &strGroupId, const std::string &strSessionId, int iTier,
                               const std::string &strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "PTT_FLOOR_TIER" );
    req.Set( "group_id", strGroupId );
    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strGroupId ) : strSesId;
    if ( !strFinalSesId.empty() ) req.Set( "sesid", strFinalSesId );
    req.Set( "session_id", strSessionId );
    req.Set( "tier", iTier >= 2 ? "emergency" : ( iTier == 1 ? "imminent" : "normal" ) );
    std::string resp;
    return SendRequestAndWait( strGroupId, req, resp );
}

bool CCmpClient::JoinGroup( const std::string &strGroupId, const std::string &strSessionId,
                            const std::string &strUserIp, int iUserPort, int iFloorPort, int iVideoPort,
                            const std::string &strSesId, const std::string &strRole, int *piLocalPort,
                            int *piLocalVideoPort, int iUserNat, const std::string &strUserSigIp,
                            int iUserPt, int iUserSrcPt, int iUserTePt, int iUserSrcTePt ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "PTT_JOIN" );
    req.Set( "group_id", strGroupId );

    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strGroupId ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );

    req.Set( "session_id", strSessionId );
    // 2단 멱등: 주소 없이 호출(①)이면 user_ip/port 를 싣지 않는다 — CMP 는 멤버 전용 포트만 선할당.
    if ( !strUserIp.empty() && iUserPort > 0 ) {
        req.Set( "user_ip", strUserIp );
        req.Set( "user_port", iUserPort );
        if ( iFloorPort > 0 ) req.Set( "user_floor_port", iFloorPort );
        if ( iVideoPort > 0 ) req.Set( "user_video_port", iVideoPort );
        req.Set( "role", strRole.empty() ? "participant" : strRole );
        if ( iUserNat ) {
            req.Set( "user_nat", 1 );
            if ( !strUserSigIp.empty() ) req.Set( "user_sig_ip", strUserSigIp );
        }
        // leg 별 PT 재작성 (cmp_media_api.md §7.4, optional — 생략=재작성 없음).
        if ( iUserPt > 0 ) req.Set( "user_pt", iUserPt );
        if ( iUserSrcPt > 0 ) req.Set( "user_src_pt", iUserSrcPt );
        if ( iUserTePt > 0 ) req.Set( "user_te_pt", iUserTePt );
        if ( iUserSrcTePt > 0 ) req.Set( "user_src_te_pt", iUserSrcTePt );
    }

    std::string resp;
    std::string strReqBody = req.ToString();
    if ( !SendRequestAndWait( strGroupId, req, resp ) ) return false;

    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse( resp );
    if ( respNode.type != SimpleJson::JSON_OBJECT ) return false;
    if ( !respNode.Has( "status" ) || respNode.Get( "status" ).AsString() != "OK" ) return false;
    if ( piLocalPort ) *piLocalPort = respNode.Has( "port" ) ? respNode.Get( "port" ).AsInt() : 0;
    if ( piLocalVideoPort ) *piLocalVideoPort = respNode.Has( "video_port" ) ? respNode.Get( "video_port" ).AsInt() : 0;
    return true;
}

bool CCmpClient::LeaveGroup( const std::string &strGroupId, const std::string &strSessionId,
                             const std::string &strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "PTT_LEAVE" );
    req.Set( "group_id", strGroupId );

    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strGroupId ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );

    req.Set( "session_id", strSessionId );

    std::string resp;
    std::string strReqBody = req.ToString();
    bool bRet = SendRequestAndWait( strGroupId, req, resp );
    return bRet;
}

bool CCmpClient::RemoveGroup( const std::string &strGroupId, const std::string &strSesId ) {
    SimpleJson::JsonNode req;
    req.Set( "cmd", "PTT_GROUP_REMOVE" );
    req.Set( "group_id", strGroupId );

    std::string strFinalSesId = strSesId.empty() ? GetSesIdByKey( strGroupId ) : strSesId;
    if ( strFinalSesId.empty() ) strFinalSesId = CSipMessageLogger::IssueSesId( "", "csp" );
    req.Set( "sesid", strFinalSesId );
    {
        std::lock_guard<std::mutex> lock( m_mutexSesid );
        m_mapKeyToSesid.erase( strGroupId );
    }

    std::string strResp;
    bool bRet = SendRequestAndWait( strGroupId, req, strResp );
    ReleaseEndpointForKey( strGroupId );  // remove 후 캐시 해제
    return bRet;
}

bool CCmpClient::Alive() {
    // 모든 endpoint 에 HEARTBEAT — endpoint 별 세션집합 지문(relay)을 stash 한다(per-shard audit).
    //   각 CMP 는 이 HEARTBEAT 소스로 이벤트 push 대상(CSP 소켓)을 학습한다(RELAY_ABORTED 라우팅).
    std::vector<CmpEndpoint> eps;
    {
        std::lock_guard<std::mutex> lock( m_mutexEndpoints );
        eps = m_endpoints;
    }
    if ( eps.empty() ) return false;

    bool bPrimaryOk = false;
    for ( size_t i = 0; i < eps.size(); ++i ) {
        SimpleJson::JsonNode req;
        req.Set( "cmd", "HEARTBEAT" );
        // CORE 명령 — wire(hdr)에는 sesid 미포함. CSP 내부 flow 로그 쌍 맞춤용으로만 발행.
        req.Set( "sesid", CSipMessageLogger::IssueSesId( "", "csp" ) );

        std::string resp;
        bool bOk = _SendOnEndpoint( eps[i], req, resp );
        if ( i == 0 ) bPrimaryOk = bOk;  // primary = Init 가 먼저 등록한 endpoints[0]

        std::lock_guard<std::mutex> lock( m_mutexDigest );
        CmpDigest &dg = m_mapEndpointDigest[eps[i].strKey];
        if ( bOk ) {
            SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse( resp );
            SimpleJson::JsonNode sd = root.Get( "session_digest" );
            if ( sd.type == SimpleJson::JSON_OBJECT ) {
                SimpleJson::JsonNode r = sd.Get( "relay" );
                dg.iCount = (int)r.GetInt( "count", 0 );
                dg.uHash = strtoull( r.GetString( "hash" ).c_str(), NULL, 16 );
                dg.bValid = true;
            }
        } else {
            dg.bValid = false;  // 무응답 endpoint 는 이번 cycle audit 제외
        }
    }
    return bPrimaryOk;
}

uint64_t CCmpClient::Fnv1a64( const std::string &s ) {
    uint64_t h = 1469598103934665603ULL;
    for ( unsigned char c : s ) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

void CCmpClient::SetAuditConfig( bool bEnable, int iGraceSec, int iMaxPerCycle, bool bZombieTeardown,
                                 const std::string &strHaRole, const std::string &strHaVip ) {
    m_bAuditEnable = bEnable;
    m_iAuditGraceSec = ( iGraceSec >= 0 ) ? iGraceSec : 30;
    m_iAuditMaxPerCycle = ( iMaxPerCycle > 0 ) ? iMaxPerCycle : 20;
    m_bAuditZombieTeardown = bZombieTeardown;
    m_strHaRole = strHaRole.empty() ? "auto" : strHaRole;
    m_strHaVip = strHaVip;
    ResolveActiveRole();  // 초기 역할 확정 (이후 매 audit cycle 재평가)
    CLog::Print( LOG_INFO,
                 "CmpClient audit: enable=%d grace=%ds max=%d zombie_teardown=%d role=%s vip=%s(active=%d)",
                 m_bAuditEnable, m_iAuditGraceSec, m_iAuditMaxPerCycle, m_bAuditZombieTeardown,
                 m_strHaRole.c_str(), m_strHaVip.empty() ? "-" : m_strHaVip.c_str(), m_bAuditActiveRole.load() );
}

// 로컬 인터페이스 중 strIp 를 소유한 게 있는지 (getifaddrs). VIP 소유 = 이 노드가 active.
bool CCmpClient::OwnsLocalIp( const std::string &strIp ) {
    if ( strIp.empty() ) return false;
    struct ifaddrs *pIfAddr = NULL;
    if ( getifaddrs( &pIfAddr ) != 0 ) return false;
    bool bOwned = false;
    for ( struct ifaddrs *ifa = pIfAddr; ifa != NULL; ifa = ifa->ifa_next ) {
        if ( ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET ) continue;
        char buf[INET_ADDRSTRLEN] = { 0 };
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        if ( inet_ntop( AF_INET, &sin->sin_addr, buf, sizeof( buf ) ) && strIp == buf ) {
            bOwned = true;
            break;
        }
    }
    freeifaddrs( pIfAddr );
    return bOwned;
}

// HaRole 동적 판정 — 매 audit cycle 및 설정 시 호출. auto+HaVip 이면 VIP 소유로 active/standby 결정.
//   hot-standby 가 승격(VIP 인수)하면 별도 훅 없이 다음 cycle(≤3s)에 active 로 전환된다.
void CCmpClient::ResolveActiveRole() {
    bool bActive;
    if ( m_strHaRole == "active" ) {
        bActive = true;
    } else if ( m_strHaRole == "standby" ) {
        bActive = false;
    } else {  // "auto"
        // HaVip 설정 시 VIP 소유로 판정(hot-standby 동적 전환). 미설정 시 active(단일노드/cold-spare — 부팅=active).
        bActive = m_strHaVip.empty() ? true : OwnsLocalIp( m_strHaVip );
    }
    m_bAuditActiveRole = bActive;
    if ( bActive != m_bLastActiveRole ) {
        CLog::Print( LOG_INFO, "Audit HaRole 전환: %s → %s (role=%s vip=%s%s)",
                     m_bLastActiveRole ? "active" : "standby", bActive ? "active" : "standby",
                     m_strHaRole.c_str(), m_strHaVip.empty() ? "-" : m_strHaVip.c_str(),
                     ( m_strHaRole == "auto" && !m_strHaVip.empty() ) ? ( bActive ? " 소유" : " 미소유" ) : "" );
        m_bLastActiveRole = bActive;
        m_bAuditStandbyLogged = false;  // 전환 시 재로그 허용
    }
}

// SESSION_LIST 전량 페이지 수집 (지정 endpoint). id→age_sec. grace 는 회수 시 client-side 적용
//   (min_age_sec=0 로 전량 받아 zombie 판정을 정확히 — grace 필터를 서버서 걸면 신규 세션이
//   zombie 로 오판된다).
bool CCmpClient::FetchSessionList( const CmpEndpoint &ep, const std::string &strKind,
                                   std::map<std::string, int> &mapOut ) {
    int offset = 0;
    for ( int guard = 0; guard < 256; ++guard ) {  // 256*40 = 10240 상한 (무한 페이지 방지)
        SimpleJson::JsonNode req;
        req.Set( "cmd", "SESSION_LIST" );
        req.Set( "kind", strKind );
        req.Set( "offset", offset );
        req.Set( "limit", 40 );
        req.Set( "min_age_sec", 0 );
        req.Set( "sesid", CSipMessageLogger::IssueSesId( "", "csp" ) );

        std::string resp;
        if ( !_SendOnEndpoint( ep, req, resp ) ) return false;
        SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse( resp );
        if ( root.GetString( "status" ) != "OK" ) return false;

        SimpleJson::JsonNode arr = root.Get( "entries" );
        if ( arr.type == SimpleJson::JSON_ARRAY ) {
            for ( size_t i = 0; i < arr.Size(); ++i ) {
                SimpleJson::JsonNode e = arr.At( i );
                std::string id = e.GetString( "session_id" );
                if ( !id.empty() ) mapOut[id] = (int)e.GetInt( "age_sec", 0 );
            }
        }
        int next = (int)root.GetInt( "next_offset", -1 );
        if ( next < 0 ) break;
        offset = next;
    }
    return true;
}

// audit 수준2 재조정 — KeepAliveLoop 이 HEARTBEAT 성공 후 매 cycle(3s) 호출.
//   per-shard: CSP relay 세션을 담당 CMP endpoint(consistent hash)로 분할해 endpoint 별 digest 와
//   대조 → 불일치 샤드만 그 CMP 에서 SESSION_LIST 를 당겨 회수. 단일 CMP 면 샤드 1개 = 종전 동작.
void CCmpClient::RunAuditCycle() {
    if ( !m_bAuditEnable ) return;

    ResolveActiveRole();  // 매 cycle 역할 재평가 — HaRole=auto+HaVip 시 VIP 소유로 active/standby 갱신(hot-standby 승격 반영)

    std::vector<CmpEndpoint> eps;
    {
        std::lock_guard<std::mutex> lock( m_mutexEndpoints );
        eps = m_endpoints;
    }
    if ( eps.empty() ) return;

    // CSP 측 relay 세션집합 (dialog + 트랜잭션 양쪽 — 설정중 세션 오판 방지)
    std::set<std::string> setCsp;
    gclsCallMap.CollectRelaySessionIds( setCsp );
    gclsTransCallMap.CollectRelaySessionIds( setCsp );

    // 세션을 담당 endpoint(consistent hash)로 분할
    std::map<std::string, std::set<std::string>> mapShard;  // endpoint key → CSP 세션집합
    for ( const auto &sid : setCsp ) {
        CmpEndpoint ep = SelectEndpointForSession( sid );
        mapShard[ep.strKey].insert( sid );
    }

    // endpoint(샤드)별로 지문 대조 — 불일치 샤드만 재조정
    static const std::set<std::string> kEmpty;
    for ( const auto &ep : eps ) {
        CmpDigest dg;
        {
            std::lock_guard<std::mutex> lock( m_mutexDigest );
            auto it = m_mapEndpointDigest.find( ep.strKey );
            if ( it != m_mapEndpointDigest.end() ) dg = it->second;
        }
        if ( !dg.bValid ) continue;  // 이 endpoint 는 이번 cycle HEARTBEAT 무응답 — 재조정 보류

        auto itShard = mapShard.find( ep.strKey );
        const std::set<std::string> &shard = ( itShard != mapShard.end() ) ? itShard->second : kEmpty;
        uint64_t uCspHash = 0;
        for ( const auto &sid : shard ) uCspHash ^= Fnv1a64( sid );
        int iCspCount = (int)shard.size();

        if ( iCspCount == dg.iCount && uCspHash == dg.uHash ) continue;  // 이 샤드 정합 — 무동작

        // 불일치 — standby 는 탐지·로그만(오회수 방지), active 만 회수.
        if ( !m_bAuditActiveRole ) {
            if ( !m_bAuditStandbyLogged ) {
                CLog::Print(
                    LOG_INFO,
                    "Audit: shard(%s) digest mismatch observed (standby role — no reclaim). csp[%d,%016llx] cmp[%d,%016llx]",
                    ep.strKey.c_str(), iCspCount, (unsigned long long)uCspHash, dg.iCount,
                    (unsigned long long)dg.uHash );
                m_bAuditStandbyLogged = true;
            }
            continue;
        }
        m_bAuditStandbyLogged = false;

        CLog::Print( LOG_INFO,
                     "Audit: shard(%s) digest mismatch — reconciling. csp[%d,%016llx] cmp[%d,%016llx]",
                     ep.strKey.c_str(), iCspCount, (unsigned long long)uCspHash, dg.iCount,
                     (unsigned long long)dg.uHash );
        ReconcileShard( ep, shard );
    }
}

// 한 CMP endpoint(샤드) 재조정 — 그 CMP 에서 SESSION_LIST 전량을 당겨 diff.
//   orphan(CMP有 shard-CSP無, grace 충족) → RemoveSession 회수. zombie(shard-CSP有 CMP無, 미디어
//   소실) → 지정 세션 teardown(opt-in). 세션 재라우팅 없이 지정 sid 로 처리(다른 샤드 오회수 방지).
void CCmpClient::ReconcileShard( const CmpEndpoint &ep, const std::set<std::string> &setShardCsp ) {
    std::map<std::string, int> mapCmp;
    if ( !FetchSessionList( ep, "relay", mapCmp ) ) {
        CLog::Print( LOG_ERROR, "Audit: SESSION_LIST(%s) fetch failed — 이 샤드 skip", ep.strKey.c_str() );
        return;
    }

    // orphan (CMP有 CSP無) — grace(age>=GraceSec) 충족분만 RemoveSession 회수
    int iReclaimed = 0, iDeferred = 0;
    for ( const auto &kv : mapCmp ) {
        const std::string &sid = kv.first;
        int age = kv.second;
        if ( setShardCsp.count( sid ) ) continue;
        if ( age < m_iAuditGraceSec ) continue;  // 신규 setup 세션 보호(grace)
        if ( iReclaimed >= m_iAuditMaxPerCycle ) {
            ++iDeferred;
            continue;
        }
        RemoveSession( sid );  // 기존 경로 — 멱등(sid sticky/ring 으로 이 endpoint 에 라우팅)
        CLog::Print( LOG_INFO, "Audit orphan reclaim: relay=%s age=%ds shard=%s (CMP有 CSP無 → RemoveSession)",
                     sid.c_str(), age, ep.strKey.c_str() );
        ++iReclaimed;
    }
    if ( iDeferred > 0 )
        CLog::Print( LOG_INFO, "Audit: orphan reclaim %d건 다음 cycle 이월 (shard=%s max=%d)", iDeferred,
                     ep.strKey.c_str(), m_iAuditMaxPerCycle );

    // zombie (shard-CSP有 CMP無) — 이 endpoint 목록에 없으면 미디어 소실. 지정 sid teardown(opt-in).
    std::set<std::string> setCmpKeys;
    for ( const auto &kv : mapCmp ) setCmpKeys.insert( kv.first );
    int iZombie = 0, iTorn = 0;
    for ( const auto &sid : setShardCsp ) {
        if ( setCmpKeys.count( sid ) ) continue;
        ++iZombie;
        if ( m_bAuditZombieTeardown && iTorn < m_iAuditMaxPerCycle ) {
            bool bDone = gclsCallMap.TeardownByRelaySessionId( sid );
            if ( !bDone ) bDone = gclsTransCallMap.TeardownByRelaySessionId( sid );
            if ( bDone ) ++iTorn;
        }
    }
    if ( iZombie > 0 ) {
        if ( m_bAuditZombieTeardown )
            CLog::Print( LOG_INFO, "Audit zombie teardown: %d/%d call(s) 종료 (shard=%s CSP有 CMP無)", iTorn,
                         iZombie, ep.strKey.c_str() );
        else
            CLog::Print( LOG_INFO,
                         "Audit: zombie relay %d건 감지 (shard=%s CSP有 CMP無 — 미디어 소실). teardown 비활성(탐지만); "
                         "StaleCallTimeout 백스톱에 위임 (활성화: MediaServer.Audit.ZombieTeardown)",
                         iZombie, ep.strKey.c_str() );
    }

    CLog::Print( LOG_INFO, "Audit shard(%s) done: orphan_reclaimed=%d zombie_detected=%d", ep.strKey.c_str(),
                 iReclaimed, iZombie );
}

void CCmpClient::RecvLoop() {
    char buffer[4096];
    struct sockaddr_in cliaddr;
    socklen_t len;

    while ( m_bRecvRunning ) {
        len = sizeof( cliaddr );
        int n = recvfrom( m_hSocket, buffer, sizeof( buffer ) - 1, 0, (struct sockaddr *)&cliaddr, &len );
        if ( n > 0 ) {
            buffer[n] = '\0';
            std::string strPacket = buffer;
            // printf("RX: %s\n", strPacket.c_str());

            // envelope v2 응답 파싱 — {hdr:{trans_id,cmd,status[,code,reason]}[,payload]}
            SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse( strPacket );
            if ( root.type == SimpleJson::JSON_OBJECT && root.Get( "hdr" ).type == SimpleJson::JSON_OBJECT ) {
                SimpleJson::JsonNode hdr = root.Get( "hdr" );
                int transId = (int)hdr.GetInt( "trans_id", 0 );

                // CMP 비동기 이벤트(RELAY_ABORTED/PTT_GROUP_ABORTED) — 동일 trans_id 의 v2 response 로
                //   ack 회신 후 이벤트 큐에 넣어 dispatch 스레드로 넘긴다 (핸들러가 RemoveSession 을
                //   부를 수 있어 RecvLoop 에서 직접 처리 금지 — 데드락 회피). cmp_media_api.md §8.
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
                        gclsSipLogger.LogMessage( "cmp", "csp", "JSON", hdr.GetString( "cmd" ).c_str(),
                                                  ( m_strCmpIp + ":" + std::to_string( m_iCmpPort ) ).c_str(),
                                                  strPacket.c_str(), hdr.GetString( "service", "system" ).c_str(),
                                                  std::to_string( transId ).c_str(), hdr.GetString( "sesid" ).c_str(),
                                                  "", "", "" );
                    }
                    {
                        std::lock_guard<std::mutex> lock( m_mutexEvents );
                        m_eventQueue.push_back( root );
                    }
                    m_cvEvents.notify_one();
                    continue;
                }

                std::string strStatus = hdr.GetString( "status", "ERROR" );

                // 소비자(AddSession 등) 파싱 호환 — payload 에 hdr 의 status/code/reason 을
                // 병합한 문자열을 Transaction 응답으로 전달한다.
                SimpleJson::JsonNode body = root.Get( "payload" );
                if ( body.type != SimpleJson::JSON_OBJECT ) {
                    body = SimpleJson::JsonNode();
                    body.type = SimpleJson::JSON_OBJECT;
                }
                body.Set( "status", strStatus );
                if ( hdr.Has( "code" ) ) body.Set( "code", hdr.GetString( "code" ) );
                if ( hdr.Has( "reason" ) ) body.Set( "reason", hdr.GetString( "reason" ) );
                std::string respBody = body.ToString();

                if ( gclsSipLogger.IsEnabled() ) {
                    // Transaction에서 sesid/service/caller/callee 조회
                    std::string strRxSesId, strRxSvc, strRxCaller, strRxCallee;
                    bool bRxLog = true;  // HB 샘플링 — 요청을 기록한 경우에만 응답도 기록
                    {
                        std::lock_guard<std::mutex> lk2( m_mutexTrans );
                        auto itT = m_mapTransactions.find( transId );
                        if ( itT != m_mapTransactions.end() ) {
                            strRxSesId = itT->second->strSesId;
                            strRxSvc = itT->second->strService;
                            strRxCaller = itT->second->strCaller;
                            strRxCallee = itT->second->strCallee;
                            bRxLog = itT->second->bLogMsg;
                        }
                    }
                    if ( !bRxLog ) {
                        OnTransactionComplete( transId, true, respBody );
                        continue;
                    }
                    const char *pszSvc = strRxSvc.empty() ? "system" : strRxSvc.c_str();
                    std::string strRxTxId = std::to_string( transId );
                    gclsSipLogger.LogMessage( "cmp", "csp", "JSON", strStatus.c_str(),
                                              ( m_strCmpIp + ":" + std::to_string( m_iCmpPort ) ).c_str(),
                                              strPacket.c_str(), pszSvc, strRxTxId.c_str(), strRxSesId.c_str(), "",
                                              strRxCaller.c_str(), strRxCallee.c_str() );
                }
                // Notify Transaction matches
                OnTransactionComplete( transId, true, respBody );
            } else {
                CLog::Print( LOG_INFO, "CmpClient non-v2 RX: %s", strPacket.c_str() );
            }
        }
    }
}

// OnPacketReceived is deprecated/unused with new RecvLoop logic.

// 이벤트 dispatch 전용 스레드 — RecvLoop 가 큐에 넣으면 여기서 HandleEvent 를 부른다.
//   (핸들러의 RemoveSession 응답은 RecvLoop 가 수신하므로 데드락 없음.)
void CCmpClient::EventDispatchLoop() {
    while ( m_bEventRunning ) {
        SimpleJson::JsonNode clsEvent;
        {
            std::unique_lock<std::mutex> lock( m_mutexEvents );
            m_cvEvents.wait( lock, [this] { return !m_eventQueue.empty() || !m_bEventRunning; } );
            if ( !m_bEventRunning && m_eventQueue.empty() ) break;
            clsEvent = m_eventQueue.front();
            m_eventQueue.pop_front();
        }
        try {
            HandleEvent( clsEvent );
        } catch ( ... ) {
            CLog::Print( LOG_ERROR, "CmpClient HandleEvent exception" );
        }
    }
}

// CMP 이벤트 처리 (EventDispatchLoop 스레드). 회수(teardown)는 active 역할일 때만 —
//   standby 는 탐지·로그만(오회수 방지, audit 회수 게이트와 동일 정책). 이벤트는 CMP 가 자원을
//   확정 회수했다는 authoritative 신호라 audit(pull) zombie 추정과 달리 opt-in 없이 즉시 처리한다.
void CCmpClient::HandleEvent( const SimpleJson::JsonNode &event ) {
    SimpleJson::JsonNode hdr = event.Get( "hdr" );
    SimpleJson::JsonNode payload = event.Get( "payload" );
    std::string strCmd = hdr.GetString( "cmd" );

    if ( strCmd == "RELAY_ABORTED" ) {
        std::string strSid = payload.GetString( "session_id" );
        std::string strReason = payload.GetString( "reason" );
        int iHeld = (int)payload.GetInt( "held_sec", 0 );
        if ( !m_bAuditActiveRole ) {
            CLog::Print( LOG_INFO, "RELAY_ABORTED observed (standby — no teardown): relay=%s reason=%s held=%ds",
                         strSid.c_str(), strReason.c_str(), iHeld );
            return;
        }
        // 확립 호(hold_timeout) 또는 실패 setup(orphan_no_rtp) — 양쪽 CallMap 에서 지목 종료(멱등).
        bool bDone = gclsCallMap.TeardownByRelaySessionId( strSid );
        if ( !bDone ) bDone = gclsTransCallMap.TeardownByRelaySessionId( strSid );
        ReleaseEndpointForKey( strSid );
        {
            std::lock_guard<std::mutex> lock( m_mutexSesid );
            m_mapKeyToSesid.erase( strSid );
        }
        CLog::Print( LOG_INFO, "RELAY_ABORTED handled: relay=%s reason=%s held=%ds torn_down=%d",
                     strSid.c_str(), strReason.c_str(), iHeld, bDone );
    } else if ( strCmd == "PTT_GROUP_ABORTED" ) {
        std::string strGid = payload.GetString( "group_id" );
        std::string strReason = payload.GetString( "reason" );
        if ( !m_bAuditActiveRole ) {
            CLog::Print( LOG_INFO, "PTT_GROUP_ABORTED observed (standby — no action): group=%s reason=%s",
                         strGid.c_str(), strReason.c_str() );
            return;
        }
        // CMP 가 유휴 그룹(멤버·활동 없음)을 회수 — CSP 캐시(sesid/endpoint) 정리로 다음 사용 시
        //   깨끗이 재수립되게 한다. 진행 중 멤버 호는 없음(회수 조건이 멤버 0)이라 teardown 불필요.
        gclsGroupCallService.OnGroupAborted( strGid );
        ReleaseEndpointForKey( strGid );
        {
            std::lock_guard<std::mutex> lock( m_mutexSesid );
            m_mapKeyToSesid.erase( strGid );
        }
        CLog::Print( LOG_INFO, "PTT_GROUP_ABORTED handled: group=%s reason=%s (캐시 정리)", strGid.c_str(),
                     strReason.c_str() );
    } else {
        CLog::Print( LOG_INFO, "CmpClient: unknown event cmd=%s (무시)", strCmd.c_str() );
    }
}

void CCmpClient::KeepAliveLoop() {
    while ( m_bKeepAliveRunning ) {
        // Use JSON Alive()
        bool bSuccess = Alive();

        // 일시적 UDP 제어 타임아웃 1회에 CMP Disconnected 로 판정하면, OnCmpStatusChanged →
        // 그룹 재수립 + 활성 멤버 전원 재INVITE/teardown 이 발생해 진행 중 PTT 그룹콜이 끊긴다.
        // (40명 부하 시 HEARTBEAT 가 간헐 타임아웃) → 연속 kMaxAliveFail 회 실패에서만 disconnect.
        const int kMaxAliveFail = 3;  // 3회 × 3s ≈ 9s 연속 무응답이어야 진짜 다운으로 간주
        if ( bSuccess ) {
            m_iAliveFailCount = 0;
            if ( !m_bConnected ) {
                m_bConnected = true;
                if ( m_fnConnectionCallback ) {
                    m_fnConnectionCallback( true );
                }
                CLog::Print( LOG_INFO, "CMP Connected" );
            }
            // audit 수준2 — 세션집합 정합 대조(불일치 시에만 SESSION_LIST diff+회수). 예외 무해화.
            try {
                RunAuditCycle();
            } catch ( ... ) {
                CLog::Print( LOG_ERROR, "RunAuditCycle exception — cycle skip" );
            }
        } else {
            if ( m_bConnected ) {
                ++m_iAliveFailCount;
                CLog::Print( LOG_INFO, "CMP HEARTBEAT 실패 %d/%d (연속) — 임계 도달 전까지 연결 유지",
                             m_iAliveFailCount, kMaxAliveFail );
                if ( m_iAliveFailCount >= kMaxAliveFail ) {
                    m_bConnected = false;
                    m_iAliveFailCount = 0;
                    if ( m_fnConnectionCallback ) {
                        m_fnConnectionCallback( false );
                    }
                    CLog::Print( LOG_INFO, "CMP Disconnected (연속 %d회 HEARTBEAT 실패)", kMaxAliveFail );
                }
            }
        }

        std::this_thread::sleep_for( std::chrono::seconds( 3 ) );
    }
}
