#ifndef __FM_REPORTER_H__
#define __FM_REPORTER_H__

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "SimpleJson.h"

// 모듈 자기보고(FM push) 클라이언트 — docs/design/alarm_self_reporting.md.
//   OAM FM ingest 로 UDP JSON envelope v2 의 FM_REGISTER/FM_ALARM/FM_EVENT/FM_SYNC 를
//   push 한다. 활성 알람의 SoT 는 이쪽(모듈)이고 OAM 은 미러 — 통지 유실·재기동은
//   주기 FM_SYNC 가 수렴시킨다.
//   신뢰성: 전 메시지 type:"event" — ack(동일 trans_id 의 response) 미수신 시 1s 간격
//   최대 5회 재전송 (cmp/cmdp 이벤트 채널과 동일 정책, cmp_media_api.md §8).
//   ERROR code=UNREGISTERED 수신 시 FM_REGISTER 부터 다시 (OAM 재기동/최초 접속).
//   전 모듈(csp/cmp/cmdp) 공용 header-only — 모듈별 로거(CLog/PLog) 차이는 Init 의
//   로그 콜백 주입으로 흡수한다.

// 로그 레벨 — 콜백이 모듈 로거의 레벨로 매핑한다.
enum EnumFmLogLevel { FM_LOG_DEBUG = 0, FM_LOG_INFO = 1, FM_LOG_ERROR = 2 };
typedef std::function<void( EnumFmLogLevel eLevel, const std::string &strMsg )> FmLogFn;

// 알람 발생 인스턴스 — active 목록(FM_SYNC)의 단위.
struct FmActiveAlarm {
    std::string strCode;              // 알람 정의 코드 (fm_catalog.json 의 code)
    std::string strMo;                // managedObjectInstance (예: SIG_SVR_01/csp/db — 서버명 루트)
    std::string strOpenTs;            // 발생 시각 (ISO8601)
    std::string strSeverity;          // 단계 임계 알람의 현재 severity (빈 값 = 카탈로그 기본)
    SimpleJson::JsonNode nodeParams;  // msg 템플릿 치환 값
};

class CFmReporter {
public:
    static CFmReporter &GetInstance() {
        static CFmReporter instance;
        return instance;
    }

    // strCatalogFile: fm_catalog.json 경로 — 등록(FM_REGISTER) payload 로 push 된다.
    // strModule: FM_REGISTER 의 module 필드 (csp/cmp/pmp/… — 변종은 자기 이름).
    // fnLog: 모듈 로거 주입 (미지정 시 무출력).
    bool Init( const std::string &strOamIp, int iOamPort, const std::string &strNode, const std::string &strModule,
               const std::string &strCatalogFile, int iSyncSec, FmLogFn fnLog = FmLogFn() ) {
        if ( m_bRunning ) return true;

        m_strOamIp = strOamIp;
        m_iOamPort = iOamPort;
        m_strNode = strNode;
        m_strModule = strModule;
        m_fnLog = fnLog;
        if ( iSyncSec > 0 ) m_iSyncSec = iSyncSec;
        m_llBootId = (long long)time( NULL );

        // 카탈로그 적재 — FM_REGISTER 로 push 된다. 없으면 빈 카탈로그로 등록
        //   (OAM 이 미등록 code 의 알람을 UNKNOWN_CODE 로 거절하므로 여기서 드러낸다).
        std::ifstream clsFile( strCatalogFile.c_str() );
        if ( clsFile.is_open() ) {
            std::stringstream clsBuf;
            clsBuf << clsFile.rdbuf();
            m_nodeCatalog = SimpleJson::JsonNode::Parse( clsBuf.str() );
            Log( FM_LOG_INFO, "catalog loaded (%s)", strCatalogFile.c_str() );
        } else {
            Log( FM_LOG_ERROR, "catalog open failed (%s) — 빈 카탈로그로 등록", strCatalogFile.c_str() );
        }

        m_hSocket = socket( AF_INET, SOCK_DGRAM, 0 );
        if ( m_hSocket < 0 ) {
            Log( FM_LOG_ERROR, "socket error" );
            return false;
        }
        struct timeval tvRecv;
        tvRecv.tv_sec = 1;
        tvRecv.tv_usec = 0;
        setsockopt( m_hSocket, SOL_SOCKET, SO_RCVTIMEO, &tvRecv, sizeof( tvRecv ) );

        m_bRunning = true;
        m_threadRecv = std::thread( &CFmReporter::RecvLoop, this );
        m_threadTimer = std::thread( &CFmReporter::TimerLoop, this );

        Log( FM_LOG_INFO, "started (oam=%s:%d, node=%s, boot_id=%lld, sync=%ds)", m_strOamIp.c_str(),
             m_iOamPort, m_strNode.c_str(), m_llBootId, m_iSyncSec );
        SendRegister();
        return true;
    }

    void Stop() {
        if ( !m_bRunning ) return;
        m_bRunning = false;
        if ( m_threadTimer.joinable() ) m_threadTimer.join();
        if ( m_threadRecv.joinable() ) m_threadRecv.join();
        if ( m_hSocket != -1 ) {
            close( m_hSocket );
            m_hSocket = -1;
        }
        Log( FM_LOG_INFO, "stopped" );
    }

