#ifndef _SIM_SESSION_H_
#define _SIM_SESSION_H_

#include "SipClient.h"
#include "SipClientSetup.h"
#include "RtpThread.h"
#include "SipStack.h"
#include "SipMessage.h"
#include <atomic>
#include <string>
#include <mutex>

// Forward declaration
class SimSession;

// ─────────────────────────────────────────────
//  SessionSipClient: ISipUserAgentCallBack 구현
// ─────────────────────────────────────────────
class SessionSipClient : public CSipClient {
public:
    SessionSipClient(SimSession* owner) : m_pOwner(owner) {}
    virtual ~SessionSipClient() {}

    virtual void EventRegister( CSipServerInfo * pclsInfo, int iStatus );
    virtual void EventIncomingCall( const char * pszCallId, const char * pszFrom,
                                    const char * pszTo, CSipCallRtp * pclsRtp, CSipMessage * pclsMessage = NULL );
    virtual void EventCallStart( const char * pszCallId, CSipCallRtp * pclsRtp );
    virtual void EventCallEnd( const char * pszCallId, int iSipStatus );

    SimSession* m_pOwner;
};

// ─────────────────────────────────────────────
//  통계 구조체
// ─────────────────────────────────────────────
struct SimStats {
    std::atomic<int> iRegOk{0};       // 등록 성공
    std::atomic<int> iRegFail{0};     // 등록 실패
    std::atomic<int> iGmsOk{0};       // GMS SUBSCRIBE 성공
    std::atomic<int> iCmsOk{0};       // CMS SUBSCRIBE 성공
    std::atomic<int> iNotifyRecv{0};  // NOTIFY 수신 횟수
    std::atomic<int> iConfNotify{0};  // Conference NOTIFY 수신 횟수
    std::atomic<int> iXcapTokenOk{0}; // CSC-1 토큰 취득 성공 (Phase 3C)
    std::atomic<int> iXcapTokenFail{0};
    std::atomic<int> iXcapOk{0};      // XCAP GET 200 (Phase 3D)
    std::atomic<int> iXcap304{0};     // XCAP GET 304 (ETag 조건부)
    std::atomic<int> iXcapFail{0};    // XCAP GET 실패/4xx/5xx
    std::atomic<int> iCallOk{0};      // 통화 성공 (CallStart 이벤트)
    std::atomic<int> iCallFail{0};    // 통화 실패
    std::atomic<int> iCallEnd{0};     // 통화 종료
    std::atomic<long long> llTotalRegMs{0};   // 등록 응답시간 합계 (ms)
    std::atomic<long long> llTotalCallMs{0};  // 통화 설정시간 합계 (ms)

    // 타임스탬프 (각 세션에서 개별 관리)
    long long tRegStart{0};   // ms since epoch
    long long tCallStart{0};
};

// ─────────────────────────────────────────────
//  시나리오 열거
// ─────────────────────────────────────────────
enum ESimScenario {
    E_SCENARIO_NONE = 0,
    E_SCENARIO_REGISTER,      // REGISTER만
    E_SCENARIO_SUBSCRIBE,     // REGISTER + GMS/CMS SUBSCRIBE
    E_SCENARIO_CALL,          // REGISTER + peer간 통화
    E_SCENARIO_GROUP_CALL,    // REGISTER + SUBSCRIBE + 그룹통화
    E_SCENARIO_FULL,          // 위 전부 반복
};

// ─────────────────────────────────────────────
//  SimSession: 하나의 가상 단말기
// ─────────────────────────────────────────────
class SimSession : public ISipStackCallBack {
public:
    SimSession(int id,
               const std::string& strUser,
               const std::string& strAuthId,
               const std::string& strDomain,
               const std::string& strPwd,
               const std::string& strServerIp,
               int iServerPort,
               const std::string& strLocalIp,
               int iLocalPort,
               bool bPttMode,
               const std::string& strGroupId = "");
    ~SimSession();

    bool Start();
    void Stop();

    void SetNoRegister(bool b) { m_bNoRegister = b; }
    void SetNoXcap(bool b) { m_bNoXcap = b; }

    // 액션
    void StartCall(const std::string& strTarget = "");
    void StopCall();
    void StartGroupCall(const std::string& strGroupId = "");
    void SubscribeGms();
    void SubscribeCms();
    void AffiliateGroup(bool bDeaffiliate = false);   // MCPTT 그룹 affiliation (TS 24.379 §9) — 그룹 URI 로 PUBLISH
    void SendPttRequest();
    void SendPttRelease();

    // ISipStackCallBack (SUBSCRIBE/NOTIFY 처리)
    virtual bool RecvRequest(int iThreadId, CSipMessage* pclsMessage);
    virtual bool RecvResponse(int iThreadId, CSipMessage* pclsMessage);
    virtual bool SendTimeout(int iThreadId, CSipMessage* pclsMessage);

    // 설정
    int          m_iId;
    std::string  m_strUser;
    std::string  m_strAuthId;
    std::string  m_strDomain;
    std::string  m_strPwd;
    std::string  m_strServerIp;
    int          m_iServerPort;
    std::string  m_strLocalIp;
    int          m_iLocalPort;
    bool         m_bPttMode;
    std::string  m_strGroupId;

    // SIP
    CSipUserAgent       m_clsUserAgent;
    CSipStackSetup      m_clsSetup;
    CSipServerInfo      m_clsServerInfo;
    SessionSipClient*   m_pSipClient;
    CRtpThread          m_clsRtpThread;

    // 상태
    std::string  m_strInviteId;
    bool         m_bRegistered;
    bool         m_bInCall;
    bool         m_bNoRegister{false};  // true 면 REGISTER 자동 송신 skip (외부 SIP peer 모드)
    bool         m_bNoXcap{false};      // true 면 NOTIFY 수신 시 XCAP HTTP GET skip (Phase 3D)
    bool         m_bGmsSubscribed;
    bool         m_bCmsSubscribed;
    std::string  m_strAccessToken;      // CSC-1 bearer 토큰 캐시 (Phase 3C)

    // SUBSCRIBE 다이얼로그 정보
    std::string  m_strGmsCallId;
    std::string  m_strCmsCallId;
    int          m_iGmsSeq;
    int          m_iCmsSeq;
    std::string  m_strGmsFromTag;
    std::string  m_strCmsFromTag;

    // 통계
    SimStats m_stats;

    static long long NowMs();

private:
    // SUBSCRIBE 메시지 생성 후 전송
    void SendSubscribe(const std::string& strPsi,
                       std::string& strCallIdOut,
                       int& iSeqOut,
                       std::string& strFromTagOut);

    // 수신된 NOTIFY 처리
    void HandleNotify(CSipMessage* pclsMessage);

    // XCAP / IdMS (Phase 3) — CSC-1 토큰 취득 + 문서 GET
    bool AcquireXcapToken(const std::string& strHost, int iPort, bool bTls);
    void FetchXcapDoc(const std::string& strXcapRoot, const std::string& strSel,
                      const std::string& strDocEtag);

public:
    // 능동 XCAP 취득 (Phase 3D 검증용). 정식 흐름은 xcap-diff NOTIFY 수신→HandleNotify→GET 이나,
    // cspsim 의 out-of-dialog NOTIFY 미수신(psip UserAgent 가 선소비) 환경에서 SUBSCRIBE 후
    // user-profile/service-config/group 문서를 직접 GET 하여 token+TLS+200/304 경로를 검증.
    void ProbeXcap(const std::string& strXcapRoot);
};

#endif
