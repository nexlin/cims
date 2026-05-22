#ifndef _SIP_MESSAGE_LOGGER_H_
#define _SIP_MESSAGE_LOGGER_H_

#include <cstdio>
#include <map>
#include <mutex>
#include <string>

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

    /** ILogCallBack::Print — SIP 스택 콜백 (from/to = ue↔csp 기본) */
    void Print( EnumLogLevel eLevel, const char *fmt, ... ) override;

    /** 인터페이스 메시지 기록 — 호출자가 from/to 직접 전달 */
    void LogMessage( const char *pszFrom, const char *pszTo, const char *pszProto, const char *pszMethod,
                     const char *pszPeer, const char *pszBody, const char *pszService = "system",
                     const char *pszTxId = "", const char *pszSesId = "", const char *pszDetail = "",
                     const char *pszCaller = "", const char *pszCallee = "" );

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

    /** Ensure hourly directories and rotate files if needed */
    void EnsureHourlyFiles( const std::string &strFlowHourDir, const std::string &strMsgHourDir );

    /** Close all open files */
    void CloseAllFiles();

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

    // Flow file (통합, 노드당 1파일)
    FILE *m_pFlowFile;

    // Per-interface message files (replaces single raw file)
    FILE *m_pSipFile;
    FILE *m_pCmpFile;
    FILE *m_pCscFile;
    int m_iSipSeq;  // current line number (1-based)
    int m_iCmpSeq;
    int m_iCscSeq;
};

extern CSipMessageLogger gclsSipLogger;

#endif
