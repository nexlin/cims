#ifndef _SIP_MESSAGE_LOGGER_H_
#define _SIP_MESSAGE_LOGGER_H_

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "Log.h"

/**
 * @brief Unified message logger for SIP/CMP/CSC protocols
 *
 * Writes to two outputs per message:
 *   1. {service}_flow.jsonl (always) — compact, no body
 *      - phone_flow.jsonl  (VoLTE)
 *      - ptt_flow.jsonl    (PTT)
 *      - system_flow.jsonl (system/admin)
 *   2. Per-service per-protocol detail file (when raw logging enabled):
 *      - phone_sip.jsonl / phone_cmp.jsonl
 *      - ptt_sip.jsonl   / ptt_cmp.jsonl
 *      - system_csc.jsonl
 *
 * Directory: {baseDir}/YYYY/MM/DD/HH/
 * Hourly rotation: all files rotate together.
 *
 * Service classification:
 *   - SIP: domain in Request-URI/To → volte (IMS domain), ptt (PTT domain), else "system"
 *   - CMP: caller specifies ("volte" or "ptt")
 *   - CSC: always "system"
 *   - HEARTBEAT/OPTIONS: always "system"
 */
class CSipMessageLogger : public ILogCallBack {
public:
    CSipMessageLogger();
    ~CSipMessageLogger();

    /** Initialize with separate base directories for flow/message logs */
    void Init( const std::string &strFlowBaseDir, const std::string &strMsgBaseDir, const std::string &strSystemId,
               bool bRawLogEnabled = true );

    /** Config 의 "Realm" 배열에서 빌드된 domain→service 매핑을 주입 */
    void SetDomainServiceMap( const std::map<std::string, std::string> &mapDomainToService );

    bool IsEnabled() const {
        return m_bEnabled;
    }
    bool IsRawLogEnabled() const {
        return m_bRawLogEnabled;
    }
    /** 논리 노드 ID (e.g. "csp_01") — CMP 제어 envelope hdr.node 등 발신 주체 표기용 */
    const std::string &GetSystemId() const {
        return m_strSystemId;
    }

    /** ILogCallBack::Print — SIP 스택 콜백 (from/to = ue↔csp 기본) */
    void Print( EnumLogLevel eLevel, const char *fmt, ... ) override;

    /** 인터페이스 메시지 기록 — 호출자가 from/to 직접 전달 */
    void LogMessage( const char *pszFrom, const char *pszTo, const char *pszProto, const char *pszMethod,
                     const char *pszPeer, const char *pszBody, const char *pszService = "system",
                     const char *pszTxId = "", const char *pszSesId = "", const char *pszDetail = "",
                     const char *pszCaller = "", const char *pszCallee = "" );

    /** 비정상(스캔/사기) 세션 기록 — 수신 시점에 ModuleDispatcher 가 분류해 호출.
     *  전용 {systemId}.security.{mm5}.jsonl 에 1줄 append (open-per-write 비동기).
     *  reasons = 콤마구분 사유("external_ip,scanner_ua,fraud_number,..."). */
    void LogSecurity( const char *pszPeer, const char *pszMethod, const char *pszCaller, const char *pszCallee,
                      const char *pszUa, const char *pszCallId, const char *pszReasons, bool bRegisteredCaller );

    /** Call-ID에 sesid/subid 매핑 등록 (INVITE 전에 호출) */
    void SetCallSesId( const std::string &strCallId, const std::string &strSesId, const std::string &strSubId = "" );

    /** 신규 sesid 발행 — 포맷: {caller}::{module}::{us_ts}::{counter}
     *  caller가 비어있으면 leading "::"로 시작 */
    static std::string IssueSesId( const std::string &strCaller, const char *pszModule = "csp" );

    /** Call-ID에 매핑된 sesid 반환. 없으면 신규 발행하여 저장. */
    std::string GetOrIssueSesId( const std::string &strCallId, const std::string &strCaller );

    /** Call-ID에 매핑된 sesid 단순 조회 (없으면 빈문자열) */
    std::string GetSesIdByCallId( const std::string &strCallId );

private:
    /** 비동기 writer 큐 항목: 목적 파일 경로 + 포맷 완료된 한 줄(개행 포함) */
    struct LogItem {
        std::string path;
        std::string line;
    };

    /** Determine service from SIP message domain */
    std::string ClassifyService( const char *pszMsg, const std::string &strCallId, const std::string &strMethod );

    /** 통합 flow.jsonl 기록.
     *  필드 순서: ts, service, caller, callee, sesid, subid, node, from, to,
     *            proto, method, detail, mid, seq, iface
     *  빈 값은 key 생략. */
    void WriteFlowLine( const char *pszService, const char *pszTs, const char *pszFrom, const char *pszTo,
                        const char *pszProto, const char *pszMethod, const char *pszDetail = "",
                        const char *pszTxId = "", const char *pszSesId = "", const char *pszSubId = "", int iSeq = 0,
                        const char *pszIface = "", const char *pszCaller = "", const char *pszCallee = "" );

    /** Write to {system_id}_{iface}.msg.jsonl, returns line number (seq).
     *  필드 순서: ts, dir, peer, caller, callee, sesid, proto, msg
     *  빈 값은 key 생략. */
    int WriteInterfaceLine( const char *pszIface, const char *pszTs, const char *pszDir, const char *pszPeer,
                            const char *pszProto, const char *pszMsg, const char *pszCaller = "",
                            const char *pszCallee = "", const char *pszSesId = "" );

