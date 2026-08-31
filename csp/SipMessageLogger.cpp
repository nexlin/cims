#include "SipMessageLogger.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <utility>
#include <vector>

#include "FmReporter.h"

CSipMessageLogger gclsSipLogger;

// Maximum Call-ID cache entries before eviction
static const size_t MAX_CALLID_CACHE = 10000;

// 비동기 writer 튜닝값
static const size_t kNotifyThreshold = 128;  // 큐가 이만큼 쌓이면 즉시 flush 깨움
static const size_t kMaxQueue = 200000;      // 큐 상한 (스풀까지 막힌 극단 상황의 메모리 폭주 방지)
static const int kFlushIntervalMs = 100;     // 주기 flush (버퍼 잔여분 보장)
static const size_t kNasQueueMax = 8;        // dispatch→flusher 대기 배치 상한 (포화 = 저장 경로 지연 신호)
static const int kReplayRetryMs = 2000;      // 스풀 재생 실패 후 재시도 간격
static const int kSpoolTrimIntervalMs = 5000;  // 스풀 용량 정리 최소 간격
static const int kStopFlusherWaitMs = 2000;    // 정지 시 flusher 종료 대기 상한 (초과 시 detach)

CSipMessageLogger::CSipMessageLogger()
    : m_bEnabled( false ),
      m_bRawLogEnabled( true ),
      m_iSipSeq( -1 ),
      m_iCmpSeq( -1 ),
      m_iCscSeq( -1 ),
      m_bWriterRunning( false ),
      m_ulDroppedLogs( 0 ),
      m_bSeedApplied( false ),
      m_bStoreAlarmOpen( false ),
      m_bStoreDegraded( false ),
      m_llLastSpoolTrimMs( 0 ) {
}

CSipMessageLogger::~CSipMessageLogger() {
    StopWriter();  // 잔여 큐 스풀 회수 후 dispatch 조인 (flusher 는 갇혀 있으면 detach)
}

void CSipMessageLogger::Init( const std::string &strFlowBaseDir, const std::string &strMsgBaseDir,
                              const std::string &strSystemId, bool bRawLogEnabled, const std::string &strSpoolDir,
                              int iStallSec, int iSpoolMaxMb ) {
    if ( strFlowBaseDir.empty() && strMsgBaseDir.empty() ) return;
    m_strFlowBaseDir = strFlowBaseDir;
    m_strMsgBaseDir = strMsgBaseDir;
    m_strSystemId = strSystemId.empty() ? "csp_01" : strSystemId;
    // node 필드용: "csp_01" → "csp" (언더스코어+숫자 제거)
    m_strNodeName = m_strSystemId;
    auto upos = m_strNodeName.find( '_' );
    if ( upos != std::string::npos ) m_strNodeName = m_strNodeName.substr( 0, upos );
    m_bRawLogEnabled = bRawLogEnabled;

    // 저장 경로(NAS 가능) I/O 는 전부 flusher 스레드로 — 여기서는 로컬 스풀만 만진다.
    m_ctx = std::make_shared<StoreCtx>();
    m_ctx->strSpoolDir = strSpoolDir.empty() ? "spool" : strSpoolDir;
    m_ctx->strFlowBaseDir = m_strFlowBaseDir;
    m_ctx->strMsgBaseDir = m_strMsgBaseDir;
    m_ctx->iStallMs = ( iStallSec > 0 ? iStallSec : 5 ) * 1000;
    m_ctx->llSpoolMaxBytes = (long long)( iSpoolMaxMb > 0 ? iSpoolMaxMb : 1024 ) * 1024 * 1024;
    MkdirP( m_ctx->strSpoolDir );

    // 시딩 대상: 기동 시점 버킷의 iface msg 파일 (재기동 seq 연속성 — flusher 가 비동기 계수)
    m_ctx->strSeedBucketKey = GetMsgHourDir() + "/" + BucketSuffix();
    static const char *arrIfaces[3] = { "sip", "cmp", "csc" };
    for ( int i = 0; i < 3; i++ ) m_ctx->strSeedPath[i] = MsgFilePath( arrIfaces[i] );

    // 이전 run 스풀 잔량 스캔 (로컬) — 잔량이 있으면 드레인 전 직행 금지 (경로별 줄 순서 보존)
    long long llBytes = 0;
    bool bPending = false;
    ScanSpool( m_ctx->strSpoolDir, [&]( const std::string &, time_t, long long llSize ) {
        llBytes += llSize;
        bPending = true;
    } );
    m_ctx->llSpoolBytes.store( llBytes );
    m_ctx->bSpoolPending.store( bPending );

    m_bEnabled = true;
    StartWriter();  // dispatch + NAS flusher 기동 (활성화 시 1회)
}

void CSipMessageLogger::LogSecurity( const char *pszPeer, const char *pszMethod, const char *pszCaller,
                                     const char *pszCallee, const char *pszUa, const char *pszCallId,
                                     const char *pszReasons, bool bRegisteredCaller ) {
    if ( !m_bEnabled ) return;
    std::string strTs = GetTimestamp();
    std::string strMsgHourDir = GetMsgHourDir();
    std::lock_guard<std::mutex> lock( m_mtx );
    RotateBucket( strMsgHourDir );
    std::string strPath = strMsgHourDir + "/" + m_strSystemId + ".security." + BucketSuffix() + ".jsonl";
    std::string strLine = "{\"ts\":\"" + strTs + "\"";
    strLine += ",\"peer\":\"" + JsonEsc( pszPeer ) + "\"";
    strLine += ",\"method\":\"" + JsonEsc( pszMethod ) + "\"";
    strLine += ",\"caller\":\"" + JsonEsc( pszCaller ) + "\"";
    strLine += ",\"callee\":\"" + JsonEsc( pszCallee ) + "\"";
    strLine += ",\"ua\":\"" + JsonEsc( pszUa ) + "\"";
    strLine += ",\"call_id\":\"" + JsonEsc( pszCallId ) + "\"";
    strLine += ",\"reasons\":\"" + JsonEsc( pszReasons ) + "\"";
    strLine += ",\"registered_caller\":";
    strLine += ( bRegisteredCaller ? "true" : "false" );
    strLine += "}\n";
    EnqueueLine( strPath, std::move( strLine ) );
}

void CSipMessageLogger::SetCallSesId( const std::string &strCallId, const std::string &strSesId,
                                      const std::string &strSubId ) {
    std::lock_guard<std::mutex> lock( m_mtx );
    if ( !strCallId.empty() && !strSesId.empty() ) {
        m_mapCallSesId[strCallId] = strSesId;
    }
    if ( !strCallId.empty() && !strSubId.empty() ) {
        m_mapCallSubId[strCallId] = strSubId;
    }
}

std::string CSipMessageLogger::IssueSesId( const std::string &strCaller, const char *pszModule ) {
    // 포맷: {caller}::{module}::{yyyymmddHHMMSSuuuuuu}::{counter}
    // counter: 동일 us_ts가 연속될 때 1씩 증가
    static std::mutex sMtx;
    static std::string sLastTs;
    static unsigned int sCounter = 0;

    struct timeval tv;
    gettimeofday( &tv, NULL );
    struct tm t;
    localtime_r( &tv.tv_sec, &t );
    char tsBuf[32];
    snprintf( tsBuf, sizeof( tsBuf ), "%04d%02d%02d%02d%02d%02d%06ld", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
              t.tm_hour, t.tm_min, t.tm_sec, (long)tv.tv_usec );

    unsigned int counter;
    std::string ts( tsBuf );
    {
        std::lock_guard<std::mutex> lock( sMtx );
        if ( ts == sLastTs ) {
            ++sCounter;
        } else {
            sLastTs = ts;
            sCounter = 1;
        }
        counter = sCounter;
    }

    const char *mod = ( pszModule && pszModule[0] ) ? pszModule : "csp";
    std::string r;
    r.reserve( strCaller.size() + 50 );
    r.append( strCaller );
    r.append( "::" );
    r.append( mod );
    r.append( "::" );
    r.append( ts );
    r.append( "::" );
    r.append( std::to_string( counter ) );
    return r;
}

