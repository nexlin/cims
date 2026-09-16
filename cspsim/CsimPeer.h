// CsimPeer — libcsim 피어 엔진: 고정 수신점 하나에 가상 신원 범위를 얹은 외부 SIP 피어(IBCF·IP-PBX·MGCF) 시뮬레이터
//   (test_instrument.md §3.2). SimSession 이 "가입자 한 명 = 스택 하나" 인 것과 달리, 피어는 스택 하나가 여러 신원의
//   다수 동시 호를 다룬다 — 호마다 RTP 스레드를 따로 갖고 Call-ID 로 식별한다.
//
//   · 수신: INVITE 가 오면 관측자(워커)에게 알리고 응답은 관측자가 정한다(Ring/Answer/Reject) — 스택 스레드를 막지 않는다.
//           silent 모드면 아무 응답도 내지 않는다(죽은 피어 모사 — route_set failover 시험).
//   · 발신: 자기 범위의 신원(From)으로 상대 도메인의 사용자에게 INVITE. Request-URI/To 는 `user@toDomain`
//           (psip 기본은 `user@접속IP` 라 CreateCall 뒤 메시지를 고쳐 보낸다). 프로파일 ibcf 는 P-Charging-Vector 를 싣는다
//           (TS 24.229 §7.2A.5 — P-Asserted-Identity 는 psip 이 From 도메인으로 낸다).
//   · 코덱: 프로파일 기본(ibcf/mgcf = AMR-WB,AMR,PCMU,PCMA · pbx = PCMA,PCMU) 또는 지정 목록. 착신 answer 는 오퍼와의
//           첫 공통 코덱, 없으면 488(IP-PBX G.711 ↔ AMR-WB 불일치 = cmp.md §11 트랜스코딩 과제가 드러나는 지점).
//   · 미디어 보안: NNI 는 평문 — SAVP 오퍼는 488.
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
    /** 발신 INVITE 의 2xx — srdMs = StartCall → 200. */
    virtual void OnPeerCallStart(CsimPeer* /*p*/, const std::string& /*callId*/, long long /*srdMs*/) {}
    /** 다이얼로그 종료 — 발신 실패 최종 응답·상대 BYE·CANCEL·타이머. */
    virtual void OnPeerCallEnd(CsimPeer* /*p*/, const std::string& /*callId*/, int /*iSipStatus*/) {}
    /** 로컬 BYE 의 최종 응답 — sddMs. */
    virtual void OnPeerByeResponse(CsimPeer* /*p*/, const std::string& /*callId*/, int /*iSipStatus*/, long long /*sddMs*/) {}
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

    /** 발신 — fromUser(자기 범위) → toUser@toDomain, 다음 홉 = destIp:destPort(상대의 피어링 접속점).
     *  반환 = Call-ID, 실패면 빈 문자열. */
    std::string StartCall(const std::string& fromUser, const std::string& toUser, const std::string& toDomain,
                          const std::string& destIp, int destPort, ESipTransport eTransport);
    bool Ring(const std::string& callId, int iCode = 180);
    /** 착신 200 OK — 반환 0=성공, 488=공통 코덱 없음/SAVP, 481=대기 착신 없음. RTP 송수신을 시작한다. */
    int Answer(const std::string& callId);
    bool Reject(const std::string& callId, int iCode);
    /** 통화 종료(BYE) 또는 미확립 발신 취소(CANCEL). SDD 기점을 찍는다. */
    bool Bye(const std::string& callId);
    bool HasCall(const std::string& callId);
    bool Connected(const std::string& callId);
    /** 수신 RTP 품질(누계) — 호가 없으면 false. */
    bool RtpStats(const std::string& callId, unsigned long long& rx, unsigned long long& lost, long long& jitterUs);
    void ResetRtpStats(const std::string& callId);
    size_t CallCount();
    std::string LocalIp() const { return m_clsSetup.m_strLocalIp; }

    // ISipUserAgentCallBack (스택 스레드)
    void EventRegister(CSipServerInfo*, int) override {}
    void EventIncomingCall(const char* pszCallId, const char* pszFrom, const char* pszTo, CSipCallRtp* pclsRtp,
                           CSipMessage* pclsMessage) override;
    void EventCallRing(const char*, int, CSipCallRtp*) override {}
    void EventCallStart(const char* pszCallId, CSipCallRtp* pclsRtp) override;
    void EventCallEnd(const char* pszCallId, int iSipStatus) override;
    // ISipStackCallBack — BYE 최종 응답 관측(UA 보다 먼저 등록, 처리는 UA 에 위임)
    bool RecvRequest(int, CSipMessage*) override { return false; }
    bool RecvResponse(int iThreadId, CSipMessage* pclsMessage) override;
    bool SendTimeout(int, CSipMessage*) override { return false; }

private:
    struct Call {
        std::string callId, fromUser, toUser;
        bool outbound = false;
        bool connected = false;
        CRtpThread* rtp = nullptr;
        CSipCallRtp offer;              // 착신 오퍼(answer 용)
        bool hasOffer = false;
        long long tStart = 0;           // INVITE 송신 시각(SRD 기점)
        long long tBye = 0;             // BYE 송신 시각(SDD 기점) — 0 이면 미송신
    };

    CsimPeerConfig m_cfg;
    CSipUserAgent m_clsUserAgent;
    CSipStackSetup m_clsSetup;
    ICsimPeerObserver* m_pObserver = nullptr;
    std::mutex m_mtx;
    std::map<std::string, Call> m_calls;
    bool m_bStarted = false;
    std::vector<const CSipCodecEntry*> m_codecs;   // 해석된 코덱 엔트리(우선순위 순)

    CRtpThread* newRtp();
    void freeRtp(CRtpThread* rtp);
    /** 오퍼 SDP(audio, 자기 코덱 전부) */
    void buildOffer(CSipCallRtp& clsRtp, CRtpThread* rtp);
    /** 오퍼에서 자기 코덱 목록과의 첫 공통 엔트리 — 없으면 NULL */
    const CSipCodecEntry* pickAnswerCodec(const CSipCallRtp& offer) const;
    static long long nowMs();
    static std::string genIcid();
};

#endif
