#include "FmReporter.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "PLog.h"

static const int kFmRetryIntervalSec = 1;   // ack 미수신 재전송 간격 (cmp_media_api.md §8)
static const int kFmMaxAttempts = 5;        // 재전송 상한 — 초과 시 폐기 (FM_SYNC 가 수렴)
static const int kFmRegisterRetrySec = 5;   // 미등록 상태의 FM_REGISTER 재시도 간격
static const size_t kFmMaxPacket = 4096;    // envelope v2 §1.2 — datagram 4KB 상한
static const size_t kFmEventQueueMax = 32;  // 미등록 구간 이벤트 버퍼 상한

// trans_id 초기값 — 부팅 시각(ms) 시드 (CmpClient SeedTransId 관례 — 재기동 직후
//   구 프로세스 앞 지연 응답과의 오매칭 창 제거).
static unsigned int FmSeedTransId() {
    struct timeval tv;
    gettimeofday( &tv, NULL );
    return (unsigned int)( ( (unsigned long long)tv.tv_sec * 1000ULL + tv.tv_usec / 1000 ) & 0x3FFFFFFF ) | 1;
}

static std::string FmNowIso() {
    char szBuf[32];
    time_t t = time( NULL );
    struct tm tmLocal;
    localtime_r( &t, &tmLocal );
    strftime( szBuf, sizeof( szBuf ), "%Y-%m-%dT%H:%M:%S", &tmLocal );
    return szBuf;
}

CFmReporter::CFmReporter()
    : m_iOamPort( 0 ),
      m_iSyncSec( 60 ),
      m_llBootId( 0 ),
      m_hSocket( -1 ),
      m_bRunning( false ),
      m_bRegistered( false ),
      m_iNextTransId( FmSeedTransId() ),
      m_iNextSeq( 0 ),
      m_tLastRegister( 0 ),
      m_tLastSync( 0 ) {
}

CFmReporter::~CFmReporter() {
    Stop();
}

CFmReporter &CFmReporter::GetInstance() {
    static CFmReporter instance;
    return instance;
}

bool CFmReporter::Init( const std::string &strOamIp, int iOamPort, const std::string &strNode,
                        const std::string &strModule, const std::string &strCatalogFile, int iSyncSec ) {
    if ( m_bRunning ) return true;

    m_strOamIp = strOamIp;
    m_iOamPort = iOamPort;
    m_strNode = strNode;
    m_strModule = strModule;
    if ( iSyncSec > 0 ) m_iSyncSec = iSyncSec;
    m_llBootId = (long long)time( NULL );

    // 카탈로그 적재 — FM_REGISTER 로 push 된다. 없으면 빈 카탈로그로 등록
    //   (OAM 이 미등록 code 의 알람을 UNKNOWN_CODE 로 거절하므로 여기서 드러낸다).
    std::ifstream clsFile( strCatalogFile.c_str() );
    if ( clsFile.is_open() ) {
        std::stringstream clsBuf;
        clsBuf << clsFile.rdbuf();
        m_nodeCatalog = SimpleJson::JsonNode::Parse( clsBuf.str() );
        LOG_INFO( "FmReporter", "catalog loaded (%s)", strCatalogFile.c_str() );
    } else {
        LOG_ERROR( "FmReporter", "catalog open failed (%s) — 빈 카탈로그로 등록", strCatalogFile.c_str() );
    }

    m_hSocket = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( m_hSocket < 0 ) {
        LOG_ERROR( "FmReporter", "socket error" );
        return false;
    }
    struct timeval tvRecv;
    tvRecv.tv_sec = 1;
    tvRecv.tv_usec = 0;
    setsockopt( m_hSocket, SOL_SOCKET, SO_RCVTIMEO, &tvRecv, sizeof( tvRecv ) );

    m_bRunning = true;
    m_threadRecv = std::thread( &CFmReporter::RecvLoop, this );
    m_threadTimer = std::thread( &CFmReporter::TimerLoop, this );

    LOG_INFO( "FmReporter", "started (oam=%s:%d, node=%s, boot_id=%lld, sync=%ds)", m_strOamIp.c_str(),
                 m_iOamPort, m_strNode.c_str(), m_llBootId, m_iSyncSec );
    SendRegister();
    return true;
}

