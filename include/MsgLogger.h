#pragma once
/**
 * MsgLogger.h  —  컴포넌트 간 메시지를 공유 스토리지(NAS/ext_mnt)에 기록
 *
 * 디렉터리 구조:
 *   {ext_mnt}/msg_logs/{YYYY}/{MM}/{DD}/{prefix}/{caller}/{HHmmss}_{caller}_{callee}_{callid_short}.d/{component}.jsonl
 *
 * JSONL 포맷 (1줄 = 1메시지):
 *   {"ts":"HH:MM:SS.us","from":"cwrtc","to":"csp","proto":"SIP",
 *    "label":"REGISTER","body":"REGISTER sip:..."}
 */

#include <string>
#include <map>
#include <mutex>

class CMsgLogger {
public:
    CMsgLogger() = default;

    void Init(const std::string& extMntDir, const std::string& component);

    bool IsEnabled() const { return !m_strLogDir.empty(); }

    /**
     * @brief 통화의 caller/callee 정보를 등록한다 (최초 1회).
     *        이후 같은 callId로 Log() 호출 시 이 정보로 디렉터리를 생성한다.
     */
    void SetCallInfo(const char* callId, const char* caller, const char* callee);

    /**
     * 메시지 1건 기록
     */
    void Log(const char* callId,
             const char* from,
             const char* to,
             const char* proto,
             const char* label,
             const char* body);

private:
    std::string m_strLogDir;    // {ext_mnt}/msg_logs
    std::string m_strComponent; // "csp", "cwrtc", "cmp"
    std::mutex  m_mtx;

    // callId → 디렉터리 경로 캐시 (한 번 생성하면 재사용)
    std::map<std::string, std::string> m_mapCallDir;

    struct CallInfo {
        std::string strCaller;
        std::string strCallee;
    };
    std::map<std::string, CallInfo> m_mapCallInfo;

    std::string BuildCallDir(const std::string& callId);
    bool        EnsureDirRecursive(const std::string& path);
    static std::string SanitizeForPath(const std::string& s, int maxLen = 40);
    static std::string CallerPrefix(const std::string& caller);
    static std::string EscapeJson(const std::string& s);
    static std::string GetTimestamp();
    static void        GetDateParts(std::string& yyyy, std::string& mm, std::string& dd);
    static std::string GetTimeShort();  // HHmmss
};

extern CMsgLogger gclsMsgLogger;
