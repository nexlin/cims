// CsimPeer — libcsim 피어 엔진: 고정 수신점 하나에 가상 신원 범위를 얹은 외부 SIP 피어(IBCF·IP-PBX·MGCF) 시뮬레이터
//   (test_instrument.md §3.2). SimSession 이 "가입자 한 명 = 스택 하나" 인 것과 달리, 피어는 스택 하나가 여러 신원의
//   다수 동시 호를 다룬다 — 호마다 RTP 스레드를 따로 갖고 Call-ID 로 식별한다.
//
//   · 수신: INVITE 가 오면 관측자(워커)에게 알리고 응답은 관측자가 정한다(Ring/Progress/Answer/Reject) — 스택 스레드를
//           막지 않는다. silent 모드면 아무 응답도 내지 않는다(죽은 피어 모사 — route_set failover 시험).
//   · 발신: 자기 범위의 신원(From)으로 상대 도메인의 사용자에게 INVITE. Request-URI/To 는 `user@toDomain`
//           (psip 기본은 `user@접속IP` 라 CreateCall 뒤 메시지를 고쳐 보낸다). 프로파일 ibcf 는 P-Charging-Vector 를 싣는다
//           (TS 24.229 §7.2A.5 — P-Asserted-Identity 는 psip 이 From 도메인으로 낸다).
//   · 코덱: 프로파일 기본(ibcf/mgcf = AMR-WB,AMR,PCMU,PCMA · pbx = PCMA,PCMU) 또는 지정 목록. 착신 answer 는 오퍼와의
//           첫 공통 코덱, 없으면 488(IP-PBX G.711 ↔ AMR-WB 불일치 = cmp.md §11 트랜스코딩 과제가 드러나는 지점).
//   · 미디어 보안: NNI 는 평문 — SAVP 오퍼는 488.
//   · pbx·mgcf 동작(D 단계): 183 early media(+RFC 3262 100rel/PRACK — UAS 는 INVITE 가 100rel 을 지원하면 RSeq 를 싣고,
//           UAC 는 RSeq 있는 1xx 에 PRACK 을 낸다), hold/resume(re-INVITE a=sendonly/sendrecv — RFC 3264 §8.4),
//           blind REFER 발신(RFC 3515), RFC 4733 telephone-event DTMF(오퍼/echo → CRtpThread), RFC 3326 Reason
//           `Q.850;cause=` 송신·수신 관측, pbx 트렁크 REGISTER(계정 하나로 DID 범위 대표 — SIPconnect 2.0 §8, Digest H(A1)).
#ifndef _CSIM_PEER_H_
#define _CSIM_PEER_H_

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "RtpThread.h"
#include "SipCodecTable.h"
#include "SipStack.h"
#include "SipUserAgent.h"

class CsimPeer;

