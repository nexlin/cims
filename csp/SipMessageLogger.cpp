#include "SipMessageLogger.h"

#include <cstdarg>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>

CSipMessageLogger gclsSipLogger;

// Maximum Call-ID cache entries before eviction
static const size_t MAX_CALLID_CACHE = 10000;

CSipMessageLogger::CSipMessageLogger()
    : m_bEnabled(false), m_bRawLogEnabled(true),
      m_pFlowFile(NULL),
      m_pSipFile(NULL), m_pCmpFile(NULL), m_pCscFile(NULL),
      m_iSipSeq(0), m_iCmpSeq(0), m_iCscSeq(0)
{
}

CSipMessageLogger::~CSipMessageLogger()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    CloseAllFiles();
}

void CSipMessageLogger::CloseAllFiles()
{
    // Called under m_mtx lock
    if (m_pFlowFile)       { fclose(m_pFlowFile);       m_pFlowFile = NULL; }
    if (m_pSipFile)        { fclose(m_pSipFile);        m_pSipFile = NULL; }
    if (m_pCmpFile)        { fclose(m_pCmpFile);        m_pCmpFile = NULL; }
    if (m_pCscFile)        { fclose(m_pCscFile);        m_pCscFile = NULL; }
}

void CSipMessageLogger::Init(const std::string& strFlowBaseDir,
                              const std::string& strMsgBaseDir,
                              const std::string& strSystemId,
                              bool bRawLogEnabled)
{
    if (strFlowBaseDir.empty() && strMsgBaseDir.empty()) return;
    m_strFlowBaseDir = strFlowBaseDir;
    m_strMsgBaseDir = strMsgBaseDir;
    m_strSystemId = strSystemId.empty() ? "csp_01" : strSystemId;
    // node 필드용: "csp_01" → "csp" (언더스코어+숫자 제거)
    m_strNodeName = m_strSystemId;
    auto upos = m_strNodeName.find('_');
    if (upos != std::string::npos) m_strNodeName = m_strNodeName.substr(0, upos);
    m_bRawLogEnabled = bRawLogEnabled;
    if (!m_strFlowBaseDir.empty()) MkdirP(m_strFlowBaseDir);
    if (!m_strMsgBaseDir.empty()) MkdirP(m_strMsgBaseDir);
    m_bEnabled = true;
}

void CSipMessageLogger::SetCallSesId(const std::string& strCallId, const std::string& strSesId, const std::string& strSubId) {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!strCallId.empty() && !strSesId.empty()) {
        m_mapCallSesId[strCallId] = strSesId;
    }
    if (!strCallId.empty() && !strSubId.empty()) {
        m_mapCallSubId[strCallId] = strSubId;
    }
}

std::string CSipMessageLogger::IssueSesId(const std::string& strCaller, const char* pszModule)
{
    // 포맷: {caller}::{module}::{yyyymmddHHMMSSuuuuuu}::{counter}
    // counter: 동일 us_ts가 연속될 때 1씩 증가
    static std::mutex sMtx;
    static std::string sLastTs;
    static unsigned int sCounter = 0;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    char tsBuf[32];
    snprintf(tsBuf, sizeof(tsBuf), "%04d%02d%02d%02d%02d%02d%06ld",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, (long)tv.tv_usec);

    unsigned int counter;
    std::string ts(tsBuf);
    {
        std::lock_guard<std::mutex> lock(sMtx);
        if (ts == sLastTs) {
            ++sCounter;
        } else {
            sLastTs = ts;
            sCounter = 1;
        }
        counter = sCounter;
    }

    const char* mod = (pszModule && pszModule[0]) ? pszModule : "csp";
    std::string r;
    r.reserve(strCaller.size() + 50);
    r.append(strCaller);
    r.append("::");
    r.append(mod);
    r.append("::");
    r.append(ts);
    r.append("::");
    r.append(std::to_string(counter));
    return r;
}

