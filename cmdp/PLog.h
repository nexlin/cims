#ifndef __CMP_LOG_H__
#define __CMP_LOG_H__

/**
 * PLog.h — CMP 서버 체계적 로깅 유틸리티 (Header-Only)
 *
 * 기능:
 *   - 타임스탬프 자동 추가 [YYYY-MM-DD HH:MM:SS.mmm]
 *   - 로그 레벨: ERROR / WARN / INFO / DEBUG
 *   - 반복 로그 억제 (LOG_THROTTLE)
 *   - 파일 출력 + 로테이션 (크기 기반)
 *
 * 로테이션 동작:
 *   cmp.log (현재) → cmp.log.1 → cmp.log.2 → ... → cmp.log.N (삭제)
 *   LogMaxSizeMB 초과 시 자동 회전, 최대 LogMaxFiles 개 보관
 *
 * 사용법:
 *   // 초기화 (파일 로깅)
 *   PLog::Instance().InitFile("../log", "cmp", 10, 5);  // 10MB, 5개 파일
 *
 *   LOG_INFO("CmpServer", "allocResource port=%d", port);
 *   LOG_ERROR("CmpServer", "bind failed: %s", strerror(errno));
 *   LOG_THROTTLE(2, "McpttGroup", "Member %s left.", sessionId);
 */

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <sys/time.h>
#include <sys/stat.h>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <string>

enum CmpLogLevel {
    CMP_LOG_ERROR = 0,
    CMP_LOG_WARN  = 1,
    CMP_LOG_INFO  = 2,
    CMP_LOG_DEBUG = 3
};

class PLog {
public:
    static PLog& Instance() {
        static PLog inst;
        return inst;
    }

    void SetLevel(CmpLogLevel level) { _level = level; }
    CmpLogLevel GetLevel() const { return _level; }

    /**
     * 파일 로깅 초기화
     * @param logDir      로그 디렉토리 경로 (빈 문자열이면 stdout만 사용)
     * @param baseName    로그 파일 기본 이름 (예: "cmp" → cmp.log)
     * @param maxSizeMB   파일 최대 크기(MB), 초과 시 로테이션
     * @param maxFiles    보관할 최대 로그 파일 수 (로테이션 백업 수)
     */
    void InitFile(const std::string& logDir, const std::string& baseName,
                  int maxSizeMB = 10, int maxFiles = 5) {
        std::lock_guard<std::mutex> lock(_fileMtx);

        _logDir = logDir;
        _baseName = baseName;
        _maxSizeBytes = (long)maxSizeMB * 1024L * 1024L;
        _maxFiles = maxFiles;

        if (_logDir.empty()) return;

        // Ensure directory exists
        ensureDir(_logDir);

        // Build current log path
        _currentPath = _logDir + "/" + _baseName + ".log";

        // Open (append mode)
        _fp = fopen(_currentPath.c_str(), "a");
        if (!_fp) {
            fprintf(stderr, "[PLog] Failed to open log file: %s\n", _currentPath.c_str());
        }
    }

    void Print(CmpLogLevel level, const char* tag, const char* fmt, ...) {
        if (level > _level) return;

        char timeBuf[32];
        getTimestamp(timeBuf, sizeof(timeBuf));
        const char* lvl = levelStr(level);

        char msgBuf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msgBuf, sizeof(msgBuf), fmt, ap);
        va_end(ap);

        char lineBuf[2200];
        int lineLen = snprintf(lineBuf, sizeof(lineBuf), "[%s] [%s] [%s] %s\n", timeBuf, lvl, tag, msgBuf);

        writeLog(lineBuf, lineLen);
    }

    bool PrintThrottled(int intervalSec, CmpLogLevel level, const char* tag, const char* fmt, ...) {
        if (level > _level) return false;

        char msgBuf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msgBuf, sizeof(msgBuf), fmt, ap);
        va_end(ap);

        std::string key = std::string(tag) + "|" + msgBuf;

        struct timeval tv;
        gettimeofday(&tv, NULL);
        long nowMs = tv.tv_sec * 1000L + tv.tv_usec / 1000;

        std::lock_guard<std::mutex> lock(_throttleMtx);
        auto it = _throttleMap.find(key);
        if (it != _throttleMap.end()) {
            ThrottleEntry& e = it->second;
            long elapsedMs = nowMs - e.lastPrintMs;
            if (elapsedMs < intervalSec * 1000) {
                e.suppressedCount++;
                return false;
            }
            char timeBuf[32];
            getTimestamp(timeBuf, sizeof(timeBuf));
            const char* lvl = levelStr(level);
            char lineBuf[2200];
            int lineLen;
            if (e.suppressedCount > 0) {
                lineLen = snprintf(lineBuf, sizeof(lineBuf),
                    "[%s] [%s] [%s] %s (suppressed %d duplicates)\n",
                    timeBuf, lvl, tag, msgBuf, e.suppressedCount);
            } else {
                lineLen = snprintf(lineBuf, sizeof(lineBuf),
                    "[%s] [%s] [%s] %s\n", timeBuf, lvl, tag, msgBuf);
            }
            writeLog(lineBuf, lineLen);
            e.lastPrintMs = nowMs;
            e.suppressedCount = 0;
            return true;
        }

        // First occurrence
        char timeBuf[32];
        getTimestamp(timeBuf, sizeof(timeBuf));
        const char* lvl = levelStr(level);
        char lineBuf[2200];
        int lineLen = snprintf(lineBuf, sizeof(lineBuf),
            "[%s] [%s] [%s] %s\n", timeBuf, lvl, tag, msgBuf);
        writeLog(lineBuf, lineLen);

        ThrottleEntry newEntry;
        newEntry.lastPrintMs = nowMs;
        newEntry.suppressedCount = 0;
        _throttleMap[key] = newEntry;
        return true;
    }

    ~PLog() {
        std::lock_guard<std::mutex> lock(_fileMtx);
        if (_fp) {
            fclose(_fp);
            _fp = NULL;
        }
    }