/** 피어 엔진 관측자 — 스택 스레드에서 불리므로 짧게(큐에 넣기) 끝낸다. */
struct ICsimPeerObserver {
    virtual ~ICsimPeerObserver() {}
    /** INVITE 수신 — 응답 전. bHasPai = P-Asserted-Identity 존재(ibcf 프로파일의 신원 검사). */
    virtual void OnPeerIncoming(CsimPeer* /*p*/, const std::string& /*callId*/, const std::string& /*fromUser*/,
                                const std::string& /*toUser*/, bool /*bHasPai*/) {}
    /** 발신 INVITE 의 1xx — bHasSdp = early media answer, bPrackSent = RSeq 가 있어 PRACK 을 냈다. */
    virtual void OnPeerRing(CsimPeer* /*p*/, const std::string& /*callId*/, int /*iSipStatus*/, bool /*bHasSdp*/, bool /*bPrackSent*/) {}
    /** 발신 INVITE 의 2xx — srdMs = StartCall → 200. */
    virtual void OnPeerCallStart(CsimPeer* /*p*/, const std::string& /*callId*/, long long /*srdMs*/) {}
    /** 다이얼로그 종료 — 발신 실패 최종 응답·상대 BYE·CANCEL·타이머. iQ850 = 상대 BYE/최종 응답의 Reason Q.850 cause(0 = 없음). */
    virtual void OnPeerCallEnd(CsimPeer* /*p*/, const std::string& /*callId*/, int /*iSipStatus*/, int /*iQ850*/) {}
    /** 로컬 BYE 의 최종 응답 — sddMs. */
    virtual void OnPeerByeResponse(CsimPeer* /*p*/, const std::string& /*callId*/, int /*iSipStatus*/, long long /*sddMs*/) {}
    /** 착신 측(UAS)이 자기 신뢰 1xx 에 대한 PRACK 을 받았다. */
    virtual void OnPeerPrack(CsimPeer* /*p*/, const std::string& /*callId*/) {}
    /** 상대가 re-INVITE 를 보냈다(psip 이 200 으로 answer) — bRemoteHold = 상대 SDP 방향이 sendonly/inactive. */
    virtual void OnPeerReInvite(CsimPeer* /*p*/, const std::string& /*callId*/, bool /*bRemoteHold*/) {}
    /** 자기 re-INVITE(hold/resume) 의 최종 응답. */
    virtual void OnPeerReInviteResponse(CsimPeer* /*p*/, const std::string& /*callId*/, int /*iSipStatus*/) {}
    /** 자기 REFER 의 최종 응답(202 정상). */
    virtual void OnPeerReferResponse(CsimPeer* /*p*/, const std::string& /*callId*/, int /*iSipStatus*/) {}
    /** 트렁크 REGISTER 최종 결과 — rrdMs = Register() → 최종 응답. */
    virtual void OnPeerRegister(CsimPeer* /*p*/, int /*iSipStatus*/, long long /*rrdMs*/) {}
};

/** pbx 트렁크 REGISTER 계정 — 대상의 access 접속점(Digest 챌린지가 있는 쪽)으로 등록한다. */
struct CsimPeerTrunkRegister {
    std::string user;            // 계정(사용자부) — DID 범위를 대표
    std::string realm;           // 대상 도메인(realm) — To/From host
    std::string ha1;             // H(A1) — 비면 password
    std::string password;
    std::string ip;              // 등록 목적지(대상 access 접속점)
    int port = 5060;
    ESipTransport transport = E_SIP_UDP;
    int expires = 3600;
};

struct CsimPeerConfig {
    std::string name;
    std::string profile = "ibcf";       // ibcf | pbx | mgcf
    std::string bindIp;
    int port = 5080;
    ESipTransport transport = E_SIP_UDP;
    std::string domain;                 // 피어 도메인 — From/To/PAI 의 host
    std::vector<std::string> codecs;    // rtpmap 이름(PCMU·PCMA·AMR-WB·AMR·G722) 우선순위 순. 비면 프로파일 기본
    std::string mediaFile;              // AMR-WB raw 프레임 파일 — 비면 합성 PCMU
    bool silent = false;                // 착신 INVITE 무응답(죽은 피어)
    std::string certFile;               // transport=TLS 서버 인증서(PEM, key 결합)
    std::string userAgent;              // User-Agent 헤더 — 비면 "cims-tester-peer/<profile>"
    bool prack = false;                 // RFC 3262 — 발신 INVITE 에 Supported/Require: 100rel, 1xx(RSeq) 에 PRACK. 착신은 INVITE 가 100rel 이면 RSeq
    bool dtmf = true;                   // RFC 4733 telephone-event 를 오퍼/echo
    int dtmfPt = 101;                   // 오퍼 시 telephone-event PT
    CsimPeerTrunkRegister trunk;        // user 비면 등록 없음(고정 IP 피어링)
    /** 프로파일 기본 100rel — ibcf/mgcf 는 IMS 코어·MGCF 가 precondition/early media 에 100rel 을 쓴다(TS 24.229 §5.1.3.1), pbx 는 선택 */
    static bool DefaultPrack(const std::string& profile) { return profile != "pbx"; }
};

class CsimPeer : public ISipUserAgentCallBack, public ISipStackCallBack {
public:
    explicit CsimPeer(const CsimPeerConfig& cfg);
    ~CsimPeer();

