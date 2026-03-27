#pragma once
/**
 * MsgLogger.h  —  컴포넌트 간 메시지를 공유 스토리지(NAS/ext_mnt)에 기록
 *
 * 파일 구조:
 *   {logDir}/msg_logs/{YYYYMMDD}/{sanitized_call_id}/{component}.jsonl
 *
 * JSONL 포맷 (1줄 = 1메시지):
 *   {"ts":"HH:MM:SS.us","from":"cwrtc","to":"csp","proto":"SIP",
 *    "label":"REGISTER","body":"REGISTER sip:..."}
 */

#include <string>
#include <mutex>

class CMsgLogger {
public:
    CMsgLogger() = default;

    /**
     * @param extMntDir  ext_mnt 루트 디렉터리 (빈 문자열이면 비활성화)
     * @param component  이 인스턴스의 컴포넌트 이름 ("csp", "cwrtc", "cmp")
     */
    void Init(const std::string& extMntDir, const std::string& component);

    bool IsEnabled() const { return !m_strLogDir.empty(); }

    /**
     * 메시지 1건 기록
     * @param callId   SIP Call-ID (디렉터리 키, NULL/빈 값이면 기록 안 함)
     * @param from     송신 컴포넌트 ("csp","cwrtc","cmp","ue",...)
     * @param to       수신 컴포넌트
     * @param proto    프로토콜 ("SIP","JSON","WS")
     * @param label    요약 ("REGISTER","200 OK","addSession",...)
     * @param body     원문 메시지 전체
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

    bool        EnsureDir(const std::string& path);
    static std::string SanitizeCallId(const std::string& callId);
    static std::string EscapeJson(const std::string& s);
    static std::string GetTimestamp();
    static std::string GetDateStr();
};

extern CMsgLogger gclsMsgLogger;