std::string CSipMessageLogger::GetOrIssueSesId(const std::string& strCallId,
                                                 const std::string& strCaller)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!strCallId.empty()) {
        auto it = m_mapCallSesId.find(strCallId);
        if (it != m_mapCallSesId.end() && !it->second.empty()) return it->second;
    }
    std::string sid = IssueSesId(strCaller, "csp");
    if (!strCallId.empty()) {
        // Call-ID 캐시 크기 제한
        if (m_mapCallSesId.size() >= MAX_CALLID_CACHE) {
            m_mapCallSesId.erase(m_mapCallSesId.begin());
        }
        m_mapCallSesId[strCallId] = sid;
    }
    return sid;
}

std::string CSipMessageLogger::GetSesIdByCallId(const std::string& strCallId)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_mapCallSesId.find(strCallId);
    if (it != m_mapCallSesId.end()) return it->second;
    return "";
}

void CSipMessageLogger::SetDomainServiceMap(const std::map<std::string, std::string>& mapDomainToService)
{
    // 초기화 시점(CspServer 시작 직후)에만 호출되므로 락 없이 안전.
    // 이후 Print() 에서 m_mtx 보호 하에 read-only 로만 접근.
    m_mapDomainToService = mapDomainToService;
}

/**
 * 호스트 문자열에서 도메인 부분 추출.
 * "<sip:user@host:port>;tag=X" 혹은 "sip:user@host" 등 다양한 패턴 수용.
 */
static std::string ExtractDomainFromUri(const std::string& strUri)
{
    if (strUri.empty()) return "";

    // Skip leading "<" and scheme (sip:/sips:/tel:)
    size_t start = 0;
    if (strUri[0] == '<') start = 1;

    size_t colon = strUri.find(':', start);
    if (colon == std::string::npos) return "";
    start = colon + 1;

    // 찾을 끝: ';' '>' ',' ' ' '\r' '\n'
    size_t end = strUri.size();
    for (size_t i = start; i < strUri.size(); ++i) {
        char c = strUri[i];
        if (c == ';' || c == '>' || c == ',' || c == ' ' || c == '\r' || c == '\n') {
            end = i;
            break;
        }
    }

    // user@host 에서 host 추출
    std::string uriBody = strUri.substr(start, end - start);
    size_t at = uriBody.find('@');
    std::string hostPort = (at != std::string::npos) ? uriBody.substr(at + 1) : uriBody;

    // host:port 에서 host
    size_t pcolon = hostPort.find(':');
    if (pcolon != std::string::npos) hostPort = hostPort.substr(0, pcolon);

    return hostPort;
}

std::string CSipMessageLogger::ClassifyService(const char* pszMsg, const std::string& strCallId, const std::string& strMethod)
{
    // 주의: 이 함수는 Print()에서 m_mtx 를 이미 잡은 상태에서 호출되므로
    //       내부에서 m_mtx 를 재획득하지 않는다 (std::mutex 는 재귀 불가).
    if (strMethod == "OPTIONS" || strMethod == "HEARTBEAT") {
        return "system";
    }

    bool bIsResponse = (pszMsg && strncmp(pszMsg, "SIP/2.0", 7) == 0);

    if (!bIsResponse && pszMsg) {
        // Request-URI 도메인 추출 (첫 줄: "METHOD sip:user@domain SIP/2.0")
        std::string strReqUri;
        const char* pFirstSpace = strchr(pszMsg, ' ');
        if (pFirstSpace) {
            const char* pEnd = pFirstSpace + 1;
            while (*pEnd && *pEnd != ' ' && *pEnd != '\r' && *pEnd != '\n') pEnd++;
            strReqUri = std::string(pFirstSpace + 1, pEnd);
        }
        std::string strToRaw = ExtractHeader(pszMsg, "To:", "t:");
        std::string strFromRaw = ExtractHeader(pszMsg, "From:", "f:");

        std::string strReqDomain = ExtractDomainFromUri(strReqUri);
        std::string strToDomain  = ExtractDomainFromUri(strToRaw);
        std::string strFromDomain = ExtractDomainFromUri(strFromRaw);

        // Request-URI → To → From 순서로 lookup 하여 첫 매치 반환.
        // TX/RX 판별은 Print()에서 메시지 이후에 결정되므로 여기는 공통 로직.
        std::string strService;
        auto it = m_mapDomainToService.find(strReqDomain);
        if (it == m_mapDomainToService.end()) it = m_mapDomainToService.find(strToDomain);
        if (it == m_mapDomainToService.end()) it = m_mapDomainToService.find(strFromDomain);
        if (it != m_mapDomainToService.end()) strService = it->second;

        if (strService.empty()) {
            // 도메인 미등록 → 빈값 ("" — flow에서 service key 생략)
            return "";
        }

        // Cache for subsequent responses with same Call-ID
        if (!strCallId.empty()) {
            if (m_mapCallService.size() > MAX_CALLID_CACHE) {
                auto itE = m_mapCallService.begin();
                size_t half = m_mapCallService.size() / 2;
                for (size_t i = 0; i < half && itE != m_mapCallService.end(); ++i) {
                    itE = m_mapCallService.erase(itE);
                }
            }
            m_mapCallService[strCallId] = strService;
        }

        return strService;
    }

    // Response: Call-ID 캐시에서 조회
    if (!strCallId.empty()) {
        auto it = m_mapCallService.find(strCallId);
        if (it != m_mapCallService.end()) return it->second;
    }

    return "";
}