    /** 5분 버킷 회전: 디렉터리 보장 + 버킷 변경 시 iface seq 리셋(미계수=-1). 파일 핸들은 유지하지 않음
     *  (open-per-write). 함수명은 호환을 위해 유지. */
    void EnsureHourlyFiles( const std::string &strFlowHourDir, const std::string &strMsgHourDir );

    /** Close all open files (open-per-write 전환 후 no-op 안전망) */
    void CloseAllFiles();

    /** 현재 5분 버킷 접미사 "00".."55" (분/5*5). 파일명에 부여하여 1시간 1파일→5분 1파일. */
    std::string BucketSuffix();
    /** flow 파일 경로: {flowHourDir}/{systemId}.flow.{mm5}.jsonl */
    std::string FlowFilePath();
    /** iface msg 파일 경로: {msgHourDir}/{systemId}_{iface}.msg.{mm5}.jsonl */
    std::string MsgFilePath( const char *pszIface );
    /** 파일의 현재 줄 수 — 버킷 첫 write 시 seq 연속성(재기동 대비) 계수용. 없으면 0. */
    static int CountFileLines( const std::string &path );

    /** Ensure directory exists (recursive) */
    static bool MkdirP( const std::string &path );

    /** Get current hourly directory path for flow logs */
    std::string GetFlowHourDir();

    /** Get current hourly directory path for message logs */
    std::string GetMsgHourDir();

    /** Get interface FILE* for a given interface, open lazily */
    FILE *GetInterfaceFile( const char *pszIface );

    /** Get sequence counter for a given interface */
    int &GetIfaceSeq( const char *pszIface );

    /** Get current timestamp string HH:MM:SS.uuuuuu */
    static std::string GetTimestamp();

    /** Escape a string for JSON output */
    static std::string JsonEsc( const char *s, int maxLen = -1 );

    /** Extract a SIP header value from message text */
    static std::string ExtractHeader( const char *pszMsg, const char *pszHeader, const char *pszShort = NULL );

    /** Extract the method or status from the first line of a SIP message */
    static std::string ExtractMethodOrStatus( const char *pszMsg );

    /** Extract URI user part from a From/To header value */
    static std::string ExtractUriUser( const std::string &strHeaderValue );

    /** Get flow FILE*, open lazily */
    FILE *GetFlowFile();

    // ── 비동기 배치 로그 writer ──────────────────────────────────────────
    /** 포맷 완료된 한 줄을 파일경로와 함께 큐에 적재 — NFS I/O 없이 즉시 반환.
     *  생산자(Print/LogMessage)가 m_mtx 를 보유한 채 호출하므로 enqueue 순서=seq 순서. */
    void EnqueueLine( const std::string &strPath, std::string &&strLine );
    /** writer 스레드 본체: flush 주기(kFlushIntervalMs) 또는 큐 임계 도달 시 큐를 비워
     *  파일경로별로 라인을 합쳐 경로당 1회 open→append→close (open-per-write → open-per-batch). */
    void WriterLoop();
    /** 한 배치를 파일경로별로 합쳐 기록. 호출 후 batch 는 비워진다. */
    void FlushBatch( std::deque<LogItem> &batch );
    void StartWriter();
    void StopWriter();

    std::string m_strFlowBaseDir;  // service_log base
    std::string m_strMsgBaseDir;   // msg_log base
    std::string m_strSystemId;     // e.g. "csp_01" (파일명용)
    std::string m_strNodeName;     // e.g. "csp" (flow node 필드용)
    bool m_bEnabled;
    bool m_bRawLogEnabled;
    std::mutex m_mtx;

    // Realm configuration for service classification (domain → service name)
    std::map<std::string, std::string> m_mapDomainToService;

    // Call-ID → service cache for SIP correlation
    std::map<std::string, std::string> m_mapCallService;
    // Call-ID → sesid/subid cache (GroupCallService가 등록)
    std::map<std::string, std::string> m_mapCallSesId;
    std::map<std::string, std::string> m_mapCallSubId;

    // Current open file state (hourly rotation)
    std::string m_strCurrentFlowHourDir;
    std::string m_strCurrentMsgHourDir;
    std::string m_strCurrentBucketKey;  // 5분 버킷 회전 감지 키 ({msgHourDir}/{mm5})

    // Flow file (통합, 노드당 1파일)
    FILE *m_pFlowFile;

    // Per-interface message files (replaces single raw file)
    FILE *m_pSipFile;
    FILE *m_pCmpFile;
    FILE *m_pCscFile;
    int m_iSipSeq;  // current line number (1-based)
    int m_iCmpSeq;
    int m_iCscSeq;

    // 비동기 writer 상태 — 생산자는 m_qMtx 만 잡고 enqueue, writer 만 파일 I/O 수행
    std::deque<LogItem> m_logQueue;  // 기록 대기 (파일경로 + 한 줄)
    std::mutex m_qMtx;               // m_logQueue 보호 (m_mtx 와 독립; 항상 m_mtx→m_qMtx 순서)
    std::condition_variable m_qCv;
    std::thread m_writerThread;
    std::atomic<bool> m_bWriterRunning;
    std::atomic<unsigned long> m_ulDroppedLogs;  // 큐 상한 초과로 버려진 줄 수
};

extern CSipMessageLogger gclsSipLogger;

#endif
