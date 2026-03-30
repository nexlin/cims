#pragma once
#include "SipMutex.h"
#include <string>
#include <map>
#include <vector>

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
    bool        bSipRegistered;   // CSP SIP 등록 완료 여부

    CWsClient() : iWsPort(0), bSipRegistered(false) {}
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
    bool        bAutoAnswered;  // Answer-Mode:Auto로 SIP 200 OK 이미 전송됨
    std::string strBrowserSdp;  // 브라우저의 SDP offer/answer (ICE 정보 추출용)
    int         iAudioPt;       // 오디오 페이로드 타입: 111=Opus, 99=AMR-WB
    bool        bVideoEnabled;  // H.264 비디오 릴레이 활성화
    int         iDtlsPort;      // 할당된 DTLS 포트 (FreePorts 호출용)
    int         iRtpPort;       // 할당된 RTP 포트 (FreePorts 호출용)
    CRtpThreadArg* pclsRtpArg;

    CCallSession() : iWsPort(0), iCmpPort(0), bPtt(false), bOutgoing(false),
                     bAutoAnswered(false), iAudioPt(99), bVideoEnabled(false),
                     iDtlsPort(0), iRtpPort(0), pclsRtpArg(nullptr) {}
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
    bool   SetClientSipRegistered(const std::string& userId, bool bRegistered);
    bool   SetCallAutoAnswered(const std::string& callId);

    // 통화 세션 관리
    bool   InsertCall(const CCallSession& sess);
    bool   GetCall(const std::string& callId, CCallSession& out);
    bool   UpdateCallCmp(const std::string& callId, const std::string& cmpIp, int cmpPort);
    bool   UpdateCallRtpArg(const std::string& callId, CRtpThreadArg* pArg);
    bool   UpdateCallBrowserSdp(const std::string& callId, const std::string& sdp);
    bool   DeleteCall(const std::string& callId);
    void   ClearUserActiveCall(const std::string& userId);
    /** PTT 그룹 ID(strPeerUserId)가 일치하는 활성 통화 세션 목록 반환 */
    void   GetPttSessionsByGroup(const std::string& groupId, std::vector<CCallSession>& out);

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