/**
 * ILogCallBack::Print - called from CLog with the already-formatted message.
 */
void CSipMessageLogger::Print(EnumLogLevel eLevel, const char* fmt, ...)
{
    if (!m_bEnabled) return;
    if (eLevel != LOG_NETWORK) return;

    // Format the string from CLog callback
    char szBuf[1024 * 8];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(szBuf, sizeof(szBuf) - 1, fmt, ap);
    va_end(ap);
    szBuf[sizeof(szBuf) - 1] = '\0';

    // Skip the "[threadid] " prefix
    const char* pszMsg = szBuf;
    if (*pszMsg == '[') {
        const char* p = strchr(pszMsg, ']');
        if (p) {
            pszMsg = p + 1;
            while (*pszMsg == ' ') pszMsg++;
        }
    }

    // Determine direction: UdpSend = TX, UdpRecv/TcpRecv = RX
    const char* pszDir = NULL;
    const char* pszAfterParen = NULL;

    if (strncmp(pszMsg, "UdpSend(", 8) == 0 || strncmp(pszMsg, "TcpSend(", 8) == 0 ||
        strncmp(pszMsg, "TlsSend(", 8) == 0) {
        pszDir = "TX";
        pszAfterParen = pszMsg + 8;
    } else if (strncmp(pszMsg, "UdpRecv(", 8) == 0 || strncmp(pszMsg, "TcpRecv(", 8) == 0 ||
               strncmp(pszMsg, "TlsRecv(", 8) == 0) {
        pszDir = "RX";
        pszAfterParen = pszMsg + 8;
    } else {
        return;
    }

    // Extract peer IP:PORT from "IP:PORT) ..."
    char szPeer[64] = {0};
    const char* pClose = strchr(pszAfterParen, ')');
    if (pClose && (pClose - pszAfterParen) < (int)sizeof(szPeer)) {
        strncpy(szPeer, pszAfterParen, pClose - pszAfterParen);
        szPeer[pClose - pszAfterParen] = '\0';
    }

    // Find the SIP message body
    const char* pszSipMsg = NULL;
    if (pClose) {
        pszSipMsg = pClose + 1;
        while (*pszSipMsg == ' ' || *pszSipMsg == '\r' || *pszSipMsg == '\n' || *pszSipMsg == '[') pszSipMsg++;
    }

    if (!pszSipMsg || *pszSipMsg == '\0') return;

    // Extract SIP headers
    std::string strCallId = ExtractHeader(pszSipMsg, "Call-ID:", "i:");
    std::string strMethod = ExtractMethodOrStatus(pszSipMsg);
    std::string strFromRaw = ExtractHeader(pszSipMsg, "From:", "f:");
    std::string strToRaw = ExtractHeader(pszSipMsg, "To:", "t:");
    std::string strFromUri = ExtractUriUser(strFromRaw);
    std::string strToUri = ExtractUriUser(strToRaw);
    std::string strCSeq = ExtractHeader(pszSipMsg, "CSeq:", NULL);

    std::string strTs = GetTimestamp();
    std::string strFlowHourDir = GetFlowHourDir();
    std::string strMsgHourDir = GetMsgHourDir();

    std::lock_guard<std::mutex> lock(m_mtx);
    EnsureHourlyFiles(strFlowHourDir, strMsgHourDir);

    // Classify service from SIP message domain
    std::string strService = ClassifyService(pszSipMsg, strCallId, strMethod);

    // Clean up Call-ID cache on BYE/CANCEL
    if (strMethod == "BYE" || strMethod == "CANCEL") {
        // Deferred cleanup: leave in cache for responses, will be evicted by size limit
    }

    // SIP: TX=csp->ue, RX=ue->csp
    const char* pszFrom = (strcmp(pszDir, "TX") == 0) ? "csp" : "ue";
    const char* pszTo   = (strcmp(pszDir, "TX") == 0) ? "ue"  : "csp";

    // 1. {system_id}_sip.jsonl (per-interface)
    int iSeq = 0;
    if (m_bRawLogEnabled) {
        iSeq = WriteInterfaceLine("sip", strTs.c_str(), pszDir, szPeer, "SIP", pszSipMsg,
                                   strFromUri.c_str(), strToUri.c_str());
    }

    // SIP은 CSP 단독 기록이므로 mid 불필요

    // detail 생성: INVITE=from→to, REGISTER=user, 응답=빈값
    std::string strDetail;
    if (strMethod == "INVITE" && !strFromUri.empty() && !strToUri.empty()) {
        strDetail = strFromUri + "\xe2\x86\x92" + strToUri;  // UTF-8 →
    } else if (strMethod == "REGISTER" && !strFromUri.empty()) {
        strDetail = strFromUri;
    } else if (strMethod == "SUBSCRIBE" && !strToUri.empty()) {
        strDetail = strToUri;
    } else if (strMethod == "BYE" && !strFromUri.empty()) {
        strDetail = strFromUri;
    }

    // sesid 조회/발행: 같은 Call-ID는 동일 sesid 유지
    // REGISTER/INVITE/SUBSCRIBE 등 모든 SIP 메시지에 대해 발행/계승
    std::string strSesId;
    if (!strCallId.empty()) {
        auto itSes = m_mapCallSesId.find(strCallId);
        if (itSes != m_mapCallSesId.end() && !itSes->second.empty()) {
            strSesId = itSes->second;
        } else {
            // 신규 발행: caller = From URI (있을 때)
            strSesId = IssueSesId(strFromUri, "csp");
            if (m_mapCallSesId.size() >= MAX_CALLID_CACHE) {
                m_mapCallSesId.erase(m_mapCallSesId.begin());
            }
            m_mapCallSesId[strCallId] = strSesId;
        }
    } else {
        // Call-ID 없는 경우(드문 상황): 일회성 sesid
        strSesId = IssueSesId(strFromUri, "csp");
    }

    // subid: VoLTE=Call-ID(leg 구분), PTT=session_seq(캐시 조회)
    std::string strSubId;
    if (strService == "phone" || strService == "volte") {
        strSubId = strCallId;
    } else {
        auto itSub = m_mapCallSubId.find(strCallId);
        if (itSub != m_mapCallSubId.end()) strSubId = itSub->second;
    }

    // 2. flow.jsonl (SIP: mid 불필요)
    //    caller/callee는 From/To URI에서 추출 (Request/Response 모두 동일하게 From=originator, To=target)
    WriteFlowLine(strService.c_str(), strTs.c_str(), pszFrom, pszTo, "SIP",
                  strMethod.c_str(), strDetail.c_str(), "",
                  strSesId.c_str(), strSubId.c_str(),
                  iSeq, "sip",
                  strFromUri.c_str(), strToUri.c_str());
}