std::string CSipMessageLogger::IssueUniqueId( const char *pszIssuer ) {
    // 포맷: {issuer}_{yyyymmddHHMMSSmmm}_{index} — 원격 프로세스에 상태로 남는 ID 전용.
    // index: 동일 ms 타임스탬프가 연속될 때 1씩 증가
    static std::mutex sMtx;
    static std::string sLastTs;
    static unsigned int sCounter = 0;

    struct timeval tv;
    gettimeofday( &tv, NULL );
    struct tm t;
    localtime_r( &tv.tv_sec, &t );
    char tsBuf[32];
    snprintf( tsBuf, sizeof( tsBuf ), "%04d%02d%02d%02d%02d%02d%03ld", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
              t.tm_hour, t.tm_min, t.tm_sec, (long)( tv.tv_usec / 1000 ) );

    unsigned int counter;
    std::string ts( tsBuf );
    {
        std::lock_guard<std::mutex> lock( sMtx );
        if ( ts == sLastTs ) {
            ++sCounter;
        } else {
            sLastTs = ts;
            sCounter = 1;
        }
        counter = sCounter;
    }

    std::string r;
    r.reserve( ts.size() + 24 );
    r.append( pszIssuer && pszIssuer[0] ? pszIssuer : "csp" );
    r.append( "_" );
    r.append( ts );
    r.append( "_" );
    r.append( std::to_string( counter ) );
    return r;
}

std::string CSipMessageLogger::GetOrIssueSesId( const std::string &strCallId, const std::string &strCaller ) {
    std::lock_guard<std::mutex> lock( m_mtx );
    if ( !strCallId.empty() ) {
        auto it = m_mapCallSesId.find( strCallId );
        if ( it != m_mapCallSesId.end() && !it->second.empty() ) return it->second;
    }
    std::string sid = IssueSesId( strCaller, "csp" );
    if ( !strCallId.empty() ) {
        // Call-ID 캐시 크기 제한
        if ( m_mapCallSesId.size() >= MAX_CALLID_CACHE ) {
            m_mapCallSesId.erase( m_mapCallSesId.begin() );
        }
        m_mapCallSesId[strCallId] = sid;
    }
    return sid;
}

std::string CSipMessageLogger::GetSesIdByCallId( const std::string &strCallId ) {
    std::lock_guard<std::mutex> lock( m_mtx );
    auto it = m_mapCallSesId.find( strCallId );
    if ( it != m_mapCallSesId.end() ) return it->second;
    return "";
}

void CSipMessageLogger::SetDomainServiceMap( const std::map<std::string, std::string> &mapDomainToService ) {
    // 초기화 시점(CspServer 시작 직후)에만 호출되므로 락 없이 안전.
    // 이후 Print() 에서 m_mtx 보호 하에 read-only 로만 접근.
    m_mapDomainToService = mapDomainToService;
}

/**
 * 호스트 문자열에서 도메인 부분 추출.
 * "<sip:user@host:port>;tag=X" 혹은 "sip:user@host" 등 다양한 패턴 수용.
 */
static std::string ExtractDomainFromUri( const std::string &strUri ) {
    if ( strUri.empty() ) return "";

    // Skip leading "<" and scheme (sip:/sips:/tel:)
    size_t start = 0;
    if ( strUri[0] == '<' ) start = 1;

    size_t colon = strUri.find( ':', start );
    if ( colon == std::string::npos ) return "";
    start = colon + 1;

    // 찾을 끝: ';' '>' ',' ' ' '\r' '\n'
    size_t end = strUri.size();
    for ( size_t i = start; i < strUri.size(); ++i ) {
        char c = strUri[i];
        if ( c == ';' || c == '>' || c == ',' || c == ' ' || c == '\r' || c == '\n' ) {
            end = i;
            break;
        }
    }

    // user@host 에서 host 추출
    std::string uriBody = strUri.substr( start, end - start );
    size_t at = uriBody.find( '@' );
    std::string hostPort = ( at != std::string::npos ) ? uriBody.substr( at + 1 ) : uriBody;

    // host:port 에서 host
    size_t pcolon = hostPort.find( ':' );
    if ( pcolon != std::string::npos ) hostPort = hostPort.substr( 0, pcolon );

    return hostPort;
}

std::string CSipMessageLogger::ClassifyService( const char *pszMsg, const std::string &strCallId,
                                                const std::string &strMethod ) {
    // 주의: 이 함수는 Print()에서 m_mtx 를 이미 잡은 상태에서 호출되므로
    //       내부에서 m_mtx 를 재획득하지 않는다 (std::mutex 는 재귀 불가).
    if ( strMethod == "OPTIONS" || strMethod == "HEARTBEAT" ) {
        return "system";
    }

    bool bIsResponse = ( pszMsg && strncmp( pszMsg, "SIP/2.0", 7 ) == 0 );

    if ( !bIsResponse && pszMsg ) {
        // Request-URI 도메인 추출 (첫 줄: "METHOD sip:user@domain SIP/2.0")
        std::string strReqUri;
        const char *pFirstSpace = strchr( pszMsg, ' ' );
        if ( pFirstSpace ) {
            const char *pEnd = pFirstSpace + 1;
            while ( *pEnd && *pEnd != ' ' && *pEnd != '\r' && *pEnd != '\n' ) pEnd++;
            strReqUri = std::string( pFirstSpace + 1, pEnd );
        }
        std::string strToRaw = ExtractHeader( pszMsg, "To:", "t:" );
        std::string strFromRaw = ExtractHeader( pszMsg, "From:", "f:" );

        std::string strReqDomain = ExtractDomainFromUri( strReqUri );
        std::string strToDomain = ExtractDomainFromUri( strToRaw );
        std::string strFromDomain = ExtractDomainFromUri( strFromRaw );

        // Request-URI → To → From 순서로 lookup 하여 첫 매치 반환.
        // TX/RX 판별은 Print()에서 메시지 이후에 결정되므로 여기는 공통 로직.
        std::string strService;
        auto it = m_mapDomainToService.find( strReqDomain );
        if ( it == m_mapDomainToService.end() ) it = m_mapDomainToService.find( strToDomain );
        if ( it == m_mapDomainToService.end() ) it = m_mapDomainToService.find( strFromDomain );
        if ( it != m_mapDomainToService.end() ) strService = it->second;

        if ( strService.empty() ) {
            // 도메인 미등록 → 빈값 ("" — flow에서 service key 생략)
            return "";
        }

        // Cache for subsequent responses with same Call-ID
        if ( !strCallId.empty() ) {
            if ( m_mapCallService.size() > MAX_CALLID_CACHE ) {
                auto itE = m_mapCallService.begin();
                size_t half = m_mapCallService.size() / 2;
                for ( size_t i = 0; i < half && itE != m_mapCallService.end(); ++i ) {
                    itE = m_mapCallService.erase( itE );
                }
            }
            m_mapCallService[strCallId] = strService;
        }

        return strService;
    }

    // Response: Call-ID 캐시에서 조회
    if ( !strCallId.empty() ) {
        auto it = m_mapCallService.find( strCallId );
        if ( it != m_mapCallService.end() ) return it->second;
    }

    return "";
}

/**
 * ILogCallBack::Print - called from CLog with the already-formatted message.
 */
