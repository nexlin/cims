#ifndef _SIP_MESSAGE_LOGGER_H_
#define _SIP_MESSAGE_LOGGER_H_

#include "Log.h"
#include <string>
#include <mutex>
#include <cstdio>

/**
 * @brief SIP/CMP message logger that captures LOG_NETWORK callbacks from psip
 *
 * Parses psip network log format:
 *   TX: "[threadid] UdpSend(IP:PORT) \n[SIP message text]"
 *   RX: "[threadid] UdpRecv(IP:PORT) \n[SIP message text]"
 *
 * Writes JSONL to {baseDir}/YYYY/MM/DD/HH/sip.jsonl with hourly rotation.
 */
class CSipMessageLogger : public ILogCallBack
{
public:
    CSipMessageLogger();
    ~CSipMessageLogger();

    /** Initialize with base directory for log output */
    void Init(const std::string& strBaseDir);

    bool IsEnabled() const { return m_bEnabled; }

    /** ILogCallBack::Print - receives formatted log string from CLog */
    void Print(EnumLogLevel eLevel, const char* fmt, ...) override;

    /** Log a CMP JSON protocol message */
    void LogCmp(const char* pszDir, const char* pszPeer,
                const char* pszMethod, const char* pszBody);

private:
    /** Write a single JSONL line to the hourly log file */
    void WriteJsonl(const char* pszTs, const char* pszDir, const char* pszPeer,
                    const char* pszCallId, const char* pszMethod,
                    const char* pszFromUri, const char* pszToUri,
                    const char* pszProto, const char* pszMsg);

    /** Ensure directory exists (recursive) */
    static bool MkdirP(const std::string& path);

    /** Get current hourly directory path */
    std::string GetHourlyDir();

    /** Get current timestamp string HH:MM:SS.uuuuuu */
    static std::string GetTimestamp();

    /** Escape a string for JSON output */
    static std::string JsonEsc(const char* s, int maxLen = -1);

    /** Extract a SIP header value from message text */
    static std::string ExtractHeader(const char* pszMsg, const char* pszHeader, const char* pszShort = NULL);

    /** Extract the method or status from the first line of a SIP message */
    static std::string ExtractMethodOrStatus(const char* pszMsg);

    /** Extract URI user part from a From/To header value */
    static std::string ExtractUriUser(const std::string& strHeaderValue);

    std::string m_strBaseDir;
    bool        m_bEnabled;
    std::mutex  m_mtx;

    // Current open file state (hourly rotation)
    std::string m_strCurrentHourDir;
    FILE*       m_pFile;
};

extern CSipMessageLogger gclsSipLogger;

#endif