void CFmReporter::Stop() {
    if ( !m_bRunning ) return;
    m_bRunning = false;
    if ( m_threadTimer.joinable() ) m_threadTimer.join();
    if ( m_threadRecv.joinable() ) m_threadRecv.join();
    if ( m_hSocket != -1 ) {
        close( m_hSocket );
        m_hSocket = -1;
    }
    LOG_INFO( "FmReporter", "stopped" );
}

// ── 공개 API ─────────────────────────────────────────────────────────────

void CFmReporter::AlarmOpen( const std::string &strCode, const std::string &strMo ) {
    SimpleJson::JsonNode nodeEmpty;
    AlarmOpen( strCode, strMo, nodeEmpty );
}

void CFmReporter::AlarmOpen( const std::string &strCode, const std::string &strMo,
                             const SimpleJson::JsonNode &nodeParams ) {
    if ( !m_bRunning ) return;
    std::string strAkey = strCode + "@" + strMo;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        if ( m_mapActive.find( strAkey ) != m_mapActive.end() ) return;  // 이미 open — 전이 아님
        FmActiveAlarm clsActive;
        clsActive.strCode = strCode;
        clsActive.strMo = strMo;
        clsActive.strOpenTs = FmNowIso();
        clsActive.nodeParams = nodeParams;
        m_mapActive[strAkey] = clsActive;
    }
    SimpleJson::JsonNode nodePayload;
    nodePayload.Set( "action", "open" );
    nodePayload.Set( "code", strCode );
    nodePayload.Set( "mo_instance", strMo );
    if ( nodeParams.type == SimpleJson::JSON_OBJECT ) nodePayload.Set( "params", nodeParams );
    nodePayload.Set( "ts", FmNowIso() );
    SendFm( "FM_ALARM", nodePayload );
    LOG_INFO( "FmReporter", "ALARM OPEN %s", strAkey.c_str() );
}

void CFmReporter::AlarmClose( const std::string &strCode, const std::string &strMo ) {
    if ( !m_bRunning ) return;
    std::string strAkey = strCode + "@" + strMo;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        if ( m_mapActive.erase( strAkey ) == 0 ) return;  // open 아님 — 전이 아님
    }
    SimpleJson::JsonNode nodePayload;
    nodePayload.Set( "action", "close" );
    nodePayload.Set( "code", strCode );
    nodePayload.Set( "mo_instance", strMo );
    nodePayload.Set( "ts", FmNowIso() );
    SendFm( "FM_ALARM", nodePayload );
    LOG_INFO( "FmReporter", "ALARM CLOSE %s", strAkey.c_str() );
}

void CFmReporter::SendEvent( const std::string &strType, const std::string &strKind, const std::string &strMo ) {
    if ( !m_bRunning ) return;
    SimpleJson::JsonNode nodePayload;
    nodePayload.Set( "type", strType );
    nodePayload.Set( "kind", strKind );
    if ( !strMo.empty() ) nodePayload.Set( "mo_instance", strMo );
    nodePayload.Set( "ts", FmNowIso() );
    // 미등록 상태(OAM 미기동/재기동)의 이벤트는 재전송 5회로도 유실된다 — 기동 순서상
    //   모듈이 OAM 보다 먼저 뜨는 부트에서 process_started 가 통째로 사라지는 것을
    //   막기 위해 등록 성공 시까지 버퍼링 후 flush (상한 초과 시 오래된 것부터 폐기).
    if ( !m_bRegistered ) {
        std::lock_guard<std::mutex> lock( m_mutex );
        if ( m_queueEvents.size() >= kFmEventQueueMax ) m_queueEvents.pop_front();
        m_queueEvents.push_back( nodePayload );
        return;
    }
    SendFm( "FM_EVENT", nodePayload );
}

void CFmReporter::FlushQueuedEvents() {
    std::deque<SimpleJson::JsonNode> queued;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        queued.swap( m_queueEvents );
    }
    for ( std::deque<SimpleJson::JsonNode>::iterator it = queued.begin(); it != queued.end(); ++it ) {
        SendFm( "FM_EVENT", *it );
    }
}

// ── 송신 코어 ────────────────────────────────────────────────────────────

