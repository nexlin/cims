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
      m_pPhoneFlowFile(NULL), m_pPttFlowFile(NULL), m_pSystemFlowFile(NULL),
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
    if (m_pPhoneFlowFile)  { fclose(m_pPhoneFlowFile);  m_pPhoneFlowFile = NULL; }
    if (m_pPttFlowFile)    { fclose(m_pPttFlowFile);    m_pPttFlowFile = NULL; }
    if (m_pSystemFlowFile) { fclose(m_pSystemFlowFile); m_pSystemFlowFile = NULL; }
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
    m_bRawLogEnabled = bRawLogEnabled;
    if (!m_strFlowBaseDir.empty()) MkdirP(m_strFlowBaseDir);
    if (!m_strMsgBaseDir.empty()) MkdirP(m_strMsgBaseDir);
    m_bEnabled = true;
}

void CSipMessageLogger::SetRealms(const std::string& strVoipRealm, const std::string& strPttRealm)
{
    m_strVoipRealm = strVoipRealm;
    m_strPttRealm = strPttRealm;
}

std::string CSipMessageLogger::ClassifyService(const char* pszMsg, const std::string& strCallId, const std::string& strMethod)
{
    // HEARTBEAT and OPTIONS are always system
    if (strMethod == "OPTIONS" || strMethod == "HEARTBEAT") {
        return "system";
    }

    // For requests (INVITE, REGISTER, SUBSCRIBE, etc.), extract domain from Request-URI and To header
    // For responses (100/180/200/etc.), lookup by Call-ID cache
    bool bIsResponse = (pszMsg && strncmp(pszMsg, "SIP/2.0", 7) == 0);

    if (!bIsResponse && pszMsg) {
        // Extract Request-URI domain and To header domain
        std::string strReqUri;
        // Request-URI is on the first line: "METHOD sip:user@domain ..."
        const char* pFirstSpace = strchr(pszMsg, ' ');
        if (pFirstSpace) {
            const char* pEnd = pFirstSpace + 1;
            while (*pEnd && *pEnd != ' ' && *pEnd != '\r' && *pEnd != '\n') pEnd++;
            strReqUri = std::string(pFirstSpace + 1, pEnd);
        }

        std::string strToRaw = ExtractHeader(pszMsg, "To:", "t:");

        // Check domains
        std::string strService = "system";
        if (!m_strVoipRealm.empty()) {
            if (strReqUri.find(m_strVoipRealm) != std::string::npos ||
                strToRaw.find(m_strVoipRealm) != std::string::npos) {
                strService = "phone";
            }
        }
        if (strService == "system" && !m_strPttRealm.empty()) {
            if (strReqUri.find(m_strPttRealm) != std::string::npos ||
                strToRaw.find(m_strPttRealm) != std::string::npos) {
                strService = "ptt";
            }
        }

        // Cache for subsequent messages with same Call-ID
        if (!strCallId.empty() && strService != "system") {
            // Evict oldest entries if cache is too large
            if (m_mapCallService.size() > MAX_CALLID_CACHE) {
                // Simple eviction: clear half the cache
                auto it = m_mapCallService.begin();
                size_t half = m_mapCallService.size() / 2;
                for (size_t i = 0; i < half && it != m_mapCallService.end(); ++i) {
                    it = m_mapCallService.erase(it);
                }
            }
            m_mapCallService[strCallId] = strService;
        }

        return strService;
    }

    // Response or non-parseable: lookup by Call-ID
    if (!strCallId.empty()) {
        auto it = m_mapCallService.find(strCallId);
        if (it != m_mapCallService.end()) {
            return it->second;
        }
    }

    return "system";
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
        iSeq = WriteInterfaceLine("sip", strTs.c_str(), pszDir, szPeer, "SIP", pszSipMsg);
    }

    // 2. {system_id}_{service}_flow.jsonl (with seq + iface linkage)
    WriteFlowLine(strService.c_str(), strTs.c_str(), pszFrom, pszTo, "SIP",
                  strMethod.c_str(), strCallId.c_str(),
                  strFromUri.c_str(), strToUri.c_str(), szPeer, iSeq, "sip");
}

