#ifndef _SIP_MESSAGE_LOGGER_H_
#define _SIP_MESSAGE_LOGGER_H_

#include <map>
#include <mutex>
#include <string>

#include "Log.h"
#include "ServiceLogWriter.h"

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
 *   생산자(Print/LogMessage/LogSecurity)는 파일시스템 호출 없이 포맷+큐 적재만 하고,
 *   2단 writer(dispatch + NAS flusher + 로컬 스풀 폴백/재생)는 공용 CServiceLogWriter
 *   (include/ServiceLogWriter.h — cmp/cmdp 와 동일 구현)가 수행한다. 폴백 진입/회복은
 *   A-PRC-006 storage_failure 알람으로 자기보고한다. 계약 상세: flow_logging.md §2.
 *   시딩(재기동 시 기동 버킷의 기존 줄 계수)은 flusher 가 비동기 수행하고
 *   WriteInterfaceLine 의 첫 write 가 합류한다.
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

    // ── 서비스 로그 writer — 공용 CServiceLogWriter 위임 ─────────────────────
    /** 포맷 완료된 한 줄을 파일경로와 함께 큐에 적재 — 파일 I/O 없이 즉시 반환.
     *  생산자(Print/LogMessage)가 m_mtx 를 보유한 채 호출하므로 enqueue 순서=seq 순서. */
    void EnqueueLine( const std::string &strPath, std::string &&strLine ) {
        m_clsWriter.Enqueue( strPath, std::move( strLine ) );
    }

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

    // 서비스 로그 writer (2단 dispatch+flusher+스풀 — include/ServiceLogWriter.h 공용 구현)
    CServiceLogWriter m_clsWriter;
    // 시딩 대상 버킷 키 ({msgHourDir}/{mm5}) — WriteInterfaceLine 첫 write 의 시딩 합류 판정
    std::string m_strSeedBucketKey;
};

extern CSipMessageLogger gclsSipLogger;

#endif
