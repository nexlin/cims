#ifndef _SIP_MESSAGE_LOGGER_H_
#define _SIP_MESSAGE_LOGGER_H_

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
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
 *
 * 저장 경로 무의존(non-blocking) 계약 — Dir 가 NAS(NFS) 여도 SIP/제어 스레드는 막히지 않는다:
 *   - 생산자(Print/LogMessage/LogSecurity)는 파일시스템 호출 없이 큐 적재만 한다.
 *   - dispatch 스레드가 큐를 소비해 목적지를 정한다. 저장소가 건강하면 NAS flusher 큐로,
 *     아니면 로컬 스풀(SpoolDir)로 기록한다 — dispatch 도 NFS 를 만지지 않는다.
 *   - NAS flusher 스레드만 저장 경로 I/O 를 수행한다. 쓰기 실패(fail-fast)·in-flight 정체
 *     (StallSec 초과, hard mount 행)·flusher 큐 포화 시 폴백이 걸리고, 회복되면 스풀을
 *     경로별 줄 순서대로 재생(replay)한 뒤 직행으로 복귀한다 (seq↔줄번호 정합 보존).
 *   - 폴백 진입/회복은 A-PRC-006 storage_failure 알람으로 자기보고한다.
 */
class CSipMessageLogger : public ILogCallBack {
public:
    CSipMessageLogger();
    ~CSipMessageLogger();

    /** Initialize with separate base directories for flow/message logs.
     *  strSpoolDir: 저장 경로 무응답 시 로컬 스풀 루트 (로컬 디스크 경로여야 한다).
     *  iStallSec: NAS flusher in-flight 가 이 시간을 넘으면 저장소 무응답으로 판정.
     *  iSpoolMaxMb: 스풀 용량 상한 — 초과 시 오래된 스풀 파일부터 폐기(계수). */
    void Init( const std::string &strFlowBaseDir, const std::string &strMsgBaseDir, const std::string &strSystemId,
               bool bRawLogEnabled = true, const std::string &strSpoolDir = "spool", int iStallSec = 5,
               int iSpoolMaxMb = 1024 );

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
     *  전용 {systemId}.security.{mm5}.jsonl 에 1줄 append (비동기).
     *  reasons = 콤마구분 사유("external_ip,scanner_ua,fraud_number,..."). */
    void LogSecurity( const char *pszPeer, const char *pszMethod, const char *pszCaller, const char *pszCallee,
                      const char *pszUa, const char *pszCallId, const char *pszReasons, bool bRegisteredCaller );

    /** Call-ID에 sesid/subid 매핑 등록 (INVITE 전에 호출) */
    void SetCallSesId( const std::string &strCallId, const std::string &strSesId, const std::string &strSubId = "" );

    /** 신규 sesid 발행 — 포맷: {caller}::{module}::{us_ts}::{counter}
     *  caller가 비어있으면 leading "::"로 시작 */
    static std::string IssueSesId( const std::string &strCaller, const char *pszModule = "csp" );

    /** 원격 프로세스(CMP/CMDP)에 상태로 남는 식별자 발행 — 포맷: {issuer}_{yyyymmddHHMMSSmmm}_{index}.
     *  ms 타임스탬프 + 동일-ms 순번이라 프로세스 재시작 경계에서도 유일 — 재기동 후 첫 발행이
     *  상대 노드의 잔존 고아 세션과 충돌하지 않는다. 영숫자+밑줄만 사용 (MSRP URI 세션부 안전). */
    static std::string IssueUniqueId( const char *pszIssuer );

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