private:
    PLog() : _level(CMP_LOG_INFO), _fp(NULL), _maxSizeBytes(10L * 1024 * 1024), _maxFiles(5), _written(0) {}

    CmpLogLevel _level;

    // File logging
    FILE* _fp;
    std::string _logDir;
    std::string _baseName;
    std::string _currentPath;
    long _maxSizeBytes;
    int  _maxFiles;
    long _written;  // bytes written since last check
    std::mutex _fileMtx;

    // Throttle
    struct ThrottleEntry {
        long lastPrintMs;
        int  suppressedCount;
    };
    std::unordered_map<std::string, ThrottleEntry> _throttleMap;
    std::mutex _throttleMtx;

    /**
     * 로그 한 줄 출력 — stdout + 파일 (로테이션 포함)
     */
    void writeLog(const char* line, int len) {
        // Always write to stdout
        fputs(line, stdout);
        fflush(stdout);

        // Write to file if configured
        std::lock_guard<std::mutex> lock(_fileMtx);
        if (_fp) {
            fwrite(line, 1, len, _fp);
            fflush(_fp);
            _written += len;

            // Check rotation every 4KB to avoid stat() overhead
            if (_written >= 4096) {
                _written = 0;
                checkRotation();
            }
        }
    }

    /**
     * 파일 크기 확인 후 로테이션 수행
     *
     * 로테이션 방식:
     *   cmp.log.4 (삭제)
     *   cmp.log.3 → cmp.log.4
     *   cmp.log.2 → cmp.log.3
     *   cmp.log.1 → cmp.log.2
     *   cmp.log   → cmp.log.1  (현재 파일 닫고 이동)
     *   cmp.log   (새로 생성)
     */
    void checkRotation() {
        if (!_fp || _currentPath.empty()) return;

        struct stat st;
        if (fstat(fileno(_fp), &st) != 0) return;
        if (st.st_size < _maxSizeBytes) return;

        // Close current
        fclose(_fp);
        _fp = NULL;

        // Rotate: remove oldest, shift others
        std::string base = _logDir + "/" + _baseName + ".log";

        // Remove oldest file if exists
        std::string oldest = base + "." + std::to_string(_maxFiles);
        remove(oldest.c_str());

        // Shift: .N-1 → .N, ... .1 → .2
        for (int i = _maxFiles - 1; i >= 1; --i) {
            std::string src = base + "." + std::to_string(i);
            std::string dst = base + "." + std::to_string(i + 1);
            rename(src.c_str(), dst.c_str());
        }

        // Current → .1
        rename(base.c_str(), (base + ".1").c_str());

        // Open new current
        _fp = fopen(base.c_str(), "a");
    }

    static void ensureDir(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            // mkdir -p equivalent (single level)
            mkdir(path.c_str(), 0755);
        }
    }

    static void getTimestamp(char* buf, int bufLen) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm;
        localtime_r(&tv.tv_sec, &tm);
        int ms = (int)(tv.tv_usec / 1000);
        snprintf(buf, bufLen, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    }

    static const char* levelStr(CmpLogLevel level) {
        switch(level) {
            case CMP_LOG_ERROR: return "E";
            case CMP_LOG_WARN:  return "W";
            case CMP_LOG_INFO:  return "I";
            case CMP_LOG_DEBUG: return "D";
            default: return "?";
        }
    }
};

// ---- Convenience Macros ----

#define LOG_ERROR(tag, fmt, ...) \
    PLog::Instance().Print(CMP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)

#define LOG_WARN(tag, fmt, ...) \
    PLog::Instance().Print(CMP_LOG_WARN, tag, fmt, ##__VA_ARGS__)

#define LOG_INFO(tag, fmt, ...) \
    PLog::Instance().Print(CMP_LOG_INFO, tag, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(tag, fmt, ...) \
    PLog::Instance().Print(CMP_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)

/**
 * LOG_THROTTLE(intervalSec, tag, fmt, ...)
 * 동일한 메시지를 intervalSec 초에 1번만 출력. 나머지는 억제 후 카운트 표시.
 */
#define LOG_THROTTLE(intervalSec, tag, fmt, ...) \
    PLog::Instance().PrintThrottled(intervalSec, CMP_LOG_INFO, tag, fmt, ##__VA_ARGS__)

#endif // __CMP_LOG_H__