void CSipMessageLogger::LogMessage(const char* pszFrom, const char* pszTo,
                                    const char* pszProto, const char* pszMethod,
                                    const char* pszPeer, const char* pszBody,
                                    const char* pszService,
                                    const char* pszTxId,
                                    const char* pszSesId,
                                    const char* pszDetail,
                                    const char* pszCaller,
                                    const char* pszCallee)
{
    if (!m_bEnabled) return;

    std::string strTs = GetTimestamp();
    const char* proto = (pszProto && *pszProto) ? pszProto : "JSON";
    const char* pszDir = (pszFrom && strcmp(pszFrom, "csp") == 0) ? "TX" : "RX";
    const char* service = (pszService && *pszService) ? pszService : "";
    std::string strFlowHourDir = GetFlowHourDir();
    std::string strMsgHourDir = GetMsgHourDir();

    // Determine interface from proto
    const char* iface = "cmp";
    if (proto && strcmp(proto, "CSC") == 0) {
        iface = "csc";
    } else if (proto && strcmp(proto, "SIP") == 0) {
        iface = "sip";
    }

    std::lock_guard<std::mutex> lock(m_mtx);
    EnsureHourlyFiles(strFlowHourDir, strMsgHourDir);

    // 1. {system_id}_{iface}.jsonl (per-interface)
    int iSeq = 0;
    if (m_bRawLogEnabled) {
        iSeq = WriteInterfaceLine(iface, strTs.c_str(), pszDir,
                                  pszPeer ? pszPeer : "",
                                  proto, pszBody ? pszBody : "",
                                  pszCaller, pszCallee);
    }

    // 2. flow.jsonl
    WriteFlowLine(service, strTs.c_str(), pszFrom ? pszFrom : "", pszTo ? pszTo : "",
                  proto, pszMethod ? pszMethod : "",
                  pszDetail ? pszDetail : "", pszTxId ? pszTxId : "",
                  pszSesId ? pszSesId : "", "",
                  iSeq, iface,
                  pszCaller, pszCallee);
}