    /** NAS flusher 스레드와 dispatch 가 공유하는 상태.
     *  flusher 는 종료 시 저장 경로 I/O 에 갇혀 join 불가할 수 있어 detach 되므로,
     *  logger 멤버가 아니라 shared_ptr 로 수명을 분리한다 — flusher 는 이 구조체와
     *  전역 로거(CLog)만 만지고 CSipMessageLogger 본체는 절대 참조하지 않는다. */
    struct StoreCtx {
        std::mutex mtx;  // nasQueue/inflight 보호
        std::condition_variable cv;
        std::deque<std::deque<LogItem>> nasQueue;  // 직행 대기 배치 (dispatch → flusher)
        std::deque<LogItem> inflight;              // flusher 가 기록 중인 배치 (정지 시 스풀 회수용)
        std::atomic<bool> bRun{ true };
        std::atomic<bool> bExited{ false };               // flusher 스레드 종료 표식 (join/detach 판단)
        std::atomic<bool> bNasHealthy{ true };            // 저장 경로 판정 (dispatch 라우팅 기준)
        std::atomic<bool> bSpoolPending{ false };         // 스풀 잔량 존재 — 드레인 전 직행 금지 (순서 보존)
        std::atomic<long long> llOpStartMs{ 0 };          // 저장 경로 op 시작 시각 (0=idle) — 정체 감지
        std::atomic<long long> llSpoolBytes{ 0 };         // 스풀 사용량 (근사)
        std::atomic<bool> bLastOpOk{ true };              // 마지막 저장 경로 op 성공 여부 (idle 회복 판정)
        std::atomic<unsigned long> ulSpooledLines{ 0 };   // 폴백으로 스풀에 적재된 누적 줄 수
        std::atomic<unsigned long> ulReplayedLines{ 0 };  // 스풀→NAS 재생 완료 누적 줄 수
        std::atomic<unsigned long> ulDroppedLines{ 0 };   // 스풀 기록 실패/용량 폐기 줄 수
        std::string strLastError;                         // 마지막 실패 사유 (mtx 보호)
        // 시딩: flusher 가 기동 직후 시작 버킷 파일의 기존 줄 수를 계수 → dispatch 가 합류
        std::string strSeedBucketKey;
        std::string strSeedPath[3];  // sip/cmp/csc msg 경로 (기동 시점 버킷)
        long long llSeedCount[3] = { 0, 0, 0 };
        std::atomic<bool> bSeedDone{ false };
        // 불변 설정 (Init 에서 확정)
        std::string strSpoolDir;
        std::string strFlowBaseDir;
        std::string strMsgBaseDir;
        int iStallMs = 5000;
        long long llSpoolMaxBytes = 0;
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

    /** 5분 버킷 회전 — 순수 북키핑 (m_mtx 보유 하 호출, 파일시스템 무접촉).
     *  버킷 변경 시 iface seq 리셋: 기동 첫 버킷은 -1(flusher 시딩 대기), 이후 버킷은 0
     *  (새 파일명이라 기존 줄이 있을 수 없음). 디렉터리 생성은 flusher 가 기록 직전에 수행. */
    void RotateBucket( const std::string &strMsgHourDir );

    /** 현재 5분 버킷 접미사 "00".."55" (분/5*5). 파일명에 부여하여 1시간 1파일→5분 1파일. */
    std::string BucketSuffix();
    /** flow 파일 경로: {flowHourDir}/{systemId}.flow.{mm5}.jsonl */
    std::string FlowFilePath();
    /** iface msg 파일 경로: {msgHourDir}/{systemId}_{iface}.msg.{mm5}.jsonl */
    std::string MsgFilePath( const char *pszIface );
    /** 파일의 현재 줄 수 — 시딩(재기동 대비 seq 연속성) 계수용. 없으면 0. */
    static int CountFileLines( const std::string &path );

    /** Ensure directory exists (recursive) */
    static bool MkdirP( const std::string &path );

    /** Get current hourly directory path for flow logs */
    std::string GetFlowHourDir();

    /** Get current hourly directory path for message logs */
    std::string GetMsgHourDir();

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

    // ── 비동기 배치 로그 writer (dispatch + NAS flusher 2단) ────────────────
    /** 포맷 완료된 한 줄을 파일경로와 함께 큐에 적재 — 파일 I/O 없이 즉시 반환.
     *  생산자(Print/LogMessage)가 m_mtx 를 보유한 채 호출하므로 enqueue 순서=seq 순서. */
    void EnqueueLine( const std::string &strPath, std::string &&strLine );
    /** dispatch 스레드 본체: flush 주기(kFlushIntervalMs) 또는 큐 임계 도달 시 큐를 비워
     *  저장소 건강 여부에 따라 NAS flusher 큐 또는 로컬 스풀로 라우팅. NFS 무접촉. */
    void WriterLoop();
    /** 한 배치를 로컬 스풀에 기록 (dispatch/정지 경로 전용 — 로컬 디스크 I/O). */
    void SpoolBatch( std::deque<LogItem> &batch );
    /** 배치 라우팅: 직행(nasQueue) | 폴백(회수 후 스풀). 반환 false = 큐 포화 역압
     *  (배치를 m_logQueue 앞으로 되돌림 — 호출자는 한 tick 쉬고 재시도). */
    bool RouteBatch( std::deque<LogItem> &batch, bool bForceSpoolOnBackpressure );
    /** 폴백 진입 시 flusher 큐 잔량(스풀 내용보다 오래된 분)을 FIFO 로 먼저 스풀에 회수. */
    void ReclaimNasQueueToSpool();
    /** 스풀 용량 상한 초과 시 오래된 스풀 파일부터 폐기 (dispatch 전용, 주기 제한). */
    void TrimSpoolIfNeeded();
    /** flusher 시딩 결과를 iface seq 에 합류 — 같은 버킷이고 생산자 write 전(-1)일 때만. */
    void ApplySeedIfPending();
    /** 알람/상태 전이 정리 — A-PRC-006 open/close (dispatch 스레드 단독 소유). */
    void ReconcileStoreAlarm();
    void StartWriter();
    void StopWriter();