void CSipMessageLogger::Print( EnumLogLevel eLevel, const char *fmt, ... ) {
    if ( !m_bEnabled ) return;
    if ( eLevel != LOG_NETWORK ) return;

    // Format the string from CLog callback
    char szBuf[1024 * 8];
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( szBuf, sizeof( szBuf ) - 1, fmt, ap );
    va_end( ap );
    szBuf[sizeof( szBuf ) - 1] = '\0';

    // Skip the "[threadid] " prefix
    const char *pszMsg = szBuf;
    if ( *pszMsg == '[' ) {
        const char *p = strchr( pszMsg, ']' );
        if ( p ) {
            pszMsg = p + 1;
            while ( *pszMsg == ' ' ) pszMsg++;
        }
    }

    // Determine direction: UdpSend = TX, UdpRecv/TcpRecv = RX
    const char *pszDir = NULL;
    const char *pszAfterParen = NULL;

    if ( strncmp( pszMsg, "UdpSend(", 8 ) == 0 || strncmp( pszMsg, "TcpSend(", 8 ) == 0 ||
         strncmp( pszMsg, "TlsSend(", 8 ) == 0 ) {
        pszDir = "TX";
        pszAfterParen = pszMsg + 8;
    } else if ( strncmp( pszMsg, "UdpRecv(", 8 ) == 0 || strncmp( pszMsg, "TcpRecv(", 8 ) == 0 ||
                strncmp( pszMsg, "TlsRecv(", 8 ) == 0 ) {
        pszDir = "RX";
        pszAfterParen = pszMsg + 8;
    } else {
        return;
    }

    // Extract peer IP:PORT from "IP:PORT) ..."
    char szPeer[64] = { 0 };
    const char *pClose = strchr( pszAfterParen, ')' );
    if ( pClose && ( pClose - pszAfterParen ) < (int)sizeof( szPeer ) ) {
        strncpy( szPeer, pszAfterParen, pClose - pszAfterParen );
        szPeer[pClose - pszAfterParen] = '\0';
    }

    // Find the SIP message body
    const char *pszSipMsg = NULL;
    if ( pClose ) {
        pszSipMsg = pClose + 1;
        while ( *pszSipMsg == ' ' || *pszSipMsg == '\r' || *pszSipMsg == '\n' || *pszSipMsg == '[' ) pszSipMsg++;
    }

    if ( !pszSipMsg || *pszSipMsg == '\0' ) return;

    // Extract SIP headers
    std::string strCallId = ExtractHeader( pszSipMsg, "Call-ID:", "i:" );
    std::string strMethod = ExtractMethodOrStatus( pszSipMsg );
    std::string strFromRaw = ExtractHeader( pszSipMsg, "From:", "f:" );
    std::string strToRaw = ExtractHeader( pszSipMsg, "To:", "t:" );
    std::string strFromUri = ExtractUriUser( strFromRaw );
    std::string strToUri = ExtractUriUser( strToRaw );
    std::string strCSeq = ExtractHeader( pszSipMsg, "CSeq:", NULL );

    std::string strTs = GetTimestamp();
    std::string strMsgHourDir = GetMsgHourDir();

    std::lock_guard<std::mutex> lock( m_mtx );
    RotateBucket( strMsgHourDir );

    // Classify service from SIP message domain
    std::string strService = ClassifyService( pszSipMsg, strCallId, strMethod );

    // Clean up Call-ID cache on BYE/CANCEL
    if ( strMethod == "BYE" || strMethod == "CANCEL" ) {
        // Deferred cleanup: leave in cache for responses, will be evicted by size limit
    }

    // SIP: TX=csp->ue, RX=ue->csp
    const char *pszFrom = ( strcmp( pszDir, "TX" ) == 0 ) ? "csp" : "ue";
    const char *pszTo = ( strcmp( pszDir, "TX" ) == 0 ) ? "ue" : "csp";

    // sesid 조회/발행 (msg.jsonl 기록 전에 결정) — 같은 Call-ID 는 동일 sesid 유지.
    //   REGISTER/INVITE/SUBSCRIBE 등 모든 SIP 메시지에 발행/계승.
    std::string strSesId;
    if ( !strCallId.empty() ) {
        auto itSes = m_mapCallSesId.find( strCallId );
        if ( itSes != m_mapCallSesId.end() && !itSes->second.empty() ) {
            strSesId = itSes->second;
        } else {
            // 신규 발행: caller = From URI (있을 때)
            strSesId = IssueSesId( strFromUri, "csp" );
            if ( m_mapCallSesId.size() >= MAX_CALLID_CACHE ) {
                m_mapCallSesId.erase( m_mapCallSesId.begin() );
            }
            m_mapCallSesId[strCallId] = strSesId;
        }
    } else {
        // Call-ID 없는 경우(드문 상황): 일회성 sesid
        strSesId = IssueSesId( strFromUri, "csp" );
    }

    // 1. {system_id}_sip.jsonl (per-interface) — sesid embed (G7, 2026-04-23)
    int iSeq = 0;
    if ( m_bRawLogEnabled ) {
        iSeq = WriteInterfaceLine( "sip", strTs.c_str(), pszDir, szPeer, "SIP", pszSipMsg, strFromUri.c_str(),
                                   strToUri.c_str(), strSesId.c_str() );
    }

    // SIP은 CSP 단독 기록이므로 mid 불필요

    // detail 생성: INVITE=from→to, REGISTER=user, 응답=빈값
    std::string strDetail;
    if ( strMethod == "INVITE" && !strFromUri.empty() && !strToUri.empty() ) {
        strDetail = strFromUri + "\xe2\x86\x92" + strToUri;  // UTF-8 →
    } else if ( strMethod == "REGISTER" && !strFromUri.empty() ) {
        strDetail = strFromUri;
    } else if ( strMethod == "SUBSCRIBE" && !strToUri.empty() ) {
        strDetail = strToUri;
    } else if ( strMethod == "BYE" && !strFromUri.empty() ) {
        strDetail = strFromUri;
    }

    // subid: VoLTE=Call-ID(leg 구분), PTT=session_seq(캐시 조회)
    //   v3 (2026-04-22): AccessService.kind = "volte" 사용. 기존 "volte"/"volte" 도 호환.
    std::string strSubId;
    if ( strService == "volte" ) {
        strSubId = strCallId;
    } else {
        auto itSub = m_mapCallSubId.find( strCallId );
        if ( itSub != m_mapCallSubId.end() ) strSubId = itSub->second;
    }

    // 2. flow.jsonl (SIP: mid 불필요)
    //    caller/callee는 From/To URI에서 추출 (Request/Response 모두 동일하게 From=originator, To=target)
    WriteFlowLine( strService.c_str(), strTs.c_str(), pszFrom, pszTo, "SIP", strMethod.c_str(), strDetail.c_str(), "",
                   strSesId.c_str(), strSubId.c_str(), iSeq, "sip", strFromUri.c_str(), strToUri.c_str() );
}

void CSipMessageLogger::LogMessage( const char *pszFrom, const char *pszTo, const char *pszProto, const char *pszMethod,
                                    const char *pszPeer, const char *pszBody, const char *pszService,
                                    const char *pszTxId, const char *pszSesId, const char *pszDetail,
                                    const char *pszCaller, const char *pszCallee ) {
    if ( !m_bEnabled ) return;

    std::string strTs = GetTimestamp();
    const char *proto = ( pszProto && *pszProto ) ? pszProto : "JSON";
    const char *pszDir = ( pszFrom && strcmp( pszFrom, "csp" ) == 0 ) ? "TX" : "RX";
    const char *service = ( pszService && *pszService ) ? pszService : "";
    std::string strMsgHourDir = GetMsgHourDir();

    // Determine interface from proto
    const char *iface = "cmp";
    if ( proto && strcmp( proto, "CSC" ) == 0 ) {
        iface = "csc";
    } else if ( proto && strcmp( proto, "SIP" ) == 0 ) {
        iface = "sip";
    }

    std::lock_guard<std::mutex> lock( m_mtx );
    RotateBucket( strMsgHourDir );

    // 1. {system_id}_{iface}.jsonl (per-interface) — G7 sesid embed
    int iSeq = 0;
    if ( m_bRawLogEnabled ) {
        iSeq = WriteInterfaceLine( iface, strTs.c_str(), pszDir, pszPeer ? pszPeer : "", proto, pszBody ? pszBody : "",
                                   pszCaller, pszCallee, pszSesId ? pszSesId : "" );
    }

    // 2. flow.jsonl
    WriteFlowLine( service, strTs.c_str(), pszFrom ? pszFrom : "", pszTo ? pszTo : "", proto,
                   pszMethod ? pszMethod : "", pszDetail ? pszDetail : "", pszTxId ? pszTxId : "",
                   pszSesId ? pszSesId : "", "", iSeq, iface, pszCaller, pszCallee );
}