void CSipMessageLogger::EnsureHourlyFiles(const std::string& strFlowHourDir,
                                           const std::string& strMsgHourDir)
{
    // Called under m_mtx lock
    bool bFlowChanged = (strFlowHourDir != m_strCurrentFlowHourDir);
    bool bMsgChanged  = (strMsgHourDir != m_strCurrentMsgHourDir);

    if (!bFlowChanged && !bMsgChanged) return;

    // Close all existing files on any change
    CloseAllFiles();

    // Update flow dir
    if (bFlowChanged && !strFlowHourDir.empty()) {
        MkdirP(strFlowHourDir);
        m_strCurrentFlowHourDir = strFlowHourDir;
    }

    // Update msg dir and open interface files
    if (bMsgChanged && !strMsgHourDir.empty()) {
        MkdirP(strMsgHourDir);
        m_strCurrentMsgHourDir = strMsgHourDir;

        // Open per-interface files and count existing lines for seq continuity
        const char* ifaces[] = { "sip", "cmp", "csc" };
        FILE** files[] = { &m_pSipFile, &m_pCmpFile, &m_pCscFile };
        int* seqs[] = { &m_iSipSeq, &m_iCmpSeq, &m_iCscSeq };

        for (int i = 0; i < 3; i++) {
            std::string path = strMsgHourDir + "/" + m_strSystemId + "_" + ifaces[i] + ".msg.jsonl";
            *files[i] = fopen(path.c_str(), "a+");
            *seqs[i] = 0;
            if (*files[i]) {
                fseek(*files[i], 0, SEEK_SET);
                char buf[4096];
                while (fgets(buf, sizeof(buf), *files[i])) (*seqs[i])++;
                fseek(*files[i], 0, SEEK_END);
            }
        }
    }

    // Flow files are opened on first write (lazy) via GetFlowFile()
}