void CFmReporter::SendRegister() {
    SimpleJson::JsonNode nodePayload;
    nodePayload.Set( "module", m_strModule );
    nodePayload.Set( "catalog", m_nodeCatalog );
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        SimpleJson::JsonNode nodeActive;
        nodeActive.type = SimpleJson::JSON_ARRAY;
        for ( std::map<std::string, FmActiveAlarm>::iterator it = m_mapActive.begin(); it != m_mapActive.end(); ++it ) {
            SimpleJson::JsonNode nodeItem;
            nodeItem.Set( "code", it->second.strCode );
            nodeItem.Set( "mo_instance", it->second.strMo );
            nodeItem.Set( "open_ts", it->second.strOpenTs );
            if ( it->second.nodeParams.type == SimpleJson::JSON_OBJECT )
                nodeItem.Set( "params", it->second.nodeParams );
            nodeActive.Add( nodeItem );
        }
        nodePayload.Set( "active", nodeActive );
        m_tLastRegister = time( NULL );
    }
    SendFm( "FM_REGISTER", nodePayload );
}

void CFmReporter::SendSync() {
    SimpleJson::JsonNode nodePayload;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        SimpleJson::JsonNode nodeActive;
        nodeActive.type = SimpleJson::JSON_ARRAY;
        for ( std::map<std::string, FmActiveAlarm>::iterator it = m_mapActive.begin(); it != m_mapActive.end(); ++it ) {
            SimpleJson::JsonNode nodeItem;
            nodeItem.Set( "code", it->second.strCode );
            nodeItem.Set( "mo_instance", it->second.strMo );
            nodeItem.Set( "open_ts", it->second.strOpenTs );
            if ( it->second.nodeParams.type == SimpleJson::JSON_OBJECT )
                nodeItem.Set( "params", it->second.nodeParams );
            nodeActive.Add( nodeItem );
        }
        nodePayload.Set( "active", nodeActive );
        m_tLastSync = time( NULL );
    }
    SendFm( "FM_SYNC", nodePayload );
}

unsigned int CFmReporter::SendFm( const std::string &strCmd, SimpleJson::JsonNode &nodePayload ) {
    if ( m_hSocket == -1 ) return 0;

    unsigned int iTransId;
    unsigned int iSeq;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        iTransId = m_iNextTransId++;
        iSeq = ++m_iNextSeq;
    }
    nodePayload.Set( "boot_id", m_llBootId );
    nodePayload.Set( "seq", (int)iSeq );

    SimpleJson::JsonNode nodeHdr;
    nodeHdr.Set( "ver", 2 );
    nodeHdr.Set( "trans_id", (int)iTransId );
    nodeHdr.Set( "node", m_strNode );
    nodeHdr.Set( "cmd", strCmd );
    nodeHdr.Set( "type", "event" );
    nodeHdr.Set( "service", "cims" );

    SimpleJson::JsonNode nodePacket;
    nodePacket.Set( "hdr", nodeHdr );
    nodePacket.Set( "payload", nodePayload );
    std::string strPacket = nodePacket.ToString();
    if ( strPacket.size() > kFmMaxPacket ) {
        LOG_ERROR( "FmReporter", "%s packet too large (%zu > %zu) — 미전송", strCmd.c_str(),
                     strPacket.size(), kFmMaxPacket );
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        Pending clsPending;
        clsPending.strPacket = strPacket;
        clsPending.strCmd = strCmd;
        clsPending.iAttempts = 1;
        clsPending.tNextSend = time( NULL ) + kFmRetryIntervalSec;
        m_mapPending[iTransId] = clsPending;
    }

    struct sockaddr_in clsAddr;
    memset( &clsAddr, 0, sizeof( clsAddr ) );
    clsAddr.sin_family = AF_INET;
    clsAddr.sin_addr.s_addr = inet_addr( m_strOamIp.c_str() );
    clsAddr.sin_port = htons( m_iOamPort );
    sendto( m_hSocket, strPacket.c_str(), strPacket.size(), 0, (struct sockaddr *)&clsAddr, sizeof( clsAddr ) );
    return iTransId;
}

// ── 수신/타이머 ──────────────────────────────────────────────────────────

void CFmReporter::RecvLoop() {
    char szBuf[kFmMaxPacket + 1];
    while ( m_bRunning ) {
        ssize_t iLen = recv( m_hSocket, szBuf, kFmMaxPacket, 0 );
        if ( iLen <= 0 ) continue;  // SO_RCVTIMEO 1s — 종료 플래그 재확인
        szBuf[iLen] = '\0';
        SimpleJson::JsonNode nodeMsg = SimpleJson::JsonNode::Parse( std::string( szBuf, (size_t)iLen ) );
        if ( nodeMsg.type != SimpleJson::JSON_OBJECT ) continue;
        HandleResponse( nodeMsg );
    }
}

