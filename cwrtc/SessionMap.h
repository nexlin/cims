#pragma once
#include "SipMutex.h"
#include <string>
#include <map>

struct CRtpThreadArg;  // forward declaration

/**
 * WebSocket 연결 클라이언트 (브라우저 사용자)
 */
struct CWsClient {
    std::string strUserId;
    std::string strDomain;
    std::string strPassword;
    std::string strAuthId;
    std::string strWsIp;
    int         iWsPort;
    std::string strActiveCallId;  // 현재 통화 callId (없으면 "")
};

/**
 * 통화 세션 정보
 */
struct CCallSession {
    std::string strCallId;
    std::string strUserId;      // 브라우저 사용자 ID
    std::string strWsIp;
    int         iWsPort;
    std::string strPeerUserId;  // 상대방 ID (발신/착신)
    std::string strCmpIp;       // CMP RTP IP
    int         iCmpPort;       // CMP RTP 포트 (0이면 미확정)
    bool        bPtt;           // PTT 그룹 콜 여부
    std::string strGroupId;     // PTT 그룹 ID
    bool        bOutgoing;      // true: 브라우저→CSP, false: CSP→브라우저
    std::string strBrowserSdp;  // 브라우저의 SDP offer/answer (ICE 정보 추출용)
    CRtpThreadArg* pclsRtpArg;

    CCallSession() : iWsPort(0), iCmpPort(0), bPtt(false), bOutgoing(false), pclsRtpArg(nullptr) {}
};

/**
 * WS 클라이언트 레지스트리 + 통화 세션 레지스트리
 */
class CSessionMap {
public:
    // WS 클라이언트 관리
    bool   InsertClient(const std::string& userId, const std::string& wsIp, int wsPort,
                        const std::string& domain, const std::string& password, const std::string& authId);
    bool   DeleteClient(const std::string& wsIp, int wsPort);
    bool   GetClientByUser(const std::string& userId, CWsClient& out);
    bool   GetClientByWs(const std::string& wsIp, int wsPort, CWsClient& out);
    std::string GetUserIdByWs(const std::string& wsIp, int wsPort);

    // 통화 세션 관리
    bool   InsertCall(const CCallSession& sess);
    bool   GetCall(const std::string& callId, CCallSession& out);
    bool   UpdateCallCmp(const std::string& callId, const std::string& cmpIp, int cmpPort);
    bool   UpdateCallRtpArg(const std::string& callId, CRtpThreadArg* pArg);
    bool   UpdateCallBrowserSdp(const std::string& callId, const std::string& sdp);
    bool   DeleteCall(const std::string& callId);
    void   ClearUserActiveCall(const std::string& userId);

private:
    // key: userId
    std::map<std::string, CWsClient>  m_clsClientMap;
    // key: wsIp_port
    std::map<std::string, std::string> m_clsWsKeyMap;  // wsKey → userId
    // key: callId
    std::map<std::string, CCallSession> m_clsCallMap;
    CSipMutex m_clsMutex;

    static std::string WsKey(const std::string& ip, int port);
};

extern CSessionMap gclsSessionMap;