FILE* CSipMessageLogger::GetFlowFile()
{
    if (m_strCurrentFlowHourDir.empty()) return NULL;
    if (!m_pFlowFile) {
        std::string path = m_strCurrentFlowHourDir + "/" + m_strSystemId + ".flow.jsonl";
        m_pFlowFile = fopen(path.c_str(), "a");
    }
    return m_pFlowFile;
}

FILE* CSipMessageLogger::GetInterfaceFile(const char* pszIface)
{
    // Called under m_mtx lock
    if (strcmp(pszIface, "sip") == 0) return m_pSipFile;
    if (strcmp(pszIface, "cmp") == 0) return m_pCmpFile;
    if (strcmp(pszIface, "csc") == 0) return m_pCscFile;
    return m_pSipFile; // fallback
}

int& CSipMessageLogger::GetIfaceSeq(const char* pszIface)
{
    if (strcmp(pszIface, "cmp") == 0) return m_iCmpSeq;
    if (strcmp(pszIface, "csc") == 0) return m_iCscSeq;
    return m_iSipSeq; // default/sip
}

void CSipMessageLogger::WriteFlowLine(const char* pszService,
                                       const char* pszTs, const char* pszFrom, const char* pszTo,
                                       const char* pszProto, const char* pszMethod,
                                       const char* pszDetail,
                                       const char* pszTxId,
                                       const char* pszSesId, const char* pszSubId,
                                       int iSeq, const char* pszIface,
                                       const char* pszCaller, const char* pszCallee)
{
    FILE* pFile = GetFlowFile();
    if (!pFile) return;

    // 순서: ts, service, caller, callee, sesid, subid, node, from, to,
    //       proto, method, detail, mid, seq, iface
    // 빈 값은 key 생략.
    bool bFirst = true;
    auto emit = [&](const char* key, const std::string& val, bool isNumeric = false, int iNum = 0) {
        if (!isNumeric && val.empty()) return;
        if (!bFirst) fprintf(pFile, ",");
        bFirst = false;
        if (isNumeric) {
            fprintf(pFile, "\"%s\":%d", key, iNum);
        } else {
            fprintf(pFile, "\"%s\":\"%s\"", key, val.c_str());
        }
    };

    fprintf(pFile, "{");
    emit("ts",      pszTs ? pszTs : "");
    emit("service", pszService ? pszService : "");
    emit("caller",  pszCaller ? JsonEsc(pszCaller) : "");
    emit("callee",  pszCallee ? JsonEsc(pszCallee) : "");
    emit("sesid",   pszSesId ? JsonEsc(pszSesId) : "");
    emit("subid",   pszSubId ? JsonEsc(pszSubId) : "");
    emit("node",    m_strNodeName);
    emit("from",    pszFrom ? pszFrom : "");
    emit("to",      pszTo ? pszTo : "");
    emit("proto",   pszProto ? pszProto : "");
    emit("method",  pszMethod ? JsonEsc(pszMethod) : "");
    emit("detail",  pszDetail ? JsonEsc(pszDetail) : "");
    emit("mid",     pszTxId ? JsonEsc(pszTxId) : "");
    if (iSeq > 0) emit("seq", "", true, iSeq);
    emit("iface",   pszIface ? pszIface : "");
    fprintf(pFile, "}\n");
    fflush(pFile);
}

int CSipMessageLogger::WriteInterfaceLine(const char* pszIface, const char* pszTs, const char* pszDir,
                                           const char* pszPeer, const char* pszProto,
                                           const char* pszMsg,
                                           const char* pszCaller, const char* pszCallee)
{
    // Called under m_mtx lock
    FILE* pFile = GetInterfaceFile(pszIface);
    if (!pFile) return 0;

    std::string strEscMsg = JsonEsc(pszMsg);

    int& iSeq = GetIfaceSeq(pszIface);
    iSeq++;

    // 순서: ts, dir, peer, caller, callee, proto, msg
    fprintf(pFile, "{\"ts\":\"%s\",\"dir\":\"%s\",\"peer\":\"%s\"",
            pszTs ? pszTs : "", pszDir ? pszDir : "", pszPeer ? pszPeer : "");
    if (pszCaller && pszCaller[0])
        fprintf(pFile, ",\"caller\":\"%s\"", JsonEsc(pszCaller).c_str());
    if (pszCallee && pszCallee[0])
        fprintf(pFile, ",\"callee\":\"%s\"", JsonEsc(pszCallee).c_str());
    fprintf(pFile, ",\"proto\":\"%s\",\"msg\":\"%s\"}\n",
            pszProto ? pszProto : "", strEscMsg.c_str());
    fflush(pFile);

    return iSeq;
}

