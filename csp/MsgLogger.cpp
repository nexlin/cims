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
    EnsureDirRecursive(m_strLogDir);
}

// ── 내부 헬퍼 ─────────────────────────────────────────────────────────────────

bool CMsgLogger::EnsureDirRecursive(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return true;

    // 부모 디렉터리부터 재귀 생성
    size_t pos = path.rfind('/');
    if (pos != std::string::npos && pos > 0) {
        EnsureDirRecursive(path.substr(0, pos));
    }
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
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

void CMsgLogger::GetDateParts(std::string& yyyy, std::string& mm, std::string& dd)
{
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[8];
    snprintf(buf, sizeof(buf), "%04d", t.tm_year + 1900); yyyy = buf;
    snprintf(buf, sizeof(buf), "%02d", t.tm_mon + 1);     mm = buf;
    snprintf(buf, sizeof(buf), "%02d", t.tm_mday);        dd = buf;
}

std::string CMsgLogger::GetTimeShort()
{
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d%02d%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

std::string CMsgLogger::SanitizeForPath(const std::string& s, int maxLen)
{
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<'  || c == '>'  || c == '|' || c == ' ')
            r += '_';
        else
            r += c;
    }
    if ((int)r.size() > maxLen) r = r.substr(0, maxLen);
    return r;
}

std::string CMsgLogger::CallerPrefix(const std::string& caller)
{
    // 뒷 2자리를 제외한 앞자리 (예: "+821357007002" → "+8213570070")
    if (caller.size() <= 2) return caller;
    return caller.substr(0, caller.size() - 2);
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

// ── caller/callee 정보 등록 ───────────────────────────────────────────────────

void CMsgLogger::SetCallInfo(const char* callId, const char* caller, const char* callee)
{
    if (!callId || !callId[0]) return;
    std::lock_guard<std::mutex> lock(m_mtx);
    std::string key(callId);
    if (m_mapCallInfo.find(key) == m_mapCallInfo.end()) {
        CallInfo info;
        info.strCaller = caller ? caller : "unknown";
        info.strCallee = callee ? callee : "";
        m_mapCallInfo[key] = info;
    }
}

// ── 디렉터리 경로 생성 ───────────────────────────────────────────────────────

std::string CMsgLogger::BuildCallDir(const std::string& callId)
{
    // 캐시 확인 (lock 보유 상태에서 호출)
    auto itDir = m_mapCallDir.find(callId);
    if (itDir != m_mapCallDir.end()) return itDir->second;

    // caller/callee 정보 조회 (없으면 기본값)
    std::string caller = "unknown";
    std::string callee;
    auto itInfo = m_mapCallInfo.find(callId);
    if (itInfo != m_mapCallInfo.end()) {
        caller = itInfo->second.strCaller;
        callee = itInfo->second.strCallee;
    }

    // 경로 구성: msg_logs/YYYY/MM/DD/{prefix}/{caller}/{HHmmss}_{caller}_{callee}_{callid_short}.d
    std::string yyyy, mm, dd;
    GetDateParts(yyyy, mm, dd);

    std::string prefix = SanitizeForPath(CallerPrefix(caller), 20);
    std::string safeCaller = SanitizeForPath(caller, 20);
    std::string safeCallee = SanitizeForPath(callee, 20);
    std::string callIdShort = SanitizeForPath(callId, 16);
    std::string timeStr = GetTimeShort();

    std::string dirName = timeStr + "_" + safeCaller;
    if (!safeCallee.empty()) dirName += "_" + safeCallee;
    dirName += "_" + callIdShort + ".d";

    std::string fullPath = m_strLogDir + "/" + yyyy + "/" + mm + "/" + dd
                         + "/" + prefix + "/" + safeCaller + "/" + dirName;

    m_mapCallDir[callId] = fullPath;
    return fullPath;
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

    std::string line =
        std::string("{\"ts\":\"")   + GetTimestamp()         + "\","
        "\"from\":\""  + (from  ? from  : "") + "\","
        "\"to\":\""    + (to    ? to    : "") + "\","
        "\"proto\":\"" + (proto ? proto : "") + "\","
        "\"label\":\""  + EscapeJson(label ? label : "") + "\","
        "\"body\":\""   + EscapeJson(body  ? body  : "") + "\"}";

    std::lock_guard<std::mutex> lock(m_mtx);
    std::string callDir = BuildCallDir(callId);
    std::string filePath = callDir + "/" + m_strComponent + ".jsonl";

    EnsureDirRecursive(callDir);

    FILE* f = fopen(filePath.c_str(), "a");
    if (f) {
        fprintf(f, "%s\n", line.c_str());
        fclose(f);
    }

    // 캐시 크기 제한 (메모리 누수 방지, 오래된 항목 정리)
    if (m_mapCallDir.size() > 10000) {
        m_mapCallDir.clear();
        m_mapCallInfo.clear();
    }
}
