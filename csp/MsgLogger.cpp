#include "MsgLogger.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

CMsgLogger gclsMsgLogger;

// ── 초기화 ────────────────────────────────────────────────────────────────────

void CMsgLogger::Init(const std::string& extMntDir, const std::string& component)
{
    if (extMntDir.empty()) return;
    m_strLogDir    = extMntDir + "/msg_logs";
    m_strComponent = component;
    EnsureDir(m_strLogDir);
}

// ── 내부 헬퍼 ─────────────────────────────────────────────────────────────────

bool CMsgLogger::EnsureDir(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return true;
    return mkdir(path.c_str(), 0755) == 0;
}

std::string CMsgLogger::GetTimestamp()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06ld",
             t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000);
    return buf;
}

std::string CMsgLogger::GetDateStr()
{
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d%02d%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return buf;
}

std::string CMsgLogger::SanitizeCallId(const std::string& callId)
{
    std::string s;
    s.reserve(callId.size());
    for (char c : callId) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<'  || c == '>'  || c == '|' || c == ' ')
            s += '_';
        else
            s += c;
    }
    if (s.size() > 80) s = s.substr(0, 80);
    return s;
}

std::string CMsgLogger::EscapeJson(const std::string& s)
{
    std::string r;
    r.reserve(s.size() + 32);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:
                if (c < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x", c);
                    r += hex;
                } else {
                    r += (char)c;
                }
        }
    }
    return r;
}

// ── 메시지 기록 ───────────────────────────────────────────────────────────────

void CMsgLogger::Log(const char* callId,
                     const char* from,
                     const char* to,
                     const char* proto,
                     const char* label,
                     const char* body)
{
    if (m_strLogDir.empty()) return;
    if (!callId || !callId[0]) return;

    std::string safeId  = SanitizeCallId(callId);
    std::string dateDir = m_strLogDir + "/" + GetDateStr();
    std::string callDir = dateDir + "/" + safeId;
    std::string filePath = callDir + "/" + m_strComponent + ".jsonl";

    std::string line =
        std::string("{\"ts\":\"")   + GetTimestamp()         + "\","
        "\"from\":\""  + (from  ? from  : "") + "\","
        "\"to\":\""    + (to    ? to    : "") + "\","
        "\"proto\":\"" + (proto ? proto : "") + "\","
        "\"label\":\""  + EscapeJson(label ? label : "") + "\","
        "\"body\":\""   + EscapeJson(body  ? body  : "") + "\"}";

    std::lock_guard<std::mutex> lock(m_mtx);
    EnsureDir(dateDir);
    EnsureDir(callDir);

    FILE* f = fopen(filePath.c_str(), "a");
    if (f) {
        fprintf(f, "%s\n", line.c_str());
        fclose(f);
    }
}