    bool IsEnabled() const {
        return m_bRunning;
    }
    // 논리 노드 ID (hdr.node = SystemId) — 이벤트 mo 구성용 (<node>/<module>, 서버명 루트).
    const std::string &Node() const {
        return m_strNode;
    }

    // 알람 open/close — (code, mo) 가 활성키. 이미 같은 상태면 no-op (전이 시에만 통지).
    void AlarmOpen( const std::string &strCode, const std::string &strMo ) {
        SimpleJson::JsonNode nodeEmpty;
        AlarmOpen( strCode, strMo, nodeEmpty );
    }

    void AlarmOpen( const std::string &strCode, const std::string &strMo, const SimpleJson::JsonNode &nodeParams ) {
        AlarmOpen( strCode, strMo, nodeParams, "" );
    }

    // severity 동반 open — 단계 임계(staged) 알람용. 이미 open 이라도 severity 가 다르면
    //   재통지한다 (OAM transition 이 action=change/moreSevere|lessSevere 로 기록 —
    //   표준화 §3.4(d)). 빈 severity 는 카탈로그 기본값 사용(기존 동작).
    void AlarmOpen( const std::string &strCode, const std::string &strMo, const SimpleJson::JsonNode &nodeParams,
                    const std::string &strSeverity ) {
        if ( !m_bRunning ) return;
        std::string strAkey = strCode + "@" + strMo;
        {
            std::lock_guard<std::mutex> lock( m_mutex );
            std::map<std::string, FmActiveAlarm>::iterator it = m_mapActive.find( strAkey );
            if ( it != m_mapActive.end() ) {
                if ( it->second.strSeverity == strSeverity ) return;  // 이미 같은 상태 — 전이 아님
                it->second.strSeverity = strSeverity;                 // 승격/완화 — open_ts 는 유지
                it->second.nodeParams = nodeParams;
            } else {
                FmActiveAlarm clsActive;
                clsActive.strCode = strCode;
                clsActive.strMo = strMo;
                clsActive.strOpenTs = FmNowIso();
                clsActive.strSeverity = strSeverity;
                clsActive.nodeParams = nodeParams;
                m_mapActive[strAkey] = clsActive;
            }
        }
        SimpleJson::JsonNode nodePayload;
        nodePayload.Set( "action", "open" );
        nodePayload.Set( "code", strCode );
        nodePayload.Set( "mo_instance", strMo );
        if ( !strSeverity.empty() ) nodePayload.Set( "perceived_severity", strSeverity );
        if ( nodeParams.type == SimpleJson::JSON_OBJECT ) nodePayload.Set( "params", nodeParams );
        nodePayload.Set( "ts", FmNowIso() );
        SendFm( "FM_ALARM", nodePayload );
        Log( FM_LOG_INFO, "ALARM OPEN %s%s%s", strAkey.c_str(), strSeverity.empty() ? "" : " sev=",
             strSeverity.c_str() );
    }

    void AlarmClose( const std::string &strCode, const std::string &strMo ) {
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
        Log( FM_LOG_INFO, "ALARM CLOSE %s", strAkey.c_str() );
    }

    // 정상 동작 이벤트 (stateChange/audit) — 활성 상태 없음, best-effort (재전송 1s×5).
    void SendEvent( const std::string &strType, const std::string &strKind, const std::string &strMo ) {
        SimpleJson::JsonNode nodeEmpty;
        SendEvent( strType, strKind, strMo, nodeEmpty );
    }

