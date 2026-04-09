#include "MsgLogger.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

CMsgLogger gclsMsgLogger;

void CMsgLogger::Init(const std::string& msgLogDir, const std::string& component)
{
    if (msgLogDir.empty()) return;
    m_strBaseDir = msgLogDir + "/" + component;
    EnsureDirRecursive(m_strBaseDir);
}

// ── 새 API: 통계용 간결 로그 ──────────────────────────────────

void CMsgLogger::Log(const char* iface, const char* dir, const char* proto,
                     const char* method, const char* peer)
{
    if (m_strBaseDir.empty()) return;

    std::string yyyy, mm, dd, hh;
    GetDateHourParts(yyyy, mm, dd, hh);

    std::string dirPath = m_strBaseDir + "/" + yyyy + "/" + mm + "/" + dd + "/" + hh;

    std::string ts = GetTimestamp();
    char line[512];
    snprintf(line, sizeof(line),
        "{\"ts\":\"%s\",\"dir\":\"%s\",\"proto\":\"%s\",\"method\":\"%s\",\"peer\":\"%s\"}",
        ts.c_str(), dir ? dir : "", proto ? proto : "",
        method ? method : "", peer ? peer : "");

    std::lock_guard<std::mutex> lock(m_mtx);
    EnsureDirRecursive(dirPath);

    std::string filePath = dirPath + "/" + (iface ? iface : "unknown") + ".jsonl";
    FILE* f = fopen(filePath.c_str(), "a");
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

// ── 하위 호환: 기존 Log(callId, from, to, proto, label, body) ──

void CMsgLogger::Log(const char* callId, const char* from, const char* to,
                     const char* proto, const char* label, const char* body)
{
    (void)callId; (void)body;  // 통계 로그에서는 무시
    // from/to로 방향 결정
    const char* dir = "out";
    if (from && (strcmp(from, "ue") == 0 || strcmp(from, "cwrtc") == 0))
        dir = "in";
    // 인터페이스 결정
    const char* iface = "sip";
    if ((to && strcmp(to, "cmp") == 0) || (from && strcmp(from, "cmp") == 0))
        iface = "cmp";
    else if ((to && strcmp(to, "csc") == 0) || (from && strcmp(from, "csc") == 0))
        iface = "csc";

    Log(iface, dir, proto, label, "");
}

// ── 유틸리티 ──────────────────────────────────────────────────

bool CMsgLogger::EnsureDirRecursive(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return true;
    size_t pos = path.rfind('/');
    if (pos != std::string::npos && pos > 0)
        EnsureDirRecursive(path.substr(0, pos));
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

std::string CMsgLogger::GetTimestamp()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

void CMsgLogger::GetDateHourParts(std::string& yyyy, std::string& mm, std::string& dd, std::string& hh)
{
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[8];
    snprintf(buf, sizeof(buf), "%04d", t.tm_year + 1900); yyyy = buf;
    snprintf(buf, sizeof(buf), "%02d", t.tm_mon + 1);     mm = buf;
    snprintf(buf, sizeof(buf), "%02d", t.tm_mday);        dd = buf;
    snprintf(buf, sizeof(buf), "%02d", t.tm_hour);        hh = buf;
}
