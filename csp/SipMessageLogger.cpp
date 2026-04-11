#include "SipMessageLogger.h"

#include <cstdarg>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>

CSipMessageLogger gclsSipLogger;

CSipMessageLogger::CSipMessageLogger()
    : m_bEnabled(false), m_pFile(NULL)
{
}

CSipMessageLogger::~CSipMessageLogger()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_pFile) {
        fclose(m_pFile);
        m_pFile = NULL;
    }
}

void CSipMessageLogger::Init(const std::string& strBaseDir)
{
    if (strBaseDir.empty()) return;
    m_strBaseDir = strBaseDir;
    MkdirP(m_strBaseDir);
    m_bEnabled = true;
}

/**
 * ILogCallBack::Print - called from CLog with the already-formatted message.
 *
 * The fmt + varargs from CLog callback look like:
 *   "[threadid] %s"
 * where the second arg is the formatted szBuf from CLog::Print.
 *
 * We only care about LOG_NETWORK level messages.
 * The szBuf content from psip for network messages looks like:
 *   "UdpSend(192.168.0.2:5060) \n<SIP message>"
 *   "UdpRecv(192.168.0.2:5060) \n<SIP message>"
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
        // Not a network send/recv message we recognize
        return;
    }

    // Extract peer IP:PORT from "IP:PORT) ..."
    char szPeer[64] = {0};
    const char* pClose = strchr(pszAfterParen, ')');
    if (pClose && (pClose - pszAfterParen) < (int)sizeof(szPeer)) {
        strncpy(szPeer, pszAfterParen, pClose - pszAfterParen);
        szPeer[pClose - pszAfterParen] = '\0';
    }

    // Find the SIP message body (after ") \n[" — psip wraps msg in brackets)
    const char* pszSipMsg = NULL;
    if (pClose) {
        pszSipMsg = pClose + 1;
        // Skip whitespace, newlines, and opening bracket
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

    WriteJsonl(strTs.c_str(), pszDir, szPeer,
               strCallId.c_str(), strMethod.c_str(),
               strFromUri.c_str(), strToUri.c_str(),
               "SIP", pszSipMsg);
}

void CSipMessageLogger::LogCmp(const char* pszDir, const char* pszPeer,
                                const char* pszMethod, const char* pszBody)
{
    if (!m_bEnabled) return;

    std::string strTs = GetTimestamp();

    WriteJsonl(strTs.c_str(), pszDir, pszPeer,
               "", pszMethod ? pszMethod : "",
               "", "",
               "JSON", pszBody ? pszBody : "");
}

void CSipMessageLogger::WriteJsonl(const char* pszTs, const char* pszDir, const char* pszPeer,
                                    const char* pszCallId, const char* pszMethod,
                                    const char* pszFromUri, const char* pszToUri,
                                    const char* pszProto, const char* pszMsg)
{
    std::string strHourDir = GetHourlyDir();

    std::lock_guard<std::mutex> lock(m_mtx);

    // Hourly rotation
    if (strHourDir != m_strCurrentHourDir) {
        if (m_pFile) {
            fclose(m_pFile);
            m_pFile = NULL;
        }
        MkdirP(strHourDir);
        m_strCurrentHourDir = strHourDir;
        std::string strFilePath = strHourDir + "/sip.jsonl";
        m_pFile = fopen(strFilePath.c_str(), "a");
    }

    if (!m_pFile) return;

    // Build JSONL line
    std::string strEscMsg = JsonEsc(pszMsg);

    fprintf(m_pFile,
        "{\"ts\":\"%s\",\"dir\":\"%s\",\"peer\":\"%s\","
        "\"call_id\":\"%s\",\"method\":\"%s\","
        "\"from_uri\":\"%s\",\"to_uri\":\"%s\"",
        pszTs ? pszTs : "",
        pszDir ? pszDir : "",
        pszPeer ? pszPeer : "",
        JsonEsc(pszCallId).c_str(),
        JsonEsc(pszMethod).c_str(),
        JsonEsc(pszFromUri).c_str(),
        JsonEsc(pszToUri).c_str());

    if (pszProto && strcmp(pszProto, "SIP") != 0) {
        fprintf(m_pFile, ",\"proto\":\"%s\"", pszProto);
    }

    fprintf(m_pFile, ",\"msg\":\"%s\"}\n", strEscMsg.c_str());
    fflush(m_pFile);
}

std::string CSipMessageLogger::GetHourlyDir()
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/%04d/%02d/%02d/%02d",
             m_strBaseDir.c_str(),
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

    // Search for the header (case-sensitive, line-start)
    const char* pHeaders[] = { pszHeader, pszShort };
    for (int h = 0; h < 2; h++) {
        if (!pHeaders[h]) continue;
        int hlen = (int)strlen(pHeaders[h]);
        const char* p = pszMsg;
        while (*p) {
            // Check at beginning of line
            if (strncasecmp(p, pHeaders[h], hlen) == 0) {
                p += hlen;
                while (*p == ' ' || *p == '\t') p++;
                // Read until end of line
                const char* eol = p;
                while (*eol && *eol != '\r' && *eol != '\n') eol++;
                return std::string(p, eol - p);
            }
            // Advance to next line
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }
    return "";
}

std::string CSipMessageLogger::ExtractMethodOrStatus(const char* pszMsg)
{
    if (!pszMsg) return "";
    // First line: "INVITE sip:..." or "SIP/2.0 200 OK"
    const char* eol = pszMsg;
    while (*eol && *eol != '\r' && *eol != '\n') eol++;

    std::string firstLine(pszMsg, eol - pszMsg);

    // Response: starts with "SIP/2.0"
    if (firstLine.find("SIP/2.0") == 0) {
        // Extract status code
        size_t sp = firstLine.find(' ');
        if (sp != std::string::npos) {
            size_t sp2 = firstLine.find(' ', sp + 1);
            if (sp2 != std::string::npos)
                return firstLine.substr(sp + 1, sp2 - sp - 1);
            return firstLine.substr(sp + 1);
        }
    }

    // Request: method is the first token
    size_t sp = firstLine.find(' ');
    if (sp != std::string::npos) return firstLine.substr(0, sp);

    return firstLine;
}

std::string CSipMessageLogger::ExtractUriUser(const std::string& strHeaderValue)
{
    if (strHeaderValue.empty()) return "";

    // Look for "sip:" or "tel:" in the value
    std::string result;
    size_t pos = strHeaderValue.find("sip:");
    if (pos == std::string::npos) pos = strHeaderValue.find("tel:");
    if (pos == std::string::npos) return "";

    pos += 4; // skip "sip:" or "tel:"
    // Read user part until '@', '>', ';', or end
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
