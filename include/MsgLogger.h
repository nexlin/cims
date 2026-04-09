#pragma once
/**
 * MsgLogger.h — 인터페이스별 메시지 통계 로그 (body 미포함, 카운트용)
 *
 * 디렉터리: {MsgLogDir}/{component}/YYYY/MM/DD/HH/{interface}.jsonl
 *
 * JSONL: {"ts":"HH:MM:SS","dir":"in|out","proto":"SIP","method":"INVITE","peer":"+821357007002"}
 */

#include <string>
#include <mutex>

class CMsgLogger {
public:
    CMsgLogger() = default;

    void Init(const std::string& msgLogDir, const std::string& component);
    bool IsEnabled() const { return !m_strBaseDir.empty(); }

    /**
     * 메시지 1건 기록 (통계용, body 미포함)
     * @param iface     인터페이스명 ("sip", "csc", "cmp", "mcptt", "console")
     * @param dir       방향 ("in" 또는 "out")
     * @param proto     프로토콜 ("SIP", "JSON", "HTTPS")
     * @param method    메서드/상태 ("INVITE", "200", "add", "GET /api/...")
     * @param peer      상대방 식별 (번호, IP 등, 생략 가능)
     */
    void Log(const char* iface, const char* dir, const char* proto,
             const char* method, const char* peer = "");

    // 하위 호환: 기존 API (callId 무시, iface="sip" 자동)
    void SetCallInfo(const char*, const char*, const char*) {}
    void Log(const char* callId, const char* from, const char* to,
             const char* proto, const char* label, const char* body);

private:
    std::string m_strBaseDir;   // {MsgLogDir}/{component}
    std::mutex  m_mtx;

    static bool EnsureDirRecursive(const std::string& path);
    static std::string GetTimestamp();
    static void GetDateHourParts(std::string& yyyy, std::string& mm, std::string& dd, std::string& hh);
};

extern CMsgLogger gclsMsgLogger;