    bool Start(std::string& err);
    void Stop();
    void SetObserver(ICsimPeerObserver* p) { m_pObserver = p; }
    const CsimPeerConfig& Config() const { return m_cfg; }
    /** 프로파일 기본 코덱 목록 (codecs 가 비었을 때). */
    static std::vector<std::string> DefaultCodecs(const std::string& profile);

    /** 트렁크 REGISTER 시작(config.trunk) — 결과는 OnPeerRegister. 계정이 없으면 false. */
    bool Register();
    bool Registered() const { return m_bRegistered; }

    /** 발신 — fromUser(자기 범위) → toUser@toDomain, 다음 홉 = destIp:destPort(상대의 피어링 접속점).
     *  iMediaMode = CRtpThread::EMediaMode(AUTO/NONE/EXPLICIT). 반환 = Call-ID, 실패면 빈 문자열. */
    std::string StartCall(const std::string& fromUser, const std::string& toUser, const std::string& toDomain,
                          const std::string& destIp, int destPort, ESipTransport eTransport,
                          int iMediaMode = CRtpThread::E_MEDIA_AUTO);
    bool Ring(const std::string& callId, int iCode = 180);
    /** 착신 183 Session Progress + SDP answer(early media — 링백/안내음 RTP 송신 시작). INVITE 가 100rel 을 지원하고 config.prack 이면
     *  RSeq 를 싣는다(상대 PRACK → OnPeerPrack). 반환 0=성공, 488=공통 코덱 없음/SAVP, 481=대기 착신 없음. */
    int Progress(const std::string& callId);
    /** 착신 200 OK — 반환 0=성공, 488=공통 코덱 없음/SAVP, 481=대기 착신 없음. RTP 송수신을 시작한다(183 뒤면 이미 흐르는 것을 유지). */
    int Answer(const std::string& callId);
    bool Reject(const std::string& callId, int iCode, int iQ850 = 0);
    /** 통화 종료(BYE) 또는 미확립 발신 취소(CANCEL). iQ850 > 0 이면 Reason: Q.850;cause= 를 싣는다. SDD 기점을 찍는다. */
    bool Bye(const std::string& callId, int iQ850 = 0);
    /** hold(re-INVITE a=sendonly) / resume(a=sendrecv) — 결과는 OnPeerReInviteResponse. */
    bool Hold(const std::string& callId);
    bool Resume(const std::string& callId);
    /** blind REFER — 통화 상대를 toUser 로 전달(Refer-To = toUser@상대 Contact). 결과는 OnPeerReferResponse. */
    bool Refer(const std::string& callId, const std::string& toUser);
    /** RFC 4733 DTMF 숫자열 송신 — telephone-event 가 협상되지 않았으면 false. */
    bool SendDtmf(const std::string& callId, const std::string& digits);
    /** 미디어 평면(test_instrument.md §4) — 착신 호의 RTP 모드는 Progress/Answer 전에 정한다(발신은 StartCall 인자). */
    bool SetMediaMode(const std::string& callId, int iMediaMode);
    /** 송출 시작 — bDefault 면 풀 기본 원천, 아니면 코덱별 샘플 파일(빈 값 = 합성). SDP 교환 전(RTP 미기동)이면 false. */
    bool MediaSend(const std::string& callId, bool bDefault, const std::string& amrWbFile, const std::string& pcmuFile,
                   const std::string& pcmaFile, bool bLoop);
    bool MediaStop(const std::string& callId);
    unsigned long long RtpSent(const std::string& callId);
    bool HasCall(const std::string& callId);
    bool Connected(const std::string& callId);
    /** 수신 RTP 품질(누계) — 호가 없으면 false. */
    bool RtpStats(const std::string& callId, unsigned long long& rx, unsigned long long& lost, long long& jitterUs);
    /** 수신 품질 부가 — wire PT(MOS 코덱 판정)·RTCP SR/RR 수신 수·마지막 보고 블록 fraction lost(0~255, -1 없음). */
    bool RtpQuality(const std::string& callId, int& pt, int& rtcpRx, int& rrFractionLost);
    /** DTMF 송수신 누계 — 호가 없으면 false. */
    bool DtmfStats(const std::string& callId, int& sent, int& recv, std::string& recvDigits, int& negotiatedPt);
    void ResetRtpStats(const std::string& callId);
    size_t CallCount();
    std::string LocalIp() const { return m_clsSetup.m_strLocalIp; }

