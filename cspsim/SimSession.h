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
    /** REFER 최종 응답 (psip RecvReferResponse) — 전달 게이트(transfer_allowed=false → 403) 판정용. */
    virtual void EventTransferResponse( const char * pszCallId, int iSipStatus );

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
    E_SCENARIO_TRANSFER,          // A→B 통화 후 A 가 blind REFER(→C) — 전달 후 B–C (volte_supplementary_services.md §6.1)
    E_SCENARIO_TRANSFER_ATTENDED, // A→B + A→C(상담) 후 A 가 attended REFER — 전달 후 B–C (§6.2)
    E_SCENARIO_PICKUP,            // A→B 링잉 중 C 가 당겨받기 코드 다이얼 — A–C (§5)
    E_SCENARIO_DIALOG_PICKUP,     // C 가 B 를 dialog 구독(BLF) → A→B 링잉 NOTIFY → C 가 INVITE-Replaces — A–C (§6.2)
    E_SCENARIO_SUBSCRIBE_EVENT,   // 등록 후 -event 토큰으로 SUBSCRIBE 1건 — 최종 응답 프로브 (RFC 6665 §8.2.1 489 등)
    E_SCENARIO_HUNT,              // [volte] A→대표번호(-pilot): B·C 병렬 링, C 응답 → A–C (dispatch_center.md §4, -count 3~4)
    E_SCENARIO_MONITOR,          // [volte] A↔B 통화 중 M(감청자)이 dialog 구독→INVITE-Join → 청취(SSRC 2개), A/B 무영향 (§5)
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

    /** IMS AKA 소프트-USIM (sip_access_security.md §8.2). k/opc hex32, sqnMs =
     * 단말 초기 SQN_MS (0 이면 첫 챌린지부터 수용, 큰 값이면 SQN 이탈 → AUTS
     * 재동기 경로 재현). */
    void SetAka(const std::string &k, const std::string &opc, uint64_t sqnMs) {
      m_clsServerInfo.m_strAkaK = k;
      m_clsServerInfo.m_strAkaOpc = opc;
      m_clsServerInfo.m_iAkaSqnMs = sqnMs;
    }

    /** IMS AKA + IPsec 단말 (sip_access_security.md §8.3). sec-agree 를 켜고 Security-Client 에
     * ipsec-3gpp(단말 spi/port) 만 제안한다. -aka_k/-aka_opc 필수, transport udp. 세션당 스택이
     * 하나이므로 보호 포트쌍은 로컬 포트+1/+2 (재인증마다 +2). */
    void SetIpsec(bool b, const std::string &alg, const std::string &ealg) {
      m_clsServerInfo.m_clsIpsec.m_bEnabled = b;
      if (!alg.empty()) m_clsServerInfo.m_clsIpsec.m_strAlg = alg;
      if (!ealg.empty()) m_clsServerInfo.m_clsIpsec.m_strEalg = ealg;
      if (b) m_clsServerInfo.m_bSecAgree = true;
    }

    /** 미디어 SRTP 모드 (SDES — media_security.md §8): 0=off, 1=optional(AVP+a=crypto
     *  best-effort 오퍼), 2=required(RTP/SAVP 오퍼 — 상대 crypto 부재 시 협상 실패).
     *  수신 오퍼는 모드>0 이면 내용대로 수락. sec-agree 활성 시 REGISTER Security-Client 에
     *  sdes-srtp;mediasec 능력을 선언한다(§4.1) — SetSecAgree 이후에 호출할 것. */
    void SetSrtpMode(int i) {
      m_iSrtpMode = i;
      if (i > 0 && m_clsServerInfo.m_bSecAgree && m_clsServerInfo.m_strSecurityClient.empty())
        m_clsServerInfo.m_strSecurityClient = "tls, sdes-srtp;mediasec";
    }
    int  m_iSrtpMode = 0;
    /** 이 세션이 마지막 SDP 에 선언한 자기 송신 키 (inline base64) — answer 수신 시 세션 확정용.
     *  a=crypto 는 m-line 단위(RFC 4568 §5)라 audio/video 키를 따로 든다. */
    std::string m_strSrtpLocalKey;
    std::string m_strSrtpVideoLocalKey;

    void SetNoRegister(bool b) { m_bNoRegister = b; }
    void SetNoXcap(bool b) { m_bNoXcap = b; }
    void SetCscHost(const std::string& h, int p, bool tls) { m_strCscHost = h; m_iCscPort = p; m_bCscTls = tls; }

    // 액션
    void StartCall(const std::string& strTarget = "");
    void StopCall();

    // ── 호 전달·당겨받기 (volte_supplementary_services.md §5·§6) ──
    /** blind REFER 발신 — 현재 통화(m_strInviteId)의 상대를 strTarget 으로 전달한다.
     *  서버(B2BUA)가 REFER 를 종단하고 상대 leg 를 strTarget 에 연결한다. */
    void BlindTransfer(const std::string& strTarget);
    /** 상담(consultation) 통화 발신 — 두 번째 다이얼로그(m_strConsultId)로 strTarget 을 부른다.
     *  첫 통화(m_strInviteId)는 유지. attended transfer 전제. */
    void StartConsultCall(const std::string& strTarget);
    /** attended REFER 발신 — 첫 통화(m_strInviteId)와 상담 통화(m_strConsultId)를 Replaces 로
     *  묶어 두 상대를 연결하고 자신은 빠진다. StartConsultCall 이 확립된 뒤 호출. */
    void AttendedTransfer();
    std::atomic<int>  m_iReferStatus{0};          // 마지막 REFER 최종 응답 (202 정상 / 403 transfer_allowed=false)
    /** 당겨받기 대상(ringing-hold) 모드 — INVITE 수신 시 180 만 보내고 200 을 보내지 않는다.
     *  다른 단말이 당겨받기 코드로 이 링잉 호를 가져갈 수 있게 한다. Start() 전/후 무관. */
    void SetRingHold(bool b) { m_bRingHold = b; }

    /** dialog-event(RFC 4235) 구독 — 감시 대상 AoR 의 호 상태 변화를 dialog-info NOTIFY 로 받는다
     *  (관제 BLF). NOTIFY 수신 시 링잉 leg Call-ID/태그를 학습해 INVITE-Replaces 당겨받기에 쓴다. */
    void SubscribeDialog(const std::string& strWatchedAor);
    /** INVITE-with-Replaces(RFC 3891) 발신 — replacesCallId(+태그) 대상 다이얼로그를 교체한다
     *  (BLF 클릭 당겨받기·표준 attended 완결). target 은 Request-URI user(임의 — 서버는 Replaces 로 라우팅). */
    void StartCallWithReplaces(const std::string& strTarget, const std::string& strReplacesCallId,
                               const std::string& strToTag, const std::string& strFromTag);
    /** INVITE-with-Join(RFC 3911, 업무망 합법감청 합류 — dispatch_center.md §5.3) 발신 —
     *  joinCallId(+태그) 대상 세션에 청취 leg 로 합류한다. SDP 는 recvonly. 서버가 CMP tap 을 붙이고
     *  200(sendonly, a=ssrc 라벨) 응답하면 미디어(SSRC 2개)를 수신한다. bWithMedia=false 면 미디어 소켓만 대기. */
    void StartCallWithJoin(const std::string& strTarget, const std::string& strJoinCallId,
                           const std::string& strToTag, const std::string& strFromTag);
    std::atomic<int>  m_iDialogNotifyCount{0};   // 수신 dialog NOTIFY 수
    std::atomic<int>  m_iDlgSubStatus{0};        // dialog SUBSCRIBE 최종 응답 (200 / 403 그룹 밖 감시 / 489)
    /** 임의 이벤트 패키지 SUBSCRIBE 프로브 — strEvent 를 Event 헤더에 그대로 싣고 최종 응답을
     *  m_iEventSubStatus 에 기록한다 (RFC 6665 §8.2.1: 미지원 패키지 → 489 Bad Event 판정용). */
    void SubscribeEvent(const std::string& strEvent, const std::string& strResourceAor);
    std::atomic<int>  m_iEventSubStatus{0};
    std::string       m_strWatchedDlgCallId;     // 마지막 dialog NOTIFY 의 활성 dialog Call-ID (Replaces 대상)
    std::string       m_strWatchedDlgState;      // early|confirmed|terminated
    std::string       m_strWatchedDlgLocalTag;   // dialog-info local-tag
    std::string       m_strWatchedDlgRemoteTag;  // dialog-info remote-tag
    /** 누적 수신 RTP 패킷 수 — 전달·픽업 후 미디어 흐름 검증용. */
    unsigned long long RecvPackets() const { return m_clsRtpThread.m_ullRecvTotal.load(); }
    /** 수신 audio RTP 의 서로 다른 SSRC 수 — 감청 leg 는 caller/callee 2개를 받는다(S3-SCN-MONITOR). */
    size_t RecvSsrcCount() { return m_clsRtpThread.RecvSsrcCount(); }
    std::atomic<int>  m_iJoinStatus{0};   // Join INVITE 최종 응답 (200 성공 / 403·481·488·486 거절)

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
    // IdMS 로그인 자격(XCAP 토큰용, users.login_id/passwd) — SIP 자격과 별개(sip_access_security.md §4.7).
    //   비어 있으면 구식 tel:+msisdn / m_strPwd 로 authreq 를 시도한다(-creds 없는 옛 전개 호환).
    std::string  m_strIdmsLogin;
    std::string  m_strIdmsLoginPw;
    void SetIdmsLogin(const std::string& strLogin, const std::string& strPw) {
        m_strIdmsLogin = strLogin; m_strIdmsLoginPw = strPw;
    }
    std::string  m_strServerIp;
    int          m_iServerPort;
    // IPsec 등록(-ipsec) 뒤 요청 목적지 = 서버 보호 포트 port_ps (EventRegister 200 에서 UA 의 SA 셋에서 얻음).
    //   0 이면 m_iServerPort. TS 33.203 §7.1 — 단말 요청은 port_uc → port_ps (SA 1).
    int          m_iRoutePort = 0;
    int          RoutePort() const { return m_iRoutePort > 0 ? m_iRoutePort : m_iServerPort; }
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
    std::string  m_strConsultId;        // attended transfer 상담 통화 call-id (두 번째 다이얼로그)
    bool         m_bRingHold{false};    // 당겨받기 대상 — 180 만 보내고 200 보류
    bool         m_bRegistered;
    bool         m_bInCall;
    std::atomic<int> m_iLastCallEndStatus{0};   // 마지막 EventCallEnd 의 SIP 상태 — 발신 실패(403/404/488) 판정용
    std::atomic<int> m_iIncomingInvites{0};     // 수신 INVITE 누계 — 대표번호 포크 도달 판정 (hunt 시나리오)
    std::string  m_strLastPCalledParty;         // 마지막 수신 INVITE 의 P-Called-Party-ID (대표번호 착신 표시)
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

    // dialog-event 구독 다이얼로그 (RFC 4235 — 관제 BLF)
    std::string  m_strDlgSubCallId;
    int          m_iDlgSubSeq{0};
    std::string  m_strDlgSubFromTag;
    std::string  m_strDlgWatchedAor;   // 감시 대상 AoR

    // 이벤트 패키지 프로브 다이얼로그 (SubscribeEvent)
    std::string  m_strEventSubCallId;
    int          m_iEventSubSeq{0};
    std::string  m_strEventSubFromTag;

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
    // 자원(AoR) 대상 out-of-dialog SUBSCRIBE — Req-URI/To = 자원, Event/Accept 지정, 본문 없음.
    //   dialog(RFC 4235)·이벤트 프로브 공용. strAccept 가 비면 Accept 를 싣지 않는다.
    void SendEventSubscribe(const std::string& strEvent, const std::string& strAccept,
                            const std::string& strResourceAor,
                            std::string& strCallIdOut, int& iSeqOut, std::string& strFromTagOut);
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
