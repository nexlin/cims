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
    bool        m_bWss;             // WSS(TLS WebSocket) 사용 여부
    std::string m_strCertFile;      // WSS용 TLS 인증서+키 PEM 파일 경로
    std::string m_strApiToken;      // WebSocket API 인증 토큰 (비어있으면 비활성화)
    std::string m_strUserAgent;     // SIP REGISTER에 사용할 User-Agent 문자열
    std::string m_strPAccessNetworkInfo; // P-Access-Network-Info 헤더 값 (비어있으면 생략)
    std::string m_strSipIp;         // CSP IP
    int         m_iSipPort;         // CSP SIP UDP 포트 (default 5060)
    std::string m_strSipDomain;     // SIP realm/domain (call, fallback)
    std::string m_strPttDomain;     // SIP domain for PTT users
    int         m_iSipLocalPort;    // cwrtc SIP 수신 포트 (default 5062)
    int         m_iRtpPortBase;     // RTP 포트 풀 시작 (default 50100)
    int         m_iRtpPortCount;    // 포트 쌍 수 (default 50)
    std::string m_strLogDir;        // 로그 디렉터리
    std::string m_strDocRoot;       // HTML 문서 루트
};

extern CCwrtcSetup gclsCwrtcSetup;