    // ISipUserAgentCallBack (스택 스레드)
    void EventRegister(CSipServerInfo* pclsInfo, int iStatus) override;
    void EventIncomingCall(const char* pszCallId, const char* pszFrom, const char* pszTo, CSipCallRtp* pclsRtp,
                           CSipMessage* pclsMessage) override;
    void EventCallRing(const char* pszCallId, int iSipStatus, CSipCallRtp* pclsRtp) override;
    void EventCallStart(const char* pszCallId, CSipCallRtp* pclsRtp) override;
    void EventCallEnd(const char* pszCallId, int iSipStatus) override;
    void EventReInvite(const char* pszCallId, CSipCallRtp* pclsRemoteRtp, CSipCallRtp* pclsLocalRtp) override;
    void EventReInviteResponse(const char* pszCallId, int iSipStatus, CSipCallRtp* pclsRemoteRtp) override;
    void EventPrack(const char* pszCallId, CSipCallRtp* pclsRtp) override;
    void EventTransferResponse(const char* pszCallId, int iSipStatus) override;
    // ISipStackCallBack — BYE 최종 응답·Reason 헤더 관측(UA 보다 먼저 등록, 처리는 UA 에 위임)
    bool RecvRequest(int iThreadId, CSipMessage* pclsMessage) override;
    bool RecvResponse(int iThreadId, CSipMessage* pclsMessage) override;
    bool SendTimeout(int, CSipMessage*) override { return false; }

private:
    struct Call {
        std::string callId, fromUser, toUser;
        bool outbound = false;
        bool connected = false;
        bool early = false;             // 183 + SDP 를 냈다(UAS) / 받았다(UAC) — RTP 가 이미 흐른다
        bool has100rel = false;         // 착신 INVITE 가 100rel 을 지원(Supported/Require)
        bool remoteHold = false;        // 상대 re-INVITE 방향이 sendonly/inactive
        int q850 = 0;                   // 상대가 실어 온 Reason Q.850 cause
        CRtpThread* rtp = nullptr;
        CSipCallRtp offer;              // 착신 오퍼(answer 용)
        bool hasOffer = false;
        CSipCallRtp answer;             // 착신 answer(183 과 200 이 같은 answer 를 낸다)
        bool hasAnswer = false;
        long long tStart = 0;           // INVITE 송신 시각(SRD 기점)
        long long tBye = 0;             // BYE 송신 시각(SDD 기점) — 0 이면 미송신
    };

    CsimPeerConfig m_cfg;
    CSipUserAgent m_clsUserAgent;
    CSipStackSetup m_clsSetup;
    CSipServerInfo m_clsServerInfo;
    ICsimPeerObserver* m_pObserver = nullptr;
    std::mutex m_mtx;
    std::map<std::string, Call> m_calls;
    bool m_bStarted = false;
    bool m_bRegistered = false;
    long long m_tRegStart = 0;
    std::vector<const CSipCodecEntry*> m_codecs;   // 해석된 코덱 엔트리(우선순위 순)

    CRtpThread* newRtp();
    void freeRtp(CRtpThread* rtp);
    /** 오퍼 SDP(audio, 자기 코덱 전부 + telephone-event) */
    void buildOffer(CSipCallRtp& clsRtp, CRtpThread* rtp);
    /** 착신 answer 구성(첫 공통 코덱 + telephone-event echo) — 없으면 false. 호 상태의 answer 를 채우고 rtp PT 를 맞춘다. */
    bool buildAnswer(Call& c);
    /** 오퍼에서 자기 코덱 목록과의 첫 공통 엔트리 — 없으면 NULL */
    const CSipCodecEntry* pickAnswerCodec(const CSipCallRtp& offer) const;
    /** 상대 SDP 의 telephone-event PT/클록을 rtp 에 반영(없으면 -1) */
    static void applyRemoteDtmf(CRtpThread* rtp, const CSipCallRtp& remote);
    static int parseQ850(CSipMessage* pclsMessage);
    static long long nowMs();
    static std::string genIcid();
};

#endif