void CFmReporter::HandleResponse( const SimpleJson::JsonNode &nodeMsg ) {
    if ( !nodeMsg.Has( "hdr" ) ) return;
    SimpleJson::JsonNode nodeHdr = nodeMsg.Get( "hdr" );
    if ( nodeHdr.GetString( "type" ) != "response" ) return;
    unsigned int iTransId = (unsigned int)nodeHdr.GetInt( "trans_id" );
    std::string strCmd = nodeHdr.GetString( "cmd" );
    std::string strStatus = nodeHdr.GetString( "status" );

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        if ( m_mapPending.erase( iTransId ) == 0 ) return;  // 미상/지연 응답 — 무시
    }

    if ( strStatus == "OK" ) {
        if ( strCmd == "FM_REGISTER" ) {
            m_bRegistered = true;
            if ( nodeMsg.Has( "payload" ) ) {
                int iSync = (int)nodeMsg.Get( "payload" ).GetInt( "sync_interval_sec", 0 );
                if ( iSync > 0 ) m_iSyncSec = iSync;
            }
            LOG_INFO( "FmReporter", "registered (sync=%ds)", m_iSyncSec );
            FlushQueuedEvents();  // 미등록 구간에 쌓인 이벤트 송신
        }
        return;
    }

    std::string strCode = nodeHdr.GetString( "code" );
    if ( strCode == "UNREGISTERED" ) {
        // OAM 재기동/최초 접속 — FM_REGISTER 부터 다시 (TimerLoop 가 재시도)
        if ( m_bRegistered ) LOG_INFO( "FmReporter", "OAM 미등록 응답 — 재등록 예정" );
        m_bRegistered = false;
    } else {
        LOG_ERROR( "FmReporter", "%s ERROR %s (%s)", strCmd.c_str(), strCode.c_str(),
                     nodeHdr.GetString( "reason" ).c_str() );
    }
}

void CFmReporter::TimerLoop() {
    struct sockaddr_in clsAddr;
    memset( &clsAddr, 0, sizeof( clsAddr ) );
    clsAddr.sin_family = AF_INET;
    clsAddr.sin_addr.s_addr = inet_addr( m_strOamIp.c_str() );
    clsAddr.sin_port = htons( m_iOamPort );

    while ( m_bRunning ) {
        sleep( 1 );
        time_t tNow = time( NULL );

        // 미등록 — FM_REGISTER 재시도 (초기 접속 실패/OAM 재기동/UNREGISTERED 응답)
        if ( !m_bRegistered ) {
            bool bDue;
            {
                std::lock_guard<std::mutex> lock( m_mutex );
                bDue = ( tNow - m_tLastRegister >= kFmRegisterRetrySec );
            }
            if ( bDue ) SendRegister();
        } else if ( tNow - m_tLastSync >= m_iSyncSec ) {
            SendSync();
        }

        // pending 재전송 (1s×5) — 초과분 폐기. 활성 알람은 FM_SYNC 가 복구하므로
        //   여기서 잃어도 수렴은 보장된다 (이벤트는 best-effort).
        std::vector<std::string> vecResend;
        {
            std::lock_guard<std::mutex> lock( m_mutex );
            for ( std::map<unsigned int, Pending>::iterator it = m_mapPending.begin(); it != m_mapPending.end(); ) {
                if ( it->second.tNextSend > tNow ) {
                    ++it;
                    continue;
                }
                if ( it->second.iAttempts >= kFmMaxAttempts ) {
                    LOG_DEBUG( "FmReporter", "%s ack 미수신 %d회 — 폐기 (trans_id=%u)",
                                 it->second.strCmd.c_str(), it->second.iAttempts, it->first );
                    m_mapPending.erase( it++ );
                    continue;
                }
                it->second.iAttempts++;
                it->second.tNextSend = tNow + kFmRetryIntervalSec;
                vecResend.push_back( it->second.strPacket );
                ++it;
            }
        }
        for ( size_t i = 0; i < vecResend.size(); ++i ) {
            sendto( m_hSocket, vecResend[i].c_str(), vecResend[i].size(), 0, (struct sockaddr *)&clsAddr,
                    sizeof( clsAddr ) );
        }
    }
}