void CSipMessageLogger::RotateBucket( const std::string &strMsgHourDir ) {
    // Called under m_mtx lock. 5분 버킷 회전 — 순수 북키핑. 파일시스템 무접촉:
    //   디렉터리 생성은 flusher 가 기록 직전에, 기존 줄 계수(시딩)는 flusher 가 기동 시 1회 수행.
    //   run 도중 새로 열리는 버킷은 새 파일명이라 기존 줄이 있을 수 없으므로 seq=0 에서 시작한다.
    //   기동 첫 버킷만 -1(시딩 대기)로 두어 flusher 계수 결과가 합류하게 한다 — 합류 전에
    //   생산자 write 가 먼저 오면 0 부터 시작한다 (seq 어긋남은 리더의 sesid/내용 폴백이 흡수).
    std::string strBucketKey = strMsgHourDir + "/" + BucketSuffix();
    if ( strBucketKey == m_strCurrentBucketKey ) return;
    bool bFirstRotation = m_strCurrentBucketKey.empty();
    m_strCurrentBucketKey = strBucketKey;
    if ( bFirstRotation && m_ctx && strBucketKey == m_ctx->strSeedBucketKey ) return;  // 시딩 대기(-1) 유지
    m_iSipSeq = 0;
    m_iCmpSeq = 0;
    m_iCscSeq = 0;
}

std::string CSipMessageLogger::BucketSuffix() {
    time_t now = time( NULL );
    struct tm t;
    localtime_r( &now, &t );
    char buf[8];
    snprintf( buf, sizeof( buf ), "%02d", ( t.tm_min / 5 ) * 5 );  // 00,05,10,...,55
    return buf;
}

std::string CSipMessageLogger::FlowFilePath() {
    std::string dir = GetFlowHourDir();
    if ( dir.empty() ) return "";
    return dir + "/" + m_strSystemId + ".flow." + BucketSuffix() + ".jsonl";
}

std::string CSipMessageLogger::MsgFilePath( const char *pszIface ) {
    std::string dir = GetMsgHourDir();
    if ( dir.empty() ) return "";
    return dir + "/" + m_strSystemId + "_" + ( pszIface ? pszIface : "sip" ) + ".msg." + BucketSuffix() + ".jsonl";
}

int CSipMessageLogger::CountFileLines( const std::string &path ) {
    FILE *f = fopen( path.c_str(), "r" );
    if ( !f ) return 0;
    // 개행 계수 — SIP 원문 줄은 수 KB 를 넘으므로 fgets 반복 계수는 과계수한다.
    int n = 0;
    char buf[65536];
    size_t r;
    while ( ( r = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) {
        for ( size_t i = 0; i < r; i++ ) {
            if ( buf[i] == '\n' ) n++;
        }
    }
    fclose( f );
    return n;
}

int &CSipMessageLogger::GetIfaceSeq( const char *pszIface ) {
    if ( strcmp( pszIface, "cmp" ) == 0 ) return m_iCmpSeq;
    if ( strcmp( pszIface, "csc" ) == 0 ) return m_iCscSeq;
    return m_iSipSeq;  // default/sip
}

void CSipMessageLogger::WriteFlowLine( const char *pszService, const char *pszTs, const char *pszFrom,
                                       const char *pszTo, const char *pszProto, const char *pszMethod,
                                       const char *pszDetail, const char *pszTxId, const char *pszSesId,
                                       const char *pszSubId, int iSeq, const char *pszIface, const char *pszCaller,
                                       const char *pszCallee ) {
    std::string strPath = FlowFilePath();
    if ( strPath.empty() ) return;

    // 파일 I/O 없이 한 줄을 메모리에 포맷한 뒤 비동기 writer 큐로 적재 (open-per-write 제거).
    // 순서: ts, service, caller, callee, sesid, subid, node, from, to,
    //       proto, method, detail, mid, seq, iface — 빈 값은 key 생략.
    std::string line;
    line.reserve( 256 );
    line += '{';
    bool bFirst = true;
    auto emit = [&]( const char *key, const std::string &val, bool isNumeric = false, int iNum = 0 ) {
        if ( !isNumeric && val.empty() ) return;
        if ( !bFirst ) line += ',';
        bFirst = false;
        line += '"';
        line += key;
        line += "\":";
        if ( isNumeric ) {
            line += std::to_string( iNum );
        } else {
            line += '"';
            line += val;
            line += '"';
        }
    };

    emit( "ts", pszTs ? pszTs : "" );
    emit( "service", pszService ? pszService : "" );
    emit( "caller", pszCaller ? JsonEsc( pszCaller ) : "" );
    emit( "callee", pszCallee ? JsonEsc( pszCallee ) : "" );
    emit( "sesid", pszSesId ? JsonEsc( pszSesId ) : "" );
    emit( "subid", pszSubId ? JsonEsc( pszSubId ) : "" );
    emit( "node", m_strNodeName );
    emit( "from", pszFrom ? pszFrom : "" );
    emit( "to", pszTo ? pszTo : "" );
    emit( "proto", pszProto ? pszProto : "" );
    emit( "method", pszMethod ? JsonEsc( pszMethod ) : "" );
    emit( "detail", pszDetail ? JsonEsc( pszDetail ) : "" );
    emit( "mid", pszTxId ? JsonEsc( pszTxId ) : "" );
    if ( iSeq > 0 ) emit( "seq", "", true, iSeq );
    emit( "iface", pszIface ? pszIface : "" );
    line += "}\n";
    EnqueueLine( strPath, std::move( line ) );
}

int CSipMessageLogger::WriteInterfaceLine( const char *pszIface, const char *pszTs, const char *pszDir,
                                           const char *pszPeer, const char *pszProto, const char *pszMsg,
                                           const char *pszCaller, const char *pszCallee, const char *pszSesId ) {
    // Called under m_mtx lock
    std::string strPath = MsgFilePath( pszIface );
    if ( strPath.empty() ) return 0;

    int &iSeq = GetIfaceSeq( pszIface );
    // -1 = 기동 첫 버킷의 시딩(flusher 비동기 계수) 미도착 — 여기서 저장 경로를 읽지 않는다
    //   (생산자 무접촉 계약). 0 부터 시작하고 어긋남은 리더의 sesid/내용 폴백이 흡수한다.
    if ( iSeq < 0 ) iSeq = 0;

    std::string strEscMsg = JsonEsc( pszMsg );
    iSeq++;

    // 파일 I/O 없이 한 줄을 메모리에 포맷한 뒤 비동기 writer 큐로 적재.
    // 순서: ts, dir, peer, caller, callee, sesid, proto, msg
    std::string line;
    line.reserve( strEscMsg.size() + 128 );
    line += "{\"ts\":\"";
    line += ( pszTs ? pszTs : "" );
    line += "\",\"dir\":\"";
    line += ( pszDir ? pszDir : "" );
    line += "\",\"peer\":\"";
    line += ( pszPeer ? pszPeer : "" );
    line += '"';
    if ( pszCaller && pszCaller[0] ) {
        line += ",\"caller\":\"";
        line += JsonEsc( pszCaller );
        line += '"';
    }
    if ( pszCallee && pszCallee[0] ) {
        line += ",\"callee\":\"";
        line += JsonEsc( pszCallee );
        line += '"';
    }
    if ( pszSesId && pszSesId[0] ) {
        line += ",\"sesid\":\"";
        line += JsonEsc( pszSesId );
        line += '"';
    }
    line += ",\"proto\":\"";
    line += ( pszProto ? pszProto : "" );
    line += "\",\"msg\":\"";
    line += strEscMsg;
    line += "\"}\n";
    EnqueueLine( strPath, std::move( line ) );

    return iSeq;
}

// ─────────────────────────────────────────────────────────────────────────────
// 비동기 배치 writer — dispatch + NAS flusher 2단
//   생산자(Print/LogMessage)는 m_mtx 보유 중 포맷한 한 줄을 EnqueueLine 으로 적재만 하고
//   즉시 반환한다(파일 I/O 없음). dispatch 스레드가 주기/임계마다 큐를 비워:
//     - 저장소 건강 + 스풀 잔량 없음 → flusher 큐(nasQueue)로 직행
//     - 아니면 → 로컬 스풀 미러 파일에 append (dispatch 는 NFS 무접촉 — 항상 join 가능)
//   NAS flusher 스레드만 저장 경로(NAS 가능) I/O 를 수행한다. hard NFS mount 는 쓰기가
//   실패하는 대신 무기한 행이므로, 정체(in-flight > StallSec)를 dispatch 가 감지해 폴백을
//   건다 — 갇히는 스레드는 flusher 하나뿐이고 SIP/제어 평면은 계속 돈다.
//   회복 시 flusher 가 스풀을 오래된 파일부터 저장 경로로 재생(replay)한 뒤 직행 복귀 —
//   경로별 줄 순서 = enqueue(=seq) 순서가 유지되어 flow.seq → msg 줄번호 정합 보존.
//   (스풀 재생 중 crash 는 재생분 중복(at-least-once) 가능 — 리더의 sesid/내용 매칭이 흡수.)
// ─────────────────────────────────────────────────────────────────────────────
long long CSipMessageLogger::NowMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch() )
        .count();
}

