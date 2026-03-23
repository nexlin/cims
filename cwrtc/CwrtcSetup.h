#pragma once
#include <string>

/**
 * cwrtc 설정 - JSON 파일에서 로드
 */
class CCwrtcSetup {
public:
    CCwrtcSetup();
    bool Load(const char* pszConfigFile);

    std::string m_strLocalIp;       // 서버 IP
    int         m_iWsPort;          // WebSocket 포트 (default 3000)
    std::string m_strSipIp;         // CSP IP
    int         m_iSipPort;         // CSP SIP UDP 포트 (default 5060)
    std::string m_strSipDomain;     // SIP realm/domain
    int         m_iSipLocalPort;    // cwrtc SIP 수신 포트 (default 5062)
    int         m_iRtpPortBase;     // RTP 포트 풀 시작 (default 50100)
    int         m_iRtpPortCount;    // 포트 쌍 수 (default 50)
    std::string m_strLogDir;        // 로그 디렉터리
    std::string m_strDocRoot;       // HTML 문서 루트
};

extern CCwrtcSetup gclsCwrtcSetup;