    void SendEvent( const std::string &strType, const std::string &strKind, const std::string &strMo,
                    const SimpleJson::JsonNode &nodeParams ) {
        if ( !m_bRunning ) return;
        SimpleJson::JsonNode nodePayload;
        nodePayload.Set( "type", strType );
        nodePayload.Set( "kind", strKind );
        if ( !strMo.empty() ) nodePayload.Set( "mo_instance", strMo );
        if ( nodeParams.type == SimpleJson::JSON_OBJECT ) nodePayload.Set( "params", nodeParams );
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

private:
    static const int kFmRetryIntervalSec = 1;   // ack 미수신 재전송 간격 (cmp_media_api.md §8)
    static const int kFmMaxAttempts = 5;        // 재전송 상한 — 초과 시 폐기 (FM_SYNC 가 수렴)
    static const int kFmRegisterRetrySec = 5;   // 미등록 상태의 FM_REGISTER 재시도 간격
    // FM 채널 datagram 상한 — FM_REGISTER 가 카탈로그 전량을 실으므로 CMP 미디어 채널의
    //   4KB(envelope v2 §1.2)로는 알람 수 종부터 등록이 영구 실패한다. FM 채널은 32KB
    //   (OAM fm_ingest recv 는 64KB — OAM 을 먼저 올려야 4KB 초과 등록이 수신된다).
    static const size_t kFmMaxPacket = 32768;
    static const size_t kFmEventQueueMax = 32;  // 미등록 구간 이벤트 버퍼 상한

    CFmReporter()
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

    ~CFmReporter() {
        Stop();
    }

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

    void Log( EnumFmLogLevel eLevel, const char *pszFmt, ... ) {
        if ( !m_fnLog ) return;
        char szBuf[512];
        va_list ap;
        va_start( ap, pszFmt );
        vsnprintf( szBuf, sizeof( szBuf ), pszFmt, ap );
        va_end( ap );
        m_fnLog( eLevel, szBuf );
    }

    void RecvLoop() {   // ack/response 수신 (SO_RCVTIMEO 1s)
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

    void TimerLoop() {  // 1s tick — pending 재전송·미등록 재시도·FM_SYNC 주기 송신
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
                        Log( FM_LOG_DEBUG, "%s ack 미수신 %d회 — 폐기 (trans_id=%u)",
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

    void SendRegister() {
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
                if ( !it->second.strSeverity.empty() ) nodeItem.Set( "perceived_severity", it->second.strSeverity );
                if ( it->second.nodeParams.type == SimpleJson::JSON_OBJECT )
                    nodeItem.Set( "params", it->second.nodeParams );
                nodeActive.Add( nodeItem );
            }
            nodePayload.Set( "active", nodeActive );
            m_tLastRegister = time( NULL );
        }
        SendFm( "FM_REGISTER", nodePayload );
    }

    void SendSync() {
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
                if ( !it->second.strSeverity.empty() ) nodeItem.Set( "perceived_severity", it->second.strSeverity );
                if ( it->second.nodeParams.type == SimpleJson::JSON_OBJECT )
                    nodeItem.Set( "params", it->second.nodeParams );
                nodeActive.Add( nodeItem );
            }
            nodePayload.Set( "active", nodeActive );
            m_tLastSync = time( NULL );
        }
        SendFm( "FM_SYNC", nodePayload );
    }

    void FlushQueuedEvents() {
        std::deque<SimpleJson::JsonNode> queued;
        {
            std::lock_guard<std::mutex> lock( m_mutex );
            queued.swap( m_queueEvents );
        }
        for ( std::deque<SimpleJson::JsonNode>::iterator it = queued.begin(); it != queued.end(); ++it ) {
            SendFm( "FM_EVENT", *it );
        }
    }

    unsigned int SendFm( const std::string &strCmd, SimpleJson::JsonNode &nodePayload ) {
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
            Log( FM_LOG_ERROR, "%s packet too large (%zu > %zu) — 미전송", strCmd.c_str(),
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

    void HandleResponse( const SimpleJson::JsonNode &nodeMsg ) {
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
                Log( FM_LOG_INFO, "registered (sync=%ds)", m_iSyncSec );
                FlushQueuedEvents();  // 미등록 구간에 쌓인 이벤트 송신
            }
            return;
        }

        std::string strCode = nodeHdr.GetString( "code" );
        if ( strCode == "UNREGISTERED" ) {
            // OAM 재기동/최초 접속 — FM_REGISTER 부터 다시 (TimerLoop 가 재시도)
            if ( m_bRegistered ) Log( FM_LOG_INFO, "OAM 미등록 응답 — 재등록 예정" );
            m_bRegistered = false;
        } else {
            Log( FM_LOG_ERROR, "%s ERROR %s (%s)", strCmd.c_str(), strCode.c_str(),
                 nodeHdr.GetString( "reason" ).c_str() );
        }
    }

    struct Pending {
        std::string strPacket;
        std::string strCmd;
        int iAttempts;
        time_t tNextSend;
    };

    std::string m_strOamIp;
    int m_iOamPort;
    std::string m_strNode;               // hdr.node — 논리 노드 ID (SystemId)
    std::string m_strModule;             // FM_REGISTER module 필드
    int m_iSyncSec;                      // FM_SYNC 주기 (FM_REGISTER 응답의 sync_interval_sec 가 덮음)
    long long m_llBootId;                // 기동 epoch — OAM 의 재기동 감지 키
    SimpleJson::JsonNode m_nodeCatalog;  // fm_catalog.json 파싱 결과
    FmLogFn m_fnLog;                     // 모듈 로거 주입 (CLog/PLog 차이 흡수)

    int m_hSocket;
    std::atomic<bool> m_bRunning;
    std::atomic<bool> m_bRegistered;
    std::thread m_threadRecv;
    std::thread m_threadTimer;

    std::mutex m_mutex;                                // m_mapActive/m_mapPending/m_iNextTransId/m_iNextSeq 보호
    std::map<std::string, FmActiveAlarm> m_mapActive;  // akey(code@mo) → active
    std::map<unsigned int, Pending> m_mapPending;      // trans_id → 재전송 대기
    std::deque<SimpleJson::JsonNode> m_queueEvents;    // 미등록 구간 이벤트 버퍼 (등록 시 flush)
    unsigned int m_iNextTransId;
    unsigned int m_iNextSeq;  // boot 당 단조증가 — OAM 의 UDP 역전 방어 입력
    time_t m_tLastRegister;
    time_t m_tLastSync;
};

#define gclsFmReporter CFmReporter::GetInstance()

#endif