void CSipMessageLogger::EnqueueLine( const std::string &strPath, std::string &&strLine ) {
    if ( strPath.empty() || strLine.empty() ) return;
    bool bNotify = false;
    {
        std::lock_guard<std::mutex> lk( m_qMtx );
        if ( m_logQueue.size() >= kMaxQueue ) {
            // backlog 상한 초과(로컬 스풀까지 막힌 극단 상황) — 가장 오래된 줄을 버려 메모리 폭주 방지.
            m_logQueue.pop_front();
            m_ulDroppedLogs.fetch_add( 1 );
        }
        m_logQueue.push_back( LogItem{ strPath, std::move( strLine ) } );
        if ( m_logQueue.size() >= kNotifyThreshold ) bNotify = true;
    }
    if ( bNotify ) m_qCv.notify_one();
}

void CSipMessageLogger::WriterLoop() {
    const auto interval = std::chrono::milliseconds( kFlushIntervalMs );
    StoreCtx &ctx = *m_ctx;
    while ( m_bWriterRunning.load() ) {
        std::deque<LogItem> batch;
        {
            std::unique_lock<std::mutex> lk( m_qMtx );
            m_qCv.wait_for( lk, interval, [this] { return !m_logQueue.empty() || !m_bWriterRunning.load(); } );
            batch.swap( m_logQueue );
        }
        ApplySeedIfPending();

        // 정체 감지: flusher 가 저장 경로 op 에 StallSec 이상 갇혀 있으면 무응답 판정.
        long long llOpStart = ctx.llOpStartMs.load();
        if ( llOpStart != 0 && NowMs() - llOpStart > ctx.iStallMs && ctx.bNasHealthy.load() ) {
            ctx.bNasHealthy.store( false );
            ctx.bLastOpOk.store( false );
            std::lock_guard<std::mutex> lk( ctx.mtx );
            ctx.strLastError = "stall: store op in-flight > " + std::to_string( ctx.iStallMs ) + "ms";
        }

        if ( !batch.empty() ) {
            if ( !RouteBatch( batch, false ) ) {
                // flusher 큐 포화 역압 — 배치는 m_logQueue 앞으로 되돌아갔다. 한 tick 쉰다
                //   (스풀로 우회하면 큐 잔량보다 새 줄이 먼저 스풀에 앉아 경로별 순서가 깨진다).
                std::this_thread::sleep_for( interval );
            }
        }
        ReconcileStoreAlarm();
    }
    // 종료 — 잔여 큐 회수. 역압이면 스풀로 강제 회수해 어떤 경우에도 막히지 않는다.
    for ( ;; ) {
        std::deque<LogItem> batch;
        {
            std::lock_guard<std::mutex> lk( m_qMtx );
            batch.swap( m_logQueue );
        }
        if ( batch.empty() ) break;
        RouteBatch( batch, true );
    }
}

bool CSipMessageLogger::RouteBatch( std::deque<LogItem> &batch, bool bForceSpoolOnBackpressure ) {
    if ( batch.empty() ) return true;
    StoreCtx &ctx = *m_ctx;
    bool bFallback = !ctx.bNasHealthy.load() || ctx.bSpoolPending.load();
    if ( !bFallback ) {
        bool bQueued = false;
        {
            std::lock_guard<std::mutex> lk( ctx.mtx );
            if ( ctx.nasQueue.size() < kNasQueueMax ) {
                ctx.nasQueue.push_back( std::move( batch ) );
                bQueued = true;
            }
        }
        if ( bQueued ) {
            ctx.cv.notify_one();
            return true;
        }
        if ( !bForceSpoolOnBackpressure ) {
            // 큐 포화 — 순서 보존을 위해 m_logQueue 앞으로 되돌린다 (다음 tick 재시도).
            std::lock_guard<std::mutex> lk( m_qMtx );
            for ( auto it = batch.rbegin(); it != batch.rend(); ++it ) {
                m_logQueue.push_front( std::move( *it ) );
            }
            batch.clear();
            return false;
        }
    }
    // 폴백 — flusher 큐 잔량(현재 배치보다 오래된 줄)부터 회수해 경로별 순서를 지킨다.
    ReclaimNasQueueToSpool();
    SpoolBatch( batch );
    return true;
}

void CSipMessageLogger::ReclaimNasQueueToSpool() {
    StoreCtx &ctx = *m_ctx;
    std::deque<std::deque<LogItem>> pending;
    {
        std::lock_guard<std::mutex> lk( ctx.mtx );
        pending.swap( ctx.nasQueue );
    }
    for ( auto &b : pending ) SpoolBatch( b );
}

void CSipMessageLogger::ApplySeedIfPending() {
    // flusher 의 기동 시딩(기존 줄 계수) 합류 — 생산자 write 가 아직 없을 때(-1)만 유효.
    if ( m_bSeedApplied || !m_ctx || !m_ctx->bSeedDone.load() ) return;
    std::lock_guard<std::mutex> lock( m_mtx );
    m_bSeedApplied = true;
    if ( !m_strCurrentBucketKey.empty() && m_strCurrentBucketKey != m_ctx->strSeedBucketKey ) return;  // 버킷 지남
    if ( m_iSipSeq < 0 ) m_iSipSeq = (int)m_ctx->llSeedCount[0];
    if ( m_iCmpSeq < 0 ) m_iCmpSeq = (int)m_ctx->llSeedCount[1];
    if ( m_iCscSeq < 0 ) m_iCscSeq = (int)m_ctx->llSeedCount[2];
}

void CSipMessageLogger::ReconcileStoreAlarm() {
    // 폴백 상태 전이의 단일 소유자 — FM 알람(A-PRC-006)과 로컬 로그 모두 dispatch 만 만진다
    //   (flusher 는 detach 될 수 있어 전역 싱글턴 호출을 맡기지 않는다).
    StoreCtx &ctx = *m_ctx;
    bool bDegraded = !ctx.bNasHealthy.load() || ctx.bSpoolPending.load();

    if ( bDegraded != m_bStoreDegraded ) {
        m_bStoreDegraded = bDegraded;
        std::string strReason;
        {
            std::lock_guard<std::mutex> lk( ctx.mtx );
            strReason = ctx.strLastError;
        }
        if ( bDegraded ) {
            CLog::Print( LOG_ERROR, "service_log store fallback engaged (dir=%s, reason=%s, spool=%s)",
                         m_strMsgBaseDir.c_str(), strReason.empty() ? "spool backlog" : strReason.c_str(),
                         ctx.strSpoolDir.c_str() );
        } else {
            CLog::Print( LOG_SYSTEM,
                         "service_log store recovered — spool drained (spooled=%lu, replayed=%lu, dropped=%lu)",
                         ctx.ulSpooledLines.load(), ctx.ulReplayedLines.load(), ctx.ulDroppedLines.load() );
        }
    }

    if ( gclsFmReporter.IsEnabled() ) {
        const std::string strMo = gclsFmReporter.Node() + "/csp/service_log";
        if ( bDegraded && !m_bStoreAlarmOpen ) {
            std::string strReason;
            {
                std::lock_guard<std::mutex> lk( ctx.mtx );
                strReason = ctx.strLastError;
            }
            SimpleJson::JsonNode nodeParams;
            nodeParams.Set( "path", m_strMsgBaseDir.c_str() );
            nodeParams.Set( "reason", strReason.empty() ? "spool backlog" : strReason.c_str() );
            nodeParams.Set( "spooled", (int)ctx.ulSpooledLines.load() );
            nodeParams.Set( "dropped", (int)ctx.ulDroppedLines.load() );
            gclsFmReporter.AlarmOpen( "A-PRC-006", strMo, nodeParams );
            m_bStoreAlarmOpen = true;
        } else if ( !bDegraded && m_bStoreAlarmOpen ) {
            gclsFmReporter.AlarmClose( "A-PRC-006", strMo );
            m_bStoreAlarmOpen = false;
        }
    }
}

