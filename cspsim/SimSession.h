#ifndef _SIM_SESSION_H_
#define _SIM_SESSION_H_

#include "SipClient.h"
#include "SipClientSetup.h"
#include "RtpThread.h"
#include "SipStack.h"
#include "SipMessage.h"
#include <atomic>
#include <map>
#include <string>
#include <vector>
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
    std::atomic<int> iAffiliateOk{0};  // affiliation PUBLISH 200 OK
    std::atomic<int> iAffiliateRej{0}; // affiliation PUBLISH 4xx (CSP 멤버십 게이트 403 등)
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
               const std::string& strHa1,
               const std::string& strServerIp,
               int iServerPort,
               const std::string& strLocalIp,
               int iLocalPort,
               bool bPttMode,
               const std::string& strGroupId = "");
    ~SimSession();

    bool Start();
    void Stop();

    /** 시그널링 transport 선택 (기본 UDP). TLS 면 스택을 TLS 클라이언트로 기동한다 —
     *  등록·발신 목적지가 모두 이 transport 로 나가고, 서버 발신(fan-out INVITE·NOTIFY·
     *  세션 갱신)은 그 연결로 되돌아온다. Start() 전에 호출할 것. */
    void SetTransport(ESipTransport e) { m_eTransport = e; }
    /** RFC 3329 sec-agree — REGISTER 에 Security-Client/Require 를 싣고 401 의
     * Security-Server 를 Security-Verify 로 echo 한다. verifyOverride 가 있으면
     * echo 대신 그 값(강등 변조 시험). */
    void SetSecAgree(bool b, const std::string &verifyOverride) {
      m_clsServerInfo.m_bSecAgree = b;
      m_clsServerInfo.m_strSecurityVerifyOverride = verifyOverride;
    }

    void SetNoRegister(bool b) { m_bNoRegister = b; }
    void SetNoXcap(bool b) { m_bNoXcap = b; }
    void SetCscHost(const std::string& h, int p, bool tls) { m_strCscHost = h; m_iCscPort = p; m_bCscTls = tls; }

    // 액션
    void StartCall(const std::string& strTarget = "");
    void StopCall();
    void StartGroupCall(const std::string& strGroupId = "");
    void SetEmergency(int iCond) { m_iEmergencyCond = iCond; }  // 0/1/2 (normal/imminent/emergency)
    void SetAdhocMembers(const std::vector<std::string>& v) { m_vecAdhoc = v; }  // ad hoc 멤버 MSISDN
    void SubscribeGms();
    void SubscribeCms();
    void SubscribeReg();   // Event: reg — 자신의 등록 상태 구독 (RFC 3680, 실제 UE 플로우)
    /** Event: conference — 그룹 참가자 정보 구독 (RFC 4575). 그룹 AoR 로
     * SUBSCRIBE → 200 OK + 즉시 스냅샷 NOTIFY, 이후 멤버 변동마다 구독 경로로
     * NOTIFY. */
    void SubscribeConference(const std::string &strGroupId);
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
    std::string  m_strHa1;   // H(A1) — 비어 있지 않으면 m_strPwd 대신 Digest 응답 계산에 사용
    std::string  m_strServerIp;
    int          m_iServerPort;
    std::string  m_strLocalIp;
    int          m_iLocalPort;
    bool         m_bPttMode;
    std::string  m_strGroupId;
    /** 시그널링 transport — 등록 목적지(CSipServerInfo)와 스택 기동 모드에 함께 반영된다. */
    ESipTransport m_eTransport = E_SIP_UDP;
    // MCPTT condition (TS 24.379): 0=normal/1=imminent/2=emergency. >0 이면 그룹 INVITE 의
    // mcptt-info 에 emergency-ind/imminentperil-ind 를 실어 긴급 개시(키업)를 시뮬레이트.
    int          m_iEmergencyCond = 0;
    // ad hoc 그룹콜 (Rel-18): 비면 일반. 비어있지 않으면 그룹 INVITE 에 resource-lists(멤버) 주입.
    std::vector<std::string> m_vecAdhoc;

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
    std::string  m_strCscHost;          // CSC IP — REGISTER 전 IdMS auth 대상 (빈 문자열이면 skip)
    int          m_iCscPort{0};         // CSC McpttServer 포트
    bool         m_bCscTls{false};      // CSC TLS 여부
    bool         m_bGmsSubscribed;
    bool         m_bCmsSubscribed;
    std::string  m_strAccessToken;      // CSC-1 bearer 토큰 캐시 (Phase 3C)
    std::map<std::string, std::string> m_mapXcapEtag;  // URL → ETag 캐시 (앱 종료 시 초기화)

    // SUBSCRIBE 다이얼로그 정보
    std::string  m_strGmsCallId;
    std::string  m_strCmsCallId;
    int          m_iGmsSeq;
    int          m_iCmsSeq;
    std::string  m_strGmsFromTag;
    std::string  m_strCmsFromTag;

    // reg-event 구독 다이얼로그 (RFC 3680 — 실제 UE 는 REGISTER 직후 자신의 등록 상태 구독)
    bool         m_bRegSubscribed{false};
    std::string  m_strRegSubCallId;
    int          m_iRegSubSeq{0};
    std::string  m_strRegSubFromTag;

    // conference 구독 다이얼로그 (RFC 4575 — 그룹 참가자 정보. 그룹 AoR 로
    // SUBSCRIBE)
    std::string m_strConfSubGroup;
    std::string m_strConfSubCallId;
    int m_iConfSubSeq{0};
    std::string m_strConfSubFromTag;

    // 통계
    SimStats m_stats;

    static long long NowMs();

private:
    // SUBSCRIBE 메시지 생성 후 전송
    void SendSubscribe(const std::string& strPsi,
                       std::string& strCallIdOut,
                       int& iSeqOut,
                       std::string& strFromTagOut);

    // SUBSCRIBE Expires=0 — 기존 다이얼로그(Call-ID/From-tag) 재사용
    void SendUnsubscribe(const std::string& strPsi,
                         const std::string& strCallId,
                         int& iSeq,
                         const std::string& strFromTag);

    // 표준 로그아웃 플로우: de-affiliate → SUBSCRIBE Expires=0 × 2
    // (REGISTER Expires=0 는 m_clsUserAgent.Stop() 에서 자동 처리)
    public: void Logout(); private:

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
