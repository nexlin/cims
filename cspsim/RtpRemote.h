// RtpRemote — 미디어 전담 워커(test_instrument.md §4 미디어 평면 후속): CRtpThread 의 소켓·송수신 스레드를 다른 워커(미디어 에이전트)에 두는 원격 모드의 계약.
//   SimSession/CsimPeer 는 CRtpThread 를 그대로 쓰고, CRtpThread 가 IRtpRemote 가 있으면 Create/Start/Stop/송출 제어/DTMF/통계를 원격에 위임한다 —
//   SDP 의 c=/m= 는 에이전트가 준 주소·포트. 구현(HTTP 클라이언트)은 워커(MediaAgentClient). floor 제어(PTT)는 원격 모드에 없다(컴파일 게이트).
#ifndef _CSIM_RTP_REMOTE_H_
#define _CSIM_RTP_REMOTE_H_

#include <string>

struct RtpRemoteStart {
    std::string destIp;
    int destPort = 0;
    int audioPt = -1;
    int dtmfPt = -1;
    int dtmfClock = 8000;
    bool dtmfInband = false;
    int mode = 0;                 // CRtpThread::EMediaMode
    bool useMediaFile = true;     // AMR-WB 합의 — 에이전트의 Media.AudioFile 원천
    int destVideoPort = 0;
    bool videoOffer = true;
    std::string srtpSuite, srtpLocal, srtpRemote;     // 오디오 SDES 키(비면 평문)
    std::string vSuite, vLocal, vRemote;              // 비디오 SDES 키
};

struct RtpRemoteStats {
    unsigned long long tx = 0, rx = 0, lost = 0;
    long long jitterUs = 0;
    int recvPt = -1;
    int rtcpRx = 0, rtcpRrBlocks = 0, rrFractionLost = -1;
    int dtmfSent = 0, dtmfRecv = 0;
    std::string dtmfDigits;
    unsigned long long ssrcCount = 0;
    bool sendRunning = false, sourceEnded = false;
};

struct IRtpRemote {
    virtual ~IRtpRemote() {}
    /** 스트림 자리 할당 — 에이전트가 소켓을 열고 (id, 광고 IP, RTP 포트, 비디오 포트(0 = 없음)) 를 준다. */
    virtual bool Allocate(bool bVideo, std::string& id, std::string& ip, int& port, int& videoPort) = 0;
    virtual bool Release(const std::string& id) = 0;
    /** 송수신 시작(재Start = 목적지·코덱 갱신). */
    virtual bool Start(const std::string& id, const RtpRemoteStart& s) = 0;
    virtual bool Stop(const std::string& id) = 0;
    /** 송출 제어 — op: send(amrwb,pcmu,pcma,g722 파일·loop) · send_default · stop · hold(on|off) · dtmf(digits) · reset(통계·DTMF 초기화) */
    virtual bool Control(const std::string& id, const std::string& op, const std::string& a = "", const std::string& b = "",
                         const std::string& c = "", const std::string& d = "", bool bLoop = true) = 0;
    virtual bool Stats(const std::string& id, RtpRemoteStats& out) = 0;
};

#endif