void CSipMessageLogger::SpoolBatch( std::deque<LogItem> &batch ) {
    if ( batch.empty() ) return;
    StoreCtx &ctx = *m_ctx;
    // 파일경로별 병합 후 스풀 미러에 append (로컬 디스크 — 실패는 즉시 반환, 행 없음).
    std::unordered_map<std::string, std::pair<std::string, size_t>> groups;  // path → (data, lines)
    for ( auto &item : batch ) {
        auto &slot = groups[item.path];
        slot.first += item.line;
        slot.second++;
    }
    batch.clear();
    for ( auto &kv : groups ) {
        SpoolAppend( ctx, kv.first, kv.second.first, kv.second.second );
    }
    TrimSpoolIfNeeded();
}

void CSipMessageLogger::TrimSpoolIfNeeded() {
    StoreCtx &ctx = *m_ctx;
    if ( ctx.llSpoolBytes.load() <= ctx.llSpoolMaxBytes ) return;
    long long llNow = NowMs();
    if ( llNow - m_llLastSpoolTrimMs < kSpoolTrimIntervalMs ) return;
    m_llLastSpoolTrimMs = llNow;

    // 오래된 스풀 파일부터 폐기 — 재생 중(.replay) 파일은 건드리지 않는다.
    std::vector<std::pair<time_t, std::pair<std::string, long long>>> files;  // (mtime, (path, size))
    ScanSpool( ctx.strSpoolDir, [&]( const std::string &strPath, time_t tMtime, long long llSize ) {
        if ( strPath.size() > 7 && strPath.compare( strPath.size() - 7, 7, ".replay" ) == 0 ) return;
        files.push_back( { tMtime, { strPath, llSize } } );
    } );
    std::sort( files.begin(), files.end() );
    long long llTarget = ctx.llSpoolMaxBytes * 9 / 10;  // 90% 까지 정리
    for ( auto &f : files ) {
        if ( ctx.llSpoolBytes.load() <= llTarget ) break;
        int iLines = CountFileLines( f.second.first );
        if ( unlink( f.second.first.c_str() ) == 0 ) {
            ctx.llSpoolBytes.fetch_sub( f.second.second );
            ctx.ulDroppedLines.fetch_add( (unsigned long)iLines );
            CLog::Print( LOG_ERROR, "service_log spool over capacity — dropped %s (%d lines)",
                         f.second.first.c_str(), iLines );
        }
    }
}

void CSipMessageLogger::StartWriter() {
    bool bExpected = false;
    if ( m_bWriterRunning.compare_exchange_strong( bExpected, true ) ) {
        m_writerThread = std::thread( &CSipMessageLogger::WriterLoop, this );
        m_nasThread = std::thread( &CSipMessageLogger::NasFlusherLoop, m_ctx );
    }
}

void CSipMessageLogger::StopWriter() {
    if ( !m_bWriterRunning.exchange( false ) ) return;
    m_qCv.notify_all();
    if ( m_writerThread.joinable() ) m_writerThread.join();  // dispatch 는 NFS 무접촉 — 항상 join 가능

    // 저장소가 건강하면 flusher 큐 잔량이 저장 경로로 나가도록 잠시 기다린다 (정상 종료 시
    //   마지막 줄까지 직행 — 스풀 잔존을 남기지 않는다). 죽은 저장소는 기다리지 않는다.
    StoreCtx &ctx = *m_ctx;
    long long llDrainDeadline = NowMs() + kStopFlusherWaitMs;
    while ( NowMs() < llDrainDeadline && ctx.bNasHealthy.load() && !ctx.bSpoolPending.load() ) {
        bool bIdle;
        {
            std::lock_guard<std::mutex> lk( ctx.mtx );
            bIdle = ctx.nasQueue.empty() && ctx.inflight.empty();
        }
        if ( bIdle ) break;
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
    }

    // flusher 정지 — 저장 경로 op 에 갇혀 있으면 detach 한다 (NFS killable 대기라 프로세스
    //   종료(exit_group)가 회수한다). detach 후 flusher 는 StoreCtx(shared_ptr)만 참조.
    ctx.bRun.store( false );
    ctx.cv.notify_all();
    for ( int i = 0; i < kStopFlusherWaitMs / 100 && !ctx.bExited.load(); i++ ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }
    if ( m_nasThread.joinable() ) {
        if ( ctx.bExited.load() ) {
            m_nasThread.join();
        } else {
            m_nasThread.detach();
        }
    }

    // 미기록 잔량(nasQueue + inflight) 스풀 회수 — 다음 기동의 replay 가 저장 경로로 밀어넣는다.
    //   (inflight 는 갇힌 op 가 나중에 완료되면 중복 기록될 수 있다 — at-least-once 수용.)
    std::deque<std::deque<LogItem>> remains;
    {
        std::lock_guard<std::mutex> lk( ctx.mtx );
        remains.swap( ctx.nasQueue );
        if ( !ctx.inflight.empty() && !ctx.bExited.load() ) remains.push_front( std::move( ctx.inflight ) );
        ctx.inflight.clear();
    }
    for ( auto &batch : remains ) SpoolBatch( batch );
}

// ── NAS flusher (저장 경로 I/O 전담 — StoreCtx 외 무접촉) ─────────────────────
void CSipMessageLogger::NasFlusherLoop( std::shared_ptr<StoreCtx> pCtx ) {
    StoreCtx &ctx = *pCtx;

    // 기동 작업 ①: base 디렉터리 보장 (저장 경로 최초 접촉 — 여기서 행이면 여기만 갇힌다)
    ctx.llOpStartMs.store( NowMs() );
    if ( !ctx.strFlowBaseDir.empty() ) MkdirP( ctx.strFlowBaseDir );
    if ( !ctx.strMsgBaseDir.empty() && ctx.strMsgBaseDir != ctx.strFlowBaseDir ) MkdirP( ctx.strMsgBaseDir );
    // 기동 작업 ②: 시작 버킷 시딩 — 저장 경로의 기존 줄 + 이전 run 스풀 잔량(재생 대기분) 계수
    for ( int i = 0; i < 3; i++ ) {
        if ( ctx.strSeedPath[i].empty() || !ctx.bRun.load() ) continue;
        long long n = CountFileLines( ctx.strSeedPath[i] );
        std::string strSpool = SpoolPathFor( ctx, ctx.strSeedPath[i] );
        n += CountFileLines( strSpool );
        n += CountFileLines( strSpool + ".replay" );
        ctx.llSeedCount[i] = n;
    }
    ctx.llOpStartMs.store( 0 );
    ctx.bSeedDone.store( true );

    long long llNextReplayMs = 0;
    while ( ctx.bRun.load() ) {
        std::deque<LogItem> batch;
        {
            std::unique_lock<std::mutex> lk( ctx.mtx );
            ctx.cv.wait_for( lk, std::chrono::milliseconds( 200 ),
                             [&] { return !ctx.nasQueue.empty() || !ctx.bRun.load(); } );
            if ( !ctx.bRun.load() ) break;
            if ( !ctx.nasQueue.empty() ) {
                batch.swap( ctx.nasQueue.front() );
                ctx.nasQueue.pop_front();
                ctx.inflight = batch;  // 정지 시 회수용 사본
            }
        }
        if ( !batch.empty() ) {
            if ( ctx.bSpoolPending.load() ) {
                // dispatch 의 회수(reclaim)와 pop 이 겹친 드문 경쟁 — 이 배치는 스풀 내용보다
                //   오래됐을 수 있으므로 저장 경로 직행 대신 스풀로 우회해 재생 경로로 일원화.
                SpoolBatchToCtx( ctx, batch );
            } else {
                FlushBatchToStore( ctx, batch );
            }
            std::lock_guard<std::mutex> lk( ctx.mtx );
            ctx.inflight.clear();
            continue;
        }
        // idle — 스풀 재생 (실패 시 백오프)
        if ( ctx.bSpoolPending.load() && NowMs() >= llNextReplayMs ) {
            ReplaySpoolOne( ctx );
            if ( !ctx.bLastOpOk.load() ) llNextReplayMs = NowMs() + kReplayRetryMs;
        }
        // 정체로 unhealthy 가 됐지만 스풀 유입이 없었던 경우 — 직전 op 성공이 확인되면 복귀
        if ( !ctx.bNasHealthy.load() && !ctx.bSpoolPending.load() && ctx.bLastOpOk.load() ) {
            ctx.bNasHealthy.store( true );
        }
    }
    ctx.bExited.store( true );
}