    /** NAS flusher 스레드 본체 — ctx 만 참조 (logger 본체 무접촉, detach 안전).
     *  기동 시: base 디렉터리 보장 + 시작 버킷 시딩 계수. 이후: nasQueue 배치 flush,
     *  idle 이면 스풀 재생. 실패/정체 시 ctx 상태만 갱신 (알람은 dispatch 가 올린다). */
    static void NasFlusherLoop( std::shared_ptr<StoreCtx> ctx );
    /** 배치를 저장 경로에 기록. 실패 그룹부터는 스풀로 우회 적재. */
    static void FlushBatchToStore( StoreCtx &ctx, std::deque<LogItem> &batch );
    /** 배치 전체를 스풀로 우회 (flusher 전용 — 스풀 잔량 존재 중 직행 대기분의 순서 보존). */
    static void SpoolBatchToCtx( StoreCtx &ctx, std::deque<LogItem> &batch );
    /** 스풀에서 가장 오래된 파일 하나를 저장 경로로 재생. 스풀이 비면 bSpoolPending 해제.
     *  반환: 재생을 시도했으면 true (실패 포함), 스풀이 비어 할 일이 없었으면 false. */
    static bool ReplaySpoolOne( StoreCtx &ctx );
    /** 한 목적 경로 분량을 스풀 미러 파일에 append (로컬). 실패 시 폐기 계수. */
    static bool SpoolAppend( StoreCtx &ctx, const std::string &strTarget, const std::string &strData, size_t nLines );
    /** 목적 경로 → 스풀 미러 경로 ({spool}/abs{path} | {spool}/rel/{path}) */
    static std::string SpoolPathFor( const StoreCtx &ctx, const std::string &strTarget );
    /** 스풀 미러 경로 → 목적 경로 (SpoolPathFor 역변환. 실패 시 빈 문자열) */
    static std::string TargetPathFor( const StoreCtx &ctx, const std::string &strSpoolFile );
    /** 스풀 트리 재귀 스캔 — 파일 (경로, mtime, size) 콜백. */
    static void ScanSpool( const std::string &strDir,
                           const std::function<void( const std::string &, time_t, long long )> &fn );
    /** monotonic ms (steady_clock) — 정체 감지/백오프용 */
    static long long NowMs();

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
    // Call-ID → sesid/subid 캐시 (GroupCallService가 등록)
    std::map<std::string, std::string> m_mapCallSesId;
    std::map<std::string, std::string> m_mapCallSubId;

    // 5분 버킷 회전 감지 키 ({msgHourDir}/{mm5})
    std::string m_strCurrentBucketKey;

    // per-interface msg 파일 줄 번호 (1-based). -1 = 기동 첫 버킷 시딩 대기.
    int m_iSipSeq;
    int m_iCmpSeq;
    int m_iCscSeq;

    // 비동기 writer 상태 — 생산자는 m_qMtx 만 잡고 enqueue, dispatch 가 소비
    std::deque<LogItem> m_logQueue;  // 기록 대기 (파일경로 + 한 줄)
    std::mutex m_qMtx;               // m_logQueue 보호 (m_mtx 와 독립; 항상 m_mtx→m_qMtx 순서)
    std::condition_variable m_qCv;
    std::thread m_writerThread;  // dispatch 스레드 (항상 join 가능 — NFS 무접촉)
    std::atomic<bool> m_bWriterRunning;
    std::atomic<unsigned long> m_ulDroppedLogs;  // 큐 상한 초과로 버려진 줄 수

    std::shared_ptr<StoreCtx> m_ctx;  // NAS flusher 공유 상태
    std::thread m_nasThread;          // NAS flusher (정지 시 join 불가하면 detach)
    bool m_bSeedApplied;              // 시딩 합류 완료 (dispatch 단독 접근)
    bool m_bStoreAlarmOpen;           // A-PRC-006 FM 보고 상태 (dispatch 단독 접근)
    bool m_bStoreDegraded;            // 폴백 상태 로컬 로그 전이 추적 (dispatch 단독 접근)
    long long m_llLastSpoolTrimMs;    // 스풀 용량 정리 주기 제한 (dispatch 단독 접근)
};

extern CSipMessageLogger gclsSipLogger;

#endif