void CSipMessageLogger::LogMessage(const char* pszFrom, const char* pszTo,
                                    const char* pszProto, const char* pszMethod,
                                    const char* pszPeer, const char* pszBody,
                                    const char* pszService)
{
    if (!m_bEnabled) return;

    std::string strTs = GetTimestamp();
    const char* proto = (pszProto && *pszProto) ? pszProto : "JSON";
    const char* pszDir = (pszFrom && strcmp(pszFrom, "csp") == 0) ? "TX" : "RX";
    const char* service = (pszService && *pszService) ? pszService : "system";
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
                                  proto, pszBody ? pszBody : "");
    }

    // 2. {system_id}_{service}_flow.jsonl (with seq + iface linkage)
    WriteFlowLine(service, strTs.c_str(), pszFrom ? pszFrom : "", pszTo ? pszTo : "",
                  proto, pszMethod ? pszMethod : "",
                  "", "", "", pszPeer ? pszPeer : "", iSeq, iface);
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
            std::string path = strMsgHourDir + "/" + m_strSystemId + "_" + ifaces[i] + ".jsonl";
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

FILE* CSipMessageLogger::GetFlowFile(const char* pszService)
{
    // Called under m_mtx lock
    if (m_strCurrentFlowHourDir.empty()) return NULL;

    FILE** ppFile = NULL;
    std::string strFileName;

    if (strcmp(pszService, "phone") == 0) {
        ppFile = &m_pPhoneFlowFile;
        strFileName = m_strSystemId + "_phone_flow.jsonl";
    } else if (strcmp(pszService, "ptt") == 0) {
        ppFile = &m_pPttFlowFile;
        strFileName = m_strSystemId + "_ptt_flow.jsonl";
    } else {
        ppFile = &m_pSystemFlowFile;
        strFileName = m_strSystemId + "_system_flow.jsonl";
    }

    if (!*ppFile) {
        *ppFile = fopen((m_strCurrentFlowHourDir + "/" + strFileName).c_str(), "a");
    }
    return *ppFile;
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
                                       const char* pszCallId, const char* pszFromUri, const char* pszToUri,
                                       const char* pszPeer, int iSeq, const char* pszIface)
{
    // Called under m_mtx lock
    FILE* pFile = GetFlowFile(pszService);
    if (!pFile) return;

    fprintf(pFile,
        "{\"ts\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
        "\"proto\":\"%s\",\"method\":\"%s\",\"call_id\":\"%s\","
        "\"from_uri\":\"%s\",\"to_uri\":\"%s\",\"peer\":\"%s\","
        "\"seq\":%d,\"iface\":\"%s\"}\n",
        pszTs ? pszTs : "",
        pszFrom ? pszFrom : "",
        pszTo ? pszTo : "",
        pszProto ? pszProto : "",
        JsonEsc(pszMethod).c_str(),
        JsonEsc(pszCallId).c_str(),
        JsonEsc(pszFromUri).c_str(),
        JsonEsc(pszToUri).c_str(),
        pszPeer ? pszPeer : "",
        iSeq,
        pszIface ? pszIface : "sip");
    fflush(pFile);
}

int CSipMessageLogger::WriteInterfaceLine(const char* pszIface, const char* pszTs, const char* pszDir,
                                           const char* pszPeer, const char* pszProto,
                                           const char* pszMsg)
{
    // Called under m_mtx lock
    FILE* pFile = GetInterfaceFile(pszIface);
    if (!pFile) return 0;

    std::string strEscMsg = JsonEsc(pszMsg);

    int& iSeq = GetIfaceSeq(pszIface);
    iSeq++;
    fprintf(pFile,
        "{\"ts\":\"%s\",\"dir\":\"%s\",\"peer\":\"%s\",\"proto\":\"%s\",\"msg\":\"%s\"}\n",
        pszTs ? pszTs : "",
        pszDir ? pszDir : "",
        pszPeer ? pszPeer : "",
        pszProto ? pszProto : "",
        strEscMsg.c_str());
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
    snprintf(buf, sizeof(buf), "%s/%04d/%02d/%02d/%02d/%s",
             m_strFlowBaseDir.c_str(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             m_strSystemId.c_str());
    return buf;
}

std::string CSipMessageLogger::GetMsgHourDir()
{
    if (m_strMsgBaseDir.empty()) return "";
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/%04d/%02d/%02d/%02d/%s",
             m_strMsgBaseDir.c_str(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             m_strSystemId.c_str());
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