void CSipMessageLogger::FlushBatchToStore( StoreCtx &ctx, std::deque<LogItem> &batch ) {
    if ( batch.empty() ) return;
    // 파일경로별 병합 — 같은 경로의 줄은 batch 순서(=enqueue/seq 순서)대로 누적.
    std::unordered_map<std::string, std::pair<std::string, size_t>> groups;
    for ( auto &item : batch ) {
        auto &slot = groups[item.path];
        slot.first += item.line;
        slot.second++;
    }
    batch.clear();

    bool bFailed = false;
    for ( auto &kv : groups ) {
        if ( bFailed ) {
            // 앞 그룹 실패 후에는 저장 경로를 더 만지지 않고 스풀로 우회 (지연 누적 방지)
            SpoolAppend( ctx, kv.first, kv.second.first, kv.second.second );
            continue;
        }
        // 디렉터리 보장 후 경로당 1회 open→append→close (서로 다른 파일끼리는 순서 무관)
        std::string strDir = kv.first.substr( 0, kv.first.rfind( '/' ) );
        ctx.llOpStartMs.store( NowMs() );
        MkdirP( strDir );
        int iErr = 0;
        FILE *pFile = fopen( kv.first.c_str(), "a" );
        bool bOk = false;
        if ( pFile ) {
            bOk = fwrite( kv.second.first.data(), 1, kv.second.first.size(), pFile ) == kv.second.first.size();
            if ( !bOk ) iErr = errno;
            fclose( pFile );
        } else {
            iErr = errno;
        }
        ctx.llOpStartMs.store( 0 );
        if ( bOk ) {
            ctx.bLastOpOk.store( true );
        } else {
            bFailed = true;
            ctx.bLastOpOk.store( false );
            ctx.bNasHealthy.store( false );
            {
                std::lock_guard<std::mutex> lk( ctx.mtx );
                ctx.strLastError = std::string( "write failed: " ) + strerror( iErr );
            }
            SpoolAppend( ctx, kv.first, kv.second.first, kv.second.second );
        }
    }
}

void CSipMessageLogger::SpoolBatchToCtx( StoreCtx &ctx, std::deque<LogItem> &batch ) {
    if ( batch.empty() ) return;
    std::unordered_map<std::string, std::pair<std::string, size_t>> groups;
    for ( auto &item : batch ) {
        auto &slot = groups[item.path];
        slot.first += item.line;
        slot.second++;
    }
    batch.clear();
    for ( auto &kv : groups ) {
        SpoolAppend( ctx, kv.first, kv.second.first, kv.second.second );
    }
}

bool CSipMessageLogger::SpoolAppend( StoreCtx &ctx, const std::string &strTarget, const std::string &strData,
                                     size_t nLines ) {
    std::string strSpoolPath = SpoolPathFor( ctx, strTarget );
    std::string strDir = strSpoolPath.substr( 0, strSpoolPath.rfind( '/' ) );
    MkdirP( strDir );
    FILE *pFile = fopen( strSpoolPath.c_str(), "a" );
    bool bOk = false;
    if ( pFile ) {
        bOk = fwrite( strData.data(), 1, strData.size(), pFile ) == strData.size();
        fclose( pFile );
    }
    if ( bOk ) {
        ctx.llSpoolBytes.fetch_add( (long long)strData.size() );
        ctx.ulSpooledLines.fetch_add( (unsigned long)nLines );
        ctx.bSpoolPending.store( true );
    } else {
        // 로컬 스풀마저 실패 (디스크 풀 등) — 폐기 계수만 남긴다
        ctx.ulDroppedLines.fetch_add( (unsigned long)nLines );
    }
    return bOk;
}

bool CSipMessageLogger::ReplaySpoolOne( StoreCtx &ctx ) {
    // 재생 대상 선택: 중단분(.replay) 우선, 없으면 가장 오래된 파일을 .replay 로 rename.
    std::string strPick;
    time_t tPickMtime = 0;
    bool bPickIsReplay = false;
    ScanSpool( ctx.strSpoolDir, [&]( const std::string &strPath, time_t tMtime, long long ) {
        bool bReplay = strPath.size() > 7 && strPath.compare( strPath.size() - 7, 7, ".replay" ) == 0;
        if ( bReplay != bPickIsReplay ) {
            if ( !bReplay ) return;  // .replay 가 이미 후보면 일반 파일은 무시
            strPick.clear();         // 일반 후보를 .replay 로 교체
            bPickIsReplay = true;
        }
        if ( strPick.empty() || tMtime < tPickMtime ) {
            strPick = strPath;
            tPickMtime = tMtime;
        }
    } );

    if ( strPick.empty() ) {
        // 스풀 드레인 완료 — 직행 복귀 (알람 close 는 dispatch 가 수행)
        ctx.bSpoolPending.store( false );
        ctx.llSpoolBytes.store( 0 );
        if ( ctx.bLastOpOk.load() ) ctx.bNasHealthy.store( true );
        return false;
    }

    if ( !bPickIsReplay ) {
        std::string strRenamed = strPick + ".replay";
        if ( rename( strPick.c_str(), strRenamed.c_str() ) != 0 ) return true;
        strPick = strRenamed;
    }

    // 내용 적재 (로컬 읽기)
    std::string strData;
    long long llSize = 0;
    {
        FILE *pFile = fopen( strPick.c_str(), "r" );
        if ( !pFile ) return true;  // 경쟁 삭제 등 — 다음 tick 재평가
        char buf[65536];
        size_t r;
        while ( ( r = fread( buf, 1, sizeof( buf ), pFile ) ) > 0 ) strData.append( buf, r );
        fclose( pFile );
        llSize = (long long)strData.size();
    }
    size_t nLines = 0;
    for ( char c : strData ) {
        if ( c == '\n' ) nLines++;
    }

    std::string strBase = strPick.substr( 0, strPick.size() - 7 );  // ".replay" 제거
    std::string strTarget = TargetPathFor( ctx, strBase );
    if ( strTarget.empty() ) {
        // 매핑 불능(손상 경로) — 폐기
        unlink( strPick.c_str() );
        ctx.llSpoolBytes.fetch_sub( llSize );
        ctx.ulDroppedLines.fetch_add( (unsigned long)nLines );
        return true;
    }

    // 저장 경로 append (여기서 행이면 flusher 만 갇힌다 — dispatch 가 정체를 감지)
    std::string strDir = strTarget.substr( 0, strTarget.rfind( '/' ) );
    ctx.llOpStartMs.store( NowMs() );
    MkdirP( strDir );
    int iErr = 0;
    FILE *pFile = fopen( strTarget.c_str(), "a" );
    bool bOk = false;
    if ( pFile ) {
        bOk = fwrite( strData.data(), 1, strData.size(), pFile ) == strData.size();
        if ( !bOk ) iErr = errno;
        fclose( pFile );
    } else {
        iErr = errno;
    }
    ctx.llOpStartMs.store( 0 );

    if ( bOk ) {
        unlink( strPick.c_str() );
        ctx.llSpoolBytes.fetch_sub( llSize );
        ctx.ulReplayedLines.fetch_add( (unsigned long)nLines );
        ctx.bLastOpOk.store( true );
    } else {
        ctx.bLastOpOk.store( false );
        ctx.bNasHealthy.store( false );
        std::lock_guard<std::mutex> lk( ctx.mtx );
        ctx.strLastError = std::string( "replay failed: " ) + strerror( iErr );
    }
    return true;
}