std::string CSipMessageLogger::GetFlowHourDir()
{
    if (m_strFlowBaseDir.empty()) return "";
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/%04d/%02d/%02d/%02d",
             m_strFlowBaseDir.c_str(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);
    return buf;
}

std::string CSipMessageLogger::GetMsgHourDir()
{
    if (m_strMsgBaseDir.empty()) return "";
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/%04d/%02d/%02d/%02d",
             m_strMsgBaseDir.c_str(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);
    return buf;
}

std::string CSipMessageLogger::GetTimestamp()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06d",
             t.tm_hour, t.tm_min, t.tm_sec, (int)tv.tv_usec);
    return buf;
}

std::string CSipMessageLogger::JsonEsc(const char* s, int maxLen)
{
    if (!s) return "";
    std::string r;
    int len = (maxLen > 0) ? maxLen : (int)strlen(s);
    r.reserve(len + 32);
    for (int i = 0; i < len && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 0x20) {
                    char h[8];
                    snprintf(h, sizeof(h), "\\u%04x", c);
                    r += h;
                } else {
                    r += (char)c;
                }
        }
    }
    return r;
}

std::string CSipMessageLogger::ExtractHeader(const char* pszMsg, const char* pszHeader, const char* pszShort)
{
    if (!pszMsg) return "";

    const char* pHeaders[] = { pszHeader, pszShort };
    for (int h = 0; h < 2; h++) {
        if (!pHeaders[h]) continue;
        int hlen = (int)strlen(pHeaders[h]);
        const char* p = pszMsg;
        while (*p) {
            if (strncasecmp(p, pHeaders[h], hlen) == 0) {
                p += hlen;
                while (*p == ' ' || *p == '\t') p++;
                const char* eol = p;
                while (*eol && *eol != '\r' && *eol != '\n') eol++;
                return std::string(p, eol - p);
            }
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }
    return "";
}

std::string CSipMessageLogger::ExtractMethodOrStatus(const char* pszMsg)
{
    if (!pszMsg) return "";
    const char* eol = pszMsg;
    while (*eol && *eol != '\r' && *eol != '\n') eol++;

    std::string firstLine(pszMsg, eol - pszMsg);

    if (firstLine.find("SIP/2.0") == 0) {
        size_t sp = firstLine.find(' ');
        if (sp != std::string::npos) {
            size_t sp2 = firstLine.find(' ', sp + 1);
            if (sp2 != std::string::npos)
                return firstLine.substr(sp + 1, sp2 - sp - 1);
            return firstLine.substr(sp + 1);
        }
    }

    size_t sp = firstLine.find(' ');
    if (sp != std::string::npos) return firstLine.substr(0, sp);

    return firstLine;
}

std::string CSipMessageLogger::ExtractUriUser(const std::string& strHeaderValue)
{
    if (strHeaderValue.empty()) return "";

    std::string result;
    size_t pos = strHeaderValue.find("sip:");
    if (pos == std::string::npos) pos = strHeaderValue.find("tel:");
    if (pos == std::string::npos) return "";

    pos += 4;
    while (pos < strHeaderValue.size()) {
        char c = strHeaderValue[pos];
        if (c == '@' || c == '>' || c == ';' || c == ' ') break;
        result += c;
        pos++;
    }
    return result;
}

bool CSipMessageLogger::MkdirP(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return true;
    size_t pos = path.rfind('/');
    if (pos != std::string::npos && pos > 0)
        MkdirP(path.substr(0, pos));
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}