std::string CSipMessageLogger::SpoolPathFor( const StoreCtx &ctx, const std::string &strTarget ) {
    // 절대/상대 목적 경로를 무손실 왕복 가능한 미러 경로로: {spool}/abs{…} | {spool}/rel/{…}
    if ( !strTarget.empty() && strTarget[0] == '/' ) return ctx.strSpoolDir + "/abs" + strTarget;
    return ctx.strSpoolDir + "/rel/" + strTarget;
}

std::string CSipMessageLogger::TargetPathFor( const StoreCtx &ctx, const std::string &strSpoolFile ) {
    std::string strAbsPrefix = ctx.strSpoolDir + "/abs/";
    std::string strRelPrefix = ctx.strSpoolDir + "/rel/";
    if ( strSpoolFile.compare( 0, strAbsPrefix.size(), strAbsPrefix ) == 0 ) {
        return strSpoolFile.substr( strAbsPrefix.size() - 1 );  // 선행 '/' 유지
    }
    if ( strSpoolFile.compare( 0, strRelPrefix.size(), strRelPrefix ) == 0 ) {
        return strSpoolFile.substr( strRelPrefix.size() );
    }
    return "";
}

void CSipMessageLogger::ScanSpool( const std::string &strDir,
                                   const std::function<void( const std::string &, time_t, long long )> &fn ) {
    DIR *pDir = opendir( strDir.c_str() );
    if ( !pDir ) return;
    struct dirent *pEnt;
    while ( ( pEnt = readdir( pDir ) ) != NULL ) {
        if ( strcmp( pEnt->d_name, "." ) == 0 || strcmp( pEnt->d_name, ".." ) == 0 ) continue;
        std::string strPath = strDir + "/" + pEnt->d_name;
        struct stat st;
        if ( stat( strPath.c_str(), &st ) != 0 ) continue;
        if ( S_ISDIR( st.st_mode ) ) {
            ScanSpool( strPath, fn );
        } else if ( S_ISREG( st.st_mode ) ) {
            fn( strPath, st.st_mtime, (long long)st.st_size );
        }
    }
    closedir( pDir );
}

std::string CSipMessageLogger::GetFlowHourDir() {
    if ( m_strFlowBaseDir.empty() ) return "";
    time_t now = time( NULL );
    struct tm t;
    localtime_r( &now, &t );
    char buf[256];
    snprintf( buf, sizeof( buf ), "%s/%04d/%02d/%02d/%02d", m_strFlowBaseDir.c_str(), t.tm_year + 1900, t.tm_mon + 1,
              t.tm_mday, t.tm_hour );
    return buf;
}

std::string CSipMessageLogger::GetMsgHourDir() {
    if ( m_strMsgBaseDir.empty() ) return "";
    time_t now = time( NULL );
    struct tm t;
    localtime_r( &now, &t );
    char buf[256];
    snprintf( buf, sizeof( buf ), "%s/%04d/%02d/%02d/%02d", m_strMsgBaseDir.c_str(), t.tm_year + 1900, t.tm_mon + 1,
              t.tm_mday, t.tm_hour );
    return buf;
}

std::string CSipMessageLogger::GetTimestamp() {
    struct timeval tv;
    gettimeofday( &tv, NULL );
    struct tm t;
    localtime_r( &tv.tv_sec, &t );
    char buf[32];
    snprintf( buf, sizeof( buf ), "%02d:%02d:%02d.%06d", t.tm_hour, t.tm_min, t.tm_sec, (int)tv.tv_usec );
    return buf;
}

std::string CSipMessageLogger::JsonEsc( const char *s, int maxLen ) {
    if ( !s ) return "";
    std::string r;
    int len = ( maxLen > 0 ) ? maxLen : (int)strlen( s );
    r.reserve( len + 32 );
    for ( int i = 0; i < len && s[i]; i++ ) {
        unsigned char c = (unsigned char)s[i];
        switch ( c ) {
            case '"':
                r += "\\\"";
                break;
            case '\\':
                r += "\\\\";
                break;
            case '\n':
                r += "\\n";
                break;
            case '\r':
                r += "\\r";
                break;
            case '\t':
                r += "\\t";
                break;
            default:
                if ( c < 0x20 ) {
                    char h[8];
                    snprintf( h, sizeof( h ), "\\u%04x", c );
                    r += h;
                } else {
                    r += (char)c;
                }
        }
    }
    return r;
}

std::string CSipMessageLogger::ExtractHeader( const char *pszMsg, const char *pszHeader, const char *pszShort ) {
    if ( !pszMsg ) return "";

    const char *pHeaders[] = { pszHeader, pszShort };
    for ( int h = 0; h < 2; h++ ) {
        if ( !pHeaders[h] ) continue;
        int hlen = (int)strlen( pHeaders[h] );
        const char *p = pszMsg;
        while ( *p ) {
            if ( strncasecmp( p, pHeaders[h], hlen ) == 0 ) {
                p += hlen;
                while ( *p == ' ' || *p == '\t' ) p++;
                const char *eol = p;
                while ( *eol && *eol != '\r' && *eol != '\n' ) eol++;
                return std::string( p, eol - p );
            }
            while ( *p && *p != '\n' ) p++;
            if ( *p == '\n' ) p++;
        }
    }
    return "";
}

std::string CSipMessageLogger::ExtractMethodOrStatus( const char *pszMsg ) {
    if ( !pszMsg ) return "";
    const char *eol = pszMsg;
    while ( *eol && *eol != '\r' && *eol != '\n' ) eol++;

    std::string firstLine( pszMsg, eol - pszMsg );

    if ( firstLine.find( "SIP/2.0" ) == 0 ) {
        size_t sp = firstLine.find( ' ' );
        if ( sp != std::string::npos ) {
            size_t sp2 = firstLine.find( ' ', sp + 1 );
            if ( sp2 != std::string::npos ) return firstLine.substr( sp + 1, sp2 - sp - 1 );
            return firstLine.substr( sp + 1 );
        }
    }

    size_t sp = firstLine.find( ' ' );
    if ( sp != std::string::npos ) return firstLine.substr( 0, sp );

    return firstLine;
}

std::string CSipMessageLogger::ExtractUriUser( const std::string &strHeaderValue ) {
    if ( strHeaderValue.empty() ) return "";

    std::string result;
    size_t pos = strHeaderValue.find( "sip:" );
    if ( pos == std::string::npos ) pos = strHeaderValue.find( "tel:" );
    if ( pos == std::string::npos ) return "";

    pos += 4;
    while ( pos < strHeaderValue.size() ) {
        char c = strHeaderValue[pos];
        if ( c == '@' || c == '>' || c == ';' || c == ' ' ) break;
        result += c;
        pos++;
    }
    return result;
}

bool CSipMessageLogger::MkdirP( const std::string &path ) {
    struct stat st;
    if ( stat( path.c_str(), &st ) == 0 ) return true;
    size_t pos = path.rfind( '/' );
    if ( pos != std::string::npos && pos > 0 ) MkdirP( path.substr( 0, pos ) );
    return mkdir( path.c_str(), 0755 ) == 0 || errno == EEXIST;
}
